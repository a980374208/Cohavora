"""Independent 100-source/one-grid16 input freeze with a proven private-media gate."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import ipaddress
from pathlib import Path
import re
import sys

import b11_hd_input_freeze as hd

base = hd.base
ROOT = base.ROOT
SCOPE = "B11_100_SOURCE_GRID16_DIAGNOSTIC"
PATH_GATE_SCOPE = "B11_100_SOURCE_PRIVATE_MEDIA_PATH_GATE"
CONFIGURATION = base.CONFIGURATION
EXECUTABLE_NAME = base.EXECUTABLE_NAME
TOOL_INPUTS = (
    "tests/runtime/tools/meeting/b11_hd_input_freeze.py",
    "tests/runtime/tools/meeting/b11_hd_remote_run.py",
    "tests/runtime/tools/meeting/b11_hd_layer_probe.py",
    "tests/runtime/tools/meeting/b11_100_input_freeze.py",
    "tests/runtime/tools/meeting/b11_100_remote_run.py",
    "tests/runtime/tools/meeting/b11_100_grid_probe.py",
    "tests/runtime/tools/meeting/b11_publisher_network.py",
    "tests/runtime/tools/meeting/b11_namespace_publisher_worker.py",
)
PUBLISHER = {"kind": "load_test", "count": 100, "subscribers": 0,
             "resolution": "high", "codec": "vp8", "duration": "15m",
             "num_per_second": 5, "simulcast": True}
PROBE = {"observe_seconds": 300, "settle_seconds": 35, "maximum_wall_seconds": 450,
         "minimum_remote_videos": 100, "receiver_count": 1,
         "receiver_arguments": ["--scale-100"]}
QUALITY_CONTRACT = {
    "low": {"width": 320, "heights": [150, 180], "source_fps": 15},
    "medium": {"width": 640, "heights": [360], "source_fps": 20},
    "high": {"width": 1280, "heights": [720], "source_fps": 30},
    "subscription_mode": "AUTOMATIC_GRID16", "selected_count": 16,
    "source_assignment_by_sequence": "UNKNOWN_CONCURRENT_PUBLISH_ORDER",
    "receive_fps_status_at_freeze": "NOT_RUN", "minimum_frame_rate_ratio": 0.90,
    "maximum_frame_age_ms": 3000, "maximum_sample_gap_seconds": 3,
    "minimum_observation_seconds": 20, "dimension_match_ratio": 1.0,
}


def validate_profile(profile: dict) -> dict:
    if (type(profile) is not dict or set(profile) != {
            "schema", "scope", "build_configuration", "transport", "target_config", "target",
            "publisher", "probe", "quality_contract", "source_identity_path", "network_contract"}
            or type(profile.get("schema")) is not int or profile["schema"] != 1
            or profile.get("scope") != SCOPE or profile.get("build_configuration") != CONFIGURATION
            or profile.get("transport") != "ssh"
            or not base._same_typed(profile.get("probe"), PROBE)
            or not base._same_typed(profile.get("quality_contract"), QUALITY_CONTRACT)):
        raise ValueError("invalid_100_profile")
    base.validate_profile({"schema": 1, "scope": base.SCOPE, "build_configuration": CONFIGURATION,
                           "probe_mode": "grid16_transport", "transport": "ssh",
                           "target_config": profile["target_config"], "target": profile["target"],
                           "publisher": dict(base.PUBLISHER), "probe": dict(base.PROBE)})
    publisher = profile["publisher"]
    if (type(publisher) is not dict or set(publisher) != set(PUBLISHER) | {"identity_prefix"}
            or not base._same_typed({key: publisher[key] for key in PUBLISHER}, PUBLISHER)
            or type(publisher["identity_prefix"]) is not str
            or not re.fullmatch(r"b11grid100_[A-Za-z0-9]{1,32}", publisher["identity_prefix"])):
        raise ValueError("invalid_100_publisher")
    network = profile["network_contract"]
    if (type(network) is not dict or set(network) != {
            "publisher_media_path", "path_gate_required", "path_gate_evidence_path"}
            or network["publisher_media_path"] != "TASK_NAMESPACE_DNAT_TO_HOST_LOCAL"
            or network["path_gate_required"] is not True):
        raise ValueError("invalid_100_network_contract")
    for value in (profile["source_identity_path"], network["path_gate_evidence_path"]):
        if type(value) is not str or not value or any(ord(c) < 32 for c in value):
            raise ValueError("invalid_100_evidence_path")
    return profile


def publisher_identities(profile: dict) -> list[str]:
    prefix = validate_profile(profile)["publisher"]["identity_prefix"]
    return [f"{prefix}_pub_{sequence}" for sequence in range(100)]


def validate_path_gate(gate: dict, profile: dict, source: dict, *, require_cleanup: bool = True) -> dict:
    """A loopback signaling route alone cannot qualify private RTP delivery."""
    if (type(gate) is not dict or set(gate) != {
            "schema", "scope", "status", "formal_b11_status", "target", "cli_sha256",
            "config_sha256", "sfu_image", "task_owner", "facts", "cleanup"}
            or type(gate.get("schema")) is not int or gate["schema"] != 1
            or gate.get("scope") != PATH_GATE_SCOPE or gate.get("status") != "PASS"
            or gate.get("formal_b11_status") != "NOT_RUN"):
        raise ValueError("private_media_path_gate_not_passed")
    target = {key: profile["target"][key] for key in ("instance", "service_url", "config_path", "sfu_container")}
    prerequisite = source["remote_prerequisite"]
    if (not base._same_typed(gate["target"], target)
            or gate["cli_sha256"] != source["cli_sha256"]
            or gate["config_sha256"] != prerequisite["config_sha256"]
            or gate["sfu_image"] != prerequisite["sfu_image"]):
        raise ValueError("private_media_gate_target_mismatch")
    owner = gate["task_owner"]
    if (type(owner) is not dict or set(owner) != {
            "room", "remote_directory", "namespace", "veth_host", "veth_peer", "namespace_inode",
            "publisher_pid", "publisher_start_ticks", "owner_id", "host_ifindex", "peer_ifindex",
            "subnet", "host_ip", "peer_ip", "rtc_target_ip"}
            or type(owner["room"]) is not str or not re.fullmatch(r"b11-[A-Za-z0-9_-]+", owner["room"])
            or owner["remote_directory"] != "/tmp/" + owner["room"]
            or any(type(owner[key]) is not str or not re.fullmatch(r"[A-Za-z0-9_-]{1,63}", owner[key])
                   for key in ("namespace", "veth_host", "veth_peer"))
            or type(owner["namespace_inode"]) is not int or owner["namespace_inode"] <= 0
            or type(owner["publisher_pid"]) is not int or owner["publisher_pid"] <= 0
            or type(owner["publisher_start_ticks"]) is not str
            or not re.fullmatch(r"[0-9]+", owner["publisher_start_ticks"])
            or type(owner["owner_id"]) is not str or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", owner["owner_id"])
            or type(owner["host_ifindex"]) is not int or owner["host_ifindex"] <= 0
            or type(owner["peer_ifindex"]) is not int or owner["peer_ifindex"] <= 0):
        raise ValueError("invalid_private_media_gate_owner")
    try:
        if any(type(owner[key]) is not str for key in ("subnet", "host_ip", "peer_ip", "rtc_target_ip")):
            raise ValueError
        subnet = ipaddress.ip_network(owner["subnet"], strict=True)
        host = ipaddress.ip_address(owner["host_ip"])
        peer = ipaddress.ip_address(owner["peer_ip"])
        rtc_target = ipaddress.ip_address(owner["rtc_target_ip"])
        if (subnet.version != 4 or subnet.prefixlen != 30
                or not subnet.subnet_of(ipaddress.ip_network("198.18.0.0/15"))
                or host == peer or host not in subnet.hosts() or peer not in subnet.hosts()
                or rtc_target.version != 4
                or not any(rtc_target in ipaddress.ip_network(network)
                           for network in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))):
            raise ValueError
    except (ValueError, TypeError):
        raise ValueError("invalid_private_media_gate_subnet") from None
    facts = gate["facts"]
    if (type(facts) is not dict or set(facts) != {
            "observation_seconds", "publisher_count", "subscriber_count", "codec", "simulcast", "namespace_udp_dnat_packets",
            "private_veth_rx_bytes", "private_veth_rx_packets", "public_tx_bytes", "media_route", "sfu_running",
            "private_ingress_rtp_packets", "private_ingress_rtp_bytes", "private_egress_rtp_packets", "private_egress_rtp_bytes",
            "advertised_layer_count", "advertised_high_width", "advertised_high_height", "source_fps",
            "namespace_default_route_absent", "namespace_peer_route_only", "sfu_peer_route_interface_matches",
            "sfu_rtc_target_local_and_listening"}
            or type(facts["observation_seconds"]) not in (int, float) or not 15 <= facts["observation_seconds"] <= 60
            or not base._same_typed(facts["publisher_count"], 1)
            or not base._same_typed(facts["subscriber_count"], 1)
            or facts["codec"] != "vp8" or facts["simulcast"] is not True
            or facts["media_route"] != "DNAT_TO_HOST_LOCAL" or facts["sfu_running"] is not True
            or not base._same_typed(facts["advertised_layer_count"], 3)
            or not base._same_typed(facts["advertised_high_width"], 1280)
            or not base._same_typed(facts["advertised_high_height"], 720)
            or not base._same_typed(facts["source_fps"], 30)
            or any(facts[key] is not True for key in ("namespace_default_route_absent", "namespace_peer_route_only",
                                                      "sfu_peer_route_interface_matches", "sfu_rtc_target_local_and_listening"))
            or any(type(facts[key]) is not int for key in ("namespace_udp_dnat_packets", "private_veth_rx_bytes",
                                                          "private_veth_rx_packets", "public_tx_bytes",
                                                          "private_ingress_rtp_packets", "private_ingress_rtp_bytes",
                                                          "private_egress_rtp_packets", "private_egress_rtp_bytes"))
            or facts["namespace_udp_dnat_packets"] < 1 or facts["private_veth_rx_bytes"] < 1048576
            or any(facts[key] < 100 for key in ("private_veth_rx_packets", "private_ingress_rtp_packets", "private_egress_rtp_packets"))
            or any(facts[key] < 1048576 for key in ("private_ingress_rtp_bytes", "private_egress_rtp_bytes"))
            or not 0 <= facts["public_tx_bytes"] <= 262144):
        raise ValueError("private_media_gate_measurement_insufficient")
    cleanup = gate["cleanup"]
    if (type(require_cleanup) is not bool or type(cleanup) is not dict
            or set(cleanup) != {"publisher_stopped", "subscriber_stopped", "network_removed", "credentials_removed"}
            or any(cleanup[key] is not True for key in ("publisher_stopped", "subscriber_stopped", "credentials_removed"))
            or cleanup["network_removed"] is not require_cleanup):
        raise ValueError("private_media_gate_cleanup_incomplete")
    return gate


def _snapshot(executable: Path, profile_path: Path, root: Path) -> dict:
    root, executable, profile_path = root.resolve(), executable.resolve(), profile_path.resolve()
    profile = validate_profile(base._load_json(profile_path))
    profile_record = base._file_record(profile_path)
    if profile != validate_profile(base._load_json(profile_path)):
        raise ValueError("profile_changed_while_hashing")
    transport = base._remote_transport_identity(profile, root)
    source_path = base._target_config_path(profile["source_identity_path"], root)
    source = hd._validate_source_identity(base._load_json(source_path))
    source_record = base._file_record(source_path)
    if source != hd._validate_source_identity(base._load_json(source_path)):
        raise ValueError("source_identity_changed_while_hashing")
    prerequisite = source["remote_prerequisite"]
    if any(prerequisite[key] != profile["target"][key]
           for key in ("instance", "service_url", "config_path", "sfu_container")):
        raise ValueError("100_source_target_mismatch")
    prerequisite_record = base._file_record(base._target_config_path(
        prerequisite["prerequisite_checkpoint_file"]["path"], root))
    if prerequisite_record != prerequisite["prerequisite_checkpoint_file"]:
        raise ValueError("100_prerequisite_checkpoint_changed")
    gate_path = base._target_config_path(profile["network_contract"]["path_gate_evidence_path"], root)
    gate = validate_path_gate(base._load_json(gate_path), profile, source)
    gate_record = base._file_record(gate_path)
    if gate != validate_path_gate(base._load_json(gate_path), profile, source):
        raise ValueError("private_media_gate_changed_while_hashing")
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
    # Include companion namespace/packet-observer workers created for this scope.
    paths.update((root / "tests/runtime/tools/meeting").glob("b11_100_*.py"))
    sources = {path.relative_to(root).as_posix(): base._file_record(path)
               for path in sorted(paths, key=lambda path: path.relative_to(root).as_posix())}
    if head != base._git_head(root):
        raise ValueError("git_head_changed_while_hashing")
    return {"head": head, "repository": str(root), "profile": profile, "profile_file": profile_record,
            "source_identity": source, "source_identity_file": source_record,
            "private_media_path_gate": gate, "private_media_path_gate_file": gate_record,
            "prerequisite_checkpoint_file": prerequisite_record, "source_inputs": sources,
            "binary_identity": identity, "build_metadata": metadata, "runtime_files": files,
            "remote_transport_identity": transport}


def freeze_inputs(output: Path, executable: Path, profile: Path, *, root: Path = ROOT) -> dict:
    output = output.resolve()
    if output.exists():
        raise ValueError("freeze_manifest_already_exists")
    value = {"schema": 1, "scope": SCOPE, "created_utc": datetime.now(timezone.utc).isoformat(),
             "local_inputs": "FROZEN", "runtime_status": "NOT_RUN", "formal_b11_status": "NOT_RUN",
             "binary_source_equivalence": "UNKNOWN", "remote_target": "BOUND_NOT_REVERIFIED",
             "private_media_path_status": "PREVIOUS_ONE_SOURCE_GATE_PASSED_REVERIFY_REQUIRED",
             "inputs": _snapshot(executable, profile, root)}
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
            stream.write("\n")
    except FileExistsError:
        raise ValueError("freeze_manifest_already_exists") from None
    return value


def verify_inputs(manifest: Path, executable: Path, profile: Path, *, require_remote=True,
                  instance: str | None = None, service_url: str | None = None,
                  transport: str | None = None, target_config: Path | None = None, root: Path = ROOT) -> dict:
    frozen = base._load_json(manifest)
    if (type(frozen) is not dict or set(frozen) != {
            "schema", "scope", "created_utc", "local_inputs", "runtime_status", "formal_b11_status",
            "binary_source_equivalence", "remote_target", "private_media_path_status", "inputs"}
            or type(frozen.get("schema")) is not int or frozen["schema"] != 1
            or frozen.get("scope") != SCOPE or frozen.get("local_inputs") != "FROZEN"
            or frozen.get("runtime_status") != "NOT_RUN" or frozen.get("formal_b11_status") != "NOT_RUN"
            or frozen.get("binary_source_equivalence") != "UNKNOWN"
            or frozen.get("remote_target") != "BOUND_NOT_REVERIFIED"
            or frozen.get("private_media_path_status") != "PREVIOUS_ONE_SOURCE_GATE_PASSED_REVERIFY_REQUIRED"):
        raise ValueError("invalid_100_manifest")
    current = _snapshot(executable, profile, root)
    if current != frozen["inputs"]:
        raise ValueError("frozen_inputs_changed")
    binding = current["profile"]
    if (require_remote and not binding["target"]["instance"]
            or instance is not None and instance != binding["target"]["instance"]
            or service_url is not None and service_url != binding["target"]["service_url"]
            or transport is not None and transport != "ssh"
            or target_config is not None and base._target_config_path(str(target_config), root)
                != base._target_config_path(binding["target_config"], root)):
        raise ValueError("100_runtime_target_mismatch")
    return frozen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    create = subs.add_parser("freeze")
    create.add_argument("--output", type=Path, required=True)
    verify = subs.add_parser("verify")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--require-remote", action="store_true")
    verify.add_argument("--instance")
    verify.add_argument("--service-url")
    verify.add_argument("--transport", choices=("ssh",))
    verify.add_argument("--target-config", type=Path)
    for command in (create, verify):
        command.add_argument("--executable", type=Path, required=True)
        command.add_argument("--profile", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "freeze":
            value = freeze_inputs(args.output, args.executable, args.profile)
        else:
            value = verify_inputs(args.manifest, args.executable, args.profile, instance=args.instance,
                                  service_url=args.service_url, transport=args.transport, target_config=args.target_config)
    except (OSError, ValueError) as error:
        reason = str(error) if type(error) is ValueError and re.fullmatch(r"[a-z0-9_]+", str(error)) else "100_freeze_io_error"
        print(json.dumps({"status": "REJECTED", "reason": reason}), file=sys.stderr)
        return 2
    print(json.dumps({"status": "FROZEN" if args.command == "freeze" else "VERIFIED", "scope": SCOPE,
                      "runtime_status": value["runtime_status"], "formal_b11_status": value["formal_b11_status"],
                      "private_media_path_status": value["private_media_path_status"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
