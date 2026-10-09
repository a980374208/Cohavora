"""Repeated real-SFU paging and grid-switch render diagnosis, separate from soak."""
from __future__ import annotations

import argparse
import csv
import ctypes
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import time
import uuid

import meeting_soak as soak
import b11_input_freeze as b11_freeze


EXTRA_FIELDS = (
    "track_frames_received", "lease_rejected_frames", "selected_native_sinks",
    "selected_native_recent", "selected_active_leases", "duplicate_same_track",
    "duplicate_new_track", "page", "page_size", "page_count",
    "stats_sample_seq", "stats_age_ms", "stats_video_streams",
    "stats_track_ids_available",
)
RESOURCE_FIELDS = ("resource_pid", "resource_sample_seq", "resource_status",
    "resource_reason", "cpu_time_seconds", "cpu_percent_of_one_core",
    "cpu_percent_of_host", "cpu_sample_interval_seconds", "logical_processor_count")
RESOURCE_COUNTER_FIELDS = ("private_bytes", "working_set_bytes", "handles",
    "cpu_time_seconds", "cpu_percent_of_one_core", "cpu_percent_of_host")
FIELDS = ("elapsed_s", "step", *soak.METRIC_FIELDS[5:], *EXTRA_FIELDS, *RESOURCE_FIELDS,
          "selected_fingerprint")
REMOTE_VIDEOS = 17
CYCLES = 2
OBSERVE_SECONDS = 12
STALL_SECONDS = 10
SETTLE_SECONDS = 35
MAX_WALL_SECONDS = 480
TRACK_BOOL_FIELDS = ("intent_present", "intent_subscribed", "subscription_dirty",
    "settings_dirty", "publication_present", "publication_subscribed",
    "publication_enabled", "subscription_error", "sink_active",
    "stats_bytes_available", "stats_packets_available", "stats_lost_available",
    "stats_decoded_available",
    "stats_received_available")
TRACK_COUNT_FIELDS = ("intent_policy_revision", "current_binding_serial",
    "sink_binding_serial", "sink_binding_count", "sink_on_frame_count",
    "sink_delivered_frame_count", "stats_match_count", "stats_bytes",
    "stats_packets", "stats_decoded", "stats_received")


class ClientResourceSampler(soak.ProcessMetrics):
    """Sample the launched observer's pinned handle; CPU uses monotonic intervals."""
    def __init__(self, pid):
        super().__init__(pid)
        self.sequence = 0
        self.previous_cpu = self.previous_monotonic = None
        self.logical_processor_count = os.cpu_count()
        if os.name == "nt" and self.handle:
            from ctypes import wintypes as w
            self.kernel.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
            self.kernel.GetProcessTimes.restype = w.BOOL
            # Include every processor group on hosts with more than 64 logical CPUs.
            self.kernel.GetActiveProcessorCount.argtypes = [w.WORD]
            self.kernel.GetActiveProcessorCount.restype = w.DWORD
            self.logical_processor_count = self.kernel.GetActiveProcessorCount(0xffff) or None

    def _cpu_seconds(self):
        if not self.handle:
            return None
        from ctypes import wintypes as w
        times = [w.FILETIME() for _ in range(4)]
        if not self.kernel.GetProcessTimes(self.handle, *(ctypes.byref(value) for value in times)):
            return None
        ticks = sum((value.dwHighDateTime << 32) | value.dwLowDateTime for value in times[2:])
        return ticks / 10_000_000

    def sample(self, now=None):
        self.sequence += 1
        now = time.monotonic() if now is None else now
        values = super().sample()
        try:
            cpu_seconds = self._cpu_seconds()
        except (OSError, AttributeError):
            cpu_seconds = None
        values.update(resource_pid=self.pid, resource_sample_seq=self.sequence,
            resource_status="UNKNOWN", resource_reason="process_metrics_unavailable",
            logical_processor_count=self.logical_processor_count,
            cpu_time_seconds=cpu_seconds, cpu_percent_of_one_core=None,
            cpu_percent_of_host=None, cpu_sample_interval_seconds=None)
        baseline = self.previous_cpu is None
        if cpu_seconds is not None and self.previous_cpu is not None and \
                _resource_value_available(now) and \
                now > self.previous_monotonic and cpu_seconds >= self.previous_cpu:
            values["cpu_sample_interval_seconds"] = now - self.previous_monotonic
            values["cpu_percent_of_one_core"] = (cpu_seconds - self.previous_cpu) / \
                (now - self.previous_monotonic) * 100
            if type(self.logical_processor_count) is int and self.logical_processor_count > 0:
                values["cpu_percent_of_host"] = values["cpu_percent_of_one_core"] / \
                    self.logical_processor_count
        self.previous_cpu = cpu_seconds if _resource_value_available(now) else None
        self.previous_monotonic = now if self.previous_cpu is not None else None
        if all(_resource_value_available(values.get(name)) for name in RESOURCE_COUNTER_FIELDS):
            values.update(resource_status="AVAILABLE", resource_reason="")
        elif baseline and cpu_seconds is not None and all(
                _resource_value_available(values.get(name)) for name in
                ("private_bytes", "working_set_bytes", "handles")) and \
                type(self.logical_processor_count) is int and self.logical_processor_count > 0:
            values.update(resource_status="BASELINE", resource_reason="cpu_interval_not_yet_available")
        return values


def _resource_value_available(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def client_resource_summary(samples):
    """Collector coverage only; it never awards a resource/quality acceptance PASS."""
    coverage = {}
    for name in RESOURCE_COUNTER_FIELDS:
        values = [row[name] for row in samples if _resource_value_available(row.get(name))]
        coverage[name] = {"valid_samples": len(values), "peak": max(values, default=None),
            "mean": statistics.fmean(values) if values else None}
    incomplete = [row for row in samples if row.get("resource_status") != "AVAILABLE"]
    # One CPU baseline is expected. A later read failure, including a new baseline,
    # remains an evidence gap even when a subsequent sample recovers.
    if samples and samples[0].get("resource_status") == "BASELINE":
        incomplete = [row for row in incomplete if row is not samples[0]]
    pid = samples[0].get("resource_pid") if samples else None
    pid_consistent = type(pid) is int and pid > 0 and all(
        row.get("resource_pid") == pid for row in samples)
    complete = bool(samples) and pid_consistent and not incomplete and all(
        values["valid_samples"] > 0 for values in coverage.values())
    return {"status": "AVAILABLE" if complete else "UNKNOWN",
        "quality_evidence_eligible": complete, "quality_acceptance_status": "NOT_EVALUATED",
        "reason": "counters_collected" if complete else "client_resource_evidence_incomplete",
        "pid": pid, "pid_consistent": pid_consistent,
        "sample_count": len(samples), "unknown_samples": len(incomplete),
        "cpu_measurement": "process_kernel_plus_user_delta_over_monotonic_interval",
        "cpu_host_denominator": "all_active_logical_processors", "fields": coverage}


def validate_track_probes(value: object) -> list[dict]:
    if not isinstance(value, list) or len(value) > 16:
        raise soak.StopRun("invalid_selected_tracks")
    result = []
    seen = set()
    for item in value:
        if not isinstance(item, dict):
            raise soak.StopRun("invalid_selected_track")
        sid = item.get("sid_hash")
        if not isinstance(sid, str) or not re.fullmatch(r"[0-9a-f]{16}", sid) or sid in seen:
            raise soak.StopRun("invalid_selected_track_sid")
        seen.add(sid)
        safe = {"sid_hash": sid}
        for name in ("rtc_track_hash", "binding_rtc_track_hash",
                     "publication_media_track_hash", "stats_stream_hash"):
            hashed = item.get(name)
            if not isinstance(hashed, str) or (hashed and
                    not re.fullmatch(r"[0-9a-f]{16}", hashed)):
                raise soak.StopRun("invalid_selected_track_hash")
            safe[name] = hashed
        for name in TRACK_BOOL_FIELDS:
            if type(item.get(name)) is not bool:
                raise soak.StopRun("invalid_selected_track_state")
            safe[name] = item[name]
        sent = item.get("sent_subscribed")
        if sent is not None and type(sent) is not bool:
            raise soak.StopRun("invalid_selected_track_sent")
        safe["sent_subscribed"] = sent
        for name in TRACK_COUNT_FIELDS:
            count = item.get(name)
            if type(count) is not int or not 0 <= count <= 2**53:
                raise soak.StopRun("invalid_selected_track_counter")
            safe[name] = count
        lost = item.get("stats_lost")
        if type(lost) is not int or not -(2**53) <= lost <= 2**53:
            raise soak.StopRun("invalid_selected_track_loss")
        safe["stats_lost"] = lost
        for name in ("sink_frame_age_ms", "sink_on_frame_age_ms"):
            age = item.get(name)
            if type(age) is not int or not -1 <= age <= 2**53:
                raise soak.StopRun("invalid_sink_frame_age")
            safe[name] = age
        result.append(safe)
    return result


def validate_inbound_streams(value: object) -> list[dict]:
    if not isinstance(value, list) or len(value) > 32:
        raise soak.StopRun("invalid_inbound_streams")
    result = []
    for item in value:
        if not isinstance(item, dict):
            raise soak.StopRun("invalid_inbound_stream")
        safe = {}
        for name in ("track_hash", "stats_id_hash", "ssrc_hash", "mid_hash",
                     "mapped_sid_hash"):
            hashed = item.get(name)
            if not isinstance(hashed, str) or (hashed and
                    not re.fullmatch(r"[0-9a-f]{16}", hashed)):
                raise soak.StopRun("invalid_inbound_stream_hash")
            safe[name] = hashed
        report_index = item.get("report_index")
        if type(report_index) is not int or not 0 <= report_index <= 16:
            raise soak.StopRun("invalid_inbound_report_index")
        safe["report_index"] = report_index
        for name in ("mapped_binding_serial", "mapped_binding_count",
                     "mapped_sink_on_frame_count", "mapped_sink_delivered_frame_count"):
            count = item.get(name)
            if type(count) is not int or not 0 <= count <= 2**53:
                raise soak.StopRun("invalid_inbound_binding_counter")
            safe[name] = count
        for name in ("mapped_binding_current", "mapped_sink_active"):
            value = item.get(name)
            if type(value) is not bool:
                raise soak.StopRun("invalid_inbound_binding_state")
            safe[name] = value
        age = item.get("mapped_sink_frame_age_ms")
        if type(age) is not int or not -1 <= age <= 2**53:
            raise soak.StopRun("invalid_inbound_binding_frame_age")
        safe["mapped_sink_frame_age_ms"] = age
        for name in ("bytes", "packets", "decoded", "received"):
            count = item.get(name)
            if type(count) is not int or not 0 <= count <= 2**53:
                raise soak.StopRun("invalid_inbound_stream_counter")
            safe[name] = count
            available = item.get(name + "_available")
            if type(available) is not bool:
                raise soak.StopRun("invalid_inbound_stream_availability")
            safe[name + "_available"] = available
        lost = item.get("lost")
        if type(lost) is not int or not -(2**53) <= lost <= 2**53:
            raise soak.StopRun("invalid_inbound_stream_loss")
        if type(item.get("lost_available")) is not bool:
            raise soak.StopRun("invalid_inbound_stream_loss_availability")
        safe["lost"] = lost
        safe["lost_available"] = item["lost_available"]
        result.append(safe)
    return result


def build_steps(grid16_transport=False, grid16_transition=False) -> list[dict]:
    if grid16_transition:
        return [
            {"cycle": 1, "action": "grid9", "page_size": 9,
             "page": 0, "page_count": 2, "selected": 9, "observe_seconds": 45},
            {"cycle": 1, "action": "grid16", "page_size": 16,
             "page": 0, "page_count": 2, "selected": 16, "observe_seconds": 60},
            {"cycle": 1, "action": "pin", "page_size": None,
             "page": None, "page_count": None, "selected": None, "observe_seconds": 45},
            {"cycle": 1, "action": "unpin", "page_size": 16,
             "page": 0, "page_count": 2, "selected": 16, "observe_seconds": 120},
        ]
    if grid16_transport:
        return [{"cycle": 1, "action": "grid16", "page_size": 16,
                 "page": 0, "page_count": 2, "selected": 16}]
    actions = [("grid9", 9, 0), ("next_page", 9, 1),
               ("grid4", 4, 0), ("next_page", 4, 1),
               ("grid9", 9, 0), ("next_page", 9, 1),
               ("grid4", 4, 0),
               *[("next_page", 4, page) for page in (1, 2, 3, 4, 0)]]
    return [{"cycle": cycle, "action": action, "page_size": size, "page": page,
             "page_count": (REMOTE_VIDEOS + size - 1) // size,
             "selected": min(size, REMOTE_VIDEOS - page * size)}
            for cycle in range(1, CYCLES + 1)
            for action, size, page in actions]


def probe_profile(grid16_transport=False, grid16_transition=False) -> dict:
    steps = build_steps(grid16_transport, grid16_transition)
    return {"observe_seconds": None if grid16_transition else 300 if grid16_transport else OBSERVE_SECONDS,
            **({"step_observation_seconds": [step["observe_seconds"] for step in steps]}
               if grid16_transition else {}),
            "stall_seconds": 20 if grid16_transport or grid16_transition else STALL_SECONDS,
            "settle_seconds": SETTLE_SECONDS,
            "maximum_wall_seconds": 520 if grid16_transition else 420 if grid16_transport else MAX_WALL_SECONDS,
            "minimum_remote_videos": REMOTE_VIDEOS, "receiver_count": 1}


def run(output: Path, executable: Path, grid16_transport=False,
        grid16_transition=False, *, input_manifest: Path | None = None,
        profile: Path | None = None) -> dict:
    if grid16_transport and grid16_transition:
        raise ValueError("select_one_probe_mode")
    if (input_manifest is None) != (profile is None):
        raise ValueError("input_manifest_and_profile_required_together")
    frozen_inputs = None
    if input_manifest is not None:
        if not os.environ.get("LIVEKIT_URL"):
            raise ValueError("bound_service_environment_missing")
        frozen_inputs = b11_freeze.verify_inputs(input_manifest, executable, profile,
            require_remote=True, service_url=os.environ.get("LIVEKIT_URL"))
        mode = "grid16_transition" if grid16_transition else "grid16_transport" if grid16_transport else "render"
        if frozen_inputs["inputs"]["profile"]["probe_mode"] != mode:
            raise ValueError("frozen_probe_mode_mismatch")
        if frozen_inputs["inputs"]["profile"]["probe"] != probe_profile(grid16_transport, grid16_transition):
            raise ValueError("frozen_probe_measurement_profile_mismatch")
        binary_identity = frozen_inputs["inputs"]["binary_identity"]
    else:
        binary_identity = soak.verify_runtime_binary(executable)
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    run_id = uuid.uuid4().hex
    soak.atomic_json(output / "run.json", {
        "schema": 1, "run_id": run_id,
        "kind": "grid16_transition_probe" if grid16_transition else
            "grid16_transport_probe" if grid16_transport else "targeted_render_probe",
        "started_utc": soak.utc_now(), "binary_sha256": soak.sha256(executable),
        "build_configuration": soak.BUILD_CONFIGURATION,
        "binary_identity": binary_identity,
        "input_freeze": None if frozen_inputs is None else {
            "manifest_sha256": soak.sha256(input_manifest),
            "profile_sha256": soak.sha256(profile),
            "remote_target": frozen_inputs["remote_target"],
            "binary_source_equivalence": frozen_inputs["binary_source_equivalence"]},
        "acceptance_scope": "B11_PREFLIGHT_DIAGNOSTIC",
        "formal_b11_status": "NOT_RUN",
        "source_inputs": soak.source_fingerprint(),
        "probe_sha256": soak.sha256(Path(__file__)),
    })
    steps = build_steps(grid16_transport, grid16_transition)
    observe_seconds = 300 if grid16_transport else OBSERVE_SECONDS
    stall_seconds = 20 if grid16_transport or grid16_transition else STALL_SECONDS
    max_wall_seconds = (520 if grid16_transition else 420 if grid16_transport
                        else MAX_WALL_SECONDS)
    soak.atomic_json(output / "plan.json", steps)
    soak.atomic_json(output / "profile.json", {"schema": 1,
        "minimum_remote_videos": REMOTE_VIDEOS, "cycles": CYCLES,
        "observation_seconds_per_step": None if grid16_transition else observe_seconds,
        "step_observation_seconds": [step.get("observe_seconds", observe_seconds)
                                     for step in steps],
        "stall_seconds": stall_seconds, "settle_seconds": SETTLE_SECONDS,
        "maximum_wall_seconds": max_wall_seconds})
    summary = {"schema": 1, "run_id": run_id,
        "kind": "grid16_transition_probe" if grid16_transition else
            "grid16_transport_probe" if grid16_transport else "targeted_render_probe",
        "result": "INCONCLUSIVE", "status": "INCONCLUSIVE", "reason": "not_started",
        "formal_b11_status": "NOT_RUN", "diagnostic_only": True, "release_eligible": False,
        "formal_soak_status": "NOT_RUN", "exit_code": None,
        "client_resources": client_resource_summary([]),
        "planned_steps": len(steps), "completed_steps": 0, "step_results": []}
    if not soak.desktop_available():
        summary["reason"] = "interactive_desktop_unavailable"
        soak.archive_evidence(output, summary)
        return summary

    process = None
    process_metrics = None
    resource_samples = []
    status = None
    start = time.monotonic()
    sequence = 0
    step_index = -1
    step_sent_at = step_ready_at = last_router_at = None
    first = last = None
    last_router = 0
    last_decoded = decoded_progress_samples = decoded_resets = 0
    previous_fingerprint = None
    with (output / "events.jsonl").open("w", encoding="utf-8") as events, \
         (output / "metrics.csv").open("w", encoding="utf-8", newline="") as metrics, \
         (output / "tracks.jsonl").open("w", encoding="utf-8") as tracks:
        writer = csv.DictWriter(metrics, fieldnames=FIELDS)
        writer.writeheader()

        def event(name, **values):
            events.write(soak.json.dumps({"elapsed_s": round(time.monotonic() - start, 3),
                                          "event": name, **values}) + "\n")
            events.flush()

        def send(action, target=None):
            nonlocal sequence
            sequence += 1
            soak.atomic_json(output / "command.json", {"schema": 1, "run_id": run_id,
                "seq": sequence, "action": action})
            event("command_sent", seq=sequence, action=action,
                  target=target, previous_fingerprint=previous_fingerprint)

        def finish_step(outcome):
            nonlocal previous_fingerprint
            target = steps[step_index]
            summary["step_results"].append({
                "seq": sequence, "cycle": target["cycle"], "action": target["action"],
                "page": target["page"], "page_size": target["page_size"],
                "selected": first["selected"],
                "selected_fingerprint": first["selected_fingerprint"],
                "outcome": outcome,
                "router_delta": last["render_router_submitted"] - first["render_router_submitted"],
                "track_frame_delta": last["track_frames_received"] - first["track_frames_received"],
                "decoded_first": first["decoded_frames"],
                "decoded_last": last["decoded_frames"],
                "decoded_progress_samples": decoded_progress_samples,
                "decoded_resets": decoded_resets,
                "gpu_delta": last["render_delivered_to_gpu"] - first["render_delivered_to_gpu"],
                "native_sinks_last": last["selected_native_sinks"],
                "native_recent_first": first["selected_native_recent"],
                "native_recent_last": last["selected_native_recent"],
                "active_leases_last": last["selected_active_leases"],
                "fingerprint_changed_during_step":
                    last["selected_fingerprint"] != first["selected_fingerprint"],
                "lease_rejected_delta": last["lease_rejected_frames"] - first["lease_rejected_frames"],
                "duplicate_new_delta": last["duplicate_new_track"] - first["duplicate_new_track"],
            })
            if outcome == "PROGRESSED":
                summary["completed_steps"] += 1
            else:
                summary["stalled_tracks_first"] = first["selected_tracks"]
                summary["stalled_tracks_last"] = last["selected_tracks"]
                summary["stalled_inbound_first"] = first["inbound_streams"]
                summary["stalled_inbound_last"] = last["inbound_streams"]
            previous_fingerprint = first["selected_fingerprint"]
            event("step_ended", seq=sequence, outcome=outcome,
                  router_delta=summary["step_results"][-1]["router_delta"])

        def begin_step(index, now):
            nonlocal step_index, step_sent_at, step_ready_at, first, last
            nonlocal last_decoded, decoded_progress_samples, decoded_resets
            step_index = index
            step_sent_at = now
            step_ready_at = None
            first = last = None
            last_decoded = decoded_progress_samples = decoded_resets = 0
            send(steps[index]["action"], steps[index])

        try:
            process = subprocess.Popen(
                [str(executable), "--meeting-soak", "--soak-directory", str(output)],
                cwd=soak.ROOT, env=os.environ.copy(), stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            event("process_started", pid=process.pid)
            process_metrics = ClientResourceSampler(process.pid)
            resource_samples.append(process_metrics.sample(time.monotonic()))
            event("resource_sampler_started", **resource_samples[-1])
            while time.monotonic() - start < max_wall_seconds:
                time.sleep(1)
                now = time.monotonic()
                if process.poll() is not None:
                    raise RuntimeError("observer_exited_early")
                try:
                    raw = soak.read_json(output / "status.json")
                    current = soak.validate_status(raw, run_id, process.pid)
                    for name in EXTRA_FIELDS:
                        value = raw.get(name)
                        if type(value) is not int or not 0 <= value <= 2**53:
                            raise soak.StopRun("invalid_probe_counter")
                        current[name] = value
                    fingerprint = raw.get("selected_fingerprint")
                    if not isinstance(fingerprint, str) or (fingerprint and
                            not re.fullmatch(r"[0-9a-f]{64}", fingerprint)):
                        raise soak.StopRun("invalid_selection_fingerprint")
                    current["selected_fingerprint"] = fingerprint
                    current["selected_tracks"] = validate_track_probes(
                        raw.get("selected_tracks"))
                    current["inbound_streams"] = validate_inbound_streams(
                        raw.get("inbound_streams"))
                except OSError:
                    continue
                status = current
                resources = process_metrics.sample(now)
                resource_samples.append(resources)
                status.update(resources)
                label = "connect" if step_index < 0 else (f"{step_index + 1:02d}:"
                    f"{steps[step_index]['action']}:p{steps[step_index]['page']}")
                writer.writerow({"elapsed_s": round(now - start, 3), "step": label,
                    **{name: status.get(name) for name in FIELDS if name not in ("elapsed_s", "step")}})
                metrics.flush()
                tracks.write(soak.json.dumps({
                    "elapsed_s": round(now - start, 3), "step": label,
                    "page": status["page"], "page_size": status["page_size"],
                    "selected_fingerprint": status["selected_fingerprint"],
                    "stats_sample_seq": status["stats_sample_seq"],
                    "stats_age_ms": status["stats_age_ms"],
                    "selected_tracks": status["selected_tracks"],
                    "inbound_streams": status["inbound_streams"]}) + "\n")
                tracks.flush()
                if status["state"] == "failed":
                    raise RuntimeError("observer_failed")
                if step_index < 0:
                    if status["state"] == "connected" and \
                            status["remote_video_count"] >= REMOTE_VIDEOS:
                        begin_step(0, now)
                    continue
                target = steps[step_index]
                if status["command_seq"] == sequence and \
                        status["command_status"] == "rejected":
                    raise RuntimeError("command_rejected")
                if step_ready_at is None:
                    if now - step_sent_at >= SETTLE_SECONDS:
                        raise RuntimeError("step_settle_timeout")
                    if status["command_seq"] != sequence or \
                            status["command_status"] != "applied" or \
                            (target["page"] is not None and
                             status["page"] != target["page"]) or \
                            (target["page_size"] is not None and
                             status["page_size"] != target["page_size"]) or \
                            (target["page_count"] is not None and
                             status["page_count"] != target["page_count"]) or \
                            (target["selected"] is not None and
                             status["selected"] != target["selected"]) or \
                            status["selected"] == 0 or \
                            status["bound"] != status["selected"] or \
                            not status["selected_fingerprint"] or \
                            status["selected_fingerprint"] == previous_fingerprint:
                        continue
                    step_ready_at = last_router_at = now
                    first = last = status.copy()
                    last_router = status["render_router_submitted"]
                    last_decoded = status["decoded_frames"]
                    event("step_ready", seq=sequence, page=status["page"],
                          page_size=status["page_size"], selected=status["selected"],
                          fingerprint=status["selected_fingerprint"],
                          native_sinks=status["selected_native_sinks"],
                          native_recent=status["selected_native_recent"],
                          active_leases=status["selected_active_leases"])
                    continue
                last = status.copy()
                decoded_progress_samples += status["decoded_frames"] > last_decoded
                decoded_resets += status["decoded_frames"] < last_decoded
                last_decoded = status["decoded_frames"]
                if status["render_router_submitted"] > last_router:
                    last_router = status["render_router_submitted"]
                    last_router_at = now
                if now - last_router_at >= stall_seconds:
                    summary.update(result="STALLED", reason="router_no_frames_on_step",
                                   stalled_step={"seq": sequence, **target})
                    finish_step("STALLED")
                    break
                if now - step_ready_at >= target.get("observe_seconds", observe_seconds):
                    finish_step("PROGRESSED")
                    if step_index + 1 == len(steps):
                        summary.update(result="NOT_REPRODUCED",
                                       reason="all_steps_progressed" if grid16_transition
                                           else "all_page_and_grid_steps_progressed")
                        break
                    begin_step(step_index + 1, now)
            else:
                summary["reason"] = "probe_timeout"
        except (OSError, RuntimeError, soak.StopRun) as error:
            summary["reason"] = str(error)
        finally:
            if process and process.poll() is None:
                send("stop")
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
                    summary["forced_termination"] = True
            if process:
                summary["exit_code"] = process.returncode
            if process_metrics:
                process_metrics.close()
            summary["client_resources"] = client_resource_summary(resource_samples)
            if status:
                soak.atomic_json(output / "last-status.json", status)
            if step_index >= 0 and summary["result"] == "INCONCLUSIVE":
                summary["current_step"] = {"seq": step_index + 1, **steps[step_index]}
            summary["wall_seconds"] = round(time.monotonic() - start, 3)
            summary["finished_utc"] = soak.utc_now()
            event("run_ended", result=summary["result"], reason=summary["reason"])
    summary["status"] = summary["result"]
    soak.archive_evidence(output, summary)
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--grid16-transport", action="store_true")
    mode.add_argument("--grid16-transition", action="store_true")
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--input-manifest", type=Path)
    parser.add_argument("--profile", type=Path)
    args = parser.parse_args()
    if not args.executable.is_file() or not all(os.environ.get(name) for name in
            ("LIVEKIT_URL", "LIVEKIT_SOAK_TOKEN")):
        parser.error("existing executable and in-memory service credentials required")
    try:
        result = run(args.output, args.executable.resolve(), args.grid16_transport,
                     args.grid16_transition, input_manifest=args.input_manifest,
                     profile=args.profile)
    except (OSError, ValueError):
        parser.error("RelWithDebInfo binary or frozen inputs did not verify; observer was not started")
    print(f"{result['result']}: {result['reason']}; evidence={args.output.resolve()}")
    raise SystemExit(1 if result["result"] == "INCONCLUSIVE" else 0)
