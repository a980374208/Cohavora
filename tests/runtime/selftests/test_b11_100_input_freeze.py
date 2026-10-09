"""Offline 100-source freeze and fail-closed private RTP admission contracts."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / "tools/meeting"
sys.path.insert(0, str(TOOLS))
import b11_100_input_freeze as freeze
from test_b11_hd_input_freeze import source_identity


def gate(profile, source):
    return {"schema": 1, "scope": freeze.PATH_GATE_SCOPE, "status": "PASS", "formal_b11_status": "NOT_RUN",
            "target": {key: profile["target"][key] for key in ("instance", "service_url", "config_path", "sfu_container")},
            "cli_sha256": source["cli_sha256"],
            "config_sha256": source["remote_prerequisite"]["config_sha256"],
            "sfu_image": source["remote_prerequisite"]["sfu_image"],
            "task_owner": {"room": "b11-grid100-path-selftest", "remote_directory": "/tmp/b11-grid100-path-selftest",
                           "namespace": "b11n-offline", "veth_host": "b11h-offline", "veth_peer": "b11p-offline",
                           "namespace_inode": 1234, "publisher_pid": 1001, "publisher_start_ticks": "20000",
                           "owner_id": "offlineowner", "host_ifindex": 4, "peer_ifindex": 5,
                           "subnet": "198.18.0.0/30", "host_ip": "198.18.0.1", "peer_ip": "198.18.0.2",
                           "rtc_target_ip": "172.16.0.15"},
            "facts": {"observation_seconds": 15, "publisher_count": 1, "subscriber_count": 1,
                      "codec": "vp8", "simulcast": True, "namespace_udp_dnat_packets": 1,
                      "private_veth_rx_bytes": 1048576, "private_veth_rx_packets": 100,
                      "private_ingress_rtp_packets": 100, "private_ingress_rtp_bytes": 1048576,
                      "private_egress_rtp_packets": 100, "private_egress_rtp_bytes": 1048576,
                      "public_tx_bytes": 262144, "media_route": "DNAT_TO_HOST_LOCAL", "sfu_running": True,
                      "advertised_layer_count": 3, "advertised_high_width": 1280, "advertised_high_height": 720,
                      "source_fps": 30, "namespace_default_route_absent": True, "namespace_peer_route_only": True,
                      "sfu_peer_route_interface_matches": True, "sfu_rtc_target_local_and_listening": True},
            "cleanup": {"publisher_stopped": True, "subscriber_stopped": True,
                        "network_removed": True, "credentials_removed": True}}


class Grid100FreezeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.profile = json.loads((TOOLS / "b11_100_grid_profile.json").read_text(encoding="utf-8"))
        self.profile_path = self.root / "profile.json"
        self.source_path = self.root / "out/b11-grid100/source-identity.json"
        self.gate_path = self.root / "out/b11-grid100/private-media-path-gate.json"
        self.checkpoint = self.write("checkpoint.json", "offline prior prerequisite")
        self.source = source_identity(self.checkpoint)
        self.gate = gate(self.profile, self.source)
        self.write_json(self.source_path, self.source)
        self.write_json(self.gate_path, self.gate)
        self.write_json(self.profile_path, self.profile)
        self.exe = self.write("build/RelWithDebInfo/" + freeze.EXECUTABLE_NAME, "offline binary")
        self.exe.with_suffix(".pdb").write_bytes(b"offline pdb")
        self.shared_source = self.write("src/renderer.cpp", "offline source")
        for name in freeze.TOOL_INPUTS:
            self.write(name, "offline runtime tool")
        self.worker = self.write("tests/runtime/tools/meeting/b11_100_private_network.py", "offline network worker")
        self.manifest = self.root / "freeze.json"
        self.addCleanup(patch.stopall)
        patch.object(freeze.base, "_git_head", return_value="a" * 40).start()
        patch.object(freeze.base, "_source_paths", return_value=[self.shared_source]).start()
        patch.object(freeze.base, "_binary_identity", side_effect=lambda path: {
            "configuration": "RelWithDebInfo", "binary_sha256": freeze.base.sha256(path)}).start()
        patch.object(freeze.base, "_build_metadata", return_value={"configuration": "RelWithDebInfo"}).start()
        self.remote = {"transport": "ssh", "host": "192.0.2.10", "key_public_fingerprint": "SHA256:offline"}
        patch.object(freeze.base, "_remote_transport_identity", side_effect=lambda *_: dict(self.remote)).start()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def write_json(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2), encoding="utf-8")

    def freeze(self):
        return freeze.freeze_inputs(self.manifest, self.exe, self.profile_path, root=self.root)

    def verify(self, **kwargs):
        return freeze.verify_inputs(self.manifest, self.exe, self.profile_path, root=self.root, **kwargs)

    def test_scope_is_independent_and_one_source_gate_never_promotes_runtime(self):
        result = self.freeze()
        self.assertEqual(result, self.verify())
        self.assertEqual(result["scope"], "B11_100_SOURCE_GRID16_DIAGNOSTIC")
        self.assertEqual(result["runtime_status"], "NOT_RUN")
        self.assertEqual(result["formal_b11_status"], "NOT_RUN")
        self.assertEqual(result["binary_source_equivalence"], "UNKNOWN")
        self.assertEqual(result["private_media_path_status"], "PREVIOUS_ONE_SOURCE_GATE_PASSED_REVERIFY_REQUIRED")
        for validator in (freeze.base.validate_profile, freeze.hd.validate_profile):
            with self.assertRaises(ValueError):
                validator(self.profile)

    def test_one_hundred_publication_identities_and_quality_are_frozen(self):
        identities = freeze.publisher_identities(self.profile)
        self.assertEqual(len(set(identities)), 100)
        self.assertEqual(identities[0], "b11grid100_RUN01_pub_0")
        self.assertEqual(identities[-1], "b11grid100_RUN01_pub_99")
        for key, replacement in (("count", 99), ("resolution", "low"), ("simulcast", False),
                                 ("codec", "h264"), ("num_per_second", 10), ("count", True)):
            value = deepcopy(self.profile)
            value["publisher"][key] = replacement
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "invalid_100_publisher"):
                freeze.validate_profile(value)

    def test_dpi_duration_auto_geometry_and_provenance_cannot_relax(self):
        for section, key, value in (("probe", "receiver_arguments", []), ("probe", "observe_seconds", 60),
                                    ("quality_contract", "minimum_frame_rate_ratio", 0.8),
                                    ("quality_contract", "source_assignment_by_sequence", "KNOWN"),
                                    ("network_contract", "path_gate_required", False)):
            changed = deepcopy(self.profile)
            changed[section][key] = value
            with self.subTest(section=section, key=key), self.assertRaises(ValueError):
                freeze.validate_profile(changed)

    def test_missing_or_failed_path_gate_blocks_freeze(self):
        for status in ("NOT_RUN", "FAIL", "UNKNOWN", "INCONCLUSIVE"):
            value = deepcopy(self.gate)
            value["status"] = status
            with self.subTest(status=status), self.assertRaisesRegex(ValueError, "private_media_path_gate_not_passed"):
                freeze.validate_path_gate(value, self.profile, self.source)
        self.gate_path.unlink()
        with self.assertRaises(ValueError):
            self.freeze()
        self.assertFalse(self.manifest.exists())

    def test_signaling_or_veth_bytes_alone_do_not_prove_private_media(self):
        for key in ("private_ingress_rtp_packets", "private_ingress_rtp_bytes", "private_egress_rtp_packets",
                    "private_egress_rtp_bytes", "private_veth_rx_packets", "private_veth_rx_bytes"):
            value = deepcopy(self.gate)
            value["facts"][key] = 0
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "private_media_gate_measurement_insufficient"):
                freeze.validate_path_gate(value, self.profile, self.source)

    def test_conntrack_first_packet_hit_is_valid_but_zero_is_not(self):
        self.assertEqual(self.gate, freeze.validate_path_gate(self.gate, self.profile, self.source))
        self.gate["facts"]["namespace_udp_dnat_packets"] = 0
        with self.assertRaisesRegex(ValueError, "private_media_gate_measurement_insufficient"):
            freeze.validate_path_gate(self.gate, self.profile, self.source)

    def test_public_tx_and_topology_must_match_private_path(self):
        for key, replacement in (("public_tx_bytes", 262145), ("observation_seconds", 14.9),
                                 ("namespace_default_route_absent", False), ("namespace_peer_route_only", False),
                                 ("sfu_rtc_target_local_and_listening", False),
                                 ("sfu_peer_route_interface_matches", False), ("advertised_layer_count", 1),
                                 ("subscriber_count", 0), ("source_fps", 15), ("sfu_running", False)):
            value = deepcopy(self.gate)
            value["facts"][key] = replacement
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "private_media_gate_measurement_insufficient"):
                freeze.validate_path_gate(value, self.profile, self.source)

    def test_gate_target_cli_config_and_image_are_bound(self):
        for key in ("cli_sha256", "config_sha256", "sfu_image"):
            value = deepcopy(self.gate)
            value[key] = "wrong-identity"
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "private_media_gate_target_mismatch"):
                freeze.validate_path_gate(value, self.profile, self.source)
        self.gate["target"]["instance"] = "ins-other"
        with self.assertRaisesRegex(ValueError, "private_media_gate_target_mismatch"):
            freeze.validate_path_gate(self.gate, self.profile, self.source)

    def test_owner_and_private_subnet_identity_are_not_optional(self):
        for key, replacement in (("publisher_pid", 0), ("namespace_inode", True), ("publisher_start_ticks", ""),
                                 ("host_ip", "192.0.2.1"), ("peer_ip", "198.18.0.1"),
                                 ("subnet", "192.0.2.0/30"), ("owner_id", ""),
                                 ("rtc_target_ip", "81.71.85.246"), ("rtc_target_ip", "127.0.0.1"),
                                 ("rtc_target_ip", "198.18.0.1"), ("rtc_target_ip", "::1")):
            value = deepcopy(self.gate)
            value["task_owner"][key] = replacement
            with self.subTest(key=key), self.assertRaises(ValueError):
                freeze.validate_path_gate(value, self.profile, self.source)

    def test_one_source_workers_and_network_must_be_cleaned_before_freeze(self):
        for key in self.gate["cleanup"]:
            value = deepcopy(self.gate)
            value["cleanup"][key] = False
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "private_media_gate_cleanup_incomplete"):
                freeze.validate_path_gate(value, self.profile, self.source)

    def test_prestart_gate_keeps_only_its_actual_owned_network_alive(self):
        value = deepcopy(self.gate)
        value["cleanup"]["network_removed"] = False
        self.assertEqual(value, freeze.validate_path_gate(value, self.profile, self.source, require_cleanup=False))
        with self.assertRaisesRegex(ValueError, "private_media_gate_cleanup_incomplete"):
            freeze.validate_path_gate(value, self.profile, self.source)
        for key in ("publisher_stopped", "subscriber_stopped", "credentials_removed"):
            changed = deepcopy(value)
            changed["cleanup"][key] = False
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "private_media_gate_cleanup_incomplete"):
                freeze.validate_path_gate(changed, self.profile, self.source, require_cleanup=False)
        with self.assertRaisesRegex(ValueError, "private_media_gate_cleanup_incomplete"):
            freeze.validate_path_gate(self.gate, self.profile, self.source, require_cleanup=False)

    def test_gate_evidence_and_companion_worker_hashes_cannot_drift(self):
        self.freeze()
        for path in (self.gate_path, self.worker, self.shared_source, self.exe, self.exe.with_suffix(".pdb")):
            before = path.read_bytes()
            path.write_bytes(before + b" ")
            with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
                self.verify()
            path.write_bytes(before)

    def test_freeze_is_immutable_and_runtime_status_cannot_be_edited(self):
        result = self.freeze()
        before = self.manifest.read_bytes()
        with self.assertRaisesRegex(ValueError, "freeze_manifest_already_exists"):
            self.freeze()
        self.assertEqual(before, self.manifest.read_bytes())
        result["runtime_status"] = "PASS"
        self.write_json(self.manifest, result)
        with self.assertRaisesRegex(ValueError, "invalid_100_manifest"):
            self.verify()

    def test_unknown_credentials_and_non_integer_measurements_fail_closed(self):
        changed = deepcopy(self.gate)
        changed["api_secret"] = "offline-secret"
        with self.assertRaises(ValueError):
            freeze.validate_path_gate(changed, self.profile, self.source)
        changed = deepcopy(self.gate)
        changed["facts"]["private_ingress_rtp_packets"] = True
        with self.assertRaises(ValueError):
            freeze.validate_path_gate(changed, self.profile, self.source)

    def test_cli_target_and_ssh_public_fingerprint_cannot_change(self):
        self.freeze()
        for kwargs in ({"instance": "ins-other"}, {"transport": "workbench"},
                       {"service_url": "ws://192.0.2.11:17880"}, {"target_config": self.root / "other.json"}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ValueError, "100_runtime_target_mismatch"):
                self.verify(**kwargs)
        self.remote["key_public_fingerprint"] = "SHA256:changed"
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
