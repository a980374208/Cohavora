"""Independent, fail-closed review of one short product PILOT (never 8h PASS)."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import statistics

from verify_product_acceptance import validate_bundle, EvidenceError
from product_pilot_performance import paired_window
from product_pilot_correlation import correlate
from product_pilot_audio import review_outbound_audio
from product_pilot_archive import read_segment
from product_pilot_desktop_policy import bind_desktop_input_policy


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines()
            if line.strip()]


def stamp(row):
    if "utc_ms" in row:
        return row["utc_ms"] / 1000
    return datetime.fromisoformat(row["utc"].replace("Z", "+00:00")).timestamp()


def percentile(values, fraction):
    return sorted(values)[min(len(values)-1, int((len(values)-1)*fraction))] if values else None


def review_rejoin(root):
    """Use the same per-cycle external gates for PILOT and formal execution."""
    plan, uia, external = (read(root / name) for name in
        ("plan.json", "uia/uia-result.json", "external-review.json"))
    input_policy=bind_desktop_input_policy(plan,uia,external)
    run, count = plan["run_id"], plan["cycles"]
    checks = {}
    def check(name, ok, detail=None):
        checks[name] = dict(status="PASS" if ok else "FAIL", detail=detail)
    actions = rows(root / "uia/uia-actions.jsonl")
    outbound_names = {"receiver_audio_rtp_stats", "outbound_audio_pcm_continuity"}
    check("same_process_rejoin", count >= 2 and uia["cycles_completed"] == count
          and uia["verdict"] == "PILOT_COMPLETE" and uia["run_id"] == run
          and len({a["pid"] for a in actions}) == 1
          and len({a["process_run_id"] for a in actions}) == 1)
    check("external_cycle_review", external["run_id"] == run
          and external["verdict"] == "PASS_WITH_DEFERRED"
          and external["cycle_counts"] == {"PASS": count}
          and len(external["cycles"]) == count)
    for item in external["cycles"]:
        for name, status in item["checks"].items():
            if name in outbound_names:
                continue  # Aggregated below using independently bound cycle windows.
            check(f"cycle_{item['cycle']}_{name}", status == "PASS",
                  item.get("details", {}).get(name))
    for name, ok in external.get("final_checks", {}).items():
        check(name, ok)
    check("normal_exit_and_no_windows_crash", external.get("exit_code") == 0
          and external.get("windows_crash_events") == 0)
    check("minimum_duration", external.get("duration_seconds", 0) >= 240 * count)
    remote = rows(root / "remote.jsonl")
    ready = [r for r in remote if r["event"] == "load.ready"]
    expected = dict(publishers=10, video_width=160, video_height=90, video_fps=5,
                    video_bps_each=40000, video_codec="VP8", audio_bps=24000, simulcast=False)
    check("fixed_load_configuration", len(ready) == 1 and all(
          ready[0].get(k) == v for k,v in expected.items()), expected)
    pairs = [c.get("details", {}).get("logging_performance") for c in external["cycles"]]
    check("log_performance_complete", len(pairs) == count and all(
          isinstance(p, dict) and p["cpu_increase_percentage_points"] <= 2
          and p["p95_increase_upper_bound_percent"] <= 5 for p in pairs),
          dict(pairs=pairs, cpu_limit_percentage_points=2, p95_limit_percent=5))
    devices = read(root / "audio-devices.json")
    outcomes_path = root / "uia/uia-device-outcomes.jsonl"
    outcomes = rows(outcomes_path) if outcomes_path.exists() else []
    limit = read(root / "limits.json")["max_audio_gap_ms"]
    audio_cycles = [review_outbound_audio(run,[a for a in actions if a["cycle"] == cycle],
        remote,devices,outcomes,limit) for cycle in range(1,count+1)]
    for name in outbound_names:
        values = [cycle[name] for cycle in audio_cycles]
        status = "FAIL" if any(v["status"] == "FAIL" for v in values) else \
            "DEFERRED" if any(v["status"] == "DEFERRED" for v in values) else "PASS"
        checks[name] = dict(status=status, detail=dict(cycles=values))
    statuses = [c["status"] for c in checks.values()]
    report = dict(schema=2, run_id=run, scope="same-process multi-lifecycle PILOT",
        desktop_input_policy=input_policy,
        verdict="PILOT_PASS" if all(s in ("PASS", "DEFERRED") for s in statuses) else "FAIL",
        checks=checks, check_counts=dict(Counter(statuses)),
        limitations=external.get("deferred", []) + ["not_8_hours_or_100_lifecycles"],
        reviewed_utc=datetime.now(timezone.utc).isoformat())
    (root / "pilot-review.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(verdict=report["verdict"], checks=report["check_counts"])))
    return 0 if report["verdict"] == "PILOT_PASS" else 1


def review(root):
    if read(root / "plan.json").get("cycles", 1) > 1:
        return review_rejoin(root)
    plan, uia = read(root / "plan.json"), read(root / "uia/uia-result.json")
    input_policy=bind_desktop_input_policy(plan,uia)
    run = plan["run_id"]
    report = {"schema": 1, "run_id": run, "verdict": "INCONCLUSIVE",
              "desktop_input_policy": input_policy,
              "scope": "one short PILOT, not formal acceptance", "checks": {},
              "limitations": ["not_8_hours_or_100_lifecycles", "concurrent_server_load",
                  "pcm_delivery_does_not_prove_audio_quality", "wgc_handle_ownership_not_observed",
                  "gpu_dedicated_shared_counters_are_not_dxgi_local_nonlocal_budget",
                  "not_slow_disk_power_loss_real_crash_or_release_symbols"]}
    checks = report["checks"]

    def check(name, ok, detail=None, missing=False):
        checks[name] = {"status": "PASS" if ok else "UNKNOWN" if missing else "FAIL",
                        "detail": detail}

    check("uia_lifecycle", uia["run_id"] == run and uia["verdict"] == "PILOT_COMPLETE"
          and uia["cycles_completed"] == 1, uia)
    exit_path, crash_path = root / "uia/process-exit.json", root / "windows-crash-event.json"
    if exit_path.exists() and crash_path.exists():
        process_exit, crashes = read(exit_path), read(crash_path)
        check("normal_exit_and_no_windows_crash", process_exit["run_id"] == run
              and process_exit["exit_code"] == 0 and crashes["count"] == 0,
              {"exit_code": process_exit["exit_code"], "windows_events": crashes["count"]})
    else:
        check("normal_exit_and_no_windows_crash", False, "exit or Windows evidence missing", missing=True)
    actions = rows(root / "uia/uia-actions.jsonl")
    by_action = {(r["action"], r["phase"]): r for r in actions}
    needed = ("join", "page", "share_start", "share_stop", "logging", "leave", "export", "process_exit")
    check("uia_action_pairs", all((a, p) in by_action for a in needed
                                  for p in ("requested", "uia_observed")))
    remote = rows(root / "remote.jsonl")
    resources = rows(root / "external-resources.jsonl")
    probe = rows(root / "process-probe.jsonl")
    for name, values in (("remote", remote), ("external_resources", resources), ("probe", probe)):
        check(name + "_identity_sequence", bool(values) and all(r.get("run_id") == run
            and r.get("sequence") == i for i, r in enumerate(values, 1)), {"rows": len(values)})
    errors = [r for r in remote if r["event"] == "collector.error"]
    check("remote_collector_errors", not errors, errors)
    check("remote_collector_stopped", remote[-1]["event"] == "collector.stopped"
          and remote[-1].get("status") == "COMPLETE")
    if plan.get("requires_context"):
        try:
            diagnostic_events = rows(root / "diagnostic-events.jsonl")
            correlation = correlate(run, actions, remote, diagnostic_events)
            check("explicit_observer_native_correlation", True, correlation)
            check("continuous_diagnostic_witness", bool(diagnostic_events) and all(
                e["run_id"] == run and e["event_sequence"] == i
                for i, e in enumerate(diagnostic_events, 1)), {"events": len(diagnostic_events)})
        except (OSError, KeyError, ValueError, TypeError) as error:
            check("explicit_observer_native_correlation", False, type(error).__name__ + ":" + str(error))
    joined = stamp(by_action.get(("join", "uia_observed"), actions[0]))
    left = stamp(by_action.get(("leave", "requested"), actions[-1]))
    for name, values in (("resources", resources), ("probe", probe),
                         ("sfu", [r for r in remote if r["event"] == "sfu.snapshot"]),
                         ("server", [r for r in remote if r["event"] == "server.resource"])):
        times = sorted(stamp(r) for r in values)
        gaps = [b-a for a, b in zip(times, times[1:]) if b >= joined and a <= left]
        check(name + "_coverage", bool(times) and times[0] <= joined and times[-1] >= left
              and max(gaps, default=float("inf")) <= 10,
              {"max_sample_gap_s": max(gaps, default=None), "samples": len(times)})
    product = {r["participant"] for r in remote if r["event"] == "receiver.participant_joined"}
    departed = {r["participant"] for r in remote if r["event"] == "receiver.participant_left"}
    check("independent_join_leave", len(product) == 1 and product <= departed,
          {"joined": sorted(product), "left": sorted(departed)})
    samples = [r for r in remote if r["event"] == "receiver.sample"]
    screen = [r for r in samples if r["source"] == 3 and r["kind"] == "video"]
    screens = {r["sid"] for r in screen}
    unpublished = {r["sid"] for r in remote if r["event"] == "receiver.track_unpublished"
                   and r["source"] == 3}
    decoded = max((int(s.get("inbound", {}).get("frames_decoded", 0))
                   for r in screen for s in r["stats"]), default=0)
    packets = max((int(s.get("received", {}).get("packets_received", 0))
                   for r in screen for s in r["stats"]), default=0)
    check("screen_delivered_and_unpublished", bool(screens) and screens <= unpublished
          and decoded > 0 and packets > 0, {"track_sids": sorted(screens),
               "decoded_frames": decoded, "rtp_packets": packets,
               "received_frames": max((r["frames"] for r in screen), default=0)})
    audio = [r for r in samples if r["kind"] == "audio" and joined <= stamp(r) <= left]
    audio_gaps = [r["window_max_gap_ms"] for r in audio] + [r["silence_ms"] for r in audio
                                                               if r["silence_ms"] is not None]
    check("outbound_audio_pcm_continuity", bool(audio) and all(r["window_frames"] > 0 for r in audio)
          and max(audio_gaps, default=float("inf")) <= 200
          and stamp(audio[0]) <= joined + 2 and stamp(audio[-1]) >= left - 2,
          {"max_callback_or_silence_ms": max(audio_gaps, default=None),
           "frames": max((r["frames"] for r in audio), default=0),
           "samples": len(audio), "threshold_ms": 200})
    audio_rtp = [s for r in remote if r["event"] == "receiver.connection_sample"
                 and joined <= stamp(r) <= left for s in r["inbound"]
                 if s.get("stream", {}).get("kind") == "audio"]
    audio_packets = max((int(s.get("received", {}).get("packets_received", 0))
                         for s in audio_rtp), default=0)
    check("receiver_audio_rtp_stats", bool(audio) and
          audio_packets > 0,
          {"peer_connection_audio_rows": len(audio_rtp), "packets_received": audio_packets,
           "scope": "independent receiver subscribed only to product; track SID mapping not inferred"},
          missing=True)
    # No capture hardware is an explicit external limitation. Require two
    # independent observations; neither silent PCM callbacks nor a toggle state
    # can turn missing microphone RTP into PASS.
    device_path = root / "audio-devices.json"
    outcome_path = root / "uia/uia-device-outcomes.jsonl"
    if device_path.exists() and outcome_path.exists():
        devices, outcomes = read(device_path), rows(outcome_path)
        no_microphone = devices.get("run_id") == run and devices.get("collector") == \
            "independent_windows_mmdevice" and devices.get("active_capture_endpoints") == 0 \
            and devices.get("enumeration_hresult") == "00000000" \
            and devices.get("default_capture_hresult") == "80070490" \
            and any(r.get("run_id") == run and r.get("cycle") == 1
                    and r.get("device") == "microphone" and r.get("result") == "DEFERRED"
                    for r in outcomes)
        if no_microphone:
            for name in ("receiver_audio_rtp_stats", "outbound_audio_pcm_continuity"):
                checks[name] = {"status": "DEFERRED", "detail": {
                    "reason": "no_active_windows_capture_endpoint",
                    "os_evidence": "audio-devices.json", "uia_evidence": str(outcome_path.name),
                    "received_rtp_packets": audio_packets}}
            report["limitations"].append("outbound_microphone_audio_not_run_no_capture_device")
    audio_path = root / "product-audio.jsonl"
    if audio_path.exists():
        playout = rows(audio_path)
        active = [r for r in playout if r["event"] == "audio.sample"
                  and joined + 1 <= stamp(r) <= left]
        check("product_inbound_audio_continuity", bool(active)
              and all(r["run_id"] == run and r["sequence"] == i
                      for i, r in enumerate(playout, 1))
              and playout[-1]["event"] == "collector.stopped"
              and playout[-1].get("status") == "COMPLETE"
              and all(r["max_packet_gap_ms"] <= 200 and r["window_signal_frames"] > 0
                      and r["timestamp_errors"] == 0 and r["discontinuities"] == 0 for r in active)
              and stamp(active[0]) <= joined + 3 and stamp(active[-1]) >= left - 2,
              {"scope": "independent WASAPI product PID process loopback; PCM not saved",
               "samples": len(active), "max_gap_ms": max((r["max_packet_gap_ms"] for r in active), default=None),
               "minimum_rms": min((r["rms"] for r in active), default=None)})
    else:
        check("product_inbound_audio_continuity", False, "process loopback witness missing", missing=True)
    capture = [r["capture"] for r in probe if r["capture"]["frames"] > 0]
    check("capture_backend_observed", bool(capture) and all(r["backend"] in
          ("wgc", "dxgi", "gdi") for r in capture),
          {"backends": sorted({r["backend"] for r in capture}),
           "frames": max((r["frames"] for r in capture), default=0)})
    history = {k: max(r["history"][k] for r in probe) for k in
               ("queue_drops", "pending_records_dropped", "write_failures", "queue_depth")}
    check("telemetry_no_loss", all(history[k] == 0 for k in
          ("queue_drops", "pending_records_dropped", "write_failures")), history)
    archive = root / "checkpoint-archive"
    if (archive / "collector.jsonl").exists():
        archived = rows(archive / "collector.jsonl")
        revisions = defaultdict(set)
        generations = defaultdict(set)
        archive_hashes_ok = True
        archived_bytes = 0
        for event in archived:
            if event["event"] != "segment.archived":
                continue
            session, name = event["session"], event["file"]
            if not re.fullmatch(r"[0-9a-f]{32}", session) or not re.fullmatch(r"segment-[0-9]{20}\.jsonl", name):
                raise ValueError("unsafe_archive_path")
            digest = hashlib.sha256()
            size = 0
            content = read_segment(archive / session, event)
            for line in content.splitlines(keepends=True):
                digest.update(line)
                size += len(line)
                metric = json.loads(line)
                revision = metric["revision"]
                archive_hashes_ok &= event["first_revision"] <= revision <= event["last_revision"]
                revisions[session].add(revision)
                generations[session].add(metric["session_generation"])
            archive_hashes_ok &= size == event["size_bytes"] and digest.hexdigest() == event["sha256"]
            archived_bytes += size
        details = []
        for session, values in revisions.items():
            manifest = read(archive / session / "manifest.json")
            last = manifest["last_committed_revision"]
            details.append({"session": session, "first_revision": min(values),
                "last_revision": max(values), "distinct_revisions": len(values),
                "expected_revisions": last, "missing_revisions": last-len(values),
                "complete": manifest["session_complete"],
                "product_pruned_records": manifest["pruned_records"],
                "single_generation": generations[session] == {manifest["session_generation"]}})
        check("checkpoint_archive_integrity", archive_hashes_ok and bool(details)
              and archived[-1]["event"] == "collector.stopped"
              and all(r["run_id"] == run and r["sequence"] == i for i,r in enumerate(archived,1)),
              {"bytes": archived_bytes, "segments": sum(r["event"] == "segment.archived" for r in archived)})
        check("checkpoint_revision_1_through_terminal", len(details) == 1 and all(
              d["complete"] and d["single_generation"] and d["first_revision"] == 1
              and d["last_revision"] == d["expected_revisions"] == d["distinct_revisions"]
              for d in details), details)
        check("final_memory_checkpoint_agreement", len(details) == 1 and
              details[0]["last_revision"] == probe[-1].get("revision") ==
              probe[-1]["history"]["checkpoint_revision"])
    diagnostic = {k: max(r["diagnostic"][k] for r in probe) for k in
                  ("dropped_ordinary", "dropped_critical", "sink_failures", "accepted", "written")}
    check("diagnostic_writer_no_loss", all(diagnostic[k] == 0 for k in
          ("dropped_ordinary", "dropped_critical", "sink_failures")), diagnostic)
    metrics = defaultdict(list)
    stale = []
    for r in probe:
        if joined <= stamp(r) <= left and "source_utc_ms" in r:
            stale.append((r["utc_ms"] - r["source_utc_ms"]) / 1000)
        for m in r.get("metrics", []):
            if isinstance(m, dict) and m["availability"].upper() == "VALID" and m["value"] is not None:
                metrics[m["key"]].append(m["value"])
    check("product_media_metrics_fresh", bool(stale) and max(stale) <= 10,
          {"maximum_age_s": max(stale, default=None), "valid_observed_values":
           {k: max(v) for k, v in metrics.items() if k in
            ("network.rtp.inbound.streams", "network.rtp.outbound.streams", "video.codec.decoders",
             "video.pipeline.inbound_decoded", "audio.window.samples")}})
    active_decode = metrics.get("video.pipeline.active_decode_streams", [])
    check("product_active_decode_stream_count", bool(active_decode) and max(active_decode) > 0,
          {"scope": "native per-inbound-RTP framesDecoded delta; not allocated decoder instances",
           "observed_counts": sorted(set(active_decode))}, missing=not active_decode)
    active_resources = [r for r in resources if joined <= stamp(r) <= left]
    gpu = [r for r in active_resources if r["gpu_dedicated_bytes"] is not None]
    check("external_gpu_coverage", bool(active_resources) and len(gpu) == len(active_resources),
          {"samples": len(gpu), "expected": len(active_resources),
           "peak_dedicated_bytes": max((r["gpu_dedicated_bytes"] for r in gpu), default=None),
           "peak_shared_bytes": max((r["gpu_shared_bytes"] for r in gpu), default=None)}, missing=True)
    check("process_exit_release", resources[-1]["process_alive"] is False,
          {"peak_private_bytes": max(r["private_bytes"] for r in resources),
           "peak_handles": max(r["handles"] for r in resources),
           "peak_threads": max(r["threads"] for r in resources), "final": resources[-1]})
    if plan.get("requires_context"):
        released = [p for p in probe if stamp(p) >= stamp(by_action[("leave", "uia_observed")])
                    and p.get("session_complete")]
        check("native_cleanup_release", bool(released) and released[-1].get("native_cleanup_pending") == 0,
              {"last_cleanup_pending": released[-1].get("native_cleanup_pending") if released else None})
    server = [r for r in remote if r["event"] == "server.resource" and joined <= stamp(r) <= left]
    egress = [sum(i["tx_bps"] for i in r["network"].values()) for r in server]
    check("server_bandwidth_below_3mbps", bool(egress) and max(egress) <= 3_000_000,
          {"peak_egress_bps": max(egress, default=None), "p95_egress_bps": percentile(egress,.95),
           "mean_egress_bps": statistics.mean(egress) if egress else None,
           "samples_above_3mbps": sum(v > 3_000_000 for v in egress),
           "peak_cpu_pct": max((r["cpu_pct"] for r in server if r["cpu_pct"] is not None), default=None),
           "min_available_bytes": min((r["memory"]["MemAvailable"] for r in server), default=None),
           "includes_other_active_room": True})
    ready = next((r for r in remote if r["event"] == "load.ready"), None)
    check("fixed_load_configuration", bool(ready) and ready["publishers"] == 10 and
          ready["video_bps_each"] == 40000 and ready["video_fps"] == 5 and not ready["simulcast"], ready)
    bundles = list((root / "uia/export-0001").glob("cohavora-diagnostic-bundle-*"))
    check("support_bundle_exists", len(bundles) == 1)
    if len(bundles) == 1:
        manifest = read(bundles[0] / "manifest.json")
        hash_ok = all((bundles[0]/f["path"]).stat().st_size == f["size_bytes"] and
            hashlib.sha256((bundles[0]/f["path"]).read_bytes()).hexdigest() == f["sha256"]
            for f in manifest["files"])
        check("support_bundle_hashes", hash_ok, {"files": len(manifest["files"]),
              "manifest_sha256": hashlib.sha256((bundles[0]/"manifest.json").read_bytes()).hexdigest()})
        try:
            validate_bundle(bundles[0])
            check("strict_bundle_health", True)
        except EvidenceError as error:
            check("strict_bundle_health", False, {"reason": error.code,
                  "health": read(bundles[0]/"diagnostics-health.json")})
    # Raw writer segments, not the session-filtered export, prove sequence coverage.
    sequence_witness = root / "diagnostic-sequences.json"
    if sequence_witness.exists():
        witness = read(sequence_witness)
        if witness["run_id"] != run or witness["process_run_id"] != probe[0]["process_run_id"]:
            raise ValueError("diagnostic_witness_identity")
        segments = witness["segments"]
        events = [{"event_sequence": seq} for segment in segments for seq in segment["sequences"]]
    else:
        segments = sorted((root / "diagnostics").rglob("segment-*.jsonl"))
        events = [r for p in segments for r in rows(p)]
    gaps, previous = [], 0
    for event in events:
        current = event["event_sequence"]
        if current != previous + 1:
            gaps.append({"after": previous, "before": current})
        previous = current
    check("raw_diagnostic_sequence_zero_gaps", bool(events) and not gaps,
          {"segments": len(segments), "events": len(events), "gaps": gaps,
           "sequence_witness": "diagnostic-sequences.json" if sequence_witness.exists() else "raw_copy",
           "note": "sequences extracted from original typed writer segments, not session-filtered export; intentional log-Off suppression reviewed separately"})
    windows_path = root / "uia/uia-log-windows.jsonl"
    if windows_path.exists():
        windows = rows(windows_path)
        paired = []
        for w in windows:
            summary = {}
            for side in ("off", "on"):
                begin = datetime.fromisoformat(w[side+"_start_utc"].replace("Z","+00:00")).timestamp()
                end = datetime.fromisoformat(w[side+"_end_utc"].replace("Z","+00:00")).timestamp()
                values = [r["cpu_pct"] for r in resources if begin <= stamp(r) <= end
                          and r["cpu_pct"] is not None]
                summary[side] = {"seconds": end-begin, "cpu_pct_median":
                                 statistics.median(values) if values else None}
            paired.append(summary)
        check("log_off_on_30_second_windows", len(paired) == 1 and all(
              paired[0][s]["seconds"] >= 30 for s in ("off","on")), paired)
    else:
        check("log_off_on_30_second_windows", False, "missing")
    try:
        performance = [paired_window(w, probe, resources) for w in rows(windows_path)]
        check("log_performance_complete", len(performance) == 1 and all(
              p["cpu_increase_percentage_points"] <= 2 and
              p["p95_increase_upper_bound_percent"] <= 5 for p in performance),
              {"pairs": performance, "cpu_limit_percentage_points": 2,
               "p95_limit_percent": 5, "scope": "one pair; frozen contract still requires at least three pairs"})
    except (OSError, KeyError, ValueError) as error:
        check("log_performance_complete", False, type(error).__name__ + ":" + str(error), missing=True)
    # Annotated copies preserve original observation timestamps, never invent
    # causal IDs at the server. Operation attribution uses the UIA time window.
    requested = sorted((r for r in actions if r["phase"] == "requested"), key=stamp)
    with (root / "correlated-witnesses.jsonl").open("w", encoding="utf-8") as out:
        for r in remote:
            candidates = [a for a in requested if stamp(a) <= stamp(r)]
            if candidates:
                action = candidates[-1]
                out.write(json.dumps({**r, "cycle": action["cycle"],
                    "operation_id": action["operation_id"], "uia_action": action["action"],
                    "correlation": "utc_window_not_server_operation_id"}) + "\n")
    verdicts = [c["status"] for c in checks.values()]
    report["verdict"] = "FAIL" if "FAIL" in verdicts else "INCONCLUSIVE" if "UNKNOWN" in verdicts else "PILOT_PASS"
    fix_checks = ("uia_lifecycle", "telemetry_no_loss", "product_media_metrics_fresh",
                  "strict_bundle_health", "checkpoint_archive_integrity",
                  "checkpoint_revision_1_through_terminal")
    report["telemetry_fix_verdict"] = "PASS" if all(checks.get(k, {}).get("status") == "PASS"
                                                       for k in fix_checks) else "NOT_VERIFIED"
    report["check_counts"] = dict(Counter(verdicts))
    report["reviewed_utc"] = datetime.now(timezone.utc).isoformat()
    (root / "pilot-review.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"verdict": report["verdict"], "checks": report["check_counts"],
                      "failed": [k for k,v in checks.items() if v["status"] != "PASS"]}))
    return 0 if report["verdict"] == "PILOT_PASS" else 1


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, required=True)
    raise SystemExit(review(p.parse_args().root))
