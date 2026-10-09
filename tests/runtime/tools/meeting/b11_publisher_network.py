"""Task-owned namespace/veth media path gate; never changes host firewall or SFU."""
from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import ipaddress
import json
import os
from pathlib import Path
try:
    import pwd
except ImportError:  # Windows imports this module only for offline checks.
    pwd = None
import re
import secrets
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

NODE_IP = '81.71.85.246'
UDP_PORT, TCP_PORT, SIGNAL_PORT = 17882, 17881, 17880
PATH_SECONDS = 15
MIN_PRIVATE_BYTES, MIN_PRIVATE_PACKETS, MAX_PUBLIC_BYTES = 1048576, 100, 262144
PRIVATE_RTC_NETWORKS = tuple(ipaddress.ip_network(value) for value in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))


def command(argv, *, text=True):
    return subprocess.check_output(argv, text=text, timeout=20, stderr=subprocess.DEVNULL)


def atomic(path, value, *, uid=None):
    temporary = path.with_suffix(path.suffix + '.new')
    temporary.write_text(json.dumps(value))
    temporary.chmod(0o600)
    if uid is not None:
        account = pwd.getpwnam(uid)
        os.chown(temporary, account.pw_uid, account.pw_gid)
    temporary.replace(path)


def process_identity(pid):
    try:
        directory = Path('/proc') / str(pid)
        parts = (directory / 'stat').read_text().rsplit(')', 1)[1].split()
        if parts[0] == 'Z':
            return None
        return {'pid': int(pid), 'start_ticks': parts[19],
            'argv': [value.decode() for value in (directory / 'cmdline').read_bytes().rstrip(b'\0').split(b'\0')],
            'uid': directory.stat().st_uid, 'namespace_inode': (directory / 'ns/net').stat().st_ino}
    except FileNotFoundError:
        return None


def stop_owned(owner):
    result = {'pid': owner['pid'], 'stopped': False}
    for sig, grace in ((signal.SIGINT, 3), (signal.SIGTERM, 5)):
        current = process_identity(owner['pid'])
        if current is None or current['start_ticks'] != owner['start_ticks']:
            result['stopped'] = True
            return result
        if any(current[name] != owner[name] for name in ('argv', 'uid', 'namespace_inode')):
            result['reason'] = 'process_ownership_mismatch'
            return result
        try:
            os.kill(owner['pid'], sig)
        except ProcessLookupError:
            result['stopped'] = True
            return result
        deadline = time.monotonic() + grace
        while time.monotonic() < deadline:
            current = process_identity(owner['pid'])
            if current is None or current['start_ticks'] != owner['start_ticks']:
                result['stopped'] = True
                return result
            time.sleep(.1)
    result['reason'] = 'process_stop_timeout'
    return result


def prerequisite(request):
    target, source = request['target'], request['source']
    expected = source['remote_prerequisite']
    config_bytes = Path(target['config_path']).read_bytes()
    inspected = json.loads(command(['docker', 'inspect', target['sfu_container']]))[0]
    observed = {'config_sha256': hashlib.sha256(config_bytes).hexdigest(),
        'cli_sha256': hashlib.sha256(Path(source['cli_path']).read_bytes()).hexdigest(),
        'sfu_image': inspected['Image'], 'sfu_pid': int(inspected['State']['Pid']),
        'logical_cpus_observed': os.cpu_count(),
        'mem_total_kib_observed': int(next(line.split()[1] for line in
            Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemTotal:')))}
    if any(observed[name] != expected[name] for name in
            ('config_sha256', 'sfu_image', 'logical_cpus_observed', 'mem_total_kib_observed')) or \
            observed['cli_sha256'] != source['cli_sha256'] or not inspected['State']['Running']:
        raise RuntimeError('publisher_network_prerequisite_changed')
    return config_bytes, observed


def capcheck(request):
    if os.geteuid() != 0 or any(shutil.which(name) is None for name in ('ip', 'iptables', 'runuser', 'python3', 'ss')):
        raise RuntimeError('task_network_capability_missing')
    command(['ip', 'netns', 'list'])
    command(['iptables', '-V'])
    command(['runuser', '-u', 'ubuntu', '--', 'test', '-r', request['target']['config_path']])
    return {'status': 'PASS', 'global_firewall_modified': False, 'sfu_modified': False}


def rtc_target_ip(sfu_pid):
    route = json.loads(command(['ip', '-j', '-4', 'route', 'get', NODE_IP]))[0]
    address = route.get('prefsrc') or route.get('src')
    try:
        parsed = ipaddress.ip_address(address)
    except (ValueError, TypeError):
        raise RuntimeError('sfu_rtc_primary_address_unknown') from None
    local = json.loads(command(['ip', '-j', '-4', 'addr', 'show', 'dev', route['dev']]))
    if parsed.version != 4 or not any(parsed in subnet for subnet in PRIVATE_RTC_NETWORKS) or not any(
            item.get('family') == 'inet' and item.get('local') == address
            for row in local for item in row.get('addr_info', [])):
        raise RuntimeError('sfu_rtc_target_not_local_private')
    for options, port in (('-lnup', UDP_PORT), ('-lntp', TCP_PORT)):
        rows = command(['ss', '-4', '-H', options]).splitlines()
        listening = any(len(row.split()) >= 4 and row.split()[3] in
                (address + ':' + str(port), '0.0.0.0:' + str(port), '*:' + str(port)) and
                re.search(r'\bpid=' + str(int(sfu_pid)) + r'(?:,|\))', row) for row in rows)
        if not listening and options == '-lntp':
            # Go's TCP listener may use an IPv6 wildcard with v6only disabled.
            # An IPv6-only wildcard cannot receive this IPv4 private path.
            rows = command(['ss', '-6', '-H', '-lntpe']).splitlines()
            listening = any(len(row.split()) >= 4 and row.split()[3] in
                    ('*:' + str(port), '[::]:' + str(port)) and
                    re.search(r'\bpid=' + str(int(sfu_pid)) + r'(?:,|\))', row) and
                    re.search(r'(?:^|\s)v6only:0(?:\s|$)', row) for row in rows)
        if not listening:
            raise RuntimeError('sfu_rtc_target_not_listening')
    return address


def choose_subnet(routes, addresses, *, chooser=secrets.randbelow):
    occupied = []
    for row in routes:
        destination = row.get('dst', 'default')
        if destination != 'default':
            occupied.append(ipaddress.ip_network(destination, strict=False))
    for row in addresses:
        for value in row.get('addr_info', []):
            if value.get('family') == 'inet':
                occupied.append(ipaddress.ip_network(f"{value['local']}/{value['prefixlen']}", strict=False))
    base = int(ipaddress.IPv4Address('198.18.0.0'))
    for _ in range(256):
        subnet = ipaddress.ip_network((base + chooser(32768) * 4, 30))
        if not any(subnet.overlaps(value) for value in occupied):
            return subnet
    raise RuntimeError('task_private_subnet_unavailable')


def link(name, namespace=None):
    prefix = ['ip'] if namespace is None else ['ip', '-n', namespace]
    try:
        return json.loads(command(prefix + ['-j', '-d', 'link', 'show', 'dev', name]))[0]
    except subprocess.CalledProcessError:
        return None


def load_owner(request):
    path = Path(request['directory']) / 'network-owner.json'
    owner = root_json(path)
    if owner['owner_id'] != request['owner_id'] or owner['directory'] != request['directory']:
        raise RuntimeError('network_marker_mismatch')
    return owner


def root_json(path):
    if path.is_symlink() or path.stat().st_uid != 0 or path.stat().st_mode & 0o077:
        raise RuntimeError('network_marker_ownership_unknown')
    return json.loads(path.read_text())


def save_owner(request, owner):
    atomic(Path(request['directory']) / 'network-owner.json', owner)


def rollback_children(children):
    complete = True
    for child in reversed(children):
        try:
            if child.poll() is None:
                child.send_signal(signal.SIGINT)
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.terminate()
                    child.wait(timeout=5)
        except Exception:
            complete = False
    return complete


def create_network(request):
    capcheck(request)
    _, observed = prerequisite(request)
    rtc_target = rtc_target_ip(observed['sfu_pid'])
    suffix = request['owner_id'][:11]
    namespace = 'b11ns-' + request['owner_id'][:16]
    host, peer = 'b11h' + suffix, 'b11p' + suffix
    if link(host) or link(peer) or (Path('/run/netns') / namespace).exists():
        raise RuntimeError('task_network_name_collision')
    subnet = choose_subnet(json.loads(command(['ip', '-j', '-4', 'route', 'show', 'table', 'all'])),
                           json.loads(command(['ip', '-j', '-4', 'addr', 'show'])))
    owner = {'schema': 1, 'owner_id': request['owner_id'], 'directory': request['directory'],
        'host_namespace_inode': Path('/proc/self/ns/net').stat().st_ino,
        'namespace': namespace, 'namespace_inode': None, 'veth_host': host, 'veth_peer': peer,
        'host_ifindex': None, 'peer_ifindex': None, 'subnet': str(subnet),
        'host_ip': str(subnet.network_address + 1), 'peer_ip': str(subnet.network_address + 2),
        'rtc_target_ip': rtc_target,
        'host_alias': 'b11-owner:' + request['owner_id'] + ':host',
        'peer_alias': 'b11-owner:' + request['owner_id'] + ':peer', 'phase': 'PENDING'}
    save_owner(request, owner)
    namespace_created = links_created = False
    try:
        command(['ip', 'netns', 'add', namespace])
        namespace_created = True
        owner.update(namespace_inode=(Path('/run/netns') / namespace).stat().st_ino, phase='NAMESPACE')
        save_owner(request, owner)
        command(['ip', 'link', 'add', host, 'type', 'veth', 'peer', 'name', peer])
        links_created = True
        owner.update(host_ifindex=link(host)['ifindex'], peer_ifindex=link(peer)['ifindex'], phase='LINK')
        save_owner(request, owner)
        command(['ip', 'link', 'set', 'dev', host, 'alias', owner['host_alias']])
        command(['ip', 'link', 'set', 'dev', peer, 'alias', owner['peer_alias']])
        command(['ip', 'link', 'set', peer, 'netns', namespace])
        owner.update(peer_ifindex=link(peer, namespace)['ifindex'], phase='MOVED')
        save_owner(request, owner)
        command(['ip', 'addr', 'add', owner['host_ip'] + '/30', 'dev', host])
        command(['ip', 'link', 'set', host, 'up'])
        command(['ip', '-n', namespace, 'addr', 'add', owner['peer_ip'] + '/30', 'dev', peer])
        command(['ip', '-n', namespace, 'link', 'set', 'lo', 'up'])
        command(['ip', '-n', namespace, 'link', 'set', peer, 'up'])
        command(['ip', '-n', namespace, 'route', 'add', NODE_IP + '/32', 'via', owner['host_ip'], 'dev', peer])
        command(['ip', '-n', namespace, 'route', 'add', rtc_target + '/32', 'via', owner['host_ip'], 'dev', peer])
        for protocol, port in (('udp', UDP_PORT), ('tcp', TCP_PORT)):
            command(['ip', 'netns', 'exec', namespace, 'iptables', '-w', '2', '-t', 'nat', '-A', 'OUTPUT',
                '-d', NODE_IP + '/32', '-p', protocol, '-m', protocol, '--dport', str(port),
                '-j', 'DNAT', '--to-destination', rtc_target + ':' + str(port)])
        owner['phase'] = 'READY'
        save_owner(request, owner)
        return {'ok': True, 'owner': owner, 'remote_inputs': observed}
    except BaseException as failure:
        try:
            # A failed metadata write must not discard identities already read.
            # If identity reading itself failed, retry while this syscall's
            # success is still known; otherwise preserve UNKNOWN and refuse delete.
            if namespace_created and owner['namespace_inode'] is None:
                owner['namespace_inode'] = (Path('/run/netns') / namespace).stat().st_ino
                owner['phase'] = 'NAMESPACE'
            if links_created and owner['host_ifindex'] is None:
                owner['host_ifindex'] = link(host)['ifindex']
                owner['peer_ifindex'] = (link(peer) or link(peer, namespace))['ifindex']
                owner['phase'] = 'LINK'
            removed = delete_network(request, owner_override=owner)
            if not removed['network_removed']:
                raise RuntimeError('task_network_setup_rollback_incomplete')
        except Exception:
            raise RuntimeError('task_network_setup_rollback_unknown') from None
        raise failure


def verify_network(request, *, sfu_pid=None):
    owner = load_owner(request)
    if sfu_pid is None:
        _, observed = prerequisite(request)
        sfu_pid = observed['sfu_pid']
    if rtc_target_ip(sfu_pid) != owner['rtc_target_ip']:
        raise RuntimeError('sfu_rtc_target_changed')
    handle = Path('/run/netns') / owner['namespace']
    if not handle.exists() or handle.stat().st_ino != owner['namespace_inode']:
        raise RuntimeError('network_namespace_identity_changed')
    for name, index, alias, namespace in ((owner['veth_host'], owner['host_ifindex'], owner['host_alias'], None),
            (owner['veth_peer'], owner['peer_ifindex'], owner['peer_alias'], owner['namespace'])):
        observed = link(name, namespace)
        if observed is None or observed['ifindex'] != index or observed.get('ifalias') != alias:
            raise RuntimeError('network_link_identity_changed')
    routes = json.loads(command(['ip', '-n', owner['namespace'], '-j', '-4', 'route', 'show']))
    allowed = {owner['subnet'], NODE_IP, owner['rtc_target_ip']}
    if any(row.get('dst', 'default') not in allowed for row in routes) or \
            not all(any(row.get('dst') == target and row.get('gateway') == owner['host_ip'] and
                    row.get('dev') == owner['veth_peer'] for row in routes) for target in (NODE_IP, owner['rtc_target_ip'])):
        raise RuntimeError('task_namespace_route_changed')
    private_route = json.loads(command(['ip', '-j', '-4', 'route', 'get', owner['peer_ip']]))[0]
    if private_route['dev'] != owner['veth_host']:
        raise RuntimeError('sfu_private_route_changed')
    rules = [shlex.split(line) for line in command(['ip', 'netns', 'exec', owner['namespace'],
             'iptables', '-t', 'nat', '-S', 'OUTPUT']).splitlines() if line.startswith('-A ')]
    expected_rules = [['-A', 'OUTPUT', '-d', NODE_IP + '/32', '-p', protocol, '-m', protocol,
        '--dport', str(port), '-j', 'DNAT', '--to-destination', owner['rtc_target_ip'] + ':' + str(port)]
        for protocol, port in (('udp', UDP_PORT), ('tcp', TCP_PORT))]
    if rules != expected_rules:
        raise RuntimeError('task_namespace_firewall_changed')
    return owner


def delete_network(request, *, owner_override=None):
    marker = load_owner(request)
    owner = owner_override or marker
    if owner['owner_id'] != marker['owner_id'] or owner['directory'] != marker['directory']:
        raise RuntimeError('network_rollback_marker_mismatch')
    handle = Path('/run/netns') / owner['namespace']
    if handle.exists():
        if handle.stat().st_ino != owner['namespace_inode']:
            raise RuntimeError('network_namespace_identity_changed')
        if command(['ip', 'netns', 'pids', owner['namespace']]).strip():
            return {'ok': True, 'network_removed': False, 'reason': 'namespace_processes_remain'}
    host = link(owner['veth_host'])
    peer = link(owner['veth_peer'], owner['namespace']) if handle.exists() else None
    peer = peer or link(owner['veth_peer'])
    if peer and (peer['ifindex'] != owner['peer_ifindex'] or peer.get('ifalias', '') not in (
            owner['peer_alias'], '' if owner['phase'] == 'LINK' else owner['peer_alias'])):
        raise RuntimeError('network_peer_ownership_changed')
    if host:
        if host['ifindex'] != owner['host_ifindex'] or host.get('ifalias', '') not in (
                owner['host_alias'], '' if owner['phase'] == 'LINK' else owner['host_alias']):
            raise RuntimeError('network_host_ownership_changed')
        command(['ip', 'link', 'delete', owner['veth_host']])
    if handle.exists():
        command(['ip', 'netns', 'delete', owner['namespace']])
    owner['phase'] = 'REMOVED'
    save_owner(request, owner)
    return {'ok': True, 'network_removed': not handle.exists() and link(owner['veth_host']) is None}


def start_publisher(request, *, trial=False):
    from b11_namespace_publisher_worker import publisher_argv
    _, observed = prerequisite(request)
    owner = verify_network(request, sfu_pid=observed['sfu_pid'])
    directory = Path(request['directory'])
    if not trial:
        path = directory / 'path-gate.json'
        if path.is_symlink() or path.stat().st_uid != 0 or path.stat().st_mode & 0o077:
            raise RuntimeError('fresh_gate_ownership_unknown')
        gate = json.loads(path.read_text())
        expected_target = {name: request['target'][name] for name in ('instance', 'service_url', 'config_path', 'sfu_container')}
        if gate['status'] != 'PASS' or gate['scope'] != 'B11_100_SOURCE_PRIVATE_MEDIA_PATH_GATE' or \
                gate['target'] != expected_target or gate['cli_sha256'] != observed['cli_sha256'] or \
                gate['config_sha256'] != observed['config_sha256'] or gate['sfu_image'] != observed['sfu_image'] or \
                not path_passes(gate['facts']) or gate['task_owner']['room'] != request['room'] or \
                gate['task_owner']['remote_directory'] != request['directory'] or \
                any(gate['task_owner'][name] != owner[name] for name in ('owner_id', 'namespace', 'namespace_inode',
                    'veth_host', 'veth_peer', 'host_ifindex', 'peer_ifindex', 'subnet', 'host_ip', 'peer_ip', 'rtc_target_ip')) or \
                gate['cleanup'] != {'publisher_stopped': True, 'subscriber_stopped': True,
                                   'network_removed': False, 'credentials_removed': True}:
            raise RuntimeError('fresh_namespace_path_gate_not_valid')
    group = dict(request['publisher'])
    role = 'trial' if trial else 'publisher'
    if trial:
        group.update(count=1, subscribers=1, duration='60s', identity_prefix='b11path_' + request['owner_id'][:16])
    worker_request = {'schema': 1, 'scope': 'B11_TASK_NAMESPACE_PUBLISHER',
        'owner_id': request['owner_id'], 'room': request['room'], 'cli_path': request['source']['cli_path'],
        'cli_sha256': request['source']['cli_sha256'], 'config_path': request['target']['config_path'],
        'service_url': 'http://' + owner['host_ip'] + ':' + str(SIGNAL_PORT), 'publisher': group,
        'owner_path': str(directory / (role + '-owner.json')),
        'expected_namespace_inode': owner['namespace_inode']}
    request_path = directory / (role + '-request.json')
    state_path = directory / (role + '-state.json')
    if request_path.exists() or state_path.exists() or Path(worker_request['owner_path']).exists():
        raise RuntimeError('namespace_publisher_files_already_exist')
    atomic(request_path, worker_request, uid='ubuntu')
    launch = ['ip', 'netns', 'exec', owner['namespace'], 'runuser', '-u', 'ubuntu', '--',
        sys.executable, str(directory / 'b11_namespace_publisher_worker.py'), '--request', str(request_path)]
    expected_argv = publisher_argv(worker_request)
    state = {'schema': 1, 'owner_id': request['owner_id'], 'launcher': None, 'publisher': None,
             'trial': trial, 'remote_inputs': observed, 'launch_pending': True}
    atomic(state_path, state)
    children = []
    try:
        child = subprocess.Popen(launch, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, start_new_session=True)
        children.append(child)
        state['launcher'] = process_identity(child.pid)
        atomic(state_path, state)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if Path(worker_request['owner_path']).exists():
                candidate = json.loads(Path(worker_request['owner_path']).read_text())
                current = process_identity(candidate['pid'])
                if current and current['argv'] == expected_argv and current['start_ticks'] == candidate['start_ticks'] and \
                        current['namespace_inode'] == owner['namespace_inode'] and current['uid'] == pwd.getpwnam('ubuntu').pw_uid:
                    state['publisher'] = current
                    # ip exec becomes runuser after the initial Popen return.
                    state['launcher'] = process_identity(child.pid)
                    state['launch_pending'] = False
                    atomic(state_path, state)
                    return state
            if child.poll() is not None:
                raise RuntimeError('namespace_publisher_exited_early')
            time.sleep(.1)
        raise RuntimeError('namespace_publisher_owner_timeout')
    except BaseException as failure:
        complete = rollback_children(children)
        state.update(launch_pending=not complete, rollback_complete=complete)
        try:
            atomic(state_path, state)
        except Exception:
            raise RuntimeError('publisher_launch_cleanup_unknown') from None
        if not complete:
            raise RuntimeError('publisher_launch_cleanup_unknown') from None
        raise failure


def stop_publishers(request):
    results = []
    directory = Path(request['directory'])
    try:
        network_owner = load_owner(request)
    except Exception:
        return {'ok': True, 'all_stopped': False, 'processes': [], 'reason': 'network_marker_ownership_unknown'}
    for role in ('sampler', 'trial', 'publisher'):
        state_path = directory / (role + '-state.json')
        if not state_path.exists():
            continue
        try:
            state = root_json(state_path)
            if state.get('schema') != 1 or state['owner_id'] != request['owner_id']:
                raise RuntimeError('publisher_state_owner_mismatch')
            if state.get('launch_pending') or state.get('rollback_complete') is False:
                results.append({'stopped': False, 'reason': 'pending_process_cleanup_unknown'})
            for name in ('publisher', 'launcher'):
                if state.get(name):
                    if not task_process_matches(request, network_owner, role, name, state[name]):
                        raise RuntimeError('process_not_owned_by_task')
                    results.append(stop_owned(state[name]))
        except Exception:
            results.append({'stopped': False, 'reason': 'publisher_ownership_unknown'})
    return {'ok': True, 'all_stopped': all(value['stopped'] for value in results), 'processes': results}


def task_process_matches(request, network_owner, role, field, owner):
    argv = owner['argv']
    directory = Path(request['directory'])
    if role == 'sampler':
        expected_file = 'ecs_resource_sampler.py' if field == 'publisher' else 'b11_100_resource_sampler.py'
        expected_output = 'ecs-metrics.csv' if field == 'publisher' else 'namespace-metrics.csv'
        return owner['uid'] == 0 and owner['namespace_inode'] == network_owner['host_namespace_inode'] and \
            argv[:4] == [sys.executable, str(directory / expected_file), '--output', str(directory / expected_output)] and \
            argv[-2:] == ['--duration', '510']
    if owner['namespace_inode'] != network_owner['namespace_inode']:
        return False
    if field == 'launcher':
        return owner['uid'] == 0 and Path(argv[0]).name == 'runuser' and argv[1:] == ['-u', 'ubuntu', '--',
            sys.executable, str(directory / 'b11_namespace_publisher_worker.py'), '--request', str(directory / (role + '-request.json'))]
    from b11_namespace_publisher_worker import publisher_argv
    group = dict(request['publisher'])
    if role == 'trial':
        group.update(count=1, subscribers=1, duration='60s', identity_prefix='b11path_' + request['owner_id'][:16])
    expected = publisher_argv({'cli_path': request['source']['cli_path'], 'room': request['room'], 'publisher': group})
    return owner['uid'] == pwd.getpwnam('ubuntu').pw_uid and argv == expected


def start_sampler(request):
    _, observed = prerequisite(request)
    network_owner = verify_network(request, sfu_pid=observed['sfu_pid'])
    directory = Path(request['directory'])
    state_path = directory / 'sampler-state.json'
    if state_path.exists():
        raise RuntimeError('sampler_state_already_exists')
    publisher = json.loads((directory / 'publisher-state.json').read_text())['publisher']
    sfu = process_identity(observed['sfu_pid'])
    state = {'schema': 1, 'owner_id': request['owner_id'], 'publisher': None, 'launcher': None, 'launch_pending': True}
    atomic(state_path, state)
    commands = [[sys.executable, str(directory / 'ecs_resource_sampler.py'), '--output',
        str(directory / 'ecs-metrics.csv'), '--pid', str(observed['sfu_pid']), '--duration', '510'],
        [sys.executable, str(directory / 'b11_100_resource_sampler.py'), '--output', str(directory / 'namespace-metrics.csv'),
         '--publisher-pid', str(publisher['pid']), '--publisher-start-ticks', publisher['start_ticks'],
         '--sfu-pid', str(sfu['pid']), '--sfu-start-ticks', sfu['start_ticks'],
         '--private-interface', network_owner['veth_host'], '--private-ifindex', str(network_owner['host_ifindex']),
         '--owner-id', request['owner_id'], '--duration', '510']]
    children = []
    try:
        for field, argv in zip(('publisher', 'launcher'), commands):
            child = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL, start_new_session=True)
            children.append(child)
            identity = process_identity(child.pid)
            if identity is None or identity['argv'] != argv:
                raise RuntimeError('ecs_sampler_owner_unavailable')
            state[field] = identity
            atomic(state_path, state)
        time.sleep(.2)
        if any(child.poll() is not None for child in children):
            raise RuntimeError('ecs_sampler_exited_early')
        state['launch_pending'] = False
        atomic(state_path, state)
        return {'ok': True, 'ecs_owner': state['publisher'], 'namespace_owner': state['launcher']}
    except BaseException as failure:
        complete = rollback_children(children)
        state.update(launch_pending=not complete, rollback_complete=complete)
        try:
            atomic(state_path, state)
        except Exception:
            raise RuntimeError('sampler_launch_cleanup_unknown') from None
        if not complete:
            raise RuntimeError('sampler_launch_cleanup_unknown') from None
        raise failure


def observer_credential(request):
    config_bytes, _ = prerequisite(request)
    import yaml
    key, secret = next(iter(yaml.safe_load(config_bytes)['keys'].items()))
    environment = os.environ.copy()
    environment.update(LIVEKIT_URL=request['target']['local_service_url'], LIVEKIT_API_KEY=key, LIVEKIT_API_SECRET=secret)
    value = subprocess.check_output([request['source']['cli_path'], 'token', 'create', '--room', request['room'],
        '--identity', 'b11-grid100-observer', '--join', '--valid-for', '15m', '--token-only'],
        env=environment, text=True, timeout=15, stderr=subprocess.DEVNULL).strip()
    if not value or len(value) > 8192:
        raise RuntimeError('observer_credential_invalid')
    atomic(Path(request['directory']) / 'observer.json', {'LIVEKIT_URL': request['target']['service_url'],
        'LIVEKIT_SOAK_TOKEN': value, 'LIVEKIT_SOAK_ALLOW_INSECURE': '1'}, uid='ubuntu')
    return {'ok': True, 'credential_ready': True}


def _encode(value):
    return base64.urlsafe_b64encode(json.dumps(value, separators=(',', ':')).encode()).rstrip(b'=')


def room_participants(request, config_bytes):
    import yaml
    key, secret = next(iter(yaml.safe_load(config_bytes)['keys'].items()))
    now = int(time.time())
    header = _encode({'alg': 'HS256', 'typ': 'JWT'})
    payload = _encode({'iss': key, 'nbf': now - 5, 'exp': now + 60,
                      'video': {'roomAdmin': True, 'room': request['room']}})
    data = header + b'.' + payload
    signed = data + b'.' + base64.urlsafe_b64encode(hmac.new(secret.encode(), data, hashlib.sha256).digest()).rstrip(b'=')
    query = urllib.request.Request(request['target']['local_service_url'] + '/twirp/livekit.RoomService/ListParticipants',
        data=json.dumps({'room': request['room']}).encode(), headers={'Content-Type': 'application/json',
            'Authorization': 'Bearer ' + signed.decode()})
    with urllib.request.urlopen(query, timeout=10) as response:
        return json.load(response).get('participants', [])


def advertised_layers(request, config_bytes, *, diagnostics=None):
    participants = room_participants(request, config_bytes)
    tracks = [track for participant in participants for track in participant.get('tracks', [])
              if track.get('type') in ('VIDEO', 1)]
    if diagnostics is not None:
        diagnostics.update(last_api_status=200, participant_count=len(participants),
            video_track_count=len(tracks), layer_counts=[len(track.get('layers', [])) for track in tracks],
            hd_1280x720_layer_count=sum(layer.get('width') == 1280 and layer.get('height') == 720
                                       for track in tracks for layer in track.get('layers', [])))
    if len(participants) != 2 or len(tracks) != 1:
        raise RuntimeError('path_gate_room_tracks_not_ready')
    layers = tracks[0].get('layers', [])
    if len(layers) != 3 or not any(layer.get('width') == 1280 and layer.get('height') == 720 for layer in layers):
        raise RuntimeError('path_gate_hd_layers_not_advertised')
    return {'advertised_layer_count': 3, 'advertised_high_width': 1280, 'advertised_high_height': 720}


def wait_advertised_layers(request, config_bytes, *, timeout_seconds=20, diagnostics=None):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            return advertised_layers(request, config_bytes, diagnostics=diagnostics)
        except urllib.error.HTTPError as error:
            if diagnostics is not None:
                diagnostics.update(last_api_status=int(error.code), last_failure_code='http_' + str(int(error.code)))
            if error.code != 404:
                raise
        except RuntimeError as error:
            if str(error) not in ('path_gate_room_tracks_not_ready', 'path_gate_hd_layers_not_advertised'):
                raise
            if diagnostics is not None:
                diagnostics['last_failure_code'] = str(error)
        time.sleep(.5)
    raise RuntimeError('path_gate_room_not_ready_timeout')


def safe_measurement_reason(stage, error):
    if isinstance(error, urllib.error.HTTPError):
        return stage + '_http_' + str(int(error.code))
    if isinstance(error, RuntimeError) and str(error) == 'path_gate_room_not_ready_timeout':
        return stage + '_not_ready_timeout'
    return stage + '_' + type(error).__name__


def publication_snapshot(request):
    config, _ = prerequisite(request)
    participants = room_participants(request, config)
    prefix = request['publisher']['identity_prefix'] + '_pub_'
    publishers = [participant for participant in participants if participant.get('identity', '').startswith(prefix)]
    tracks = [track for participant in publishers for track in participant.get('tracks', []) if track.get('type') in ('VIDEO', 1)]
    safe = [{'sid_hash': hashlib.sha256(track.get('sid', '').encode()).hexdigest()[:16],
        'layer_count': len(track.get('layers', [])),
        'high_1280x720_advertised': any(layer.get('width') == 1280 and layer.get('height') == 720
                                      for layer in track.get('layers', []))} for track in tracks]
    complete = len(publishers) == 100 and len(tracks) == 100 and all(
        track['layer_count'] == 3 and track['high_1280x720_advertised'] for track in safe)
    return {'ok': True, 'all_sources_advertise_hd': complete, 'publisher_participants': len(publishers),
            'video_tracks': len(tracks), 'tracks': safe, 'measurement_scope': 'PUBLICATION_METADATA_NOT_DECODED_FPS'}


def interface_counters(name):
    root = Path('/sys/class/net') / name / 'statistics'
    return {field: int((root / field).read_text()) for field in ('rx_bytes', 'rx_packets', 'tx_bytes', 'tx_packets')}


def nat_packets(namespace):
    lines = command(['ip', 'netns', 'exec', namespace, 'iptables', '-t', 'nat', '-nvxL', 'OUTPUT']).splitlines()
    return sum(int(line.split()[0]) for line in lines if 'DNAT' in line and 'dpt:' + str(UDP_PORT) in line)


class PrivateRtpCapture:
    def __init__(self, owner):
        self.owner = owner
        self.values = {'private_ingress_rtp_packets': 0, 'private_ingress_rtp_bytes': 0,
                       'private_egress_rtp_packets': 0, 'private_egress_rtp_bytes': 0}
        self.udp_address_pairs = {}
        self.udp_unrecorded_pair_packets = 0
        self.stopping = False
        self.failure = None

    def packet(self, packet):
        if len(packet) < 42 or packet[12:14] != b'\x08\x00':
            return
        ihl = (packet[14] & 15) * 4
        if packet[14] >> 4 != 4 or ihl < 20 or packet[23] != 17 or len(packet) < 14 + ihl + 8 or \
                struct.unpack('!H', packet[20:22])[0] & 0x1fff:
            return
        source = socket.inet_ntoa(packet[26:30])
        destination = socket.inet_ntoa(packet[30:34])
        source_port, destination_port = struct.unpack('!HH', packet[14 + ihl:18 + ihl])
        pair = (source, destination, source_port, destination_port)
        if pair in self.udp_address_pairs or len(self.udp_address_pairs) < 32:
            self.udp_address_pairs[pair] = self.udp_address_pairs.get(pair, 0) + 1
        else:
            self.udp_unrecorded_pair_packets += 1
        payload = packet[22 + ihl:]
        if len(payload) < 12 or payload[0] >> 6 != 2 or 200 <= payload[1] <= 207:
            return
        direction = None
        if source == self.owner['peer_ip'] and destination == self.owner['rtc_target_ip'] and destination_port == UDP_PORT:
            direction = 'ingress'
        if source == self.owner['rtc_target_ip'] and destination == self.owner['peer_ip'] and source_port == UDP_PORT:
            direction = 'egress'
        if direction:
            self.values['private_' + direction + '_rtp_packets'] += 1
            self.values['private_' + direction + '_rtp_bytes'] += len(packet)

    def udp_diagnostics(self):
        roles = {self.owner['host_ip']: 'host_veth', self.owner['peer_ip']: 'namespace_peer'}
        roles[self.owner['rtc_target_ip']] = 'sfu_rtc_target'
        return {'udp_address_pairs': [{'source_address': source, 'destination_address': destination,
            'source_port': source_port, 'destination_port': destination_port, 'packets': packets,
            'source_role': roles.get(source, 'outside_host_peer'),
            'destination_role': roles.get(destination, 'outside_host_peer')}
            for (source, destination, source_port, destination_port), packets in self.udp_address_pairs.items()],
            'udp_unrecorded_pair_packets': self.udp_unrecorded_pair_packets}

    def _read(self):
        try:
            # Linux delivers outgoing packet taps to ETH_P_ALL sockets. Filtering
            # IPv4 in packet() retains both directions without storing payloads.
            with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003)) as capture:
                capture.bind((self.owner['veth_host'], 0))
                capture.settimeout(.2)
                while not self.stopping:
                    try:
                        self.packet(capture.recv(65535))
                    except socket.timeout:
                        continue
        except Exception:
            self.failure = 'private_rtp_capture_failed'

    def start(self):
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def stop(self):
        self.stopping = True
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            self.failure = 'private_rtp_capture_not_stopped'


def path_passes(facts):
    return PATH_SECONDS <= facts['observation_seconds'] <= 60 and facts['namespace_udp_dnat_packets'] >= 1 and \
        facts['private_veth_rx_bytes'] >= MIN_PRIVATE_BYTES and facts['private_veth_rx_packets'] >= MIN_PRIVATE_PACKETS and \
        all(facts['private_' + direction + '_rtp_packets'] >= MIN_PRIVATE_PACKETS and
            facts['private_' + direction + '_rtp_bytes'] >= MIN_PRIVATE_BYTES for direction in ('ingress', 'egress')) and \
        0 <= facts['public_tx_bytes'] <= MAX_PUBLIC_BYTES and \
        facts['publisher_count'] == 1 and facts['subscriber_count'] == 1 and facts['simulcast'] is True and \
        facts['codec'] == 'vp8' and facts['advertised_layer_count'] == 3 and \
        facts['advertised_high_width'] == 1280 and facts['advertised_high_height'] == 720 and \
        facts['source_fps'] == 30 and facts['media_route'] == 'DNAT_TO_HOST_LOCAL' and all(facts[name] is True for name in
            ('sfu_running', 'namespace_default_route_absent', 'namespace_peer_route_only', 'sfu_peer_route_interface_matches',
             'sfu_rtc_target_local_and_listening'))


def measure_path(request):
    config_bytes, observed = prerequisite(request)
    owner = verify_network(request, sfu_pid=observed['sfu_pid'])
    state = start_publisher(request, trial=True)
    evidence = {'schema': 1, 'scope': 'B11_100_SOURCE_PRIVATE_MEDIA_PATH_GATE', 'status': 'INCONCLUSIVE',
        'formal_b11_status': 'NOT_RUN', 'target': {name: request['target'][name] for name in
            ('instance', 'service_url', 'config_path', 'sfu_container')},
        'cli_sha256': observed['cli_sha256'], 'config_sha256': observed['config_sha256'], 'sfu_image': observed['sfu_image'],
        'task_owner': {**{name: owner[name] for name in ('owner_id', 'namespace', 'namespace_inode',
            'veth_host', 'veth_peer', 'host_ifindex', 'peer_ifindex', 'subnet', 'host_ip', 'peer_ip', 'rtc_target_ip')},
            'room': request['room'], 'remote_directory': request['directory'],
            'publisher_pid': state['publisher']['pid'], 'publisher_start_ticks': state['publisher']['start_ticks']},
        'facts': {}, 'cleanup': {'publisher_stopped': False, 'subscriber_stopped': False,
                               'network_removed': False, 'credentials_removed': True}}
    capture = PrivateRtpCapture(owner)
    stage = 'room_readiness'
    readiness = {'last_api_status': None, 'participant_count': None,
        'video_track_count': None, 'layer_counts': [], 'hd_1280x720_layer_count': None,
        'last_failure_code': None}
    private_before = None
    try:
        private_before = interface_counters(owner['veth_host'])
        capture.start()
        layers = wait_advertised_layers(request, config_bytes, diagnostics=readiness)
        stage = 'private_media_measurement'
        interface = next(line.split()[0] for line in Path('/proc/net/route').read_text().splitlines()[1:]
                         if line.split()[1] == '00000000')
        private_before, public_before = interface_counters(owner['veth_host']), interface_counters(interface)
        # NAT counters increment on the first packet of each conntrack flow.
        # Capture their absolute count in this fresh, exclusively owned namespace.
        # Early capture diagnoses ICE setup. Acceptance counters start only after
        # readiness, matching the private/public interface measurement window.
        capture.values = {name: 0 for name in capture.values}
        start = time.monotonic()
        time.sleep(PATH_SECONDS)
        capture.stop()
        elapsed = time.monotonic() - start
        private_after, public_after = interface_counters(owner['veth_host']), interface_counters(interface)
        _, current = prerequisite(request)
        verify_network(request, sfu_pid=current['sfu_pid'])
        facts = {'observation_seconds': elapsed, 'publisher_count': 1, 'subscriber_count': 1,
            'codec': 'vp8', 'simulcast': True, 'source_fps': 30, **layers,
            'namespace_udp_dnat_packets': nat_packets(owner['namespace']),
            'private_veth_rx_bytes': private_after['rx_bytes'] - private_before['rx_bytes'],
            'private_veth_rx_packets': private_after['rx_packets'] - private_before['rx_packets'],
            'public_tx_bytes': public_after['tx_bytes'] - public_before['tx_bytes'],
            'media_route': 'DNAT_TO_HOST_LOCAL', 'sfu_running': True,
            'namespace_default_route_absent': True, 'namespace_peer_route_only': True,
            'sfu_rtc_target_local_and_listening': True,
            'sfu_peer_route_interface_matches': True, **capture.values}
        evidence.update(facts=facts, status='PASS' if not capture.failure and path_passes(facts) else 'FAIL')
    except Exception as error:
        evidence['reason'] = safe_measurement_reason(stage, error)
        if hasattr(capture, 'thread') and capture.thread.is_alive():
            capture.stop()
        diagnostic = {'room_readiness': readiness, 'private_rtp': dict(capture.values),
            'rtp_capture_error': capture.failure}
        try:
            after = interface_counters(owner['veth_host'])
            diagnostic['private_veth_counters'] = {name: after[name] - private_before[name]
                for name in after} if private_before is not None else None
            diagnostic['namespace_udp_dnat_packets'] = nat_packets(owner['namespace'])
        except Exception as diagnostic_error:
            diagnostic['counter_error'] = type(diagnostic_error).__name__
        evidence['diagnostics'] = diagnostic
    finally:
        if hasattr(capture, 'thread') and capture.thread.is_alive():
            capture.stop()
        stopped = stop_publishers(request)
        evidence['cleanup'].update(publisher_stopped=stopped['all_stopped'], subscriber_stopped=stopped['all_stopped'])
        if not stopped['all_stopped'] and evidence['status'] == 'PASS':
            evidence['status'] = 'INCONCLUSIVE'
        if evidence['status'] != 'PASS':
            evidence.setdefault('diagnostics', {}).update(capture.udp_diagnostics())
        atomic(Path(request['directory']) / 'path-gate.json', evidence)
    return {'ok': True, 'gate': evidence}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--operation', choices=('capcheck', 'create', 'gate', 'start', 'sampler', 'snapshot',
        'credentials', 'remove_credentials', 'stop', 'delete'), required=True)
    parser.add_argument('--request-file', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        request = json.loads(args.request_file.read_text())
        if request['scope'] != 'B11_100_TASK_NETWORK' or not re.fullmatch(r'[0-9a-f]{32}', request['owner_id']) or \
                not re.fullmatch(r'/tmp/b11-100-[A-Za-z0-9-]+', request['directory']) or \
                str(args.request_file.parent) != request['directory']:
            raise RuntimeError('invalid_task_network_request')
        prerequisite_status = 'MATCH'
        if args.operation in ('stop', 'delete', 'remove_credentials'):
            try:
                prerequisite(request)
            except Exception:
                prerequisite_status = 'UNKNOWN'
        else:
            prerequisite(request)
        if args.operation == 'capcheck':
            response = {'ok': True, **capcheck(request)}
        elif args.operation == 'create':
            response = create_network(request)
        elif args.operation == 'gate':
            response = measure_path(request)
        elif args.operation == 'start':
            response = {'ok': True, 'state': start_publisher(request)}
        elif args.operation == 'sampler':
            response = start_sampler(request)
        elif args.operation == 'snapshot':
            response = publication_snapshot(request)
        elif args.operation == 'credentials':
            response = observer_credential(request)
        elif args.operation == 'remove_credentials':
            (Path(request['directory']) / 'observer.json').unlink(missing_ok=True)
            response = {'ok': True, 'credential_removed': not (Path(request['directory']) / 'observer.json').exists()}
        elif args.operation == 'stop':
            response = stop_publishers(request)
        else:
            response = delete_network(request)
        response['prerequisite_status'] = prerequisite_status
    except Exception as error:
        message = str(error)
        response = {'ok': False, 'reason': message if re.fullmatch(r'[a-z][a-z0-9_]{0,127}', message) else type(error).__name__}
    print(json.dumps(response))
    return 0 if response['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
