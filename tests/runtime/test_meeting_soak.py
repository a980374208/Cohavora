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
import zipfile


HERE = Path(__file__).resolve().parent
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


class SupervisorTests(unittest.TestCase):
    def run_fixture(self, output: Path, mode: str) -> dict:
        started = time.monotonic()
        result = soak.run_session(
            output=output,
            command=[sys.executable, str(HERE / "soak_fake_peer.py"),
                     "--soak-directory", str(output), "--mode", mode],
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
        self.assertLess(time.monotonic() - started, 3, "Short fixture unexpectedly blocked")
        self.assertEqual(result["l3_status"], "NOT_RUN")
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
