"""B11 probe admission/evidence checks with no observer or server access."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / "tools" / "meeting"
sys.path.insert(0, str(TOOLS))
try:
    import meeting_render_probe as probe
finally:
    sys.path.remove(str(TOOLS))


class ProbeAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="b11-probe-selftest-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.executable = self.root / "test_participant_window_remediation.exe"
        self.executable.write_bytes(b"inert selftest binary; never executed")
        self.manifest = self.root / "input-freeze.json"
        self.manifest.write_text('{"fixture": "manifest"}', encoding="utf-8")
        self.profile = self.root / "profile.json"
        self.profile.write_text('{"fixture": "profile"}', encoding="utf-8")
        self.identity = {
            "configuration": "RelWithDebInfo",
            "binary_path": str(self.executable),
            "binary_sha256": hashlib.sha256(self.executable.read_bytes()).hexdigest(),
            "pdb_path": "fixture/RelWithDebInfo/test_participant_window_remediation.pdb",
            "pdb_guid": "00000000-0000-0000-0000-000000000011",
            "pdb_age": 1,
        }
        self.frozen = {
            "inputs": {"profile": {"probe_mode": "grid16_transport", "probe": {
                           "observe_seconds": 300, "stall_seconds": 20,
                           "settle_seconds": 35, "maximum_wall_seconds": 420,
                           "minimum_remote_videos": 17, "receiver_count": 1}},
                       "binary_identity": self.identity},
            "remote_target": "BOUND_NOT_VERIFIED",
            "binary_source_equivalence": "UNKNOWN",
        }
        self.output = self.root / "evidence"
        self.launch = self.enterContext(patch.object(probe.subprocess, "Popen",
            side_effect=AssertionError("selftest must never launch observer")))
        self.desktop = self.enterContext(patch.object(probe.soak, "desktop_available",
                                                     return_value=False))
        self.enterContext(patch.object(probe.soak, "source_fingerprint", return_value={}))
        self.archive = self.enterContext(patch.object(probe.soak, "archive_evidence"))
        self.enterContext(patch.dict(os.environ, {
            "LIVEKIT_URL": "ws://203.0.113.42:18080",
            "LIVEKIT_SOAK_TOKEN": "token-canary-never-persisted",
        }))

    def assert_no_admission_side_effects(self):
        self.launch.assert_not_called()
        self.desktop.assert_not_called()
        self.archive.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_frozen_identity_and_scope_are_recorded_before_desktop_admission(self):
        with patch.object(probe.b11_freeze, "verify_inputs", return_value=self.frozen) as verify, \
                patch.object(probe.soak, "verify_runtime_binary") as fallback:
            result = probe.run(self.output, self.executable, True,
                              input_manifest=self.manifest, profile=self.profile)
        verify.assert_called_once_with(self.manifest, self.executable, self.profile,
            require_remote=True, service_url="ws://203.0.113.42:18080")
        fallback.assert_not_called()
        self.launch.assert_not_called()
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertEqual(result["reason"], "interactive_desktop_unavailable")
        self.assertEqual(result["formal_b11_status"], "NOT_RUN")
        self.assertTrue(result["diagnostic_only"])
        self.assertFalse(result["release_eligible"])
        record = json.loads((self.output / "run.json").read_text(encoding="utf-8"))
        self.assertEqual(record["build_configuration"], "RelWithDebInfo")
        self.assertEqual(record["binary_identity"], self.identity)
        self.assertEqual(record["input_freeze"], {
            "manifest_sha256": hashlib.sha256(self.manifest.read_bytes()).hexdigest(),
            "profile_sha256": hashlib.sha256(self.profile.read_bytes()).hexdigest(),
            "remote_target": "BOUND_NOT_VERIFIED",
            "binary_source_equivalence": "UNKNOWN",
        })
        self.assertEqual(record["acceptance_scope"], "B11_PREFLIGHT_DIAGNOSTIC")
        self.assertEqual(record["formal_b11_status"], "NOT_RUN")
        self.assertNotIn("token-canary-never-persisted", json.dumps(record))

    def test_partial_input_pair_rejects_before_identity_or_observer(self):
        for manifest, profile in ((self.manifest, None), (None, self.profile)):
            with self.subTest(manifest=manifest, profile=profile), \
                    patch.object(probe.b11_freeze, "verify_inputs") as verify, \
                    patch.object(probe.soak, "verify_runtime_binary") as binary:
                with self.assertRaisesRegex(ValueError, "input_manifest_and_profile_required_together"):
                    probe.run(self.output, self.executable, True,
                              input_manifest=manifest, profile=profile)
                verify.assert_not_called()
                binary.assert_not_called()
                self.assert_no_admission_side_effects()

    def test_frozen_mode_mismatch_rejects_before_observer(self):
        with patch.object(probe.b11_freeze, "verify_inputs", return_value=self.frozen), \
                patch.object(probe.soak, "verify_runtime_binary") as fallback:
            with self.assertRaisesRegex(ValueError, "frozen_probe_mode_mismatch"):
                probe.run(self.output, self.executable, grid16_transition=True,
                          input_manifest=self.manifest, profile=self.profile)
            fallback.assert_not_called()
        self.assert_no_admission_side_effects()

    def test_changed_or_pending_freeze_rejection_never_launches_observer(self):
        for reason in ("frozen_inputs_changed", "b11_remote_target_pending",
                       "b11_runtime_target_mismatch"):
            with self.subTest(reason=reason), patch.object(probe.b11_freeze, "verify_inputs",
                    side_effect=ValueError(reason)):
                with self.assertRaisesRegex(ValueError, reason):
                    probe.run(self.output, self.executable, True,
                              input_manifest=self.manifest, profile=self.profile)
                self.assert_no_admission_side_effects()

    def test_missing_runtime_service_binding_rejects_before_freeze_verification(self):
        with patch.dict(os.environ), patch.object(probe.b11_freeze, "verify_inputs") as verify:
            os.environ.pop("LIVEKIT_URL", None)
            with self.assertRaisesRegex(ValueError, "bound_service_environment_missing"):
                probe.run(self.output, self.executable, True,
                          input_manifest=self.manifest, profile=self.profile)
            verify.assert_not_called()
        self.assert_no_admission_side_effects()

    def test_frozen_measurement_threshold_drift_rejects_before_observer(self):
        self.frozen["inputs"]["profile"]["probe"]["observe_seconds"] = 30
        with patch.object(probe.b11_freeze, "verify_inputs", return_value=self.frozen):
            with self.assertRaisesRegex(ValueError, "frozen_probe_measurement_profile_mismatch"):
                probe.run(self.output, self.executable, True,
                          input_manifest=self.manifest, profile=self.profile)
        self.assert_no_admission_side_effects()

    def test_unfrozen_debug_identity_rejection_never_launches_observer(self):
        with patch.object(probe.soak, "verify_runtime_binary",
                side_effect=ValueError("runtime_binary_configuration_not_verified")) as verify, \
                patch.object(probe.b11_freeze, "verify_inputs") as frozen:
            with self.assertRaisesRegex(ValueError, "runtime_binary_configuration_not_verified"):
                probe.run(self.output, self.executable)
            verify.assert_called_once_with(self.executable)
            frozen.assert_not_called()
        self.assert_no_admission_side_effects()

    def test_mutually_exclusive_modes_reject_before_validation(self):
        with patch.object(probe.b11_freeze, "verify_inputs") as verify, \
                patch.object(probe.soak, "verify_runtime_binary") as binary:
            with self.assertRaisesRegex(ValueError, "select_one_probe_mode"):
                probe.run(self.output, self.executable, True, True,
                          input_manifest=self.manifest, profile=self.profile)
            verify.assert_not_called()
            binary.assert_not_called()
        self.assert_no_admission_side_effects()


class ClientResourceTests(unittest.TestCase):
    def sampler(self, cpu_values, counters=None):
        # Exercise interval/coverage logic without opening a Windows process handle.
        sampler = object.__new__(probe.ClientResourceSampler)
        sampler.pid = 77123
        sampler.handle = None
        sampler.sequence = 0
        sampler.previous_cpu = sampler.previous_monotonic = None
        sampler.logical_processor_count = 8
        counter_values = counters or {
            "private_bytes": 100_000_000, "working_set_bytes": 80_000_000, "handles": 350}
        self.enterContext(patch.object(probe.soak.ProcessMetrics, "sample",
            side_effect=lambda instance=None: dict(counter_values)))
        self.enterContext(patch.object(sampler, "_cpu_seconds", side_effect=cpu_values))
        return sampler

    def test_sampler_pins_the_supplied_observer_pid(self):
        def initialize(instance, pid):
            instance.pid = pid
            instance.handle = None
        with patch.object(probe.soak.ProcessMetrics, "__init__", autospec=True,
                side_effect=initialize) as initialize_parent:
            sampler = probe.ClientResourceSampler(77123)
        initialize_parent.assert_called_once_with(sampler, 77123)
        self.assertEqual(sampler.pid, 77123)
        self.assertIsNone(sampler.handle)

    def test_cpu_uses_monotonic_interval_and_explicit_host_denominator(self):
        sampler = self.sampler([20.0, 20.5])
        first = sampler.sample(10.0)
        second = sampler.sample(12.0)
        self.assertEqual(first["resource_status"], "BASELINE")
        self.assertIsNone(first["cpu_percent_of_one_core"])
        self.assertEqual(second["resource_status"], "AVAILABLE")
        self.assertEqual(second["resource_pid"], 77123)
        self.assertEqual(second["resource_sample_seq"], 2)
        self.assertEqual(second["cpu_sample_interval_seconds"], 2.0)
        self.assertEqual(second["cpu_percent_of_one_core"], 25.0)
        self.assertEqual(second["cpu_percent_of_host"], 3.125)
        summary = probe.client_resource_summary([first, second])
        self.assertEqual(summary["status"], "AVAILABLE")
        self.assertTrue(summary["quality_evidence_eligible"])
        self.assertEqual(summary["quality_acceptance_status"], "NOT_EVALUATED")

    def test_failed_reads_preserve_unknown_instead_of_zero(self):
        sampler = self.sampler([None, None], {
            "private_bytes": None, "working_set_bytes": None, "handles": None})
        samples = [sampler.sample(10.0), sampler.sample(11.0)]
        for row in samples:
            self.assertEqual(row["resource_status"], "UNKNOWN")
            for name in probe.RESOURCE_COUNTER_FIELDS:
                self.assertIsNone(row[name])
        summary = probe.client_resource_summary(samples)
        self.assertEqual(summary["status"], "UNKNOWN")
        self.assertFalse(summary["quality_evidence_eligible"])
        self.assertEqual(summary["unknown_samples"], 2)
        self.assertIsNone(summary["fields"]["private_bytes"]["peak"])

    def test_counter_reset_or_nonadvancing_clock_rejects_interval(self):
        for cpu_values, clock in (([20.0, 19.0], [10.0, 11.0]),
                                  ([20.0, 20.5], [10.0, 10.0])):
            with self.subTest(cpu_values=cpu_values, clock=clock):
                # Contexts expire per iteration, avoiding overlapping mock samplers.
                with patch.object(probe.soak.ProcessMetrics, "sample",
                        return_value={"private_bytes": 100, "working_set_bytes": 80,
                                      "handles": 3}):
                    sampler = object.__new__(probe.ClientResourceSampler)
                    sampler.pid, sampler.sequence = 77123, 0
                    sampler.previous_cpu = sampler.previous_monotonic = None
                    sampler.logical_processor_count = 8
                    with patch.object(sampler, "_cpu_seconds", side_effect=cpu_values):
                        first = dict(sampler.sample(clock[0]))
                        second = sampler.sample(clock[1])
                self.assertEqual(second["resource_status"], "UNKNOWN")
                self.assertIsNone(second["cpu_percent_of_one_core"])
                self.assertFalse(probe.client_resource_summary([first, second])[
                    "quality_evidence_eligible"])

    def test_partial_or_recovered_sampling_gap_blocks_quality_evidence(self):
        sampler = self.sampler([20.0, None, 21.0, 21.5])
        samples = [sampler.sample(value) for value in (10.0, 11.0, 12.0, 13.0)]
        self.assertEqual(samples[-1]["resource_status"], "AVAILABLE")
        summary = probe.client_resource_summary(samples)
        self.assertEqual(summary["status"], "UNKNOWN")
        self.assertFalse(summary["quality_evidence_eligible"])
        self.assertEqual(summary["unknown_samples"], 2)

    def test_no_interval_or_changed_pid_cannot_be_complete_evidence(self):
        sampler = self.sampler([20.0, 20.5])
        first = sampler.sample(10.0)
        self.assertEqual(probe.client_resource_summary([first])["status"], "UNKNOWN")
        second = sampler.sample(11.0)
        second["resource_pid"] = 77124
        summary = probe.client_resource_summary([first, second])
        self.assertFalse(summary["pid_consistent"])
        self.assertFalse(summary["quality_evidence_eligible"])


if __name__ == "__main__":
    unittest.main()
