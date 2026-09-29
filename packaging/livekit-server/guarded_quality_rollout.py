"""Run on the ECS host only. No credentials or room identities leave the host."""
import base64
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = Path('/root/livekit-quality-candidate-20260929')
STATE = ROOT / 'rollout-v7-retry2.json'
BACKUP = 'livekit-before-quality-20260929-v7-retry2'
FAILED = 'livekit-quality-failed-20260929-v7-retry2'
EXPECTED_BASE = 'sha256:caa93c01c9b18218eb84581f56721cea73fe5530d200066624ecd7e2e081bfc6'
EXPECTED_BINARY = '8d9e645d2854e76298fc4110ed0f5149180f8e3a8c58777763d5956064f7c014'


def docker(*args):
    result = subprocess.run(['docker', *args], capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError('docker_' + args[0] + '_failed')
    return result.stdout.strip()


def inspect(name):
    result = subprocess.run(['docker', 'inspect', name], capture_output=True, text=True)
    return json.loads(result.stdout)[0] if result.returncode == 0 else None


def save(path, value):
    tmp = path.with_suffix('.tmp')
    with open(tmp, 'w', opener=lambda p, flags: os.open(p, flags, 0o600)) as stream:
        json.dump(value, stream, indent=2)
    os.replace(tmp, path)


def room_count():
    import yaml
    config = yaml.safe_load(Path('/root/livekit.yaml').read_text())
    key, secret = next(iter(config['keys'].items()))
    encode = lambda value: base64.urlsafe_b64encode(json.dumps(value).encode()).decode().rstrip('=')
    body = encode({'alg': 'HS256', 'typ': 'JWT'}) + '.' + encode({
        'iss': key, 'sub': 'quality-maintenance', 'nbf': int(time.time()) - 5,
        'exp': int(time.time()) + 60, 'video': {'roomList': True}})
    token = body + '.' + base64.urlsafe_b64encode(hmac.new(secret.encode(), body.encode(), hashlib.sha256).digest()).decode().rstrip('=')
    request = urllib.request.Request('http://127.0.0.1:17880/twirp/livekit.RoomService/ListRooms',
        data=b'{}', headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=10) as response:
        rooms = json.load(response).get('rooms', [])
    return sum(int(room.get('numParticipants', room.get('num_participants', 0))) for room in rooms)


def rollback():
    state = json.loads(STATE.read_text())
    if state['phase'] in ['accepted', 'rolled_back']:
        return
    old = inspect(BACKUP)
    if old is None:
        # A failed pre-stop setup has not changed the existing service.
        current = inspect('livekit')
        if current and current['Id'] == state['original_id']:
            docker('start', 'livekit')
            state['phase'] = 'rolled_back'
            save(STATE, state)
            return
        raise RuntimeError('original_container_missing')
    if old['Id'] != state['original_id']:
        raise RuntimeError('backup_identity_changed')
    current = inspect('livekit')
    if current:
        if current['Id'] != state.get('candidate_id'):
            raise RuntimeError('candidate_identity_changed')
        docker('update', '--restart=no', 'livekit')
        docker('stop', '--time=5', 'livekit')
        docker('rename', 'livekit', FAILED)
    docker('rename', BACKUP, 'livekit')
    docker('update', '--restart=always', 'livekit')
    docker('start', 'livekit')
    state['phase'] = 'rolled_back'
    save(STATE, state)


def apply():
    if STATE.exists() or inspect(BACKUP) or inspect(FAILED):
        raise RuntimeError('prior_rollout_exists')
    original = inspect('livekit')
    if not original or not original['State']['Running'] or original['Image'] != EXPECTED_BASE:
        raise RuntimeError('original_baseline_changed')
    if original['HostConfig']['RestartPolicy']['Name'] != 'always':
        raise RuntimeError('restart_policy_changed')
    binary = ROOT / 'livekit-server-quality-v7'
    if hashlib.sha256(binary.read_bytes()).hexdigest() != EXPECTED_BINARY:
        raise RuntimeError('candidate_hash_mismatch')
    binary.chmod(0o755)
    # Derive from the exact local image ID without registry resolution. This
    # stopped build container never sees the application's config or secrets.
    build_container = docker('create', EXPECTED_BASE)
    try:
        docker('cp', str(binary), build_container + ':/livekit-server')
        image = docker('commit', build_container)
    finally:
        docker('rm', build_container)
    participants = room_count()
    if participants:
        print(json.dumps({'status': 'DEFERRED', 'active_participants': participants}))
        return
    save(ROOT / 'original-inspect-v7-retry2.json', original)
    config_bytes = Path('/root/livekit.yaml').read_bytes()
    config_backup = ROOT / 'original-config-v7-retry2.yaml'
    with open(config_backup, 'wb', opener=lambda p, flags: os.open(p, flags, 0o600)) as stream:
        stream.write(config_bytes)
    original_binary = docker('exec', 'livekit', 'sha256sum', '/livekit-server').split()[0]
    state = {'phase': 'applying', 'original_id': original['Id'], 'candidate_image': image,
             'binary_sha256': EXPECTED_BINARY, 'deadline': time.time() + 900,
             'original_binary_sha256': original_binary, 'config_sha256': hashlib.sha256(config_bytes).hexdigest(),
             'ports': [17880, 17881, 17882]}
    save(STATE, state)
    # A host-side deadline survives loss of the local validation process.
    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'watchdog'],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        docker('stop', '--time=5', 'livekit')
        docker('rename', 'livekit', BACKUP)
        docker('update', '--restart=no', BACKUP)
        config = original['Config'].copy()
        config['Image'] = image
        config['HostConfig'] = original['HostConfig']
        config.pop('Hostname', None)
        connection = http.client.HTTPConnection('localhost', timeout=20)
        connection.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.sock.connect('/var/run/docker.sock')
        connection.request('POST', '/containers/create?name=livekit', json.dumps(config),
                           {'Content-Type': 'application/json'})
        response = connection.getresponse()
        payload = response.read()
        if response.status != 201:
            raise RuntimeError('candidate_create_failed')
        state['candidate_id'] = json.loads(payload)['Id']
        save(STATE, state)
        docker('start', 'livekit')
        for _ in range(20):
            try:
                with urllib.request.urlopen('http://127.0.0.1:17880/', timeout=1) as health:
                    if health.status == 200:
                        break
            except Exception:
                time.sleep(0.5)
        else:
            raise RuntimeError('candidate_health_failed')
        state['phase'] = 'validating'
        save(STATE, state)
        print(json.dumps({'status': 'VALIDATING', 'active_participants_before': participants,
                          'candidate_image': image, 'binary_sha256': EXPECTED_BINARY,
                          'rollback_deadline_seconds': 900}))
    except Exception:
        rollback()
        raise


def watchdog():
    while True:
        state = json.loads(STATE.read_text())
        if state['phase'] in ['accepted', 'rolled_back']:
            return
        if time.time() >= state['deadline']:
            rollback()
            return
        time.sleep(5)


def accept():
    state = json.loads(STATE.read_text())
    current = inspect('livekit')
    if state['phase'] != 'validating' or not current or current['Id'] != state['candidate_id']:
        raise RuntimeError('not_validating_candidate')
    if docker('exec', 'livekit', 'sha256sum', '/livekit-server').split()[0] != EXPECTED_BINARY:
        raise RuntimeError('running_binary_mismatch')
    state['phase'] = 'accepted'
    save(STATE, state)
    print(json.dumps({'status': 'ACCEPTED', 'candidate_image': state['candidate_image']}))


if __name__ == '__main__':
    try:
        {'apply': apply, 'rollback': rollback, 'watchdog': watchdog, 'accept': accept}[sys.argv[1]]()
    except Exception as error:
        print(json.dumps({'status': 'ERROR', 'error_type': type(error).__name__}))
        raise SystemExit(1)
