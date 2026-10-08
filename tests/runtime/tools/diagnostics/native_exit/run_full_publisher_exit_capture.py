"""One-shot historical frozen-publisher Linux exit diagnostic.

Run SDK mode under GNU timeout -k 2s 660s. The pinned publisher preserves the
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
    wanted = base / ('native-exit-' + p['run_id']) / 'full-exit-capture'
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
    validate_full_plan(p)
    return p

def all_workers(p):
    return p.get('diagnostic_profile') == 'full-exit-all-660-two-240'

def subscriber_command(p, common, directory):
    args = [p['subscriber_path'], *common, '--output', str(directory), '--seconds', '660']
    if not all_workers(p):
        return [sys.executable, *args]
    trace = directory / 'pthread-exit-trace'
    return [sys.executable, p['publisher_launcher_path'], '--observer-library', p['observer_library'],
            '--observer-sha256', p['files'][p['observer_library']], '--trace-directory', str(trace),
            '--', p['subscriber_wrapper_path'], '--diagnostic-exit-phases', str(directory / 'phases.jsonl'),
            '--source-sha256', p['files'][p['subscriber_path']], *args]

def observer_valid(value, child, p):
    return (value.get('pid') == child.proc.pid and value.get('observer_sha256') == p['files'][p['observer_library']]
            and value.get('status') == 'READY' and value.get('preload_in_child_environment') is False
            and value.get('rtc_imported') is False)

def exit_evidence(child, directory, phase_path, subscriber=False):
    raw = (directory / 'stderr.log').read_bytes()
    records = []
    malformed = False
    try:
        records = [json.loads(line) for line in phase_path.read_text().splitlines()]
    except (OSError, ValueError):
        malformed = True
    fields = ('phase', 'kind', 'pid', 'tid', 'monotonic_ns', 'sequence', 'success', 'dropped')
    valid_rows = [dict((key, row[key]) for key in fields) for row in records
                  if isinstance(row, dict) and all(key in row for key in fields)]
    return dict(pid=child.proc.pid, start_ticks=child.ticks, exit_code=child.proc.poll(),
                reaped=child.proc.returncode is not None, stderr_overflow=child.overflow,
                panic_observed=b'panicked at' in raw, stack_header_observed=b'stack backtrace:' in raw,
                stderr_bytes=len(raw), phase_records=valid_rows, phase_parse_complete=not malformed and len(valid_rows)==len(records),
                phase_integrity=not malformed and phases_complete(records,child.proc.pid,subscriber),
                native_abort=child.proc.poll() == -signal.SIGABRT and not child.overflow,
                root_cause_established=False)

def lifecycle_budget(remaining):
    if remaining not in (1, 2):
        raise ValueError('lifecycle_count_invalid')
    # 20s readiness, 240s connected hold, 15s reap, two bounded API calls,
    # plus publisher cleanup and evidence drain reserve.
    return remaining * 295 + 45

def validate_full_plan(p):
    for key in ('publisher_launcher_path', 'observer_library'):
        if p.get(key) not in p['files']:
            raise ValueError('observer_manifest_incomplete')
    if p.get('diagnostic_profile') not in ('full-exit-660-two-240', 'full-exit-all-660-two-240'):
        raise ValueError('full_profile_required')
    if all_workers(p) and p.get('subscriber_wrapper_path') not in p['files']:
        raise ValueError('subscriber_wrapper_manifest_incomplete')
    if p.get('bundle_root') != p['output'] or p.get('remote_root') != '/root/livekit-product-acceptance':
        raise ValueError('api_layout_mismatch')
    if p.get('room_name') != p['room']:
        raise ValueError('api_room_mismatch')
    if p.get('hard_sdk_seconds') != 660 or p.get('cleanup_seconds') != 30:
        raise ValueError('deadline_invalid')

def phases_complete(records, pid, subscriber=False):
    expected = [('cleanup', 'begin')]
    for phase in ('capture_drain', 'audio_clear', 'room_disconnect', 'source_close', 'result_commit'):
        expected.extend(((phase, 'begin'), (phase, 'end')))
    expected.extend((('publish_return', 'end'), ('asyncio_run_return', 'end'),
                     ('python_atexit', 'begin'), ('native_ffi_dispose', 'begin'),
                     ('native_ffi_dispose', 'end'), ('integrity', 'end')))
    if subscriber:
        expected = [('cleanup', 'begin')]
        for phase in ('subscription_cancel_gather', 'room_disconnect', 'result_commit', 'reference_clear'):
            expected.extend(((phase, 'begin'), (phase, 'end')))
        expected.extend((('subscriber_run_return', 'end'), ('gc_weakref_retirement', 'begin'),
                         ('gc_weakref_retirement', 'end'), ('subscriber_retirement_return', 'end'),
                         ('asyncio_run_return', 'end'), ('python_atexit', 'begin'),
                         ('native_ffi_dispose', 'begin'), ('native_ffi_dispose', 'end'), ('integrity', 'end')))
    actual = []
    handles = {}
    previous = 0
    try:
        for index, row in enumerate(records):
            if any(type(row[k]) is not int for k in ('pid', 'tid', 'sequence', 'dropped', 'monotonic_ns')):
                return False
            if (row['pid'] != pid or row['tid'] <= 0 or row['sequence'] != index + 1
                    or row['dropped'] != 0 or row['monotonic_ns'] <= 0
                    or row['monotonic_ns'] < previous or type(row['success']) is not bool
                    or row['kind'] not in ('begin', 'end')
                    or (row['kind'] == 'end' and row['success'] is not True)):
                return False
            previous = row['monotonic_ns']
            if row['phase'] == 'native_drop_handle':
                # Native calls may recur on multiple threads, but each thread's
                # begin/end nesting must close before terminal integrity.
                if not actual or actual[-1] == ('integrity', 'end'):
                    return False
                depth = handles.get(row['tid'], 0)
                if row['kind'] == 'begin':
                    handles[row['tid']] = depth + 1
                elif depth == 0:
                    return False
                else:
                    handles[row['tid']] = depth - 1
            else:
                actual.append((row['phase'], row['kind']))
                if row['phase'] == 'integrity' and any(handles.values()):
                    return False
        return actual == expected and not any(handles.values())
    except (KeyError, TypeError):
        return False

def classify_exit(code, raw, terminal_valid, phases_valid):
    if code == -signal.SIGABRT and b'panicked at' in raw and terminal_valid:
        return 'ABORT_REPRODUCED'
    if code == 0 and terminal_valid and phases_valid:
        return 'NON_REPRODUCED'
    return 'MEASUREMENT_FAILED'

FAILURE_CODES = frozenset(('subscriber_wrapper_manifest_incomplete',)) | frozenset(('api_layout_mismatch', 'api_room_mismatch', 'child_early_exit', 'deadline_invalid', 'decoded_identity_mismatch', 'file_hash_mismatch', 'full_profile_required', 'hold_process_early_exit', 'insufficient_disk', 'insufficient_memory', 'lifecycle_count_invalid', 'manifest_incomplete', 'maps_limit_exceeded', 'native_media_measurement_failed', 'native_override_forbidden', 'observer_manifest_incomplete', 'observer_ready_invalid', 'other_test_running', 'phase_integrity_failed', 'phase_timeout', 'plan_hash_mismatch', 'publisher_changed', 'publisher_pid_reused', 'publisher_ready_invalid', 'publisher_result_failed', 'publisher_sdk_mismatch', 'run_id_invalid', 'run_scope_invalid', 'sdk_fingerprint_incomplete', 'startup_budget_admission_failed', 'stderr_limit_exceeded', 'subscriber_budget_admission_failed', 'subscriber_lifecycle_failed', 'subscriber_process_failed', 'subscriber_ready_invalid', 'subscriber_result_identity_mismatch'))

def failure_code(error):
    return str(error) if type(error) is ValueError and str(error) in FAILURE_CODES else "measurement_failed"

def save(path, value):
    temp = path.with_suffix('.tmp')
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(value, stream, separators=(',', ':'))
    temp.replace(path)

def start_ticks(pid):
    return int(Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[19])

class Child:
    def __init__(self, command, directory, extra_env=None):
        env = dict(os.environ, RUST_BACKTRACE='full')
        if extra_env: env.update(extra_env)
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
        '--'+action],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=timeout,env=dict(os.environ,RUST_BACKTRACE='full'))
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
    result = dict(schema=1,run_id=p['run_id'],status='MEASUREMENT_FAILED',exit_code=None,qualification_credit=False,diagnostic_profile=p['diagnostic_profile'])
    try:
        # Count only; do not export command lines or other users' identities.
        others = 0
        for item in Path('/proc').iterdir():
            if item.name.isdigit() and int(item.name) not in (os.getpid(),os.getppid()):
                try:
                    cmd = (item/'cmdline').read_bytes()
                    others += any(s in cmd for s in (b'product_pilot_load.py',b'subscriber.py',b'run_publisher_exit_capture.py',b'run_full_publisher_exit_capture.py'))
                except OSError: pass
        result['other_test_process_count'] = others
        if others: raise ValueError('other_test_running')
        if os.statvfs(root).f_bavail * os.statvfs(root).f_frsize < 64*1024*1024:
            raise ValueError('insufficient_disk')
        memory = dict(line.split(':',1) for line in Path('/proc/meminfo').read_text().splitlines())
        result['available_memory_kib']=int(memory['MemAvailable'].split()[0])
        if result['available_memory_kib'] < 128*1024:
            raise ValueError('insufficient_memory')
        if begin+660-time.monotonic() < lifecycle_budget(2)+15:
            raise ValueError('startup_budget_admission_failed')
        result['stage']='api_check_plan'
        api_action(p,'check-plan',10)
        common = ['--dependencies',p['dependencies'],'--config',p['config'],
            '--run-id',p['run_id'],'--room',p['room'],'--url',p['url'],
            '--scheduler-policy',p['scheduler_policy']]
        load = root/'load'; load.mkdir(mode=0o700)
        result['stage']='publisher_start'
        trace=root/'pthread-exit-trace'; trace.mkdir(mode=0o700)
        pub = Child(realtime_child_command([sys.executable,p['publisher_launcher_path'],'--observer-library',p['observer_library'],'--observer-sha256',p['files'][p['observer_library']],'--trace-directory',str(trace),'--',p['wrapper_path'],'--diagnostic-exit-phases',str(root/'phases.jsonl'),
            '--source-sha256',p['files'][p['publisher_path']],
            p['publisher_path'],*common,'--output',str(load),'--seconds','660']),load,dict(NATIVE_EXIT_TRACE_DIR=str(trace)))
        children.append(pub)
        result['publisher_process']=dict(pid=pub.proc.pid,start_ticks=pub.ticks)
        observer_ready=wait_json(pub,trace/'observer-ready.json',min(begin+15,begin+660))
        if (observer_ready.get('pid') != pub.proc.pid or observer_ready.get('observer_sha256') != p['files'][p['observer_library']]
                or observer_ready.get('status') != 'READY' or observer_ready.get('preload_in_child_environment') is not False
                or observer_ready.get('rtc_imported') is not False):
            raise ValueError('observer_ready_invalid')
        result['observer_ready']=True
        ready = wait_json(pub,load/'ready.json',min(begin+15,begin+660))
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
        for index in range(2):
            result['stage']=f'subscriber_{index+1}_start'
            if begin+660-time.monotonic() < lifecycle_budget(2-index): raise ValueError('subscriber_budget_admission_failed')
            subdir=root/f'subscriber-{index+1}'; subdir.mkdir(mode=0o700)
            if all_workers(p): (subdir/'pthread-exit-trace').mkdir(mode=0o700)
            sub=Child(realtime_child_command(subscriber_command(p,common,subdir)),subdir)
            sub.directory=subdir
            children.append(sub)
            result.setdefault('subscriber_process',[]).append(dict(pid=sub.proc.pid,start_ticks=sub.ticks))
            if all_workers(p):
                proof=wait_json(sub,subdir/'pthread-exit-trace/observer-ready.json',min(begin+630,time.monotonic()+15))
                if not observer_valid(proof,sub,p): raise ValueError('observer_ready_invalid')
            subready=wait_json(sub,subdir/'ready.json',min(begin+630,time.monotonic()+20))
            if subready['pid']!=sub.proc.pid or subready['run_id']!=p['run_id'] or subready['status']!='CONNECTED':
                raise ValueError('subscriber_ready_invalid')
            result['stage']=f'subscriber_{index+1}_hold'
            hold_until=time.monotonic()+240
            api_action(p,'subscriber-active',min(10,max(.01,begin+640-time.monotonic())))
            wait_json(sub,subdir/'decoded.json',min(time.monotonic()+20,hold_until))
            while time.monotonic()<hold_until:
                if pub.proc.poll() is not None or sub.proc.poll() is not None:
                    raise ValueError('hold_process_early_exit')
                time.sleep(min(.05,max(0,hold_until-time.monotonic())))
            decoded=json.loads((subdir/'decoded.json').read_text())
            if decoded['run_id']!=p['run_id'] or decoded['pid']!=sub.proc.pid:
                raise ValueError('decoded_identity_mismatch')
            validate_native_counts(decoded)
            result['stage']=f'subscriber_{index+1}_stop'
            if all_workers(p): capture_maps(sub,subdir/'subscriber-maps.json',[],'before_stop')
            save(subdir/'stop.json',dict(run_id=p['run_id'],action='stop'))
            phases.append('subscriber_stop_requested')
            sub.proc.wait(timeout=min(15,max(.01,begin+630-time.monotonic()))); sub.thread.join(timeout=1)
            if all_workers(p):
                result.setdefault('subscriber_exit_evidence',[]).append(exit_evidence(sub,subdir,subdir/'phases.jsonl',subscriber=True))
            if sub.proc.returncode != 0 or sub.overflow: raise ValueError('subscriber_process_failed')
            subresult=json.loads((subdir/'result.json').read_text())
            if subresult['run_id']!=p['run_id'] or subresult['pid']!=sub.proc.pid:
                raise ValueError('subscriber_result_identity_mismatch')
            validate_subscriber(json.loads((subdir/'decoded.json').read_text()),subresult)
            if all_workers(p) and not result['subscriber_exit_evidence'][-1]['phase_integrity']:
                raise ValueError('phase_integrity_failed')
            api_action(p,'post-subscriber',min(10,max(.01,begin+650-time.monotonic())))
        capture_maps(pub,root/'publisher-maps.json',maps,'before_stop')
        result['stage']='publisher_stop'
        save(load/'stop.json',dict(run_id=p['run_id'],action='stop'))
        phases.append('publisher_stop_requested')
        pub.proc.wait(timeout=max(.01,begin+659-time.monotonic())); pub.thread.join(timeout=1)
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
        result['rust_backtrace']=dict(panic_observed=b'panicked at' in raw,stack_backtrace_observed=b'stack backtrace:' in raw,stack_header_observed=b'stack backtrace:' in raw,fatal_runtime_observed=b'fatal runtime error' in raw,bytes=len(raw))
        records=[json.loads(x) for x in (root/'phases.jsonl').read_text().splitlines()]
        result['phase_records']=[{k:x[k] for k in ('phase','kind','pid','tid','monotonic_ns','sequence','success','dropped')} for x in records]
        result['phase_integrity']=phases_complete(records,pub.proc.pid)
        result['root_cause_established']=False
        if pub.proc.returncode==0 and not result['phase_integrity']:
            raise ValueError('phase_integrity_failed')
        result['status']=classify_exit(pub.proc.returncode,raw,True,result['phase_integrity'])
    except Exception as error:
        result['failure_type']=type(error).__name__
        result['failure_reason']=failure_code(error)
        if all_workers(p) and children:
            pub=children[0]
            result['status']='MEASUREMENT_FAILED'
            # A failed receiver never earns qualification or a mixed worker PASS.
            # Give publisher its own cooperative exit, within the existing budget.
            try:
                if pub.proc.poll() is None and time.monotonic() < begin+650:
                    capture_maps(pub,root/'publisher-maps.json',maps,'receiver_failure_before_stop')
                    result['publisher_failure_stop_requested']=True
                    if not (load/'stop.json').exists():
                        save(load/'stop.json',dict(run_id=p['run_id'],action='stop'))
                    pub.proc.wait(timeout=max(.01,begin+659-time.monotonic()))
                pub.thread.join(timeout=min(1,max(.01,begin+659.5-time.monotonic())))
                result['publisher_independent_exit']=exit_evidence(pub,load,root/'phases.jsonl')
            except Exception as cleanup_error:
                result['publisher_exit_capture_failure']=failure_code(cleanup_error)
    finally:
        result['child_terminal']=[]
        if all_workers(p):
            captured={row['pid'] for row in result.get('subscriber_exit_evidence',[])}
            for child in children[1:]:
                if child.proc.poll() is not None and child.proc.pid not in captured:
                    try:
                        child.thread.join(timeout=.2)
                        result.setdefault('subscriber_exit_evidence',[]).append(exit_evidence(child,child.directory,child.directory/'phases.jsonl',subscriber=True))
                    except Exception:
                        result['subscriber_exit_capture_failed']=True
        for child in children:
            forced=child.proc.poll() is None
            if forced:
                # Children share outer GNU timeout process group. Kill exact PID only.
                try:
                    if start_ticks(child.proc.pid)==child.ticks: child.proc.kill()
                except OSError: pass
                result['status']='MEASUREMENT_FAILED'
            try:
                child.proc.wait(timeout=min(2,max(.01,begin+659.8-time.monotonic())))
            except subprocess.TimeoutExpired:
                result['status']='MEASUREMENT_FAILED'
            child.thread.join(timeout=.2)
            if child.thread.is_alive() or child.overflow:
                result['status']='MEASUREMENT_FAILED'
            result['child_terminal'].append(dict(pid=child.proc.pid,start_ticks=child.ticks,
                forced=forced,exit_code=child.proc.poll(),reaped=child.proc.returncode is not None))
        if children: result['exit_code']=children[0].proc.poll()
        if all_workers(p):
            result['subscriber_abort']=any(row['native_abort'] for row in result.get('subscriber_exit_evidence',[]))
            if result['subscriber_abort']: result['status']='MEASUREMENT_FAILED'
        result['publisher_started']=len(children)>=1
        result['subscriber_started']=len(children)==3
        result['publisher_processes']=1 if children else 0
        result['subscriber_processes']=max(0,len(children)-1)
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
