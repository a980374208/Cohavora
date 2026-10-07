"""Stop once, recover read-only receipts, and verify downloaded immutable evidence."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

from product_aliyun_transport import AliyunRemoteCommandError, execute, target


STOP_SCRIPT = r'''
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);run=sys.argv[2]
dest=root/('pilot-'+run[:8])
assert json.loads((dest/'ready.json').read_text())['run_id']==run
stop=dest/'stop'
try:
 with stop.open('x'):pass
except FileExistsError:pass
print(json.dumps(dict(run_id=run,stop_present=True,stop_mtime_ns=stop.stat().st_mtime_ns)))
'''

STATE_SCRIPT = r'''
import hashlib,json,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1]);run=sys.argv[2];timing=sys.argv[3]=='1'
dest=root/('pilot-'+run[:8])
assert json.loads((dest/'ready.json').read_text())['run_id']==run
stop=dest/'stop';last=None
with (dest/'remote.jsonl').open('rb') as f:
 f.seek(0,2);size=f.tell();f.seek(max(0,size-65536));lines=f.read().splitlines(keepends=True)
 for line in reversed(lines):
  if line.endswith(b'\n'):last=json.loads(line);break
assert last is None or last['run_id']==run
route_path=root/('pilot-'+run[:8]+'-route.json')
route=json.loads(route_path.read_text()) if route_path.exists() else None
terminal=last is not None and last['event']=='collector.stopped' and route is not None and route['status'] in ('COMPLETE','FAILED')
files={};cleanup=None
if terminal:
 assert route['run_id']==run
 sys.path.insert(0,str(root))
 from product_pilot_local_route import LocalSfuRoute
 current=LocalSfuRoute(json.loads((root/'product_aliyun_target.json').read_text()),run,root/'unused-readonly.json')
 cleanup=dict(rule_check_codes=[subprocess.run(current.command('-C',current.rule(p,port)),capture_output=True).returncode for p,port in current.ports],
  cgroup_present=current.group.exists())
 for name in (['remote.jsonl','timing.jsonl','load-process/cadence.jsonl'] if timing else ['remote.jsonl']):
  path=dest/name
  h=hashlib.sha256()
  with path.open('rb') as f:
   for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
  files[name]=dict(size=path.stat().st_size,sha256=h.hexdigest())
print(json.dumps(dict(run_id=run,stop_present=stop.exists(),stop_mtime_ns=stop.stat().st_mtime_ns if stop.exists() else None,
 terminal=terminal,last_event={k:last.get(k) for k in ('run_id','event','status','utc')} if last else None,
 route=route,actual_cleanup=cleanup,files=files)))
'''


# This self-contained source is sent through the existing executor, not uploaded.
# Offline tests execute these same definitions with a proc fixture and pidfd stubs.
LAUNCH_COMMON = r'''
import itertools,json,os,re,signal,subprocess,sys,time
from pathlib import Path

def launch_route(root,run):
 sys.path.insert(0,str(root))
 from product_pilot_local_route import LocalSfuRoute
 cfg=json.loads((root/'product_aliyun_target.json').read_text())
 if cfg['remote_root']!=str(root):raise ValueError('launch_root_mismatch')
 return LocalSfuRoute(cfg,run,root/'unused-launch-readonly.json')

def launch_owner(root,run,pid,proc=Path('/proc')):
 base=proc/str(pid)
 with (base/'cmdline').open('rb') as stream:raw=stream.read(65537)
 if len(raw)>65536:raise ValueError('launch_cmdline_too_large')
 roles={str(root/'product_pilot_local_route.py'):'route',
  str(root/'product_pilot_remote.py'):'remote',str(root/'product_pilot_load.py'):'load'}
 parts=[part for part in raw.split(b'\0') if part]
 if len(parts)<2 or parts[1] not in {path.encode() for path in roles}:return None
 argv=[part.decode('utf-8','strict') for part in parts]
 role=roles[argv[1]]
 args=argv[2:]
 if role=='route':args=args[:args.index('--')] if '--' in args else args
 values=[args[i+1] for i,value in enumerate(args[:-1]) if value=='--run-id']
 if '--run-id='+run in args:raise ValueError('launch_owned_argv_ambiguous')
 if run not in values:return None
 def exact(flag,value):
  positions=[i for i,arg in enumerate(args) if arg==flag]
  return len(positions)==1 and positions[0]+1<len(args) and args[positions[0]+1]==value
 dest=root/('pilot-'+run[:8])
 valid=values==[run] and argv[0]==str(root/'venv/bin/python')
 if role=='route':
  valid=valid and exact('--target-config',str(root/'product_aliyun_target.json')) and exact('--result',str(root/('pilot-'+run[:8]+'-route.json')))
 else:valid=valid and exact('--output',str(dest/'load-process' if role=='load' else dest))
 if not valid:raise ValueError('launch_owned_argv_ambiguous')
 with (base/'stat').open() as stream:stat=stream.read(4097)
 if len(stat)>4096 or ')' not in stat:raise ValueError('launch_stat_invalid')
 fields=stat[stat.rfind(')')+2:].split()
 if len(fields)<20:raise ValueError('launch_stat_invalid')
 ticks=int(fields[19])
 if ticks<=0:raise ValueError('launch_start_ticks_invalid')
 if fields[0] in ('Z','X'):return None
 return dict(pid=pid,start_ticks=ticks,role=role)

def launch_scan(root,run,proc=Path('/proc')):
 owners=[];errors=[]
 entries=list(itertools.islice(proc.iterdir(),4097))
 if len(entries)>4096:raise ValueError('launch_proc_scan_budget_exceeded')
 for entry in entries:
  if not entry.name.isdecimal():continue
  try:
   owner=launch_owner(root,run,int(entry.name),proc)
   if owner is not None:owners.append(owner)
  except (FileNotFoundError,ProcessLookupError):pass
  except Exception as error:
   errors.append(dict(stage='proc',pid=int(entry.name),error_type=type(error).__name__))
   if len(errors)>=32:break  # Incomplete scans fail; keep the receipt bounded.
 return owners,errors

def inspect_launch(root,run,proc=Path('/proc')):
 owners,errors=launch_scan(root,run,proc)
 dest=root/('pilot-'+run[:8])
 if dest.exists() and dest.resolve()!=dest:
  errors.append(dict(stage='receipt_directory',error_type='SymlinkOrPathEscape'))
 for path in (dest/'ready.json',root/('pilot-'+run[:8]+'-route.json')):
  if path.exists():
   try:
    with path.open('rb') as stream:raw=stream.read(65537)
    if len(raw)>65536 or json.loads(raw)['run_id']!=run:raise ValueError('launch_receipt_identity_mismatch')
   except Exception as error:errors.append(dict(stage='receipt',error_type=type(error).__name__))
 route=launch_route(root,run);codes=[]
 for protocol,port in route.ports:
  try:codes.append(subprocess.run(route.command('-C',route.rule(protocol,port)),capture_output=True,timeout=6).returncode)
  except Exception as error:codes.append(None);errors.append(dict(stage='rule_check',error_type=type(error).__name__))
 present=route.group.exists()
 return dict(schema=1,run_id=run,scan_complete=not errors,owners=owners,errors=errors,
  rule_check_codes=codes,cgroup_present=present)

def cleanup_launch(root,run,initial,proc=Path('/proc')):
 operations=[];errors=[];dest=root/('pilot-'+run[:8])
 if initial.get('run_id')!=run or initial.get('scan_complete') is not True or initial.get('errors')!=[]:
  raise ValueError('launch_initial_scan_unproven')
 # Do not create a directory that the pending collector must create itself.
 if dest.is_dir() and initial['owners']:
  try:
   with (dest/'stop').open('x'):pass
   operations.append(dict(stage='run_stop',status='CREATED'))
  except FileExistsError:operations.append(dict(stage='run_stop',status='EXISTING'))
  except Exception as error:errors.append(dict(stage='run_stop',error_type=type(error).__name__))
 time.sleep(2)
 def signal_owner(owner,number):
  fd=None
  try:
   fd=os.pidfd_open(owner['pid'],0)
   if launch_owner(root,run,owner['pid'],proc)!=owner:
    raise ValueError('launch_pid_identity_changed_no_signal')
   signal.pidfd_send_signal(fd,number,None,0)
   operations.append(dict(stage='pidfd_signal',pid=owner['pid'],start_ticks=owner['start_ticks'],role=owner['role'],signal=int(number)))
  except (FileNotFoundError,ProcessLookupError):pass
  except Exception as error:errors.append(dict(stage='pidfd_signal',pid=owner['pid'],error_type=type(error).__name__))
  finally:
   if fd is not None:
    try:os.close(fd)
    except Exception as error:errors.append(dict(stage='pidfd_close',pid=owner['pid'],error_type=type(error).__name__))
 # Recheck initial identities: a reused PID is never silently re-authorized.
 for owner in sorted(initial['owners'],key=lambda item:item['role']=='route'):
  signal_owner(owner,signal.SIGTERM)
 deadline=time.monotonic()+6
 while time.monotonic()<deadline:
  owners,scan_errors=launch_scan(root,run,proc);errors.extend(scan_errors)
  if not owners or scan_errors:break
  time.sleep(.2)
 # Include newly spawned children, with a new exact ownership frame and pidfd.
 owners,scan_errors=launch_scan(root,run,proc);errors.extend(scan_errors)
 for owner in sorted(owners,key=lambda item:item['role']=='route'):
  # Initial PID reuse is evidence failure, not permission to stop the replacement.
  prior=[item for item in initial['owners'] if item['pid']==owner['pid']]
  if prior and prior[0]!=owner:
   errors.append(dict(stage='pid_reused',pid=owner['pid'],error_type='IdentityChanged'));continue
  signal_owner(owner,signal.SIGKILL)
 deadline=time.monotonic()+3
 while time.monotonic()<deadline:
  owners,scan_errors=launch_scan(root,run,proc);errors.extend(scan_errors)
  if not owners or scan_errors:break
  time.sleep(.2)
 owners,scan_errors=launch_scan(root,run,proc);errors.extend(scan_errors)
 route=launch_route(root,run)
 safe=not owners and not scan_errors
 if route.group.exists():
  try:
   if int((route.group/'net_cls.classid').read_text().strip(),0)!=route.classid or (route.group/'cgroup.procs').read_text().strip():
    raise ValueError('launch_cgroup_not_empty_or_identity_changed')
  except Exception as error:safe=False;errors.append(dict(stage='cgroup_identity',error_type=type(error).__name__))
 if safe:
  for protocol,port in route.ports:
   try:
    rule=route.rule(protocol,port)
    check=subprocess.run(route.command('-C',rule),capture_output=True,timeout=6).returncode
    if check==0:
     code=subprocess.run(route.command('-D',rule),capture_output=True,timeout=6).returncode
     operations.append(dict(stage='exact_rule_delete',protocol=protocol,returncode=code))
     if code!=0:raise RuntimeError('launch_exact_rule_delete_failed')
    elif check!=1:raise RuntimeError('launch_exact_rule_check_failed')
   except Exception as error:errors.append(dict(stage='exact_rule_delete',protocol=protocol,error_type=type(error).__name__))
  if route.group.exists():
   try:route.group.rmdir();operations.append(dict(stage='exact_cgroup_remove',status='REMOVED'))
   except Exception as error:errors.append(dict(stage='exact_cgroup_remove',error_type=type(error).__name__))
 else:errors.append(dict(stage='route_cleanup',error_type='OwnedResourcesStillPresentOrUnproven'))
 final=inspect_launch(root,run,proc)
 return dict(schema=1,run_id=run,operations=operations,errors=errors,final_observation=final)
'''

LAUNCH_ENTRY = r'''
root=Path(sys.argv[1]);run=sys.argv[2]
if not re.fullmatch(r'/root/[A-Za-z0-9/_-]+',str(root)) or root.resolve()!=root or not re.fullmatch('[a-f0-9]{32}',run):
 raise ValueError('launch_cleanup_identity_invalid')
'''
LAUNCH_INSPECT_SCRIPT = LAUNCH_COMMON + LAUNCH_ENTRY + "print(json.dumps(inspect_launch(root,run)))\n"
LAUNCH_CLEANUP_SCRIPT = LAUNCH_COMMON + LAUNCH_ENTRY + "print(json.dumps(cleanup_launch(root,run,json.loads(sys.argv[3]))))\n"


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def receipt(value, run_id):
    if value.get('run_id') != run_id or value.get('stop_present') is not True or \
            type(value.get('stop_mtime_ns')) is not int or value['stop_mtime_ns'] <= 0:
        raise ValueError('remote_stop_identity_or_receipt_invalid')
    return value


def request(executor, script, arguments, stage, attempts, timeout=15, interpreter='python3'):
    began = time.monotonic()
    item = dict(stage=stage, readonly=stage not in ('request_stop', 'launch_cleanup'), timeout_seconds=timeout)
    try:
        return json.loads(executor(shlex.quote(interpreter) + ' -c ' + shlex.quote(script) + arguments, timeout=timeout))
    except Exception as error:
        item['error_type'] = type(error).__name__
        if isinstance(error, AliyunRemoteCommandError):
            item['transport'] = error.diagnostics
        raise
    finally:
        item['elapsed_seconds'] = time.monotonic() - began
        attempts.append(item)


def stop_and_observe(cfg, run_id, timing, attempts, executor=execute):
    arguments = ' ' + shlex.quote(cfg['remote_root']) + ' ' + shlex.quote(run_id)
    session_limit_failures = 0
    try:
        receipt(request(executor, STOP_SCRIPT, arguments, 'request_stop', attempts), run_id)
    except (RuntimeError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        if isinstance(error, AliyunRemoteCommandError) and error.diagnostics['remote_exit_code'] not in (None, 0):
            raise
        if isinstance(error, AliyunRemoteCommandError):
            session_limit_failures += int(error.diagnostics['service_code'] == 'Forbidden.SessionLimit')
        # The stop command is never reissued. Only its existing receipt is read.
    deadline = time.monotonic() + 45
    failures = 0
    if session_limit_failures:
        time.sleep(.5)
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError('remote_shutdown_receipt_budget_exhausted')
        try:
            state = request(executor, STATE_SCRIPT, arguments + (' 1' if timing else ' 0'),
                            'readonly_state', attempts, min(15, remaining))
            receipt(state, run_id)  # identity or absent receipt is terminal
        except (RuntimeError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
            failures += 1
            rejected = isinstance(error, AliyunRemoteCommandError) and error.diagnostics['remote_exit_code'] not in (None, 0)
            if isinstance(error, AliyunRemoteCommandError):
                session_limit_failures += int(error.diagnostics['service_code'] == 'Forbidden.SessionLimit')
            if rejected or failures >= 3 or session_limit_failures >= 2:
                raise
        else:
            # A terminal reply that arrived outside the budget cannot prove a
            # timely close, even if an executor ignored its supplied timeout.
            if time.monotonic() >= deadline:
                raise RuntimeError('remote_shutdown_receipt_budget_exhausted')
            if state.get('terminal') is True:
                return state
        time.sleep(min(.5, max(0, deadline - time.monotonic())))


def transfer(cfg, remote, local, metadata, attempts, downloader=subprocess.run):
    if (type(metadata.get('size')) is not int or not 0 < metadata['size'] <= 1024**3 or
            not isinstance(metadata.get('sha256'), str) or not re.fullmatch('[a-f0-9]{64}', metadata['sha256'])):
        raise ValueError('remote_evidence_metadata_invalid')
    if local.exists():
        if local.stat().st_size == metadata['size'] and digest(local) == metadata['sha256']:
            return
        raise ValueError('existing_local_evidence_mismatch')
    for number in range(2):
        partial = local.with_name(local.name + '.incoming-' + str(number + 1))
        if partial.exists():
            raise ValueError('new_transfer_evidence_required')
        entry = dict(stage='readonly_download', file=local.name, readonly=True)
        try:
            result = downloader(['workbench', 'download', remote, str(partial), '-i', cfg['instance_id'],
                                 '-r', cfg['region']], capture_output=True, text=True, encoding='utf-8', timeout=90)
            entry.update(cli_returncode=result.returncode, stdout_chars=len(result.stdout), stderr_chars=len(result.stderr))
        except subprocess.TimeoutExpired:
            entry['error_type'] = 'TimeoutExpired'
        verified = partial.exists() and partial.stat().st_size == metadata['size'] and digest(partial) == metadata['sha256']
        entry['hash_verified'] = verified
        attempts.append(entry)
        if verified:
            partial.replace(local)
            return
    raise RuntimeError('remote_evidence_transfer_unproven')


def final_complete(state, run_id):
    route, cleanup, last = state.get('route') or {}, state.get('actual_cleanup') or {}, state.get('last_event') or {}
    return (last.get('run_id') == route.get('run_id') == run_id and last.get('event') == 'collector.stopped'
            and last.get('status') == route.get('status') == 'COMPLETE'
            and type(route.get('child_exit_code')) is int and route['child_exit_code'] == 0
            and route.get('cleanup_complete') is True and cleanup.get('rule_check_codes') == [1, 1]
            and all(type(c) is int for c in cleanup['rule_check_codes']) and cleanup.get('cgroup_present') is False)


def launch_observation_valid(state, run_id):
    return (type(state.get('schema')) is int and state['schema'] == 1 and state.get('run_id') == run_id and
            state.get('scan_complete') is True and state.get('errors') == [] and
            isinstance(state.get('owners'), list) and len(state['owners']) <= 64 and
            all(isinstance(owner, dict) and set(owner) == {'pid', 'start_ticks', 'role'} and
                type(owner['pid']) is int and owner['pid'] > 0 and type(owner['start_ticks']) is int and
                owner['start_ticks'] > 0 and owner['role'] in ('remote', 'load', 'route') for owner in state['owners']) and
            isinstance(state.get('rule_check_codes'), list) and len(state['rule_check_codes']) == 2 and
            all(type(code) is int and code in (0, 1) for code in state['rule_check_codes']) and
            type(state.get('cgroup_present')) is bool)


def launch_absent(state, run_id):
    return (launch_observation_valid(state, run_id) and state['owners'] == [] and
            state['rule_check_codes'] == [1, 1] and state['cgroup_present'] is False)


def recover_failed_launch(root, cfg, run_id, executor=execute):
    if (root / 'launch-cleanup.json').exists():
        raise ValueError('launch_cleanup_already_recorded')
    proof = dict(schema=1, run_id=run_id, status='FAILED', scope='launch cleanup only; original run stays FAILED', attempts=[])
    arguments = ' ' + shlex.quote(cfg['remote_root']) + ' ' + shlex.quote(run_id)
    # pidfd APIs require Python >=3.9. Use the same validated runtime as the
    # launched collector; the instance's system python3 can be older.
    interpreter = cfg['remote_root'] + '/venv/bin/python'
    try:
        initial = request(executor, LAUNCH_INSPECT_SCRIPT, arguments, 'launch_readonly_initial', proof['attempts'], 20, interpreter)
        proof['initial_observation'] = initial
        if not launch_observation_valid(initial, run_id):
            raise ValueError('launch_initial_scan_unproven')
        # Missing ready is not absence. Catch a launch arriving after the first scan.
        if launch_absent(initial, run_id):
            initial = request(executor, LAUNCH_INSPECT_SCRIPT, arguments, 'launch_readonly_confirm', proof['attempts'], 20, interpreter)
            proof['confirmation_observation'] = initial
            if not launch_observation_valid(initial, run_id):
                raise ValueError('launch_confirmation_scan_unproven')
        cleanup_failed = False
        if not launch_absent(initial, run_id):
            try:
                proof['cleanup_request'] = request(executor, LAUNCH_CLEANUP_SCRIPT,
                    arguments + ' ' + shlex.quote(json.dumps(initial)), 'launch_cleanup', proof['attempts'], 60, interpreter)
            except Exception as error:
                proof['cleanup_request_error_type'] = type(error).__name__
                cleanup_failed = True
        # A lost cleanup reply never prevents the last read-only residual query.
        final = request(executor, LAUNCH_INSPECT_SCRIPT, arguments, 'launch_readonly_final', proof['attempts'], 20, interpreter)
        proof['final_observation'] = final
        cleanup = proof.get('cleanup_request', {})
        if (cleanup_failed or not launch_absent(final, run_id) or
                (cleanup and (cleanup.get('schema') != 1 or cleanup.get('run_id') != run_id or
                              cleanup.get('errors') != [] or not launch_absent(cleanup.get('final_observation', {}), run_id)))):
            raise RuntimeError('launch_cleanup_or_actual_absence_unproven')
        proof['status'] = 'COMPLETE'
    except Exception as error:
        proof['error_type'] = type(error).__name__
    finally:
        proof['finished_utc'] = datetime.now(timezone.utc).isoformat()
        with (root / 'launch-cleanup.json').open('x', encoding='utf-8') as stream:
            json.dump(proof, stream, indent=2); stream.write('\n')
    return proof


def finish(root, run_id, timing=False, launch_attempted=False, executor=execute, downloader=subprocess.run):
    if (root / 'remote-shutdown.json').exists() and not launch_attempted:
        raise ValueError('shutdown_already_recorded')
    if not re.fullmatch('[a-f0-9]{32}', run_id):
        raise ValueError('invalid_shutdown_run_id')
    cfg = target()
    if json.loads((root / 'plan.json').read_text(encoding='utf-8-sig'))['run_id'] != run_id:
        raise ValueError('shutdown_plan_mismatch')
    if (root / 'remote-shutdown.json').exists():
        proof = json.loads((root / 'remote-shutdown.json').read_text(encoding='utf-8'))
        if proof.get('run_id') != run_id or proof.get('status') not in ('FAILED', 'COMPLETE'):
            raise ValueError('existing_shutdown_identity_invalid')
        if proof['status'] == 'FAILED':
            recover_failed_launch(root, cfg, run_id, executor)
        return proof
    proof = dict(schema=1, run_id=run_id, status='FAILED', attempts=[])
    try:
        state = stop_and_observe(cfg, run_id, timing, proof['attempts'], executor)
        proof['observed_terminal'] = state
        mappings = {'remote.jsonl': 'remote.jsonl'}
        if timing:
            mappings.update({'timing.jsonl': 'timing.jsonl', 'load-process/cadence.jsonl': 'publisher-cadence.jsonl'})
        for remote_name, local_name in mappings.items():
            transfer(cfg, cfg['remote_root'] + '/pilot-' + run_id[:8] + '/' + remote_name,
                     root / local_name, state['files'][remote_name], proof['attempts'], downloader)
        with (root / 'server-route.json').open('x', encoding='utf-8') as stream:
            json.dump(state['route'], stream, indent=2); stream.write('\n')
        if not final_complete(state, run_id):
            raise RuntimeError('remote_shutdown_or_actual_cleanup_failed')
        proof['status'] = 'COMPLETE'
    except Exception as error:
        proof['error_type'] = type(error).__name__
    finally:
        proof['finished_utc'] = datetime.now(timezone.utc).isoformat()
        try:
            with (root / 'remote-shutdown.json').open('x', encoding='utf-8') as stream:
                json.dump(proof, stream, indent=2); stream.write('\n')
        except Exception as error:
            # Persistence is evidence, not a prerequisite for compensating an
            # already submitted launch. Keep its failure visible to the caller.
            proof['receipt_error_type'] = type(error).__name__
            proof.setdefault('error_type', type(error).__name__)
            proof['status'] = 'FAILED'
    if launch_attempted and proof['status'] == 'FAILED':
        try:
            proof['launch_cleanup_status'] = recover_failed_launch(root, cfg, run_id, executor)['status']
        except Exception as error:
            proof['launch_cleanup_error_type'] = type(error).__name__
    return proof


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--timing', action='store_true')
    parser.add_argument('--launch-attempted', action='store_true')
    args = parser.parse_args()
    try:
        result = finish(args.root, args.run_id, args.timing, args.launch_attempted)
        print(json.dumps({key: result.get(key) for key in ('run_id', 'status', 'error_type',
            'receipt_error_type', 'launch_cleanup_status', 'launch_cleanup_error_type')}))
        raise SystemExit(0 if result['status'] == 'COMPLETE' else 1)
    except Exception as error:
        print(json.dumps(dict(status='FAILED', error_type=type(error).__name__)))
        raise SystemExit(1)
