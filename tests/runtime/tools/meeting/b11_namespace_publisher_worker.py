"""Ubuntu-owned CLI publisher inside one task network namespace; no secret output."""
from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
from urllib.parse import urlsplit


def publisher_argv(request):
    group = request['publisher']
    return [request['cli_path'], 'load-test', '--room', request['room'],
        '--duration', group['duration'], '--video-publishers', str(group['count']),
        '--subscribers', str(group['subscribers']), '--video-resolution', 'high',
        '--video-codec', 'vp8', '--num-per-second', str(group['num_per_second']),
        '--identity-prefix', group['identity_prefix'], '--yes']


def validate_request(request):
    if type(request) is not dict or set(request) != {'schema', 'scope', 'owner_id',
            'room', 'cli_path', 'cli_sha256', 'config_path', 'service_url', 'publisher',
            'owner_path', 'expected_namespace_inode'} or request['schema'] != 1 or \
            request['scope'] != 'B11_TASK_NAMESPACE_PUBLISHER' or \
            not re.fullmatch(r'[0-9a-f]{32}', request['owner_id']) or \
            not re.fullmatch(r'b11-100-[A-Za-z0-9-]+', request['room']):
        raise ValueError('invalid_worker_request')
    directory = PurePosixPath(request['owner_path']).parent
    if not re.fullmatch(r'/tmp/b11-100-[A-Za-z0-9-]+', str(directory)) or \
            PurePosixPath(request['owner_path']).name not in ('trial-owner.json', 'publisher-owner.json'):
        raise ValueError('invalid_worker_owner_path')
    group = request['publisher']
    expected = {'kind', 'count', 'subscribers', 'resolution', 'codec', 'duration',
                'num_per_second', 'simulcast', 'identity_prefix'}
    if type(group) is not dict or set(group) != expected or group['kind'] != 'load_test' or \
            group['resolution'] != 'high' or group['codec'] != 'vp8' or group['simulcast'] is not True or \
            type(group['count']) is not int or type(group['subscribers']) is not int or \
            (group['count'], group['subscribers'], group['duration']) not in ((1, 1, '60s'), (100, 0, '15m')) or \
            group['num_per_second'] != 5 or \
            not re.fullmatch(r'b11(?:path|grid100)_[A-Za-z0-9]{1,32}', group['identity_prefix']):
        raise ValueError('invalid_worker_publisher')
    parsed = urlsplit(request['service_url'])
    if parsed.scheme != 'http' or not parsed.hostname or \
            ipaddress.ip_address(parsed.hostname) not in ipaddress.ip_network('198.18.0.0/15') or \
            parsed.port != 17880 or parsed.username or \
            parsed.password or parsed.path not in ('', '/') or parsed.query or parsed.fragment:
        raise ValueError('invalid_worker_service_url')
    for name in ('cli_path', 'config_path'):
        if type(request[name]) is not str or not re.fullmatch(r'/[A-Za-z0-9_./-]+', request[name]) or \
                '..' in Path(request[name]).parts:
            raise ValueError('invalid_worker_input_path')
    if not re.fullmatch(r'[0-9a-f]{64}', request['cli_sha256']) or \
            type(request['expected_namespace_inode']) is not int or request['expected_namespace_inode'] <= 0:
        raise ValueError('invalid_worker_identity')
    return request


def run_request(request):
    request = validate_request(request)
    namespace_inode = os.stat('/proc/self/ns/net').st_ino
    if namespace_inode != request['expected_namespace_inode']:
        raise ValueError('worker_namespace_mismatch')
    import pwd
    if os.getuid() != pwd.getpwnam('ubuntu').pw_uid:
        raise ValueError('worker_uid_mismatch')
    cli = Path(request['cli_path'])
    if hashlib.sha256(cli.read_bytes()).hexdigest() != request['cli_sha256']:
        raise ValueError('worker_cli_changed')
    import yaml
    config = yaml.safe_load(Path(request['config_path']).read_bytes())
    key, secret = next(iter(config['keys'].items()))
    if type(key) is not str or not key or type(secret) is not str or not secret:
        raise ValueError('worker_service_keys_invalid')
    environment = os.environ.copy()
    environment.update(LIVEKIT_URL=request['service_url'], LIVEKIT_API_KEY=key, LIVEKIT_API_SECRET=secret)
    pid = os.getpid()
    ticks = Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19]
    argv = publisher_argv(request)
    owner = {'schema': 1, 'owner_id': request['owner_id'], 'pid': pid,
        'start_ticks': ticks, 'argv': argv, 'uid': os.getuid(), 'namespace_inode': namespace_inode}
    path = Path(request['owner_path'])
    temporary = path.with_suffix('.pending')
    with temporary.open('x') as stream:
        json.dump(owner, stream)
    temporary.chmod(0o600)
    os.replace(temporary, path)
    os.execve(str(cli), argv, environment)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--request', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        run_request(json.loads(args.request.read_text()))
    except Exception:
        print('task_namespace_publisher_failed', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
