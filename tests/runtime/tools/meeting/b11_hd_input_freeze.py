"""Independent 720p source/layer diagnostic freeze; never a formal B11 verdict."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys

import b11_input_freeze as base

ROOT = base.ROOT
SCOPE = "B11_HD_LAYER_DIAGNOSTIC"
CONFIGURATION = base.CONFIGURATION
EXECUTABLE_NAME = base.EXECUTABLE_NAME
CLI_VERSION = "lk version 2.18.8"
CLI_SHA256 = "05b3a48a92dece3126044a1b279212c43dfeecad30da50d755af7bc3d4edf108"
TOOL_INPUTS = (
    "tests/runtime/tools/meeting/b11_hd_input_freeze.py",
    "tests/runtime/tools/meeting/b11_hd_layer_probe.py",
    "tests/runtime/tools/meeting/b11_hd_remote_run.py",
    "tests/runtime/tools/meeting/run-hd-layer-probe.ps1",
    "tests/telemetry/test_telemetry_panel.cpp",
    "tests/render/opengl/opengl_contract.cpp",
)
PROBE = {
    "steps": [{"layout": "grid16", "quality": "low", "window_width": 1120,
               "window_height": 720, "seconds": 45},
              {"layout": "grid16", "quality": "medium", "window_width": 1600,
               "window_height": 1000, "seconds": 45},
              {"layout": "pin_identity", "identity_role": "hd", "quality": "high",
               "window_width": 1120, "window_height": 720, "seconds": 60},
              {"layout": "grid16", "quality": "low", "window_width": 1120,
               "window_height": 720, "seconds": 60}],
    "settle_seconds": 35, "stall_seconds": 20, "maximum_wall_seconds": 420,
    "minimum_remote_videos": 17, "receiver_count": 1,
    "receiver_arguments": ["--scale-100"],
}
QUALITY_CONTRACT = {
    "low": {"width": 320, "height": 180, "source_fps": 15},
    "medium": {"width": 640, "height": 360, "source_fps": 20},
    "high": {"width": 1280, "height": 720, "source_fps": 30},
    "receive_fps_status_at_freeze": "NOT_RUN",
    "minimum_frame_rate_ratio": 0.90,
    "maximum_frame_age_ms": 3000,
    "maximum_sample_gap_seconds": 3,
    "minimum_observation_seconds": 20,
    "dimension_match_ratio": 1.0,
}
PUBLISHERS = {
    "hd": {"kind": "load_test", "count": 1, "subscribers": 0,
           "resolution": "high", "codec": "vp8", "duration": "15m",
           "num_per_second": 1, "simulcast": True},
    "background": {"kind": "load_test", "count": 16, "subscribers": 0,
                   "resolution": "low", "codec": "vp8", "duration": "15m",
                   "num_per_second": 5, "simulcast": True},
}


def validate_profile(profile: dict) -> dict:
    if (type(profile) is not dict or set(profile) != {
            "schema", "scope", "build_configuration", "transport", "target_config",
            "target", "publishers", "probe", "quality_contract", "source_identity_path"}
            or type(profile.get("schema")) is not int or profile["schema"] != 1
            or profile.get("scope") != SCOPE
            or profile.get("build_configuration") != CONFIGURATION
            or profile.get("transport") != "ssh"
            or not base._same_typed(profile.get("probe"), PROBE)
            or not base._same_typed(profile.get("quality_contract"), QUALITY_CONTRACT)):
        raise ValueError("invalid_hd_profile")
    # Reuse the existing closed target validation without accepting its low-source scope.
    base.validate_profile({"schema": 1, "scope": base.SCOPE,
                           "build_configuration": CONFIGURATION, "probe_mode": "grid16_transport",
                           "transport": "ssh", "target_config": profile["target_config"],
                           "target": profile["target"], "publisher": dict(base.PUBLISHER),
                           "probe": dict(base.PROBE)})
    if (type(profile["source_identity_path"]) is not str
            or not profile["source_identity_path"]
            or any(ord(c) < 32 for c in profile["source_identity_path"])):
        raise ValueError("invalid_hd_source_identity_path")
    publishers = profile.get("publishers")
    if type(publishers) is not dict or set(publishers) != set(PUBLISHERS):
        raise ValueError("invalid_hd_publishers")
    suffixes = []
    for role, expected in PUBLISHERS.items():
        actual = publishers[role]
        if type(actual) is not dict or set(actual) != set(expected) | {"identity_prefix"}:
            raise ValueError("invalid_hd_publishers")
        if not base._same_typed({k: actual[k] for k in expected}, expected):
            raise ValueError("invalid_hd_publishers")
        prefix = actual["identity_prefix"]
        marker = "b11hd_" if role == "hd" else "b11low_"
        if type(prefix) is not str or not re.fullmatch(marker + r"[A-Za-z0-9]{1,32}", prefix):
            raise ValueError("invalid_hd_identity_prefix")
        suffixes.append(prefix[len(marker):])
    if suffixes[0] != suffixes[1]:
        raise ValueError("hd_identity_run_mismatch")
    return profile


def hd_source_identity(profile: dict) -> str:
    return validate_profile(profile)["publishers"]["hd"]["identity_prefix"] + "_pub_0"


def _validate_source_identity(value: dict) -> dict:
    if (type(value) is not dict or set(value) != {
            "schema", "kind", "tag", "cli_path", "cli_version", "cli_sha256",
            "identity_format", "hd_sequence", "first_vp8_prefix_per_process",
            "source_evidence", "assets", "load_test_flags", "runtime_status",
            "source_equivalence", "fps_evidence", "remote_prerequisite"}
            or type(value.get("schema")) is not int or value["schema"] != 1
            or value.get("kind") != "B11_HD_SOURCE_IDENTITY"
            or value.get("tag") != "v2.18.8" or value.get("cli_version") != CLI_VERSION
            or value.get("cli_sha256") != CLI_SHA256
            or value.get("identity_format") != "<identity_prefix>_pub_<sequence>"
            or type(value.get("hd_sequence")) is not int or value["hd_sequence"] != 0
            or value.get("first_vp8_prefix_per_process") != "neon"
            or value.get("runtime_status") != "NOT_RUN"
            or value.get("source_equivalence") != "OFFICIAL_ASSETS_MATCH_REMOTE_CLI_BYTES"
            or value.get("fps_evidence") != "SPEC_AND_FRAME_DURATION_SOURCE; RECEIVE_FPS_NOT_RUN"):
        raise ValueError("invalid_hd_source_identity")
    if (type(value["cli_path"]) is not str
            or not re.fullmatch(r"/[A-Za-z0-9_./-]+", value["cli_path"])
            or ".." in Path(value["cli_path"]).parts):
        raise ValueError("invalid_hd_cli_path")
    evidence = value["source_evidence"]
    source_paths = {"pkg/provider/embeds.go", "pkg/provider/vp8looper.go",
                    "pkg/loadtester/loadtest.go", "pkg/loadtester/loadtester.go"}
    if (type(evidence) is not list or len(evidence) != 4
            or {item.get("path") for item in evidence if type(item) is dict} != source_paths):
        raise ValueError("invalid_hd_source_evidence")
    for item in evidence:
        if (type(item) is not dict or set(item) != {"path", "url", "sha256", "matches"}
                or item["url"] != "https://raw.githubusercontent.com/livekit/livekit-cli/v2.18.8/" + item["path"]
                or type(item["sha256"]) is not str or not re.fullmatch(r"[0-9a-f]{64}", item["sha256"])
                or type(item["matches"]) is not list or not item["matches"]):
            raise ValueError("invalid_hd_source_evidence")
        for match in item["matches"]:
            if (type(match) is not dict or set(match) != {"line", "text"}
                    or type(match["line"]) is not int or match["line"] < 1
                    or type(match["text"]) is not str or any(ord(c) < 32 for c in match["text"])):
                raise ValueError("invalid_hd_source_evidence")
    flags = value["load_test_flags"]
    if type(flags) is not list or not all(type(flag) is str and not any(ord(c) < 32 for c in flag) for flag in flags):
        raise ValueError("invalid_hd_cli_flags")
    for flag in ("--identity-prefix", "--publishers", "--subscribers", "--video-resolution",
                 "--video-codec", "--duration", "--num-per-second", "--no-simulcast"):
        if not any(flag in line for line in flags):
            raise ValueError("missing_hd_cli_flag")
    prerequisite = value["remote_prerequisite"]
    if (type(prerequisite) is not dict or set(prerequisite) != {
            "instance", "service_url", "config_path", "sfu_container", "config_sha256", "sfu_image",
            "logical_cpus_observed", "mem_total_kib_observed", "configured_egress_mbps",
            "configured_egress_evidence_source", "provider_api_independently_verified",
            "prerequisite_checkpoint_file"}
            or not base._same_typed(prerequisite.get("logical_cpus_observed"), 16)
            or type(prerequisite.get("mem_total_kib_observed")) is not int
            or prerequisite["mem_total_kib_observed"] < 60 * 1024 * 1024
            or not base._same_typed(prerequisite.get("configured_egress_mbps"), 20)
            or prerequisite.get("configured_egress_evidence_source") != "USER_CONFIRMED"
            or prerequisite.get("provider_api_independently_verified") is not False
            or type(prerequisite.get("config_sha256")) is not str
            or not re.fullmatch(r"[0-9a-f]{64}", prerequisite["config_sha256"])
            or type(prerequisite.get("sfu_image")) is not str
            or not re.fullmatch(r"sha256:[0-9a-f]{64}", prerequisite["sfu_image"])):
        raise ValueError("invalid_hd_remote_prerequisite")
    record = prerequisite["prerequisite_checkpoint_file"]
    if (type(record) is not dict or set(record) != {"path", "size", "sha256"}
            or type(record["path"]) is not str or not record["path"]
            or type(record["size"]) is not int or record["size"] < 1
            or type(record["sha256"]) is not str or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
        raise ValueError("invalid_hd_prerequisite_checkpoint")
    assets = value["assets"]
    if type(assets) is not list or len(assets) != 9:
        raise ValueError("invalid_hd_assets")
    seen = set()
    for asset in assets:
        if type(asset) is not dict or set(asset) != {
                "prefix", "quality", "path", "lfs_sha256", "size", "embedded_offset",
                "embedded_match", "ivf", "first_keyframe_dimensions", "publisher_fps", "advertised_bitrate_bps",
                "advertised_dimensions", "advertised_dimensions_match_asset"}:
            raise ValueError("invalid_hd_assets")
        prefix, quality = asset["prefix"], asset["quality"]
        if prefix not in ("crescent", "neon", "tunnel") or quality not in ("low", "medium", "high"):
            raise ValueError("invalid_hd_assets")
        pair = (prefix, quality)
        if pair in seen:
            raise ValueError("duplicate_hd_asset")
        seen.add(pair)
        layer = QUALITY_CONTRACT[quality]
        # These two official LOW assets are 320x150 despite their 180 spec/name.
        # Preserve the discrepancy; the measured HD target is the first NEON publisher.
        actual_height = 150 if quality == "low" and prefix != "neon" else layer["height"]
        kbps = {"low": 150, "medium": 600, "high": 2000}[quality]
        if (asset["path"] != f"pkg/provider/resources/{prefix}_{layer['height']}_{kbps}.ivf"
                or type(asset["lfs_sha256"]) is not str or not re.fullmatch(r"[0-9a-f]{64}", asset["lfs_sha256"])
                or type(asset["size"]) is not int or asset["size"] <= 44
                or type(asset["embedded_offset"]) is not int or asset["embedded_offset"] < 0
                or asset["embedded_match"] is not True
                or not base._same_typed(asset["first_keyframe_dimensions"], [layer["width"], actual_height])
                or not base._same_typed(asset["advertised_dimensions"], [layer["width"], layer["height"]])
                or asset["advertised_dimensions_match_asset"] is not (actual_height == layer["height"])
                or not base._same_typed(asset["publisher_fps"], layer["source_fps"])
                or not base._same_typed(asset["advertised_bitrate_bps"], kbps * 1000)):
            raise ValueError("invalid_hd_assets")
        ivf = asset["ivf"]
        if (type(ivf) is not dict or set(ivf) != {"fourcc", "width", "height", "timebase_denominator",
                                                "timebase_numerator", "frame_count"}
                or ivf["fourcc"] != "VP80"
                or not base._same_typed(ivf["width"], layer["width"])
                or not base._same_typed(ivf["height"], actual_height)
                or any(type(ivf[key]) is not int or ivf[key] <= 0
                       for key in ("timebase_denominator", "timebase_numerator", "frame_count"))):
            raise ValueError("invalid_hd_asset_ivf")
    return value


def _snapshot(executable: Path, profile_path: Path, root: Path) -> dict:
    root, executable, profile_path = root.resolve(), executable.resolve(), profile_path.resolve()
    profile = validate_profile(base._load_json(profile_path))
    profile_record = base._file_record(profile_path)
    if profile != validate_profile(base._load_json(profile_path)):
        raise ValueError("profile_changed_while_hashing")
    remote_identity = base._remote_transport_identity(profile, root)
    source_path = base._target_config_path(profile["source_identity_path"], root)
    source_identity = _validate_source_identity(base._load_json(source_path))
    source_record = base._file_record(source_path)
    if source_identity != _validate_source_identity(base._load_json(source_path)):
        raise ValueError("source_identity_changed_while_hashing")
    prerequisite = source_identity["remote_prerequisite"]
    for key in ("instance", "service_url", "config_path", "sfu_container"):
        if prerequisite[key] != profile["target"][key]:
            raise ValueError("hd_source_target_mismatch")
    prerequisite_record = base._file_record(base._target_config_path(
        prerequisite["prerequisite_checkpoint_file"]["path"], root))
    if prerequisite_record != prerequisite["prerequisite_checkpoint_file"]:
        raise ValueError("hd_prerequisite_checkpoint_changed")
    if not executable.is_file() or executable.name.casefold() != EXECUTABLE_NAME.casefold():
        raise ValueError("runtime_binary_missing_or_wrong_name")
    head = base._git_head(root)
    identity = base._binary_identity(executable)
    if identity.get("configuration") != CONFIGURATION:
        raise ValueError("runtime_binary_configuration_not_verified")
    metadata = base._build_metadata(executable, root)
    runtime_paths = {executable, executable.with_suffix(".pdb")}
    for dll in sorted(executable.parent.rglob("*.dll")):
        runtime_paths.add(dll)
        if dll.with_suffix(".pdb").is_file():
            runtime_paths.add(dll.with_suffix(".pdb"))
    files = [base._file_record(path) for path in sorted(runtime_paths)]
    if next(item["sha256"] for item in files if item["path"] == str(executable)) != identity.get("binary_sha256"):
        raise ValueError("runtime_binary_changed_while_hashing")
    paths = set(base._source_paths(root)) | {root / name for name in TOOL_INPUTS}
    sources = {path.relative_to(root).as_posix(): base._file_record(path)
               for path in sorted(paths, key=lambda path: path.relative_to(root).as_posix())}
    if head != base._git_head(root):
        raise ValueError("git_head_changed_while_hashing")
    return {"head": head, "repository": str(root), "profile": profile, "profile_file": profile_record,
            "hd_source_identity": hd_source_identity(profile), "source_identity": source_identity,
            "source_identity_file": source_record, "source_inputs": sources,
            "prerequisite_checkpoint_file": prerequisite_record,
            "binary_identity": identity, "build_metadata": metadata, "runtime_files": files,
            "remote_transport_identity": remote_identity}


def freeze_inputs(output: Path, executable: Path, profile: Path, *, root: Path = ROOT) -> dict:
    output = output.resolve()
    if output.exists():
        raise ValueError("freeze_manifest_already_exists")
    value = {"schema": 1, "scope": SCOPE, "created_utc": datetime.now(timezone.utc).isoformat(),
             "local_inputs": "FROZEN", "runtime_status": "NOT_RUN", "formal_b11_status": "NOT_RUN",
             "binary_source_equivalence": "UNKNOWN", "remote_target": "BOUND_NOT_REVERIFIED",
             "inputs": _snapshot(executable, profile, root)}
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
            stream.write("\n")
    except FileExistsError:
        raise ValueError("freeze_manifest_already_exists") from None
    return value


def verify_inputs(manifest: Path, executable: Path, profile: Path, *,
                  require_remote: bool = True,
                  instance: str | None = None, service_url: str | None = None,
                  transport: str | None = None,
                  target_config: Path | None = None, root: Path = ROOT) -> dict:
    frozen = base._load_json(manifest)
    if (type(frozen) is not dict or set(frozen) != {
            "schema", "scope", "created_utc", "local_inputs", "runtime_status", "formal_b11_status",
            "binary_source_equivalence", "remote_target", "inputs"}
            or type(frozen.get("schema")) is not int or frozen["schema"] != 1
            or frozen.get("scope") != SCOPE or frozen.get("local_inputs") != "FROZEN"
            or frozen.get("runtime_status") != "NOT_RUN" or frozen.get("formal_b11_status") != "NOT_RUN"
            or frozen.get("binary_source_equivalence") != "UNKNOWN"
            or frozen.get("remote_target") != "BOUND_NOT_REVERIFIED"):
        raise ValueError("invalid_hd_manifest")
    current = _snapshot(executable, profile, root)
    if require_remote and not current["profile"]["target"]["instance"]:
        raise ValueError("hd_remote_target_pending")
    if frozen["inputs"] != current:
        raise ValueError("frozen_inputs_changed")
    binding = current["profile"]
    if (instance is not None and instance != binding["target"]["instance"]
            or service_url is not None and service_url != binding["target"]["service_url"]
            or transport is not None and transport != "ssh"
            or target_config is not None
                and base._target_config_path(str(target_config), root)
                    != base._target_config_path(binding["target_config"], root)):
        raise ValueError("hd_runtime_target_mismatch")
    return frozen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    freeze = subs.add_parser("freeze")
    freeze.add_argument("--output", type=Path, required=True)
    verify = subs.add_parser("verify")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--require-remote", action="store_true")
    verify.add_argument("--instance")
    verify.add_argument("--service-url")
    verify.add_argument("--transport", choices=("ssh",))
    verify.add_argument("--target-config", type=Path)
    for command in (freeze, verify):
        command.add_argument("--executable", type=Path, required=True)
        command.add_argument("--profile", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "freeze":
            value = freeze_inputs(args.output, args.executable, args.profile)
        else:
            value = verify_inputs(args.manifest, args.executable, args.profile, instance=args.instance,
                                  service_url=args.service_url, transport=args.transport,
                                  target_config=args.target_config)
    except (ValueError, OSError) as error:
        reason = str(error) if type(error) is ValueError and re.fullmatch(r"[a-z0-9_]+", str(error)) else "hd_input_freeze_io_error"
        print(json.dumps({"status": "REJECTED", "reason": reason}), file=sys.stderr)
        return 2
    print(json.dumps({"status": "FROZEN" if args.command == "freeze" else "VERIFIED",
                      "scope": SCOPE, "hd_source_identity": value["inputs"]["hd_source_identity"],
                      "runtime_status": value["runtime_status"], "formal_b11_status": value["formal_b11_status"],
                      "binary_source_equivalence": value["binary_source_equivalence"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
