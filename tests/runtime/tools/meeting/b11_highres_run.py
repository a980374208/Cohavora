"""Task-owned native high-resolution publisher orchestration over pinned SSH."""
from __future__ import annotations

import argparse
import hashlib
import inspect
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit
import uuid

import b11_hd_remote_run as shared
import b11_input_freeze as base
import b11_remote as remote
import meeting_soak as soak

import b11_highres_input_freeze as freeze

SCOPE = freeze.SCOPE
ENVIRONMENT = shared.ENVIRONMENT


def process_identity(pid, proc_root=Path("/proc")):
    """The remote owner includes user and namespace in addition to PID/start/argv."""
    try:
        directory = proc_root / str(pid)
        fields = (directory / "stat").read_text().rsplit(")", 1)[1].split()
        if fields[0] == "Z":
            return None
        return {"pid": int(pid), "start_ticks": fields[19],
            "argv": [value.decode("utf-8") for value in
                (directory / "cmdline").read_bytes().rstrip(b"\0").split(b"\0")],
            "uid": directory.stat().st_uid,
            "namespace_inode": (directory / "ns/net").stat().st_ino}
    except (FileNotFoundError, ProcessLookupError):
        return None


def stop_owned(owner):
    result = {"pid": owner["pid"], "stopped": False}
    for sig, grace in ((signal.SIGINT, 3), (signal.SIGTERM, 5)):
        current = process_identity(owner["pid"])
        if current is None or current["start_ticks"] != owner["start_ticks"]:
            result["stopped"] = True
            return result
        if any(current[name] != owner[name] for name in ("argv", "uid", "namespace_inode")):
            result["reason"] = "highres_process_ownership_changed"
            return result
        try:
            os.kill(owner["pid"], sig)
        except ProcessLookupError:
            result["stopped"] = True
            return result
        deadline = time.monotonic() + grace
        while time.monotonic() < deadline:
            current = process_identity(owner["pid"])
            if current is None or current["start_ticks"] != owner["start_ticks"]:
                result["stopped"] = True
                return result
            if any(current[name] != owner[name] for name in ("argv", "uid", "namespace_inode")):
                result["reason"] = "highres_process_ownership_changed"
                return result
            time.sleep(.1)
    result["reason"] = "highres_process_stop_timeout"
    return result


REMOTE_PROGRAM = r'''
root = Path(PAYLOAD['remote_directory'])
target = PAYLOAD['target']
task_id, room = PAYLOAD['task']['task_id'], PAYLOAD['task']['room']
marker = root / 'owner.json'

def atomic(path, value):
    temporary = path.with_suffix(path.suffix + '.new')
    with temporary.open('x') as stream:
        json.dump(value, stream)
    temporary.chmod(0o600)
    temporary.replace(path)

def owner():
    if root.is_symlink() or root.stat().st_uid != os.getuid() or root.stat().st_mode & 0o077 or \
            marker.is_symlink() or marker.stat().st_uid != os.getuid() or marker.stat().st_mode & 0o077:
        raise RuntimeError('highres_task_owner_untrusted')
    value = json.loads(marker.read_text())
    if value.get('scope') != SCOPE or value.get('task_id') != task_id or value.get('room') != room or \
            value.get('remote_directory') != str(root):
        raise RuntimeError('highres_task_owner_mismatch')
    return value

def prerequisite():
    try:
        data = Path(target['config_path']).read_bytes()
    except PermissionError:
        data = subprocess.check_output(['sudo', '-n', 'cat', target['config_path']], timeout=15,
            stderr=subprocess.DEVNULL)
    inspected = json.loads(subprocess.check_output(['sudo', '-n', 'docker', 'inspect',
        target['sfu_container']], text=True, timeout=15, stderr=subprocess.DEVNULL))[0]
    observed = {'config_sha256': hashlib.sha256(data).hexdigest(), 'sfu_image': inspected['Image'],
        'sfu_pid': int(inspected['State']['Pid']), 'logical_cpus_observed': os.cpu_count(),
        'mem_total_kib_observed': int(next(line.split()[1] for line in
            Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemTotal:')))}
    if not inspected['State']['Running'] or observed['sfu_pid'] <= 0:
        raise RuntimeError('highres_sfu_not_running')
    expected = PAYLOAD.get('expected_remote')
    if expected is not None and any(observed.get(name) != value for name, value in expected.items()):
        raise RuntimeError('highres_remote_prerequisite_changed')
    import yaml
    config = yaml.safe_load(data)
    rtc = config.get('rtc', {}) if type(config) is dict else {}
    if type(rtc) is not dict or rtc.get('node_ip') != urlsplit(target['service_url']).hostname or \
            any(type(value) is not int or value != expected for value, expected in
                ((config.get('port'),17880),(rtc.get('tcp_port'),17881),(rtc.get('udp_port'),17882))):
        raise RuntimeError('highres_sfu_endpoint_changed')
    try:
        key, secret = next(iter(config['keys'].items()))
        if type(key) is not str or type(secret) is not str or not key or not secret:
            raise ValueError
    except (KeyError, StopIteration, TypeError, ValueError, AttributeError):
        raise RuntimeError('highres_sfu_credentials_unavailable') from None
    return key, secret, observed

def jwt(key, secret, identity, grants):
    now = int(time.time())
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value,separators=(',',':')).encode()).rstrip(b'=')
    data = encode({'alg':'HS256','typ':'JWT'}) + b'.' + encode(
        {'iss':key,'sub':identity,'nbf':now-5,'exp':now+900,'video':grants})
    return (data+b'.'+base64.urlsafe_b64encode(hmac.new(secret.encode(),data,hashlib.sha256).digest()).rstrip(b'=')).decode()

def api(method, body, key, secret):
    signed = jwt(key, secret, 'b11-highres-admin-' + task_id[:12], {'roomAdmin':True,'roomCreate':True,'roomList':True,'room':room})
    query = urllib.request.Request(target['local_service_url'] + '/twirp/livekit.RoomService/' + method,
        data=json.dumps(body).encode(), headers={'Content-Type':'application/json','Authorization':'Bearer '+signed})
    with urllib.request.urlopen(query, timeout=15) as response:
        return json.load(response)

def owned_room(key, secret):
    listed = api('ListRooms', {'names':[room]}, key, secret).get('rooms', [])
    if not listed:
        return None
    if len(listed) != 1 or listed[0].get('name') != room:
        raise RuntimeError('highres_room_identity_changed')
    try:
        metadata = json.loads(listed[0].get('metadata',''))
    except (TypeError, ValueError):
        raise RuntimeError('highres_room_owner_unknown') from None
    if metadata != {'scope':SCOPE,'task_id':task_id}:
        raise RuntimeError('highres_room_owner_mismatch')
    return listed[0]

def safe_hash(value):
    return hashlib.sha256(str(value).encode()).hexdigest()[:16]

try:
    operation = PAYLOAD['operation']
    if operation == 'prepare':
        key, secret, observed = prerequisite()
        root.mkdir(mode=0o700, exist_ok=False)
        metadata = {'schema':1,'scope':SCOPE,'task_id':task_id,'room':room,'remote_directory':str(root),
            'publishers':[],'launch_pending':False,'remote_inputs':observed,'state':'PREPARING'}
        atomic(marker,metadata)
        if api('ListRooms',{'names':[room]},key,secret).get('rooms',[]):
            raise RuntimeError('highres_room_already_exists')
        created = api('CreateRoom',{'name':room,'empty_timeout':900,
            'metadata':json.dumps({'scope':SCOPE,'task_id':task_id},separators=(',',':'))},key,secret)
        if created.get('name') != room or owned_room(key,secret) is None:
            raise RuntimeError('highres_room_creation_not_verified')
        metadata['state']='PREPARED'
        atomic(marker,metadata)
        credentials = {}
        for role, identity in (('publisher',PAYLOAD['target_identity']),('observer','b11-highres-observer-'+task_id[:12])):
            # Coordinator startup publishes muted audio/video even when devices
            # are disabled; muting does not remove the publication permission need.
            credentials[role]={'LIVEKIT_URL':target['service_url'],'LIVEKIT_SOAK_ALLOW_INSECURE':'1',
                'LIVEKIT_SOAK_TOKEN':jwt(key,secret,identity,{'roomJoin':True,'room':room,
                    'canPublish':True,'canSubscribe':role=='observer','canPublishData':False})}
        atomic(root/'observer.json',credentials)
        response={'ok':True,'remote_inputs':observed,'credential_ready':True}
    elif operation == 'remove_credentials':
        owner()
        (root/'observer.json').unlink(missing_ok=True)
        response={'ok':True,'credential_removed':not (root/'observer.json').exists()}
    elif operation == 'background':
        key, secret, observed = prerequisite()
        metadata=owner()
        if metadata['publishers'] or metadata.get('launch_pending') or metadata['state']!='PREPARED':
            raise RuntimeError('highres_background_not_prepared')
        background=PAYLOAD['background']
        if hashlib.sha256(Path(background['cli_path']).read_bytes()).hexdigest()!=background['cli_sha256']:
            raise RuntimeError('highres_background_cli_changed')
        if owned_room(key,secret) is None:
            raise RuntimeError('highres_owned_room_missing')
        environment=os.environ.copy()
        environment.update(LIVEKIT_URL=target['local_service_url'],LIVEKIT_API_KEY=key,LIVEKIT_API_SECRET=secret)
        child=None
        try:
            # Persist launch intent first: an owner-capture/write failure must not
            # make a still-running child disappear from cleanup evidence.
            metadata['launch_pending']=True
            atomic(marker,metadata)
            child=subprocess.Popen(background['argv'],env=environment,stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
            captured=process_identity(child.pid)
            if captured is None or captured['argv']!=background['argv'] or captured['uid']!=os.getuid() or \
                    captured['namespace_inode']!=Path('/proc/self/ns/net').stat().st_ino:
                raise RuntimeError('highres_background_owner_unavailable')
            metadata['publishers'].append(captured)
            metadata['launch_pending']=False
            atomic(marker,metadata)
            time.sleep(1)
            if child.poll() is not None:
                raise RuntimeError('highres_background_exited_early')
            response={'ok':True,'publishers':metadata['publishers'],'remote_inputs':observed}
        except BaseException:
            if child is not None and child.poll() is None:
                child.send_signal(signal.SIGINT)
                try: child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.terminate()
                    try: child.wait(timeout=5)
                    except subprocess.TimeoutExpired: pass
            metadata['launch_pending']=child is not None and child.poll() is None
            metadata['state']='FAILED'
            try: atomic(marker,metadata)
            except Exception: pass
            raise
    elif operation == 'snapshot':
        key, secret, observed=prerequisite()
        owner()
        if owned_room(key,secret) is None:
            raise RuntimeError('highres_owned_room_missing')
        participants=api('ListParticipants',{'room':room},key,secret).get('participants',[])
        target_participants=[value for value in participants if value.get('identity')==PAYLOAD['target_identity']]
        if len(target_participants)!=1:
            raise RuntimeError('highres_target_participant_not_unique')
        tracks=[value for value in target_participants[0].get('tracks',[]) if value.get('type') in ('VIDEO',1)]
        safe=[]
        for track in tracks:
            source=track.get('source')
            source={'CAMERA':'camera','SCREEN_SHARE':'screen_share',1:'camera',3:'screen_share'}.get(source,'unknown')
            layers=[]
            for layer in track.get('layers',[]):
                quality={'LOW':0,'MEDIUM':1,'HIGH':2,0:0,1:1,2:2}.get(layer.get('quality',0),-1)
                layers.append({'quality':quality,**{name:layer.get(name,0) for name in ('width','height','bitrate')}})
            safe.append({'sid_hash':safe_hash(track.get('sid','')),'source':source,
                'width':track.get('width',0),'height':track.get('height',0),'layers':layers})
        response={'ok':True,'remote_inputs':observed,'target_identity_hash':safe_hash(PAYLOAD['target_identity']),
            'participant_count':len(participants),'video_track_count':sum(1 for p in participants for t in p.get('tracks',[])
                if t.get('type') in ('VIDEO',1)),'target_track_count':len(tracks),'tracks':safe}
    elif operation in ('room_delete','cleanup'):
        metadata=owner()
        stopped=[]
        if operation=='cleanup':
            stopped=[stop_owned(value) for value in reversed(metadata['publishers'])]
        (root/'observer.json').unlink(missing_ok=True)
        try:
            key,secret,observed=prerequisite()
            value=owned_room(key,secret)
            if value is not None:
                api('DeleteRoom',{'room':room},key,secret)
            removed=owned_room(key,secret) is None
            prerequisite_status='MATCH'
        except Exception:
            observed,removed,prerequisite_status=None,False,'UNKNOWN'
        response={'ok':True,'room_removed':removed,'credential_removed':not (root/'observer.json').exists(),
            'prerequisite_status':prerequisite_status,'remote_inputs':observed}
        if operation=='cleanup':
            response.update(all_stopped=not metadata.get('launch_pending',False) and
                all(value['stopped'] for value in stopped),owners=stopped)
            metadata['state']='CLEANED' if response['all_stopped'] and removed and response['credential_removed'] else 'UNKNOWN'
            atomic(marker,metadata)
    else:
        raise RuntimeError('unsupported_highres_remote_operation')
except BaseException as error:
    message=str(error)
    response={'ok':False,'reason':message if re.fullmatch(r'[a-z][a-z0-9_]{0,127}',message) else type(error).__name__}
print(json.dumps(response))
'''


def _validate_session(session):
    if session.get('scope', SCOPE) not in (SCOPE, freeze.FOUR_K_SCOPE):
        raise ValueError('invalid_highres_task_scope')
    task = session['task']
    if not re.fullmatch(r'[0-9a-f]{32}', task['task_id']) or \
            task['room'] != 'b11-highres-' + task['task_id'] or \
            session['remote_directory'] != '/tmp/' + task['room'] or \
            not re.fullmatch(r'b11highres_[A-Za-z0-9_]{1,48}', session['target_identity']):
        raise ValueError('invalid_highres_task_binding')
    target, identity = session['target'], session['identity']
    base.validate_service_url(target['service_url'])
    base.validate_service_url(target['local_service_url'], local=True)
    origin = urlsplit(target['service_url'])
    address = ipaddress.ip_address(identity['host'])
    if address.version != 4 or not address.is_global or identity['user'] != 'ubuntu' or \
            origin.hostname != str(address) or origin.port != 17880 or \
            target['local_service_url'] != 'http://127.0.0.1:17880' or \
            not re.fullmatch(r'/[A-Za-z0-9_./-]+', target['config_path']) or \
            '..' in Path(target['config_path']).parts or \
            not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', target['sfu_container']):
        raise ValueError('highres_target_mismatch')


def remote_command(operation, session, *, background=None):
    _validate_session(session)
    if operation not in ('prepare','remove_credentials','background','snapshot','room_delete','cleanup'):
        raise ValueError('unsupported_highres_remote_operation')
    payload = {name: session[name] for name in ('remote_directory','target','task','target_identity')}
    payload.update(operation=operation, expected_remote=session.get('expected_remote'))
    if background is not None:
        path, digest = background['cli_path'], background['cli_sha256']
        if not re.fullmatch(r'/[A-Za-z0-9_./-]+', path) or '..' in Path(path).parts or \
                not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise ValueError('invalid_highres_background_source')
        group = {'duration':'10m','count':14,'subscribers':0,'resolution':'low','codec':'vp8',
            'num_per_second':5,'identity_prefix':'b11hi_bg_' + session['task']['task_id'][:16],'simulcast':True}
        payload['background'] = {'cli_path':path,'cli_sha256':digest,
            'argv':shared.publisher_argv(path, session['task']['room'], group)}
    program = 'import base64,hashlib,hmac,json,os,re,signal,subprocess,time,urllib.request\nfrom pathlib import Path\nfrom urllib.parse import urlsplit\n'
    program += 'SCOPE = ' + repr(session.get('scope', SCOPE)) + '\n'
    program += inspect.getsource(process_identity) + '\n' + inspect.getsource(stop_owned) + '\n'
    program += 'PAYLOAD = ' + repr(payload) + '\n' + REMOTE_PROGRAM
    return ("python3 - <<'B11_HIGHRES_PY'\n" + program + '\nB11_HIGHRES_PY\n').encode()


def call(operation, session, *, background=None):
    response = remote.execute(shared._pinned_config(session['identity']),
        remote_command(operation, session, background=background))
    value = json.loads(response['stdout'])
    if value.get('ok') is not True:
        raise RuntimeError(value.get('reason','highres_remote_operation_failed'))
    return value


def remove_credentials(session):
    value = call('remove_credentials',session)
    if value.get('credential_removed') is not True:
        raise RuntimeError('highres_remote_credential_cleanup_failed')
    return value


def _read_credentials(path, service_url):
    value = base._load_json(path)
    if type(value) is not dict or set(value) != {'publisher','observer'}:
        raise ValueError('invalid_highres_credentials')
    for role in value.values():
        if type(role) is not dict or set(role) != set(ENVIRONMENT) or \
                role.get('LIVEKIT_URL') != service_url or role.get('LIVEKIT_SOAK_ALLOW_INSECURE') != '1' or \
                any(type(text) is not str or not text or any(ord(char) < 32 for char in text) for text in role.values()) or \
                not re.fullmatch(r'[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+',role['LIVEKIT_SOAK_TOKEN']):
            raise ValueError('invalid_highres_credentials')
    return value


def prepare_credentials(target_config, target, *, task_id=None, room=None,
                        target_identity=None, output, expected_remote=None, scenario='camera'):
    """Return credentials only in memory; caller must never serialize the session."""
    task_id = task_id or uuid.uuid4().hex
    room = room or 'b11-highres-' + task_id
    target_identity = target_identity or 'b11highres_' + task_id[:16] + '_source'
    identity = {'config_path':str(Path(target_config).resolve()),**remote.snapshot_identity(Path(target_config))}
    session = {'scope':freeze.scenario_scope(scenario),'identity':identity,'target':dict(target),'task':{'task_id':task_id,'room':room},
        'target_identity':target_identity,'remote_directory':'/tmp/'+room,'expected_remote':expected_remote}
    _validate_session(session)
    evidence = Path(output)
    evidence.mkdir(parents=True,exist_ok=False)
    try:
        prepared = call('prepare',session)
        session['remote_inputs'] = prepared['remote_inputs']
        session['expected_remote'] = {name:prepared['remote_inputs'][name] for name in ('config_sha256','sfu_image')}
        soak.atomic_json(evidence/'remote-prepared.json',prepared)
        with tempfile.TemporaryDirectory(prefix='b11-highres-credentials-',dir=evidence) as temporary:
            credential_path=Path(temporary)/'observer.json'
            try:
                remote.download(shared._pinned_config(identity),session['remote_directory']+'/observer.json',credential_path)
                session['credentials']=_read_credentials(credential_path,target['service_url'])
            finally:
                credential_path.unlink(missing_ok=True)
                remove_credentials(session)
        return session
    except BaseException:
        try:
            soak.atomic_json(evidence/'prepare-cleanup.json',call('cleanup',session))
        except Exception:
            soak.atomic_json(evidence/'prepare-cleanup.json',{'ok':False,'cleanup_status':'UNKNOWN'})
        raise


def start_background(session, background):
    return call('background',session,background=background)


def server_snapshot(session):
    return call('snapshot',session)


def admin_room_delete(session):
    return call('room_delete',session)


def session_cleanup(session):
    return call('cleanup',session)


def native_publisher_environment(session, *, scenario, ready_file, stop_file, maximum_seconds=600):
    if scenario not in freeze.SOURCES or type(maximum_seconds) is not int or not 1 <= maximum_seconds <= 600:
        raise ValueError('invalid_highres_publisher_scenario')
    kind, description = freeze.scenario_kind(scenario), freeze.SOURCES[scenario]
    credentials=session['credentials']['publisher']
    environment=os.environ.copy()
    environment.update(LIVEKIT_URL=credentials['LIVEKIT_URL'],LIVEKIT_TOKEN=credentials['LIVEKIT_SOAK_TOKEN'],
        LIVEKIT_TEST_ALLOW_INSECURE='1',POLICY_SOURCE_KIND=kind,
        POLICY_SOURCE_WIDTH=str(description['width']),POLICY_SOURCE_HEIGHT=str(description['height']),
        POLICY_SOURCE_CAPTURE='wgc_window' if kind=='screen' or scenario.endswith('4k') else 'synthetic_i420',
        POLICY_SOURCE_READY_FILE=str(Path(ready_file).resolve()),
        POLICY_SOURCE_STOP_FILE=str(Path(stop_file).resolve()),POLICY_SOURCE_MAX_SECONDS=str(maximum_seconds))
    if kind=='screen' or scenario.endswith('4k'):
        environment['LIVEKIT_TEST_CAPTURE_BACKEND']='wgc-window'
        for name in ('LIVEKIT_TEST_WGC_SCREEN','LIVEKIT_TEST_GDI_WINDOW','LIVEKIT_TEST_FIRST_FRAME_ONLY'):
            environment.pop(name,None)
    return environment


def _export_publisher_probes(raw_path, output_path):
    """Retain only structured, safe publisher measurements, then remove raw output."""
    try:
        with raw_path.open('r',encoding='utf-8',errors='replace') as source, output_path.open('x',encoding='utf-8') as output:
            for line in source:
                if not line.startswith('SOURCE_PROBE '):
                    continue
                value=json.loads(line.removeprefix('SOURCE_PROBE '))
                remote._safe_metadata(value)
                output.write(json.dumps(value,allow_nan=False)+'\n')
    finally:
        raw_path.unlink(missing_ok=True)


def run_session(output, publisher_executable, *, target_config, target, scenario,
                observer, background=None, expected_remote=None, publisher_ready_timeout=90):
    """Observer callback freezes verified source inputs before starting its receiver.

    callback(output, session, publisher_ready, server_snapshot) receives no raw
    credentials in its evidence arguments; session credentials remain memory-only.
    """
    output=Path(output).resolve()
    if output.exists():
        raise ValueError('highres_output_already_exists')
    output.mkdir(parents=True,exist_ok=False)
    result={'schema':1,'scope':freeze.scenario_scope(scenario),'status':'INCONCLUSIVE','reason':'not_started',
        'formal_b11_status':'NOT_RUN','formal_soak_status':'NOT_RUN','diagnostic_only':True,
        'release_eligible':False,'cleanup_status':'UNKNOWN','cleanup_issues':[]}
    session=process=publisher_stdout=None
    prepare_started=False
    original={name:os.environ.get(name) for name in ENVIRONMENT}
    ready_file,stop_file=output/'publisher-ready.json',output/'publisher-stop.flag'
    raw_publisher_stdout=output/'.publisher-stdout.tmp'
    try:
        prepare_started=True
        session=prepare_credentials(target_config,target,output=output/'prepared',expected_remote=expected_remote,
            scenario=scenario)
        environment=native_publisher_environment(session,scenario=scenario,ready_file=ready_file,stop_file=stop_file)
        publisher_stdout=raw_publisher_stdout.open('xb')
        process=subprocess.Popen([str(Path(publisher_executable).resolve()),'--policy-source-publisher'],
            cwd=soak.ROOT,env=environment,stdin=subprocess.DEVNULL,stdout=publisher_stdout,
            stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        del environment
        deadline=time.monotonic()+publisher_ready_timeout
        while time.monotonic()<deadline:
            if process.poll() is not None:
                raise RuntimeError('highres_native_publisher_exited_early')
            if ready_file.is_file():
                ready=base._load_json(ready_file)
                if ready.get('event')=='ready':
                    break
            time.sleep(.25)
        else:
            raise RuntimeError('highres_native_publisher_ready_timeout')
        expected_identity=hashlib.sha256(session['target_identity'].encode()).hexdigest()[:16]
        description,kind=freeze.SOURCES[scenario],freeze.scenario_kind(scenario)
        if ready.get('schema')!=1 or ready.get('source_kind')!=kind or \
                ready.get('identity_hash')!=expected_identity or ready.get('source_width')!=description['width'] or \
                ready.get('source_height')!=description['height'] or ready.get('target_fps')!=description['source_fps'] or \
                ready.get('capture_backend')!=('synthetic_i420' if scenario=='camera' else 'wgc_window'):
            raise RuntimeError('highres_native_source_identity_mismatch')
        if scenario.endswith('4k'):
            freeze._validate_four_k_ready(ready, {'scenario':scenario,'source':description,
                'target_identity':session['target_identity']}, ready.get('publication_sid_hash'))
        # The source plus fourteen background videos fits every remote seat of
        # grid16's first page alongside the observer's local participant seat.
        if background:
            soak.atomic_json(output/'background-started.json',start_background(session,background))
        snapshot=server_snapshot(session)
        if snapshot['target_track_count']!=1 or snapshot['tracks'][0]['sid_hash']!=ready['publication_sid_hash'] or \
                snapshot['target_identity_hash']!=expected_identity:
            raise RuntimeError('highres_server_source_identity_mismatch')
        soak.atomic_json(output/'server-source-snapshot.json',snapshot)
        for name,value in session['credentials']['observer'].items():
            os.environ[name]=value
        observed=observer(output/'observer',session,ready,snapshot)
        result.update(status=observed['status'],reason=observed.get('reason','observer_completed'))
        final_snapshot=server_snapshot(session)
        soak.atomic_json(output/'server-final-snapshot.json',final_snapshot)
        if final_snapshot['target_track_count']!=1 or final_snapshot['tracks'][0]['sid_hash']!=ready['publication_sid_hash']:
            result.update(status='FAIL',reason='highres_target_publication_changed')
    except (OSError,ValueError,RuntimeError) as error:
        result['reason']=shared._safe_failure_reason(error)
        if result['status']!='FAIL':
            result['status']='INCONCLUSIVE'
    finally:
        for name,value in original.items():
            if value is None: os.environ.pop(name,None)
            else: os.environ[name]=value
        if process is not None:
            try:
                stop_file.write_text('stop\n',encoding='ascii')
                process.wait(timeout=30)
            except (OSError,subprocess.TimeoutExpired):
                try:
                    process.terminate()
                    process.wait(timeout=10)
                except (OSError,subprocess.TimeoutExpired):
                    result['cleanup_issues'].append('highres_native_publisher_cleanup_unknown')
                result['cleanup_issues'].append('highres_native_publisher_forced_termination')
            result['publisher_exit_code']=process.returncode
            if process.returncode!=0:
                result['cleanup_issues'].append('highres_native_publisher_exit_not_clean')
        if publisher_stdout is not None:
            publisher_stdout.close()
            try:
                _export_publisher_probes(raw_publisher_stdout,output/'publisher-probe.jsonl')
            except (OSError,ValueError):
                result['cleanup_issues'].append('highres_publisher_probe_export_failed')
        if session is not None:
            session.pop('credentials',None)
            try:
                cleanup=session_cleanup(session)
                soak.atomic_json(output/'remote-cleanup.json',cleanup)
                if not all(cleanup.get(name) is True for name in ('all_stopped','room_removed','credential_removed')):
                    result['cleanup_issues'].append('highres_remote_cleanup_incomplete')
            except (OSError,ValueError,RuntimeError):
                result['cleanup_issues'].append('highres_remote_cleanup_unknown')
        elif prepare_started:
            try:
                cleanup=base._load_json(output/'prepared'/'prepare-cleanup.json')
                if not all(cleanup.get(name) is True for name in ('all_stopped','room_removed','credential_removed')):
                    raise ValueError('prepare_cleanup_incomplete')
            except (OSError,ValueError):
                result['cleanup_issues'].append('highres_prepare_cleanup_unknown')
        result['cleanup_status']='COMPLETE' if not result['cleanup_issues'] else 'UNKNOWN'
        if result['cleanup_issues'] and result['status']=='PASS':
            result.update(status='INCONCLUSIVE',reason='highres_cleanup_incomplete')
        soak.atomic_json(output/'runner-summary.json',result)
    return result
