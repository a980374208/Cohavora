"""Offline namespace admission, private RTP and exact owner checks."""
from __future__ import annotations

import ast
import copy
import ipaddress
import json
from pathlib import Path
import re
import signal
import socket
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
import urllib.error
from unittest.mock import Mock, patch

TOOLS = Path(__file__).resolve().parents[1] / 'tools' / 'meeting'
sys.path.insert(0, str(TOOLS))
try:
    import b11_100_remote_run as runner
    import b11_publisher_network as network
    import b11_namespace_publisher_worker as worker
    import b11_100_resource_sampler as sampler
finally:
    sys.path.remove(str(TOOLS))


def facts():
    return {'observation_seconds': 15.1, 'publisher_count': 1, 'subscriber_count': 1,
        'codec': 'vp8', 'simulcast': True, 'source_fps': 30, 'advertised_layer_count': 3,
        'advertised_high_width': 1280, 'advertised_high_height': 720,
        'namespace_udp_dnat_packets': 3, 'private_veth_rx_bytes': 2_000_000,
        'private_veth_rx_packets': 3000, 'public_tx_bytes': 1000, 'media_route': 'DNAT_TO_HOST_LOCAL',
        'sfu_running': True, 'namespace_default_route_absent': True, 'namespace_peer_route_only': True,
        'sfu_rtc_target_local_and_listening': True,
        'sfu_peer_route_interface_matches': True, 'private_ingress_rtp_packets': 2500,
        'private_ingress_rtp_bytes': 2_000_000, 'private_egress_rtp_packets': 2000,
        'private_egress_rtp_bytes': 1_900_000}


class AdmissionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='b11-100-network-selftest-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / 'new-evidence'
        self.source = {'cli_path': '/home/ubuntu/.local/bin/lk', 'cli_sha256': 'a' * 64,
            'remote_prerequisite': {'config_sha256': 'b' * 64, 'sfu_image': 'sha256:' + 'c' * 64}}
        self.target = {'instance': 'ins-selftest', 'service_url': 'ws://81.71.85.246:17880',
            'local_service_url': 'http://127.0.0.1:17880', 'config_path': '/home/ubuntu/livekit.yml', 'sfu_container': 'livekit'}
        self.identity = {'fixture': True}
        self.publisher = {**runner.freeze.PUBLISHER, 'identity_prefix': 'b11grid100_fixture'}
        self.operations = []
        self.enterContext(patch.object(runner.shared, '_pinned_config', return_value={'fixture': True}))
        self.power = self.enterContext(patch.object(runner.shared, '_set_execution_state', return_value=True))
        self.execute = self.enterContext(patch.object(runner.remote, 'execute', side_effect=self.execute_remote))
        self.upload = self.enterContext(patch.object(runner.remote, 'upload'))
        self.enterContext(patch.object(runner.remote, 'download', side_effect=lambda config, path, local: local.write_text('fixture')))
        self.gate_status = 'PASS'
        self.observer = Mock(return_value={'status': 'PASS', 'reason': 'all_grid_routes_progressed'})

    def execute_remote(self, config, command):
        text = command.decode()
        if 'request = {' in text:
            self.request = ast.literal_eval(next(line.removeprefix('request = ') for line in text.splitlines() if line.startswith('request = ')))
            return {'stdout': '{"ok":true}'}
        if 'B11_READ_PY' in text:
            return {'stdout': '{"value":{"fixture":true}}'}
        match = re.search(r"'--operation', '([^']+)'", text)
        if match is None:
            return {'stdout': '{"ok":true}'}
        operation = match.group(1)
        self.operations.append(operation)
        response = {'ok': True}
        if operation == 'gate':
            response['gate'] = {'schema': 1, 'scope': runner.freeze.PATH_GATE_SCOPE,
                'status': self.gate_status, 'formal_b11_status': 'NOT_RUN',
                'target': {key: self.target[key] for key in ('instance', 'service_url', 'config_path', 'sfu_container')},
                'cli_sha256': self.source['cli_sha256'], 'config_sha256': self.source['remote_prerequisite']['config_sha256'],
                'sfu_image': self.source['remote_prerequisite']['sfu_image'],
                'task_owner': {'room': self.request['room'], 'remote_directory': self.request['directory'],
                    'owner_id': self.request['owner_id'], 'namespace': 'b11ns-fixture', 'namespace_inode': 42,
                    'veth_host': 'b11hfixture', 'veth_peer': 'b11pfixture', 'host_ifindex': 3, 'peer_ifindex': 4,
                    'publisher_pid': 1001, 'publisher_start_ticks': '55', 'subnet': '198.18.1.0/30',
                    'host_ip': '198.18.1.1', 'peer_ip': '198.18.1.2', 'rtc_target_ip': '172.16.0.15'}, 'facts': facts(),
                'cleanup': {'publisher_stopped': True, 'subscriber_stopped': True,
                            'network_removed': False, 'credentials_removed': True}}
        if operation == 'stop':
            response['all_stopped'] = True
        if operation == 'delete':
            response['network_removed'] = True
        if operation == 'remove_credentials':
            response['credential_removed'] = True
        if operation == 'snapshot':
            response['all_sources_advertise_hd'] = True
        return {'stdout': json.dumps(response)}

    def invoke(self, observer=None):
        hashes = {name: runner.soak.sha256(TOOLS / name) for name in runner.UPLOAD_NAMES}
        return runner.execute(self.output, target=self.target, source=self.source, identity=self.identity,
                              tool_hashes=hashes, publisher=self.publisher, observer=observer)

    def test_freeze_rejection_has_no_network_or_desktop_side_effects(self):
        with patch.object(runner, 'verify_path_inputs', side_effect=ValueError('frozen_inputs_changed')):
            with self.assertRaisesRegex(ValueError, 'frozen_inputs_changed'):
                runner.path_preflight(self.output, self.root / 'manifest.json')
        self.execute.assert_not_called()
        self.upload.assert_not_called()
        self.power.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_plan_has_no_network_after_verifying_path_freeze(self):
        with patch.object(runner, 'verify_path_inputs', return_value={}) as verify:
            result = runner.path_preflight(self.output, self.root / 'manifest.json', plan=True)
        verify.assert_called_once()
        self.assertEqual(result['publisher_count'], 1)
        self.assertEqual(result['subscriber_count'], 1)
        self.execute.assert_not_called()
        self.power.assert_not_called()

    def test_failed_single_source_gate_never_launches100_or_observer(self):
        self.gate_status = 'FAIL'
        result = self.invoke(observer=self.observer)
        self.assertEqual(result['status'], 'FAIL')
        self.assertNotIn('start', self.operations)
        self.assertNotIn('sampler', self.operations)
        self.observer.assert_not_called()
        self.assertEqual(self.operations[-3:], ['stop', 'remove_credentials', 'delete'])

    def test_single_source_gate_requires_all_cleanup_before_final_pass(self):
        result = self.invoke()
        self.assertEqual(result['status'], 'PASS')
        gate = json.loads((self.output / 'path-gate.json').read_text())
        runner.freeze.validate_path_gate(gate, {'target': self.target}, self.source)
        self.assertTrue(gate['cleanup']['network_removed'])
        self.assertNotIn('start', self.operations)
        self.assertEqual([call.args[0] for call in self.power.call_args_list], [0x80000003, 0x80000000])

    def test_forged_pass_with_insufficient_rtp_never_starts100(self):
        original = self.execute_remote
        def insufficient(config, command):
            result = original(config, command)
            value = json.loads(result['stdout'])
            if 'gate' in value:
                value['gate']['facts']['private_ingress_rtp_packets'] = 0
            return {'stdout': json.dumps(value)}
        self.execute.side_effect = insufficient
        result = self.invoke(observer=self.observer)
        self.assertEqual(result['status'], 'INCONCLUSIVE')
        self.assertNotIn('start', self.operations)
        self.observer.assert_not_called()


class NetworkEvidenceTests(unittest.TestCase):
    @staticmethod
    def rtc_probe_values(address='172.16.0.15', udp_pid=1816, tcp_address='0.0.0.0', local_address='172.16.0.15',
                         tcp_v6only=1, tcp6_pid=1816):
        return [json.dumps([{'dev': 'eth0', 'prefsrc': address}]),
            json.dumps([{'ifname': 'eth0', 'addr_info': [{'family': 'inet', 'local': local_address}]}]),
            f'UNCONN 0 0 {address}:17882 0.0.0.0:* users:(("livekit-server",pid={udp_pid},fd=4))\n',
            f'LISTEN 0 4096 {tcp_address}:17881 0.0.0.0:* users:(("livekit-server",pid=1816,fd=5))\n',
            f'LISTEN 0 4096 *:17881 *:* users:(("livekit-server",pid={tcp6_pid},fd=9)) v6only:{tcp_v6only}\n']

    def test_rtc_target_is_real_local_private_address_owned_by_sfu_listener(self):
        with patch.object(network, 'command', side_effect=self.rtc_probe_values()):
            self.assertEqual(network.rtc_target_ip(1816), '172.16.0.15')

    def test_rtc_target_rejects_public_or_nonlocal_primary_address(self):
        for address, local in [('81.71.85.246', '81.71.85.246'), ('172.16.0.15', '172.16.0.16')]:
            with self.subTest(address=address, local=local), patch.object(network, 'command',
                    side_effect=self.rtc_probe_values(address=address, local_address=local)):
                with self.assertRaisesRegex(RuntimeError, 'sfu_rtc_target_not_local_private'):
                    network.rtc_target_ip(1816)

    def test_rtc_target_requires_both_udp_and_tcp_listeners_owned_by_current_sfu_pid(self):
        for options in ({'udp_pid': 18160}, {'tcp_address': '172.16.0.16'}):
            with self.subTest(options=options), patch.object(network, 'command', side_effect=self.rtc_probe_values(**options)):
                with self.assertRaisesRegex(RuntimeError, 'sfu_rtc_target_not_listening'):
                    network.rtc_target_ip(1816)

    def test_rtc_target_accepts_only_verified_dualstack_tcp_wildcard(self):
        for v6only, pid, accepted in ((0, 1816, True), (1, 1816, False), (0, 18160, False)):
            with self.subTest(v6only=v6only, pid=pid), patch.object(network, 'command',
                    side_effect=self.rtc_probe_values(tcp_address='172.16.0.16', tcp_v6only=v6only, tcp6_pid=pid)):
                if accepted:
                    self.assertEqual(network.rtc_target_ip(1816), '172.16.0.15')
                else:
                    with self.assertRaisesRegex(RuntimeError, 'sfu_rtc_target_not_listening'):
                        network.rtc_target_ip(1816)

    def test_network_admission_rejects_missing_private_return_route_or_stale_dnat(self):
        owner = {'namespace': 'b11ns-fixture', 'namespace_inode': 42, 'subnet': '198.18.1.0/30',
            'host_ip': '198.18.1.1', 'peer_ip': '198.18.1.2', 'rtc_target_ip': '172.16.0.15',
            'veth_host': 'b11hfixture', 'veth_peer': 'b11pfixture', 'host_ifindex': 3, 'peer_ifindex': 4,
            'host_alias': 'task-host', 'peer_alias': 'task-peer'}
        routes = [{'dst': owner['subnet'], 'dev': owner['veth_peer']}] + [
            {'dst': target, 'dev': owner['veth_peer'], 'gateway': owner['host_ip']}
            for target in (network.NODE_IP, owner['rtc_target_ip'])]
        rules = '\n'.join('-A OUTPUT -d 81.71.85.246/32 -p ' + protocol + ' -m ' + protocol +
            ' --dport ' + str(port) + ' -j DNAT --to-destination 172.16.0.15:' + str(port)
            for protocol, port in (('udp', 17882), ('tcp', 17881)))
        scenarios = [(routes, rules, None), (routes[:-1], rules, 'task_namespace_route_changed'),
            (routes + [{'dst': 'default', 'gateway': owner['host_ip']}], rules, 'task_namespace_route_changed'),
            (routes, rules.replace('172.16.0.15:', '198.18.1.1:'), 'task_namespace_firewall_changed')]
        for route_snapshot, rule_snapshot, rejected in scenarios:
            with self.subTest(rejected=rejected), patch.object(network, 'load_owner', return_value=owner), \
                    patch.object(network, 'rtc_target_ip', return_value=owner['rtc_target_ip']), \
                    patch.object(network.Path, 'exists', return_value=True), \
                    patch.object(network.Path, 'stat', return_value=SimpleNamespace(st_ino=42)), \
                    patch.object(network, 'link', side_effect=lambda name, namespace=None: {
                        'ifindex': owner['host_ifindex'] if namespace is None else owner['peer_ifindex'],
                        'ifalias': owner['host_alias'] if namespace is None else owner['peer_alias']}), \
                    patch.object(network, 'command', side_effect=[json.dumps(route_snapshot),
                        json.dumps([{'dev': owner['veth_host']}]), rule_snapshot]):
                if rejected:
                    with self.assertRaisesRegex(RuntimeError, rejected):
                        network.verify_network({}, sfu_pid=1816)
                else:
                    self.assertEqual(network.verify_network({}, sfu_pid=1816), owner)

    def test_first_room404_retries_until_three_layers_ready(self):
        layers = {'advertised_layer_count': 3, 'advertised_high_width': 1280, 'advertised_high_height': 720}
        not_found = urllib.error.HTTPError('http://127.0.0.1/room', 404, 'sensitive-error-canary', None, None)
        with patch.object(network, 'advertised_layers', side_effect=[not_found, layers]) as query, \
                patch.object(network.time, 'monotonic', side_effect=[0, 1, 2]), \
                patch.object(network.time, 'sleep') as sleep:
            result = network.wait_advertised_layers({}, b'config')
        self.assertEqual(result, layers)
        self.assertEqual(query.call_count, 2)
        sleep.assert_called_once_with(.5)

    def test_non404_http_error_is_not_retried_and_reason_contains_only_stage_code(self):
        forbidden = urllib.error.HTTPError('http://127.0.0.1/room', 403, 'sensitive-error-canary', None, None)
        with patch.object(network, 'advertised_layers', side_effect=forbidden) as query, \
                patch.object(network.time, 'monotonic', side_effect=[0, 1]), \
                patch.object(network.time, 'sleep') as sleep:
            with self.assertRaises(urllib.error.HTTPError):
                network.wait_advertised_layers({}, b'config')
        query.assert_called_once()
        sleep.assert_not_called()
        reason = network.safe_measurement_reason('room_readiness', forbidden)
        self.assertEqual(reason, 'room_readiness_http_403')
        self.assertNotIn('sensitive-error-canary', reason)

    def test_only_declared_not_ready_runtime_errors_are_retried(self):
        with patch.object(network, 'advertised_layers', side_effect=RuntimeError('other_failure_with_sensitive_detail')), \
                patch.object(network.time, 'monotonic', side_effect=[0, 1]), \
                patch.object(network.time, 'sleep') as sleep:
            with self.assertRaises(RuntimeError):
                network.wait_advertised_layers({}, b'config')
        sleep.assert_not_called()

    def test_readiness_retains_only_safe_last_api_counts_when_tracks_not_ready(self):
        participants = [{'identity': 'sensitive-publisher-id', 'tracks': [
            {'sid': 'sensitive-track', 'type': 'VIDEO', 'layers': [{'width': 320, 'height': 180}]}]}]
        diagnostics = {}
        with patch.object(network, 'room_participants', return_value=participants):
            with self.assertRaisesRegex(RuntimeError, 'path_gate_room_tracks_not_ready'):
                network.advertised_layers({}, b'config', diagnostics=diagnostics)
        self.assertEqual(diagnostics['participant_count'], 1)
        self.assertEqual(diagnostics['video_track_count'], 1)
        self.assertEqual(diagnostics['layer_counts'], [1])
        self.assertEqual(diagnostics['last_api_status'], 200)
        self.assertNotIn('sensitive-', json.dumps(diagnostics))

    def test_not_ready_timeout_preserves_last_api_counts_and_controlled_reason(self):
        diagnostics = {}
        def not_ready(*args, **kwargs):
            kwargs['diagnostics'].update(last_api_status=200, participant_count=2,
                video_track_count=0, layer_counts=[])
            raise RuntimeError('path_gate_room_tracks_not_ready')
        with patch.object(network, 'advertised_layers', side_effect=not_ready), \
                patch.object(network.time, 'monotonic', side_effect=[0, 1, 21]), \
                patch.object(network.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'path_gate_room_not_ready_timeout'):
                network.wait_advertised_layers({}, b'config', diagnostics=diagnostics)
        self.assertEqual(diagnostics['participant_count'], 2)
        self.assertEqual(diagnostics['video_track_count'], 0)
        self.assertEqual(diagnostics['last_failure_code'], 'path_gate_room_tracks_not_ready')

    def test_subnet_selection_rejects_existing_routes_and_addresses(self):
        numbers = iter((1, 2, 3))
        subnet = network.choose_subnet([{'dst': 'default'}, {'dst': '198.18.0.4/30'}],
            [{'addr_info': [{'family': 'inet', 'local': '198.18.0.9', 'prefixlen': 30}]}], chooser=lambda _: next(numbers))
        self.assertEqual(str(subnet), '198.18.0.12/30')

    def test_occupied_benchmark_range_refuses_allocation(self):
        with self.assertRaisesRegex(RuntimeError, 'task_private_subnet_unavailable'):
            network.choose_subnet([{'dst': '198.18.0.0/15'}], [], chooser=lambda _: 0)

    @staticmethod
    def packet(source, destination, source_port, destination_port, payload=b'\x80\x60' + b'\0' * 10):
        ethernet = b'\0' * 12 + b'\x08\x00'
        ipv4 = bytearray(20)
        ipv4[0], ipv4[9] = 0x45, 17
        ipv4[12:16], ipv4[16:20] = socket.inet_aton(source), socket.inet_aton(destination)
        udp = struct.pack('!HHHH', source_port, destination_port, 8 + len(payload), 0)
        return ethernet + ipv4 + udp + payload

    def test_private_capture_counts_rtp_both_directions_and_excludes_stun_or_public(self):
        capture = network.PrivateRtpCapture({'peer_ip': '198.18.1.2', 'host_ip': '198.18.1.1', 'rtc_target_ip': '172.16.0.15'})
        packet = self.packet('198.18.1.2', '172.16.0.15', 50000, 17882)
        capture.packet(packet)
        capture.packet(self.packet('172.16.0.15', '198.18.1.2', 17882, 50000))
        capture.packet(self.packet('198.18.1.2', '172.16.0.15', 50000, 17882, b'\0' * 20))
        capture.packet(self.packet('198.18.1.2', '81.71.85.246', 50000, 17882))
        capture.packet(self.packet('198.18.1.1', '198.18.1.2', 17882, 50000))
        self.assertEqual(capture.values['private_ingress_rtp_packets'], 1)
        self.assertEqual(capture.values['private_egress_rtp_packets'], 1)
        self.assertEqual(capture.values['private_ingress_rtp_bytes'], len(packet))

    def test_capture_diagnoses_unknown_sfu_source_without_claiming_private_rtp(self):
        capture = network.PrivateRtpCapture({'peer_ip': '198.18.1.2', 'host_ip': '198.18.1.1', 'rtc_target_ip': '172.16.0.15'})
        capture.packet(self.packet('172.16.0.16', '198.18.1.2', 17882, 50000, b'\0' * 20))
        capture.packet(self.packet('198.18.1.2', '198.18.1.1', 50000, 17882, b'\0' * 20))
        diagnostic = capture.udp_diagnostics()
        self.assertEqual(diagnostic['udp_address_pairs'][0], {
            'source_address': '172.16.0.16', 'destination_address': '198.18.1.2',
            'source_port': 17882, 'destination_port': 50000, 'packets': 1,
            'source_role': 'outside_host_peer', 'destination_role': 'namespace_peer'})
        self.assertEqual(capture.values['private_egress_rtp_packets'], 0)
        self.assertEqual(capture.values['private_ingress_rtp_packets'], 0)
        self.assertNotIn('payload', str(diagnostic))

    def test_capture_limits_address_pairs_but_continues_existing_pair_counts(self):
        capture = network.PrivateRtpCapture({'peer_ip': '198.18.1.2', 'host_ip': '198.18.1.1', 'rtc_target_ip': '172.16.0.15'})
        for port in range(50000, 50033):
            capture.packet(self.packet('172.16.0.15', '198.18.1.2', 17882, port, b'\0' * 20))
        capture.packet(self.packet('172.16.0.15', '198.18.1.2', 17882, 50000, b'\0' * 20))
        diagnostic = capture.udp_diagnostics()
        self.assertEqual(len(diagnostic['udp_address_pairs']), 32)
        self.assertEqual(diagnostic['udp_address_pairs'][0]['packets'], 2)
        self.assertEqual(diagnostic['udp_unrecorded_pair_packets'], 1)

    def test_counter_only_or_public_hairpin_cannot_pass_path(self):
        self.assertTrue(network.path_passes(facts()))
        for name, value in (('private_ingress_rtp_packets', 0), ('public_tx_bytes', 262145),
                            ('advertised_layer_count', 1), ('namespace_default_route_absent', False),
                            ('sfu_rtc_target_local_and_listening', False)):
            row = facts()
            row[name] = value
            self.assertFalse(network.path_passes(row))

    def test_pid_reuse_or_uid_namespace_argv_mismatch_never_receives_signal(self):
        owner = {'pid': 123, 'start_ticks': '55', 'argv': ['lk', 'load-test'], 'uid': 1000, 'namespace_inode': 42}
        for field, changed in (('start_ticks', '56'), ('argv', ['lk', 'other']), ('uid', 0), ('namespace_inode', 43)):
            with self.subTest(field=field), patch.object(network, 'process_identity', return_value={**owner, field: changed}), \
                    patch.object(network.os, 'kill') as kill:
                result = network.stop_owned(owner)
            kill.assert_not_called()
            self.assertEqual(result['stopped'], field == 'start_ticks')

    def test_rollback_owns_unreaped_children_even_if_metadata_write_failed(self):
        child = Mock(pid=123)
        child.poll.return_value = None
        self.assertTrue(network.rollback_children([child]))
        child.send_signal.assert_called_once_with(signal.SIGINT)
        child.wait.assert_called_once_with(timeout=3)

    def test_pending_host_sampler_can_never_report_complete_cleanup(self):
        with tempfile.TemporaryDirectory(prefix='b11-pending-state-') as temporary:
            directory = Path(temporary)
            (directory / 'sampler-state.json').write_text('{}')
            request = {'directory': str(directory), 'owner_id': 'a' * 32}
            with patch.object(network, 'load_owner', return_value={}), \
                    patch.object(network, 'root_json', return_value={'schema': 1, 'owner_id': 'a' * 32,
                        'launch_pending': True, 'publisher': None, 'launcher': None}):
                result = network.stop_publishers(request)
            self.assertFalse(result['all_stopped'])

    def test_untrusted_state_file_never_signals_claimed_root_process(self):
        with tempfile.TemporaryDirectory(prefix='b11-untrusted-state-') as temporary:
            directory = Path(temporary)
            (directory / 'sampler-state.json').write_text('{}')
            with patch.object(network, 'load_owner', return_value={}), \
                    patch.object(network, 'root_json', side_effect=RuntimeError('network_marker_ownership_unknown')), \
                    patch.object(network, 'stop_owned') as stop:
                result = network.stop_publishers({'directory': str(directory), 'owner_id': 'a' * 32})
            stop.assert_not_called()
            self.assertFalse(result['all_stopped'])

    def test_task_sampler_argv_cannot_reference_foreign_tool(self):
        owner = {'uid': 0, 'namespace_inode': 43, 'argv': ['python3', '/other/foreign.py', '--duration', '510']}
        self.assertFalse(network.task_process_matches({'directory': '/tmp/b11-100-fixture'},
            {'host_namespace_inode': 43}, 'sampler', 'publisher', owner))

    def test_namespace_sampler_rejects_reused_publisher_and_records_private_counters(self):
        with tempfile.TemporaryDirectory(prefix='b11-resource-fixture-') as temporary:
            directory = Path(temporary)
            proc, interfaces = directory / 'proc', directory / 'interfaces'
            def write_process(pid, ticks):
                path = proc / str(pid)
                path.mkdir(parents=True, exist_ok=True)
                fields = ['0'] * 22
                fields[0], fields[11], fields[12], fields[19], fields[21] = 'R', '11', '7', ticks, '123'
                (path / 'stat').write_text(str(pid) + ' (name with spaces) ' + ' '.join(fields))
            write_process(123, '55')
            write_process(124, '66')
            interface = interfaces / 'b11hfixture'
            (interface / 'statistics').mkdir(parents=True)
            (interface / 'ifindex').write_text('3')
            (interface / 'ifalias').write_text('b11-owner:' + 'a' * 32 + ':host')
            for field, value in (('rx_bytes', 1000000), ('tx_bytes', 500000), ('rx_packets', 1000), ('tx_packets', 500)):
                (interface / 'statistics' / field).write_text(str(value))
            args = SimpleNamespace(publisher_pid=123, publisher_start_ticks='55', sfu_pid=124,
                sfu_start_ticks='66', private_interface='b11hfixture', private_ifindex=3, owner_id='a' * 32)
            row = sampler.sample(args, 0, proc_root=proc, sys_root=interfaces)
            self.assertEqual(row['publisher_cpu_ticks'], 18)
            self.assertEqual(row['private_rx_bytes'], 1000000)
            write_process(123, '56')
            with self.assertRaisesRegex(RuntimeError, 'namespace_resource_identity_changed'):
                sampler.sample(args, 0, proc_root=proc, sys_root=interfaces)

    def test_worker_pins_720p_simulcast_group_and_private_signal_origin(self):
        request = {'schema': 1, 'scope': 'B11_TASK_NAMESPACE_PUBLISHER', 'owner_id': 'a' * 32,
            'room': 'b11-100-fixture', 'cli_path': '/home/ubuntu/.local/bin/lk', 'cli_sha256': 'b' * 64,
            'config_path': '/home/ubuntu/livekit.yml', 'service_url': 'http://198.18.1.1:17880',
            'publisher': {**runner.freeze.PUBLISHER, 'identity_prefix': 'b11grid100_fixture'},
            'owner_path': '/tmp/b11-100-fixture/publisher-owner.json', 'expected_namespace_inode': 42}
        worker.validate_request(request)
        argv = worker.publisher_argv(request)
        self.assertEqual(argv[argv.index('--video-publishers') + 1], '100')
        self.assertEqual(argv[argv.index('--video-resolution') + 1], 'high')
        self.assertNotIn('--no-simulcast', argv)
        request['service_url'] = 'http://81.71.85.246:17880'
        with self.assertRaisesRegex(ValueError, 'invalid_worker_service_url'):
            worker.validate_request(request)

    def test_raw_identity_is_not_returned_by_publication_snapshot(self):
        participants = [{'identity': f'b11grid100_fixture_pub_{i}',
            'tracks': [{'sid': 'sensitive-track-' + str(i), 'type': 'VIDEO',
                'layers': [{'width': 320, 'height': 180}, {'width': 640, 'height': 360}, {'width': 1280, 'height': 720}]}]}
            for i in range(100)]
        with patch.object(network, 'prerequisite', return_value=(b'config', {})), \
                patch.object(network, 'room_participants', return_value=participants):
            result = network.publication_snapshot({'publisher': {'identity_prefix': 'b11grid100_fixture'}})
        self.assertTrue(result['all_sources_advertise_hd'])
        self.assertEqual(result['video_tracks'], 100)
        self.assertNotIn('sensitive-track-', json.dumps(result))


if __name__ == '__main__':
    unittest.main()
