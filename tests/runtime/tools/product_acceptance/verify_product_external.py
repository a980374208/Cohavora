"""Streaming review of the fixed-load product run; never substitutes UI for media.

The original strict DXGI-budget verifier remains available. This review names
the actually collected WDDM counters and explicitly defers unobserved budgets,
GPU queues, physical microphone capture and WGC handle ownership.
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics
from datetime import datetime, timezone

from product_pilot_performance import paired_window
from product_pilot_correlation import correlate
from product_pilot_audio import review_outbound_audio
from product_pilot_archive import read_segment
from product_pilot_load import review_lifecycle
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


def interrupted_review(root, error):
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
        reason=reason,review_error=dict(type=type(error).__name__,reason=str(error)),
        cycles=cycles,cycle_counts=dict(Counter(c["verdict"] for c in cycles)),
        cycles_requested=plan["cycles"],cycles_not_run=plan["cycles"]-len(cycles),
        reviewed_utc=datetime.now(timezone.utc).isoformat())
    (root/"external-review.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(dict(verdict="FAIL",reason=reason,cycles=report["cycle_counts"])))


def review(root):
    plan, result, limits = read(root/"plan.json"), read(root/"uia/uia-result.json"), read(root/"limits.json")
    run = plan["run_id"]
    identity = read(root/"uia/product-identity.json")
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
        started_utc=result["started_utc"], finished_utc=result["finished_utc"],
        deferred=["dxgi_local_nonlocal_budget", "gpu_queue", "wgc_handle_ownership",
                  "slow_disk", "power_loss", "real_crash_recovery", "release_symbol_review",
                  "audio_pixel_quality", "physical_multimonitor_coordinates"])
    devices = read(root/"audio-devices.json")
    device_path = root/"uia/uia-device-outcomes.jsonl"
    device_outcomes = list(records(device_path)) if device_path.exists() else []
    if devices["active_capture_endpoints"] == 0:
        report["deferred"].append("outbound_microphone_no_active_capture_endpoint")
    release_details = []
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
        terminal=[p for p in probe if p.get("session_complete") and timestamp(p)>=timestamp(a["leave","uia_observed"])]
        check("native_cleanup_released", bool(terminal) and terminal[-1]["native_cleanup_pending"]==0)
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
        (root/"external-review-progress.json").write_text(json.dumps(dict(run_id=run,cycles_reviewed=cycle,last_verdict=item["verdict"])))
    exit_evidence=read(root/"uia/process-exit.json")
    crashes=read(root/"windows-crash-event.json")
    duration=(datetime.fromisoformat(result["finished_utc"])-datetime.fromisoformat(result["started_utc"])).total_seconds()
    report["duration_seconds"]=duration
    report["cycle_counts"]=dict(Counter(c["verdict"] for c in report["cycles"]))
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
    final_checks["collector_local_route_complete"]=(route["run_id"]==run and
        route["status"]=="COMPLETE" and route["cleanup_complete"] is True and
        route["child_exit_code"]==0 and route["matched_connections"]>0 and
        route["private_route_device"]=="lo" and
        route["server_config_sha256"]==plan["collector_media_route"]["server_config_sha256"])
    exits=read(root/"collector-exits.json")
    final_checks["collectors_exited_normally"]=collector_exits_complete(plan,read(root/"collectors.json"),exits)
    witness=read(root/"diagnostic-sequences.json")
    final_checks["diagnostic_final_matches_continuous"]=witness["run_id"]==run and [i for s in witness["segments"] for i in s["sequences"]]==[d["event_sequence"] for d in diagnostics]
    if formal:
        product_lifetime=product_lifetime_floor(result,identity,run,actions)
        report["product_lifetime_floor"]=product_lifetime
        final_checks["product_lifetime_floor"]=product_lifetime["passed"]
    report["final_checks"]=final_checks
    successful=result["cycles_completed"]==count and result["verdict"]==("UIA_COMPLETE" if formal else "PILOT_COMPLETE") and report["cycle_counts"].get("PASS")==count and exit_evidence["exit_code"]==0 and crashes["count"]==0 and all(r["passed"] for r in report["resource_growth"].values()) and all(final_checks.values()) and (not formal or len(release_details)==100)
    report["verdict"]="PASS_WITH_DEFERRED" if successful else "FAIL"
    report["reviewed_utc"]=datetime.now(timezone.utc).isoformat()
    (root/"external-review.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(dict(verdict=report["verdict"],cycles=report["cycle_counts"])))
    return 0 if successful else 1


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--root",type=Path,required=True)
    args=p.parse_args()
    try: raise SystemExit(review(args.root))
    except (OSError,KeyError,ValueError,TypeError,StopIteration) as error:
        (args.root/"external-review-error.json").write_text(json.dumps(dict(verdict="FAIL",error_type=type(error).__name__,reason=str(error))))
        interrupted_review(args.root,error)
        raise SystemExit(1)
