"""Short, offline fault-injection tests. These do not constitute L3 evidence."""

from __future__ import annotations

import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile


HERE = Path(__file__).resolve().parents[1] / "tools/meeting"
SPEC = importlib.util.spec_from_file_location("meeting_soak", HERE / "meeting_soak.py")
assert SPEC is not None and SPEC.loader is not None
soak = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = soak
SPEC.loader.exec_module(soak)
MIB = 1024 * 1024


def process_alive(pid: int) -> bool:
    if os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = (ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.GetExitCodeProcess.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong))
        kernel.GetExitCodeProcess.restype = ctypes.c_int
        kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return False
        try:
            code = ctypes.c_ulong()
            return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == 259
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


class PlanTests(unittest.TestCase):
    @staticmethod
    def profile(*, diagnostic=False, steady=1800, mixed=7200):
        return {"schema": soak.SCHEMA, "steady_seconds": steady, "mixed_seconds": mixed,
                "mode": "diagnostic" if diagnostic else "formal", **soak.evidence_scope(diagnostic),
                "executable": sys.executable, "min_remote_videos": 1 if diagnostic else 17,
                "max_growth_mib": None, "max_slope_mib_per_hour": None}

    def test_only_explicit_diagnostic_allows_short_or_single_phase_plans(self):
        for steady, mixed in ((10, 0), (30, 60), (28800, 0), (1800, 0), (1801, 7200)):
            with self.subTest(steady=steady, mixed=mixed):
                with self.assertRaises(ValueError):
                    soak.build_plan(steady, mixed)
                plan = soak.build_plan(steady, mixed, diagnostic=True)
                self.assertEqual(plan[0]["action"], "grid9")
                self.assertEqual(any(step["phase"] == "mixed" for step in plan), mixed > 0)
        for value in (True, "10", float("nan"), float("inf"), -1, 0):
            with self.subTest(steady=value), self.assertRaises(ValueError):
                soak.build_plan(value, 0, diagnostic=True)
        for value in (True, "0", float("nan"), float("inf"), -1):
            with self.subTest(mixed=value), self.assertRaises(ValueError):
                soak.build_plan(10, value, diagnostic=True)

    def test_scope_flags_and_credit_have_strict_types(self):
        plan = soak.build_plan()
        for field, values in (("diagnostic_only", ("false", 0, 1, None)),
                              ("allow_short", ("false", 0, 1, True)),
                              ("release_eligible", ("false", 0, True)),
                              ("qualification_credit", (False, "0", 1))):
            for value in values:
                profile = self.profile()
                profile[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    soak.validate_prepared_profile(profile, plan)

    def test_legacy_formal_profile_remains_valid(self):
        profile = self.profile()
        for name in ("mode", *soak.SCOPE_FIELDS):
            profile.pop(name)
        self.assertEqual(soak.validate_prepared_profile(profile, soak.build_plan()),
                         soak.evidence_scope(False))

    def test_short_diagnostic_cannot_be_relabelled_formal(self):
        plan = soak.build_plan(10, 0, diagnostic=True)
        original = self.profile(diagnostic=True, steady=10, mixed=0)
        for change in ({"diagnostic_only": False}, {"mode": "formal"},
                       {"mode": "formal", "diagnostic_only": False}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                soak.validate_prepared_profile({**original, **change}, plan)
        stripped = {key: value for key, value in original.items() if key not in ("mode", *soak.SCOPE_FIELDS)}
        with self.assertRaises(ValueError):
            soak.validate_prepared_profile(stripped, plan)

    def test_prepared_numeric_fields_do_not_accept_bool_or_nonfinite_values(self):
        plan = soak.build_plan()
        for field, values in (("steady_seconds", (True, "1800", float("inf"))),
                              ("mixed_seconds", (False, "7200", float("nan"))),
                              ("min_remote_videos", (True, "17", 16)),
                              ("memory_warmup", (False, "300", 0)),
                              ("build_configuration", ("Debug", True))):
            for value in values:
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    soak.validate_prepared_profile({**self.profile(), field: value}, plan)

    def test_cli_diagnostic_aliases_emit_scope_and_relwithdebinfo_without_fallback(self):
        with tempfile.TemporaryDirectory(prefix="soak-diagnostic-plan-") as directory:
            for index, flag in enumerate(("--diagnostic", "--smoke", "--allow-short")):
                output = Path(directory) / str(index)
                self.assertEqual(soak.main(["prepare", "--output", str(output), flag,
                    "--steady-seconds", "20", "--mixed-minutes", "0", "--min-remote-videos", "1"]), 0)
                profile = soak.read_json(output / "profile.json")
                self.assertEqual(profile["mode"], "diagnostic")
                self.assertEqual(Path(profile["executable"]).parent.name, "RelWithDebInfo")
                self.assertEqual(profile["build_configuration"], "RelWithDebInfo")
                for name in ("profile.json", "preparation.json"):
                    evidence = soak.read_json(output / name)
                    for key, value in soak.evidence_scope(True).items():
                        self.assertEqual(evidence[key], value)
                self.assertEqual(soak.read_json(output / "preparation.json")["l3_status"], "NOT_RUN")

    def test_library_invalid_parameters_cannot_create_output_or_start_process(self):
        with tempfile.TemporaryDirectory(prefix="soak-invalid-library-") as directory:
            output = Path(directory) / "run"
            defaults = dict(command=[sys.executable], plan=soak.build_plan(10, 0, diagnostic=True),
                            duration_seconds=10, diagnostic=True)
            invalid = ({"diagnostic": "false"}, {"self_test": 1}, {"duration_seconds": True},
                       {"duration_seconds": float("nan")}, {"heartbeat_timeout": 0},
                       {"runtime_timeout": float("inf")}, {"sample_interval": -1},
                       {"memory_warmup": True}, {"min_remote_videos": False},
                       {"min_remote_videos": 0}, {"max_growth_mib": 1},
                       {"max_growth_mib": True, "max_slope_mib_per_hour": 1},
                       {"diagnostic": False}, {"command": []}, {"plan": []},
                       {"plan": [{"at_s": False, "phase": "steady", "cycle": 0, "action": "grid9"}]})
            with patch.object(soak.subprocess, "Popen") as spawn:
                for change in invalid:
                    with self.subTest(change=change), self.assertRaises(ValueError):
                        soak.run_session(output, **{**defaults, **change})
                    self.assertFalse(output.exists())
                spawn.assert_not_called()

    def test_runtime_binary_requires_existing_pe_configuration_verifier(self):
        executable = Path(sys.executable)
        identity = {"configuration": "RelWithDebInfo", "binary_sha256": soak.sha256(executable)}
        response = subprocess.CompletedProcess([], 0, stdout=json.dumps(identity), stderr="")
        with patch.object(soak.subprocess, "run", return_value=response) as verify:
            self.assertEqual(soak.verify_runtime_binary(executable), identity)
            command = verify.call_args.args[0]
            self.assertIn("-Configuration", command)
            self.assertEqual(command[-1], "RelWithDebInfo")
            self.assertIn("test_participant_window_remediation.exe", command)
            self.assertTrue(any(part.endswith("verify_runtime_binary.ps1") for part in command))
        for bad in ({**identity, "configuration": "Debug"}, {**identity, "binary_sha256": "bad"}):
            response.stdout = json.dumps(bad)
            with patch.object(soak.subprocess, "run", return_value=response), self.assertRaises(ValueError):
                soak.verify_runtime_binary(executable)
        with patch.object(soak.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "pwsh")), \
                self.assertRaises(ValueError):
            soak.verify_runtime_binary(executable)

    def test_render_path_diagnostics_keep_numeric_evidence_safe(self) -> None:
        status = {
            "schema": soak.SCHEMA, "run_id": "diagnostic", "pid": 42,
            "heartbeat_seq": 1, "runtime_seq": 1, "command_seq": 0,
            "policy_revision": 1, "requested": 8, "selected": 8, "actual": 8,
            "bound": 8, "selected_not_bound": 0, "bound_not_selected": 0,
            "remote_video_count": 17, "render_submits": 100, "decoded_frames": 120,
            "state": "connected", "command_status": "applied",
            "render_router_submitted": 130, "render_delivered_to_gpu": 110,
            "render_expected_bindings": 8, "render_timer_active": True,
            "untrusted_detail": "do not archive",
        }
        safe = soak.validate_status(status, "diagnostic", 42)
        self.assertEqual(safe["render_router_submitted"], 130)
        self.assertTrue(safe["render_timer_active"])
        self.assertNotIn("untrusted_detail", safe)
        status["render_router_submitted"] = "130; secret"
        with self.assertRaises(soak.StopRun):
            soak.validate_status(status, "diagnostic", 42)

    def test_default_schedule_covers_both_phases_and_all_mixed_actions(self) -> None:
        plan = soak.build_plan(steady_seconds=1800, mixed_seconds=7200)
        self.assertTrue(plan)
        self.assertEqual([step["at_s"] for step in plan], sorted(step["at_s"] for step in plan))
        self.assertTrue(all(0 <= step["at_s"] < 9000 for step in plan))
        self.assertTrue(all(step["phase"] == "steady" for step in plan if step["at_s"] < 1800))
        mixed = [step for step in plan if step["phase"] == "mixed"]
        self.assertTrue(mixed)
        self.assertGreaterEqual(min(step["at_s"] for step in mixed), 1800)
        self.assertGreaterEqual(len({step["cycle"] for step in mixed}), 2)
        self.assertTrue({"grid4", "grid9", "grid16", "next_page", "pin", "unpin",
                         "whiteboard_on", "whiteboard_off", "share_start", "share_stop",
                         "soft_reconnect", "full_reconnect"}.issubset(
                             {step["action"] for step in mixed}))
        self.assertNotIn("stop", {step["action"] for step in plan})

    def test_cli_rejects_short_mixed_phase(self) -> None:
        with tempfile.TemporaryDirectory(prefix="soak-plan-") as directory:
            output = Path(directory) / "prepared"
            result = subprocess.run(
                [sys.executable, str(HERE / "meeting_soak.py"), "prepare",
                 "--output", str(output), "--mixed-minutes", "119"],
                capture_output=True, text=True, timeout=3,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((output / "profile.json").exists())

    def test_run_cannot_accept_duration_override(self) -> None:
        result = subprocess.run(
            [sys.executable, str(HERE / "meeting_soak.py"), "run",
             "--prepared", "unused-offline-fixture", "--duration", "0.1"],
            capture_output=True, text=True, timeout=3,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--duration", result.stdout + result.stderr)

    def test_prepared_duration_tampering_cannot_launch_peer(self) -> None:
        with tempfile.TemporaryDirectory(prefix="soak-plan-") as directory:
            output = Path(directory) / "prepared"
            result = subprocess.run(
                [sys.executable, str(HERE / "meeting_soak.py"), "prepare",
                 "--output", str(output), "--executable", sys.executable],
                capture_output=True, text=True, timeout=3,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            profile_path = output / "profile.json"
            profile = json.loads(profile_path.read_text(encoding="utf-8"))
            self.assertEqual(profile["steady_seconds"], 1800)
            self.assertGreaterEqual(profile["mixed_seconds"], 7200)
            profile["steady_seconds"] = 1
            profile_path.write_text(json.dumps(profile), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(HERE / "meeting_soak.py"), "run", "--prepared", str(output)],
                capture_output=True, text=True, timeout=3,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(any(output.rglob("fake_peer.pid")))

    def test_missing_service_environment_archives_not_run_without_token(self) -> None:
        marker = "fake-soak-token-for-offline-test-only-4f2e9b"
        environment = os.environ.copy()
        environment.pop("LIVEKIT_URL", None)
        environment["LIVEKIT_SOAK_TOKEN"] = marker
        with tempfile.TemporaryDirectory(prefix="soak-prerequisites-") as directory:
            output = Path(directory) / "prepared"
            prepared = subprocess.run(
                [sys.executable, str(HERE / "meeting_soak.py"), "prepare",
                 "--output", str(output), "--executable", sys.executable],
                capture_output=True, text=True, timeout=3, env=environment,
            )
            self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)
            result = subprocess.run(
                [sys.executable, str(HERE / "meeting_soak.py"), "run", "--prepared", str(output)],
                capture_output=True, text=True, timeout=3, env=environment,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("NOT_RUN", result.stdout)
            self.assertNotIn(marker, prepared.stdout + prepared.stderr + result.stdout + result.stderr)
            summaries = list(output.rglob("summary.json"))
            self.assertEqual(len(summaries), 1)
            summary = json.loads(summaries[0].read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "NOT_RUN")
            self.assertEqual(summary["l3_status"], "NOT_RUN")
            self.assertEqual(summary["reason"], "missing_executable_or_service_environment")
            self.assertFalse(any(output.rglob("run.json")), "Prerequisite failure must not start a session")
            for path in output.rglob("*"):
                if path.is_file() and path.suffix != ".zip":
                    self.assertNotIn(marker.encode(), path.read_bytes(), str(path))
            archives = list(output.rglob("*.zip"))
            self.assertEqual(len(archives), 1)
            with zipfile.ZipFile(archives[0]) as archive:
                for name in archive.namelist():
                    self.assertNotIn(marker.encode(), archive.read(name), name)


class MemoryTrendTests(unittest.TestCase):
    @staticmethod
    def samples(slope: float) -> list[dict]:
        return [{"elapsed_s": second, "private_bytes": round((100 + slope * second / 3600) * MIB)}
                for second in range(0, 3601, 60)]

    def test_known_linear_growth(self) -> None:
        trend = soak.memory_trend(self.samples(12), warmup_seconds=300)
        self.assertEqual(trend["status"], "VALID")
        self.assertAlmostEqual(trend["slope_mib_per_hour"], 12, places=4)
        # Robust growth compares median edge windows, rather than single endpoints.
        self.assertAlmostEqual(trend["growth_mib"], 9, places=4)
        self.assertGreaterEqual(trend["sample_count"], 3)

    def test_stable_plateau(self) -> None:
        trend = soak.memory_trend(self.samples(0), warmup_seconds=300)
        self.assertEqual(trend["status"], "VALID")
        self.assertAlmostEqual(trend["slope_mib_per_hour"], 0)
        self.assertAlmostEqual(trend["growth_mib"], 0)

    def test_warmup_is_excluded(self) -> None:
        samples = self.samples(0)
        for sample in samples:
            if sample["elapsed_s"] < 300:
                sample["private_bytes"] = int((20 + sample["elapsed_s"] / 3) * MIB)
        trend = soak.memory_trend(samples, warmup_seconds=300)
        self.assertEqual(trend["status"], "VALID")
        self.assertAlmostEqual(trend["slope_mib_per_hour"], 0)

    def test_dense_samples_do_not_replace_duration(self) -> None:
        samples = [{"elapsed_s": index / 100, "private_bytes": 100 * MIB}
                   for index in range(1000)]
        trend = soak.memory_trend(samples, warmup_seconds=0)
        self.assertEqual(trend["status"], "INSUFFICIENT_DATA")

    def test_no_post_warmup_samples(self) -> None:
        self.assertEqual(soak.memory_trend([], warmup_seconds=300)["status"], "INSUFFICIENT_DATA")
        self.assertEqual(soak.memory_trend(
            [{"elapsed_s": 1, "private_bytes": 100 * MIB}], warmup_seconds=300,
        )["status"], "INSUFFICIENT_DATA")

    def test_low_warmup_does_not_lower_shared_measurement_span(self):
        samples = [{"elapsed_s": second, "private_bytes": 100 * MIB} for second in (0, 10, 20)]
        for warmup in (0, 5):
            with self.subTest(warmup=warmup):
                self.assertEqual(soak.memory_trend(samples, warmup)["status"], "INSUFFICIENT_DATA")

    @staticmethod
    def settled_samples():
        return [{"elapsed_s": second, "private_bytes": 100 * MIB, "settled": True,
                 "action": "grid9", "phase": phase}
                for phase, seconds in (("steady", (300, 360, 420)), ("mixed", (1800, 1860, 1920)))
                for second in seconds]

    def test_schedule_and_memory_pass_cannot_award_diagnostic_or_selftest_credit(self):
        for flags in ({"diagnostic": True}, {"self_test": True},
                      {"diagnostic": True, "self_test": True}):
            with self.subTest(flags=flags):
                result = soak.finalize_result({"status": "PASS"}, self.settled_samples(), soak.build_plan(),
                    max_growth_mib=1, max_slope_mib_per_hour=1, **flags)
                self.assertEqual(result["schedule_status"], "PASS")
                self.assertEqual(result["memory"]["gate"], "PASS")
                self.assertEqual(result["status"], "PASS")
                self.assertEqual(result["l3_status"], "NOT_RUN")
                self.assertEqual({name: result[name] for name in soak.SCOPE_FIELDS}, soak.evidence_scope(True))

    def test_mixed_is_skipped_only_when_absent_from_plan(self):
        samples = [sample for sample in self.settled_samples() if sample["phase"] == "steady"]
        result = soak.finalize_result({"status": "PASS"}, samples, soak.build_plan(1800, 0, diagnostic=True),
            max_growth_mib=1, max_slope_mib_per_hour=1, diagnostic=True)
        self.assertEqual(result["memory"]["mixed_grid9"]["status"], "SKIPPED")
        self.assertEqual(result["memory"]["gate"], "PASS")
        for diagnostic in (False, True):
            result = soak.finalize_result({"status": "PASS"}, samples, soak.build_plan(),
                max_growth_mib=1, max_slope_mib_per_hour=1, diagnostic=diagnostic)
            self.assertEqual(result["memory"]["mixed_grid9"]["status"], "INSUFFICIENT_DATA")
            self.assertEqual(result["memory"]["gate"], "INSUFFICIENT_DATA")
            self.assertNotEqual(result["l3_status"], "PASS")

    def test_formal_scoped_l3_gate_keeps_zero_b14_credit(self):
        result = soak.finalize_result({"status": "PASS"}, self.settled_samples(), soak.build_plan(),
            max_growth_mib=1, max_slope_mib_per_hour=1)
        self.assertEqual(result["l3_status"], "PASS")
        self.assertFalse(result["diagnostic_only"])
        self.assertFalse(result["release_eligible"])
        self.assertEqual(result["qualification_credit"], 0)


class SupervisorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Real provenance is captured once before the short fault budget. Git and
        # source hashing are not the supervisor's heartbeat/convergence work.
        cls.source_inputs = soak.source_fingerprint()
        cls.platform_inputs = {name: getattr(soak.platform, name)()
                               for name in ("system", "release", "machine")}

    def run_fixture(self, output: Path, mode: str) -> dict:
        # Preload the real fake-peer code before measuring its supervision. Its
        # fault clock starts only after run_session passes this run's environment.
        # This follows the ready/start boundary used by product_uia_retest tests.
        bootstrap = output.parent / (output.name + "-bootstrap")
        bootstrap.mkdir()
        worker = r'''
import json, os, pathlib, runpy, sys, time
bootstrap = pathlib.Path(sys.argv[1])
peer_path, output, mode = sys.argv[2:5]
peer = runpy.run_path(peer_path, run_name="soak_ready_fixture")
(bootstrap / "ready").write_text("ready", encoding="ascii")
deadline = time.monotonic() + 5
while not (bootstrap / "start.json").exists():
    if time.monotonic() >= deadline:
        sys.exit(92)
    time.sleep(.005)
os.environ.update(json.loads((bootstrap / "start.json").read_text(encoding="utf-8")))
sys.argv = [peer_path, "--soak-directory", output, "--mode", mode]
sys.exit(peer["main"]())
'''
        command = [sys.executable, str(HERE / "soak_fake_peer.py"),
                   "--soak-directory", str(output), "--mode", mode]
        real_popen = subprocess.Popen
        child = real_popen(
            [sys.executable, "-c", worker, str(bootstrap), str(HERE / "soak_fake_peer.py"), str(output), mode],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        def release_peer(args, *extra, **kwargs):
            # platform and source_fingerprint share stdlib subprocess; only the
            # owned peer is substituted, unrelated probes keep their real API.
            if args != command:
                return real_popen(args, *extra, **kwargs)
            soak.atomic_json(bootstrap / "start.json", {
                "COHAVORA_SOAK_RUN_ID": kwargs["env"]["COHAVORA_SOAK_RUN_ID"]})
            return child
        try:
            deadline = time.monotonic() + 5
            while not (bootstrap / "ready").exists() and child.poll() is None and time.monotonic() < deadline:
                time.sleep(.005)
            self.assertTrue((bootstrap / "ready").is_file(), "Fake peer failed to initialize")
            self.assertIsNone(child.poll(), "Fake peer exited before supervision")
            started = time.monotonic()
            with patch.object(soak, "source_fingerprint", return_value=self.source_inputs), \
                    patch.object(soak.platform, "system", return_value=self.platform_inputs["system"]), \
                    patch.object(soak.platform, "release", return_value=self.platform_inputs["release"]), \
                    patch.object(soak.platform, "machine", return_value=self.platform_inputs["machine"]), \
                    patch.object(soak.subprocess, "Popen", side_effect=release_peer):
                result = soak.run_session(
                    output=output,
                    command=command,
                    plan=[{"at_s": 0, "action": "grid4", "phase": "steady", "cycle": 0}],
                    duration_seconds=0.8,
                    heartbeat_timeout=0.2,
                    runtime_timeout=0.22,
                    command_timeout=0.35,
                    sample_interval=0.02,
                    startup_timeout=2,
                    shutdown_timeout=0.25,
                    min_remote_videos=17,
                    memory_warmup=0,
                    self_test=True,
                )
        finally:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
        self.assertLess(time.monotonic() - started, 3, "Short fixture unexpectedly blocked")
        self.assertEqual(result["l3_status"], "NOT_RUN")
        for name in ("run.json", "profile.json", "summary.json", "manifest.json"):
            evidence = soak.read_json(output / name)
            for key, value in soak.evidence_scope(True).items():
                self.assertEqual(evidence[key], value, (name, key))
        self.assertFalse(process_alive(int((output / "fake_peer.pid").read_text(encoding="ascii"))),
                         "Supervisor must reap its owned child on success and failure")
        disk_summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(disk_summary["status"], result["status"])
        self.assertEqual(disk_summary["reason"], result["reason"])
        for filename in ("events.jsonl", "metrics.csv", "manifest.json"):
            self.assertTrue((output / filename).is_file(), filename)
        archives = list(output.parent.glob(output.name + "*.zip"))
        self.assertEqual(len(archives), 1)
        with zipfile.ZipFile(archives[0]) as archive:
            self.assertIsNone(archive.testzip())
            names = {Path(name).name for name in archive.namelist()}
            self.assertTrue({"summary.json", "events.jsonl", "metrics.csv", "manifest.json"}.issubset(names))
            summary_name = next(name for name in archive.namelist() if Path(name).name == "summary.json")
            self.assertEqual(json.loads(archive.read(summary_name))["status"], result["status"])
        return result

    def test_normal_completion_and_archive(self) -> None:
        with tempfile.TemporaryDirectory(prefix="soak-normal-") as directory:
            output = Path(directory) / "run"
            result = self.run_fixture(output, "normal")
            self.assertIn(result["status"], ("PASS", "SELF_TEST_PASS"))
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            files = manifest["files"]
            entries = files.items() if isinstance(files, dict) else (
                (item["name"], item["sha256"]) for item in files)
            hashes_checked = 0
            for relative, digest in entries:
                if isinstance(digest, dict):
                    digest = digest["sha256"]
                artifact = output / relative
                self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(), digest, relative)
                hashes_checked += 1
            self.assertGreaterEqual(hashes_checked, 4)
            archive = output.with_suffix(".zip")
            self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(),
                             archive.with_suffix(".zip.sha256").read_text(encoding="ascii").strip())

    def test_faults_cannot_report_pass(self) -> None:
        for mode in ("crash", "hang", "strand-hang", "reject", "wrong-run",
                     "exit-zero-early", "missing-stop"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="soak-fault-") as directory:
                result = self.run_fixture(Path(directory) / "run", mode)
                self.assertNotIn(result["status"], ("PASS", "SELF_TEST_PASS"))
                expected_reason = {
                    "crash": "process_exited_early", "hang": "ui_heartbeat_timeout",
                    "strand-hang": "runtime_heartbeat_timeout", "reject": "command_rejected",
                    "wrong-run": "status_identity_mismatch", "exit-zero-early": "process_exited_early",
                    "missing-stop": "shutdown_timeout",
                }[mode]
                self.assertEqual(result["reason"], expected_reason)
                if mode in ("crash", "exit-zero-early"):
                    self.assertEqual(result["exit_code"], 23 if mode == "crash" else 0)
                else:
                    self.assertTrue(result["forced_termination"])

    def test_settled_receiver_loss_and_missing_ack_fail(self) -> None:
        for mode, reason in (("load-disappears", "settled_load_or_policy_lost"),
                             ("media-freeze", "media_progress_timeout"),
                             ("wrong-command-noack", "command_convergence_timeout")):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="soak-progress-") as directory:
                result = self.run_fixture(Path(directory) / "run", mode)
                self.assertEqual(result["status"], "FAIL")
                self.assertEqual(result["reason"], reason)
                self.assertTrue(result["forced_termination"])

    def test_media_counter_reset_allows_real_progress_to_resume(self) -> None:
        with tempfile.TemporaryDirectory(prefix="soak-reset-") as directory:
            output = Path(directory) / "run"
            result = self.run_fixture(output, "counters-reset-then-progress")
            self.assertEqual(result["status"], "PASS")
            events = [json.loads(line) for line in (output / "events.jsonl").read_text(encoding="utf-8").splitlines()]
            self.assertIn("media_counters_rebased", {event["event"] for event in events})
            started = next(event for event in events if event["event"] == "steady_clock_started")
            converged = next(event for event in events if event["event"] == "command_converged")
            self.assertGreaterEqual(started["elapsed_s"], converged["elapsed_s"])
            self.assertGreaterEqual(result["measured_seconds"], result["duration_seconds"])


if __name__ == "__main__":
    unittest.main()
