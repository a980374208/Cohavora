"""Opt-in continuous meeting soak supervisor; Python 3.10+, standard library only.

prepare is offline. run owns exactly one persistent Qt/SFU observer process.
The short fake-peer tests validate this supervisor, never L3 media acceptance.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
import uuid
import zipfile
from collections import defaultdict
from datetime import datetime, timezone

SCHEMA = 1
MIB = 1024 * 1024
ROOT = Path(__file__).resolve().parents[2]
ACTIONS = ("grid9", "grid4", "next_page", "grid16", "pin", "unpin",
           "whiteboard_on", "whiteboard_off", "share_start", "share_stop",
           "soft_reconnect", "full_reconnect")
SAFE_STATUS_FIELDS = {
    "schema", "run_id", "pid", "heartbeat_seq", "command_seq", "command_status",
    "state", "runtime_seq", "policy_revision", "requested", "selected", "actual",
    "bound", "selected_not_bound", "bound_not_selected", "remote_video_count",
    "render_submits", "decoded_frames", "audio_frames", "sharing", "error_code",
    "render_backend", "policy_measurement_point",
    "render_expected_bindings", "render_hidden_bindings", "render_router_submitted",
    "render_router_rejected_binding", "render_delivered_to_gpu", "render_attached_tracks",
    "render_timer_active", "video_stage_visible", "canvas_visible", "renderer_ready",
}
METRIC_FIELDS = ("elapsed_s", "phase", "cycle", "action", "settled", "pid",
                 "private_bytes", "working_set_bytes", "handles", "heartbeat_seq",
                 "runtime_seq", "state", "policy_revision", "requested", "selected",
                 "actual", "bound", "selected_not_bound", "bound_not_selected",
                 "remote_video_count", "render_submits", "decoded_frames", "audio_frames",
                 "render_expected_bindings", "render_hidden_bindings", "render_router_submitted",
                 "render_router_rejected_binding", "render_delivered_to_gpu", "render_attached_tracks",
                 "render_timer_active", "video_stage_visible", "canvas_visible", "renderer_ready")


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def atomic_json(path: Path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
    # Windows scanners/readers can briefly omit FILE_SHARE_DELETE.
    deadline = time.monotonic() + .5
    while True:
        try:
            temporary.replace(path)
            break
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.01)


def read_json(path: Path):
    # Qt QSaveFile replacement means readers see complete snapshots only.
    if path.stat().st_size > 65536:
        raise ValueError("oversized_json")
    return json.loads(path.read_text(encoding="utf-8-sig"))


def build_plan(steady_seconds=1800, mixed_seconds=7200):
    if steady_seconds < 1800 or mixed_seconds < 7200:
        raise ValueError("formal_duration_too_short")
    if not all(math.isfinite(x) for x in (steady_seconds, mixed_seconds)):
        raise ValueError("invalid_duration")
    plan = [{"at_s": 0, "action": "grid9", "phase": "steady", "cycle": 0}]
    # 10 minute cycles. Every cycle returns to grid9 for comparable memory samples.
    offset = 0
    while offset < mixed_seconds:
        plan.append({"at_s": steady_seconds + offset,
                     "action": ACTIONS[(offset // 50) % len(ACTIONS)],
                     "phase": "mixed", "cycle": offset // 600 + 1})
        offset += 50
    return plan


def memory_trend(samples, warmup_seconds=300):
    buckets = defaultdict(list)
    valid = []
    for sample in samples:
        elapsed, value = sample.get("elapsed_s"), sample.get("private_bytes")
        if not isinstance(elapsed, (int, float)) or not isinstance(value, (int, float)):
            continue
        if not math.isfinite(elapsed) or not math.isfinite(value) or value < 0 or elapsed < warmup_seconds:
            continue
        buckets[int(elapsed // 60)].append((elapsed, value / MIB))
        valid.append(sample)
    points = [(statistics.median(p[0] for p in group),
               statistics.median(p[1] for p in group)) for _, group in sorted(buckets.items())]
    result = {"status": "INSUFFICIENT_DATA", "sample_count": len(valid),
              "buckets": len(points), "slope_mib_per_hour": None, "growth_mib": None}
    if len(points) < 3 or points[-1][0] - points[0][0] < 120:
        return result
    # Median pair slopes tolerate isolated working/allocator spikes better than endpoints.
    slopes = [(b[1] - a[1]) / (b[0] - a[0]) * 3600
              for i, a in enumerate(points) for b in points[i + 1:] if b[0] > a[0]]
    edge = max(1, len(points) // 5)
    result.update(status="VALID", slope_mib_per_hour=statistics.median(slopes),
                  growth_mib=statistics.median(p[1] for p in points[-edge:]) -
                             statistics.median(p[1] for p in points[:edge]))
    return result


class ProcessMetrics:
    """Read process counters by handle (no process-name matching or external tools)."""
    def __init__(self, pid):
        self.pid = pid
        self.handle = None
        if os.name == "nt":
            from ctypes import wintypes as w
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
            self.kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
            self.kernel.OpenProcess.restype = w.HANDLE
            self.kernel.CloseHandle.argtypes = [w.HANDLE]
            self.kernel.GetProcessHandleCount.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
            self.handle = self.kernel.OpenProcess(0x0400 | 0x0010, False, pid)
            class Counters(ctypes.Structure):
                _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD)] + [
                    (name, ctypes.c_size_t) for name in (
                        "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                        "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                        "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]
            self.counters = Counters
            self.psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.POINTER(Counters), w.DWORD]

    def sample(self):
        empty = {"private_bytes": None, "working_set_bytes": None, "handles": None}
        if self.handle:
            from ctypes import wintypes as w
            counters = self.counters()
            counters.cb = ctypes.sizeof(counters)
            handles = w.DWORD()
            if self.psapi.GetProcessMemoryInfo(self.handle, ctypes.byref(counters), counters.cb):
                empty.update(private_bytes=int(counters.PrivateUsage),
                             working_set_bytes=int(counters.WorkingSetSize))
            if self.kernel.GetProcessHandleCount(self.handle, ctypes.byref(handles)):
                empty["handles"] = handles.value
        return empty

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def desktop_available():
    if os.name != "nt":
        return False
    from ctypes import wintypes as w
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.OpenInputDesktop.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    user.OpenInputDesktop.restype = w.HANDLE
    user.GetUserObjectInformationW.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD,
                                              ctypes.POINTER(w.DWORD)]
    user.CloseDesktop.argtypes = [w.HANDLE]
    desktop = user.OpenInputDesktop(0, False, 0x0001)
    if not desktop:
        return False
    try:
        name, size = ctypes.create_unicode_buffer(256), w.DWORD()
        return bool(user.GetUserObjectInformationW(desktop, 2, name, ctypes.sizeof(name),
                                                   ctypes.byref(size))) and name.value.casefold() == "default"
    finally:
        user.CloseDesktop(desktop)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_fingerprint():
    paths = ("tests/runtime/meeting_soak.py", "tests/runtime/meeting_soak_adapter.h",
             "tests/remediation/test_participant_snapshot_remediation.cpp",
             "src/core/meeting_coordinator.cpp", "src/core/meeting_session_runtime.h",
             "src/core/video_demand_policy.cpp", "src/core/room.cpp",
             "src/telemetry/session_telemetry.h",
             "src/telemetry/stats.h", "src/telemetry/stats_collector.cpp",
             "src/ui/meeting_room_window.cpp", "src/render/video_render_session.cpp")
    inputs = {name: sha256(ROOT / name) for name in paths if (ROOT / name).is_file()}
    try:
        result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True,
                                text=True, timeout=5, check=True)
        head = result.stdout.strip()
        if len(head) != 40 or any(c not in "0123456789abcdef" for c in head):
            head = "UNKNOWN"
    except (OSError, subprocess.SubprocessError):
        head = "UNKNOWN"
    return {"head": head, "sha256": inputs}


def archive_evidence(output, summary):
    atomic_json(output / "summary.json", summary)
    # Explicit allowlist: never copy tokens, arbitrary adapter logs, environment or crash dumps.
    names = ("run.json", "plan.json", "profile.json", "events.jsonl", "metrics.csv",
             "tracks.jsonl",
             "summary.json", "last-status.json")
    files = [{"name": name, "sha256": sha256(output / name), "size": (output / name).stat().st_size}
             for name in names if (output / name).is_file()]
    manifest = {"schema": SCHEMA, "run_id": summary["run_id"], "created_utc": utc_now(),
                "status": summary["status"], "files": files}
    atomic_json(output / "manifest.json", manifest)
    archive = output.with_suffix(".zip")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as bundle:
        for item in files:
            bundle.write(output / item["name"], item["name"])
        bundle.write(output / "manifest.json", "manifest.json")
    archive.with_suffix(".zip.sha256").write_text(sha256(archive) + "\n", encoding="ascii")


class StopRun(Exception):
    def __init__(self, reason, status="FAIL"):
        self.reason, self.status = reason, status


def validate_status(value, run_id, pid):
    if not isinstance(value, dict) or value.get("schema") != SCHEMA:
        raise StopRun("invalid_status")
    if value.get("run_id") != run_id or value.get("pid") != pid:
        raise StopRun("status_identity_mismatch")
    for name in ("heartbeat_seq", "runtime_seq", "command_seq", "policy_revision", "requested",
                 "selected", "actual", "bound", "selected_not_bound", "bound_not_selected",
                 "remote_video_count", "render_submits", "decoded_frames"):
        if type(value.get(name)) is not int or not 0 <= value[name] <= 2**53:
            raise StopRun("invalid_status_counter")
    for name in ("render_expected_bindings", "render_hidden_bindings", "render_router_submitted",
                 "render_router_rejected_binding", "render_delivered_to_gpu", "render_attached_tracks"):
        if name in value and (type(value[name]) is not int or not 0 <= value[name] <= 2**53):
            raise StopRun("invalid_status_counter")
    for name in ("render_timer_active", "video_stage_visible", "canvas_visible", "renderer_ready"):
        if name in value and type(value[name]) is not bool:
            raise StopRun("invalid_status_state")
    if value.get("state") not in ("connecting", "connected", "reconnecting", "stopping", "stopped", "failed"):
        raise StopRun("invalid_status_state")
    if value.get("command_status") not in ("pending", "applied", "rejected"):
        raise StopRun("invalid_command_status")
    # Archive only fixed fields; don't trust arbitrary strings returned by an adapter.
    safe = {key: value[key] for key in SAFE_STATUS_FIELDS if key in value}
    safe["error_code"] = "adapter_reported_error" if value.get("error_code") else ""
    safe["sharing"] = value.get("sharing") is True
    safe["render_backend"] = value.get("render_backend") if value.get("render_backend") in (
        "cpu", "qt_cpu", "opengl", "dx11", "released", "unknown") else "unknown"
    safe["policy_measurement_point"] = value.get("policy_measurement_point") if value.get(
        "policy_measurement_point") in ("session_video_policy_convergence", "shutdown_quiescence") else "unknown"
    audio = value.get("audio_frames")
    safe["audio_frames"] = audio if type(audio) is int and audio >= 0 else None
    return safe


def run_session(output: Path, command: list[str], plan: list[dict], duration_seconds: float,
                heartbeat_timeout=10, runtime_timeout=15, command_timeout=40,
                sample_interval=1, startup_timeout=90, shutdown_timeout=30,
                min_remote_videos=17, memory_warmup=300,
                max_growth_mib=None, max_slope_mib_per_hour=None, self_test=False):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "run.json").exists() or output.with_suffix(".zip").exists():
        raise ValueError("run_directory_already_used")
    if not self_test:
        # Library calls cannot bypass the CLI's minimum duration/canonical plan gate.
        if duration_seconds < 9000 or plan != build_plan(1800, duration_seconds - 1800):
            raise ValueError("formal_plan_required")
    run_id = uuid.uuid4().hex
    metadata = {"schema": SCHEMA, "run_id": run_id, "started_utc": utc_now(),
                "self_test": self_test, "duration_seconds": duration_seconds,
                "platform": platform.system(), "os_release": platform.release(),
                "architecture": platform.machine(), "logical_processors": os.cpu_count(),
                "adapter_sha256": sha256(Path(command[0])),
                "source_inputs": source_fingerprint()}
    atomic_json(output / "run.json", metadata)
    atomic_json(output / "plan.json", plan)
    atomic_json(output / "manifest.json", {"schema": SCHEMA, "run_id": run_id, "status": "RUNNING"})
    atomic_json(output / "profile.json", {"min_remote_videos": min_remote_videos,
        "heartbeat_timeout": heartbeat_timeout, "runtime_timeout": runtime_timeout,
        "command_timeout": command_timeout, "sample_interval": sample_interval,
        "startup_timeout": startup_timeout, "shutdown_timeout": shutdown_timeout,
        "memory_warmup": memory_warmup, "max_growth_mib": max_growth_mib,
        "max_slope_mib_per_hour": max_slope_mib_per_hour})
    summary = {"schema": SCHEMA, "run_id": run_id, "status": "INCONCLUSIVE",
               "reason": "supervisor_interrupted", "l3_status": "NOT_RUN" if self_test else "INCONCLUSIVE",
               "scheduled_commands": len(plan), "completed_commands": 0,
               "duration_seconds": duration_seconds, "measured_seconds": 0,
               "exit_code": None, "forced_termination": False, "memory": {}}
    samples, process, metrics = [], None, None
    start, measured_start = time.monotonic(), None
    pending, active = None, {"phase": "startup", "cycle": 0, "action": "connect"}
    next_action, seq, ack_at = 0, 0, None
    status = None
    hb, runtime, hb_at, runtime_at = -1, -1, start, start
    media_value, render_at, decoded_at = None, start, start
    ack_media, unhealthy_at = None, None
    stopping_at, stopped_ack = None, False

    with (output / "events.jsonl").open("w", encoding="utf-8") as events, \
         (output / "metrics.csv").open("w", encoding="utf-8", newline="") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=METRIC_FIELDS)
        writer.writeheader()
        def event(kind, **values):
            events.write(json.dumps({"elapsed_s": round(time.monotonic() - start, 3),
                                    "event": kind, **values}, allow_nan=False) + "\n")
            events.flush()
        def send(action, now):
            nonlocal seq, pending, ack_at, ack_media
            seq += 1
            pending, ack_at = {"seq": seq, "action": action, "sent": now}, None
            ack_media = None
            atomic_json(output / "command.json", {"schema": SCHEMA, "run_id": run_id,
                                                  "seq": seq, "action": action})
            event("command_sent", seq=seq, action=action)
        try:
            if not self_test and not desktop_available():
                raise StopRun("interactive_desktop_unavailable", "NOT_RUN")
            env = os.environ.copy()
            env["COHAVORA_SOAK_RUN_ID"] = run_id
            # Never persist stdout/stderr: SDK/application diagnostics are outside this protocol.
            process = subprocess.Popen(command, env=env, cwd=ROOT, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            metrics = ProcessMetrics(process.pid)
            event("process_started", pid=process.pid)
            last_poll = time.monotonic()
            while True:
                now = time.monotonic()
                if now - last_poll > max(5, sample_interval * 5):
                    raise StopRun("system_pause_or_supervisor_stall", "INCONCLUSIVE")
                last_poll = now
                if not self_test and not desktop_available():
                    raise StopRun("desktop_locked_or_unavailable", "INCONCLUSIVE")
                if (output / "status.json").exists():
                    try:
                        latest = validate_status(read_json(output / "status.json"), run_id, process.pid)
                    except (OSError, ValueError):
                        latest = None  # Atomic rename may race a Windows file scanner; timeout still applies.
                    if latest is not None:
                        status = latest
                        if status["heartbeat_seq"] < hb or status["runtime_seq"] < runtime:
                            raise StopRun("heartbeat_regressed")
                        if status["heartbeat_seq"] > hb:
                            hb, hb_at = status["heartbeat_seq"], now
                        if status["runtime_seq"] > runtime:
                            runtime, runtime_at = status["runtime_seq"], now
                        if status["state"] == "failed":
                            raise StopRun("adapter_failed")
                        if status["selected"] > 16 or status["bound"] > 16 or status["bound_not_selected"]:
                            raise StopRun("video_budget_or_binding_violation")
                        if status["command_seq"] > seq:
                            raise StopRun("unexpected_command_ack")
                        if pending and status["command_seq"] == pending["seq"]:
                            if status["command_status"] == "rejected":
                                raise StopRun("command_rejected")
                            if status["command_status"] == "applied" and ack_at is None:
                                ack_at = now
                                ack_media = (status["render_submits"], status["decoded_frames"])
                        if stopping_at is not None:
                            stopped_ack = (status["command_seq"] == seq and
                                status["command_status"] == "applied" and status["state"] == "stopped" and
                                all(status[k] == 0 for k in ("requested", "selected", "actual", "bound")))
                exit_code = process.poll()
                if exit_code is not None:
                    summary["exit_code"] = exit_code
                    if stopped_ack and exit_code == 0:
                        summary.update(status="PASS", reason="schedule_completed_and_shutdown_confirmed")
                        break
                    raise StopRun("process_exited_early" if stopping_at is None else "shutdown_not_confirmed")
                if stopping_at is not None:
                    if now - stopping_at > shutdown_timeout:
                        raise StopRun("shutdown_timeout")
                    time.sleep(min(sample_interval, .1))
                    continue
                if measured_start is None:
                    # Timers start only when a real nonempty receiver + runtime pulse is ready.
                    if (status and status["state"] == "connected" and runtime > 0 and
                            status["remote_video_count"] >= min_remote_videos):
                        measured_start, hb_at, runtime_at = now, now, now
                        event("load_ready", remote_video_count=status["remote_video_count"])
                    elif now - start > startup_timeout:
                        raise StopRun("startup_or_load_timeout")
                else:
                    if now - hb_at > heartbeat_timeout:
                        raise StopRun("ui_heartbeat_timeout")
                    if now - runtime_at > runtime_timeout:
                        raise StopRun("runtime_heartbeat_timeout")
                    if pending and now - pending["sent"] > command_timeout:
                        raise StopRun("command_convergence_timeout")
                    if status and ack_at is not None and pending:
                        action = pending["action"]
                        counters = (status["render_submits"], status["decoded_frames"])
                        if any(value < base for value, base in zip(counters, ack_media)):
                            event("media_counters_rebased", action=action,
                                  policy_revision=status["policy_revision"])
                            ack_media = counters
                        converged = status["state"] == "connected" and status["selected_not_bound"] == 0
                        if action == "whiteboard_on":
                            converged = converged and status["selected"] == status["bound"] == 0
                        else:
                            converged = converged and status["selected"] > 0 and status["bound"] > 0
                            converged = converged and (status["render_submits"] > ack_media[0] and
                                                       status["decoded_frames"] > ack_media[1])
                        if action == "share_start":
                            converged = converged and status.get("sharing") is True
                        if action == "share_stop":
                            converged = converged and status.get("sharing") is False
                        if converged:
                            event("command_converged", seq=pending["seq"], action=action,
                                  policy_revision=status["policy_revision"])
                            summary["completed_commands"] += 1
                            if summary["completed_commands"] == 1:
                                # The full 30 minutes starts after initial grid9 convergence.
                                measured_start = now
                                event("steady_clock_started")
                            pending = None
                            media_value = (status["render_submits"], status["decoded_frames"])
                            render_at = decoded_at = now
                    if status and pending is None:
                        healthy = (status["state"] == "connected" and
                                   status["remote_video_count"] >= min_remote_videos and
                                   status["selected_not_bound"] == 0 and
                                   status["selected"] == status["bound"] and
                                   (status["selected"] > 0 if active["action"] != "whiteboard_on" else
                                    status["selected"] == 0))
                        if healthy:
                            unhealthy_at = None
                        elif unhealthy_at is None:
                            unhealthy_at = now
                        elif now - unhealthy_at > runtime_timeout:
                            raise StopRun("settled_load_or_policy_lost")
                    if status and pending is None and status["selected"] > 0:
                        current = (status["render_submits"], status["decoded_frames"])
                        if media_value is not None:
                            if current[0] > media_value[0]:
                                render_at = now
                            if current[1] > media_value[1]:
                                decoded_at = now
                            if any(value < old for value, old in zip(current, media_value)):
                                # Current binding/stream sums can shrink. A reset itself is not progress.
                                event("media_counters_rebased", action=active["action"],
                                      policy_revision=status["policy_revision"])
                        media_value = current
                        if now - min(render_at, decoded_at) > max(runtime_timeout, 20 if not self_test else runtime_timeout):
                            raise StopRun("media_progress_timeout")
                    elapsed = now - measured_start
                    summary["measured_seconds"] = round(elapsed, 3)
                    if elapsed >= duration_seconds and pending is None:
                        if next_action != len(plan):
                            raise StopRun("schedule_incomplete")
                        if unhealthy_at is not None:
                            raise StopRun("final_state_not_healthy")
                        stopping_at = now
                        send("stop", now)
                        continue
                elapsed = 0 if measured_start is None else now - measured_start
                if (measured_start is not None and pending is None and next_action < len(plan)
                        and elapsed >= plan[next_action]["at_s"]):
                    # Never replay a backlog after suspension or a prolonged command stall.
                    active = plan[next_action]
                    if not self_test and elapsed - active["at_s"] > 45:
                        raise StopRun("schedule_deadline_missed")
                    send(active["action"], now)
                    next_action += 1
                if status:
                    row = {name: status.get(name) for name in METRIC_FIELDS}
                    row.update(metrics.sample(), elapsed_s=round(elapsed, 3), phase=active["phase"],
                               cycle=active["cycle"], action=active["action"],
                               settled=pending is None and measured_start is not None)
                    writer.writerow(row)
                    csvfile.flush()
                    samples.append(row)
                time.sleep(sample_interval)
        except StopRun as failure:
            summary.update(status=failure.status, reason=failure.reason)
            event("run_ended", status=failure.status, reason=failure.reason)
        except KeyboardInterrupt:
            summary.update(status="INCONCLUSIVE", reason="operator_cancelled")
            event("operator_cancelled")
        except Exception as failure:
            # Exception text can contain paths/credentials; retain only a fixed safe classification.
            summary.update(status="INCONCLUSIVE", reason="supervisor_error",
                           error_type=type(failure).__name__)
            event("supervisor_error", error_type=type(failure).__name__)
        finally:
            if process:
                if process.poll() is None:
                    # Only terminate the process started here; never taskkill by image name.
                    summary["forced_termination"] = True
                    try:
                        process.kill()
                        process.wait(timeout=5)
                    except (OSError, subprocess.TimeoutExpired):
                        event("owned_process_cleanup_timeout")
                        summary.update(status="INCONCLUSIVE", reason="owned_process_cleanup_failed")
                summary["exit_code"] = process.poll()
            if metrics:
                metrics.close()
            if status:
                try:
                    atomic_json(output / "last-status.json", status)
                except OSError:
                    summary.update(status="INCONCLUSIVE", reason="evidence_write_failed")
            summary["finished_utc"] = utc_now()
            summary["wall_seconds"] = round(time.monotonic() - start, 3)
    steady = [s for s in samples if s["phase"] == "steady" and s["settled"]]
    # Compare the same layout once each mixed cycle; not 4/16-tile allocation steps.
    mixed = [s for s in samples if s["phase"] == "mixed" and s["action"] == "grid9" and s["settled"]]
    summary["memory"] = {"steady": memory_trend(steady, memory_warmup),
                         "mixed_grid9": memory_trend(mixed, memory_warmup),
                         "private_peak_mib": max((s["private_bytes"] / MIB for s in samples
                             if s.get("private_bytes") is not None), default=None),
                         "gate": "NOT_CONFIGURED"}
    for name, phase_samples in (("steady", steady), ("mixed_grid9", mixed)):
        expected = [s for s in phase_samples if s["elapsed_s"] >= memory_warmup]
        valid = [s for s in expected if s["private_bytes"] is not None]
        coverage = len(valid) / len(expected) if expected else 0
        tail_gap = expected[-1]["elapsed_s"] - valid[-1]["elapsed_s"] if valid else None
        summary["memory"][name].update(sample_coverage=coverage, tail_gap_seconds=tail_gap)
        if coverage < .95 or tail_gap is None or tail_gap > max(sample_interval * 3, 5):
            summary["memory"][name]["status"] = "INSUFFICIENT_DATA"
    summary["schedule_status"] = summary["status"]
    if max_growth_mib is not None and max_slope_mib_per_hour is not None:
        trends = [summary["memory"][name] for name in ("steady", "mixed_grid9")]
        if all(t["status"] == "VALID" for t in trends):
            exceeded = any(t["growth_mib"] > max_growth_mib or
                           t["slope_mib_per_hour"] > max_slope_mib_per_hour for t in trends)
            summary["memory"]["gate"] = "FAIL" if exceeded else "PASS"
            if exceeded and summary["status"] == "PASS":
                summary.update(status="FAIL", reason="memory_growth_exceeded")
        else:
            summary["memory"]["gate"] = "INSUFFICIENT_DATA"
    if not self_test and summary["status"] == "PASS" and summary["memory"]["gate"] != "PASS":
        summary.update(status="INCONCLUSIVE", reason="memory_acceptance_not_established")
    summary["l3_status"] = ("NOT_RUN" if self_test else
        summary["status"] if summary["status"] != "PASS" else
        "PASS" if summary["memory"]["gate"] == "PASS" else "INCONCLUSIVE")
    summary["limitations"] = ["audio_continuity_not_gated", "gpu_memory_not_measured",
                              "shared_media_delivery_not_gated",
                              "actual_is_available_bindings_not_active_rtp_or_decoder_count",
                              "sdk_reconnect_injection_is_not_physical_network_loss"]
    try:
        archive_evidence(output, summary)
    except OSError:
        summary.update(status="INCONCLUSIVE", reason="evidence_archive_failed",
                       l3_status="NOT_RUN" if self_test else "INCONCLUSIVE")
        # Preserve the already written CSV/events even when ZIP/disk operations fail.
        try:
            atomic_json(output / "summary.json", summary)
            atomic_json(output / "manifest.json", {"schema": SCHEMA, "run_id": run_id,
                                                    "status": "INCONCLUSIVE", "reason": "evidence_archive_failed"})
        except OSError:
            pass
    return summary


def prepare(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    mixed = args.mixed_minutes * 60
    plan = build_plan(1800, mixed)
    profile = {"schema": SCHEMA, "steady_seconds": 1800, "mixed_seconds": mixed,
               "executable": str(args.executable.resolve()), "min_remote_videos": args.min_remote_videos,
               "max_growth_mib": args.max_growth_mib, "max_slope_mib_per_hour": args.max_slope_mib_per_hour}
    atomic_json(output / "profile.json", profile)
    atomic_json(output / "plan.json", plan)
    atomic_json(output / "preparation.json", {"schema": SCHEMA, "status": "PREPARED",
        "l3_status": "NOT_RUN", "created_utc": utc_now(), "scheduled_commands": len(plan),
        "duration_seconds": 1800 + mixed, "required_environment_names": ["LIVEKIT_URL", "LIVEKIT_SOAK_TOKEN"],
        "requires": ["unlocked_interactive_windows_desktop", "continuous_remote_publishers",
                     "no_sleep_or_reboot", "user_reserved_test_window"]})
    print("PREPARED; L3 NOT_RUN; duration_seconds=" + str(1800 + mixed))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    prep = commands.add_parser("prepare", help="Write plan/profile without starting any process or meeting")
    prep.add_argument("--output", type=Path, required=True)
    prep.add_argument("--executable", type=Path,
        default=ROOT / "out/build/windows-vs2026-dev/Debug/test_participant_window_remediation.exe")
    prep.add_argument("--mixed-minutes", type=int, default=120)
    prep.add_argument("--min-remote-videos", type=int, default=17)
    prep.add_argument("--max-growth-mib", type=float)
    prep.add_argument("--max-slope-mib-per-hour", type=float)
    run = commands.add_parser("run", help="Start one real continuous meeting using a prepared profile")
    run.add_argument("--prepared", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.mode == "prepare":
            if args.mixed_minutes < 120 or args.min_remote_videos < 17:
                raise ValueError("formal_profile_too_small")
            if (args.max_growth_mib is None) != (args.max_slope_mib_per_hour is None):
                raise ValueError("both_memory_limits_required")
            for value in (args.max_growth_mib, args.max_slope_mib_per_hour):
                if value is not None and (not math.isfinite(value) or value < 0):
                    raise ValueError("invalid_memory_limit")
            return prepare(args)
        prepared = args.prepared.resolve()
        profile, plan = read_json(prepared / "profile.json"), read_json(prepared / "plan.json")
        if (profile["schema"] != SCHEMA or profile["steady_seconds"] != 1800 or
                profile["min_remote_videos"] < 17 or
                plan != build_plan(profile["steady_seconds"], profile["mixed_seconds"])):
            raise ValueError("invalid_formal_profile")
        limits = (profile["max_growth_mib"], profile["max_slope_mib_per_hour"])
        if (limits[0] is None) != (limits[1] is None) or any(
                value is not None and (type(value) not in (float, int) or
                not math.isfinite(value) or value < 0) for value in limits):
            raise ValueError("invalid_memory_limit")
        run_dir = prepared / "runs" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8])
        executable = Path(profile["executable"])
        if not executable.is_file() or any(not os.environ.get(name) for name in ("LIVEKIT_URL", "LIVEKIT_SOAK_TOKEN")):
            run_dir.mkdir(parents=True)
            summary = {"schema": SCHEMA, "run_id": run_dir.name, "status": "NOT_RUN", "l3_status": "NOT_RUN",
                       "reason": "missing_executable_or_service_environment", "created_utc": utc_now()}
            archive_evidence(run_dir, summary)
            print("NOT_RUN: missing executable or service environment; evidence=" + str(run_dir))
            return 2
        result = run_session(run_dir, [str(executable), "--meeting-soak", "--soak-directory", str(run_dir)],
            plan, 1800 + profile["mixed_seconds"], min_remote_videos=profile["min_remote_videos"],
            max_growth_mib=profile["max_growth_mib"], max_slope_mib_per_hour=profile["max_slope_mib_per_hour"])
        print(result["status"] + ": " + result["reason"] + "; L3=" + result["l3_status"] + "; evidence=" + str(run_dir))
        return 0 if result["l3_status"] == "PASS" else 1 if result["status"] == "FAIL" else 2
    except (ValueError, KeyError, TypeError, OSError):
        print("NOT_RUN: invalid preparation or inaccessible path", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
