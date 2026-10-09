"""Frozen 100-source/grid16 automatic-layer diagnostic; no formal B11 credit."""
from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

import b11_100_input_freeze as freeze
import b11_hd_layer_probe as hd
import meeting_render_probe as render
import meeting_soak as soak

MAX_STATUS_BYTES = 1024 * 1024
MAX_INBOUND_STREAMS = 400


def read_json(path):
    with Path(path).open("rb") as stream:
        data = stream.read(MAX_STATUS_BYTES + 1)
    if len(data) > MAX_STATUS_BYTES:
        raise soak.StopRun("100_status_size_exceeded")
    return json.loads(data)


def stream_key(item):
    return (item["stats_id_hash"], item["ssrc_hash"], item["report_index"])


def validate_status(raw, run_id, pid):
    if not isinstance(raw, dict):
        raise soak.StopRun("invalid_status")
    streams = raw.get("inbound_streams")
    if not isinstance(streams, list) or len(streams) > MAX_INBOUND_STREAMS:
        raise soak.StopRun("invalid_100_inbound_streams")
    # Reuse the closed validators in bounded chunks, retaining every stream.
    current = hd.validate_status({**raw, "inbound_streams": streams[:32]}, run_id, pid)
    for start in range(32, len(streams), 32):
        current["inbound_streams"].extend(hd.validate_status(
            {**raw, "inbound_streams": streams[start:start+32]}, run_id, pid)["inbound_streams"])
    keys = [stream_key(item) for item in current["inbound_streams"]]
    if len(keys) != len(set(keys)):
        raise soak.StopRun("duplicate_100_inbound_stream")
    for name in ("render_timer_active", "video_stage_visible", "canvas_visible", "renderer_ready"):
        if type(current.get(name)) is not bool:
            raise soak.StopRun("missing_100_render_state")
    for name in ("render_router_submitted", "render_delivered_to_gpu", "render_attached_tracks"):
        if type(current.get(name)) is not int:
            raise soak.StopRun("missing_100_render_counter")
    value = raw.get("selected_active_leases")
    if type(value) is not int or not 0 <= value <= 2**53:
        raise soak.StopRun("invalid_100_active_leases")
    current["selected_active_leases"] = value
    return current


def grid_signature(status):
    return sorted([track["sid_hash"], track["identity_hash"], *hd.track_identity(track)[1:],
        track["desired_quality"], track["sink_frame_width"], track["sink_frame_height"],
        track["stats_stream_hash"]] for track in status["selected_tracks"])


def ready(status, contract, allowed_identities):
    tracks = status["selected_tracks"]
    if not (status["state"] == "connected" and status["remote_video_count"] == 100 and
            status["selected"] == status["bound"] == status["requested"] == status["actual"] ==
            status["render_attached_tracks"] == status["selected_active_leases"] == len(tracks) == 16 and
            status["command_status"] == "applied" and
            status["selected_not_bound"] == status["bound_not_selected"] == 0 and
            status["page"] == 0 and status["page_size"] == 16 and status["page_count"] == 7 and
            status["layout_mode"] == "grid" and not status["pinned_sid_hash"] and
            status["render_timer_active"] and status["video_stage_visible"] and
            status["canvas_visible"] and status["renderer_ready"] and
            status["stats_age_ms"] <= contract["maximum_frame_age_ms"]):
        return False
    identities = {item["identity_hash"] for item in tracks}
    if len(identities) != 16 or not identities <= set(allowed_identities):
        return False
    for track in tracks:
        quality = track["desired_quality"]
        if quality not in ("low", "medium"):
            return False
        tier = contract[quality]
        geometry = (track["sink_frame_width"], track["sink_frame_height"])
        if not (geometry[0] == tier["width"] and geometry[1] in tier["heights"] and
                track["render_frame_dimensions_available"] and track["sink_frame_dimensions_available"] and
                (track["render_frame_width"], track["render_frame_height"]) == geometry and
                0 <= track["sink_frame_age_ms"] <= contract["maximum_frame_age_ms"] and
                track["sink_active"] and track["sink_binding_count"] == 1 and
                track["current_binding_serial"] > 0 and
                track["current_binding_serial"] == track["sink_binding_serial"] and
                track["rtc_track_hash"] and track["rtc_track_hash"] == track["binding_rtc_track_hash"] and
                track["intent_present"] and track["intent_subscribed"] and
                track["desired_enabled"] is True and track["publication_subscribed"] and
                not track["subscription_error"] and track["seat_present"] and
                track["seat_role"] == "grid" and not track["focused"] and
                track["seat_quality"] == {"low": "p180", "medium": "p360"}[quality] and
                track["stats_match_count"] >= 1 and
                all(track[name] for name in ("stats_bytes_available", "stats_packets_available", "stats_decoded_available"))):
            return False
    return True


def freeze_grid(status):
    return {"signature": grid_signature(status), "command_seq": status["command_seq"], "tracks": [{
        "sid_hash": t["sid_hash"], "identity_hash": t["identity_hash"],
        "quality": t["desired_quality"], "width": t["sink_frame_width"],
        "height": t["sink_frame_height"], "binding": list(hd.track_identity(t)),
        "source_assignment": "UNKNOWN_CONCURRENT_PUBLISH_ORDER"}
        for t in sorted(status["selected_tracks"], key=lambda t: t["sid_hash"])]}


def evaluate_invisible(rows):
    """Stationary historical stats are harmless; positive unmapped RTP is a gap."""
    leaks, unknown, unavailable, resets, unbaselined = set(), set(), set(), set(), set()
    intervals = 0
    for before, after in zip(rows, rows[1:]):
        if before["stats_sample_seq"] == after["stats_sample_seq"]:
            continue
        intervals += 1
        previous = {stream_key(t): t for t in before["inbound_streams"]}
        selected = {t["sid_hash"] for t in after["selected_tracks"]}
        for item in after["inbound_streams"]:
            key = stream_key(item)
            old = previous.get(key)
            if old is None:
                # A new report has no delta baseline; a following report closes it.
                unbaselined.add(key)
                continue
            unbaselined.discard(key)
            if not all(item[n+"_available"] and old[n+"_available"] for n in ("bytes", "packets")):
                unavailable.add(key)
                continue
            deltas = [item[n]-old[n] for n in ("bytes", "packets")]
            if min(deltas) < 0:
                resets.add(key)
            if max(deltas) <= 0:
                continue
            if item["mapped_binding_current"] and item["mapped_sink_active"] and item["mapped_sid_hash"]:
                if item["mapped_sid_hash"] not in selected:
                    leaks.add(key)
            else:
                unknown.add(key)
    status = "FAIL" if leaks else "INCONCLUSIVE" if unknown or unavailable or resets or unbaselined or intervals < 2 else "PASS"
    return {"status": status, "reason": "invisible_current_rtp_progress" if leaks else
        "rtp_attribution_gap" if unknown else "rtp_counter_gap" if unavailable or resets or unbaselined else
        "insufficient_stats_intervals" if intervals < 2 else "only_selected_current_streams_progressed",
        "distinct_stats_intervals": intervals, "nonselected_active_streams": len(leaks),
        "unmapped_progressing_streams": len(unknown), "unavailable_streams": len(unavailable),
        "counter_reset_streams": len(resets), "unbaselined_final_streams": len(unbaselined)}


def evaluate_grid(rows, frozen_grid, contract, allowed_identities):
    result = {"status": "INCONCLUSIVE", "reason": "insufficient_samples", "sample_count": len(rows)}
    if len(rows) < 3:
        return result
    duration = rows[-1]["elapsed_s"] - rows[0]["elapsed_s"]
    result["seconds"] = duration
    if duration < contract["minimum_observation_seconds"]:
        return result
    failures = []
    if any(b["elapsed_s"]-a["elapsed_s"] > contract["maximum_sample_gap_seconds"] for a,b in zip(rows, rows[1:])):
        failures.append("sample_gap")
    valid = [ready(row, contract, allowed_identities) for row in rows]
    result["dimension_and_state_match_ratio"] = sum(valid)/len(valid)
    if result["dimension_and_state_match_ratio"] < contract["dimension_match_ratio"]:
        failures.append("dimensions_or_grid_state_mismatch")
    if any(grid_signature(row) != frozen_grid["signature"] for row in rows):
        failures.append("grid_binding_geometry_or_quality_changed")
    if any(row["command_seq"] != frozen_grid["command_seq"] or row["command_status"] != "applied" for row in rows):
        failures.append("grid_command_ack_changed")
    per_track = []
    for frozen_track in frozen_grid["tracks"]:
        sid = frozen_track["sid_hash"]
        tracks = [next((t for t in row["selected_tracks"] if t["sid_hash"] == sid), None) for row in rows]
        if any(t is None for t in tracks):
            failures.append("selected_track_disappeared")
            continue
        measured = {**frozen_track}
        for field in ("sink_on_frame_count", "sink_delivered_frame_count", "stats_bytes", "stats_packets", "stats_decoded"):
            values = [t[field] for t in tracks]
            measured[field+"_delta"] = values[-1]-values[0]
            if any(b<a for a,b in zip(values, values[1:])) or values[-1] <= values[0]:
                failures.append(field+"_not_progressing")
        measured.update(native_fps=measured["sink_on_frame_count_delta"]/duration,
            decoded_fps=measured["stats_decoded_delta"]/duration,
            delivered_fps=measured["sink_delivered_frame_count_delta"]/duration,
            minimum_fps=contract[frozen_track["quality"]]["source_fps"]*contract["minimum_frame_rate_ratio"],
            maximum_frame_age_ms=max(t["sink_frame_age_ms"] for t in tracks))
        if min(measured["native_fps"], measured["decoded_fps"], measured["delivered_fps"]) < measured["minimum_fps"]:
            failures.append("track_fps_below_frozen_minimum")
        per_track.append(measured)
    for field in ("render_submits", "render_router_submitted", "render_delivered_to_gpu"):
        values = [row[field] for row in rows]
        result[field+"_delta"] = values[-1]-values[0]
        if any(b<a for a,b in zip(values, values[1:])) or values[-1] <= values[0]:
            failures.append(field+"_not_progressing")
    invisible = evaluate_invisible(rows)
    result.update(tracks=per_track, invisible_subscription=invisible)
    if invisible["status"] == "FAIL":
        failures.append(invisible["reason"])
    result.update(status="FAIL" if failures else invisible["status"],
        reason=",".join(sorted(set(failures))) if failures else "100_sources_grid16_automatic_layers_verified"
            if invisible["status"] == "PASS" else invisible["reason"])
    return result


def run(output: Path, executable: Path, *, input_manifest: Path, profile: Path) -> dict:
    frozen = freeze.verify_inputs(input_manifest, executable, profile, require_remote=True,
                                  service_url=os.environ.get("LIVEKIT_URL"))
    if not all(os.environ.get(name) for name in ("LIVEKIT_URL", "LIVEKIT_SOAK_TOKEN")):
        raise ValueError("service_environment_missing")
    settings = frozen["inputs"]["profile"]
    contract, probe = settings["quality_contract"], settings["probe"]
    allowed = {hashlib.sha256(identity.encode("utf-8")).hexdigest()[:16]
               for identity in freeze.publisher_identities(settings)}
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    run_id = uuid.uuid4().hex
    summary = {"schema": 1, "run_id": run_id, "kind": freeze.SCOPE,
        "status": "INCONCLUSIVE", "reason": "not_started", "diagnostic_only": True,
        "release_eligible": False, "formal_b11_status": "NOT_RUN", "formal_soak_status": "NOT_RUN",
        "actual_rid": "UNKNOWN_NOT_EXPOSED"}
    soak.atomic_json(output/"run.json", {"schema": 1, "run_id": run_id, "kind": freeze.SCOPE,
        "started_utc": soak.utc_now(), "build_configuration": "RelWithDebInfo",
        "binary_identity": frozen["inputs"]["binary_identity"],
        "input_manifest_sha256": soak.sha256(input_manifest), "profile_sha256": soak.sha256(profile),
        "receiver_arguments": probe["receiver_arguments"], "formal_b11_status": "NOT_RUN", "release_eligible": False})
    soak.atomic_json(output/"profile.json", settings)
    soak.atomic_json(output/"plan.json", {"action": "grid16", "publication_count": 100,
        "observe_seconds": probe["observe_seconds"], "quality": "AUTOMATIC_GRID16",
        "invisible_rule": "Positive bytes/packets delta outside current selected mappings is FAIL; unmapped progress is INCONCLUSIVE; stationary historical reports are allowed."})
    if not soak.desktop_available():
        summary["reason"] = "interactive_desktop_unavailable"
        soak.archive_evidence(output, summary)
        return summary
    process = sampler = None
    start = time.monotonic()
    resources, rows, consecutive = [], [], []
    heartbeat, seq = -1, 0
    sent_at = ready_at = last_status = frozen_grid = None
    fields = ("elapsed_s", "heartbeat_seq", "remote_video_count", "selected", "bound", "actual",
        "render_submits", "render_router_submitted", "render_delivered_to_gpu", "private_bytes",
        "working_set_bytes", "handles", *render.RESOURCE_FIELDS)
    with (output/"events.jsonl").open("w", encoding="utf-8") as events, \
         (output/"tracks.jsonl").open("w", encoding="utf-8") as tracks_file, \
         (output/"metrics.csv").open("w", newline="", encoding="utf-8") as metrics:
        writer = csv.DictWriter(metrics, fieldnames=fields)
        writer.writeheader()
        def event(name, **values):
            events.write(json.dumps({"elapsed_s": round(time.monotonic()-start,3), "event": name, **values})+"\n")
            events.flush()
        def send(action):
            nonlocal seq
            seq += 1
            soak.atomic_json(output/"command.json", {"schema": 1, "run_id": run_id, "seq": seq, "action": action})
            event("command_sent", seq=seq, action=action)
        try:
            process = subprocess.Popen([str(executable), *probe["receiver_arguments"], "--meeting-soak",
                "--soak-directory", str(output)], cwd=soak.ROOT, env=os.environ.copy(), stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
            sampler = render.ClientResourceSampler(process.pid)
            resources.append(sampler.sample())
            event("process_started", pid=process.pid)
            while time.monotonic()-start < probe["maximum_wall_seconds"]:
                time.sleep(1)
                now = time.monotonic()
                if process.poll() is not None:
                    raise soak.StopRun("observer_exited_early")
                resource = sampler.sample(now)
                resources.append(resource)
                try:
                    status = validate_status(read_json(output/"status.json"), run_id, process.pid)
                except FileNotFoundError:
                    if now-start > 15:
                        raise soak.StopRun("observer_status_missing", "INCONCLUSIVE")
                    continue
                if status["heartbeat_seq"] <= heartbeat:
                    if last_status and now-start-last_status["elapsed_s"] > contract["maximum_sample_gap_seconds"]:
                        raise soak.StopRun("observer_heartbeat_stale", "INCONCLUSIVE")
                    continue
                heartbeat = status["heartbeat_seq"]
                status.update(resource, elapsed_s=round(now-start,3))
                last_status = status
                tracks_file.write(json.dumps(status)+"\n")
                tracks_file.flush()
                writer.writerow({key:status.get(key) for key in fields})
                metrics.flush()
                if status["state"] == "failed":
                    raise soak.StopRun("observer_failed")
                if sent_at is None:
                    if status["state"]=="connected" and status["remote_video_count"]==100:
                        send("grid16")
                        sent_at = now
                    elif now-start > 90:
                        raise soak.StopRun("100_source_connection_timeout")
                    continue
                if status["command_seq"]==seq and status["command_status"]=="rejected":
                    raise soak.StopRun("grid16_command_rejected")
                if ready_at is None:
                    if now-sent_at > probe["settle_seconds"]:
                        raise soak.StopRun("100_grid_settle_timeout")
                    if status["command_seq"]==seq and status["command_status"]=="applied" and ready(status,contract,allowed):
                        if consecutive and grid_signature(consecutive[-1]) != grid_signature(status):
                            consecutive = []
                        consecutive.append(status)
                    else:
                        consecutive = []
                    if len(consecutive)<3:
                        continue
                    ready_at, rows, frozen_grid = now, [status], freeze_grid(status)
                    soak.atomic_json(output/"selected-grid-freeze.json", frozen_grid)
                    event("grid_ready", selected=16, remote_video_count=100)
                    continue
                rows.append(status)
                if now-ready_at >= probe["observe_seconds"]:
                    result = evaluate_grid(rows, frozen_grid, contract, allowed)
                    summary.update(result)
                    break
            else:
                raise soak.StopRun("100_probe_wall_timeout")
        except soak.StopRun as error:
            summary.update(status=error.status, reason=error.reason)
        except (OSError, ValueError, RuntimeError):
            summary.update(status="INCONCLUSIVE", reason="measurement_chain_error")
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
                if process.returncode != 0 and summary["status"]=="PASS":
                    summary.update(status="FAIL", reason="observer_exit_not_clean")
            if sampler:
                sampler.close()
            summary["client_resources"] = render.client_resource_summary(resources)
            if frozen_grid:
                summary["selected_grid_freeze"] = frozen_grid
            if summary["client_resources"]["status"]!="AVAILABLE" and summary["status"]=="PASS":
                summary.update(status="INCONCLUSIVE", reason="client_resource_evidence_incomplete")
            if last_status:
                soak.atomic_json(output/"last-status.json", last_status)
            summary.update(wall_seconds=round(time.monotonic()-start,3), finished_utc=soak.utc_now())
            event("run_finished", status=summary["status"], reason=summary["reason"])
    soak.archive_evidence(output, summary)
    return summary


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--input-manifest", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    args = parser.parse_args()
    result = run(args.output, args.executable.resolve(), input_manifest=args.input_manifest, profile=args.profile)
    print(json.dumps(result))
    raise SystemExit(0 if result["status"]=="PASS" else 2)
