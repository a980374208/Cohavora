"""Review one diagnostic product cycle; never award formal qualification credit.

Only product inbound RTP/decode, independent receiver delivery, paired logging,
and normal shutdown are gated. Sharing, GPU, microphone signal and long stability
are outside this verdict. Original runtime evidence is never changed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

from product_pilot_correlation import correlate
from product_pilot_load import review_lifecycle
from product_pilot_performance import paired_window
from product_pilot_shutdown import final_complete
from verify_product_external import read, records, timestamp


def review(root: Path) -> dict:
    report = dict(schema=1, verdict="FAIL", diagnostic_only=True,
                  qualification_credit=0, formal="NOT_STARTED", checks={},
                  failures=[], details={}, not_proven=["non_muted_physical_microphone",
                  "screen_share", "GPU", "full_media", "long_stability"])
    report["physical_microphone_signal"] = "NOT_PROVEN"

    def check(name, passed, detail=None):
        report["checks"][name] = "PASS" if passed else "FAIL"
        if detail is not None:
            report["details"][name] = detail
        if not passed:
            report["failures"].append(name)

    def attempt(name, function):
        try:
            function()
        except (OSError, ValueError, KeyError, TypeError, AttributeError, IndexError, OverflowError, StopIteration) as error:
            report["checks"][name] = "UNKNOWN"
            report["failures"].append(name)
            report["details"][name] = dict(error_type=type(error).__name__, reason=str(error))

    # Missing mandatory inputs cannot produce a partial PASS.
    try:
        plan = read(root / "plan.json")
        run = plan["run_id"]
        report["run_id"] = run
        result = read(root / "uia/uia-result.json")
        identity = read(root / "uia/product-identity.json")
        actions = list(records(root / "uia/uia-actions.jsonl"))
        probes = list(records(root / "process-probe.jsonl"))
        resources = list(records(root / "external-resources.jsonl"))
        remote = list(records(root / "remote.jsonl"))
    except (OSError, ValueError, KeyError, TypeError) as error:
        check("mandatory_inputs", False, dict(error_type=type(error).__name__, reason=str(error)))
        return report

    check("diagnostic_scope", bool(run) and plan.get("mode") == "FirstCycleMediaLogDiagnostic"
          and type(plan.get("cycles")) is int and plan["cycles"] == 1
          and plan.get("diagnostic_only") is True and type(plan.get("qualification_credit")) is int
          and plan["qualification_credit"] == 0 and plan.get("full_media_gpu") is False)
    check("uia_completed", result.get("run_id") == run
          and result.get("verdict") == "DIAGNOSTIC_UIA_COMPLETE"
          and type(result.get("cycles_completed")) is int and result["cycles_completed"] == 1)

    def identity_review():
        pid = identity["pid"]
        check("product_identity", identity.get("run_id") == run and type(pid) is int and pid > 0
              and Path(identity["executable"]).parent.name == "RelWithDebInfo")
        check("collector_run_binding", bool(probes) and bool(resources) and bool(remote)
              and all(r.get("run_id") == run for r in probes + resources + remote))
        check("collector_pid_binding", all((r.get("gpu_budget") or {}).get("pid") == pid for r in probes)
              and all(r.get("pid") == pid for r in resources))
        check("resource_process_binding", all(r.get("start_ticks") == identity["start_ticks"]
              and r.get("executable") == identity["executable"] for r in resources))
    attempt("identity_schema", identity_review)

    selected = {}
    def action_review():
        required = ("join", "page", "logging", "leave", "export", "process_exit")
        if any(a.get("action") == "login" for a in actions):
            required = ("login",) + required
        expected = {(a, p) for a in required for p in ("requested", "uia_observed")}
        keys = [(a["action"], a["phase"]) for a in actions]
        check("scoped_action_pairs", len(keys) == len(expected) and set(keys) == expected
              and all(type(a.get("cycle")) is int and a["cycle"] == 1 for a in actions), keys)
        if len(keys) != len(expected) or set(keys) != expected:
            raise ValueError("exact_single_cycle_action_pairs_required")
        selected.update(zip(keys, actions))
        joined = selected["join", "uia_observed"]
        sid, process_run, cycle_id = (joined[k] for k in
                                      ("anonymous_session_id", "process_run_id", "cycle_id"))
        check("action_native_binding", bool(sid) and bool(process_run) and bool(cycle_id)
              and all(a.get("run_id") == run and a.get("pid") == identity["pid"]
                      and a.get("cycle_id") == cycle_id for a in actions)
              and all(a.get("process_run_id") == process_run for a in actions)
              and all(a.get("anonymous_session_id") == sid for a in actions
                      if a["action"] != "login" and (a["action"], a["phase"]) != ("join", "requested"))
              and all(a.get("anonymous_session_id") is None for a in actions if a["action"] == "login"))
        check("action_order", keys == [(a, p) for a in required for p in ("requested", "uia_observed")]
              and all(timestamp(a) <= timestamp(b) for a, b in zip(actions, actions[1:]))
              and all(selected[a, "requested"]["operation_id"] == selected[a, "uia_observed"]["operation_id"]
                      for a in required))
        check("native_observer_correlation", True,
              correlate(run, actions, remote, list(records(root / "diagnostic-events.jsonl"))))
        active = [p for p in probes if timestamp(joined) <= timestamp(p)
                  <= timestamp(selected["leave", "requested"])]
        check("probe_native_binding", bool(active) and all(p.get("anonymous_session_id") == sid
              and p.get("process_run_id") == process_run for p in active))
        report["details"]["identity"] = dict(pid=identity["pid"], cycle_id=cycle_id,
              process_run_id=process_run, anonymous_session_id=sid)
    attempt("action_schema", action_review)

    def media_review():
        start = timestamp(selected["join", "uia_observed"])
        end = timestamp(selected["leave", "requested"])
        routes = []
        # Same actual_rtp_decode_continuity predicate as verify_product_external.
        for p in probes:
            if start + 10 <= timestamp(p) <= end:
                metrics = {m["key"]: m["value"] for m in p.get("metrics", [])
                           if m["availability"] == "VALID"}
                routes.append(dict(age_s=timestamp(p) - p["source_utc_ms"] / 1000,
                                   rtp=metrics.get("video.pipeline.inbound_rtp_streams"),
                                   decode=metrics.get("video.pipeline.active_decode_streams"),
                                   utc_ms=p.get("source_utc_ms")))
        check("actual_rtp_decode_continuity", bool(routes) and all(
              r["age_s"] <= 10 and isinstance(r["rtp"], int) and r["rtp"] > 0
              and isinstance(r["decode"], int) and r["decode"] > 0 for r in routes),
              dict(samples=len(routes), actual_rtp_counts=sorted({r["rtp"] for r in routes if r["rtp"] is not None}),
                   actual_decode_counts=sorted({r["decode"] for r in routes if r["decode"] is not None}),
                   latest=routes[-1] if routes else None))
        joined = selected["join", "uia_observed"]
        window = [r for r in remote if start <= timestamp(r) <= end
                  and r["event"] in ("receiver.connection_sample", "receiver.sample")]
        check("receiver_native_binding", bool(window) and all(
              all((r.get("observer_context") or {}).get(k) == joined[k] for k in
                  ("run_id", "cycle", "cycle_id", "pid", "process_run_id", "anonymous_session_id", "participant_sha256"))
              for r in window))
        connections = [r for r in window if r["event"] == "receiver.connection_sample"]
        packets = [int(s["received"]["packets_received"]) for r in connections
                   for s in r.get("inbound", []) if "packets_received" in s.get("received", {})]
        check("independent_receiver_rtp", bool(connections) and bool(packets) and max(packets) > 0,
              dict(samples=len(connections), maximum_packets_received=max(packets, default=None),
                   latest=connections[-1] if connections else None,
                   scope="receiver subscribed to product in joined-to-leave-requested window"))
        for kind in ("video", "audio"):
            samples = [r for r in window if r["event"] == "receiver.sample" and r.get("kind") == kind]
            frames = [r.get("window_frames") for r in samples]
            check("independent_receiver_" + kind, bool(frames)
                  and all(type(n) is int and n >= 0 for n in frames) and sum(frames) > 0,
                  dict(samples=len(samples), window_frames=frames, latest=samples[-1] if samples else None))
        prefix = "pilot-" + run[:8] + "-load-"
        snapshots = [r for r in remote if r["event"] == "sfu.snapshot" and start <= timestamp(r) <= end]
        check("fixed_load_published", bool(snapshots) and all(len([p for p in r["participants"]
              if p["identity"].startswith(prefix) and any(t["source"] == 1 and not t["muted"]
              for t in p["tracks"])]) == 10 for r in snapshots), dict(samples=len(snapshots)))
        check("remote_errors", not any(r["event"] == "collector.error" for r in remote))
    attempt("media_schema", media_review)

    def logging_review():
        windows = list(records(root / "uia/uia-log-windows.jsonl"))
        if len(windows) != 1 or windows[0]["run_id"] != run or windows[0]["cycle"] != 1:
            raise ValueError("single_run_bound_logging_window_required")
        window = windows[0]
        check("logging_operation_binding", window.get("pid") == identity["pid"]
              and window.get("operation_id") == selected["logging", "requested"]["operation_id"])
        start, end = timestamp(selected["join", "uia_observed"]), timestamp(selected["leave", "requested"])
        times = [datetime.fromisoformat(window[k].replace("Z", "+00:00")).timestamp()
                 for k in ("off_start_utc", "off_end_utc", "on_start_utc", "on_end_utc")]
        check("logging_product_window", start <= times[0] < times[1] <= times[2] < times[3] <= end)
        pair = paired_window(window, probes, resources)
        check("logging_performance", pair["cpu_increase_percentage_points"] <= 2
              and pair["p95_increase_upper_bound_percent"] <= 5,
              dict(**pair, cpu_limit_percentage_points=2, p95_limit_percent=5))
    attempt("logging_performance", logging_review)

    def exit_review():
        product = read(root / "uia/process-exit.json")
        check("product_normal_exit", product.get("run_id") == run and product.get("pid") == identity["pid"]
              and type(product.get("exit_code")) is int and product["exit_code"] == 0, product)
        check("process_released", bool(resources) and resources[-1].get("process_alive") is False)
        ready = [r for r in remote if r["event"] == "load.ready"]
        stopped = [r for r in remote if r["event"] == "load.stopped"]
        check("isolated_ten_publisher_lifecycle", review_lifecycle(ready, stopped, run),
              dict(ready=ready, stopped=stopped))
        check("remote_terminal", bool(remote) and remote[-1].get("event") == "collector.stopped"
              and remote[-1].get("status") == "COMPLETE", remote[-1] if remote else None)
        shutdown = read(root / "remote-shutdown.json")
        state = shutdown.get("observed_terminal") or {}
        check("remote_shutdown", shutdown.get("run_id") == run and shutdown.get("status") == "COMPLETE"
              and final_complete(state, run), shutdown)
        route = read(root / "server-route.json")
        check("server_route_terminal", route == state.get("route") and route.get("run_id") == run
              and route.get("status") == "COMPLETE" and type(route.get("child_exit_code")) is int
              and route["child_exit_code"] == 0 and route.get("cleanup_complete") is True, route)
        collectors = read(root / "collectors.json")
        exits = read(root / "collector-exits.json")
        controller = read(root / "controller-result.json")
        check("uia_normal_exit", controller.get("run_id") == run
              and type(controller.get("uia_pid")) is int and controller["uia_pid"] > 0
              and controller["uia_pid"] == collectors.get("uia_pid")
              and type(controller.get("uia_exit_code")) is int and controller["uia_exit_code"] == 0
              and controller.get("uia_forced_stop") is False
              and type(controller.get("exit_code")) is int and controller["exit_code"] == 0
              and not controller.get("failure"), controller)
        expected = [collectors.get(k) for k in ("resource_pid", "archive_pid", "diagnostic_pid")]
        rows = exits.get("collectors", [])
        # This scoped controller owns exactly these three collectors, no ETW.
        # Do not reinterpret or weaken the formal verifier's collector contract.
        check("local_collectors_normal_exit", collectors.get("run_id") == exits.get("run_id") == run
              and all(type(p) is int and p > 0 for p in expected) and len(set(expected)) == 3
              and isinstance(rows, list) and len(rows) == 3 and all(type(r.get("pid")) is int
              and r["pid"] > 0 and type(r.get("exit_code")) is int and r["exit_code"] == 0
              and r.get("forced_stop") is False for r in rows)
              and {r["pid"] for r in rows} == set(expected), exits)
    attempt("exit_chain_evidence", exit_review)
    report["verdict"] = "PASS_SCOPED" if not report["failures"] else "FAIL"
    report["reviewed_utc"] = datetime.now(timezone.utc).isoformat()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--review-output", type=Path, required=True,
                        help="New derivative directory; existing evidence is never overwritten")
    args = parser.parse_args()
    args.review_output.mkdir(parents=True, exist_ok=False)
    report = review(args.root)
    report["verifier_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report["input_sha256"] = {name: hashlib.sha256((args.root / name).read_bytes()).hexdigest()
        for name in ("plan.json", "uia/uia-result.json", "uia/product-identity.json",
            "uia/uia-actions.jsonl", "uia/uia-log-windows.jsonl", "process-probe.jsonl",
            "external-resources.jsonl", "remote.jsonl", "diagnostic-events.jsonl",
            "uia/process-exit.json", "remote-shutdown.json", "server-route.json",
            "collectors.json", "collector-exits.json", "controller-result.json") if (args.root / name).is_file()}
    (args.review_output / "first-cycle-media-log-review.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("verdict", "failures", "diagnostic_only", "qualification_credit", "formal")}))
    return 0 if report["verdict"] == "PASS_SCOPED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
