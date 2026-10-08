"""Streaming review of the fixed-load product run; never substitutes UI for media.

The original strict verifier remains available. This review checks the actual
product's DXGI budgets on all discovered nodes separately from WDDM counters.
GPU queues, physical microphone capture and WGC handle ownership stay explicit.
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
import re
from pathlib import Path
import statistics
from datetime import datetime, timezone

from product_pilot_performance import paired_window
from product_pilot_correlation import correlate
from product_pilot_audio import review_outbound_audio
from product_pilot_archive import read_segment
from product_pilot_load import review_lifecycle
from product_pilot_scheduler import review_scheduler_policy
from product_pilot_desktop_policy import bind_desktop_input_policy
from product_pilot_diagnostics import diagnostic_source, safe_event
from analyze_product_gpu_budget import ALL_NODES_SCOPE, MAXIMUM_OBSERVER_GAP_MS, review_cycle as review_gpu_cycle, stamp as gpu_stamp
from product_gpu_queue import SCOPE as GPU_QUEUE_SCOPE, review_cycle as review_gpu_queue_cycle, review_diagnostic_bounds
from product_gpu_etw import validate_live_capture, reconstruct as reconstruct_gpu_etw, review_hybrid_cycle, validate_frozen_limits
from verify_product_acceptance import validate_bundle, EvidenceError, instant


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def records(path):
    with path.open(encoding="utf-8-sig") as stream:
        for line in stream:
            if line.strip():
                yield json.loads(line)


def timestamp(r):
    return r["utc_ms"]/1000 if "utc_ms" in r else datetime.fromisoformat(r["utc"].replace("Z", "+00:00")).timestamp()


def product_lifetime_floor(result, identity, run, actions):
    """Bind the pre-close monotonic product lifetime; old schemas stay UNKNOWN."""
    origin=result.get("product_first_live")
    fields=("pid","start_ticks","executable","utc","run_clock_elapsed_origin_seconds","clock_source","origin_kind")
    proof=dict(passed=False,status="UNKNOWN",reason="product_lifetime_fields_missing")
    if (not isinstance(origin,dict) or any(k not in origin for k in fields)
            or any(k not in result for k in ("product_live_elapsed_seconds_before_close","product_lifetime_floor_required_seconds"))):
        return proof
    proof.update(status="FAIL",reason="product_lifetime_identity_or_clock_invalid")
    pid,ticks,executable=(origin[k] for k in ("pid","start_ticks","executable"))
    if (type(run) is not str or not run or result.get("run_id") != run or identity.get("run_id") != run
            or type(pid) is not int or pid <= 0 or type(identity.get("pid")) is not int or identity["pid"] != pid
            or type(ticks) is not int or not 0 < ticks <= (1 << 63)-1
            or type(identity.get("start_ticks")) is not int or identity["start_ticks"] != ticks
            or type(executable) is not str or not executable or not Path(executable).is_absolute()
            or identity.get("executable") != executable or type(origin["utc"]) is not str
            or origin["clock_source"] != "System.Diagnostics.Stopwatch" or origin["origin_kind"] != "first_live_requested"):
        return proof
    try:
        first_live_utc=instant(origin["utc"])
    except (EvidenceError,ValueError):
        return proof
    if first_live_utc.utcoffset().total_seconds() != 0:
        return proof
    first_requested=next((a for a in actions if a.get("phase") == "requested" and a.get("run_id") == run
        and type(a.get("pid")) is int and a["pid"] == pid),None)
    if first_requested is None or any(k not in first_requested for k in ("elapsed_seconds","utc")):
        proof.update(status="UNKNOWN",reason="product_first_live_requested_frame_missing")
        return proof
    exit_requested=next((a for a in reversed(actions) if a.get("action") == "process_exit"
        and a.get("phase") == "requested" and a.get("run_id") == run
        and type(a.get("pid")) is int and a["pid"] == pid),None)
    if exit_requested is None or "elapsed_seconds" not in exit_requested:
        proof.update(status="UNKNOWN",reason="product_exit_requested_frame_missing")
        return proof
    elapsed=result["product_live_elapsed_seconds_before_close"]
    required=result["product_lifetime_floor_required_seconds"]
    minimum=result.get("minimum_seconds")
    if type(required) is not int or type(minimum) is not int:
        return proof
    values=(origin["run_clock_elapsed_origin_seconds"],first_requested["elapsed_seconds"],
        exit_requested["elapsed_seconds"],elapsed,required,minimum)
    # Exact JSON types reject bool/string; Python ints are finite without a lossy float cast.
    if any(not (type(v) is int or type(v) is float and math.isfinite(v)) or v < 0 for v in values):
        return proof
    if (origin["run_clock_elapsed_origin_seconds"] != first_requested["elapsed_seconds"]
            or origin["utc"] != first_requested["utc"]):
        proof["reason"]="product_first_live_requested_frame_mismatch"
        return proof
    try:
        requested_elapsed=exit_requested["elapsed_seconds"]-first_requested["elapsed_seconds"]
    except OverflowError:
        proof["reason"]="product_lifetime_requested_clock_invalid"
        return proof
    if type(requested_elapsed) is float and not math.isfinite(requested_elapsed):
        proof["reason"]="product_lifetime_requested_clock_invalid"
        return proof
    proof.update(product_first_live=origin,product_live_elapsed_seconds_before_close=elapsed,
        first_requested_to_exit_requested_elapsed_seconds=requested_elapsed,
        product_lifetime_floor_required_seconds=required,reason="product_lifetime_floor_not_reached")
    passed=required >= 28800 and required == minimum and elapsed >= required and requested_elapsed >= required
    proof.update(passed=passed,status="PASS" if passed else "FAIL")
    if passed:
        proof.pop("reason")
    return proof


def collector_exits_complete(plan, collectors, exits):
    roles=["resource_pid","archive_pid","diagnostic_pid"]
    if plan.get("gpu_etw_observer"):
        roles.append("gpu_trace_pid")
        if collectors.get("gpu_trace_session") != "B14-Gpu-Release-"+plan["run_id"]:
            return False
    expected=[collectors.get(k) for k in roles]
    rows=exits.get("collectors",[])
    return (collectors.get("run_id")==plan["run_id"]==exits.get("run_id")
        and all(type(p) is int and p>0 for p in expected) and len(set(expected))==len(expected)
        and isinstance(rows,list) and len(rows)==len(expected)
        and all(type(r.get("pid")) is int and r["pid"]>0 and r.get("exit_code")==0 and r.get("forced_stop") is False for r in rows)
        and {r["pid"] for r in rows}==set(expected))


def review_diagnostic_terminal(root, identity, run, diagnostics, witness, watcher):
    """Prove all observed events, then match the native retained suffix exactly.

    Native quota can remove a closed prefix after the watcher validated it. This
    proves the complete external typed witness and the retained native bytes;
    it never claims that pruned raw attributes are still stored externally.
    """
    pid = identity.get("pid")
    process_run = witness.get("process_run_id")
    if (identity.get("run_id") != run or type(pid) is not int or pid <= 0
            or type(witness.get("schema")) is not int or witness["schema"] != 2
            or witness.get("run_id") != run or type(witness.get("pid")) is not int or witness["pid"] != pid
            or not isinstance(process_run, str) or not re.fullmatch("[0-9a-f]{32}", process_run)
            or watcher.get("run_id") != run or watcher.get("process_run_id") != process_run
            or type(watcher.get("pid")) is not int or watcher["pid"] != pid or watcher.get("status") != "COMPLETE"
            or type(watcher.get("events")) is not int or watcher["events"] != len(diagnostics)
            or not diagnostics):
        raise ValueError("diagnostic_terminal_witness_identity_or_completion")
    for sequence, event in enumerate(diagnostics, 1):
        if (event.get("run_id") != run or event.get("process_run_id") != process_run
                or type(event.get("pid")) is not int or event["pid"] != pid
                or type(event.get("event_sequence")) is not int or event["event_sequence"] != sequence
                or not isinstance(event.get("raw_sha256"), str) or not re.fullmatch("[0-9a-f]{64}", event["raw_sha256"])
                or not isinstance(event.get("source_segment"), str) or not re.fullmatch(r"segment-[0-9]{6}\.jsonl", event["source_segment"])):
            raise ValueError("diagnostic_continuous_identity_sequence_or_hash")
    probe, source = diagnostic_source(root)
    if probe.get("run_id") != run or probe.get("process_run_id") != process_run:
        raise ValueError("diagnostic_terminal_probe_identity")
    segments = witness.get("segments")
    if not isinstance(segments, list) or not segments:
        raise ValueError("diagnostic_terminal_segments_missing")
    actual_paths = sorted(source.glob("segment-*.jsonl"))
    if [p.name for p in actual_paths] != [s.get("file") for s in segments]:
        raise ValueError("diagnostic_terminal_segment_set_changed")
    first, last, native_events, native_bytes, previous_segment = None, None, 0, 0, None
    anchor = None
    for path, segment in zip(actual_paths, segments):
        if not re.fullmatch(r"segment-[0-9]{6}\.jsonl", path.name) or path.is_symlink():
            raise ValueError("diagnostic_terminal_segment_path")
        index = int(path.name[8:14])
        if previous_segment is not None and index != previous_segment + 1:
            raise ValueError("diagnostic_terminal_segment_gap")
        previous_segment = index
        if path.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("diagnostic_terminal_segment_budget")
        content = path.read_bytes()
        if (type(segment.get("size_bytes")) is not int or len(content) != segment["size_bytes"]
                or hashlib.sha256(content).hexdigest() != segment.get("sha256")
                or not content or not content.endswith(b"\n")):
            raise ValueError("diagnostic_terminal_segment_hash_size_or_complete_line")
        native_bytes += len(content)
        lines = content.splitlines(keepends=True)
        claimed_rows, claimed_sequences = segment.get("records"), segment.get("sequences")
        if (not isinstance(claimed_rows, list) or not isinstance(claimed_sequences, list)
                or len(lines) != len(claimed_rows) or len(lines) != len(claimed_sequences)):
            raise ValueError("diagnostic_terminal_row_witness_missing")
        for line, claimed, claimed_sequence in zip(lines, claimed_rows, claimed_sequences):
            event = json.loads(line)
            bounded = safe_event(event, process_run)
            sequence = bounded["event_sequence"]
            raw_hash = hashlib.sha256(line).hexdigest()
            if (type(sequence) is not int or sequence < 1 or sequence > len(diagnostics)
                    or bounded["pid"] != pid or (last is not None and sequence != last + 1)
                    or type(claimed_sequence) is not int or claimed_sequence != sequence
                    or claimed != dict(event_sequence=sequence, raw_sha256=raw_hash)):
                raise ValueError("diagnostic_terminal_retained_suffix_sequence_or_identity")
            observed = diagnostics[sequence - 1]
            native_safe = {k: v for k, v in observed.items() if k not in ("run_id", "source_segment", "raw_sha256")}
            if (observed["source_segment"] != path.name or observed["raw_sha256"] != raw_hash
                    or native_safe != bounded):
                raise ValueError("diagnostic_terminal_native_line_not_exactly_observed")
            first = sequence if first is None else first
            last = sequence
            native_events += 1
            anchor = dict(event_sequence=sequence, event_name=event["event_name"],
                outcome=event.get("attributes", {}).get("outcome"),
                drain_result=event.get("attributes", {}).get("drain_result"), raw_sha256=raw_hash)
    if (last != len(diagnostics) or anchor != witness.get("terminal_anchor")
            or anchor["event_name"] != "process.terminal" or anchor["outcome"] != "success"
            or anchor["drain_result"] != "completed"):
        raise ValueError("diagnostic_terminal_tail_or_successful_process_anchor_missing")
    return dict(passed=True, continuous_events=len(diagnostics), native_retained_events=native_events,
        native_retained_bytes=native_bytes, first_native_retained_sequence=first,
        terminal_sequence=last, pruned_native_prefix_events=first - 1,
        complete_external_typed_witness=True, complete_raw_native_retention=first == 1,
        proof="continuous sequence 1..terminal plus exact native retained suffix identity and per-line SHA256",
        limitation="external typed witness excludes raw attributes; native prefix may be legitimately pruned")


def cleanup_release(probe, actions, next_join_requested=None):
    """Review the global queue only inside this cycle's settled release window.

    The probe SID comes from the most recent history snapshot, whereas the
    pending count also includes device discovery and next-join preparation.
    Export completion bounds this cycle; a subsequent join bounds it even
    earlier if actions overlap. Process-exit draining is reviewed separately.
    """
    start = timestamp(actions["leave", "uia_observed"]) + 5
    end = timestamp(actions["export", "uia_observed"])
    if next_join_requested is not None:
        end = min(end, timestamp(next_join_requested))
    sid = actions["join", "uia_observed"]["anonymous_session_id"]
    settled = [p for p in probe if p.get("anonymous_session_id") == sid
               and p.get("session_complete") and start <= timestamp(p) < end]
    times = [timestamp(p) for p in settled]
    pending = [p.get("native_cleanup_pending") for p in settled]
    covered = (len(times) >= 2 and times[0] <= start + 2
               and times[-1] >= end - 2 and times[-1] - times[0] >= 5
               and all(0 < b - a <= 2 for a, b in zip(times, times[1:])))
    passed = covered and all(type(v) is int and v == 0 for v in pending)
    return passed, dict(scope="global queue in settled cycle release window",
        start_utc=datetime.fromtimestamp(start, timezone.utc).isoformat(),
        end_utc=datetime.fromtimestamp(end, timezone.utc).isoformat(),
        samples=len(settled), coverage_complete=covered,
        maximum_pending=max(pending) if pending and all(type(v) is int for v in pending) else None)


def groups(path, key, run):
    group, number, sequence = [], None, 0
    for row in records(path):
        if row.get("run_id") != run:
            raise ValueError("collector_run_mismatch:"+path.name)
        if "sequence" in row:
            sequence += 1
            if row["sequence"] != sequence:
                raise ValueError("collector_sequence_gap:"+path.name)
        cycle = key(row)
        if cycle is None:
            continue
        if number is not None and cycle != number:
            if cycle < number:
                raise ValueError("collector_cycle_regressed:"+path.name)
            yield number, group
            group = []
        number = cycle
        group.append(row)
    if number is not None:
        yield number, group


def archive_session(root, session, entries):
    revisions, generations, total, stored_total = set(), set(), 0, 0
    metric_summary={}
    for entry in entries:
        path = root / session / entry["file"]
        if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
            raise ValueError("archive_path_escape")
        digest, size = hashlib.sha256(), 0
        content = read_segment(root / session, entry)
        stored_total += entry.get("stored_bytes", len(content))
        for line in content.splitlines(keepends=True):
            digest.update(line); size += len(line)
            row = json.loads(line)
            # Metric rows carry generation/revision; session identity is
            # bound by the hashed segment entry and containing manifest.
            if not entry["first_revision"] <= row["revision"] <= entry["last_revision"]:
                raise ValueError("archive_revision_identity")
            revisions.add(row["revision"]); generations.add(row["session_generation"])
            key=row.get("key","")
            if key.startswith(("network.inbound.","video.codec.","reconnect.","queue.","resource.internal.")):
                summary=metric_summary.setdefault(key,dict(availability={},valid_samples=0))
                availability=row.get("availability","UNKNOWN")
                summary["availability"][availability]=summary["availability"].get(availability,0)+1
                value=row.get("value")
                if availability.upper()=="VALID" and value is not None:
                    summary["valid_samples"]+=1
                    summary["last"]=value
                    if isinstance(value,(int,float)):
                        summary["maximum"]=max(summary.get("maximum",value),value)
                    elif isinstance(value,str):
                        summary["observed"]=sorted(set(summary.get("observed",[]))|{value})
        if size != entry["size_bytes"] or digest.hexdigest() != entry["sha256"]:
            raise ValueError("archive_hash_or_size_mismatch")
        total += size
    m = read(root/session/"manifest.json")
    last = m["last_committed_revision"]
    if m["anonymous_session_id"] != session or not revisions or min(revisions) != 1 or max(revisions) != last or len(revisions) != last \
            or generations != {m["session_generation"]} or not m["session_complete"]:
        raise ValueError("archive_revision_gap_or_nonterminal")
    return dict(first_revision=1, last_revision=last, missing_revisions=0,
                archived_bytes=total, segments=len(entries), product_pruned_records=m["pruned_records"],
                stored_segment_bytes=stored_total, native_metric_summary=metric_summary)


def interrupted_review(root, error, output=None):
    output=root if output is None else output
    """Preserve partial progress without assigning unreviewed cycles PASS."""
    plan=read(root/"plan.json")
    actions=list(records(root/"uia/uia-actions.jsonl")) if (root/"uia/uia-actions.jsonl").exists() else []
    by_cycle=defaultdict(list)
    for action in actions: by_cycle[action["cycle"]].append(action)
    result=read(root/"uia/uia-result.json") if (root/"uia/uia-result.json").exists() else {}
    controller=read(root/"controller-result.json") if (root/"controller-result.json").exists() else {}
    reason=controller.get("reason") or result.get("reason") or str(error)
    verdict="TIMEOUT" if "TIMEOUT" in reason else "CRASH" if "PRODUCT_EXIT_CODE" in reason else "FAIL"
    cycles=[]
    for number,values in sorted(by_cycle.items()):
        completed={a["action"] for a in values if a["phase"]=="uia_observed"}
        cycles.append(dict(cycle=number,cycle_id=values[0]["cycle_id"],
            verdict=verdict if number==max(by_cycle) else "BLOCKED",
            uia_completed="export" in completed,reason="independent_review_incomplete",
            observed_actions=sorted(completed)))
    report=dict(schema=1,run_id=plan["run_id"],mode=plan["mode"],verdict="FAIL",
        desktop_input_policy=plan.get("desktop_input_policy","strict"),
        reason=reason,review_error=dict(type=type(error).__name__,reason=str(error)),
        cycles=cycles,cycle_counts=dict(Counter(c["verdict"] for c in cycles)),
        cycles_requested=plan["cycles"],cycles_not_run=plan["cycles"]-len(cycles),
        reviewed_utc=datetime.now(timezone.utc).isoformat())
    (output/"external-review.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(dict(verdict="FAIL",reason=reason,cycles=report["cycle_counts"])))


def review(root, output=None):
    output=root if output is None else output
    plan, result, limits = read(root/"plan.json"), read(root/"uia/uia-result.json"), read(root/"limits.json")
    input_policy=bind_desktop_input_policy(plan,result)
    run = plan["run_id"]
    identity = read(root/"uia/product-identity.json")
    if identity.get("run_id") != run or Path(identity["executable"]).parent.name != "RelWithDebInfo":
        raise ValueError("gpu_budget_product_identity_or_configuration")
    formal = plan["mode"] == "Formal"
    count = 100 if formal else plan["cycles"]
    if not formal and (count < 2 or plan["seconds"] < 240 * count):
        raise ValueError("pilot_rejoin_profile_required")
    actions = list(records(root/"uia/uia-actions.jsonl"))
    cycle_actions = defaultdict(list)
    for action in actions:
        cycle_actions[action["cycle"]].append(action)
    if set(cycle_actions) != set(range(1, count+1)):
        raise ValueError("incomplete_lifecycle_count")
    sessions, windows = {}, {}
    for cycle, values in cycle_actions.items():
        joined = [a for a in values if a["action"] == "join" and a["phase"] == "uia_observed"]
        if len(joined) != 1 or not joined[0]["anonymous_session_id"]:
            raise ValueError("cycle_native_session_unavailable")
        sessions[joined[0]["anonymous_session_id"]] = cycle
        windows[cycle] = (min(timestamp(a) for a in values), max(timestamp(a) for a in values))
    if len(sessions) != count:
        raise ValueError("not_distinct_native_lifecycles")
    streams = {
        "probe": groups(root/"process-probe.jsonl", lambda r:sessions.get(r.get("anonymous_session_id")), run),
        "resources": groups(root/"external-resources.jsonl", lambda r:r["cycle"], run),
        "remote": groups(root/"remote.jsonl", lambda r:(r.get("observer_context") or {}).get("cycle"), run),
        "audio": groups(root/"product-audio.jsonl", lambda r:next((c for c,(a,b) in windows.items() if a <= timestamp(r) <= b), None), run)}
    archives = defaultdict(list)
    archive_events = list(records(root/"checkpoint-archive/collector.jsonl"))
    if archive_events[-1]["event"] != "collector.stopped" or archive_events[-1].get("status") != "COMPLETE":
        raise ValueError("archive_collector_not_complete")
    for i, e in enumerate(archive_events, 1):
        if e["run_id"] != run or e["sequence"] != i:
            raise ValueError("archive_collector_identity")
        if e["event"] == "segment.archived":
            archives[e["session"]].append(e)
    diagnostics = list(records(root/"diagnostic-events.jsonl"))
    if not diagnostics or any(d["run_id"] != run or d["event_sequence"] != i for i,d in enumerate(diagnostics,1)):
        raise ValueError("continuous_diagnostic_sequence_gap")
    log_windows = {r["cycle"]:r for r in records(root/"uia/uia-log-windows.jsonl")}
    report = dict(schema=1, run_id=run, mode=plan["mode"], verdict="FAIL", cycles=[],
        desktop_input_policy=input_policy,
        started_utc=result["started_utc"], finished_utc=result["finished_utc"],
        deferred=["gpu_queue", "wgc_handle_ownership",
                  "slow_disk", "power_loss", "real_crash_recovery", "release_symbol_review",
                  "audio_pixel_quality", "physical_multimonitor_coordinates"])
    devices = read(root/"audio-devices.json")
    device_path = root/"uia/uia-device-outcomes.jsonl"
    device_outcomes = list(records(device_path)) if device_path.exists() else []
    if devices["active_capture_endpoints"] == 0:
        report["deferred"].append("outbound_microphone_no_active_capture_endpoint")
    release_details = []
    frozen_gpu, frozen_policy = False, None
    if plan.get("gpu_queue_limits_sha256"):
        if plan.get("diagnostic_only") or (root/"diagnostic-debugger.json").exists():
            raise ValueError("gpu_queue_frozen_limits_cannot_qualify_diagnostic")
        policy_path=root/"gpu-queue-limits.json"
        if hashlib.sha256(policy_path.read_bytes()).hexdigest()!=plan["gpu_queue_limits_sha256"]:
            raise ValueError("gpu_queue_frozen_limits_hash_changed")
        frozen_policy=read(policy_path)
        validate_frozen_limits(frozen_policy)
        frozen_gpu=True
        report["gpu_queue_frozen_limits"]=dict(sha256=plan["gpu_queue_limits_sha256"],policy=frozen_policy)
    gpu_etw_proof, gpu_etw_error = None, None
    if plan.get("gpu_etw_observer"):
        try:
            expected=dict(required=True,scope="process_owned_device_context_scheduler_packet_lifecycle",
                mode="lossless realtime owner-filtered JSONL",maximum_bytes=frozen_policy["etw_maximum_bytes"] if frozen_gpu else 8589934592,
                storage_budget_status="FROZEN_B14_TEST" if frozen_gpu else "PROVISIONAL_DIAGNOSTIC")
            if plan["gpu_etw_observer"] != expected:
                raise ValueError("gpu_etw_observer_contract_changed")
            gpu_root=root/"gpu-etw"
            summary=read(gpu_root/"trace-summary.json")
            validate_live_capture(read(gpu_root/"trace-ready.json"),summary,identity,expected["maximum_bytes"])
            if (gpu_root/"events.jsonl").stat().st_size != summary["written_bytes"]:
                raise ValueError("gpu_etw_persisted_size_mismatch")
            gpu_windows=[]
            for values in cycle_actions.values():
                endpoints={(v['action'],v['phase']):int(gpu_stamp(v)*10000)+116444736000000000 for v in values}
                gpu_windows.extend(((endpoints['join','uia_observed'],endpoints['leave','requested']),
                    (endpoints['leave','uia_observed']+50000000,endpoints['export','requested'])))
            gpu_etw_proof=reconstruct_gpu_etw(records(gpu_root/"events.jsonl"),identity["pid"],summary,windows=gpu_windows)
            if any(gpu_etw_proof[k] for k in ("final_pending","final_contexts","final_devices")):
                raise ValueError("gpu_etw_final_product_resources_not_released")
            exited=[a for a in actions if a["action"]=="process_exit" and a["phase"]=="uia_observed"]
            if len(exited)!=1 or summary["trace_end_filetime_100ns"] < int(timestamp(exited[0])*10000000)+116444736000000000:
                raise ValueError("gpu_etw_capture_does_not_span_product_exit")
            report["gpu_etw_capture"]={k:v for k,v in gpu_etw_proof.items() if k!="transitions"}
            report["gpu_etw_capture"]["collector_summary"]=summary
        except (ValueError,KeyError,TypeError,OSError) as error:
            gpu_etw_error=str(error)
    for cycle in range(1,count+1):
        data = {}
        for name, stream in streams.items():
            n, rows = next(stream)
            if n != cycle: raise ValueError("collector_cycle_missing:"+name)
            data[name] = rows
        probe, resources, remote, audio = (data[k] for k in ("probe","resources","remote","audio"))
        a = {(v["action"],v["phase"]):v for v in cycle_actions[cycle]}
        checks, details = {}, {}
        def check(name, ok, detail=None):
            checks[name] = "PASS" if ok else "FAIL"
            if detail is not None: details[name] = detail
        required = ("join","page","share_start","share_stop","logging","leave","export")
        check("uia_actions", all((name,phase) in a for name in required for phase in ("requested","uia_observed")))
        try:
            expected_observer = dict(required=True, scope=ALL_NODES_SCOPE, maximum_gap_ms=MAXIMUM_OBSERVER_GAP_MS)
            if plan.get("gpu_budget_observer") != expected_observer:
                raise ValueError("gpu_budget_observer_contract_missing_or_changed")
            gpu_proof = review_gpu_cycle(probe, run, identity["pid"], cycle_actions[cycle], ALL_NODES_SCOPE)
            check("dxgi_node0_budget_coverage", True, gpu_proof)
            check("dxgi_all_nodes_budget_coverage", True, gpu_proof)
        except (ValueError, KeyError, TypeError) as error:
            check("dxgi_node0_budget_coverage", False, str(error))
            check("dxgi_all_nodes_budget_coverage", False, str(error))
            if "dxgi_local_nonlocal_budget" not in report["deferred"]:
                report["deferred"].append("dxgi_local_nonlocal_budget")
        try:
            if plan.get("gpu_queue_observer") != dict(required=True, scope=GPU_QUEUE_SCOPE,
                    maximum_gap_ms=MAXIMUM_OBSERVER_GAP_MS, queue_bounds="FROZEN_B14_TEST" if frozen_gpu else "NOT_FROZEN"):
                raise ValueError("gpu_queue_observer_contract_missing_or_changed")
            if plan.get("gpu_etw_observer"):
                if gpu_etw_error: raise ValueError(gpu_etw_error)
                queue_proof = review_gpu_queue_cycle(probe, run, identity["pid"], cycle_actions[cycle], active_only=True)
            else:
                queue_proof = review_gpu_queue_cycle(probe, run, identity["pid"], cycle_actions[cycle])
            check("gpu_scheduler_packets_coverage", True, queue_proof)
            if frozen_gpu:
                queue_limits=review_hybrid_cycle(gpu_etw_proof,queue_proof,cycle_actions[cycle],frozen_policy,frozen=True)
                check("gpu_scheduler_packets_frozen_limits",queue_limits["passed"],queue_limits)
                check("gpu_queue_coverage",queue_limits["gpu_measurement_complete"],dict(metric=frozen_policy["metric"],
                    scope=frozen_policy["scope"],etw_scope=frozen_policy["etw_scope"],policy_sha256=plan["gpu_queue_limits_sha256"],
                    active=queue_limits["continuous_active"],release=queue_limits["continuous_release"],
                    dma_faults_delta=queue_limits["maximum_dma_faults_delta_per_node"],limitations=frozen_policy["limitations"]))
            elif plan.get("gpu_queue_diagnostic_policy_sha256"):
                policy_path = root/"gpu-queue-diagnostic-policy.json"
                if (not plan.get("diagnostic_only") or plan.get("release_eligible") is not False
                        or formal or not (root/"diagnostic-debugger.json").exists()
                        or hashlib.sha256(policy_path.read_bytes()).hexdigest() != plan["gpu_queue_diagnostic_policy_sha256"]):
                    raise ValueError("gpu_queue_provisional_policy_requires_diagnostic_identity")
                if plan.get("gpu_etw_observer"):
                    queue_limits=review_hybrid_cycle(gpu_etw_proof,queue_proof,cycle_actions[cycle],read(policy_path))
                else:
                    queue_limits = review_diagnostic_bounds(probe, run, identity["pid"], cycle_actions[cycle], queue_proof, read(policy_path))
                check("gpu_scheduler_packets_provisional_limits", queue_limits["passed"], queue_limits)
        except (ValueError, KeyError, TypeError) as error:
            check("gpu_scheduler_packets_coverage", False, str(error))
            if frozen_gpu:check("gpu_queue_coverage",False,str(error))
        start, end = timestamp(a["join","uia_observed"]), timestamp(a["leave","requested"])
        outbound = review_outbound_audio(run,cycle_actions[cycle],remote,devices,device_outcomes,limits["max_audio_gap_ms"])
        for name, outcome in outbound.items():
            checks[name] = outcome["status"]
            details[name] = outcome["detail"]
        sid = a["join","uia_observed"]["anonymous_session_id"]
        try:
            correlation=correlate(run,cycle_actions[cycle],remote,diagnostics)
            check("correlation",True,correlation)
        except ValueError as error: check("correlation",False,str(error))
        for name, rows in data.items():
            if name == "audio": rows = [r for r in rows if r["event"] == "audio.sample"]
            times=[timestamp(r) for r in rows]
            gaps=[y-x for x,y in zip(times,times[1:]) if y>=start and x<=end]
            check(name+"_coverage",bool(times) and times[0]<=start+2 and times[-1]>=end-2 and max(gaps,default=999)<=10)
        health={key:max(p["history"][key] for p in probe) for key in ("queue_drops","pending_records_dropped","write_failures","queue_peak_jobs","queue_peak_bytes")}
        check("telemetry_no_loss", all(health[k]==0 for k in ("queue_drops","pending_records_dropped","write_failures")) and
              health["queue_peak_jobs"]<=limits["telemetry_queue_jobs"] and health["queue_peak_bytes"]<=limits["telemetry_queue_bytes"],health)
        check("diagnostic_no_loss",all(not p["diagnostic"][k] for p in probe for k in ("dropped_ordinary","dropped_critical","sink_failures")))
        try:
            proof=archive_session(root/"checkpoint-archive",sid,archives[sid])
            check("revision_1_to_terminal",True,proof)
            check("memory_checkpoint_final",probe[-1]["revision"]==probe[-1]["history"]["checkpoint_revision"]==proof["last_revision"])
        except (OSError,ValueError) as error: check("revision_1_to_terminal",False,str(error))
        next_join = next((v for v in cycle_actions.get(cycle+1, [])
                          if v["action"] == "join" and v["phase"] == "requested"), None)
        released_ok, release_proof = cleanup_release(probe, a, next_join)
        check("native_cleanup_released", released_ok, release_proof)
        bundles=list((root/f"uia/export-{cycle:04}").glob("cohavora-diagnostic-bundle-*"))
        try:
            if len(bundles)!=1: raise ValueError("support_bundle_count")
            validate_bundle(bundles[0])
            if read(bundles[0]/"manifest.json")["anonymous_session_id"]!=sid: raise ValueError("wrong_cycle_bundle")
            check("support_bundle",True)
        except (EvidenceError,ValueError) as error: check("support_bundle",False,str(error))
        product={r["participant"] for r in remote if r["event"]=="receiver.participant_joined"}
        left={r["participant"] for r in remote if r["event"]=="receiver.participant_left"}
        check("peer_join_leave",len(product)==1 and product<=left)
        screen=[r for r in remote if r["event"]=="receiver.sample" and r["source"]==3 and r["kind"]=="video"]
        screen_ids={r["sid"] for r in screen}
        unpublished={r["sid"] for r in remote if r["event"]=="receiver.track_unpublished" and r["source"]==3}
        packets=max((int(s["received"]["packets_received"]) for r in screen for s in r["stats"]),default=0)
        decoded=max((int(s["inbound"]["frames_decoded"]) for r in screen for s in r["stats"]),default=0)
        check("independent_screen_delivery", bool(screen_ids) and screen_ids<=unpublished and packets>0 and decoded>0,
              dict(rtp_packets=packets,decoded_frames=decoded))
        subscribed_video={r["sid"] for r in remote if r["event"]=="receiver.track_subscribed" and r.get("kind")=="video"}
        closed_video=[r for r in remote if r["event"]=="receiver.stream_closed" and r.get("kind")=="video"]
        check("independent_video_buffers_released", bool(subscribed_video) and
            subscribed_video <= {r["sid"] for r in closed_video} and all(
                r.get("active") is False and
                isinstance(r.get("video_counter"),dict) and
                r["video_counter"].get("frames_observed")==r["video_counter"].get("buffers_released") and
                type(r["video_counter"].get("frames_observed")) is int and
                r["video_counter"]["frames_observed"]>=r["frames"] for r in closed_video),
            dict(streams=[dict(sid=r["sid"],consumed_frames=r["frames"],counter=r.get("video_counter")) for r in closed_video],
                scope="real decoded-frame delivery and all native buffer releases; pixel quality unmeasured"))
        backend={p["capture"]["backend"] for p in probe if p["capture"]["frames"]>0}
        check("backend_observed",bool(backend) and backend <= {"dxgi","wgc","gdi"},sorted(backend))
        playout=[r for r in audio if r["event"]=="audio.sample" and start+1<=timestamp(r)<=end]
        check("inbound_audio_continuity",bool(playout) and all(r["max_packet_gap_ms"]<=limits["max_audio_gap_ms"] and r["window_signal_frames"]>0 and not r["timestamp_errors"] and not r["discontinuities"] for r in playout),
              dict(samples=len(playout),maximum_gap_ms=max((r["max_packet_gap_ms"] for r in playout),default=None)))
        server=[r for r in remote if r["event"]=="server.resource" and start<=timestamp(r)<=end]
        bps=[sum(i["tx_bps"] for i in r["network"].values()) for r in server]
        check("network_capacity",bool(bps) and max(bps)<=limits["max_server_egress_bps"],
              dict(peak_bps=max(bps,default=None),mean_bps=statistics.mean(bps) if bps else None,
                   samples_above_limit=sum(v>limits["max_server_egress_bps"] for v in bps),
                   samples_above_95_percent=sum(v>.95*limits["max_server_egress_bps"] for v in bps),
                   samples=len(bps),scope="whole server NIC, including other active room"))
        sfu=[r for r in remote if r["event"]=="sfu.snapshot" and start<=timestamp(r)<=end]
        prefix="pilot-"+run[:8]+"-load-"
        check("fixed_load_published",bool(sfu) and all(
            len([p for p in r["participants"] if p["identity"].startswith(prefix) and
                 any(t["source"]==1 and not t["muted"] for t in p["tracks"])])==10 for r in sfu))
        check("remote_errors",not any(r["event"]=="collector.error" for r in remote))
        route_samples=[]
        for p in probe:
            if start+10 <= timestamp(p) <= end:
                metrics={m["key"]:m["value"] for m in p.get("metrics",[]) if m["availability"]=="VALID"}
                route_samples.append(dict(age_s=timestamp(p)-p["source_utc_ms"]/1000,
                    rtp=metrics.get("video.pipeline.inbound_rtp_streams"),
                    decode=metrics.get("video.pipeline.active_decode_streams"),
                    audio_samples=metrics.get("audio.window.samples")))
        check("actual_rtp_decode_continuity", bool(route_samples) and all(
            r["age_s"]<=10 and isinstance(r["rtp"],int) and r["rtp"]>0 and
            isinstance(r["decode"],int) and r["decode"]>0 for r in route_samples),
            dict(samples=len(route_samples), actual_rtp_counts=sorted({r["rtp"] for r in route_samples if r["rtp"] is not None}),
                 actual_decode_counts=sorted({r["decode"] for r in route_samples if r["decode"] is not None})))
        connection=[r for r in remote if r["event"]=="receiver.connection_sample" and start<=timestamp(r)<=end]
        loss_by_stream={}
        codecs=set()
        for r in connection:
            for c in r.get("codecs",[]):
                if c.get("codec",{}).get("mime_type"): codecs.add(c["codec"]["mime_type"])
            for s in r.get("inbound",[]):
                key=s.get("rtc",{}).get("id") or str(s.get("stream",{}).get("ssrc"))
                v=int(s.get("received",{}).get("packets_lost",0))
                loss_by_stream[key]=max(loss_by_stream.get(key,0),v)
        details["independent_rtp_stats"]=dict(codecs=sorted(codecs),packets_lost_by_stream=loss_by_stream,
            scope="receiver subscribed to product; does not infer product downlink packet loss")
        try:
            pair=paired_window(log_windows[cycle],probe,resources)
            check("logging_performance",pair["cpu_increase_percentage_points"]<=limits["cpu_increase_percentage_points"] and pair["p95_increase_upper_bound_percent"]<=limits["render_p95_increase_percent"],pair)
        except (KeyError,ValueError) as error: check("logging_performance",False,str(error))
        active=[r for r in resources if start<=timestamp(r)<=end]
        check("wddm_coverage",bool(active) and all(r["gpu_dedicated_bytes"] is not None and r["gpu_shared_bytes"] is not None for r in active))
        before=[r for r in active if timestamp(r)<=timestamp(a["share_start","requested"])]
        after=[r for r in active if timestamp(a["share_stop","uia_observed"])+5<=timestamp(r)<=timestamp(a["logging","requested"])]
        released=[r for r in resources if timestamp(a["leave","uia_observed"])+5<=timestamp(r)<=timestamp(a["export","requested"])]
        if before and after and released:
            b,f=before[-1],after[-1]
            private_delta=(f["private_bytes"]-b["private_bytes"])/1024**2
            gpu_delta=((f["gpu_dedicated_bytes"]+f["gpu_shared_bytes"])-(b["gpu_dedicated_bytes"]+b["gpu_shared_bytes"]))/1024**2
            check("share_stop_resources",private_delta<=limits["share_stop_private_delta_mib"] and gpu_delta<=limits["share_stop_wddm_delta_mib"],dict(private_delta_mib=private_delta,wddm_delta_mib=gpu_delta))
            release={k:statistics.median(r[k] for r in released) for k in ("private_bytes","handles","threads")}
            gpu_release=[r["gpu_dedicated_bytes"]+r["gpu_shared_bytes"] for r in released
                         if r["gpu_dedicated_bytes"] is not None and r["gpu_shared_bytes"] is not None]
            check("release_wddm_coverage", len(gpu_release)==len(released))
            release["wddm_bytes"]=statistics.median(gpu_release) if gpu_release else None
            release_details.append(release)
        else: check("share_stop_resources",False,"comparable_settled_windows_missing")
        item=dict(cycle=cycle,cycle_id=a["join","uia_observed"]["cycle_id"],anonymous_session_id=sid,
                  verdict="PASS" if all(x=="PASS" or (name in outbound and x=="DEFERRED") for name,x in checks.items()) else "FAIL",checks=checks,details=details)
        report["cycles"].append(item)
        (output/"external-review-progress.json").write_text(json.dumps(dict(run_id=run,cycles_reviewed=cycle,last_verdict=item["verdict"])))
    exit_evidence=read(root/"uia/process-exit.json")
    crashes=read(root/"windows-crash-event.json")
    duration=(datetime.fromisoformat(result["finished_utc"])-datetime.fromisoformat(result["started_utc"])).total_seconds()
    report["duration_seconds"]=duration
    report["cycle_counts"]=dict(Counter(c["verdict"] for c in report["cycles"]))
    if frozen_gpu and all(c["checks"].get("gpu_queue_coverage")=="PASS" for c in report["cycles"]):
        report["deferred"].remove("gpu_queue")
    report["diagnostic_events"]=len(diagnostics)
    report["diagnostic_sequence_gaps"]=0
    report["exit_code"]=exit_evidence["exit_code"]
    report["windows_crash_events"]=crashes["count"]
    report["resource_growth"]={}
    if formal and len(release_details)==100:
        for field,limit in (("private_bytes",limits["private_growth_mib"]*1024**2),("wddm_bytes",limits["wddm_gpu_growth_mib"]*1024**2),("handles",limits["handles_growth"]),("threads",limits["threads_growth"])):
            available=all(r[field] is not None for r in release_details)
            growth=statistics.median(r[field] for r in release_details[-10:])-statistics.median(r[field] for r in release_details[:10]) if available else None
            report["resource_growth"][field]=dict(growth=growth,limit=limit,passed=available and growth<=limit)
    final_checks={}
    load_ready, load_stopped = [], []
    for name,file in (("audio","product-audio.jsonl"),("remote","remote.jsonl")):
        last=None
        for last in records(root/file):
            if name == "remote" and last.get("event") == "load.ready":
                load_ready.append(last)
            if name == "remote" and last.get("event") == "load.stopped":
                load_stopped.append(last)
        final_checks[name+"_complete"]=bool(last) and last.get("event")=="collector.stopped" and last.get("status")=="COMPLETE"
    final_checks["isolated_load_lifecycle_complete"] = review_lifecycle(load_ready, load_stopped, run)
    last=None
    for last in records(root/"external-resources.jsonl"): pass
    final_checks["process_released"]=bool(last) and last.get("process_alive") is False
    watcher=read(root/"diagnostic-watcher-result.json")
    final_checks["diagnostic_watcher_complete"]=watcher["run_id"]==run and watcher["status"]=="COMPLETE" and watcher["events"]==len(diagnostics)
    final_checks["diagnostic_privacy_schema"]=watcher.get("privacy_schema_violations")==0
    route=read(root/"server-route.json")
    if 'collector_scheduler_policy' in plan:
        scheduler_review=review_scheduler_policy(plan, records(root/'remote.jsonl'), route)
        report['collector_scheduler_policy']=scheduler_review
        final_checks['collector_scheduler_policy_complete']=scheduler_review['passed']
    final_checks["collector_local_route_complete"]=(route["run_id"]==run and
        route["status"]=="COMPLETE" and route["cleanup_complete"] is True and
        route["child_exit_code"]==0 and route["matched_connections"]>0 and
        route["private_route_device"]=="lo" and
        route["server_config_sha256"]==plan["collector_media_route"]["server_config_sha256"])
    exits=read(root/"collector-exits.json")
    final_checks["collectors_exited_normally"]=collector_exits_complete(plan,read(root/"collectors.json"),exits)
    witness=read(root/"diagnostic-sequences.json")
    try:
        diagnostic_terminal=review_diagnostic_terminal(root,identity,run,diagnostics,witness,watcher)
        final_checks["diagnostic_final_matches_continuous"]=diagnostic_terminal["passed"]
        report["diagnostic_terminal_witness"]=diagnostic_terminal
    except (OSError,KeyError,TypeError,ValueError) as error:
        final_checks["diagnostic_final_matches_continuous"]=False
        report["diagnostic_terminal_witness"]=dict(passed=False,reason=str(error))
    if formal:
        product_lifetime=product_lifetime_floor(result,identity,run,actions)
        report["product_lifetime_floor"]=product_lifetime
        final_checks["product_lifetime_floor"]=product_lifetime["passed"]
    report["final_checks"]=final_checks
    successful=result["cycles_completed"]==count and result["verdict"]==("UIA_COMPLETE" if formal else "PILOT_COMPLETE") and report["cycle_counts"].get("PASS")==count and exit_evidence["exit_code"]==0 and crashes["count"]==0 and all(r["passed"] for r in report["resource_growth"].values()) and all(final_checks.values()) and (not formal or len(release_details)==100)
    report["verdict"]="PASS_WITH_DEFERRED" if successful else "FAIL"
    report["reviewed_utc"]=datetime.now(timezone.utc).isoformat()
    (output/"external-review.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(dict(verdict=report["verdict"],cycles=report["cycle_counts"])))
    return 0 if successful else 1


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--root",type=Path,required=True)
    p.add_argument("--review-output",type=Path,help="New derivative review directory; original evidence and FAIL remain unchanged")
    args=p.parse_args()
    output=args.review_output or args.root
    if args.review_output: output.mkdir(parents=True,exist_ok=False)
    try: raise SystemExit(review(args.root,output))
    except (OSError,KeyError,ValueError,TypeError,StopIteration) as error:
        (output/"external-review-error.json").write_text(json.dumps(dict(verdict="FAIL",error_type=type(error).__name__,reason=str(error))))
        interrupted_review(args.root,error,output)
        raise SystemExit(1)
