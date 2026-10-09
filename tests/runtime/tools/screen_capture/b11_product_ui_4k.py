"""Real Qt sharing controls, owned WGC window and independent native receiver."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tests/runtime/tools/meeting"))
import b11_input_freeze as inputs
import b11_highres_run as remote_run
import meeting_soak as soak

SCOPE = "B11_PRODUCT_UI_4K_DIAGNOSTIC_V2"
TARGET = {
    "instance": "ssh:43.138.244.33", "service_url": "ws://43.138.244.33:17880",
    "local_service_url": "http://127.0.0.1:17880",
    "config_path": "/home/ubuntu/openmeeting-server/components/livekit/config/livekit.yml",
    "sfu_container": "livekit", "expected_vcpus": 16, "expected_memory_gib": 64,
    "minimum_public_egress_mbps": 20,
}
EXPECTED_REMOTE = {
    "config_sha256": "ce6497eb0e0d0b385e7ac689ae5643aeb52a9238f09f92c6594ecc6487b5b69f",
    "sfu_image": "sha256:d0c04791bf63ca8dcea123571827d56ba9504bd57c8d5de60ee03b5408cc3154",
}
STEPS = (("start_4k", "ui_start", 5, 3840, 2160),
         ("switch_1080p", "ui_switch_quality", 2, 1920, 1080),
         ("return_4k", "ui_switch_quality", 5, 3840, 2160))
FPS, MEASURE_SECONDS, SETTLE_SECONDS = 20, 30, 10
MINIMUM_FPS = FPS * 0.9
SAME_FIELDS = ("same_sid", "same_track", "same_rtc_track", "same_source", "same_preview")
QUIET_SECONDS, QUIET_MAXIMUM_GAP_SECONDS = 2, 1
QUIET_COUNTERS = {"publisher": ("capture_frames",),
                  "receiver": ("frames", "callback_frames", "post_detach_callbacks")}


class RunFailure(Exception):
    pass


def require(condition, code):
    if not condition:
        raise RunFailure(code)


def binary_identity(executable):
    verifier = ROOT / "tests/runtime/tools/diagnostics/verify_runtime_binary.ps1"
    completed = subprocess.run(["pwsh", "-NoProfile", "-File", str(verifier),
        "-Executable", str(executable), "-ExpectedExecutableName", executable.name,
        "-Configuration", "RelWithDebInfo"], capture_output=True, text=True,
        timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
    require(completed.returncode == 0, "binary_configuration_not_verified")
    value = json.loads(completed.stdout)
    require(value["configuration"] == "RelWithDebInfo" and
            value["binary_sha256"] == inputs.sha256(executable), "binary_fingerprint_mismatch")
    return value


def input_records(publisher, receiver):
    paths = set(inputs._source_paths(ROOT))
    paths.update((Path(__file__).resolve(), ROOT / "tests/test_settings_async.cpp",
                  ROOT / "tests/security/test_session_credentials.cpp",
                  ROOT / "tests/media/screen_share/test_screen_share_quality.cpp",
                  ROOT / "tests/media/screen_share/test_screen_share_session.cpp"))
    paths.update((ROOT / "tests/runtime/tools/meeting").glob("*.py"))
    paths.add(ROOT / "tests/runtime/tools/diagnostics/verify_runtime_binary.ps1")
    for executable in (publisher, receiver):
        paths.update((executable, executable.with_suffix(".pdb")))
        paths.update(executable.parent.glob("*.dll"))
        paths.add(ROOT / "build-debug/tests" / (executable.stem + ".vcxproj"))
    paths.add(ROOT / "build-debug/CMakeCache.txt")
    return [inputs._file_record(path) for path in sorted(paths)]


def read_status(directory, process, room=None):
    require(process.poll() is None, "media_process_exited_early")
    path = directory / "status.json"
    if not path.is_file():
        return None
    value = inputs._load_json(path)
    require(value.get("schema") == 1 and value.get("pid") == process.pid,
            "status_process_identity_mismatch")
    if room is not None:
        require(value.get("run_id") == room, "status_run_identity_mismatch")
        require(not value.get("error_code"), "product_control_error")
    stamp = value.get("utc_ms")
    require(type(stamp) in (int, float) and -0.25 <= time.time() - stamp / 1000 <= 3,
            "status_stale_or_future")
    return value


def wait_for(predicate, code, seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.2)
    raise RunFailure(code)


def active_profile(value, resolution, width, height):
    return value and value.get("share_state") == 2 and value.get("quality_status") == 0 and \
        value.get("resolution") == resolution and value.get("fps") == FPS and \
        (value.get("applied_width"), value.get("applied_height")) == (width, height)


def evaluate_step(samples, resolution, width, height):
    require(len(samples) >= 30, "measurement_samples_missing")
    frame_key = "frames_4k" if width == 3840 else "frames_1080p"
    for row in samples:
        publisher, receiver = row["publisher"], row["receiver"]
        require(active_profile(publisher, resolution, width, height), "product_profile_drift")
        require(publisher.get("capture_backend") == "wgc" and
                (publisher.get("source_width"), publisher.get("source_height")) == (3840, 2160) and
                (publisher.get("source_client_width"), publisher.get("source_client_height")) == (3840, 2160) and
                publisher.get("owned_source_matches") == 1 and publisher.get("capture_failures") == 0,
                "owned_wgc_source_not_verified")
        require(all(publisher.get(key) is True for key in SAME_FIELDS), "publisher_identity_changed")
        require(receiver.get("connected") is True and receiver.get("attached") is True and
                receiver.get("dimensions_request_accepted") is True and
                receiver.get("same_sid") is True and receiver.get("same_track") is True and
                receiver.get("subscriptions") == 1 and receiver.get("removed") == 0 and
                receiver.get("unsubscribed") == 0, "receiver_binding_changed")
        require((receiver.get("width"), receiver.get("height")) == (width, height) and
                0 <= receiver.get("frame_age_ms", -1) <= 1000,
                "received_dimensions_or_freshness_failed")
        require(publisher.get("sid_hash") == receiver.get("sid_hash") and bool(receiver.get("sid_hash")),
                "publication_correlation_failed")
    first, last = samples[0], samples[-1]
    for role, counters in (("publisher", ("capture_frames", "source_frames", "preview_frames")),
                           ("receiver", ("frames", "frames_4k", "frames_1080p"))):
        for previous, current in zip(samples, samples[1:]):
            delta = current[role]["elapsed_s"] - previous[role]["elapsed_s"]
            require(0 < delta <= 2, "measurement_time_gap_or_regression")
            require(all(current[role][key] >= previous[role][key] for key in counters),
                    "measurement_counter_regression")
    duration = last["receiver"]["elapsed_s"] - first["receiver"]["elapsed_s"]
    source_duration = last["publisher"]["elapsed_s"] - first["publisher"]["elapsed_s"]
    require(duration >= MEASURE_SECONDS and source_duration >= MEASURE_SECONDS,
            "measurement_window_incomplete")
    frame_delta = last["receiver"]["frames"] - first["receiver"]["frames"]
    expected_delta = last["receiver"][frame_key] - first["receiver"][frame_key]
    require(frame_delta == expected_delta and frame_delta > 0, "unexpected_frame_dimensions_in_window")
    source_delta = last["publisher"]["source_frames"] - first["publisher"]["source_frames"]
    capture_delta = last["publisher"]["capture_frames"] - first["publisher"]["capture_frames"]
    preview_delta = last["publisher"]["preview_frames"] - first["publisher"]["preview_frames"]
    fps = frame_delta / duration
    require(fps >= MINIMUM_FPS and source_delta / source_duration >= MINIMUM_FPS and
            capture_delta / source_duration >= MINIMUM_FPS and preview_delta > 0,
            "frame_rate_below_frozen_threshold")
    return {"status": "PASS", "resolution": resolution, "width": width, "height": height,
            "received_decoded_fps": fps, "capture_delivery_fps": capture_delta / source_duration,
            "video_source_input_fps": source_delta / source_duration,
            "receiver_frames_delta": frame_delta, "expected_dimension_frames_delta": expected_delta,
            "seconds": duration, "publisher_seconds": source_duration, "sample_count": len(samples),
            "sid_hash": last["receiver"]["sid_hash"], "same_track_source_preview": True,
            "exact_capture_instance_identity": "UNKNOWN", "gpu_performance_status": "NOT_EVALUATED"}


def quiet_observation(samples):
    """Validate a progressing quiet window, including callbacks rejected after detach."""
    require(bool(samples), "quiet_measurement_samples_missing")
    for row in samples:
        require(type(row) is dict and all(type(row.get(role)) is dict
                for role in QUIET_COUNTERS), "quiet_measurement_status_missing")
        publisher, receiver = row["publisher"], row["receiver"]
        require(publisher.get("local_share_tracks") == 0 and publisher.get("share_state") == 0,
                "local_share_not_stopped")
        require(receiver.get("attached") is False and receiver.get("removed") == 1,
                "quiet_receiver_binding_changed")
        for role, sequence in (("publisher", "heartbeat_seq"), ("receiver", "sample_seq")):
            value = row[role]
            for key in ("utc_ms", "elapsed_s", sequence, *QUIET_COUNTERS[role]):
                number = value.get(key)
                require(type(number) in (int, float) and math.isfinite(number) and number >= 0,
                        "quiet_measurement_field_invalid")
                if key not in ("utc_ms", "elapsed_s"):
                    require(number == int(number), "quiet_measurement_field_invalid")
    result = {"schema": 1, "scope": SCOPE, "minimum_seconds": QUIET_SECONDS,
              "maximum_gap_seconds": QUIET_MAXIMUM_GAP_SECONDS, "sample_count": len(samples),
              "publisher": {}, "receiver": {},
              "counter_semantics": {
                  "capture_frames": "production capture delivery entry count",
                  "frames": "receiver I420 frames accepted while attached",
                  "callback_frames": "all receiver I420 callback entries, including after detach",
                  "post_detach_callbacks": "receiver I420 callback entries observed while detached"},
              "scope_boundary": "counter delta is zero in this measured window; prior callbacks remain recorded"}
    for role, sequence in (("publisher", "heartbeat_seq"), ("receiver", "sample_seq")):
        values = [row[role] for row in samples]
        first, last = values[0], values[-1]
        gaps = []
        utc_gaps = []
        for previous, current in zip(values, values[1:]):
            delta = current["elapsed_s"] - previous["elapsed_s"]
            utc_delta = (current["utc_ms"] - previous["utc_ms"]) / 1000
            require(current[sequence] > previous[sequence] and delta > 0 and utc_delta > 0,
                    "quiet_status_did_not_advance")
            require(delta <= QUIET_MAXIMUM_GAP_SECONDS and utc_delta <= QUIET_MAXIMUM_GAP_SECONDS,
                    "quiet_measurement_time_gap")
            gaps.append(delta); utc_gaps.append(utc_delta)
        for key in QUIET_COUNTERS[role]:
            require(all(value[key] == first[key] for value in values), "frames_after_share_stop")
        result[role] = {
            "elapsed_seconds": last["elapsed_s"] - first["elapsed_s"],
            "utc_seconds": (last["utc_ms"] - first["utc_ms"]) / 1000,
            "maximum_elapsed_gap_seconds": max(gaps, default=0),
            "maximum_utc_gap_seconds": max(utc_gaps, default=0),
            "first_utc_ms": first["utc_ms"], "last_utc_ms": last["utc_ms"],
            "first_elapsed_s": first["elapsed_s"], "last_elapsed_s": last["elapsed_s"],
            "sequence_field": sequence, "first_sequence": first[sequence], "last_sequence": last[sequence],
            "counter_baseline": {key: first[key] for key in QUIET_COUNTERS[role]},
            "counter_last": {key: last[key] for key in QUIET_COUNTERS[role]},
            "counter_delta": {key: last[key] - first[key] for key in QUIET_COUNTERS[role]}}
    return result


def evaluate_quiet(samples):
    result = quiet_observation(samples)
    require(all(result[role][key] >= QUIET_SECONDS for role in QUIET_COUNTERS
                for key in ("elapsed_seconds", "utc_seconds")), "quiet_measurement_window_incomplete")
    result["status"] = "PASS"
    return result


def run(output, publisher, receiver, target_config):
    output.mkdir(parents=True, exist_ok=False)
    pubdir, recdir, owned_dir = output / "publisher", output / "receiver", output / "owned-window"
    pubdir.mkdir(); recdir.mkdir(); owned_dir.mkdir()
    result = {"schema": 1, "scope": SCOPE, "status": "FAIL", "diagnostic_only": True,
              "release_eligible": False, "formal_b11_status": "NOT_RUN", "steps": [],
              "configuration": "RelWithDebInfo", "cleanup_issues": []}
    session = None
    processes, streams, sequence = [], [], 0
    owned_process = None
    records = None
    try:
        identities = {"publisher": binary_identity(publisher), "receiver": binary_identity(receiver)}
        records = input_records(publisher, receiver)
        records.append(inputs._file_record(target_config))
        owned_stream = (owned_dir / "raw-output.tmp").open("wb"); streams.append(owned_stream)
        owned_process = subprocess.Popen([str(publisher), "--share4k-owned-window", "--share4k-directory", str(owned_dir)],
            cwd=ROOT, stdout=owned_stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        def owned_ready():
            value = read_status(owned_dir, owned_process)
            if not value:
                return False
            require(value.get("client_width") == 3840 and value.get("client_height") == 2160,
                    "owned_window_client_not_4k")
            require(type(value.get("hwnd")) is str and value["hwnd"].isascii() and
                    value["hwnd"].isdecimal() and int(value["hwnd"]) > 0 and
                    type(value.get("title")) is str and value["title"].startswith("Cohavora share4k owned animation "),
                    "owned_window_identity_not_verified")
            return value
        owned = wait_for(owned_ready, "owned_window_not_ready", 20)
        session = remote_run.prepare_credentials(target_config, TARGET, output=output / "prepared",
            expected_remote=EXPECTED_REMOTE, scenario="screen4k")
        room = session["task"]["room"]
        soak.atomic_json(pubdir / "run.json", {"schema": 1, "run_id": room})
        manifest = {"schema": 1, "scope": SCOPE, "status": "FROZEN", "head": inputs._git_head(ROOT),
            "configuration": "RelWithDebInfo", "binary_identities": identities, "files": records,
            "remote_inputs": session["remote_inputs"], "target": TARGET, "task": session["task"],
            "owned_window": {key: owned[key] for key in ("pid", "hwnd", "title", "client_width", "client_height")},
            "contract": {"steps": STEPS, "fps": FPS, "minimum_fps": MINIMUM_FPS,
                "measurement_seconds": MEASURE_SECONDS, "settle_seconds": SETTLE_SECONDS,
                "quiet_seconds": QUIET_SECONDS, "quiet_maximum_gap_seconds": QUIET_MAXIMUM_GAP_SECONDS,
                "quiet_progress": "publisher heartbeat_seq and receiver sample_seq, utc_ms and elapsed_s strictly advance",
                "quiet_counter_semantics": "capture_frames and accepted receiver frames remain unchanged; all receiver I420 callback entries and post-detach entries have zero delta over the measured window",
                "source": "owned 3840x2160 native HWND via production WGC",
                "ui": "Qt QAccessible buttons and real QComboBox selections",
                "exact_capture_instance_identity": "UNKNOWN", "formal_b11_status": "NOT_RUN"}}
        soak.atomic_json(output / "input-freeze.json", manifest)
        for role, binary, arguments, directory in (
            ("publisher", publisher, ["--share4k-product-ui", "--share4k-directory", str(pubdir), "--language", "en_US"], pubdir),
            ("observer", receiver, ["--product-ui-observer", str(recdir)], recdir)):
            credentials = session["credentials"][role]
            environment = os.environ.copy()
            for name in tuple(environment):
                if name.startswith("LIVEKIT_TEST_") or name.startswith("POLICY_SOURCE_"):
                    environment.pop(name, None)
            environment.update(LIVEKIT_URL=credentials["LIVEKIT_URL"], LIVEKIT_TEST_ALLOW_INSECURE="1",
                SHARE4K_ROOM=room, SHARE4K_SOURCE_HWND=str(owned["hwnd"]), SHARE4K_SOURCE_TITLE=owned["title"])
            environment["LIVEKIT_TOKEN" if role == "publisher" else "RECEIVER_TOKEN"] = credentials["LIVEKIT_SOAK_TOKEN"]
            stream = (directory / "raw-output.tmp").open("wb"); streams.append(stream)
            processes.append(subprocess.Popen([str(binary), *arguments], cwd=ROOT, env=environment,
                stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW))
        pub, rec = processes
        def get_pub():
            owned_ready()
            value = read_status(pubdir, pub, room)
            if value:
                require(value.get("owned_window_pid") == owned_process.pid,
                        "publisher_owned_window_process_mismatch")
            return value
        get_rec = lambda: read_status(recdir, rec)
        wait_for(lambda: (p := get_pub()) and p.get("state") == 5 and p.get("share_state") == 0 and p,
                 "product_meeting_not_ready")
        wait_for(lambda: (r := get_rec()) and r.get("connected") is True and r,
                 "independent_receiver_not_connected")
        def command(action, resolution=None):
            nonlocal sequence
            sequence += 1
            value = {"schema": 1, "run_id": room, "seq": sequence, "action": action}
            if resolution is not None:
                value.update(resolution=resolution, fps=FPS)
            soak.atomic_json(pubdir / "command.json", value)
            def acknowledged():
                p = get_pub()
                if not p or p.get("command_seq") != sequence:
                    return False
                require(p.get("command_status") != "rejected", "ui_command_rejected")
                return p if p.get("command_status") == "applied" else False
            return wait_for(acknowledged, "ui_command_timeout", 65)
        for name, action, resolution, width, height in STEPS:
            command(action, resolution)
            wait_for(lambda: (r := get_rec()) and (r.get("width"), r.get("height")) == (width, height) and
                     r.get("attached") is True and r, "expected_receiver_dimensions_missing")
            settle_deadline = time.monotonic() + SETTLE_SECONDS
            while time.monotonic() < settle_deadline:
                get_pub(); get_rec(); time.sleep(0.5)
            rows = []
            with (output / (name + "-samples.jsonl")).open("x", encoding="utf-8") as stream:
                while True:
                    pair = {"publisher": get_pub(), "receiver": get_rec()}
                    require(all(pair.values()), "measurement_status_missing")
                    rows.append(pair); stream.write(json.dumps(pair) + "\n"); stream.flush()
                    if (pair["receiver"]["elapsed_s"] - rows[0]["receiver"]["elapsed_s"] >= MEASURE_SECONDS and
                        pair["publisher"]["elapsed_s"] - rows[0]["publisher"]["elapsed_s"] >= MEASURE_SECONDS):
                        break
                    time.sleep(0.5)
            stage = evaluate_step(rows, resolution, width, height)
            snapshot = remote_run.server_snapshot(session)
            soak.atomic_json(output / (name + "-server.json"), snapshot)
            require(snapshot["target_track_count"] == 1 and snapshot["tracks"][0]["source"] == "screen_share" and
                    snapshot["tracks"][0]["sid_hash"] == stage["sid_hash"] and
                    (snapshot["tracks"][0]["width"], snapshot["tracks"][0]["height"]) == (width, height),
                    "server_publication_metadata_mismatch")
            stage["name"] = name; stage["server_metadata_status"] = "MATCH"
            result["steps"].append(stage)
            print(json.dumps({"event": "step_completed", **stage}), flush=True)
        command("ui_stop")
        wait_for(lambda: (r := get_rec()) and r.get("attached") is False and r.get("removed") == 1 and r,
                 "remote_publication_not_removed")
        quiet_rows = []
        with (output / "stop-quiet-samples.jsonl").open("x", encoding="utf-8") as stream:
            while True:
                pair = {"publisher": get_pub(), "receiver": get_rec()}
                quiet_rows.append(pair)
                stream.write(json.dumps(pair) + "\n"); stream.flush()
                observation = quiet_observation(quiet_rows)
                if all(observation[role][key] >= QUIET_SECONDS for role in QUIET_COUNTERS
                       for key in ("elapsed_seconds", "utc_seconds")):
                    break
                time.sleep(0.5)
        quiet = evaluate_quiet(quiet_rows)
        soak.atomic_json(output / "stop-quiet.json", quiet)
        result["stop_quiet"] = quiet
        snapshot = remote_run.server_snapshot(session)
        soak.atomic_json(output / "server-stopped.json", snapshot)
        require(snapshot["target_track_count"] == 0, "server_share_publication_after_stop")
        result.update(status="PASS", reason="actual_product_ui_wgc_publish_switch_receive_stop_verified",
                      stop_status="PASS", task=session["task"])
    except RunFailure as error:
        result["reason"] = str(error)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        result["reason"] = "product_ui_execution_or_measurement_failed"
    finally:
        if processes:
            if processes[0].poll() is None:
                sequence += 1
                soak.atomic_json(pubdir / "command.json", {"schema": 1, "run_id": session["task"]["room"],
                    "seq": sequence, "action": "stop"})
            (recdir / "observer.stop").write_text("stop\n", encoding="ascii")
            for process in processes:
                try:
                    process.wait(timeout=35)
                except subprocess.TimeoutExpired:
                    process.terminate(); process.wait(timeout=10)
                    result["cleanup_issues"].append("task_media_process_forced_termination")
                if process.returncode != 0:
                    result["cleanup_issues"].append("task_media_process_exit_not_clean")
            result["exit_codes"] = [process.returncode for process in processes]
            for directory in (pubdir, recdir):
                if (directory / "final.json").is_file():
                    result[directory.name + "_final"] = inputs._load_json(directory / "final.json")
            if result["status"] == "PASS":
                if not all(result.get("publisher_final", {}).get(key) is True for key in
                           ("room_retired", "shutdown_acknowledged", "callback_context_detached", "webrtc_deinitialized")) or \
                        not result.get("receiver_final", {}).get("finished"):
                    result["cleanup_issues"].append("task_room_retirement_not_verified")
        for stream in streams:
            stream.close()
        if owned_process is not None:
            (owned_dir / "owned.stop").write_text("stop\n", encoding="ascii")
            try:
                owned_process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                owned_process.terminate(); owned_process.wait(timeout=10)
                result["cleanup_issues"].append("owned_window_forced_termination")
            result["owned_window_exit_code"] = owned_process.returncode
            if owned_process.returncode != 0:
                result["cleanup_issues"].append("owned_window_exit_not_clean")
            if (owned_dir / "final.json").is_file():
                result["owned_window_final"] = inputs._load_json(owned_dir / "final.json")
            if result["status"] == "PASS" and result.get("owned_window_final", {}).get("window_closed") is not True:
                result["cleanup_issues"].append("owned_window_close_not_verified")
        for directory in (pubdir, recdir, owned_dir):
            (directory / "raw-output.tmp").unlink(missing_ok=True)
        if session is not None:
            session.pop("credentials", None)
            try:
                cleanup = remote_run.session_cleanup(session)
                soak.atomic_json(output / "remote-cleanup.json", cleanup)
                require(all(cleanup.get(key) is True for key in ("all_stopped", "room_removed", "credential_removed")),
                        "remote_cleanup_incomplete")
            except (RunFailure, OSError, ValueError, RuntimeError):
                result["cleanup_issues"].append("remote_cleanup_unknown")
        if records is not None:
            changed = [r["path"] for r in records if not Path(r["path"]).is_file() or
                       inputs.sha256(Path(r["path"])) != r["sha256"]]
            verification = {"status": "MATCH" if not changed else "CHANGED", "changed_inputs": changed}
            soak.atomic_json(output / "post-run-freeze-verification.json", verification)
            if changed:
                result["cleanup_issues"].append("frozen_execution_inputs_changed")
        result["cleanup_status"] = "COMPLETE" if not result["cleanup_issues"] else "UNKNOWN"
        if result["cleanup_issues"] and result["status"] == "PASS":
            result.update(status="INCONCLUSIVE", reason="runtime_cleanup_or_input_verification_incomplete")
        soak.atomic_json(output / "summary.json", result)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target-config", type=Path, default=ROOT / "out/b11-independent-2k-screen-20261009/target.json")
    args = parser.parse_args()
    binary_dir = ROOT / "build-debug/RelWithDebInfo"
    result = run(args.output.resolve(), binary_dir / "test_participant_window_remediation.exe",
                 binary_dir / "test_screen_share_quality_runtime.exe", args.target_config.resolve())
    print(json.dumps({key: result[key] for key in ("status", "reason", "cleanup_status")}), flush=True)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
