"""Freeze independent 2K or owned-window 4K source and layer measurement inputs."""
from __future__ import annotations

import argparse
from copy import deepcopy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import b11_input_freeze as base

ROOT = base.ROOT
SCOPE = "B11_HIGHRES_PIXEL_LAYER_SELECTION_DIAGNOSTIC_V2"
FOUR_K_SCOPE = "B11_4K_OWNED_WINDOW_LAYER_GPU_DIAGNOSTIC_V1"
CONFIGURATION = base.CONFIGURATION
EXECUTABLE_NAME = base.EXECUTABLE_NAME
TOOL_INPUTS = (
    "tests/runtime/tools/meeting/b11_highres_input_freeze.py",
    "tests/runtime/tools/meeting/b11_highres_layer_probe.py",
    "tests/runtime/tools/meeting/b11_highres_run.py",
    "tests/runtime/tools/meeting/b11_highres_publisher_probe.py",
    "tests/runtime/tools/meeting/b11_gpu_resource_sampler.py",
    "tests/runtime/selftests/test_b11_highres_input_freeze.py",
    "tests/runtime/selftests/test_b11_highres_layer_probe.py",
    "tests/runtime/selftests/test_b11_highres_publisher_probe.py",
    "tests/runtime/selftests/test_b11_highres_run.py",
    "tests/runtime/selftests/test_b11_gpu_resource_sampler.py",
    "tests/runtime/tools/meeting/b11_hd_layer_probe.py",
    "tests/runtime/tools/meeting/b11_hd_input_freeze.py",
    "tests/runtime/tools/meeting/b11_hd_remote_run.py",
    "tests/telemetry/test_telemetry_panel.cpp",
    "tests/render/opengl/opengl_contract.cpp",
)
QUALITY_CONTRACT = {
    "policy_version": 2,
    "supersedes_policy": "grid_720_cap_main_720_floor",
    "grid_selection": "floor_available_layer",
    "grid_maximum_dimensions": [2560, 1440],
    "grid_below_minimum": "lowest_layer_within_cap",
    "main_selection": "ceil_available_layer",
    "main_maximum_dimensions": [3840, 2160],
    "main_above_maximum": "highest_layer_within_cap",
    "single_layer_selection": "only_available_layer",
    "main_minimum_dimensions": None,
    "minimum_frame_rate_ratio": 0.90, "maximum_frame_age_ms": 3000,
    "maximum_sample_gap_seconds": 3, "minimum_observation_seconds": 20,
    "dimension_match_ratio": 1.0, "receive_fps_status_at_freeze": "NOT_RUN",
    "grid_first_page_local_seats": {"participant": True, "screen_share": False},
    "allow_dynamic_encoder_layer_downscale": True,
    "require_fresh_publisher_layer_evidence": True,
    "maximum_publisher_age_seconds": 3,
    "maximum_source_aspect_relative_error": 0.02,
}
SOURCES = {
    "camera": {"source": "camera", "width": 2560, "height": 1440,
               "source_fps": 30, "codec": "vp8", "simulcast": True},
    "screen": {"source": "screen_share", "width": 2560, "height": 1440,
               "source_fps": 20, "codec": "vp8", "simulcast": True},
}
SOURCES.update({
    "camera4k": {"source": "camera", "width": 3840, "height": 2160,
                 "source_fps": 30, "codec": "vp8", "simulcast": True,
                 "capture_backend": "wgc_window", "source_scope": "owned_window_native_fixture"},
    "screen4k": {"source": "screen_share", "width": 3840, "height": 2160,
                 "source_fps": 20, "codec": "vp8", "simulcast": True,
                 "capture_backend": "wgc_window", "source_scope": "owned_window_native_fixture"},
})
LAYERS = {
    "camera": {"low": {"quality": "low", "rid": "q", "width": 320, "height": 180, "source_fps": 15},
               "medium": {"quality": "medium", "rid": "h", "width": 640, "height": 360, "source_fps": 20},
               "high": {"quality": "high", "rid": "f", "width": 2560, "height": 1440, "source_fps": 30}},
    "screen": {"low": {"quality": "low", "rid": "q", "width": 1280, "height": 720, "source_fps": 3},
               "high": {"quality": "medium", "rid": "h", "width": 2560, "height": 1440, "source_fps": 20}},
}
LAYERS["camera4k"] = deepcopy(LAYERS["camera"])
LAYERS["camera4k"]["high"].update(width=3840, height=2160)
LAYERS["screen4k"] = deepcopy(LAYERS["screen"])
LAYERS["screen4k"]["low"].update(width=1920, height=1080)
LAYERS["screen4k"]["high"].update(width=3840, height=2160)
PROBES = {
    "camera": {"steps": [
        {"layout": "grid16", "request": "grid_low", "window_width": 1120, "window_height": 720, "seconds": 30},
        {"layout": "grid16", "request": "grid_large", "window_width": 3840, "window_height": 2160, "seconds": 30},
        {"layout": "pin_identity", "request": "main_highest", "window_width": 3840, "window_height": 2160, "seconds": 30},
        {"layout": "grid16", "request": "grid_large", "window_width": 3840, "window_height": 2160, "seconds": 30}],
        "settle_seconds": 35, "maximum_wall_seconds": 420, "minimum_remote_videos": 15,
        "receiver_count": 1, "receiver_arguments": ["--scale-100"]},
    "screen": {"steps": [
        {"layout": "auto", "request": "main_small", "window_width": 1120, "window_height": 720, "seconds": 30},
        {"layout": "auto", "request": "main_highest", "window_width": 3840, "window_height": 2160, "seconds": 30},
        {"layout": "auto", "request": "main_small", "window_width": 1120, "window_height": 720, "seconds": 30}],
        "settle_seconds": 35, "maximum_wall_seconds": 240, "minimum_remote_videos": 1,
        "receiver_count": 1, "receiver_arguments": ["--scale-100"]},
}
PROBES["camera4k"] = deepcopy(PROBES["camera"])
PROBES["camera4k"]["steps"].insert(2, {"layout": "pin_identity", "request": "main_small",
    "window_width": 850, "window_height": 560, "seconds": 30})
PROBES["camera4k"]["maximum_wall_seconds"] = 480
PROBES["screen4k"] = deepcopy(PROBES["screen"])


def scenario_kind(scenario):
    if scenario not in SOURCES:
        raise ValueError("invalid_highres_scenario")
    return "camera" if SOURCES[scenario]["source"] == "camera" else "screen"


def scenario_scope(scenario):
    scenario_kind(scenario)
    return FOUR_K_SCOPE if scenario.endswith("4k") else SCOPE


def quality_contract(scenario):
    scenario_kind(scenario)
    contract = deepcopy(QUALITY_CONTRACT)
    if scenario.endswith("4k"):
        contract.update(required_source_backend="wgc_window",
            required_source_scope="owned_window_native_fixture", require_gpu_resource_evidence=True,
            gpu_required_scope="measurement_steps", capture_counter_scope="windows_x64",
            capture_counter_semantics="converted_callback_delivery_count_and_video_source_api_count",
            capture_counter_invariant="capture_frames_ge_captured_frames_gt_zero_independent_monotonic_progress")
    return contract


def make_profile(scenario, target, target_config, source_identity_path):
    if type(scenario) is not str or scenario not in SOURCES:
        raise ValueError("invalid_highres_scenario")
    return {"schema": 1, "scope": scenario_scope(scenario), "build_configuration": CONFIGURATION,
        "scenario": scenario, "transport": "ssh", "target_config": str(target_config),
        "source_identity_path": str(source_identity_path), "target": deepcopy(target),
        "probe": deepcopy(PROBES[scenario]), "quality_contract": quality_contract(scenario)}


def validate_profile(value):
    if (type(value) is not dict or set(value) != {"schema", "scope", "build_configuration",
            "scenario", "transport", "target_config", "source_identity_path", "target", "probe", "quality_contract"}
            or type(value.get("schema")) is not int or value["schema"] != 1
            or value.get("scope") not in (SCOPE, FOUR_K_SCOPE) or value.get("build_configuration") != CONFIGURATION
            or type(value.get("scenario")) is not str or value["scenario"] not in SOURCES or value.get("transport") != "ssh"
            or value["scope"] != scenario_scope(value["scenario"])
            or not base._same_typed(value.get("probe"), PROBES[value["scenario"]])
            or not base._same_typed(value.get("quality_contract"), quality_contract(value["scenario"]))):
        raise ValueError("invalid_highres_profile")
    base.validate_profile({"schema": 1, "scope": base.SCOPE, "build_configuration": CONFIGURATION,
        "probe_mode": "grid16_transport", "transport": "ssh", "target_config": value["target_config"],
        "target": value["target"], "publisher": dict(base.PUBLISHER), "probe": dict(base.PROBE)})
    if type(value["source_identity_path"]) is not str or not value["source_identity_path"] or any(
            ord(c) < 32 for c in value["source_identity_path"]):
        raise ValueError("invalid_highres_source_path")
    return value


def _validate_record(record):
    if (type(record) is not dict or set(record) != {"path", "size", "sha256"}
            or type(record["path"]) is not str or not record["path"]
            or type(record["size"]) is not int or record["size"] < 1
            or type(record["sha256"]) is not str or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
        raise ValueError("invalid_highres_artifact")


def validate_source_identity(value, profile):
    scenario = profile["scenario"]
    if (type(value) is not dict or set(value) != {"schema", "kind", "scenario", "task", "target",
            "target_identity", "source", "layers", "remote_prerequisite", "artifacts", "publisher_configuration"}
            or type(value.get("schema")) is not int or value["schema"] != 1
            or value.get("kind") != "B11_HIGHRES_SOURCE_IDENTITY" or value.get("scenario") != scenario
            or value.get("publisher_configuration") != CONFIGURATION
            or not base._same_typed(value.get("target"), profile["target"])
            or not base._same_typed(value.get("source"), SOURCES[scenario])
            or not base._same_typed(value.get("layers"), LAYERS[scenario])):
        raise ValueError("invalid_highres_source_identity")
    task = value["task"]
    if (type(task) is not dict or set(task) != {"task_id", "room"}
            or type(task["task_id"]) is not str or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", task["task_id"])
            or type(task["room"]) is not str or not re.fullmatch(r"b11-[A-Za-z0-9_-]{1,100}", task["room"])
            or type(value["target_identity"]) is not str
            or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", value["target_identity"])):
        raise ValueError("invalid_highres_task_identity")
    prerequisite = value["remote_prerequisite"]
    if (type(prerequisite) is not dict or set(prerequisite) != {"config_sha256", "sfu_image"}
            or type(prerequisite["config_sha256"]) is not str
            or not re.fullmatch(r"[0-9a-f]{64}", prerequisite["config_sha256"])
            or type(prerequisite["sfu_image"]) is not str
            or not re.fullmatch(r"sha256:[0-9a-f]{64}", prerequisite["sfu_image"])):
        raise ValueError("invalid_highres_remote_prerequisite")
    artifacts = value["artifacts"]
    if (type(artifacts) is not dict or set(artifacts) != {
            "publisher_binary", "publisher_symbols", "publisher_ready", "server_snapshot", "extra"}
            or type(artifacts["extra"]) is not list):
        raise ValueError("invalid_highres_artifacts")
    records = [artifacts[name] for name in artifacts if name != "extra"] + artifacts["extra"]
    for record in records:
        _validate_record(record)
    if len({record["path"] for record in records}) != len(records):
        raise ValueError("duplicate_highres_artifact")
    return value


def _source_track_hash(snapshot, source):
    import hashlib
    expected_identity = hashlib.sha256(source["target_identity"].encode()).hexdigest()[:16]
    if (type(snapshot) is not dict or snapshot.get("ok") is not True
            or snapshot.get("target_identity_hash") != expected_identity
            or type(snapshot.get("target_track_count")) is not int or snapshot["target_track_count"] != 1
            or type(snapshot.get("tracks")) is not list or len(snapshot["tracks"]) != 1):
        raise ValueError("invalid_highres_server_snapshot")
    track = snapshot["tracks"][0]
    if (type(track) is not dict or not re.fullmatch(r"[0-9a-f]{16}", str(track.get("sid_hash", "")))
            or track.get("source") != source["source"]["source"]
            or not base._same_typed(track.get("width"), source["source"]["width"])
            or not base._same_typed(track.get("height"), source["source"]["height"])
            or type(track.get("layers")) is not list):
        raise ValueError("invalid_highres_server_track")
    qualities = {"low": 0, "medium": 1, "high": 2}
    declarations = {(layer.get("quality"), layer.get("width"), layer.get("height"))
        for layer in track["layers"] if type(layer) is dict}
    if (any(type(layer.get("quality")) is not int for layer in track["layers"] if type(layer) is dict)
            or len(declarations) != len(track["layers"]) or declarations != {
                (qualities[layer["quality"]], layer["width"], layer["height"])
                for layer in source["layers"].values()}):
        raise ValueError("highres_server_layers_mismatch")
    return track["sid_hash"]


def _publisher_binary_identity(executable, root):
    expected_name = "test_screen_share_runtime.exe"
    executable = executable.resolve()
    if not executable.is_file() or executable.name.casefold() != expected_name.casefold():
        raise ValueError("highres_publisher_binary_not_verified")
    try:
        verified = subprocess.run(["pwsh", "-NoProfile", "-File", str(root /
            "tests/runtime/tools/diagnostics/verify_runtime_binary.ps1"), "-Executable", str(executable),
            "-ExpectedExecutableName", expected_name, "-Configuration", CONFIGURATION],
            capture_output=True, text=True, timeout=30, check=True,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        identity = json.loads(verified.stdout)
    except (OSError, subprocess.SubprocessError, ValueError):
        raise ValueError("highres_publisher_binary_not_verified") from None
    if (type(identity) is not dict or identity.get("configuration") != CONFIGURATION
            or identity.get("binary_sha256") != base.sha256(executable)
            or type(identity.get("binary_path")) is not str
            or Path(identity["binary_path"]).resolve() != executable):
        raise ValueError("highres_publisher_binary_not_verified")
    return identity


def _snapshot(executable, profile_path, root):
    root, executable, profile_path = root.resolve(), executable.resolve(), profile_path.resolve()
    profile = validate_profile(base._load_json(profile_path))
    profile_record = base._file_record(profile_path)
    transport = base._remote_transport_identity(profile, root)
    source_path = base._target_config_path(profile["source_identity_path"], root)
    source = validate_source_identity(base._load_json(source_path), profile)
    source_record = base._file_record(source_path)
    artifacts = source["artifacts"]
    records = [artifacts[name] for name in artifacts if name != "extra"] + artifacts["extra"]
    for record in records:
        if base._file_record(base._target_config_path(record["path"], root)) != record:
            raise ValueError("highres_artifact_changed")
    sid_hash = _source_track_hash(base._load_json(base._target_config_path(
        artifacts["server_snapshot"]["path"], root)), source)
    if profile["scenario"].endswith("4k"):
        ready = base._load_json(base._target_config_path(artifacts["publisher_ready"]["path"], root))
        _validate_four_k_ready(ready, source, sid_hash)
    if not executable.is_file() or executable.name.casefold() != EXECUTABLE_NAME.casefold():
        raise ValueError("runtime_binary_missing_or_wrong_name")
    receiver_identity = base._binary_identity(executable)
    publisher_identity = _publisher_binary_identity(base._target_config_path(artifacts["publisher_binary"]["path"], root), root)
    if any(identity.get("configuration") != CONFIGURATION for identity in (receiver_identity, publisher_identity)):
        raise ValueError("runtime_binary_configuration_not_verified")
    head = base._git_head(root)
    runtime_paths = {executable, executable.with_suffix(".pdb")}
    for dll in executable.parent.rglob("*.dll"):
        runtime_paths.add(dll)
        if dll.with_suffix(".pdb").is_file():
            runtime_paths.add(dll.with_suffix(".pdb"))
    files = [base._file_record(path) for path in sorted(runtime_paths)]
    if (next(record["sha256"] for record in files if record["path"] == str(executable)) !=
            receiver_identity.get("binary_sha256") or artifacts["publisher_binary"]["sha256"] !=
            publisher_identity.get("binary_sha256")):
        raise ValueError("runtime_binary_changed_while_hashing")
    paths = set(base._source_paths(root)) | {root / name for name in TOOL_INPUTS}
    paths.update((root / "tests/runtime/tools/meeting").glob("b11_highres_*.py"))
    sources = {path.relative_to(root).as_posix(): base._file_record(path) for path in sorted(paths)}
    if (head != base._git_head(root) or profile != validate_profile(base._load_json(profile_path))
            or source != validate_source_identity(base._load_json(source_path), profile)):
        raise ValueError("highres_inputs_changed_while_hashing")
    return {"head": head, "repository": str(root), "profile": profile, "profile_file": profile_record,
        "source_identity": source, "source_identity_file": source_record, "source_track_sid_hash": sid_hash,
        "source_inputs": sources, "runtime_files": files, "binary_identity": receiver_identity,
        "publisher_binary_identity": publisher_identity, "build_metadata": base._build_metadata(executable, root),
        "remote_transport_identity": transport}


def freeze_inputs(output, executable, profile, *, root=ROOT):
    if output.exists():
        raise ValueError("freeze_manifest_already_exists")
    snapshot = _snapshot(executable, profile, root)
    value = {"schema": 1, "scope": snapshot["profile"]["scope"], "created_utc": datetime.now(timezone.utc).isoformat(),
        "local_inputs": "FROZEN", "runtime_status": "NOT_RUN", "formal_b11_status": "NOT_RUN",
        "binary_source_equivalence": "UNKNOWN", "inputs": snapshot}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")
    return value


def verify_inputs(manifest, executable, profile, *, service_url=None, root=ROOT):
    value = base._load_json(manifest)
    if (type(value) is not dict or set(value) != {"schema", "scope", "created_utc", "local_inputs",
            "runtime_status", "formal_b11_status", "binary_source_equivalence", "inputs"}
            or type(value.get("schema")) is not int or value["schema"] != 1
            or value.get("scope") not in (SCOPE, FOUR_K_SCOPE) or value.get("local_inputs") != "FROZEN"
            or value.get("runtime_status") != "NOT_RUN" or value.get("formal_b11_status") != "NOT_RUN"
            or value.get("binary_source_equivalence") != "UNKNOWN"):
        raise ValueError("invalid_highres_manifest")
    if _snapshot(executable, profile, root) != value["inputs"] or value["scope"] != value["inputs"]["profile"]["scope"]:
        raise ValueError("frozen_inputs_changed")
    if service_url is not None and service_url != value["inputs"]["profile"]["target"]["service_url"]:
        raise ValueError("highres_runtime_target_mismatch")
    return value


def _validate_four_k_ready(ready, source, sid_hash):
    """A server declaration alone cannot prove that native WGC captured 4K."""
    import hashlib
    description = source["source"]
    expected = {"schema": 1, "event": "ready", "source_kind": scenario_kind(source["scenario"]),
        "source_width": description["width"], "source_height": description["height"],
        "target_fps": description["source_fps"], "capture_backend": description["capture_backend"],
        "source_scope": description["source_scope"],
        "capturer_id": 1,
        "capture_size_mismatches": 0,
        "identity_hash": hashlib.sha256(source["target_identity"].encode()).hexdigest()[:16],
        "publication_sid_hash": sid_hash}
    if (type(ready) is not dict or any(not base._same_typed(ready.get(name), value)
            for name, value in expected.items()) or type(ready.get("capture_frames")) is not int
            or ready["capture_frames"] < 1 or type(ready.get("captured_frames")) is not int
            or not 0 < ready["captured_frames"] <= ready["capture_frames"]
            or type(sid_hash) is not str or not re.fullmatch(r"[0-9a-f]{16}", sid_hash)):
        raise ValueError("highres_native_four_k_capture_mismatch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    create = sub.add_parser("freeze")
    create.add_argument("--output", type=Path, required=True)
    verify = sub.add_parser("verify")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--service-url")
    for command in (create, verify):
        command.add_argument("--executable", type=Path, required=True)
        command.add_argument("--profile", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.operation == "freeze":
            value = freeze_inputs(args.output, args.executable, args.profile)
        else:
            value = verify_inputs(args.manifest, args.executable, args.profile, service_url=args.service_url)
    except (OSError, ValueError):
        print(json.dumps({"status": "REJECTED", "reason": "highres_freeze_inputs_rejected"}), file=sys.stderr)
        return 2
    print(json.dumps({"status": "FROZEN" if args.operation == "freeze" else "VERIFIED", "scope": value["scope"],
        "runtime_status": "NOT_RUN", "formal_b11_status": "NOT_RUN"}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
