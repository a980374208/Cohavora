"""Offline HD SSH admission, publisher grouping and exact owner cleanup checks."""
from __future__ import annotations

import ast
from copy import deepcopy
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'tools' / 'meeting'
sys.path.insert(0, str(TOOLS))
try:
    import b11_hd_remote_run as runner
finally:
    sys.path.remove(str(TOOLS))


class HdRunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='b11-hd-runner-selftest-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.exe = self.root / 'observer.exe'
        self.exe.write_bytes(b'inert binary; never executed')
        self.manifest = self.root / 'freeze.json'
        self.manifest.write_text('{}')
        self.profile_file = self.root / 'profile.json'
        self.profile_file.write_text('{}')
        groups = deepcopy(runner.freeze.PUBLISHERS)
        groups['hd']['identity_prefix'] = 'b11hd_selftest'
        groups['background']['identity_prefix'] = 'b11low_selftest'
        self.profile = {'publishers': groups, 'probe': dict(runner.freeze.PROBE),
            'target': {'service_url': 'ws://203.0.113.1:17880',
                       'local_service_url': 'http://127.0.0.1:17880',
                       'config_path': '/home/ubuntu/livekit.yml', 'sfu_container': 'livekit'}}
        self.source = {'cli_path': '/home/ubuntu/.local/bin/lk', 'cli_sha256': 'a' * 64,
                       'remote_prerequisite': {}}
        self.frozen = {'inputs': {'profile': self.profile, 'source_identity': self.source,
                                 'remote_transport_identity': {'config_path': 'fixture'},
                                 'source_inputs': {'tests/runtime/tools/meeting/ecs_resource_sampler.py': {
                                     'sha256': runner.soak.sha256(TOOLS / 'ecs_resource_sampler.py')}}}}
        self.output = self.root / 'hd-evidence'
        self.operations = []
        self.verify = self.enterContext(patch.object(runner.freeze, 'verify_inputs', return_value=self.frozen))
        self.enterContext(patch.object(runner.freeze, 'hd_source_identity', return_value='b11hd_selftest_pub_0'))
        self.pin = self.enterContext(patch.object(runner, '_pinned_config', return_value={'fixture': True}))
        self.execute = self.enterContext(patch.object(runner.remote, 'execute', side_effect=self.exec_remote))
        self.upload = self.enterContext(patch.object(runner.remote, 'upload'))
        self.download = self.enterContext(patch.object(runner.remote, 'download', side_effect=self.download_remote))
        self.execution_state = self.enterContext(patch.object(runner, '_set_execution_state', return_value=True))
        self.probe = SimpleNamespace(run=lambda *args, **kwargs: {'status': 'PASS', 'reason': 'fixture'})
        self.enterContext(patch.dict(sys.modules, {'b11_hd_layer_probe': self.probe}))

    def exec_remote(self, config, command):
        payload_line = next(line for line in command.decode().splitlines() if line.startswith('PAYLOAD = '))
        payload = ast.literal_eval(payload_line.removeprefix('PAYLOAD = '))
        self.operations.append(payload['operation'])
        response = {'ok': True}
        if payload['operation'] == 'cleanup':
            response.update(all_stopped=True, credential_removed=True, prerequisite_status='MATCH')
        return {'stdout': json.dumps(response)}

    def download_remote(self, config, remote_path, local_path):
        if remote_path.endswith('/observer.json'):
            local_path.write_text(json.dumps({'LIVEKIT_URL': self.profile['target']['service_url'],
                'LIVEKIT_SOAK_TOKEN': 'ephemeral-fixture-never-returned', 'LIVEKIT_SOAK_ALLOW_INSECURE': '1'}))
        else:
            local_path.write_text('fixture metadata')

    def run_fixture(self, **kwargs):
        return runner.run(self.output, self.exe, input_manifest=self.manifest,
                          profile=self.profile_file, **kwargs)

    def test_frozen_input_rejection_has_no_network_or_output_side_effects(self):
        self.verify.side_effect = ValueError('frozen_inputs_changed')
        with self.assertRaisesRegex(ValueError, 'frozen_inputs_changed'):
            self.run_fixture()
        self.execute.assert_not_called()
        self.upload.assert_not_called()
        self.download.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_plan_verifies_all_frozen_inputs_but_never_uses_network(self):
        result = self.run_fixture(plan=True)
        self.verify.assert_called_once_with(self.manifest, self.exe, self.profile_file, require_remote=True)
        self.assertEqual(result['remote_operations'], 0)
        self.assertEqual(result['hd_identity'], 'b11hd_selftest_pub_0')
        self.execute.assert_not_called()
        self.execution_state.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_group_arguments_preserve_one_hd_and_sixteen_background_sources(self):
        hd = runner.publisher_argv(self.source['cli_path'], 'fixture-room', self.profile['publishers']['hd'])
        low = runner.publisher_argv(self.source['cli_path'], 'fixture-room', self.profile['publishers']['background'])
        self.assertEqual(hd[hd.index('--video-publishers') + 1], '1')
        self.assertEqual(low[low.index('--video-publishers') + 1], '16')
        self.assertEqual(hd[hd.index('--video-resolution') + 1], 'high')
        self.assertEqual(low[low.index('--video-resolution') + 1], 'low')
        self.assertEqual(hd[hd.index('--identity-prefix') + 1], 'b11hd_selftest')
        self.assertEqual(low[low.index('--identity-prefix') + 1], 'b11low_selftest')
        self.assertNotIn('--no-simulcast', hd)
        self.assertNotIn('--no-simulcast', low)
        self.assertNotIn('ephemeral-fixture-never-returned', repr(hd + low))

    def test_remote_program_is_valid_python_and_payload_preserves_literal_shell_text(self):
        command = runner.remote_command('setup', directory='/tmp/b11-hd-selftest',
            room='b11-hd-selftest', profile=self.profile, source=self.source).decode()
        program = command.split("<<'B11_HD_PY'\n", 1)[1].rsplit('\nB11_HD_PY', 1)[0]
        ast.parse(program)
        self.assertIn('sudo', program)
        self.assertNotIn('docker restart', program)

    def test_observer_failure_always_cleans_remote_groups_and_restores_environment(self):
        def fail_probe(*args, **kwargs):
            self.assertEqual(os.environ['LIVEKIT_SOAK_TOKEN'], 'ephemeral-fixture-never-returned')
            self.assertFalse((self.output.with_name(self.output.name + '-prepared') / 'observer.json').exists())
            raise RuntimeError('observer_fixture_failure')
        self.probe.run = fail_probe
        with patch.dict(os.environ, {'LIVEKIT_SOAK_TOKEN': 'caller-fixture'}):
            result = self.run_fixture()
            self.assertEqual(os.environ['LIVEKIT_SOAK_TOKEN'], 'caller-fixture')
        self.assertEqual(result['status'], 'INCONCLUSIVE')
        self.assertEqual(self.operations[-1], 'cleanup')
        self.assertNotIn('ephemeral-fixture-never-returned', json.dumps(result))
        self.assertEqual(result['cleanup_issues'], [])
        self.assertEqual([call.args[0] for call in self.execution_state.call_args_list],
                         [0x80000003, 0x80000000])

    def test_unstructured_observer_error_does_not_persist_credentials(self):
        def fail_probe(*args, **kwargs):
            raise RuntimeError('observer rejected credential: ephemeral-fixture-never-returned')
        self.probe.run = fail_probe
        result = self.run_fixture()
        self.assertEqual(result['reason'], 'RuntimeError')
        self.assertNotIn('ephemeral-fixture-never-returned', json.dumps(result))
        self.assertEqual(self.operations[-1], 'cleanup')

    def test_setup_response_failure_still_attempts_cleanup(self):
        original = self.exec_remote
        def failed_setup(config, command):
            result = original(config, command)
            if self.operations[-1] == 'setup':
                raise runner.remote.RemoteError('ssh_operation_failed')
            return result
        self.execute.side_effect = failed_setup
        result = self.run_fixture()
        self.assertEqual(self.operations, ['setup', 'cleanup'])
        self.assertEqual(result['status'], 'INCONCLUSIVE')
        self.upload.assert_not_called()

    def test_cleanup_or_one_missing_evidence_file_does_not_skip_other_downloads(self):
        original = self.download_remote
        def failed_csv(config, path, local):
            if path.endswith('/ecs-metrics.csv'):
                raise runner.remote.RemoteError('ssh_operation_failed')
            return original(config, path, local)
        self.download.side_effect = failed_csv
        result = self.run_fixture()
        self.assertEqual(result['status'], 'INCONCLUSIVE')
        self.assertIn('evidence_unavailable:ecs-metrics.csv', result['cleanup_issues'])
        self.assertTrue((self.output / 'remote' / 'metadata.json').exists())
        self.assertTrue((self.output / 'remote' / 'ecs-metrics.status.json').exists())

    def test_cleanup_gap_preserves_confirmed_observer_fail(self):
        self.probe.run = lambda *args, **kwargs: {'status': 'FAIL', 'reason': 'hd_fps_below_threshold'}
        original = self.download_remote
        def failed_csv(config, path, local):
            if path.endswith('/ecs-metrics.csv'):
                raise runner.remote.RemoteError('ssh_operation_failed')
            return original(config, path, local)
        self.download.side_effect = failed_csv
        result = self.run_fixture()
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['primary_result'], 'FAIL')
        self.assertEqual(result['reason'], 'hd_fps_below_threshold')
        self.assertEqual(result['cleanup_status'], 'UNKNOWN')

    def test_cli_success_is_only_pass(self):
        arguments = ['--output', str(self.output), '--executable', str(self.exe),
            '--input-manifest', str(self.manifest), '--profile', str(self.profile_file)]
        for status, expected in (('PASS', 0), ('FAIL', 1), ('INCONCLUSIVE', 1)):
            with self.subTest(status=status), patch.object(runner, 'run', return_value={'status': status}), \
                    patch('builtins.print'):
                self.assertEqual(runner.main(arguments), expected)

    def test_exact_owner_mismatch_is_never_signaled(self):
        owner = {'pid': 1001, 'start_ticks': '55', 'argv': ['lk', 'load-test', '--room', 'owned-room']}
        with patch.object(runner, 'process_identity', return_value={**owner, 'argv': ['lk', 'other-task']}), \
                patch.object(runner.os, 'kill') as kill:
            result = runner.stop_owned(owner, kill=kill)
        kill.assert_not_called()
        self.assertEqual(result['reason'], 'ownership_mismatch')
        self.assertFalse(result['stopped'])

    def test_remote_cleanup_continues_other_owners_and_removes_credential_after_failure(self):
        program = ast.parse(runner.REMOTE_PROGRAM)
        cleanup_node = next(node for node in program.body if isinstance(node, ast.FunctionDef) and node.name == 'cleanup')
        code = compile(ast.Module(body=[cleanup_node], type_ignores=[]), '<offline-cleanup>', 'exec')
        credential = self.root / 'observer.json'
        credential.write_text('ephemeral-fixture')
        calls = []
        def stop(owner):
            calls.append(owner['pid'])
            if owner['pid'] == 3:
                raise OSError('fixture read failure')
            return {'pid': owner['pid'], 'stopped': True}
        namespace = {'root': self.root, 'stop_owned': stop}
        exec(code, namespace)
        result = namespace['cleanup']({'sampler': {'pid': 3},
                                      'publishers': [{'pid': 1}, {'pid': 2}]})
        self.assertEqual(calls, [3, 2, 1])
        self.assertFalse(result['all_stopped'])
        self.assertTrue(result['credential_removed'])
        self.assertFalse(credential.exists())

    def test_reused_pid_is_not_signaled_and_owned_exit_is_verified(self):
        owner = {'pid': 1001, 'start_ticks': '55', 'argv': ['lk', 'load-test']}
        with patch.object(runner, 'process_identity', return_value={**owner, 'start_ticks': '56'}), \
                patch.object(runner.os, 'kill') as kill:
            result = runner.stop_owned(owner, kill=kill)
        kill.assert_not_called()
        self.assertTrue(result['stopped'])
        with patch.object(runner, 'process_identity', side_effect=[owner, None]), \
                patch.object(runner.os, 'kill') as kill:
            result = runner.stop_owned(owner, kill=kill, monotonic=lambda: 1, sleep=lambda _: None)
        kill.assert_called_once_with(1001, signal.SIGINT)
        self.assertTrue(result['stopped'])


if __name__ == '__main__':
    unittest.main()
