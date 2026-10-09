"""Offline B11 input drift/security contracts; never SFU or media acceptance."""
from __future__ import annotations

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1] / "tools/meeting"
SPEC = importlib.util.spec_from_file_location("b11_input_freeze", HERE / "b11_input_freeze.py")
assert SPEC and SPEC.loader
freeze = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = freeze
SPEC.loader.exec_module(freeze)


def profile(*, bound=False):
    return {"schema": 1, "scope": freeze.SCOPE,
            "build_configuration": "RelWithDebInfo", "probe_mode": "grid16_transport",
            "target": {"instance": "i-plannedexample" if bound else None,
                       "service_url": "ws://192.0.2.10:17880" if bound else None,
                       "local_service_url": "http://127.0.0.1:17880" if bound else None,
                       "config_path": "/root/livekit.yaml", "sfu_container": "livekit",
                       "expected_vcpus": 16, "expected_memory_gib": 64,
                       "minimum_public_egress_mbps": 20},
            "publisher": deepcopy(freeze.PUBLISHER), "probe": deepcopy(freeze.PROBE)}


class FreezeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        for path in freeze.TOOL_INPUTS:
            self.write(path, "source\n")
        for path in ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "tests/CMakeLists.txt"):
            self.write(path, "source\n")
        for directory in ("src", "cmake", "tests/cmake", "tests/runtime/probes", "tests/support"):
            self.write(directory + "/input.cpp", "source\n")
        self.binary = self.root / "build/RelWithDebInfo" / freeze.EXECUTABLE_NAME
        self.binary.parent.mkdir(parents=True)
        self.binary.write_bytes(b"offline-executable-fixture")
        self.binary.with_suffix(".pdb").write_bytes(b"offline-pdb-fixture")
        self.dll = self.binary.parent / "renderer.dll"
        self.dll.write_bytes(b"offline-dll-fixture")
        self.dll.with_suffix(".pdb").write_bytes(b"offline-dll-pdb-fixture")
        self.write("build/CMakeCache.txt", "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(self.root) +
                   "\nCMAKE_CONFIGURATION_TYPES:STRING=Debug;RelWithDebInfo\n"
                   "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n")
        self.project = self.root / "build/tests/test_participant_window_remediation.vcxproj"
        self.write_project(self.binary.parent)
        self.profile = self.root / "profile.json"
        self.write_profile(profile())
        self.manifest = self.root / "manifest.json"
        self.head = "a" * 40
        self.addCleanup(patch.stopall)
        patch.object(freeze, "_git_head", side_effect=lambda _: self.head).start()
        # The real CodeView verifier is reused in production; fixture bytes cannot be executed.
        patch.object(freeze, "_binary_identity", side_effect=lambda path: {
            "configuration": "RelWithDebInfo", "binary_path": str(path),
            "binary_sha256": freeze.sha256(path), "pdb_path": str(path.with_suffix(".pdb")),
            "pdb_guid": "00000000-0000-0000-0000-000000000000", "pdb_age": 1}).start()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def write_project(self, output):
        self.project.parent.mkdir(parents=True, exist_ok=True)
        condition = "'$(Configuration)|$(Platform)'=='RelWithDebInfo|x64'"
        self.project.write_text(
            '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">'
            '<PropertyGroup><OutDir Condition="' + condition + '">' + str(output) + '</OutDir>'
            '<TargetName Condition="' + condition + '">test_participant_window_remediation</TargetName>'
            '<TargetExt Condition="' + condition + '">.exe</TargetExt></PropertyGroup></Project>',
            encoding="utf-8")

    def write_profile(self, value):
        self.profile.write_text(json.dumps(value, indent=2), encoding="utf-8")

    def create_freeze(self):
        return freeze.freeze_inputs(self.manifest, self.binary, self.profile, root=self.root)

    def verify(self, **kwargs):
        return freeze.verify_inputs(self.manifest, self.binary, self.profile, root=self.root, **kwargs)

    def bind_ssh(self, *, instance="ins-offlineexample"):
        config = self.write("out/ssh-target.json", '{"offline_fixture":true}\n')
        known_hosts = self.write("out/known_hosts", "offline-public-host-key\n")
        value = profile(bound=True)
        value.update(transport="ssh", target_config="out/ssh-target.json")
        value["target"]["instance"] = instance
        self.write_profile(value)
        self.ssh_identity = {"transport": "ssh", "host": "192.0.2.10", "port": 22,
                             "user": "ubuntu", "key_path": str(self.root / "not-read.pem"),
                             "known_hosts_path": str(known_hosts),
                             "key_public_fingerprint": "SHA256:offline-public-key"}

        def identity(path):
            self.assertEqual(path, config)
            return {**self.ssh_identity, "config_sha256": freeze.sha256(path),
                    "known_hosts_sha256": freeze.sha256(known_hosts)}

        # Public identity is mocked; these tests never open a private key or access SSH.
        patch.object(freeze, "_ssh_transport_identity", side_effect=identity).start()
        return config, known_hosts

    def test_offline_freeze_is_immutable_pending_and_runtime_not_run(self):
        created = self.create_freeze()
        self.assertEqual(self.verify(), created)
        self.assertEqual(created["remote_target"], "PENDING")
        self.assertEqual(created["runtime_status"], "NOT_RUN")
        self.assertEqual(created["binary_source_equivalence"], "UNKNOWN")
        original = self.manifest.read_bytes()
        with self.assertRaisesRegex(ValueError, "freeze_manifest_already_exists"):
            self.create_freeze()
        self.assertEqual(self.manifest.read_bytes(), original)
        with self.assertRaisesRegex(ValueError, "b11_remote_target_pending"):
            self.verify(require_remote=True)

    def test_bound_target_must_match_runtime_arguments(self):
        self.write_profile(profile(bound=True))
        created = self.create_freeze()
        self.assertEqual(created["remote_target"], "BOUND_NOT_VERIFIED")
        self.verify(require_remote=True, instance="i-plannedexample", service_url="ws://192.0.2.10:17880")
        for kwargs in ({"instance": "i-different"}, {"service_url": "ws://192.0.2.11:17880"}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ValueError, "b11_runtime_target_mismatch"):
                self.verify(require_remote=True, **kwargs)

    def test_ssh_binding_freezes_public_identity_and_preserves_verdict_boundaries(self):
        config, _ = self.bind_ssh()
        created = self.create_freeze()
        self.verify(require_remote=True, instance="ins-offlineexample", transport="ssh",
                    service_url="ws://192.0.2.10:17880", target_config=config)
        identity = created["inputs"]["remote_transport_identity"]
        self.assertEqual(identity["host"], "192.0.2.10")
        self.assertEqual(identity["config_path"], str(config))
        self.assertEqual(created["remote_target"], "BOUND_NOT_VERIFIED")
        self.assertEqual(created["runtime_status"], "NOT_RUN")
        self.assertEqual(created["binary_source_equivalence"], "UNKNOWN")
        for kwargs in ({"transport": "workbench"},
                       {"target_config": self.root / "other-target.json"}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ValueError, "b11_runtime_target_mismatch"):
                self.verify(require_remote=True, **kwargs)

    def test_ssh_credentials_and_known_hosts_drift_need_new_freeze(self):
        config, known_hosts = self.bind_ssh()
        self.create_freeze()
        for key, replacement in (("port", 2222), ("user", "root"),
                                 ("key_path", str(self.root / "different.pem")),
                                 ("key_public_fingerprint", "SHA256:different-public-key"),
                                 ("known_hosts_path", str(self.root / "different-known-hosts"))):
            with self.subTest(key=key):
                original = self.ssh_identity[key]
                self.ssh_identity[key] = replacement
                with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
                    self.verify(require_remote=True)
                self.ssh_identity[key] = original
        for path in (config, known_hosts):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
                    self.verify(require_remote=True)
                path.write_bytes(original)

    def test_ssh_host_must_match_service_origin_and_explicit_host_identity(self):
        self.bind_ssh(instance="ssh:192.0.2.10")
        self.create_freeze()
        self.ssh_identity["host"] = "192.0.2.11"
        with self.assertRaisesRegex(ValueError, "b11_ssh_target_mismatch"):
            self.verify(require_remote=True)
        self.ssh_identity["host"] = "192.0.2.10"
        changed = json.loads(self.profile.read_text(encoding="utf-8"))
        changed["target"]["instance"] = "ssh:192.0.2.11"
        self.write_profile(changed)
        with self.assertRaisesRegex(ValueError, "b11_ssh_target_mismatch"):
            self.verify(require_remote=True)

    def test_transport_schema_refuses_partial_or_arbitrary_remote_identity(self):
        for transport, config in (("ssh", None), ("other", "out/target.json"),
                                  ("workbench", "out/target.json"),
                                  ("ssh", "bad\npath")):
            with self.subTest(transport=transport, config=config):
                value = profile(bound=True)
                value["transport"] = transport
                if config is not None:
                    value["target_config"] = config
                with self.assertRaises(ValueError):
                    freeze.validate_profile(value)
        value = profile()
        value.update(transport="ssh", target_config="out/target.json")
        with self.assertRaisesRegex(ValueError, "incomplete_b11_remote_binding"):
            freeze.validate_profile(value)
        value = profile(bound=True)
        value.update(transport="ssh", target_config="out/target.json")
        value["target"]["instance"] = "i-aliyunexample"
        with self.assertRaisesRegex(ValueError, "invalid_b11_ssh_instance"):
            freeze.validate_profile(value)
        value = profile(bound=True)
        value["transport"] = "workbench"
        self.assertEqual(freeze.validate_profile(value), value)

    def test_same_size_source_and_head_drift_are_rejected(self):
        self.create_freeze()
        source = self.root / "src/input.cpp"
        source.write_text("other!\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()
        source.write_text("source\n", encoding="utf-8")
        self.head = "b" * 40
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()

    def test_source_file_addition_and_missing_mandatory_tool_reject(self):
        self.create_freeze()
        added = self.write("src/new_caller.cpp", "new owner")
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()
        added.unlink()
        (self.root / freeze.TOOL_INPUTS[1]).unlink()
        with self.assertRaisesRegex(ValueError, "required_input_missing"):
            self.verify()

    def test_executable_pdb_dll_and_companion_pdb_drift_reject(self):
        self.create_freeze()
        for path in (self.binary, self.binary.with_suffix(".pdb"), self.dll, self.dll.with_suffix(".pdb")):
            with self.subTest(path=path.name):
                content = path.read_bytes()
                path.write_bytes(content + b"changed")
                with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
                    self.verify()
                path.write_bytes(content)
        self.binary.with_suffix(".pdb").unlink()
        with self.assertRaisesRegex(ValueError, "required_input_missing"):
            self.verify()

    def test_new_deployed_dll_cannot_escape_inventory(self):
        self.create_freeze()
        self.write("build/RelWithDebInfo/plugins/new.dll", "new-runtime-library")
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()

    def test_unrelated_test_pdb_is_not_a_runtime_dependency(self):
        self.create_freeze()
        self.write("build/RelWithDebInfo/unrelated_test.pdb", "unrelated")
        self.verify()

    def test_profile_bytes_or_target_binding_change_need_new_freeze(self):
        self.create_freeze()
        self.profile.write_text(json.dumps(profile(), separators=(",", ":")), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()
        self.write_profile(profile(bound=True))
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()

    def test_target_and_build_metadata_cannot_be_spoofed_by_folder_name(self):
        with patch.object(freeze, "_binary_identity", return_value={"configuration": "Debug"}):
            with self.assertRaisesRegex(ValueError, "runtime_binary_configuration_not_verified"):
                self.create_freeze()
        self.write_project(self.binary.parent.parent / "Debug")
        with self.assertRaisesRegex(ValueError, "binary_target_output_not_verified"):
            self.create_freeze()
        self.write_project(self.binary.parent)
        (self.root / "build/CMakeCache.txt").unlink()
        with self.assertRaisesRegex(ValueError, "binary_build_metadata_missing"):
            self.create_freeze()

    def test_sensitive_urls_and_credentials_are_refused_before_manifest_write(self):
        for url in ("ws://user:secret@192.0.2.10:17880", "ws://192.0.2.10:17880?token=secret",
                    "ws://192.0.2.10:17880/#secret", "ws://192.0.2.10:17880/path",
                    "ws://192.0.2.10:17880?", "ws://192.0.2.10:17880\\secret"):
            with self.subTest(url=url):
                value = profile(bound=True)
                value["target"]["service_url"] = url
                self.write_profile(value)
                with self.assertRaisesRegex(ValueError, "invalid_service_origin"):
                    self.create_freeze()
                self.assertFalse(self.manifest.exists())
        value = profile()
        value["LIVEKIT_SOAK_TOKEN"] = "secret"
        self.write_profile(value)
        with self.assertRaisesRegex(ValueError, "invalid_b11_profile"):
            self.create_freeze()
        value = profile()
        value["target"]["api_secret"] = "secret"
        self.write_profile(value)
        with self.assertRaisesRegex(ValueError, "invalid_b11_target"):
            self.create_freeze()

    def test_partial_remote_target_and_invalid_port_are_refused(self):
        value = profile()
        value["target"]["instance"] = "i-example"
        with self.assertRaisesRegex(ValueError, "incomplete_b11_remote_binding"):
            freeze.validate_profile(value)
        value = profile(bound=True)
        value["target"]["local_service_url"] = "http://127.0.0.1:17881"
        with self.assertRaisesRegex(ValueError, "inconsistent_b11_service_ports"):
            freeze.validate_profile(value)

    def test_fixed_measurement_gate_cannot_be_weakened_or_bool_coerced(self):
        for section, key, replacement in (("probe", "observe_seconds", 5),
                                           ("probe", "minimum_remote_videos", 1),
                                           ("publisher", "count", 1),
                                           ("publisher", "simulcast", 1),
                                           ("target", "expected_vcpus", True)):
            with self.subTest(section=section, key=key):
                value = profile()
                value[section][key] = replacement
                with self.assertRaises(ValueError):
                    freeze.validate_profile(value)
        value = profile()
        value["probe_mode"] = "grid16_transition"
        value["probe"] = deepcopy(freeze.TRANSITION_PROBE)
        value["publisher"]["simulcast"] = False
        self.assertEqual(freeze.validate_profile(value), value)
        value["probe"]["step_observation_seconds"][-1] = 1
        with self.assertRaises(ValueError):
            freeze.validate_profile(value)

    def test_manifest_cannot_claim_pass_or_inject_secret_fields(self):
        frozen = self.create_freeze()
        for key, replacement in (("runtime_status", "PASS"),
                                 ("binary_source_equivalence", "VERIFIED"),
                                 ("api_secret", "secret")):
            with self.subTest(key=key):
                changed = deepcopy(frozen)
                changed[key] = replacement
                self.manifest.write_text(json.dumps(changed), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "invalid_b11_manifest"):
                    self.verify()

    def test_duplicate_profile_key_is_not_silently_accepted(self):
        content = json.dumps(profile())
        self.profile.write_text(content[:-1] + ', "schema": 1}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate_json_key"):
            self.create_freeze()


if __name__ == "__main__":
    unittest.main()
