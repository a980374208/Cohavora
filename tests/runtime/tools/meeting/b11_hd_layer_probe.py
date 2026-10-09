"""Measure one frozen HD source across grid/pin/grid, without formal B11 credit."""
from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import uuid

import b11_hd_input_freeze as freeze
import meeting_render_probe as render
import meeting_soak as soak


def _hash(value):
    return isinstance(value, str) and bool(re.fullmatch(r"[0-9a-f]{16}", value))


def validate_tracks(raw):
    tracks = render.validate_track_probes(raw)
    for item, safe in zip(raw, tracks):
        if not _hash(item.get("identity_hash")):
            raise soak.StopRun("invalid_hd_identity_hash")
        safe["identity_hash"] = item["identity_hash"]
        for name in ("sink_frame_width", "sink_frame_height", "render_frame_width", "render_frame_height"):
            if type(item.get(name)) is not int or not 0 <= item[name] <= 16384:
                raise soak.StopRun("invalid_hd_dimensions")
            safe[name] = item[name]
        for name in ("sink_frame_dimensions_available", "render_frame_dimensions_available", "seat_present", "focused"):
            if type(item.get(name)) is not bool:
                raise soak.StopRun("invalid_hd_state")
            safe[name] = item[name]
        for name in ("desired_width", "desired_height", "desired_max_fps"):
            value = item.get(name)
            if value is not None and (type(value) is not int or not 0 <= value <= 16384):
                raise soak.StopRun("invalid_hd_intent")
            safe[name] = value
        if item.get("desired_enabled") is not None and type(item["desired_enabled"]) is not bool:
            raise soak.StopRun("invalid_hd_intent")
        if item.get("desired_quality") not in (None, "low", "medium", "high", "unknown"):
            raise soak.StopRun("invalid_hd_intent")
        safe.update(desired_enabled=item.get("desired_enabled"),
                    desired_quality=item.get("desired_quality"))
        for name, allowed in (("seat_role", ("grid", "main", "sidebar", "picture_in_picture")),
                              ("seat_quality", ("none", "p180", "p360", "p720", "p1080", "p1440", "p2160", "unknown"))):
            value = item.get(name)
            if safe["seat_present"] and value not in allowed:
                raise soak.StopRun("invalid_hd_seat")
            safe[name] = value if value in allowed else None
    return tracks


def validate_status(raw, run_id, pid):
    current = soak.validate_status(raw, run_id, pid)
    current["selected_tracks"] = validate_tracks(raw.get("selected_tracks"))
    # Existing validator rejects raw/untrusted strings and verifies RTP counters.
    current["inbound_streams"] = render.validate_inbound_streams(raw.get("inbound_streams"))
    for item, safe in zip(raw["inbound_streams"], current["inbound_streams"]):
        for name in ("frame_width", "frame_height", "mapped_sink_frame_width", "mapped_sink_frame_height"):
            if type(item.get(name)) is not int or not 0 <= item[name] <= 16384:
                raise soak.StopRun("invalid_hd_inbound_dimensions")
            safe[name] = item[name]
        for name in ("frame_width_available", "frame_height_available", "frames_per_second_available", "mapped_sink_frame_dimensions_available"):
            if type(item.get(name)) is not bool:
                raise soak.StopRun("invalid_hd_inbound_availability")
            safe[name] = item[name]
        fps = item.get("frames_per_second")
        if type(fps) not in (int, float) or not 0 <= fps <= 1000:
            raise soak.StopRun("invalid_hd_inbound_fps")
        safe["frames_per_second"] = fps
    for name in ("focused_sid_hash", "pinned_sid_hash"):
        value = raw.get(name)
        if value != "" and not _hash(value):
            raise soak.StopRun("invalid_hd_focus_hash")
        current[name] = value
    initial = raw["state"] == "connecting" and not current["selected_tracks"]
    if raw.get("layout_mode") not in ("auto", "grid", "speaker", "picture_in_picture") and not (initial and raw.get("layout_mode") == ""):
        raise soak.StopRun("invalid_hd_layout")
    if raw.get("demand_reason") not in ("visible", "pinned", "active_speaker", "screen_share", "hidden", "whiteboard", "permission_denied", "muted") and not (initial and raw.get("demand_reason") == ""):
        raise soak.StopRun("invalid_hd_demand")
    current.update(layout_mode=raw["layout_mode"], demand_reason=raw["demand_reason"])
    for name in ("page", "page_size", "page_count", "stats_sample_seq", "stats_age_ms"):
        if type(raw.get(name)) is not int or not 0 <= raw[name] <= 2**53:
            raise soak.StopRun("invalid_hd_counter")
        current[name] = raw[name]
    return current


def target_track(status, identity_hash):
    tracks = [t for t in status["selected_tracks"] if t["identity_hash"] == identity_hash]
    if len(tracks) > 1:
        raise soak.StopRun("hd_target_not_unique")
    return tracks[0] if tracks else None


def track_identity(track):
    return (track["sid_hash"], track["rtc_track_hash"], track["binding_rtc_track_hash"],
            track["current_binding_serial"], track["sink_binding_serial"])


def ready(status, track, quality, contract):
    if not track:
        return False
    expected = contract[quality]
    dimensions = (track["sink_frame_width"], track["sink_frame_height"])
    media_ready = (status["state"] == "connected" and
        status["selected_not_bound"] == status["bound_not_selected"] == 0 and
        status["render_timer_active"] and status["video_stage_visible"] and
        track["sink_active"] and track["sink_frame_dimensions_available"] and
        0 <= track["sink_frame_age_ms"] <= contract["maximum_frame_age_ms"] and
        dimensions == (expected["width"], expected["height"]) and
        track["sink_binding_count"] == 1 and track["current_binding_serial"] > 0 and
        track["current_binding_serial"] == track["sink_binding_serial"] and
        bool(track["rtc_track_hash"]) and track["rtc_track_hash"] == track["binding_rtc_track_hash"] and
        track["intent_present"] and track["intent_subscribed"] and
        track["desired_enabled"] is True and track["desired_quality"] == quality and
        track["publication_subscribed"] and not track["subscription_error"] and
        status.get("stats_age_ms", 0) <= contract["maximum_frame_age_ms"] and
        all(track[name] for name in ("stats_bytes_available", "stats_packets_available", "stats_decoded_available")))
    if quality == "high":
        return media_ready and status["layout_mode"] == "speaker" and \
            status["demand_reason"] == "pinned" and track["focused"] and track["seat_role"] == "main" and \
            status["pinned_sid_hash"] == track["sid_hash"] and status["focused_sid_hash"] == track["sid_hash"]
    return media_ready and status["layout_mode"] == "grid" and not status["pinned_sid_hash"] and \
        status["page"] == 0 and status["page_size"] == 16 and status["selected"] == status["bound"] == 16


def evaluate_step(rows, quality, contract):
    """Rows are post-settle, distinct observer heartbeats, not metadata FPS."""
    result = {"quality": quality, "status": "INCONCLUSIVE", "reason": "insufficient_samples",
              "sample_count": len(rows), "source_fps": contract[quality]["source_fps"]}
    if len(rows) < 3:
        return result
    first, last = rows[0], rows[-1]
    duration = last["elapsed_s"] - first["elapsed_s"]
    result["seconds"] = duration
    if duration < contract["minimum_observation_seconds"]:
        return result
    failures = []
    if any(b["elapsed_s"] - a["elapsed_s"] > contract["maximum_sample_gap_seconds"]
           for a, b in zip(rows, rows[1:])):
        failures.append("sample_gap")
    identities = {track_identity(row["target"]) for row in rows}
    if len(identities) != 1:
        failures.append("target_binding_changed")
    valid = [ready(row, row["target"], quality, contract) for row in rows]
    result["dimension_and_state_match_ratio"] = sum(valid) / len(valid)
    if result["dimension_and_state_match_ratio"] < contract["dimension_match_ratio"]:
        failures.append("dimensions_or_view_state_mismatch")
    for field in ("sink_on_frame_count", "sink_delivered_frame_count", "stats_bytes", "stats_packets", "stats_decoded"):
        values = [row["target"][field] for row in rows]
        result[field + "_delta"] = values[-1] - values[0]
        if any(b < a for a, b in zip(values, values[1:])):
            failures.append(field + "_reset")
        if values[-1] <= values[0]:
            failures.append(field + "_no_progress")
    for field in ("render_submits", "render_router_submitted", "render_delivered_to_gpu"):
        result[field + "_delta"] = last[field] - first[field]
        if last[field] <= first[field] or any(b[field] < a[field] for a, b in zip(rows, rows[1:])):
            failures.append(field + "_not_progressing")
    if len({r["target"]["stats_stream_hash"] for r in rows}) != 1:
        failures.append("stats_stream_changed_during_step")
    expected = contract[quality]
    canvas_available = all(r["target"]["render_frame_dimensions_available"] for r in rows)
    if canvas_available and any((r["target"]["render_frame_width"], r["target"]["render_frame_height"]) !=
            (expected["width"], expected["height"]) for r in rows):
        failures.append("canvas_frame_dimensions_mismatch")
    result["canvas_dimensions_status"] = "AVAILABLE" if canvas_available else "UNKNOWN"
    native_fps = result["sink_on_frame_count_delta"] / duration
    decoded_fps = result["stats_decoded_delta"] / duration
    result.update(native_fps=native_fps, decoded_fps=decoded_fps,
                  minimum_fps=contract[quality]["source_fps"] * contract["minimum_frame_rate_ratio"],
                  received_dimensions=sorted({(r["target"]["sink_frame_width"], r["target"]["sink_frame_height"]) for r in rows}),
                  max_frame_age_ms=max(r["target"]["sink_frame_age_ms"] for r in rows),
                  target_identity=list(track_identity(first["target"])))
    result["distinct_stats_samples"] = len({r["stats_sample_seq"] for r in rows})
    if result["distinct_stats_samples"] < 3:
        failures.append("stats_samples_not_progressing")
    if min(native_fps, decoded_fps) < result["minimum_fps"]:
        failures.append("frame_rate_below_frozen_minimum")
    result.update(status="FAIL" if failures else "PASS" if canvas_available else "INCONCLUSIVE",
        reason=",".join(failures) if failures else "actual_dimensions_and_frames_progressed" if canvas_available else "target_canvas_dimensions_unavailable")
    return result


def run(output: Path, executable: Path, *, input_manifest: Path, profile: Path) -> dict:
    frozen = freeze.verify_inputs(input_manifest, executable, profile, require_remote=True,
                                  service_url=os.environ.get("LIVEKIT_URL"))
    if not all(os.environ.get(name) for name in ("LIVEKIT_URL", "LIVEKIT_SOAK_TOKEN")):
        raise ValueError("service_environment_missing")
    settings = frozen["inputs"]["profile"]
    contract, plan = settings["quality_contract"], settings["probe"]["steps"]
    identity = freeze.hd_source_identity(settings)
    identity_hash = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:16]
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    run_id = uuid.uuid4().hex
    summary = {"schema": 1, "run_id": run_id, "kind": freeze.SCOPE,
        "status": "INCONCLUSIVE", "reason": "not_started", "diagnostic_only": True,
        "release_eligible": False, "formal_b11_status": "NOT_RUN", "formal_soak_status": "NOT_RUN",
        "completed_steps": 0, "step_results": [], "actual_rid": "UNKNOWN_NOT_EXPOSED"}
    soak.atomic_json(output / "run.json", {"schema": 1, "run_id": run_id,
        "kind": freeze.SCOPE, "started_utc": soak.utc_now(), "build_configuration": "RelWithDebInfo",
        "binary_identity": frozen["inputs"]["binary_identity"],
        "receiver_arguments": settings["probe"]["receiver_arguments"],
        "input_manifest_sha256": soak.sha256(input_manifest), "profile_sha256": soak.sha256(profile),
        "target_identity_hash": identity_hash, "formal_b11_status": "NOT_RUN", "release_eligible": False})
    soak.atomic_json(output / "plan.json", plan)
    soak.atomic_json(output / "profile.json", settings)
    if not soak.desktop_available():
        summary["reason"] = "interactive_desktop_unavailable"
        soak.archive_evidence(output, summary)
        return summary
    process = sampler = None
    start = time.monotonic()
    resources, last_status, target_identity_value = [], None, None
    seq, step_index, heartbeat = 0, -1, -1
    sent_at = ready_at = None
    rows, consecutive = [], []
    fields = ("elapsed_s", "step", "heartbeat_seq", "selected", "bound", "layout_mode", "desired_quality", "width", "height", "sink_frames", "stats_decoded", "stats_packets", "stats_bytes", "private_bytes", "working_set_bytes", "handles", *render.RESOURCE_FIELDS)
    with (output / "events.jsonl").open("w", encoding="utf-8") as events, \
         (output / "tracks.jsonl").open("w", encoding="utf-8") as tracks, \
         (output / "metrics.csv").open("w", newline="", encoding="utf-8") as metrics:
        writer = csv.DictWriter(metrics, fieldnames=fields)
        writer.writeheader()
        def event(name, **values):
            events.write(json.dumps({"elapsed_s": round(time.monotonic() - start, 3), "event": name, **values}) + "\n")
            events.flush()
        def send(action):
            nonlocal seq
            seq += 1
            soak.atomic_json(output / "command.json", {"schema": 1, "run_id": run_id, "seq": seq, "action": action})
            event("command_sent", seq=seq, action=action)
        def begin(index):
            nonlocal step_index, sent_at, ready_at, rows, consecutive
            step_index, sent_at, ready_at, rows, consecutive = index, time.monotonic(), None, [], []
            send("pin_identity:" + identity if plan[index]["layout"] == "pin_identity" else "grid16" if index == 0 else "unpin")
        try:
            process = subprocess.Popen([str(executable), *settings["probe"]["receiver_arguments"], "--meeting-soak", "--soak-directory", str(output)],
                cwd=soak.ROOT, env=os.environ.copy(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            sampler = render.ClientResourceSampler(process.pid)
            resources.append(sampler.sample())
            event("process_started", pid=process.pid)
            while time.monotonic() - start < settings["probe"]["maximum_wall_seconds"]:
                time.sleep(1)
                now = time.monotonic()
                if process.poll() is not None:
                    raise soak.StopRun("observer_exited_early")
                resource = sampler.sample(now)
                resources.append(resource)
                try:
                    status = validate_status(soak.read_json(output / "status.json"), run_id, process.pid)
                except FileNotFoundError:
                    continue
                if status["heartbeat_seq"] <= heartbeat:
                    if now - start > 15 and last_status and now - start - last_status["elapsed_s"] > contract["maximum_sample_gap_seconds"]:
                        raise soak.StopRun("observer_heartbeat_stale")
                    continue
                heartbeat = status["heartbeat_seq"]
                status.update(resource, elapsed_s=round(now - start, 3), step=step_index)
                track = target_track(status, identity_hash)
                status["target"] = track
                last_status = status
                tracks.write(json.dumps(status) + "\n")
                tracks.flush()
                writer.writerow({**{k: status.get(k) for k in fields},
                    "desired_quality": track.get("desired_quality") if track else None,
                    "width": track.get("sink_frame_width") if track else None,
                    "height": track.get("sink_frame_height") if track else None,
                    "sink_frames": track.get("sink_on_frame_count") if track else None,
                    **{k: track.get(k) if track else None for k in ("stats_decoded", "stats_packets", "stats_bytes")}})
                metrics.flush()
                if status["state"] == "failed":
                    raise soak.StopRun("observer_failed")
                if step_index < 0:
                    if status["state"] == "connected" and status["remote_video_count"] >= 17:
                        begin(0)
                    elif now - start > 90:
                        raise soak.StopRun("connection_timeout")
                    continue
                quality = "high" if step_index == 1 else "low"
                if status["command_seq"] == seq and status["command_status"] == "rejected":
                    raise soak.StopRun("command_rejected")
                if ready_at is None:
                    if now - sent_at > settings["probe"]["settle_seconds"]:
                        raise soak.StopRun("hd_layer_settle_timeout")
                    if status["command_seq"] == seq and status["command_status"] == "applied" and ready(status, track, quality, contract):
                        consecutive.append(status)
                    else:
                        consecutive = []
                    if len(consecutive) < 3:
                        continue
                    candidate = track_identity(track)
                    if target_identity_value is None:
                        target_identity_value = candidate
                    elif candidate != target_identity_value:
                        raise soak.StopRun("target_binding_changed_across_steps")
                    ready_at, rows = now, [status]
                    event("step_ready", step=step_index, quality=quality, target_identity=list(candidate),
                          dimensions=[track["sink_frame_width"], track["sink_frame_height"]], settle_seconds=now-sent_at)
                    continue
                if track is None:
                    raise soak.StopRun("hd_target_lost")
                rows.append(status)
                if now - ready_at >= plan[step_index]["seconds"]:
                    result = evaluate_step(rows, quality, contract)
                    summary["step_results"].append(result)
                    event("step_finished", step=step_index, status=result["status"], reason=result["reason"])
                    if result["status"] != "PASS":
                        raise soak.StopRun(result["reason"], result["status"])
                    summary["completed_steps"] += 1
                    if step_index == len(plan) - 1:
                        summary.update(status="PASS", reason="actual_low_high_low_dimensions_and_frames_verified")
                        break
                    begin(step_index + 1)
            else:
                raise soak.StopRun("probe_wall_timeout")
        except soak.StopRun as error:
            summary.update(status=error.status, reason=error.reason)
        except (OSError, ValueError, RuntimeError):
            summary.update(status="INCONCLUSIVE", reason="measurement_chain_error")
        finally:
            if process and process.poll() is None:
                try:
                    send("stop")
                except OSError:
                    summary.update(status="FAIL", reason="observer_stop_command_write_failed")
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
                    summary.update(status="FAIL", reason="observer_forced_termination")
            if process:
                summary["exit_code"] = process.returncode
                if process.returncode != 0 and summary["status"] == "PASS":
                    summary.update(status="FAIL", reason="observer_exit_not_clean")
            if sampler:
                sampler.close()
            summary["client_resources"] = render.client_resource_summary(resources)
            if summary["client_resources"]["status"] != "AVAILABLE" and summary["status"] == "PASS":
                summary.update(status="INCONCLUSIVE", reason="client_resource_evidence_incomplete")
            if last_status:
                soak.atomic_json(output / "last-status.json", last_status)
            summary.update(wall_seconds=round(time.monotonic()-start, 3), finished_utc=soak.utc_now())
            event("run_finished", status=summary["status"], reason=summary["reason"])
    soak.archive_evidence(output, summary)
    return summary
