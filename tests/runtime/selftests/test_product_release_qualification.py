"""Offline release admission checks; all PILOT and product files are inert fixtures."""
from __future__ import annotations

import json
import copy
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid


TOOLS = Path(__file__).resolve().parents[3] / "tests/runtime/tools/product_acceptance"
sys.path.insert(0, str(TOOLS))
import release_product_acceptance as release
from product_pilot_scheduler import load_scheduler_policy


class ReleaseQualificationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="product-release-admission-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        binary = self.root / "RelWithDebInfo/Cohavora.exe"
        binary.parent.mkdir()
        binary.write_text("Inert fixture only; never executed", encoding="utf-8")
        policy_path = TOOLS / "product_pilot_scheduler_policy.json"
        scheduler = load_scheduler_policy(policy_path)
        gpu_path = TOOLS / "product_gpu_queue_limits.json"
        gpu_policy = release.read(gpu_path)
        gpu_sha = release.digest(gpu_path)
        inputs = {str(binary): release.digest(binary), str(policy_path): release.digest(policy_path)}
        self.validation = dict(
            verdict="PASS", historical_crash_regression_closed=True,
            diagnostic_only=False, release_eligible=True, current_source_release_gate=True,
            configuration="RelWithDebInfo", desktop_input_policy="diagnostic",
            source_hashes={str(binary): release.digest(binary)},
            evidence_hashes={str(policy_path): release.digest(policy_path)})
        self.pilots = []
        for index in range(3):
            run_id = uuid.uuid4().hex
            path = self.root / f"pilot-{index + 1:02}"
            (path / "uia").mkdir(parents=True)
            plan = dict(run_id=run_id, mode="Pilot", cycles=2, load={"fixture": "same"},
                        desktop_input_policy="diagnostic", collector_scheduler_policy=scheduler,
                        gpu_queue_limits_sha256=gpu_sha)
            pilot = dict(run_id=run_id, verdict="PILOT_PASS", desktop_input_policy="diagnostic",
                         checks={"log_performance_complete": {"status": "PASS", "detail": "fixture"}})
            cycles = [dict(checks={name: "PASS" for name in release.FULL_MEDIA_GPU_CHECKS},
                           details={"backend_observed": ["dxgi"],
                                    "gpu_queue_coverage": {"policy_sha256": gpu_sha}})
                      for _ in range(2)]
            external = dict(
                run_id=run_id, verdict="PASS_WITH_DEFERRED", desktop_input_policy="diagnostic",
                deferred=[], cycles=cycles, cycle_counts={"PASS": 2},
                gpu_queue_frozen_limits={"sha256": gpu_sha, "policy": gpu_policy},
                final_checks={"collector_scheduler_policy_complete": True},
                collector_scheduler_policy={"passed": True, "policy_sha256": scheduler["sha256"]})
            documents = {
                "plan.json": plan, "pilot-review.json": pilot, "external-review.json": external,
                "runner-exit.json": dict(verdict="EVIDENCE_COMPLETE", exit_code=0, run_id=run_id),
                "executed-inputs.json": inputs, "uia/uia-result.json": {"desktop_input_policy": "diagnostic"}}
            for name, document in documents.items():
                (path / name).write_text(json.dumps(document), encoding="utf-8")
            shutil.copyfile(policy_path, path / "collector-scheduler-policy.json")
            self.pilots.append(path)

    def rewrite_run(self, root, run_id, names=None):
        for name in names or ("plan.json", "pilot-review.json", "external-review.json", "runner-exit.json"):
            path = root / name
            document = release.read(path)
            document["run_id"] = run_id
            path.write_text(json.dumps(document), encoding="utf-8")

    def evaluate(self, full=True):
        return release.evaluate(self.pilots, self.validation, require_full_media_gpu=full)

    def test_three_independent_full_pilots_can_qualify(self):
        for full in (False, True):
            with self.subTest(full_media_gpu=full):
                gate = self.evaluate(full)
                self.assertEqual(gate["verdict"], "READY")
                self.assertEqual(len({proof["run_id"] for proof in gate["pilots"]}), 3)
                self.assertEqual(gate["full_media_gpu_ready"], full)

    def test_copied_pilots_in_distinct_directories_cannot_qualify(self):
        # Reproduce the original failure with real copied evidence, not a mocked predicate.
        for path in self.pilots[1:]:
            shutil.rmtree(path)
            shutil.copytree(self.pilots[0], path)
        self.assertEqual(len({path.resolve() for path in self.pilots}), 3)
        for full in (False, True):
            with self.subTest(full_media_gpu=full):
                with self.assertRaisesRegex(ValueError, "^pilot_run_identity_reused$"):
                    self.evaluate(full)

    def test_run_identity_requires_canonical_32_lowercase_hex_characters(self):
        for run_id in (None, True, 123, "", "a" * 31, "a" * 33, "A" * 32, "g" * 32,
                       "a" * 32 + "\n", ["a" * 32]):
            with self.subTest(run_id=run_id):
                self.rewrite_run(self.pilots[0], run_id)
                with self.assertRaisesRegex(ValueError, "^pilot_run_identity_invalid$"):
                    self.evaluate()

    def test_locally_inconsistent_run_evidence_still_fails(self):
        root = self.pilots[0]
        original = release.read(root / "plan.json")["run_id"]
        for name, error in (("pilot-review.json", "review_run_identity_mismatch"),
                            ("external-review.json", "review_run_identity_mismatch"),
                            ("runner-exit.json", "runner_not_complete")):
            with self.subTest(document=name):
                self.rewrite_run(root, uuid.uuid4().hex, [name])
                with self.assertRaisesRegex(ValueError, "^" + error + "$"):
                    self.evaluate()
                self.rewrite_run(root, original, [name])

    def test_repeated_directory_cannot_qualify(self):
        self.pilots[2] = self.pilots[0]
        with self.assertRaisesRegex(ValueError, "^three_distinct_full_pairs_required$"):
            self.evaluate()

    def test_explicitly_excluded_scoped_pass_cannot_qualify(self):
        marker = self.pilots[0] / "qualification-excluded.json"
        run_id = release.read(self.pilots[0] / "plan.json")["run_id"]
        # The raw PASS files stay valid; only the user-approved scope excludes credit.
        declaration = dict(run_id=run_id, qualification_credit=0,
                           release_eligible=False, microphone_source="DEFERRED_BY_USER")
        for payload in (json.dumps(declaration), "", "invalid json"):
            marker.write_text(payload, encoding="utf-8")
            for full in (False, True):
                with self.subTest(payload=payload, full_media_gpu=full):
                    with self.assertRaisesRegex(ValueError, "^pilot_qualification_explicitly_excluded:"):
                        self.evaluate(full)
        self.assertEqual(release.read(self.pilots[0] / "pilot-review.json")["verdict"], "PILOT_PASS")

    @unittest.skipUnless(sys.platform == "win32", "Exercise real Windows PowerShell entry point")
    def test_excluded_first_pilot_stops_continuation_before_next_runtime(self):
        validation = self.root / "validation.json"
        validation.write_text(json.dumps(self.validation), encoding="utf-8")
        missing_executable = self.root / "fixture-missing-executable.exe"
        gate = self.root / "must-not-exist.json"
        formal = self.root / "must-not-start-formal"
        prefix = self.root / "must-not-start-pilot"
        first = self.pilots[0]
        for marker in ("qualification-excluded.json", "diagnostic-debugger.json"):
            with self.subTest(marker=marker):
                path = first / marker
                path.write_text("", encoding="utf-8")
                result = subprocess.run([
                    "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    str(TOOLS / "continue_product_acceptance.ps1"),
                    "-Validation", str(validation), "-DedicatedDesktopSessionId", "1",
                    "-DesktopInputPolicy", "diagnostic", "-Executable", str(missing_executable),
                    "-AudioCollector", str(missing_executable), "-PreparedDirectory", str(self.root),
                    "-FirstPilot", str(first), "-PilotPrefix", str(prefix),
                    "-Gate", str(gate), "-FormalRoot", str(formal)],
                    capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn("CURRENT_SOURCE_VALIDATION_PASS", output)
                self.assertIn("FIRST_PILOT_QUALIFICATION_EXCLUDED", output)
                self.assertNotIn("PILOT_FAILED:", output)
                self.assertFalse(gate.exists())
                self.assertFalse(formal.exists())
                self.assertFalse(Path(str(prefix) + "-02").exists())
                self.assertFalse(Path(str(prefix) + "-03").exists())
                path.unlink()

    def test_current_source_rejects_unqualified_or_incomplete_validation(self):
        cases = (("verdict", "FAIL", "focused_or_crash_regression_not_closed"),
                 ("historical_crash_regression_closed", 1, "focused_or_crash_regression_not_closed"),
                 ("diagnostic_only", True, "current_source_validation_not_release_eligible"),
                 ("release_eligible", False, "current_source_validation_not_release_eligible"),
                 ("current_source_release_gate", False, "current_source_validation_not_release_eligible"),
                 ("configuration", "Debug", "current_source_validation_configuration_invalid"),
                 ("source_hashes", {}, "current_source_validation_hashes_missing"),
                 ("evidence_hashes", {}, "current_source_validation_hashes_missing"))
        for field, value, error in cases:
            with self.subTest(field=field):
                validation = dict(self.validation, **{field: value})
                with self.assertRaisesRegex(ValueError, error):
                    release.validate_current_source(validation, True, "diagnostic")
        with self.assertRaisesRegex(ValueError, "desktop_input_policy_mismatch"):
            release.validate_current_source(self.validation, True, "strict")

    def test_current_source_rechecks_source_and_evidence_bytes(self):
        for field, error in (("source_hashes", "validated_source_changed"),
                             ("evidence_hashes", "validation_evidence_changed")):
            with self.subTest(field=field):
                validation = copy.deepcopy(self.validation)
                path = next(iter(validation[field]))
                validation[field][path] = "0" * 64
                with self.assertRaisesRegex(ValueError, error):
                    release.validate_current_source(validation, True, "diagnostic")
                validation[field] = {str(self.root / "missing-proof"): "0" * 64}
                with self.assertRaises(FileNotFoundError):
                    release.validate_current_source(validation, True, "diagnostic")

    def test_final_release_reuses_current_source_validation(self):
        self.validation["release_eligible"] = False
        with self.assertRaisesRegex(ValueError, "current_source_validation_not_release_eligible"):
            self.evaluate()

    def test_validate_only_cannot_issue_gate_and_legacy_cli_requires_pilots(self):
        validation = self.root / "validation.json"
        validation.write_text(json.dumps(self.validation), encoding="utf-8")
        command = [sys.executable, str(TOOLS / "release_product_acceptance.py"),
                   "--validation", str(validation), "--require-full-media-gpu"]
        result = subprocess.run(command + ["--validate-only", "--desktop-input-policy", "diagnostic"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout),
                         {"verdict": "CURRENT_SOURCE_VALIDATION_PASS", "release_gate_issued": False})
        gate_path = self.root / "must-not-exist.json"
        for args in (["--validate-only", "--output", str(gate_path)], []):
            result = subprocess.run(command + args, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(gate_path.exists())

    @unittest.skipUnless(sys.platform == "win32", "Exercise real Windows PowerShell entry points")
    def test_controllers_reject_invalid_validation_before_any_runtime_entry(self):
        # Run the real entry scripts. Beyond the gate, safe fixture failures stop
        # the valid cases before any dot-source, desktop, lock, wake or PILOT call.
        first = self.root / "first"
        first.mkdir()
        (first / "runner-exit.json").write_text(
            json.dumps({"verdict": "FIXTURE_STOP", "exit_code": 1}), encoding="utf-8")
        validation_path = self.root / "validation.json"
        missing_executable = self.root / "RelWithDebInfo/fixture-missing-executable.exe"
        common = ["-Validation", str(validation_path), "-DedicatedDesktopSessionId", "1",
                  "-DesktopInputPolicy", "diagnostic", "-Executable", str(missing_executable),
                  "-AudioCollector", str(missing_executable), "-PreparedDirectory", str(self.root)]
        entries = {
            "run_b14_acceptance.ps1": ["-EvidenceDirectory", str(self.root)],
            "continue_product_acceptance.ps1": ["-FirstPilot", str(first), "-PilotPrefix", str(self.root / "pilot"),
                                                "-Gate", str(self.root / "gate.json"),
                                                "-FormalRoot", str(self.root / "formal")]}
        for case in ("missing", "fail", "stale_source", "stale_evidence", "nonrelease", "valid"):
            validation = copy.deepcopy(self.validation)
            if case == "fail":
                validation["verdict"] = "FAIL"
            elif case == "nonrelease":
                validation["release_eligible"] = False
            elif case in ("stale_source", "stale_evidence"):
                field = "source_hashes" if case == "stale_source" else "evidence_hashes"
                validation[field][next(iter(validation[field]))] = "0" * 64
            if case != "missing":
                validation_path.write_text(json.dumps(validation), encoding="utf-8")
            for script, args in entries.items():
                with self.subTest(case=case, entry=script):
                    result = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                             "-File", str(TOOLS / script), *common, *args],
                                            capture_output=True, text=True, timeout=30)
                    output = result.stdout + result.stderr
                    self.assertNotEqual(result.returncode, 0, output)
                    if case == "valid":
                        self.assertIn("CURRENT_SOURCE_VALIDATION_PASS", output)
                        self.assertNotIn("CURRENT_SOURCE_VALIDATION_REJECTED", output)
                        self.assertIn("FIRST_PILOT_FAILED" if script.startswith("continue")
                                      else "fixture-missing-executable", output)
                    else:
                        self.assertIn("CURRENT_SOURCE_VALIDATION_REJECTED", output)
                        self.assertNotIn("CURRENT_SOURCE_VALIDATION_PASS", output)
            self.assertFalse((self.root / "gate.json").exists())
            self.assertFalse((self.root / "controller-state.json").exists())
            self.assertFalse((self.root / "formal").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
