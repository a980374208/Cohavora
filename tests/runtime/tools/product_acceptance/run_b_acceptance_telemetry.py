"""B15 native callback timing, actual product UI, stopped-sampling baseline.

No UIA polling occurs during paired or long measurements. Three independent
pairs alternate order; native media and process counters verify the load.
"""
from __future__ import annotations
import argparse
from datetime import datetime,timezone
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time
import uuid
from run_b_acceptance_network import NetworkPeers
from run_b_acceptance_files import events,sha
from run_b_acceptance_recovery import snapshot
import invoke_screen_share_quality_probe as service


def samples(root):
    return [json.loads(x) for x in (root/'telemetry-samples.jsonl').read_text(encoding='utf-8').splitlines() if x.strip()]


class TelemetryPeers(NetworkPeers):
    def __init__(self,root,binary,room,auth,mode,seconds):
        super().__init__(root,binary,room,auth)
        self.mode,self.seconds=mode,seconds

    def launch(self,role):
        names=('B_TELEMETRY_MODE','B_TELEMETRY_MAX_SECONDS')
        previous={k:os.environ.get(k) for k in names}
        try:
            for key in names:os.environ.pop(key,None)
            os.environ['B_TELEMETRY_MAX_SECONDS']=str(min(10800,self.seconds+400))
            if role=='receiver':os.environ['B_TELEMETRY_MODE']='2' if self.mode=='off' else '1'
            super().launch(role)
        finally:
            for key,value in previous.items():
                if value is None:os.environ.pop(key,None)
                else:os.environ[key]=value
        if role=='receiver' and self.mode=='off':
            self.send(role,'telemetry_stop_sampling')
            self.wait(role,lambda rows:next((x for x in rows if x['event']=='telemetry_sampling_stopped'
                and x['actual_stop_on_strand']),None),'actual_stop_on_strand')

    def observe_performance(self):
        sequence=self.send('receiver','telemetry_observe')
        return self.wait('receiver',lambda rows:next((x for x in rows if x['event']=='telemetry_observation'
            and x['sequence']==sequence),None),'performance_observation')

    def wait_duration(self,seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if any(child.poll() is not None for child in self.processes.values()):
                raise RuntimeError('product_peer_exited_during_measurement')
            if any(x['event'] in ('telemetry_command_failed','coordinator_error','fixture_timeout')
                for x in events(self.roots['receiver'])):raise RuntimeError('native_telemetry_failure')
            time.sleep(min(1,max(0,end-time.monotonic())))


def percentile(timing):
    count=timing['samples']
    if count<500:raise RuntimeError('callback_p99_requires_500_actual_frames')
    rank=math.ceil(count*.99);seen=0
    for bucket,value in sorted(((int(k),v) for k,v in timing['histogram'].items())):
        seen+=value
        if seen>=rank:
            if bucket==0 or bucket==timing['overflow_bucket']:raise RuntimeError('callback_p99_quantile_unbounded')
            return dict(samples=count,lower_ns=bucket*timing['bucket_ns'],upper_ns=(bucket+1)*timing['bucket_ns'])
    raise RuntimeError('callback_histogram_inconsistent')


def summarize(root,mode):
    rows=[x for x in samples(root) if x['phase']=='measured']
    if len(rows)<30:raise RuntimeError('insufficient_paired_native_resource_samples')
    duration=(rows[-1]['utc_ms']-rows[0]['utc_ms'])/1000
    if duration<30 or any(not x['memory_available'] or not x['cpu_available'] or not x['logical_processors'] for x in rows):
        raise RuntimeError('native_resource_counter_coverage')
    if mode=='off' and any(not x['sampling_stopped'] or not x['diagnostic']['production_paused'] or
        x['diagnostic']['retention_enabled'] or not x['stopped_native_terminal'] or x['stopped_stats_in_flight'] for x in rows):
        raise RuntimeError('baseline_sampling_or_log_production_not_actually_stopped')
    if mode=='off' and len({x['snapshot']['revision'] for x in rows})!=1:
        raise RuntimeError('baseline_cached_snapshot_still_advancing')
    if mode=='on' and (rows[-1]['snapshot']['revision']<=rows[0]['snapshot']['revision'] or
        any(x['snapshot']['session_complete'] or x['diagnostic']['production_paused'] for x in rows)):
        raise RuntimeError('feature_telemetry_not_advancing')
    for row in rows:
        h=row['history'];c=row['console']
        if h['memory_bytes']>8*1024**2 or h['queue_bytes']>h['queue_byte_capacity'] or h['queue_depth']>h['queue_capacity']:
            raise RuntimeError('history_memory_or_queue_bound_exceeded')
        if h['memory_bytes']+h['queue_bytes']+h['pending_bytes']>32*1024**2:
            raise RuntimeError('telemetry_32MiB_budget_exceeded')
        if c['cache_bytes']>c['cache_limit'] or c['entries']>c['entry_limit'] or c['pending_bytes']>c['pending_limit']:
            raise RuntimeError('actual_UI_cache_or_queue_bound_exceeded')
        if h['write_failures'] or row['diagnostic']['sink_failures']:
            raise RuntimeError('persistence_failed_during_measurement')
    return dict(samples=len(rows),duration_s=duration,
        cpu_pct=100*(rows[-1]['cpu_seconds']-rows[0]['cpu_seconds'])/duration/rows[0]['logical_processors'],
        working_set_median=statistics.median(x['working_set_bytes'] for x in rows),
        private_median=statistics.median(x['private_bytes'] for x in rows),
        telemetry_memory_peak=max(x['history']['memory_bytes']+x['history']['queue_bytes']+x['history']['pending_bytes'] for x in rows),
        callback_frames=rows[-1]['callback_samples']-rows[0]['callback_samples'],
        measured_start_ms=rows[0]['utc_ms'],measured_end_ms=rows[-1]['utc_ms'])


def run_window(root,binary,mode,seconds):
    root.mkdir();room='b15-'+uuid.uuid4().hex
    result=dict(status='RUNNING',mode=mode,room=room,requested_seconds=seconds)
    manifest=root/'result.json'
    def save():manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    save();peers=None
    try:
        result['server_before']=snapshot(room)
        peers=TelemetryPeers(root,binary,room,service.credentials(room),mode,seconds)
        peers.launch('receiver')
        result['prejoin_resources']=samples(peers.roots['receiver'])[0]
        peers.launch('publisher')
        result['media_before']=peers.advancing_media()
        peers.wait_duration(10)
        peers.send('receiver','telemetry_open')
        peers.wait_duration(20)
        peers.send('receiver','telemetry_begin',phase='measured')
        result['start']=peers.observe_performance()
        print('B15 '+mode+' measured '+str(seconds)+'s',flush=True)
        peers.wait_duration(seconds)
        result['end']=peers.observe_performance()
        result['media_after']=peers.advancing_media(result['media_before']['decoded_frames'])
        result['summary']=summarize(peers.roots['receiver'],mode)
        result['callback_p99']=percentile(result['end']['callback_timing'])
        ui=result['end']['ui_batch_timing'];refresh=result['end']['ui_refresh_timing']
        elapsed=(result['end']['utc_ms']-result['start']['utc_ms'])/1000
        if ui['maximum_ns']>4_000_000:raise RuntimeError('actual_log_UI_batch_exceeded_4ms')
        if ui['samples']>elapsed*20+1:raise RuntimeError('actual_log_UI_refresh_exceeded_20Hz')
        if refresh['samples']<seconds*.8 or refresh['samples']>elapsed+1:
            raise RuntimeError('actual_telemetry_refresh_not_1Hz')
        peers.send('receiver','telemetry_begin',phase='settled')
        peers.send('receiver','telemetry_close')
        peers.send('receiver','leave')
        peers.wait('receiver',lambda rows:next((x for x in rows if x['event']=='left'),None),'receiver_left')
        peers.wait_duration(15)
        result['released']=peers.observe_performance()
        if seconds>=9000:
            result['long_resource_gate']=long_resource_gate(peers.roots['receiver'],result)
            if result['long_resource_gate']['status']!='PASS':raise RuntimeError('original_long_resource_gate_failed')
        peers.send('receiver','telemetry_finalize')
        closed=peers.wait('receiver',lambda rows:next((x for x in rows if x['event']=='telemetry_store_closed'),None),'persistence_close')
        result['persistence_close']=closed
        if not closed['history_completed'] or closed['disk_unknown'] or not closed['diagnostic_completed']:
            raise RuntimeError('telemetry_persistence_drain_failed')
        peers.finish('receiver',already_left=True);peers.finish('publisher')
        result['lifecycle']=peers.lifecycle
        result['server_after']=snapshot(room)
        if result['server_after']['participants_remaining'] or result['server_after']['publications_remaining']:
            raise RuntimeError('telemetry_room_not_empty')
        if any(result['server_before'][k]!=result['server_after'][k] for k in ('config_sha256','image','binary')):
            raise RuntimeError('telemetry_service_fingerprint_changed')
        result['status']='PASS'
    except Exception as error:result.update(status='FAIL',failure=type(error).__name__+':'+str(error))
    finally:
        if peers:peers.cleanup()
        result['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    return result


def paired(off,on):
    a,b=off['summary'],on['summary']
    increase=b['cpu_pct']-a['cpu_pct']
    memory=b['working_set_median']-a['working_set_median']
    p99=100*(on['callback_p99']['upper_ns']/off['callback_p99']['lower_ns']-1)
    fps_a=a['callback_frames']/a['duration_s'];fps_b=b['callback_frames']/b['duration_s']
    verdict=dict(status='PASS',cpu_increase_pp=increase,working_set_increase_bytes=memory,
        callback_p99_increase_upper_percent=p99,actual_callback_fps=dict(off=fps_a,on=fps_b),
        limits=dict(cpu_increase_pp=1,working_set_increase_bytes=32*1024**2,callback_p99_increase_percent=5,
            UI_batch_ms=4,log_refresh_hz=20,telemetry_refresh_hz=1))
    if increase>1 or memory>32*1024**2 or p99>5:verdict['status']='FAIL'
    if min(fps_a,fps_b)<10 or abs(fps_a-fps_b)/max(fps_a,fps_b)>.1:
        verdict.update(status='FAIL',failure='paired_actual_media_load_mismatch')
    return verdict


def long_resource_gate(root,result):
    rows=[x for x in samples(root) if x['phase']=='measured']
    start=rows[0]['utc_ms'];duration=(rows[-1]['utc_ms']-start)/1000
    settled=[x for x in rows if x['utc_ms']>=start+300000]
    if duration<9000 or len(rows)<1500 or not settled:
        raise RuntimeError('original_9000s_long_resource_coverage_not_met')
    gap=max((b['utc_ms']-a['utc_ms'])/1000 for a,b in zip(rows,rows[1:]))
    buckets={}
    for row in settled:buckets.setdefault(int((row['utc_ms']-start)/60000),[]).append(row)
    points=[(statistics.median(x['utc_ms'] for x in items)/1000,
        statistics.median(x['private_bytes'] for x in items)/1024**2) for items in buckets.values()]
    mx=statistics.mean(x for x,y in points);my=statistics.mean(y for x,y in points)
    slope=3600*sum((x-mx)*(y-my) for x,y in points)/sum((x-mx)**2 for x,y in points)
    growth=points[-1][1]-points[0][1]
    handles=statistics.median(x['handles'] for x in settled[-60:])-statistics.median(x['handles'] for x in settled[:60])
    threads=statistics.median(x['threads'] for x in settled[-60:])-statistics.median(x['threads'] for x in settled[:60])
    release=(result['released']['private_bytes']-result['prejoin_resources']['private_bytes'])/1024**2
    observation=dict(status='PASS',duration_s=duration,samples=len(rows),maximum_gap_s=gap,warmup_s=300,
        private_growth_mib=growth,slope_mib_per_hour=slope,handles_growth=handles,threads_growth=threads,
        release_private_delta_mib=release,
        limits=dict(duration_s=9000,minimum_samples=1500,maximum_gap_s=30,private_growth_mib=64,
            slope_mib_per_hour=64,handles_growth=64,threads_growth=8,release_private_delta_mib=64))
    if gap>30 or growth>64 or slope>64 or handles>64 or threads>8 or release>64:
        observation['status']='FAIL'
    return observation


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--phase',choices=('paired','soak'),default='paired')
    parser.add_argument('--seconds',type=int)
    parser.add_argument('--pairs',type=int,default=3)
    args=parser.parse_args();args.output=args.output.resolve();binary=args.binary.resolve()
    seconds=args.seconds or (9000 if args.phase=='soak' else 60)
    if seconds<40 or seconds>10000 or not 1<=args.pairs<=3:parser.error('invalid sampling duration/pair count')
    if args.phase=='soak' and seconds<9000:parser.error('the original long gate requires >=9000 seconds')
    args.output.mkdir(parents=True,exist_ok=False)
    service.SERVER_PORT=17980;service.SERVER_CONTAINER='cohavora-b-acceptance-20261001'
    service.CONFIG_PATH='/root/livekit-b-acceptance-20261001/livekit.yaml'
    verifier=Path(__file__).resolve().parents[1]/'diagnostics/verify_runtime_binary.ps1'
    verified=subprocess.run(['pwsh','-NoProfile','-File',str(verifier),'-Executable',str(binary),
        '-ExpectedExecutableName','uia_entry_fixture.exe','-Configuration','RelWithDebInfo'],capture_output=True,text=True)
    if verified.returncode:parser.exit(2,'RelWithDebInfo verification failed\n')
    inputs=['src/core/room.cpp','src/core/room.h','src/render/owned_i420_frame.cpp','src/render/owned_i420_frame.h',
        'src/core/meeting_coordinator.cpp','src/core/meeting_coordinator.h','src/telemetry/session_telemetry.cpp',
        'src/telemetry/telemetry_report.cpp','src/ui/meeting_log_console.cpp',
        'src/ui/telemetry_panel_controller.h','src/ui/telemetry_panel_controller.cpp',
        'tests/runtime/probes/b_file_product_runtime.h','tests/runtime/probes/b_network_product_runtime.h',
        'tests/runtime/probes/b_telemetry_product_runtime.h',str(Path(__file__))]
    result=dict(status='RUNNING',phase=args.phase,configuration='RelWithDebInfo',binary_identity=json.loads(verified.stdout),
        diagnostic_stage_timing=os.environ.get('B_TELEMETRY_STAGE_TIMING')=='1',
        head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        inputs={p:sha(Path(p)) for p in inputs},windows=[],pairs=[])
    manifest=args.output/'index.json'
    def save():manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    save()
    if args.phase=='paired':
        for pair in range(args.pairs):
            values={}
            for mode in (('off','on') if pair%2==0 else ('on','off')):
                name=str(pair+1)+'-'+mode
                value=run_window(args.output/name,binary,mode,seconds)
                result['windows'].append(dict(name=name,status=value['status'],evidence=name+'/result.json'))
                values[mode]=value;print(name+' '+value['status'],flush=True);save()
                if value['status']!='PASS':break
            if len(values)!=2 or any(x['status']!='PASS' for x in values.values()):break
            verdict=paired(values['off'],values['on']);result['pairs'].append(verdict);save()
            print('pair '+str(pair+1)+' '+verdict['status'],flush=True)
            if verdict['status']!='PASS':break
        complete=len(result['pairs'])==args.pairs and all(x['status']=='PASS' for x in result['pairs'])
    else:
        value=run_window(args.output/'long-feature',binary,'on',seconds)
        result['windows'].append(dict(name='long-feature',status=value['status'],evidence='long-feature/result.json'))
        complete=value['status']=='PASS'
    result['status']='PASS' if complete and all(sha(Path(p))==h for p,h in result['inputs'].items()) and \
        sha(binary)==result['binary_identity']['binary_sha256'] else 'FAIL'
    result['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    return 0 if result['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
