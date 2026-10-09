"""Measure source-bound 2K/4K layers, actual frames, FPS, CPU and WDDM resources."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time
import uuid

import b11_highres_input_freeze as freeze
import b11_hd_layer_probe as hd
import b11_highres_publisher_probe as publisher_probe
import b11_gpu_resource_sampler as gpu
import meeting_render_probe as render
import meeting_soak as soak


class ObserverFailed(soak.StopRun):
    def __init__(self, safe_status):
        super().__init__("observer_failed", status="INCONCLUSIVE")
        self.safe_status = safe_status


def validate_status(raw, run_id, pid):
    baseline = soak.validate_status(raw, run_id, pid)
    if baseline["state"] == "failed":
        # Coordinator can fail before an accepted media plan exists. Validate
        # identity/counters first, then preserve this terminal cause without
        # inventing layout fields or applying the media-projection validator.
        safe_errors = {"meeting_failed", "unexpected_disconnect", "unexpected_shutdown",
                       "coordinator_error", "screen_share_failed"}
        if type(raw.get("error_code")) is str and raw["error_code"] in safe_errors:
            baseline["error_code"] = raw["error_code"]
        baseline["media_projection_status"] = "UNAVAILABLE"
        raise ObserverFailed(baseline)
    current = hd.validate_status(raw, run_id, pid)
    for item, safe in zip(raw["selected_tracks"], current["selected_tracks"]):
        if item.get("source") not in ("camera", "screen_share", "unknown"):
            raise soak.StopRun("invalid_highres_track_source")
        safe["source"] = item["source"]
        for name in ("show_local_participant", "show_local_screen_share"):
            if type(item.get(name)) is not bool:
                raise soak.StopRun("invalid_highres_local_seats")
            safe[name] = item[name]
        for name in ("seat_subscription_width", "seat_subscription_height",
                     "selected_layer_announced_width", "selected_layer_announced_height"):
            value = item.get(name)
            if value is not None and (type(value) is not int or not 0 <= value <= 16384):
                raise soak.StopRun("invalid_highres_layer_dimensions")
            safe[name] = value
        quality = item.get("seat_selected_layer_quality")
        if quality not in (None, "none", "low", "medium", "high"):
            raise soak.StopRun("invalid_highres_selected_layer")
        safe["seat_selected_layer_quality"] = quality
    return current


def actual_layer(track, source):
    layer = track.get("publisher_layer") if track else None
    rid = track.get("publisher_rid_correlated") if track else None
    return layer if layer in source["layers"] and source["layers"][layer]["rid"] == rid else None


def correlate_publisher(track, sample, source):
    if track is None:
        return
    layer = publisher_probe.match_layer(track, sample, source)
    rid = None
    if layer is not None:
        matches = [item["rid"] for item in sample["outbound"]
            if item["rid"] == source["layers"][layer]["rid"]
            and (item["width"], item["height"]) == (track["sink_frame_width"], track["sink_frame_height"])]
        if len(matches) == 1:
            rid = matches[0]
    track.update(publisher_layer=layer if rid is not None else None, publisher_rid_correlated=rid)


def publisher_layer_ready(status, track, source, contract):
    sample = status.get("publisher_probe")
    layer = actual_layer(track, source)
    return (contract["require_fresh_publisher_layer_evidence"] is True and layer is not None
        and sample is not None and 0 <= sample["age_seconds"] <= contract["maximum_publisher_age_seconds"]
        and publisher_probe.match_layer(track, sample, source) == layer)


def target_track(status, source):
    identity_hash = hashlib.sha256(source["target_identity"].encode()).hexdigest()[:16]
    return hd.target_track(status, identity_hash)


def measurement_ready(status, track, source, sid_hash, contract):
    if not track:
        return False
    return (status["state"] == "connected" and status["selected_not_bound"] == status["bound_not_selected"] == 0
        and status["render_timer_active"] and status["video_stage_visible"] and status["renderer_ready"]
        and track["sid_hash"] == sid_hash and track["source"] == source["source"]["source"]
        and (track["source_width"], track["source_height"]) ==
            (source["source"]["width"], source["source"]["height"])
        and track["sink_active"] and track["sink_frame_dimensions_available"]
        and min(track["sink_frame_width"], track["sink_frame_height"]) > 0
        and 0 <= track["sink_frame_age_ms"] <= contract["maximum_frame_age_ms"]
        and track["sink_binding_count"] == 1 and track["current_binding_serial"] > 0
        and track["current_binding_serial"] == track["sink_binding_serial"]
        and bool(track["rtc_track_hash"]) and track["rtc_track_hash"] == track["binding_rtc_track_hash"]
        and track["intent_present"] and track["intent_subscribed"] and track["desired_enabled"] is True
        and track["publication_subscribed"] and not track["subscription_error"]
        and status["stats_age_ms"] <= contract["maximum_frame_age_ms"]
        and all(track[name] for name in ("stats_bytes_available", "stats_packets_available", "stats_decoded_available"))
        and track["render_frame_dimensions_available"]
        and (track["render_frame_width"], track["render_frame_height"]) ==
            (track["sink_frame_width"], track["sink_frame_height"])
        and _raw_dimensions_valid(status, track)
        and publisher_layer_ready(status, track, source, contract))


def _viewport_requested_dimensions(status, track, step, contract):
    """Independently fit the observed physical seat to the declared source."""
    names = ("viewport_stage_width", "viewport_stage_height", "source_width", "source_height")
    if any(type(track.get(name)) is not int or track[name] <= 0 for name in names):
        return None
    stage_width, stage_height = track["viewport_stage_width"], track["viewport_stage_height"]
    if stage_width > track["window_width"] or stage_height > track["window_height"]:
        return None
    if step["request"].startswith("grid"):
        side = {4: 2, 9: 3, 16: 4}.get(status["page_size"])
        if side is None:
            return None
        extent_width, extent_height = stage_width // side, stage_height // side
        maximum = contract["grid_maximum_dimensions"]
    else:
        # This frozen fixture has a sidebar when more than one remote is selected.
        extent_width = stage_width * 3 // 4 if status["selected"] > 1 else stage_width
        extent_height = stage_height
        maximum = contract["main_maximum_dimensions"]
    source_width, source_height = track["source_width"], track["source_height"]
    maximum_width, maximum_height = maximum if source_width >= source_height else maximum[::-1]
    scale = min(extent_width / source_width, extent_height / source_height,
        maximum_width / source_width, maximum_height / source_height, 1.0)
    return (min(maximum_width, max(1, math.floor(source_width * scale + 0.5))),
        min(maximum_height, max(1, math.floor(source_height * scale + 0.5))))


def expected_layer(track, source, step, contract):
    """Select only declared layers; the unique layer is a distinct contract."""
    layers = list(source["layers"].items())
    if len(layers) == 1:
        return layers[0][0]
    main = not step["request"].startswith("grid")
    maximum = contract["main_maximum_dimensions" if main else "grid_maximum_dimensions"]
    candidates = [(name, layer) for name, layer in layers
        if max(layer["width"], layer["height"]) <= maximum[0]
        and min(layer["width"], layer["height"]) <= maximum[1]]
    if not candidates:
        return None
    ordering = lambda item: (item[1]["width"] * item[1]["height"],
        item[1]["width"], item[1]["height"], {"low": 0, "medium": 1, "high": 2}[item[1]["quality"]], item[1]["rid"])
    candidates.sort(key=ordering)
    width, height = track["seat_width"], track["seat_height"]
    fits = [(name, layer) for name, layer in candidates if
        layer["width"] >= width and layer["height"] >= height] if main else [
        (name, layer) for name, layer in candidates if layer["width"] <= width and layer["height"] <= height]
    return (fits[0] if main else fits[-1])[0] if fits else (candidates[-1] if main else candidates[0])[0]


def _viewport_tier(width, height):
    edge = min(width, height)
    return next((tier for limit, tier in ((180, "p180"), (360, "p360"), (720, "p720"),
        (1080, "p1080"), (1440, "p1440")) if edge <= limit), "p2160")


def request_policy_valid(status, track, source, step, contract):
    if ((track["window_width"], track["window_height"]) != (step["window_width"], step["window_height"])
            or track["viewport_device_pixel_ratio"] != 1.0
            or not track["seat_present"]):
        return False
    width, height = track["desired_width"], track["desired_height"]
    seat_width, seat_height = track["seat_width"], track["seat_height"]
    if any(type(value) is not int or value <= 0 for value in (width, height, seat_width, seat_height)):
        return False
    if (seat_width, seat_height) != _viewport_requested_dimensions(status, track, step, contract):
        return False
    selected = expected_layer(track, source, step, contract)
    if selected is None:
        return False
    layer = source["layers"][selected]
    if not ((width, height) == (layer["width"], layer["height"])
            == (track.get("seat_subscription_width"), track.get("seat_subscription_height"))
            == (track.get("selected_layer_announced_width"), track.get("selected_layer_announced_height"))
            and track.get("seat_selected_layer_quality") == track["desired_quality"] == layer["quality"]
            and track["seat_quality"] == _viewport_tier(seat_width, seat_height)
            and track["desired_max_fps"] == (15 if min(width, height) <= 180 else 30)):
        return False
    request = step["request"]
    if request.startswith("grid"):
        local_seats = contract["grid_first_page_local_seats"]
        local_count = int(track["show_local_participant"]) + int(track["show_local_screen_share"])
        if not (status["layout_mode"] == "grid" and track["seat_role"] == "grid"
                and not track["focused"] and not status["pinned_sid_hash"] and status["page"] == 0
                and status["page_size"] == int(step["layout"][4:])
                and track["show_local_participant"] is local_seats["participant"]
                and track["show_local_screen_share"] is local_seats["screen_share"]
                and status["selected"] == status["bound"]
                and status["selected"] + local_count == int(step["layout"][4:])):
            return False
        return True
    if not (status["layout_mode"] == "speaker" and track["seat_role"] == "main" and track["focused"]
            and status["focused_sid_hash"] == track["sid_hash"]):
        return False
    if step["layout"] == "auto":
        if status["demand_reason"] != "screen_share" or status["pinned_sid_hash"]:
            return False
    elif status["demand_reason"] != "pinned" or status["pinned_sid_hash"] != track["sid_hash"]:
        return False
    return True


def actual_resolution_valid(track, source, step, contract):
    width, height = track["sink_frame_width"], track["sink_frame_height"]
    source_size = source["source"]
    if min(width, height) <= 0 or abs(width * source_size["height"] / (height * source_size["width"]) - 1) > contract["maximum_source_aspect_relative_error"]:
        return False
    selected = expected_layer(track, source, step, contract)
    if selected is None or actual_layer(track, source) != selected:
        return False
    layer = source["layers"][selected]
    if step["request"].startswith("grid"):
        return (contract["allow_dynamic_encoder_layer_downscale"] is True
            and width <= layer["width"] and height <= layer["height"])
    # A main measurement must deliver its selected declaration, including 4K.
    return (width, height) == (layer["width"], layer["height"])


def _raw_dimensions_valid(row, track):
    dimensions = (track["sink_frame_width"], track["sink_frame_height"])
    streams = [stream for stream in row["inbound_streams"]
        if stream["mapped_sid_hash"] == track["sid_hash"] and stream["track_hash"] == track["rtc_track_hash"]
        and stream["mapped_binding_current"] and stream["mapped_sink_active"]]
    if not streams:
        return False
    return all(stream["frame_width_available"] and stream["frame_height_available"]
        and stream["mapped_sink_frame_dimensions_available"]
        and (stream["frame_width"], stream["frame_height"]) == dimensions
        and (stream["mapped_sink_frame_width"], stream["mapped_sink_frame_height"]) == dimensions
        and stream["mapped_binding_serial"] == track["current_binding_serial"] for stream in streams)


def _dimension_pairs(rows, width_name, height_name):
    return sorted({(row["target"].get(width_name), row["target"].get(height_name))
        for row in rows if all(type(row["target"].get(name)) is int
            and row["target"][name] > 0 for name in (width_name, height_name))})


def evaluate_step(rows, step, source, sid_hash, contract):
    result = {"request": step["request"], "layout": step["layout"], "status": "INCONCLUSIVE",
        "reason": "insufficient_samples", "sample_count": len(rows),
        "expected_window": [step["window_width"], step["window_height"]]}
    if len(rows) < 3:
        return result
    first, last = rows[0], rows[-1]
    duration = last["elapsed_s"] - first["elapsed_s"]
    result["seconds"] = duration
    if duration < contract["minimum_observation_seconds"]:
        return result
    failures = []
    if any(b["elapsed_s"] - a["elapsed_s"] > contract["maximum_sample_gap_seconds"] for a, b in zip(rows, rows[1:])):
        failures.append("sample_gap")
    if len({hd.track_identity(row["target"]) for row in rows}) != 1:
        failures.append("target_binding_changed")
    if not all(measurement_ready(row, row["target"], source, sid_hash, contract) for row in rows):
        failures.append("media_state_mismatch")
    request_ok = all(request_policy_valid(row, row["target"], source, step, contract) for row in rows)
    actual_ok = all(actual_resolution_valid(row["target"], source, step, contract) for row in rows)
    result["request_policy_status"] = "PASS" if request_ok else "FAIL"
    result["actual_resolution_status"] = "PASS" if actual_ok else "FAIL"
    if not request_ok:
        failures.append("request_policy_mismatch")
    if not actual_ok:
        failures.append("actual_grid_selected_layer_mismatch" if step["request"].startswith("grid") else "actual_main_resolution_mismatch")
    actual_layers = {actual_layer(row["target"], source) for row in rows}
    if None in actual_layers or len(actual_layers) != 1:
        failures.append("actual_layer_changed_during_step")
    if len({(row["target"]["sink_frame_width"], row["target"]["sink_frame_height"]) for row in rows}) != 1:
        failures.append("received_dimensions_changed_during_step")
    if not all(publisher_layer_ready(row, row["target"], source, contract) for row in rows):
        failures.append("publisher_layer_evidence_mismatch")
    expected_fps = max((source["layers"][name]["source_fps"] for name in actual_layers if name is not None), default=0)
    for field in ("sink_on_frame_count", "sink_delivered_frame_count", "stats_bytes", "stats_packets", "stats_decoded"):
        values = [row["target"][field] for row in rows]
        result[field + "_delta"] = values[-1] - values[0]
        if values[-1] <= values[0] or any(b < a for a, b in zip(values, values[1:])):
            failures.append(field + "_not_progressing")
    for field in ("render_submits", "render_router_submitted", "render_delivered_to_gpu"):
        values = [row[field] for row in rows]
        result[field + "_delta"] = values[-1] - values[0]
        if values[-1] <= values[0] or any(b < a for a, b in zip(values, values[1:])):
            failures.append(field + "_not_progressing")
    if len({row["target"]["stats_stream_hash"] for row in rows}) != 1:
        failures.append("stats_stream_changed_during_step")
    if len({row["stats_sample_seq"] for row in rows}) < 3:
        failures.append("stats_samples_not_progressing")
    if not all(_raw_dimensions_valid(row, row["target"]) for row in rows):
        failures.append("raw_rtp_frame_dimensions_mismatch")
    if not all(row["target"]["render_frame_dimensions_available"] and
            (row["target"]["render_frame_width"], row["target"]["render_frame_height"]) ==
            (row["target"]["sink_frame_width"], row["target"]["sink_frame_height"]) for row in rows):
        failures.append("canvas_frame_dimensions_mismatch")
    fps = {"native_fps": result["sink_on_frame_count_delta"] / duration,
        "delivered_fps": result["sink_delivered_frame_count_delta"] / duration,
        "decoded_fps": result["stats_decoded_delta"] / duration}
    minimum_fps = expected_fps * contract["minimum_frame_rate_ratio"]
    if min(fps.values()) < minimum_fps or expected_fps <= 0:
        failures.append("frame_rate_below_frozen_minimum")
    if len({row["command_seq"] for row in rows}) != 1 or any(row["command_status"] != "applied" for row in rows):
        failures.append("step_command_ack_changed")
    if source["scenario"].endswith("4k"):
        samples = [row.get("publisher_probe") for row in rows]
        counters_valid = all(type(sample) is dict and all(type(sample.get(name)) is int and sample[name] > 0
            for name in ("captured_frames", "capture_frames")) and type(sample.get("elapsed_seconds")) in (int, float)
            and math.isfinite(sample["elapsed_seconds"]) for sample in samples)
        if counters_valid:
            source_interval = samples[-1]["elapsed_seconds"] - samples[0]["elapsed_seconds"]
            source_delta = samples[-1]["captured_frames"] - samples[0]["captured_frames"]
            backend_delta = samples[-1]["capture_frames"] - samples[0]["capture_frames"]
            source_fps = source_delta/source_interval if source_interval > 0 else None
            backend_fps = backend_delta/source_interval if source_interval > 0 else None
            counters_valid = source_interval > 0 and source_delta > 0 and backend_delta > 0 and all(b[name] >= a[name]
                for a,b in zip(samples,samples[1:]) for name in ("captured_frames","capture_frames"))
            source_rate_ok = counters_valid and source_fps >= source["source"]["source_fps"] * contract["minimum_frame_rate_ratio"]
            result.update(source_capture_fps=source_fps, capture_callback_delivery_fps=backend_fps,
                source_capture_interval_seconds=source_interval, source_capture_count_delta=source_delta,
                capture_callback_delivery_count_delta=backend_delta, source_capture_rate_status="PASS" if source_rate_ok else "FAIL")
        else:
            source_rate_ok = False
            result.update(source_capture_rate_status="UNKNOWN")
        if not source_rate_ok:
            failures.append("source_capture_frame_rate_below_frozen_minimum")
    resources = render.client_resource_summary(rows)
    gpu_resources = gpu.gpu_resource_summary(rows)
    result.update(fps, source_fps=expected_fps, minimum_fps=minimum_fps,
        actual_layers=sorted(name for name in actual_layers if name is not None),
        announced_layer_dimensions=sorted({(source["layers"][name]["width"], source["layers"][name]["height"])
            for name in actual_layers if name is not None}),
        publisher_rids_correlated=sorted({row["target"]["publisher_rid_correlated"] for row in rows
            if row["target"].get("publisher_rid_correlated") is not None}),
        receiver_rid="UNKNOWN_NOT_EXPOSED",
        viewport_demand_dimensions=sorted({(row["target"]["seat_width"], row["target"]["seat_height"]) for row in rows}),
        selected_layer_qualities=sorted({row["target"]["seat_selected_layer_quality"] for row in rows
            if row["target"].get("seat_selected_layer_quality") in ("low", "medium", "high")}),
        selected_layer_announced_dimensions=_dimension_pairs(rows,
            "selected_layer_announced_width", "selected_layer_announced_height"),
        seat_subscription_dimensions=_dimension_pairs(rows, "seat_subscription_width", "seat_subscription_height"),
        requested_dimensions=sorted({(row["target"]["desired_width"], row["target"]["desired_height"]) for row in rows}),
        received_dimensions=sorted({(row["target"]["sink_frame_width"], row["target"]["sink_frame_height"]) for row in rows}),
        remote_subscriptions=sorted({row["selected"] for row in rows}),
        local_participant_seats=sorted({int(row["target"]["show_local_participant"]) for row in rows}),
        local_screen_share_seats=sorted({int(row["target"]["show_local_screen_share"]) for row in rows}),
        target_identity=list(hd.track_identity(first["target"])), client_resources=resources,
        gpu_resources=gpu_resources)
    media_reason = ",".join(sorted(set(failures))) if failures else "actual_policy_layers_frames_verified"
    result.update(media_status="FAIL" if failures else "PASS", media_reason=media_reason)
    complete = resources["status"] == "AVAILABLE" and (not contract.get("require_gpu_resource_evidence")
        or gpu_resources["status"] == "AVAILABLE")
    result.update(status="FAIL" if failures else "PASS" if complete else "INCONCLUSIVE",
        reason=media_reason if failures or complete else "client_resource_evidence_incomplete" if
            resources["status"] != "AVAILABLE" else "gpu_resource_evidence_incomplete")
    return result


def run(output: Path, executable: Path, *, input_manifest: Path, profile: Path):
    frozen = freeze.verify_inputs(input_manifest, executable, profile, service_url=os.environ.get("LIVEKIT_URL"))
    if not all(os.environ.get(name) for name in ("LIVEKIT_URL", "LIVEKIT_SOAK_TOKEN")):
        raise ValueError("service_environment_missing")
    settings, source = frozen["inputs"]["profile"], frozen["inputs"]["source_identity"]
    contract, plan, sid_hash = settings["quality_contract"], settings["probe"]["steps"], frozen["inputs"]["source_track_sid_hash"]
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    run_id = uuid.uuid4().hex
    summary = {"schema": 1, "run_id": run_id, "kind": settings["scope"], "scenario": settings["scenario"],
        "status": "INCONCLUSIVE", "reason": "not_started", "diagnostic_only": True, "release_eligible": False,
        "formal_b11_status": "NOT_RUN", "completed_steps": 0, "step_results": [], "actual_rid": "UNKNOWN_NOT_EXPOSED"}
    soak.atomic_json(output / "run.json", {"schema": 1, "run_id": run_id, "kind": settings["scope"],
        "started_utc": soak.utc_now(), "build_configuration": freeze.CONFIGURATION,
        "binary_identity": frozen["inputs"]["binary_identity"], "input_manifest_sha256": soak.sha256(input_manifest),
        "profile_sha256": soak.sha256(profile), "target_sid_hash": sid_hash})
    soak.atomic_json(output / "plan.json", plan)
    soak.atomic_json(output / "profile.json", settings)
    if not soak.desktop_available():
        summary["reason"] = "interactive_desktop_unavailable"
        soak.archive_evidence(output, summary)
        return summary
    process = sampler = sender_sampler = gpu_sampler = None
    start, resources, last_status, identity_value = time.monotonic(), [], None, None
    seq, index, heartbeat = 0, -1, -1
    sent_at = ready_at = None
    actions, rows, consecutive = [], [], []
    with (output / "events.jsonl").open("w", encoding="utf-8") as events, (output / "tracks.jsonl").open("w", encoding="utf-8") as tracks:
        def event(name, **values):
            events.write(json.dumps({"elapsed_s": round(time.monotonic()-start, 3), "event": name, **values}) + "\n")
            events.flush()
        def send(action):
            nonlocal seq, sent_at
            seq += 1
            sent_at = time.monotonic()
            soak.atomic_json(output / "command.json", {"schema": 1, "run_id": run_id, "seq": seq, "action": action})
            event("command_sent", seq=seq, action=action)
        def begin(next_index):
            nonlocal index, actions, ready_at, rows, consecutive
            index, ready_at, rows, consecutive = next_index, None, [], []
            step = plan[index]
            actions = [f"window_resize:{step['window_width']}x{step['window_height']}"]
            if step["layout"].startswith("grid"):
                actions += ["unpin", step["layout"]]
            else:
                actions += ["auto" if step["layout"] == "auto" else "pin_identity:" + source["target_identity"]]
            send(actions.pop(0))
        try:
            publisher_ready = freeze.base._target_config_path(source["artifacts"]["publisher_ready"]["path"], soak.ROOT)
            sender_sampler = publisher_probe.PublisherProbeSampler(
                publisher_ready.parent / ".publisher-stdout.tmp", source, sid_hash)
            process = subprocess.Popen([str(executable), *settings["probe"]["receiver_arguments"],
                "--meeting-soak", "--soak-directory", str(output)], cwd=soak.ROOT, env=os.environ.copy(),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            sampler = render.ClientResourceSampler(process.pid)
            gpu_sampler = gpu.GpuResourceSampler(process.pid)
            first_resource = sampler.sample()
            first_resource.update(gpu_sampler.sample())
            resources.append(first_resource)
            event("process_started", pid=process.pid)
            while time.monotonic()-start < settings["probe"]["maximum_wall_seconds"]:
                time.sleep(1)
                now = time.monotonic()
                if process.poll() is not None:
                    raise soak.StopRun("observer_exited_early")
                resource = sampler.sample(now)
                resource.update(gpu_sampler.sample(now))
                resources.append(resource)
                sender_sample = sender_sampler.sample(time.monotonic())
                try:
                    status = validate_status(soak.read_json(output / "status.json"), run_id, process.pid)
                except FileNotFoundError:
                    continue
                if status["heartbeat_seq"] <= heartbeat:
                    if last_status and now-start-last_status["elapsed_s"] > contract["maximum_sample_gap_seconds"]:
                        raise soak.StopRun("observer_heartbeat_stale")
                    continue
                heartbeat = status["heartbeat_seq"]
                status.update(resource, elapsed_s=round(now-start, 3), step=index)
                track = target_track(status, source)
                status["publisher_probe"] = sender_sample
                correlate_publisher(track, sender_sample, source)
                status["target"] = track
                last_status = status
                tracks.write(json.dumps(status) + "\n")
                tracks.flush()
                if status["state"] == "failed":
                    raise soak.StopRun("observer_failed")
                if index < 0:
                    if status["state"] == "connected" and status["remote_video_count"] >= settings["probe"]["minimum_remote_videos"]:
                        begin(0)
                    elif now-start > 90:
                        raise soak.StopRun("connection_timeout")
                    continue
                if status["command_seq"] == seq and status["command_status"] == "rejected":
                    raise soak.StopRun("command_rejected")
                if status["command_seq"] != seq or status["command_status"] != "applied":
                    if now-sent_at > settings["probe"]["settle_seconds"]:
                        raise soak.StopRun("highres_command_timeout")
                    continue
                if actions:
                    send(actions.pop(0))
                    continue
                if ready_at is None:
                    if now-sent_at > settings["probe"]["settle_seconds"]:
                        raise soak.StopRun("highres_measurement_settle_timeout")
                    if measurement_ready(status, track, source, sid_hash, contract):
                        consecutive.append(status)
                    else:
                        consecutive = []
                    if len(consecutive) < 3 or len({(actual_layer(row["target"], source),
                            row["target"]["sink_frame_width"], row["target"]["sink_frame_height"])
                            for row in consecutive[-3:]}) != 1:
                        continue
                    candidate = hd.track_identity(track)
                    if identity_value is None:
                        identity_value = candidate
                    elif candidate != identity_value:
                        raise soak.StopRun("target_binding_changed_across_steps")
                    ready_at, rows = now, [status]
                    event("step_ready", step=index, actual_layer=actual_layer(track, source))
                    continue
                if track is None or hd.track_identity(track) != identity_value:
                    raise soak.StopRun("target_binding_changed_during_step")
                rows.append(status)
                if now-ready_at >= plan[index]["seconds"]:
                    result = evaluate_step(rows, plan[index], source, sid_hash, contract)
                    summary["step_results"].append(result)
                    summary["completed_steps"] += 1
                    event("step_finished", step=index, status=result["status"], reason=result["reason"])
                    # Policy/resolution/FPS failures retain evidence and proceed.
                    if index == len(plan)-1:
                        statuses = {result["status"] for result in summary["step_results"]}
                        summary.update(status="FAIL" if "FAIL" in statuses else "INCONCLUSIVE" if "INCONCLUSIVE" in statuses else "PASS",
                            reason="highres_steps_failed" if "FAIL" in statuses else "highres_evidence_incomplete" if "INCONCLUSIVE" in statuses else "highres_policy_layers_fps_resources_verified")
                        break
                    begin(index+1)
            else:
                raise soak.StopRun("probe_wall_timeout")
        except soak.StopRun as error:
            if isinstance(error, ObserverFailed):
                last_status = dict(error.safe_status)
                last_status.update(resource, elapsed_s=round(time.monotonic()-start, 3), step=index)
                tracks.write(json.dumps(last_status) + "\n")
                tracks.flush()
                summary["observer_error_code"] = error.safe_status["error_code"]
                event("observer_failed", error_code=error.safe_status["error_code"])
            summary.update(status="FAIL" if any(result["status"] == "FAIL" for result in summary["step_results"])
                else error.status, reason=error.reason)
        except (OSError, ValueError, RuntimeError) as error:
            safe_reason = publisher_probe.safe_error_reason(error) or "unclassified_measurement_error"
            summary["measurement_error_reason"] = safe_reason
            event("measurement_error", reason=safe_reason)
            summary.update(status="FAIL" if any(result["status"] == "FAIL" for result in summary["step_results"])
                else "INCONCLUSIVE", reason="measurement_chain_error")
        finally:
            if process and process.poll() is None:
                try:
                    send("stop")
                    process.wait(timeout=30)
                except (OSError, subprocess.TimeoutExpired):
                    process.kill()
                    process.wait(timeout=10)
                    summary.update(status="FAIL", reason="observer_forced_termination")
            if process:
                summary["exit_code"] = process.returncode
                if process.returncode != 0 and summary["status"] == "PASS":
                    summary.update(status="FAIL", reason="observer_exit_not_clean")
            if sampler:
                sampler.close()
            if sender_sampler:
                sender_sampler.close()
            if gpu_sampler:
                gpu_sampler.close()
            summary["client_resources"] = render.client_resource_summary(resources)
            summary["gpu_resources"] = gpu.gpu_resource_summary(resources)
            summary["gpu_required_scope"] = contract.get("gpu_required_scope", "NOT_REQUIRED")
            summary["gpu_measurement_steps_status"] = "AVAILABLE" if len(summary["step_results"]) == len(plan) and all(
                result.get("gpu_resources", {}).get("status") == "AVAILABLE" for result in summary["step_results"]) else "UNKNOWN"
            media_statuses = {result.get("media_status", "INCONCLUSIVE") for result in summary["step_results"]}
            summary["media_status"] = "FAIL" if "FAIL" in media_statuses else "PASS" if \
                len(summary["step_results"]) == len(plan) and media_statuses == {"PASS"} else "INCONCLUSIVE"
            if summary["client_resources"]["status"] != "AVAILABLE" and summary["status"] == "PASS":
                summary.update(status="INCONCLUSIVE", reason="client_resource_evidence_incomplete")
            if last_status:
                soak.atomic_json(output / "last-status.json", last_status)
            summary.update(wall_seconds=round(time.monotonic()-start, 3), finished_utc=soak.utc_now())
            event("run_finished", status=summary["status"], reason=summary["reason"])
    soak.archive_evidence(output, summary)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("output", "executable", "input-manifest", "profile"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    value = run(args.output, args.executable, input_manifest=args.input_manifest, profile=args.profile)
    print(json.dumps({key: value[key] for key in ("status", "reason", "completed_steps")}))
    return 0 if value["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
