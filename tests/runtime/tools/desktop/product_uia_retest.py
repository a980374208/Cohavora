"""Local UIA lifecycle retest supervisor. No remote deployment or media verdict.

Uses the existing product driver and checkpoint collector. A separate process
monitors driver progress, product identity, WM_NULL response and Private Bytes.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time
import uuid

# Explicit sibling-domain dependency; keep direct script execution supported.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "meeting"))

from meeting_soak import (ProcessMetrics, archive_evidence, atomic_json,
                          desktop_available, memory_trend, sha256, utc_now)

ROOT = Path(__file__).resolve().parents[4]


class Failure(Exception):
    pass


class JsonlTail:
    """Incremental reader; an unfinished final row is never accepted as progress."""
    def __init__(self, path):
        self.path, self.offset, self.last = path, 0, None

    def read(self):
        self.rows = []
        if not self.path.exists():
            return self.last
        if self.path.stat().st_size < self.offset:
            raise Failure("EVIDENCE_TRUNCATED")
        with self.path.open("rb") as stream:
            stream.seek(self.offset)
            while True:
                line = stream.readline(65537)
                if len(line) > 65536:
                    raise Failure("EVIDENCE_ROW_TOO_LARGE")
                if not line or not line.endswith(b"\n"):
                    break
                self.last = json.loads(line.decode("utf-8-sig"))
                self.rows.append(self.last)
                self.offset = stream.tell()
        return self.last


class Product(ProcessMetrics):
    """Pinned kernel handle; termination never reopens a possibly reused PID."""
    def __init__(self, identity, executable):
        super().__init__(identity["pid"])
        from ctypes import wintypes as w
        self.kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
        self.kernel.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
        self.kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
        self.kernel.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
        self.close()
        self.handle = self.kernel.OpenProcess(0x100000 | 0x0400 | 0x0010 | 0x0001, False, self.pid)
        if not self.handle:
            raise Failure("PRODUCT_HANDLE_UNAVAILABLE")
        try:
            times = [w.FILETIME() for _ in range(4)]
            if not self.kernel.GetProcessTimes(self.handle, *(ctypes.byref(v) for v in times)):
                raise Failure("PRODUCT_CREATION_TIME_UNAVAILABLE")
            ticks = (times[0].dwHighDateTime << 32) + times[0].dwLowDateTime + 504911232000000000
            name, size = ctypes.create_unicode_buffer(32768), w.DWORD(32768)
            if not self.kernel.QueryFullProcessImageNameW(self.handle, 0, name, ctypes.byref(size)):
                raise Failure("PRODUCT_IMAGE_UNAVAILABLE")
            if ticks != identity["start_ticks"] or Path(name.value).resolve() != executable.resolve():
                raise Failure("PRODUCT_IDENTITY_MISMATCH")
        except BaseException:
            self.close()
            raise
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        self.user.EnumWindows.argtypes = [self.callback, w.LPARAM]
        self.user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        self.user.IsWindowVisible.argtypes = [w.HWND]
        self.user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM,
                                                  w.UINT, w.UINT, ctypes.POINTER(ctypes.c_size_t)]
        self.user.SendMessageTimeoutW.restype = w.LPARAM

    def alive(self):
        return self.kernel.WaitForSingleObject(self.handle, 0) == 258

    def responsive(self):
        from ctypes import wintypes as w
        windows = []
        @self.callback
        def visit(window, unused):
            process = w.DWORD()
            self.user.GetWindowThreadProcessId(window, ctypes.byref(process))
            if process.value == self.pid and self.user.IsWindowVisible(window):
                windows.append(window)
            return True
        self.user.EnumWindows(visit, 0)
        if not windows:
            return None
        # Each WM_NULL is bounded; stop on the first nonresponsive window.
        for window in windows[:16]:
            result = ctypes.c_size_t()
            if not self.user.SendMessageTimeoutW(window, 0, 0, 0, 0x22, 500, ctypes.byref(result)):
                return False
        return True

    def stop(self):
        if self.alive():
            self.kernel.TerminateProcess(self.handle, 1)
            self.kernel.WaitForSingleObject(self.handle, 5000)


def profile(mode, steady=None, mixed=None, cycles=None):
    short = mode == "smoke"
    result = dict(mode=mode, steady_seconds=steady if steady is not None else (10 if short else 1800),
                  mixed_seconds=mixed if mixed is not None else (40 if short else 7200),
                  mixed_cycles=cycles if cycles is not None else (2 if short else 12),
                  operation_timeout=180, startup_timeout=120, hang_timeout=15,
                  probe_timeout=15, maximum_sample_gap=10, minimum_free_bytes=1024**3,
                  private_growth_mib=64, private_slope_mib_hour=64)
    if result["mixed_cycles"] < 2 or result["steady_seconds"] < 1 or result["mixed_seconds"] < 2:
        raise ValueError("invalid_retest_plan")
    if mode == "retest" and (result["steady_seconds"] < 1800 or result["mixed_seconds"] < 7200):
        raise ValueError("retest_duration_too_short")
    result["maximum_seconds"] = (240 if mode == "probe" else
        result["steady_seconds"] + result["mixed_seconds"] + 360 * (result["mixed_cycles"] + 1))
    return result


def supervise(output, executable, settings, *, command=None, product_factory=Product,
              desktop_check=desktop_available, poll_seconds=1):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    run_id = uuid.uuid4().hex
    started = time.monotonic()
    summary = dict(run_id=run_id, status="RUNNING", l3_status="NOT_RUN",
                   reason="incomplete", mode=settings["mode"], media_acceptance="NOT_RUN")
    atomic_json(output / "summary.json", summary)
    atomic_json(output / "profile.json", settings)
    atomic_json(output / "plan.json", dict(steady_seconds=settings["steady_seconds"],
        mixed_seconds=settings["mixed_seconds"], mixed_cycles=settings["mixed_cycles"],
        mixed_actions=["join", "page", "share_start", "share_stop", "logging", "leave", "export"],
        excluded=["SDK reconnect injection", "network faults", "remote RTP/audio quality"]))
    inputs = [Path(__file__), ROOT / "tests/uia/product_desktop.ps1",
              ROOT / "tests/runtime/tools/meeting/meeting_soak.py", ROOT / "tests/runtime/tools/product_acceptance/product_pilot_checkpoints.py"]
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True,
                          text=True, check=True, timeout=10).stdout.strip()
    atomic_json(output / "run.json", dict(run_id=run_id, started_utc=utc_now(), head=head,
        inputs={str(p.relative_to(ROOT)): sha256(p) for p in inputs},
        executable_sha256=sha256(executable), binary_source_provenance="NOT_VERIFIED"))
    env = os.environ.copy()
    env.update(LIVEKIT_UIA_RUN_ID=run_id, LIVEKIT_UIA_DEDICATED_DESKTOP="1",
               LIVEKIT_UIA_PILOT_PROBE=str(output / "process-probe.jsonl"))
    # This local retest must never invoke the historical remote context uploader.
    env.pop("LIVEKIT_UIA_REMOTE_CONTEXT", None)
    env.pop("LIVEKIT_UIA_LOG_PAIR", None)
    custom_command = command is not None
    if command is None:
        command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                   str(ROOT / "tests/uia/product_desktop.ps1"), "-Executable", str(executable),
                   "-OutputDirectory", str(output / "uia"), "-RunId", run_id]
        if settings["mode"] == "probe":
            command += ["-ProbeOnly"]
        else:
            command += ["-Retest", "-Cycles", str(settings["mixed_cycles"] + 1),
                        "-SteadySeconds", str(settings["steady_seconds"]),
                        "-MixedSeconds", str(settings["mixed_seconds"])]
            if settings["mode"] == "smoke":
                command += ["-RetestSmoke", "-ShareSeconds", "3", "-LogPairSeconds", "2",
                            "-StopSettleSeconds", "2", "-RoomSettleSeconds", "2"]
    env["UIA_RETEST_TEST_OUTPUT"] = str(output)  # also used by offline fault fixtures
    driver = product = collector = None
    action_tail = JsonlTail(output / "uia/uia-actions.jsonl")
    probe_tail = JsonlTail(output / "process-probe.jsonl")
    samples, checkpoints = [], {}
    phase_starts, phase_durations = {}, {}
    process_run, probe_sequence = None, -1
    last_action_offset, last_progress = 0, started
    last_probe_offset, last_probe = 0, started
    unresponsive = no_window = None
    previous_tick = started
    event_stream = (output / "events.jsonl").open("x", encoding="utf-8", buffering=1)
    metrics = (output / "metrics.csv").open("x", encoding="utf-8", buffering=1)
    metrics.write("elapsed_s,phase,cycle,private_bytes,working_set_bytes,handles,responsive\n")
    def event(name, **values):
        event_stream.write(json.dumps(dict(utc=utc_now(), elapsed_s=time.monotonic()-started,
                                          event=name, **values)) + "\n")
    try:
        if not desktop_check():
            raise Failure("DESKTOP_UNAVAILABLE")
        if settings["mode"] != "probe" and not custom_command:
            if not all(env.get(k) for k in ("LIVEKIT_UIA_ACCOUNT", "LIVEKIT_UIA_PASSWORD", "LIVEKIT_UIA_MEETING_ID")):
                raise Failure("DEDICATED_ACCOUNT_AND_MEETING_REQUIRED")
        driver = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, creationflags=0x08000000 if os.name == "nt" else 0)
        event("driver.started", pid=driver.pid)
        while True:
            now = time.monotonic()
            if now - previous_tick > settings["maximum_sample_gap"]:
                raise Failure("SUPERVISOR_SAMPLE_GAP")
            previous_tick = now
            if now - started > settings["maximum_seconds"]:
                raise Failure("RUN_WATCHDOG_TIMEOUT")
            if not desktop_check():
                raise Failure("DESKTOP_UNAVAILABLE")
            if shutil.disk_usage(output).free < settings["minimum_free_bytes"]:
                raise Failure("EVIDENCE_DISK_RESERVE_EXHAUSTED")
            identity_path = output / "uia/product-identity.json"
            if product is None and identity_path.exists():
                try:
                    identity = json.loads(identity_path.read_text(encoding="utf-8-sig"))
                except json.JSONDecodeError:
                    identity = None  # first write may still be in progress
                if identity:
                    if identity["run_id"] != run_id:
                        raise Failure("PRODUCT_RUN_IDENTITY_MISMATCH")
                    product = product_factory(identity, executable)
                    last_probe = now
                    event("product.attached", pid=identity["pid"])
                    if settings["mode"] != "probe":
                        collector = subprocess.Popen([sys.executable,
                            str(ROOT / "tests/runtime/tools/product_acceptance/product_pilot_checkpoints.py"),
                            "--probe", str(output / "process-probe.jsonl"),
                            "--result", str(output / "uia/uia-result.json"),
                            "--output", str(output / "checkpoint-archive"), "--run-id", run_id,
                            "--seconds", str(int(settings["maximum_seconds"])),
                            "--maximum-bytes", str(8 * 1024**3)], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, creationflags=0x08000000 if os.name == "nt" else 0)
            action = action_tail.read()
            for row in action_tail.rows:
                if row.get("run_id") != run_id or (product and row.get("pid") != product.pid):
                    raise Failure("ACTION_IDENTITY_MISMATCH")
                event("uia.action", action=row.get("action"), phase=row.get("phase"),
                      cycle=row.get("cycle"), driver_elapsed_s=row.get("elapsed_seconds"))
                if row.get("action") in ("retest_steady", "retest_mixed"):
                    key = (row["action"], row["cycle"])
                    if row["phase"] == "started":
                        if key in phase_starts:
                            raise Failure("DUPLICATE_PHASE_START")
                        phase_starts[key] = row["elapsed_seconds"]
                    if row["phase"] == "completed":
                        if key not in phase_starts or key in phase_durations:
                            raise Failure("PHASE_COMPLETION_MISMATCH")
                        phase_durations[key] = row["elapsed_seconds"] - phase_starts[key]
            if action:
                if action.get("run_id") != run_id or (product and action.get("pid") != product.pid):
                    raise Failure("ACTION_IDENTITY_MISMATCH")
                if action_tail.offset != last_action_offset:
                    last_progress, last_action_offset = now, action_tail.offset
            if collector and collector.poll() is not None and not (output / "uia/uia-result.json").exists():
                raise Failure("CHECKPOINT_COLLECTOR_EXITED_EARLY")
            result_path = output / "uia/uia-result.json"
            if driver.poll() is not None:
                if not result_path.exists():
                    raise Failure("DRIVER_EXIT_WITHOUT_RESULT")
                result = json.loads(result_path.read_text(encoding="utf-8-sig"))
                expected = "PROBED" if settings["mode"] == "probe" else "RETEST_COMPLETE"
                if result.get("run_id") != run_id or driver.returncode != 0 or result.get("verdict") != expected:
                    raise Failure("DRIVER_FAILED")
                if product is None:
                    raise Failure("PRODUCT_NOT_OBSERVED")
                if settings["mode"] != "probe":
                    expected_phases = {("retest_steady", 1): settings["steady_seconds"]}
                    expected_phases.update({("retest_mixed", i): settings["mixed_seconds"] / settings["mixed_cycles"]
                                           for i in range(2, settings["mixed_cycles"] + 2)})
                    if set(phase_durations) != set(expected_phases) or any(
                            phase_durations[k] < minimum for k, minimum in expected_phases.items()):
                        raise Failure("PHASE_DURATION_INCOMPLETE")
                    if result.get("cycles_completed") != settings["mixed_cycles"] + 1:
                        raise Failure("INCOMPLETE_CYCLES")
                    if not (output / "uia/process-exit.json").exists():
                        raise Failure("PRODUCT_EXIT_EVIDENCE_MISSING")
                    exit_result = json.loads((output / "uia/process-exit.json").read_text(encoding="utf-8-sig"))
                    if exit_result.get("run_id") != run_id or exit_result.get("pid") != product.pid or exit_result.get("exit_code") != 0:
                        raise Failure("PRODUCT_EXIT_FAILED")
                break
            if not product:
                if now - started > settings["startup_timeout"]:
                    raise Failure("PRODUCT_STARTUP_TIMEOUT")
            else:
                closing = bool(action and action.get("action") == "process_exit")
                if not product.alive() and not closing:
                    # ProbeOnly intentionally kills its own guest process in finally.
                    if settings["mode"] != "probe" or not result_path.exists():
                        raise Failure("PRODUCT_UNEXPECTED_EXIT")
                if product.alive():
                    responsive = product.responsive()
                    unresponsive = (unresponsive or now) if responsive is False else None
                    no_window = (no_window or now) if responsive is None else None
                    if unresponsive and now - unresponsive > settings["hang_timeout"]:
                        raise Failure("PRODUCT_UI_HANG")
                    if no_window and now - no_window > settings["startup_timeout"] and not closing:
                        raise Failure("PRODUCT_WINDOW_MISSING")
                    sample = product.sample()
                    if sample["private_bytes"] is None:
                        raise Failure("MEMORY_COUNTER_UNAVAILABLE")
                    phase = action.get("action", "startup") if action else "startup"
                    cycle = action.get("cycle", 0) if action else 0
                    sample.update(elapsed_s=now-started, phase=phase, cycle=cycle)
                    samples.append(sample)
                    metrics.write(f'{now-started:.3f},{phase},{cycle},{sample["private_bytes"]},'
                                  f'{sample["working_set_bytes"]},{sample["handles"]},{responsive}\n')
                    if action and phase == "leave" and action.get("phase") == "uia_observed":
                        checkpoints.setdefault(cycle, []).append(sample)
                if settings["mode"] != "probe" and not closing:
                    probe = probe_tail.read()
                    if probe:
                        if probe.get("run_id") != run_id or not re.fullmatch("[0-9a-f]{32}", probe.get("process_run_id", "")):
                            raise Failure("PROBE_IDENTITY_MISMATCH")
                        if probe_tail.offset != last_probe_offset:
                            if process_run and process_run != probe["process_run_id"]:
                                raise Failure("PROBE_PROCESS_CHANGED")
                            if probe.get("sequence", -1) <= probe_sequence:
                                raise Failure("PROBE_SEQUENCE_STALLED")
                            process_run, probe_sequence = probe["process_run_id"], probe["sequence"]
                            last_probe, last_probe_offset = now, probe_tail.offset
                        for section, fields in {"history": ("queue_drops", "pending_records_dropped", "write_failures"),
                            "diagnostic": ("dropped_ordinary", "dropped_critical", "sink_failures")}.items():
                            if any(probe.get(section, {}).get(f, 0) for f in fields):
                                raise Failure("PRODUCT_EVIDENCE_LOSS")
                    if now-last_probe > settings["probe_timeout"]:
                        raise Failure("PROCESS_PROBE_STALE")
            if now-last_progress > settings["operation_timeout"]:
                raise Failure("UIA_OPERATION_WATCHDOG_TIMEOUT")
            time.sleep(poll_seconds)
        if collector and collector.wait(timeout=15) != 0:
            raise Failure("CHECKPOINT_ARCHIVE_FAILED")
        summary.update(status="PASS", reason="declared_ui_scope_complete")
    except (Exception, KeyboardInterrupt) as error:
        summary.update(status="FAIL", reason=str(error) if isinstance(error, Failure) else type(error).__name__)
        event("failure", reason=summary["reason"])
    finally:
        atomic_json(output / "collector-stop.json", dict(run_id=run_id, status=summary["status"]))
        # Stop the driver before the product to prevent late UIA activity.
        if driver and driver.poll() is None:
            driver.kill()
            driver.wait(timeout=10)
        if product:
            product.stop()
            product.close()
        if collector and collector.poll() is None:
            try:
                collector.wait(timeout=10)
            except subprocess.TimeoutExpired:
                collector.kill()
                collector.wait(timeout=5)
                summary.update(status="FAIL", reason="COLLECTOR_SHUTDOWN_TIMEOUT")
        metrics.close()
        event_stream.close()
    steady_samples = [s for s in samples if s["phase"] == "retest_steady"]
    if steady_samples:
        origin = steady_samples[0]["elapsed_s"]
        steady_samples = [dict(s, elapsed_s=s["elapsed_s"]-origin) for s in steady_samples]
    comparable = [dict(elapsed_s=statistics.median(s["elapsed_s"] for s in group),
                       private_bytes=statistics.median(s["private_bytes"] for s in group))
                  for cycle, group in sorted(checkpoints.items()) if cycle > 1]
    summary["memory"] = dict(steady=memory_trend(steady_samples), mixed=memory_trend(comparable, 0))
    summary["phase_durations"] = [dict(phase=k[0], cycle=k[1], seconds=v) for k, v in phase_durations.items()]
    summary["memory"]["steady"]["coverage"] = len(steady_samples) * poll_seconds / settings["steady_seconds"]
    summary["memory"]["mixed"]["checkpoints"] = len(comparable)
    if settings["mode"] == "retest" and summary["status"] == "PASS":
        if summary["memory"]["steady"]["coverage"] < .95 or len(comparable) != settings["mixed_cycles"]:
            summary.update(status="FAIL", reason="MEMORY_COVERAGE_INCOMPLETE")
        for trend in summary["memory"].values():
            if trend["status"] != "VALID":
                summary.update(status="FAIL", reason="INSUFFICIENT_MEMORY_DATA")
            elif trend["growth_mib"] > settings["private_growth_mib"] or trend["slope_mib_per_hour"] > settings["private_slope_mib_hour"]:
                summary.update(status="FAIL", reason="MEMORY_TREND_LIMIT")
    summary.update(elapsed_s=time.monotonic()-started, samples=len(samples),
                   short_test=settings["mode"] != "retest",
                   checkpoint_archive="separate_hash_verified_segments" if collector else "NOT_RUN")
    archive_evidence(output, summary)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=("probe", "smoke", "retest"), default="retest")
    parser.add_argument("--steady-seconds", type=int)
    parser.add_argument("--mixed-seconds", type=int)
    parser.add_argument("--mixed-cycles", type=int)
    args = parser.parse_args()
    settings = profile(args.mode, args.steady_seconds, args.mixed_seconds, args.mixed_cycles)
    result = supervise(args.output, args.executable.resolve(strict=True), settings)
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
