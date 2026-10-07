"""Review diagnostic callback timing; never emits a formal/PILOT acceptance."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import statistics
from product_pilot_audio_reference import reference_evidence
from product_pilot_scheduler import validate_realtime_restriction


def records(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]


def maximum(rows, field):
    return max((r[field] for r in rows if r.get(field) is not None), default=None)


def scheduling_details(delivered):
    return [dict(monotonic_s=r["monotonic_s"], stream=r["stream"],
                 ffi_gap_ms=r.get("ffi_gap_ms"), python_gap_ms=r.get("python_gap_ms"),
                 **r["ffi_thread_scheduling"])
            for r in delivered if isinstance(r.get("ffi_thread_scheduling"), dict)
            and max(r.get("ffi_gap_ms") or 0, r.get("python_gap_ms") or 0) > 200]


def measured_wait_distribution(audio):
    values=sorted(r["ffi_thread_scheduling"]["runnable_wait_delta_ms"] for r in audio
        if r.get("ffi_thread_scheduling", {}).get("status")=="MEASURED"
        and r["ffi_thread_scheduling"].get("runnable_wait_delta_ms") is not None)
    return dict(samples=len(values), **{name:values[min(len(values)-1, max(0, int(len(values)*p)-1))]
        if values else None for name,p in (("p50_ms",.5),("p95_ms",.95),("p99_ms",.99),("max_ms",1))})


def task_cpu_summary(samples):
    available = {r['sampled_at_s']:r for r in samples if r.get('available') and r.get('sampled_at_s') is not None}
    rows = [available[k] for k in sorted(available)]
    measured, thread_totals, resets = [], {}, 0
    for a,b in zip(rows,rows[1:]):
        span = b['sampled_at_s']-a['sampled_at_s']
        delta = b['cpu_user_s']+b['cpu_system_s']-a['cpu_user_s']-a['cpu_system_s']
        if b['thread_start_ticks'] != a['thread_start_ticks'] or delta < 0:
            resets += 1
            continue
        threads = []
        for tid, now in b.get('thread_cpu',{}).items():
            previous = a.get('thread_cpu',{}).get(tid)
            if not previous or not previous.get('available') or not now.get('available'):
                continue
            if now['thread_start_ticks'] != previous['thread_start_ticks']:
                continue
            elapsed = now['sampled_at_s']-previous['sampled_at_s']
            cpu = now['cpu_user_s']+now['cpu_system_s']-previous['cpu_user_s']-previous['cpu_system_s']
            if elapsed <= 0 or cpu < 0:
                continue
            identity = (tid,now['thread_start_ticks'])
            thread_totals[identity] = thread_totals.get(identity,0)+cpu
            threads.append(dict(tid=int(tid),start_ticks=now['thread_start_ticks'],
                cpu_percent_of_one_core=100*cpu/elapsed,nice=now['nice'],policy=now['policy']))
        measured.append(dict(sampled_at_s=b['sampled_at_s'],interval_ms=span*1000,
            cpu_percent_of_one_core=100*delta/span,
            top_threads=sorted(threads,key=lambda r:r['cpu_percent_of_one_core'],reverse=True)[:12]))
    return dict(verdict='CPU_OBSERVATION_COMPLETE' if measured and not resets else 'CPU_OBSERVATION_INCOMPLETE',
        available_samples=len(rows),unavailable_samples=sum(not r.get('available') for r in samples),counter_resets=resets,
        peak_cpu_percent_of_one_core=max((r['cpu_percent_of_one_core'] for r in measured),default=None),
        maximum_probe_collection_ms=max((r.get('collection_ms') or r.get('probe_ms') or 0 for r in rows),default=None),
        maximum_threads=max((r.get('threads',0) for r in rows),default=None),
        hottest_interval=max(measured,key=lambda r:r['cpu_percent_of_one_core'],default=None),
        top_observed_thread_cpu_seconds=[dict(tid=int(tid),start_ticks=start,cpu_seconds=cpu)
            for (tid,start),cpu in sorted(thread_totals.items(),key=lambda item:item[1],reverse=True)[:12]],
        intervals=measured,
        scope='Task process includes observer; same TID/start-time successive snapshots only, newly born/exited or unavailable threads unmeasured; no shared-service attribution')


def realtime_restriction_review(remote,cpu):
    receiver=[r for r in remote if r['event']=='receiver.realtime_restriction']
    publisher=[r['publisher_process'] for r in remote if r['event']=='load.ready']
    proof_complete=False
    try:
        if len(receiver)==len(publisher)==1:
            validate_realtime_restriction(receiver[0]['state'])
            validate_realtime_restriction(publisher[0].get('publisher_realtime_restriction'))
            proof_complete=receiver[0].get('release_eligible') is False
    except ValueError:
        pass
    threads=[s for r in cpu for s in r.get('proc_sampler',{}).get('task_cpu',{}).get('thread_cpu',{}).values()]
    available=[s for s in threads if s.get('available')]
    policies=dict(Counter(str(s['policy']) for s in available))
    complete=proof_complete and bool(available) and all(s['policy'] in (0,3,5) for s in available)
    return dict(verdict='REALTIME_RESTRICTION_EVIDENCE_COMPLETE' if complete else 'REALTIME_RESTRICTION_EVIDENCE_INCOMPLETE',
        receiver=receiver[0]['state'] if len(receiver)==1 else None,
        publisher=publisher[0].get('publisher_realtime_restriction') if len(publisher)==1 else None,
        observed_thread_policy_counts=policies,unavailable_thread_samples=len(threads)-len(available),
        diagnostic_only=True,release_eligible=False,
        scope='Before-SDK capability/no-new-privs/hard-RTPRIO readback and sampled task-native policies; born/exited thread snapshots may be partial, shared processes unmodified')


def analyze(root):
    plan=json.loads((root/"plan.json").read_text(encoding="utf-8-sig"))
    timing=records(root/"timing.jsonl")
    remote=records(root/"remote.jsonl")
    run=plan["run_id"]
    if not plan.get("diagnostic_only") or plan.get("release_eligible") is not False:
        raise ValueError("timing_review_requires_diagnostic_run")
    complete=(bool(timing) and timing[0]["event"]=="timing.started" and
        timing[-1]["event"]=="timing.stopped" and
        all(r["run_id"]==run and r["sequence"]==i for i,r in enumerate(timing,1)) and
        all(r.get("lost",0)==0 for r in timing))
    if timing and timing[0].get('scheduling_probe') == 'worker_cached_proc_tid_v2':
        complete = complete and timing[-1].get('proc_sampler_closed') is True
    reference = reference_evidence(remote, timing) if plan.get('diagnostic_audio_reference', {}).get('required') else None
    reference_handles = set(reference['ffi_stream_handles']) if reference else set()
    audio=[r for r in timing if r["event"]=="audio_delivery" and r['stream'] not in reference_handles]
    if reference is not None:
        complete = complete and reference['verdict'] == 'REFERENCE_EVIDENCE_COMPLETE'
    cpu=[r for r in timing if r["event"]=="timing.sample"]
    realtime_review = realtime_restriction_review(remote,cpu) if plan.get('diagnostic_realtime_restriction',{}).get('required') else None
    if realtime_review is not None:
        complete=complete and realtime_review['verdict']=='REALTIME_RESTRICTION_EVIDENCE_COMPLETE'
    task_cpu = task_cpu_summary([r['proc_sampler']['task_cpu'] for r in cpu if r.get('proc_sampler')])
    cadence_path = root/'publisher-cadence.jsonl'
    cadence = records(cadence_path) if cadence_path.exists() else []
    publisher_cpu = task_cpu_summary([r['process_cpu'] for r in cadence if r.get('process_cpu')])
    cpu_pct=[100*((b["cpu_user_s"]+b["cpu_system_s"])-(a["cpu_user_s"]+a["cpu_system_s"]))/
        (b["monotonic_s"]-a["monotonic_s"]) for a,b in zip(cpu,cpu[1:]) if b["monotonic_s"]>a["monotonic_s"]]
    intervals=[]
    prior={}
    for row in remote:
        if row["event"]!="receiver.sample" or row.get("kind")!="audio":continue
        sid=row["sid"]
        start=prior.get(sid,row["monotonic_s"]-1.3)
        prior[sid]=row["monotonic_s"]
        if row.get("window_max_gap_ms",0)<=200 and (row.get("silence_ms") or 0)<=200:continue
        # Same remote process monotonic clock; do not infer an FFI handle/SID map.
        nearby=[r for r in timing if start-.1<=r["monotonic_s"]<=row["monotonic_s"]+.1]
        delivered=[r for r in nearby if r["event"]=="audio_delivery" and r['stream'] not in reference_handles]
        slow=[r for r in nearby if r["event"]=="video_copy_slow"]
        intervals.append(dict(utc=row["utc"],cycle=(row.get("observer_context") or {}).get("cycle"),
            pcm_consumer_gap_ms=row["window_max_gap_ms"],silence_ms=row["silence_ms"],
            ffi_stream_handles=sorted({r["stream"] for r in delivered}),
            first_python_ffi_entry_gap_ms=maximum(delivered,"ffi_gap_ms"),
            ffi_to_pcm_construction_wait_ms=maximum(delivered,"ffi_to_python_ms"),
            pcm_construction_gap_ms=maximum(delivered,"python_gap_ms"),
            ffi_thread_scheduling=scheduling_details(delivered),
            event_loop_lag_ms=maximum(nearby,"lag_ms"),
            slow_video_copies=len(slow),slow_video_copy_sum_ms=sum(r["duration_ms"] for r in slow),
            slow_ffi_operations=[dict(operation=r["operation"],duration_ms=r["duration_ms"])
                for r in nearby if r["event"]=="ffi_request_slow"],
            slow_ffi_handle_releases=[dict(duration_ms=r["duration_ms"],
                failed=r["failed"],event_loop_thread=r["event_loop_thread"])
                for r in nearby if r["event"]=="ffi_handle_dispose_slow"]))
    report=dict(schema=1,run_id=run,diagnostic_only=True,release_eligible=False,
        verdict="TIMING_EVIDENCE_COMPLETE" if complete and audio else "TIMING_EVIDENCE_INCOMPLETE",
        timing_events=len(timing),event_counts=dict(Counter(r["event"] for r in timing)),
        scope="Python FFI entry, construction and event-loop timing; arrival before Python/GIL and audible quality unmeasured",
        ffi_entry_gap_ms=maximum(audio,"ffi_gap_ms"),ffi_to_python_wait_ms=maximum(audio,"ffi_to_python_ms"),
        python_construction_gap_ms=maximum(audio,"python_gap_ms"),pcm_copy_ms=maximum(audio,"copy_ms"),
        loop_lag_ms=maximum(timing,"lag_ms"),video_copy_ms=maximum([r for r in timing if r["event"]=="video_copy_slow"],"duration_ms"),
        ffi_handle_dispose_ms=maximum([r for r in timing if r["event"]=="ffi_handle_dispose_slow"],"duration_ms"),
        video_formats=[{k:r[k] for k in ("width","height","format","python_buffer_bytes")} for r in timing if r["event"]=="video_format"],
        decoded_counter_formats=[dict(width=w,height=h,format=f) for w,h,f in sorted({
            (r["video_counter"]["width"],r["video_counter"]["height"],r["video_counter"]["format"])
            for r in remote if isinstance(r.get("video_counter"),dict) and r["video_counter"].get("width") is not None})],
        collector_cpu_percent_of_one_core=dict(peak=max(cpu_pct,default=None),mean=statistics.mean(cpu_pct) if cpu_pct else None),
        scheduling=dict(scope="Observed FFI callback Linux TID/start-time runnable-wait counters; not all decoder threads, blocked time, or pre-GIL native production",
            probe=timing[0].get('scheduling_probe','LEGACY_NOT_RECORDED'),
            interval_scope=timing[0].get('scheduling_scope','Legacy synchronous samples'),
            status_counts=dict(Counter(r.get("ffi_thread_scheduling", {}).get("status", "NOT_COLLECTED") for r in audio)),
            callback_nice_counts=dict(Counter(str(r.get("ffi_thread_scheduling", {}).get("nice", "UNKNOWN")) for r in audio)),
            callback_policy_counts=dict(Counter(str(r.get("ffi_thread_scheduling", {}).get("policy", "UNKNOWN")) for r in audio)),
            measured_runnable_wait=measured_wait_distribution(audio),
            maximum_probe_ms=max((r["ffi_thread_scheduling"]["probe_ms"] for r in audio if "ffi_thread_scheduling" in r), default=None),
            maximum_background_read_ms=max((r.get('proc_sampler',{}).get('maximum_background_read_ms',0) for r in cpu),default=None),
            maximum_cached_age_ms=max((r['ffi_thread_scheduling'].get('sample_age_ms',0) for r in audio if r.get('ffi_thread_scheduling')),default=None),
            long_ffi_or_python_gaps=scheduling_details(audio),
            cpu_pressure_scope="whole server; cannot attribute pressure to one process",
            maximum_cpu_pressure_avg10=max((r["cpu_pressure"]["avg10"] for r in cpu if r.get("cpu_pressure", {}).get("available")), default=None)),
        pcm_fail_intervals=intervals,
        audio_reference=reference,
        receiver_task_cpu=task_cpu,publisher_task_cpu=publisher_cpu,
        realtime_restriction=realtime_review,
        evidence_sha256={name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in ("plan.json","timing.jsonl","remote.jsonl")},
        analyzer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        reference_analyzer_sha256=hashlib.sha256(Path(__file__).with_name('product_pilot_audio_reference.py').read_bytes()).hexdigest())
    (root/"timing-analysis.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(report,indent=2))
    return 0 if complete and audio else 1


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,required=True)
    raise SystemExit(analyze(parser.parse_args().root))
