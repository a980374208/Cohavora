"""Frozen SSH orchestration for the independent B11 HD/layer diagnostic."""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import inspect
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import uuid

import b11_hd_input_freeze as freeze
import b11_remote as remote
import meeting_soak as soak

ROOT = Path(__file__).resolve().parents[4]
ENVIRONMENT = ("LIVEKIT_URL", "LIVEKIT_SOAK_TOKEN", "LIVEKIT_SOAK_ALLOW_INSECURE")


def publisher_argv(cli_path: str, room: str, group: dict) -> list[str]:
    args = [cli_path, "load-test", "--room", room, "--duration", group["duration"],
        "--video-publishers", str(group["count"]), "--subscribers", str(group["subscribers"]),
        "--video-resolution", group["resolution"], "--video-codec", group["codec"],
        "--num-per-second", str(group["num_per_second"]),
        "--identity-prefix", group["identity_prefix"], "--yes"]
    if not group["simulcast"]:
        args.append("--no-simulcast")
    return args


def process_identity(pid, proc_root=Path("/proc")):
    try:
        directory = proc_root / str(pid)
        fields = (directory / "stat").read_text().rsplit(")", 1)[1].split()
        if fields[0] == "Z":
            return None
        argv = (directory / "cmdline").read_bytes().split(b"\0")
        if argv and argv[-1] == b"":
            argv.pop()
        return {"pid": int(pid), "start_ticks": fields[19],
            "argv": [value.decode("utf-8") for value in argv]}
    except (FileNotFoundError, ProcessLookupError):
        return None


def stop_owned(owner, *, proc_root=Path("/proc"), kill=os.kill,
               monotonic=time.monotonic, sleep=time.sleep):
    """Signal only an exact PID/start-time/argv match; verify bounded exit."""
    result = {"pid": owner["pid"], "owned": False, "stopped": False}
    def current():
        value = process_identity(owner["pid"], proc_root)
        if value is None or value["start_ticks"] != owner["start_ticks"]:
            return None
        return value
    for sig, grace in ((signal.SIGINT, 3), (signal.SIGTERM, 5)):
        value = current()
        if value is None:
            result["stopped"] = True
            return result
        if value["argv"] != owner["argv"]:
            result["reason"] = "ownership_mismatch"
            return result
        result["owned"] = True
        try:
            kill(owner["pid"], sig)
        except ProcessLookupError:
            result["stopped"] = True
            return result
        deadline = monotonic() + grace
        while monotonic() < deadline:
            value = current()
            if value is None:
                result["stopped"] = True
                return result
            if value["argv"] != owner["argv"]:
                result["reason"] = "ownership_mismatch"
                return result
            sleep(.1)
    result["reason"] = "stop_timeout"
    return result


REMOTE_PROGRAM = r'''
root = Path(PAYLOAD['directory'])
target, source = PAYLOAD['target'], PAYLOAD['source']
expected = source['remote_prerequisite']

def atomic_metadata(value):
    temporary = root / 'metadata.json.new'
    temporary.write_text(json.dumps(value))
    temporary.chmod(0o600)
    temporary.replace(root / 'metadata.json')

def prerequisite():
    config_path = Path(target['config_path'])
    try:
        config_bytes = config_path.read_bytes()
    except PermissionError:
        config_bytes = subprocess.check_output(['sudo', '-n', 'cat', str(config_path)], timeout=15)
    inspected = json.loads(subprocess.check_output(
        ['sudo', '-n', 'docker', 'inspect', target['sfu_container']], text=True, timeout=15))[0]
    observed = {
        'config_sha256': hashlib.sha256(config_bytes).hexdigest(),
        'cli_sha256': hashlib.sha256(Path(source['cli_path']).read_bytes()).hexdigest(),
        'sfu_image': inspected['Image'], 'logical_cpus_observed': os.cpu_count(),
        'mem_total_kib_observed': int(next(line.split()[1] for line in
            Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemTotal:'))),
        'sfu_pid': int(inspected['State']['Pid'])}
    matches = all(observed[name] == expected[name] for name in
        ('config_sha256', 'sfu_image', 'logical_cpus_observed', 'mem_total_kib_observed'))
    matches = matches and observed['cli_sha256'] == source['cli_sha256'] and \
        inspected['State']['Running'] is True and observed['sfu_pid'] > 0
    if not matches:
        raise RuntimeError('remote_prerequisite_changed')
    sampler_path = root / 'ecs_resource_sampler.py'
    if (PAYLOAD['operation'] == 'start' or sampler_path.exists()) and \
            hashlib.sha256(sampler_path.read_bytes()).hexdigest() != PAYLOAD['sampler_sha256']:
        raise RuntimeError('remote_sampler_fingerprint_changed')
    return config_bytes, observed

def cleanup(metadata):
    results = []
    owners = ([metadata['sampler']] if metadata.get('sampler') else []) + \
        list(reversed(metadata.get('publishers', [])))
    for owner in owners:
        try:
            result = stop_owned(owner)
        except Exception:
            result = {'pid': owner.get('pid'), 'stopped': False, 'reason': 'owner_cleanup_failed'}
        results.append(result)
    try:
        (root / 'observer.json').unlink(missing_ok=True)
        credential_removed = not (root / 'observer.json').exists()
    except OSError:
        credential_removed = False
    return {'owners': results, 'all_stopped': all(item['stopped'] for item in results),
        'credential_removed': credential_removed}

try:
    if PAYLOAD['operation'] == 'cleanup':
        try:
            _, observed = prerequisite()
            prerequisite_status = 'MATCH'
        except Exception:
            observed, prerequisite_status = None, 'UNKNOWN'
        metadata_issue = root.exists() and not (root / 'metadata.json').exists()
        try:
            metadata = json.loads((root / 'metadata.json').read_text()) if \
                (root / 'metadata.json').exists() else {'publishers': [], 'sampler': None}
        except Exception:
            metadata, metadata_issue = {'publishers': [], 'sampler': None}, True
        if metadata.get('publishers') or metadata.get('sampler'):
            if metadata.get('scope') != 'B11_HD_LAYER_DIAGNOSTIC' or \
                    metadata.get('room') != PAYLOAD['room'] or \
                    metadata.get('remote_directory') != str(root):
                metadata, metadata_issue = {'publishers': [], 'sampler': None}, True
        response = {'ok': True, 'prerequisite_status': prerequisite_status,
            'remote_inputs': observed, **cleanup(metadata)}
        if metadata_issue:
            response.update(all_stopped=False, reason='remote_task_ownership_unknown')
        if root.exists():
            (root / 'cleanup.json').write_text(json.dumps(response))
    else:
        config_bytes, observed = prerequisite()
        operation = PAYLOAD['operation']
        if operation == 'setup':
            root.mkdir(mode=0o700, exist_ok=False)
            metadata = {'schema': 1, 'scope': 'B11_HD_LAYER_DIAGNOSTIC',
                'room': PAYLOAD['room'], 'remote_directory': str(root),
                'state': 'PREPARED', 'publishers': [], 'sampler': None,
                'remote_inputs': observed}
            atomic_metadata(metadata)
            response = {'ok': True, 'remote_inputs': observed}
        elif operation == 'start':
            import yaml
            metadata = json.loads((root / 'metadata.json').read_text())
            if metadata['state'] != 'PREPARED' or metadata['publishers'] or metadata['sampler']:
                raise RuntimeError('remote_state_not_prepared')
            config = yaml.safe_load(config_bytes)
            key, secret = next(iter(config['keys'].items()))
            environment = os.environ.copy()
            environment.update(LIVEKIT_URL=target['local_service_url'],
                LIVEKIT_API_KEY=str(key), LIVEKIT_API_SECRET=str(secret))
            launched = []
            def launch(argv, role):
                child = subprocess.Popen(argv, env=environment if role != 'sampler' else os.environ.copy(), stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, start_new_session=True)
                launched.append(child)
                owner = process_identity(child.pid)
                if owner is None or owner['argv'] != argv:
                    raise RuntimeError('launched_owner_not_verified')
                owner['role'] = role
                if role == 'sampler':
                    metadata['sampler'] = owner
                else:
                    metadata['publishers'].append(owner)
                atomic_metadata(metadata)
                return child
            try:
                for group in PAYLOAD['groups']:
                    launch(group['argv'], group['role'])
                launch([sys.executable, str(root / 'ecs_resource_sampler.py'), '--output',
                    str(root / 'ecs-metrics.csv'), '--pid', str(observed['sfu_pid']),
                    '--duration', str(PAYLOAD['sampler_seconds'])], 'sampler')
                time.sleep(2)
                if any(child.poll() is not None for child in launched):
                    raise RuntimeError('remote_worker_exited_early')
                credential = subprocess.check_output([source['cli_path'], 'token', 'create',
                    '--room', PAYLOAD['room'], '--identity', 'b11-hd-observer', '--join',
                    '--valid-for', '15m', '--token-only'], env=environment,
                    text=True, timeout=15).strip()
                if not credential or len(credential) > 8192:
                    raise RuntimeError('invalid_observer_credential')
                with (root / 'observer.json').open('x') as stream:
                    json.dump({'LIVEKIT_URL': target['service_url'],
                        'LIVEKIT_SOAK_TOKEN': credential, 'LIVEKIT_SOAK_ALLOW_INSECURE': '1'}, stream)
                (root / 'observer.json').chmod(0o600)
                metadata['state'] = 'RUNNING'
                atomic_metadata(metadata)
                response = {'ok': True, 'state': 'RUNNING', 'publishers': metadata['publishers'],
                    'sampler': metadata['sampler'], 'remote_inputs': observed}
            except BaseException:
                metadata['state'] = 'FAILED'
                rollback = cleanup(metadata)
                # Unreaped children cannot have a reused PID: use their Popen owner
                # only when identity capture failed before it reached metadata.
                recorded = {item['pid'] for item in metadata['publishers']}
                if metadata.get('sampler'):
                    recorded.add(metadata['sampler']['pid'])
                for child in launched:
                    if child.pid not in recorded and child.poll() is None:
                        try:
                            child.send_signal(signal.SIGINT)
                            child.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            child.terminate()
                            try:
                                child.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                rollback['all_stopped'] = False
                        except Exception:
                            rollback['all_stopped'] = False
                metadata['rollback'] = rollback
                atomic_metadata(metadata)
                raise
        elif operation == 'remove_credentials':
            (root / 'observer.json').unlink(missing_ok=True)
            response = {'ok': True, 'credential_removed': not (root / 'observer.json').exists()}
        elif operation == 'verify':
            response = {'ok': True, 'remote_inputs': observed}
        else:
            raise RuntimeError('unsupported_hd_remote_operation')
except BaseException as error:
    response = {'ok': False, 'reason': str(error) if isinstance(error, RuntimeError) else type(error).__name__}
print(json.dumps(response))
'''


def remote_command(operation: str, *, directory: str, room: str, profile: dict,
                   source: dict, sampler_sha256: str | None = None) -> bytes:
    payload = {'operation': operation, 'directory': directory, 'room': room,
        'target': profile['target'], 'source': source,
        'groups': [{'role': role, 'argv': publisher_argv(source['cli_path'], room, group)}
                   for role, group in profile['publishers'].items()],
        'sampler_seconds': profile['probe']['maximum_wall_seconds'] + 60,
        'sampler_sha256': sampler_sha256}
    program = "import hashlib, json, os, signal, subprocess, sys, time\nfrom pathlib import Path\n"
    program += inspect.getsource(process_identity) + "\n" + inspect.getsource(stop_owned) + "\n"
    program += "PAYLOAD = " + repr(payload) + "\n" + REMOTE_PROGRAM
    return ("python3 - <<'B11_HD_PY'\n" + program + "\nB11_HD_PY\n").encode('utf-8')


def _pinned_config(identity: dict) -> dict:
    current = remote.snapshot_identity(Path(identity['config_path']))
    if any(current.get(key) != value for key, value in identity.items() if key != 'config_path'):
        raise ValueError('frozen_ssh_identity_changed')
    return {'schema': 1, **{key: current[key] for key in remote.FIELDS - {'schema'}}}


def _safe_failure_reason(error: Exception) -> str:
    value = str(error)
    return value if re.fullmatch(r'[a-z][a-z0-9_]{0,127}', value) else type(error).__name__


def _set_execution_state(flags: int) -> bool:
    if os.name != 'nt':
        return True
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.SetThreadExecutionState.argtypes = [ctypes.c_uint32]
    kernel.SetThreadExecutionState.restype = ctypes.c_uint32
    return bool(kernel.SetThreadExecutionState(flags))


def run(output: Path, executable: Path, *, input_manifest: Path, profile: Path,
        prepared_directory: Path | None = None, plan=False) -> dict:
    frozen = freeze.verify_inputs(input_manifest, executable, profile, require_remote=True)
    bound = frozen['inputs']['profile']
    source = frozen['inputs']['source_identity']
    identity = frozen['inputs']['remote_transport_identity']
    sampler_path = ROOT / 'tests/runtime/tools/meeting/ecs_resource_sampler.py'
    sampler_sha256 = frozen['inputs']['source_inputs'][
        'tests/runtime/tools/meeting/ecs_resource_sampler.py']['sha256']
    if soak.sha256(sampler_path) != sampler_sha256:
        raise ValueError('frozen_sampler_inputs_changed')
    output = output.resolve()
    prepared = (prepared_directory or output.with_name(output.name + '-prepared')).resolve()
    if output.is_relative_to(prepared) or prepared.is_relative_to(output) or output.exists() or prepared.exists():
        raise ValueError('hd_output_or_prepared_already_exists')
    if plan:
        return {'scope': freeze.SCOPE, 'status': 'PASS', 'phase': 'FROZEN_PLAN',
            'runtime_status': 'NOT_RUN', 'remote_operations': 0,
            'hd_identity': freeze.hd_source_identity(bound),
            'output': str(output), 'prepared_directory': str(prepared), 'formal_b11_status': 'NOT_RUN'}
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    room = 'b11-hd-' + stamp + '-' + uuid.uuid4().hex[:8]
    directory = '/tmp/' + room
    output.mkdir(parents=True, exist_ok=False)
    prepared.mkdir(parents=True, exist_ok=False)
    evidence = output / 'remote'
    evidence.mkdir()
    credential_path = prepared / 'observer.json'
    original_environment = {name: os.environ.get(name) for name in ENVIRONMENT}
    summary = {'schema': 1, 'scope': freeze.SCOPE, 'room': room,
        'status': 'INCONCLUSIVE', 'reason': 'not_started', 'formal_b11_status': 'NOT_RUN',
        'primary_result': 'INCONCLUSIVE', 'cleanup_status': 'NOT_RUN',
        'diagnostic_only': True, 'release_eligible': False, 'cleanup_issues': [],
        'input_manifest_sha256': soak.sha256(input_manifest), 'remote_directory': directory}
    started = False
    awake_requested = False

    def remove_local_credential():
        try:
            credential_path.unlink(missing_ok=True)
            return not credential_path.exists()
        except OSError:
            if 'local_credential_cleanup_failed' not in summary['cleanup_issues']:
                summary['cleanup_issues'].append('local_credential_cleanup_failed')
            return False

    def command(operation):
        result = remote.execute(_pinned_config(identity), remote_command(operation,
            directory=directory, room=room, profile=bound, source=source, sampler_sha256=sampler_sha256))
        response = json.loads(result['stdout'])
        if not response.get('ok'):
            raise RuntimeError('hd_remote_' + operation + '_failed')
        return response

    try:
        awake_requested = True
        if not _set_execution_state(0x80000003):
            raise RuntimeError('desktop_execution_state_unavailable')
        # Mark the known task directory before setup, so a lost setup response still
        # reaches cleanup. No process may launch before this first prerequisite check.
        started = True
        command('setup')
        command('verify')
        remote.upload(_pinned_config(identity), sampler_path,
            directory + '/ecs_resource_sampler.py')
        launched = command('start')
        soak.atomic_json(evidence / 'launched.json', launched)
        command('verify')
        remote.download(_pinned_config(identity), directory + '/observer.json', credential_path)
        try:
            credential = json.loads(credential_path.read_text(encoding='utf-8'))
            if set(credential) != set(ENVIRONMENT) or credential['LIVEKIT_URL'] != bound['target']['service_url']:
                raise ValueError('invalid_observer_credential')
            for name in ENVIRONMENT:
                if type(credential[name]) is not str or not credential[name]:
                    raise ValueError('invalid_observer_credential')
                os.environ[name] = credential[name]
        finally:
            if not remove_local_credential():
                raise RuntimeError('local_credential_cleanup_failed')
        del credential
        command('remove_credentials')
        import b11_hd_layer_probe
        observer = b11_hd_layer_probe.run(output / 'observer', executable,
            input_manifest=input_manifest, profile=profile)
        summary.update(status=observer['status'], primary_result=observer['status'],
            reason=observer.get('reason', 'observer_completed'),
            observer_summary_path=str(output / 'observer' / 'summary.json'))
    except (OSError, ValueError, RuntimeError) as error:
        summary['reason'] = _safe_failure_reason(error)
    finally:
        remove_local_credential()
        for name, value in original_environment.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        if started:
            try:
                cleaned = command('cleanup')
                soak.atomic_json(evidence / 'cleanup.json', cleaned)
                if not cleaned['all_stopped'] or not cleaned['credential_removed']:
                    summary['cleanup_issues'].append('remote_owner_or_credential_cleanup_incomplete')
                if cleaned['prerequisite_status'] != 'MATCH':
                    summary['cleanup_issues'].append('remote_prerequisite_drift')
            except (OSError, ValueError, RuntimeError):
                summary['cleanup_issues'].append('remote_cleanup_failed')
            # Downloads continue independently; one missing sampler file must not
            # hide owner metadata or undo the already attempted cleanup.
            for name in ('metadata.json', 'ecs-metrics.csv', 'ecs-metrics.status.json'):
                try:
                    remote.download(_pinned_config(identity), directory + '/' + name, evidence / name)
                except (OSError, ValueError, RuntimeError):
                    summary['cleanup_issues'].append('evidence_unavailable:' + name)
        if awake_requested:
            try:
                if not _set_execution_state(0x80000000):
                    summary['cleanup_issues'].append('desktop_execution_state_restore_failed')
            except OSError:
                summary['cleanup_issues'].append('desktop_execution_state_restore_failed')
        summary['cleanup_status'] = 'COMPLETE' if started and not summary['cleanup_issues'] else 'UNKNOWN'
        if summary['cleanup_issues'] and summary['primary_result'] != 'FAIL':
            summary['status'] = 'INCONCLUSIVE'
        summary['finished_utc'] = soak.utc_now()
        soak.atomic_json(output / 'runner-summary.json', summary)
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--input-manifest', type=Path, required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--prepared-directory', type=Path)
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args(argv)
    try:
        result = run(args.output, args.executable, input_manifest=args.input_manifest,
            profile=args.profile, prepared_directory=args.prepared_directory, plan=args.plan)
    except (OSError, ValueError, RuntimeError):
        print(json.dumps({'scope': freeze.SCOPE, 'status': 'INCONCLUSIVE',
            'reason': 'hd_frozen_inputs_or_local_paths_not_valid', 'formal_b11_status': 'NOT_RUN'}))
        return 1
    print(json.dumps(result, ensure_ascii=True))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
