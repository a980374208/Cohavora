"""One-shot historical frozen-publisher Linux exit diagnostic.

Run SDK mode under GNU timeout -k 2s 120s. The pinned publisher preserves the
historical reproduction input; this controller does not validate current source.

Plan is trusted only after its separately frozen SHA256 is checked. No RTC import
occurs in this controller (or API-only cleanup). Raw stderr never leaves the run.
"""
from __future__ import annotations
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import threading
import time

PUBLISHER_SHA = '5113c47cff213ef8161238d4e20eff9c1d19fb1e43e3d35cb44bd8f827e5d373'
MAX_STDERR = 1024 * 1024

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def load_plan(path, expected):
    if digest(path) != expected:
        raise ValueError('plan_hash_mismatch')
    p = json.loads(Path(path).read_text())
    if not re.fullmatch('[a-f0-9]{32}', p['run_id']):
        raise ValueError('run_id_invalid')
    base = Path('/root/livekit-product-acceptance')
    wanted = base / ('native-exit-' + p['run_id']) / 'exit-capture'
    if Path(p['output']) != wanted or p['room'] != 'native-exit-' + p['run_id']:
        raise ValueError('run_scope_invalid')
    if any(os.environ.get(k) for k in ('LD_PRELOAD', 'LIVEKIT_LIB_PATH')):
        raise ValueError('native_override_forbidden')
    for filename, sha in p['files'].items():
        if not re.fullmatch('[a-f0-9]{64}', sha) or digest(filename) != sha:
            raise ValueError('file_hash_mismatch')
    required = [p[k] for k in ('publisher_path','wrapper_path','subscriber_path','scheduler_policy')]
    required += p['sdk_files'] + [p['api_helper_path']]
    # The publisher family imports these companion modules dynamically.
    # Every executable input must belong to the independently frozen manifest.
    required += [str(Path(p['publisher_path']).parent / name) for name in
                 ('product_pilot_scheduler.py', 'product_pilot_timing.py',
                  'product_pilot_video_counter.py')]
    required += [str(Path(p['subscriber_path']).parent / name) for name in
                 ('product_pilot_scheduler.py', 'product_pilot_video_counter.py')]
    if len(p['sdk_files']) != 6 or not any(x.endswith('.so') for x in p['sdk_files']):
        raise ValueError('sdk_fingerprint_incomplete')
    if any(x not in p['files'] for x in required):
        raise ValueError('manifest_incomplete')
    if p['files'][p['publisher_path']] != PUBLISHER_SHA:
        raise ValueError('publisher_changed')
    return p

def save(path, value):
    temp = path.with_suffix('.tmp')
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(value, stream, separators=(',', ':'))
    temp.replace(path)

def start_ticks(pid):
    return int(Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[19])

class Child:
    def __init__(self, command, directory):
        env = dict(os.environ, RUST_BACKTRACE='full')
        env.pop('LD_PRELOAD', None)
        self.overflow = False
        self.total = 0
        fd = os.open(directory / 'stderr.log', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        self.proc = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, start_new_session=False)
        self.ticks = start_ticks(self.proc.pid)
        def drain():
            with os.fdopen(fd,'wb') as stream:
                while True:
                    data = self.proc.stderr.read(8192)
                    if not data: break
                    stream.write(data[:max(0, MAX_STDERR-self.total)])
                    self.total += len(data)
                    self.overflow |= self.total > MAX_STDERR
        self.thread = threading.Thread(target=drain, daemon=True)
        self.thread.start()

def wait_json(child, path, deadline):
    while time.monotonic() < deadline:
        if path.exists(): return json.loads(path.read_text())
        if child.proc.poll() is not None: raise ValueError('child_early_exit')
        time.sleep(.05)
    raise ValueError('phase_timeout')

def validate_native_counts(decoded):
    if not (decoded['video_tracks'] == 10 and decoded['audio_tracks'] == 1
            and decoded['minimum_video_frames'] >= 50 and decoded['audio_frames'] >= 500):
        raise ValueError('native_media_measurement_failed')

def validate_subscriber(decoded, result):
    validate_native_counts(decoded)
    if not (result['schema'] == 1 and result['status'] in
            ('EXIT_DIAGNOSTIC_COMPLETE_GEOMETRY_DIFFERENCE_RECORDED',
             'EXIT_DIAGNOSTIC_COMPLETE_GEOMETRY_MATCH_OBSERVED')
            and result['stop_requested'] is True and result['disconnected'] is True
            and result['all_streams_closed'] is True and result['errors'] == []):
        raise ValueError('subscriber_lifecycle_failed')

def api_action(p, action, timeout=10):
    proc = subprocess.run([sys.executable,p['api_helper_path'],'--plan',p['_plan_path'],
        '--'+action],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=timeout)
    value = json.loads(proc.stdout)
    if proc.returncode or value.get('status') != 'COMPLETE':
        raise ValueError('api_'+action+'_failed')

def capture_maps(child, path, observations, phase):
    if start_ticks(child.proc.pid) != child.ticks:
        raise ValueError('publisher_pid_reused')
    raw=Path(f'/proc/{child.proc.pid}/maps').read_bytes()
    if len(raw)>256*1024: raise ValueError('maps_limit_exceeded')
    rows=[]
    for line in raw.decode().splitlines():
        fields=line.split(None,5)
        if len(fields)==6 and fields[5].startswith('/'):
            lo,hi=fields[0].split('-')
            rows.append(dict(start=int(lo,16),end=int(hi,16),offset=int(fields[2],16),path=fields[5]))
    observations.append(dict(stage=phase,monotonic_ns=time.monotonic_ns(),mappings=rows))
    if path.exists(): path.unlink()
    save(path,dict(pid=child.proc.pid,start_ticks=child.ticks,snapshots=observations))

def sdk_run(p):
    root = Path(p['output']); root.mkdir(mode=0o700, parents=False, exist_ok=False)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    resource.setrlimit(resource.RLIMIT_RTPRIO,(0,0))
    sys.path.insert(0,str(Path(p['publisher_path']).parent))
    from product_pilot_scheduler import realtime_child_command, load_scheduler_policy
    scheduler_policy=load_scheduler_policy(Path(p['scheduler_policy']))
    begin = time.monotonic(); children = []; phases = []; maps = []
    result = dict(schema=1,run_id=p['run_id'],status='MEASUREMENT_FAILED',exit_code=None)
    try:
        # Count only; do not export command lines or other users' identities.
        others = 0
        for item in Path('/proc').iterdir():
            if item.name.isdigit() and int(item.name) not in (os.getpid(),os.getppid()):
                try:
                    cmd = (item/'cmdline').read_bytes()
                    others += any(s in cmd for s in (b'product_pilot_load.py',b'subscriber.py',b'run_publisher_exit_capture.py'))
                except OSError: pass
        result['other_test_process_count'] = others
        if others: raise ValueError('other_test_running')
        if os.statvfs(root).f_bavail * os.statvfs(root).f_frsize < 64*1024*1024:
            raise ValueError('insufficient_disk')
        memory = dict(line.split(':',1) for line in Path('/proc/meminfo').read_text().splitlines())
        result['available_memory_kib']=int(memory['MemAvailable'].split()[0])
        if result['available_memory_kib'] < 128*1024:
            raise ValueError('insufficient_memory')
        api_action(p,'check-plan',10)
        common = ['--dependencies',p['dependencies'],'--config',p['config'],
            '--run-id',p['run_id'],'--room',p['room'],'--url',p['url'],
            '--scheduler-policy',p['scheduler_policy']]
        load = root/'load'; load.mkdir(mode=0o700)
        pub = Child(realtime_child_command([sys.executable,p['wrapper_path'],'--diagnostic-exit-phases',str(root/'phases.jsonl'),
            '--source-sha256',p['files'][p['publisher_path']],
            p['publisher_path'],*common,'--output',str(load),'--seconds','120']),load)
        children.append(pub)
        result['publisher_process']=dict(pid=pub.proc.pid,start_ticks=pub.ticks)
        ready = wait_json(pub,load/'ready.json',min(begin+15,begin+120))
        if ready['pid'] != pub.proc.pid or ready['run_id'] != p['run_id'] or ready['state'] != 'READY':
            raise ValueError('publisher_ready_invalid')
        # Reuse the original validator, without executing its RTC entrypoint.
        sys.path.insert(0,str(Path(p['publisher_path']).parent))
        import product_pilot_load
        product_pilot_load.validate_ready(ready,p['run_id'],pub.proc.pid,
            product_pilot_load.cgroup_fingerprint(),scheduler_policy)
        if ready['sdk']!='1.1.19': raise ValueError('publisher_sdk_mismatch')
        api_action(p,'publisher-ready')
        capture_maps(pub,root/'publisher-maps.json',maps,'ready')
        if begin+120-time.monotonic() < 97: raise ValueError('subscriber_budget_admission_failed')
        subdir=root/'subscriber'; subdir.mkdir(mode=0o700)
        sub=Child(realtime_child_command([sys.executable,p['subscriber_path'],*common,'--output',str(subdir),'--seconds','120']),subdir)
        children.append(sub)
        result['subscriber_process']=dict(pid=sub.proc.pid,start_ticks=sub.ticks)
        subready=wait_json(sub,subdir/'ready.json',begin+35)
        if subready['pid']!=sub.proc.pid or subready['run_id']!=p['run_id'] or subready['status']!='CONNECTED':
            raise ValueError('subscriber_ready_invalid')
        hold_until=time.monotonic()+20
        api_action(p,'subscriber-active',min(10,max(.01,begin+110-time.monotonic())))
        wait_json(sub,subdir/'decoded.json',min(begin+50,hold_until))
        while time.monotonic()<hold_until:
            if pub.proc.poll() is not None or sub.proc.poll() is not None:
                raise ValueError('hold_process_early_exit')
            time.sleep(min(.05,max(0,hold_until-time.monotonic())))
        decoded=json.loads((subdir/'decoded.json').read_text())
        if decoded['run_id']!=p['run_id'] or decoded['pid']!=sub.proc.pid:
            raise ValueError('decoded_identity_mismatch')
        validate_native_counts(decoded)
        save(subdir/'stop.json',dict(run_id=p['run_id'],action='stop'))
        phases.append('subscriber_stop_requested')
        sub.proc.wait(timeout=min(15,max(.01,begin+100-time.monotonic()))); sub.thread.join(timeout=1)
        if sub.proc.returncode != 0 or sub.overflow: raise ValueError('subscriber_process_failed')
        subresult=json.loads((subdir/'result.json').read_text())
        if subresult['run_id']!=p['run_id'] or subresult['pid']!=sub.proc.pid:
            raise ValueError('subscriber_result_identity_mismatch')
        validate_subscriber(json.loads((subdir/'decoded.json').read_text()),subresult)
        api_action(p,'post-subscriber',min(10,max(.01,begin+115-time.monotonic())))
        capture_maps(pub,root/'publisher-maps.json',maps,'before_stop')
        save(load/'stop.json',dict(run_id=p['run_id'],action='stop'))
        phases.append('publisher_stop_requested')
        pub.proc.wait(timeout=max(.01,begin+119-time.monotonic())); pub.thread.join(timeout=1)
        result['exit_code']=pub.proc.returncode
        result['stderr_path']=str(load/'stderr.log')
        terminal=json.loads((load/'result.json').read_text())
        result['publisher_result']=dict(status=terminal['status'],rooms_disconnected=terminal['rooms_disconnected'],capture_tasks_drained=terminal['capture_tasks_drained'],error_count=len(terminal['errors']))
        if not (terminal['status']=='COMPLETE' and terminal['errors']==[] and terminal['pid']==pub.proc.pid
                and terminal['run_id']==p['run_id'] and terminal['capture_tasks_drained'] is True
                and terminal['rooms_disconnected']==10 and terminal['sources_created']==11
                and terminal['sources_closed']==11 and terminal['audio_queues_cleared']==1):
            raise ValueError('publisher_result_failed')
        if pub.overflow: raise ValueError('stderr_limit_exceeded')
        raw=(load/'stderr.log').read_bytes()
        result['rust_backtrace']=dict(panic_observed=b'panicked at' in raw,stack_backtrace_observed=b'stack backtrace:' in raw,fatal_runtime_observed=b'fatal runtime error' in raw,bytes=len(raw))
        records=[json.loads(x) for x in (root/'phases.jsonl').read_text().splitlines()]
        result['phase_records']=[{k:x[k] for k in ('phase','kind','pid','tid','monotonic_ns','sequence','success','dropped')} for x in records]
        if pub.proc.returncode==0 and (not records or records[-1]['phase']!='integrity'
                or records[-1]['success'] is not True or any(x['dropped'] for x in records)
                or any(x['kind']=='end' and x['success'] is not True for x in records)):
            raise ValueError('phase_integrity_failed')
        result['status']='NON_REPRODUCED' if pub.proc.returncode==0 else 'NATIVE_EXIT_OBSERVED'
    except Exception as error:
        result['failure_type']=type(error).__name__
        result['failure_reason']='measurement_failed'
    finally:
        result['child_terminal']=[]
        for child in children:
            forced=child.proc.poll() is None
            if forced:
                # Children share outer GNU timeout process group. Kill exact PID only.
                try:
                    if start_ticks(child.proc.pid)==child.ticks: child.proc.kill()
                except OSError: pass
                result['status']='MEASUREMENT_FAILED'
            try:
                child.proc.wait(timeout=min(2,max(.01,begin+119.8-time.monotonic())))
            except subprocess.TimeoutExpired:
                result['status']='MEASUREMENT_FAILED'
            child.thread.join(timeout=.2)
            if child.thread.is_alive() or child.overflow:
                result['status']='MEASUREMENT_FAILED'
            result['child_terminal'].append(dict(pid=child.proc.pid,start_ticks=child.ticks,
                forced=forced,exit_code=child.proc.poll(),reaped=child.proc.returncode is not None))
        if children: result['exit_code']=children[0].proc.poll()
        result['publisher_started']=len(children)>=1
        result['subscriber_started']=len(children)==2
        result['publisher_processes']=1 if children else 0
        result['subscriber_processes']=1 if len(children)==2 else 0
        result['phases']=phases
        result['elapsed_s']=round(time.monotonic()-begin,3)
        save(root/'capture-result.json',result)
    return 0 if result['status']!='MEASUREMENT_FAILED' else 2

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--plan-sha256',required=True)
    parser.add_argument('--cleanup',action='store_true')
    args=parser.parse_args()
    p=load_plan(args.plan,args.plan_sha256)
    p['_plan_path']=str(args.plan)
    if args.cleanup:
        api_action(p,'cleanup',30)
        return 0
    return sdk_run(p)

if __name__=='__main__':
    raise SystemExit(main())
