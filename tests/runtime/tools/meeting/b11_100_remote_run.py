"""One-source private-path gate and frozen 100-source namespace orchestration."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shlex
import sys
from urllib.parse import urlsplit
import uuid

import b11_100_input_freeze as freeze
import b11_hd_remote_run as shared
import b11_remote as remote
import meeting_soak as soak

ROOT = Path(__file__).resolve().parents[4]
PATH_INPUT_SCOPE = 'B11_SINGLE_SOURCE_PRIVATE_PATH_INPUTS'
TOOL_NAMES = ('b11_100_remote_run.py', 'b11_publisher_network.py', 'b11_namespace_publisher_worker.py',
    'b11_100_resource_sampler.py',
    'b11_100_input_freeze.py', 'b11_hd_input_freeze.py', 'b11_input_freeze.py', 'b11_remote.py',
    'b11_hd_remote_run.py', 'meeting_soak.py', 'ecs_resource_sampler.py')
UPLOAD_NAMES = ('b11_publisher_network.py', 'b11_namespace_publisher_worker.py', 'ecs_resource_sampler.py',
                'b11_100_resource_sampler.py')


def _target_from_source(source):
    prerequisite = source['remote_prerequisite']
    target = {name: prerequisite[name] for name in ('instance', 'service_url', 'config_path', 'sfu_container')}
    target.update(local_service_url='http://127.0.0.1:17880', expected_vcpus=16,
                  expected_memory_gib=64, minimum_public_egress_mbps=20)
    return target


def freeze_path_inputs(output: Path, source_identity: Path, target_config: Path):
    base = freeze.base
    source = freeze.hd._validate_source_identity(base._load_json(source_identity))
    prerequisite = source['remote_prerequisite']
    target = _target_from_source(source)
    identity = remote.snapshot_identity(target_config)
    if identity['host'] != '81.71.85.246' or identity['user'] != 'ubuntu' or \
            urlsplit(target['service_url']).hostname != identity['host'] or urlsplit(target['service_url']).port != 17880:
        raise ValueError('single_source_target_mismatch')
    identity = {'config_path': str(target_config.resolve()), **identity}
    checkpoint = prerequisite['prerequisite_checkpoint_file']
    if base._file_record(Path(checkpoint['path'])) != checkpoint:
        raise ValueError('single_source_prerequisite_checkpoint_changed')
    files = {name: base._file_record(Path(__file__).parent / name) for name in TOOL_NAMES}
    value = {'schema': 1, 'scope': PATH_INPUT_SCOPE, 'status': 'FROZEN',
        'formal_b11_status': 'NOT_RUN', 'target': target, 'source_identity': source,
        'source_identity_file': base._file_record(source_identity), 'tools': files,
        'remote_transport_identity': identity, 'prerequisite_checkpoint_file': checkpoint}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')
    return value


def verify_path_inputs(manifest: Path):
    value = freeze.base._load_json(manifest)
    expected_fields = {'schema', 'scope', 'status', 'formal_b11_status', 'target', 'source_identity',
        'source_identity_file', 'tools', 'remote_transport_identity', 'prerequisite_checkpoint_file'}
    if type(value) is not dict or set(value) != expected_fields or value['schema'] != 1 or \
            value['scope'] != PATH_INPUT_SCOPE or value['status'] != 'FROZEN' or value['formal_b11_status'] != 'NOT_RUN' or \
            set(value['tools']) != set(TOOL_NAMES):
        raise ValueError('invalid_single_source_input_freeze')
    for record in [value['source_identity_file'], value['prerequisite_checkpoint_file'], *value['tools'].values()]:
        if freeze.base._file_record(Path(record['path'])) != record:
            raise ValueError('single_source_frozen_inputs_changed')
    if freeze.hd._validate_source_identity(freeze.base._load_json(Path(value['source_identity_file']['path']))) != value['source_identity']:
        raise ValueError('single_source_identity_changed')
    shared._pinned_config(value['remote_transport_identity'])
    source, target = value['source_identity'], value['target']
    if not freeze.base._same_typed(target, _target_from_source(source)) or \
            value['remote_transport_identity']['host'] != '81.71.85.246' or \
            value['remote_transport_identity']['user'] != 'ubuntu':
        raise ValueError('single_source_target_changed')
    for name in ('instance', 'service_url', 'config_path', 'sfu_container'):
        if target[name] != source['remote_prerequisite'][name]:
            raise ValueError('single_source_target_changed')
    return value


def setup_command(request):
    source, target = request['source'], request['target']
    program = r'''
import hashlib,json,os,pathlib,subprocess
request = REQUEST
target,source=request['target'],request['source']
expected=source['remote_prerequisite']
config=pathlib.Path(target['config_path']).read_bytes()
inspected=json.loads(subprocess.check_output(['sudo','-n','docker','inspect',target['sfu_container']],text=True,timeout=15))[0]
observed={'config_sha256':hashlib.sha256(config).hexdigest(),
 'cli_sha256':hashlib.sha256(pathlib.Path(source['cli_path']).read_bytes()).hexdigest(),
 'sfu_image':inspected['Image'],'logical_cpus_observed':os.cpu_count(),
 'mem_total_kib_observed':int(next(line.split()[1] for line in pathlib.Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemTotal:')))}
if any(observed[name]!=expected[name] for name in ('config_sha256','sfu_image','logical_cpus_observed','mem_total_kib_observed')) or observed['cli_sha256']!=source['cli_sha256'] or not inspected['State']['Running']:
 raise RuntimeError('remote_prerequisite_changed')
pathlib.Path(request['directory']).mkdir(mode=0o700,exist_ok=False)
print(json.dumps({'ok':True,'remote_inputs':observed}))
'''.replace('REQUEST', repr(request), 1)
    return ("python3 - <<'B11_PATH_PY'\n" + program + "\nB11_PATH_PY\n").encode()


def network_command(operation, request, tool_hashes):
    # Python repr preserves literal input; only static, quoted executable arguments
    # enter the shell. The controller rechecks shared prerequisites internally.
    program = "import hashlib,json,pathlib,subprocess\n"
    program += 'directory=pathlib.Path(' + repr(request['directory']) + ')\n'
    program += 'expected=' + repr(tool_hashes) + '\n'
    program += "for name,digest in expected.items():\n if hashlib.sha256((directory/name).read_bytes()).hexdigest()!=digest: raise RuntimeError('remote_tool_fingerprint_changed')\n"
    arguments = ['sudo', '-n', 'python3', request['directory'] + '/b11_publisher_network.py',
                 '--operation', operation, '--request-file', request['directory'] + '/network-request.json']
    program += 'result=subprocess.run(' + repr(arguments) + ',capture_output=True,text=True,timeout=70)\n'
    program += "value=json.loads(result.stdout)\nprint(json.dumps(value))\n"
    return ("python3 - <<'B11_PATH_PY'\n" + program + "\nB11_PATH_PY\n").encode()


def execute(output: Path, *, target: dict, source: dict, identity: dict, tool_hashes: dict,
            publisher: dict, observer=None):
    output = output.resolve()
    if output.exists():
        raise ValueError('100_output_already_exists')
    output.mkdir(parents=True, exist_ok=False)
    prepared = output / 'prepared'
    prepared.mkdir()
    remote_evidence = output / 'remote'
    remote_evidence.mkdir()
    owner_id = uuid.uuid4().hex
    room = 'b11-100-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + owner_id[:12]
    request = {'schema': 1, 'scope': 'B11_100_TASK_NETWORK', 'owner_id': owner_id, 'room': room,
        'directory': '/tmp/' + room, 'target': target, 'source': source, 'publisher': publisher}
    request_path = prepared / 'network-request.json'
    soak.atomic_json(request_path, request)
    remote_hashes = {**tool_hashes, 'network-request.json': soak.sha256(request_path)}
    credential_path = prepared / 'observer.json'
    original_environment = {name: os.environ.get(name) for name in shared.ENVIRONMENT}
    result = {'schema': 1, 'scope': freeze.SCOPE if observer else freeze.PATH_GATE_SCOPE,
        'status': 'INCONCLUSIVE', 'primary_result': 'INCONCLUSIVE', 'reason': 'not_started',
        'formal_b11_status': 'NOT_RUN', 'diagnostic_only': True, 'release_eligible': False,
        'cleanup_issues': [], 'cleanup_status': 'UNKNOWN', 'remote_directory': request['directory']}
    result['server_resource_status'] = 'NOT_RUN'
    remote_created = modules_ready = awake = False
    full_started = sampler_started = False
    gate = None

    def call(operation):
        response = remote.execute(shared._pinned_config(identity), network_command(operation, request, remote_hashes))
        value = json.loads(response['stdout'])
        if not value.get('ok'):
            raise RuntimeError('task_network_' + operation + '_failed')
        return value

    def remove_local_credential():
        try:
            credential_path.unlink(missing_ok=True)
        except OSError:
            result['cleanup_issues'].append('local_credential_cleanup_failed')

    try:
        awake = True
        if not shared._set_execution_state(0x80000003):
            raise RuntimeError('desktop_execution_state_unavailable')
        remote_created = True
        response = remote.execute(shared._pinned_config(identity), setup_command(request))
        setup = json.loads(response['stdout'])
        if not setup.get('ok'):
            raise RuntimeError('task_remote_setup_failed')
        for name in UPLOAD_NAMES:
            if soak.sha256(Path(__file__).parent / name) != tool_hashes[name]:
                raise ValueError('task_network_inputs_changed')
            remote.upload(shared._pinned_config(identity), Path(__file__).parent / name, request['directory'] + '/' + name)
        remote.upload(shared._pinned_config(identity), request_path, request['directory'] + '/network-request.json')
        modules_ready = True
        call('capcheck')
        created = call('create')
        soak.atomic_json(remote_evidence / 'network-created.json', created)
        gate = call('gate')['gate']
        soak.atomic_json(output / 'path-gate-measurement.json', gate)
        if gate['status'] != 'PASS':
            result.update(status=gate['status'], primary_result=gate['status'], reason='private_media_path_gate_not_passed')
        elif observer is None:
            result.update(status='PASS', primary_result='PASS', reason='single_source_private_media_path_confirmed')
        else:
            # No 100-source process is launched unless this fresh owner's own
            # packet gate passes; an earlier gate cannot substitute for this run.
            freeze.validate_path_gate(gate, {'target': target}, source, require_cleanup=False)
            if gate['task_owner']['owner_id'] != owner_id or gate['task_owner']['room'] != room or \
                    gate['task_owner']['remote_directory'] != request['directory']:
                raise ValueError('fresh_media_gate_owner_mismatch')
            started = call('start')
            full_started = True
            soak.atomic_json(remote_evidence / 'publisher-started.json', started)
            soak.atomic_json(remote_evidence / 'sampler-started.json', call('sampler'))
            sampler_started = True
            result['server_resource_status'] = 'UNKNOWN'
            call('credentials')
            remote.download(shared._pinned_config(identity), request['directory'] + '/observer.json', credential_path)
            try:
                values = json.loads(credential_path.read_text())
                if set(values) != set(shared.ENVIRONMENT) or values['LIVEKIT_URL'] != target['service_url']:
                    raise ValueError('observer_credential_invalid')
                for name in shared.ENVIRONMENT:
                    if type(values[name]) is not str or not values[name]:
                        raise ValueError('observer_credential_invalid')
                    os.environ[name] = values[name]
            finally:
                remove_local_credential()
            del values
            call('remove_credentials')
            observed = observer(output / 'observer')
            result.update(status=observed['status'], primary_result=observed['status'],
                          reason=observed.get('reason', 'observer_completed'))
            publication = call('snapshot')
            soak.atomic_json(remote_evidence / 'publication-snapshot.json', publication)
            result['publication_status'] = 'PASS' if publication['all_sources_advertise_hd'] else 'FAIL'
            if not publication['all_sources_advertise_hd']:
                result.update(status='FAIL', primary_result='FAIL', reason='100_hd_publications_not_confirmed')
    except (OSError, ValueError, RuntimeError) as error:
        result['reason'] = shared._safe_failure_reason(error)
        if result['primary_result'] != 'FAIL':
            result['status'] = 'INCONCLUSIVE'
    finally:
        remove_local_credential()
        for name, value in original_environment.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        if modules_ready:
            for operation in ('stop', 'remove_credentials', 'delete'):
                try:
                    cleanup = call(operation)
                    soak.atomic_json(remote_evidence / ('cleanup-' + operation + '.json'), cleanup)
                    if operation == 'stop' and not cleanup['all_stopped'] or \
                            operation == 'delete' and not cleanup['network_removed']:
                        result['cleanup_issues'].append(operation + '_incomplete')
                    if cleanup.get('prerequisite_status') == 'UNKNOWN':
                        result['cleanup_issues'].append('remote_prerequisite_changed')
                    if gate is not None and operation == 'stop':
                        gate['cleanup'].update(publisher_stopped=cleanup['all_stopped'], subscriber_stopped=cleanup['all_stopped'])
                    if gate is not None and operation == 'delete':
                        gate['cleanup']['network_removed'] = cleanup['network_removed']
                    if gate is not None and operation == 'remove_credentials':
                        gate['cleanup']['credentials_removed'] = cleanup['credential_removed']
                except (OSError, ValueError, RuntimeError):
                    result['cleanup_issues'].append(operation + '_cleanup_failed')
            names = ['trial-state.json'] + (['publisher-state.json'] if full_started else []) + (['sampler-state.json',
                     'ecs-metrics.csv', 'ecs-metrics.status.json',
                     'namespace-metrics.csv', 'namespace-metrics.status.json'] if sampler_started else [])
            for name in names:
                try:
                    # Controller metadata is root-owned. Copy only its closed,
                    # credential-free JSON (or numeric CSV) via a sudo read.
                    script = "python3 - <<'B11_READ_PY'\nimport json,subprocess\n"
                    script += 'value=subprocess.check_output(' + repr(['sudo', '-n', 'cat', request['directory'] + '/' + name]) + ',text=True,timeout=15)\n'
                    if name.endswith('.json'):
                        script += 'print(json.dumps({"value":json.loads(value)}))\n'
                        metadata = json.loads(remote.execute(shared._pinned_config(identity),
                            (script + 'B11_READ_PY\n').encode())['stdout'])['value']
                        soak.atomic_json(remote_evidence / name, metadata)
                    else:
                        # The sampler CSV is numeric and has no credentials;
                        # change only this task's file ownership for scp.
                        quote_path = shlex.quote(request['directory'] + '/' + name)
                        command = ('sudo -n chown ubuntu:ubuntu ' + quote_path + '\n'
                            + "printf '{\"ok\":true}\\n'\n").encode()
                        remote.execute(shared._pinned_config(identity), command)
                        remote.download(shared._pinned_config(identity), request['directory'] + '/' + name, remote_evidence / name)
                except (OSError, ValueError, RuntimeError):
                    result['cleanup_issues'].append('remote_evidence_missing:' + name)
            if sampler_started:
                resources_complete = True
                for name, counter in (('ecs-metrics.status.json', 'samples'),
                                      ('namespace-metrics.status.json', 'sample_count')):
                    try:
                        status = json.loads((remote_evidence / name).read_text())
                        if status.get('error') is not None or type(status.get(counter)) is not int or status[counter] < 300:
                            raise ValueError('resource_sampling_incomplete')
                    except (OSError, ValueError):
                        resources_complete = False
                        result['cleanup_issues'].append('resource_sampling_incomplete:' + name)
                result['server_resource_status'] = 'AVAILABLE' if resources_complete else 'UNKNOWN'
        elif remote_created:
            result['cleanup_issues'].append('network_modules_not_ready_no_network_started')
        if gate is not None:
            if result['cleanup_issues'] and gate['status'] == 'PASS':
                gate['status'] = 'INCONCLUSIVE'
            soak.atomic_json(output / 'path-gate.json', gate)
        if awake:
            try:
                if not shared._set_execution_state(0x80000000):
                    result['cleanup_issues'].append('display_sleep_restore_failed')
            except OSError:
                result['cleanup_issues'].append('display_sleep_restore_failed')
        result['cleanup_status'] = 'COMPLETE' if not result['cleanup_issues'] else 'UNKNOWN'
        if result['cleanup_issues'] and result['primary_result'] != 'FAIL':
            result['status'] = 'INCONCLUSIVE'
        soak.atomic_json(output / 'runner-summary.json', result)
    return result


def path_preflight(output, manifest, *, plan=False):
    inputs = verify_path_inputs(manifest)
    if output.exists():
        raise ValueError('path_preflight_output_exists')
    if plan:
        return {'status': 'PASS', 'phase': 'SINGLE_SOURCE_FROZEN_PLAN', 'network_operations': 0,
                'publisher_count': 1, 'subscriber_count': 1, 'formal_b11_status': 'NOT_RUN'}
    group = {**freeze.PUBLISHER, 'identity_prefix': 'b11grid100_pathprobe'}
    return execute(output, target=inputs['target'], source=inputs['source_identity'],
        identity=inputs['remote_transport_identity'],
        tool_hashes={name: inputs['tools'][name]['sha256'] for name in UPLOAD_NAMES}, publisher=group)


def run(output, executable, *, input_manifest, profile, plan=False):
    frozen = freeze.verify_inputs(input_manifest, executable, profile, require_remote=True)
    bound, inputs = frozen['inputs']['profile'], frozen['inputs']
    if output.exists():
        raise ValueError('100_output_already_exists')
    if plan:
        return {'status': 'PASS', 'phase': '100_SOURCE_FROZEN_PLAN', 'network_operations': 0,
                'publisher_count': 100, 'receiver_count': 1, 'formal_b11_status': 'NOT_RUN'}
    tool_hashes = {name: inputs['source_inputs']['tests/runtime/tools/meeting/' + name]['sha256'] for name in UPLOAD_NAMES}
    import b11_100_grid_probe
    return execute(output, target=bound['target'], source=inputs['source_identity'],
        identity=inputs['remote_transport_identity'], tool_hashes=tool_hashes, publisher=bound['publisher'],
        observer=lambda child: b11_100_grid_probe.run(child, executable, input_manifest=input_manifest, profile=profile))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='operation', required=True)
    create = sub.add_parser('freeze-path')
    create.add_argument('--source-identity', type=Path, required=True)
    create.add_argument('--target-config', type=Path, required=True)
    create.add_argument('--output', type=Path, required=True)
    path = sub.add_parser('path-preflight')
    path.add_argument('--input-manifest', type=Path, required=True)
    path.add_argument('--output', type=Path, required=True)
    path.add_argument('--plan', action='store_true')
    full = sub.add_parser('run')
    for name in ('output', 'executable', 'input-manifest', 'profile'):
        full.add_argument('--' + name, type=Path, required=True)
    full.add_argument('--plan', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.operation == 'freeze-path':
            freeze_path_inputs(args.output, args.source_identity, args.target_config)
            result = {'status': 'PASS', 'scope': PATH_INPUT_SCOPE, 'phase': 'INPUTS_FROZEN', 'formal_b11_status': 'NOT_RUN'}
        elif args.operation == 'path-preflight':
            result = path_preflight(args.output, args.input_manifest, plan=args.plan)
        else:
            result = run(args.output, args.executable, input_manifest=args.input_manifest, profile=args.profile, plan=args.plan)
    except (OSError, ValueError, RuntimeError):
        result = {'status': 'INCONCLUSIVE', 'reason': 'frozen_inputs_or_task_network_unavailable', 'formal_b11_status': 'NOT_RUN'}
    print(json.dumps(result))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
