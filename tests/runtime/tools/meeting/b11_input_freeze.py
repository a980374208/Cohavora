"""Offline B11 grid16 preflight input freeze; no server access or runtime verdict."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[4]
CONFIGURATION = "RelWithDebInfo"
EXECUTABLE_NAME = "test_participant_window_remediation.exe"
SCOPE = "B11_GRID16_PREFLIGHT"
TOOL_INPUTS = (
    "tests/runtime/tools/meeting/b11_input_freeze.py",
    "tests/runtime/tools/meeting/b11_remote.py",
    "tests/runtime/tools/meeting/meeting_render_probe.py",
    "tests/runtime/tools/meeting/meeting_soak.py",
    "tests/runtime/tools/meeting/ecs_resource_sampler.py",
    "tests/runtime/tools/meeting/soak_low_bandwidth_publishers.py",
    "tests/runtime/tools/diagnostics/verify_runtime_binary.ps1",
    "tests/runtime/tools/diagnostics/invoke_diagnostic_probe.ps1",
    "tests/runtime/orchestration/common.ps1",
    "tests/runtime/orchestration/run-render-probe.ps1",
    "tests/meeting/test_participant_snapshot_remediation.cpp",
)
PROBE = {"observe_seconds": 300, "stall_seconds": 20, "settle_seconds": 35,
         "maximum_wall_seconds": 420, "minimum_remote_videos": 17, "receiver_count": 1}
TRANSITION_PROBE = {"observe_seconds": None, "step_observation_seconds": [45, 60, 45, 120],
                    "stall_seconds": 20, "settle_seconds": 35, "maximum_wall_seconds": 520,
                    "minimum_remote_videos": 17, "receiver_count": 1}
PUBLISHER = {"kind": "load_test", "count": 17, "subscribers": 0,
             "resolution": "low", "codec": "vp8", "duration": "15m",
             "num_per_second": 5, "simulcast": True}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _no_duplicate_keys(items):
    value = {}
    for key, item in items:
        if key in value:
            raise ValueError("duplicate_json_key")
        value[key] = item
    return value


def _load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"),
                          object_pairs_hook=_no_duplicate_keys,
                          parse_constant=lambda _: (_ for _ in ()).throw(
                              ValueError("non_finite_json")))
    except (OSError, UnicodeError, json.JSONDecodeError):
        raise ValueError("input_json_unreadable") from None


def _same_typed(actual, expected):
    if type(actual) is not type(expected):
        return False
    if isinstance(expected, dict):
        return actual.keys() == expected.keys() and all(
            _same_typed(actual[key], value) for key, value in expected.items())
    if isinstance(expected, list):
        return len(actual) == len(expected) and all(
            _same_typed(item, value) for item, value in zip(actual, expected))
    return actual == expected


def validate_service_url(value: str, *, local=False) -> str:
    """The current controlled runner supports a bare ws/http origin only."""
    try:
        parsed = urlsplit(value)
        if (type(value) is not str or any(char.isspace() or ord(char) < 32 for char in value)
                or parsed.scheme != ("http" if local else "ws") or not parsed.hostname
                or parsed.port is None or not 1 <= parsed.port <= 65535
                or parsed.username is not None or parsed.password is not None
                or parsed.query or parsed.fragment or parsed.path not in ("", "/")
                or "?" in value or "#" in value or "\\" in value):
            raise ValueError
        if local and parsed.hostname != "127.0.0.1":
            raise ValueError
    except (TypeError, ValueError, AttributeError):
        raise ValueError("invalid_service_origin") from None
    return value


def validate_profile(profile: dict) -> dict:
    # A closed schema prevents copying credentials or arbitrary URL/query fields.
    required = {"schema", "scope", "build_configuration", "probe_mode", "target", "publisher", "probe"}
    if (not isinstance(profile, dict) or not required <= set(profile)
            or set(profile) - required - {"transport", "target_config"}
            or type(profile.get("schema")) is not int or profile["schema"] != 1
            or profile.get("scope") != SCOPE or profile.get("build_configuration") != CONFIGURATION
            or profile.get("probe_mode") not in ("grid16_transport", "grid16_transition")):
        raise ValueError("invalid_b11_profile")
    transport = profile.get("transport", "workbench")
    if transport not in ("workbench", "ssh"):
        raise ValueError("invalid_b11_transport")
    if transport == "ssh":
        config = profile.get("target_config")
        if (type(config) is not str or not config
                or any(ord(char) < 32 for char in config)):
            raise ValueError("invalid_b11_target_config")
    elif "target_config" in profile:
        raise ValueError("invalid_b11_target_config")
    publisher = profile.get("publisher")
    if not isinstance(publisher, dict) or type(publisher.get("simulcast")) is not bool:
        raise ValueError("invalid_b11_publisher")
    expected_publisher = {**PUBLISHER, "simulcast": publisher["simulcast"]}
    expected_probe = PROBE if profile["probe_mode"] == "grid16_transport" else TRANSITION_PROBE
    if (not _same_typed(publisher, expected_publisher)
            or not _same_typed(profile.get("probe"), expected_probe)):
        raise ValueError("invalid_b11_profile")
    target = profile.get("target")
    if not isinstance(target, dict) or set(target) != {
            "instance", "service_url", "local_service_url", "config_path", "sfu_container",
            "expected_vcpus", "expected_memory_gib", "minimum_public_egress_mbps"}:
        raise ValueError("invalid_b11_target")
    expected = {"expected_vcpus": 16, "expected_memory_gib": 64, "minimum_public_egress_mbps": 20}
    if any(not _same_typed(target.get(key), value) for key, value in expected.items()):
        raise ValueError("invalid_b11_target_resources")
    if (type(target["config_path"]) is not str or not re.fullmatch(
            r"/[A-Za-z0-9_./-]+", target["config_path"])
            or ".." in Path(target["config_path"]).parts
            or type(target["sfu_container"]) is not str
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", target["sfu_container"])):
        raise ValueError("invalid_b11_deployment_target")
    binding = [target[key] is None for key in ("instance", "service_url", "local_service_url")]
    if all(binding):
        if transport == "ssh":
            raise ValueError("incomplete_b11_remote_binding")
        return profile
    if any(binding) or type(target["instance"]) is not str:
        raise ValueError("incomplete_b11_remote_binding")
    instance = target["instance"]
    if transport == "ssh":
        if not re.fullmatch(r"ins-[A-Za-z0-9]+", instance):
            try:
                if not instance.startswith("ssh:"):
                    raise ValueError
                ipaddress.ip_address(instance[4:])
            except ValueError:
                raise ValueError("invalid_b11_ssh_instance") from None
    elif not re.fullmatch(r"i-[A-Za-z0-9]+", instance):
        raise ValueError("incomplete_b11_remote_binding")
    validate_service_url(target["service_url"])
    validate_service_url(target["local_service_url"], local=True)
    if urlsplit(target["service_url"]).port != urlsplit(target["local_service_url"]).port:
        raise ValueError("inconsistent_b11_service_ports")
    return profile


def _target_config_path(value: str, root: Path) -> Path:
    path = Path(value)
    return (path if path.is_absolute() else root / path).resolve()


def _ssh_transport_identity(config_path: Path) -> dict:
    import b11_remote
    return b11_remote.snapshot_identity(config_path)


def _remote_transport_identity(profile: dict, root: Path) -> dict:
    transport = profile.get("transport", "workbench")
    if transport != "ssh":
        return {"transport": transport}
    config_path = _target_config_path(profile["target_config"], root)
    identity = _ssh_transport_identity(config_path)
    target = profile["target"]
    if (identity.get("transport") != transport
            or identity.get("host") != urlsplit(target["service_url"]).hostname
            or target["instance"].startswith("ssh:")
                and identity.get("host") != target["instance"][4:]):
        raise ValueError("b11_ssh_target_mismatch")
    # The transport module exposes only paths and public fingerprints, never key bytes.
    return {"config_path": str(config_path), **identity}


def _file_record(path: Path) -> dict:
    path = path.resolve()
    if not path.is_file():
        raise ValueError("required_input_missing")
    before = path.stat()
    digest = sha256(path)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("input_changed_while_hashing")
    return {"path": str(path), "size": after.st_size, "sha256": digest}


def _git_head(root: Path) -> str:
    try:
        result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                                capture_output=True, text=True, check=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        raise ValueError("git_head_unavailable") from None
    value = result.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("git_head_unavailable")
    return value


def _source_paths(root: Path) -> list[Path]:
    paths = {root / name for name in TOOL_INPUTS}
    paths.update(root / name for name in ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json"))
    if (root / "vcpkg-configuration.json").is_file():
        paths.add(root / "vcpkg-configuration.json")
    for name in ("src", "cmake", "tests/cmake", "tests/runtime/probes", "tests/support"):
        directory = root / name
        if not directory.is_dir():
            raise ValueError("required_source_directory_missing")
        paths.update(path for path in directory.rglob("*") if path.is_file()
                     and path.suffix.lower() != ".md" and "__pycache__" not in path.parts)
    paths.add(root / "tests/CMakeLists.txt")
    return sorted(paths, key=lambda path: path.relative_to(root).as_posix())


def _binary_identity(executable: Path) -> dict:
    # Reuse the project PE CodeView check rather than trusting the enclosing directory name.
    import meeting_soak
    return meeting_soak.verify_runtime_binary(executable)


def _build_metadata(executable: Path, root: Path) -> dict:
    build = next((path for path in executable.parents if (path / "CMakeCache.txt").is_file()), None)
    if build is None:
        raise ValueError("binary_build_metadata_missing")
    cache = build / "CMakeCache.txt"
    text = cache.read_text(encoding="utf-8-sig")
    home = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", text, re.MULTILINE)
    configs = re.search(r"^CMAKE_CONFIGURATION_TYPES:STRING=(.+)$", text, re.MULTILINE)
    generator = re.search(r"^CMAKE_GENERATOR:INTERNAL=(.+)$", text, re.MULTILINE)
    if (not home or Path(home.group(1).strip()).resolve() != root
            or not configs or CONFIGURATION not in configs.group(1).strip().split(";")
            or not generator or not generator.group(1).startswith("Visual Studio ")):
        raise ValueError("binary_build_metadata_not_verified")
    projects = sorted(build.rglob("test_participant_window_remediation.vcxproj"))
    if len(projects) != 1:
        raise ValueError("binary_target_project_not_unique")
    project = projects[0]
    try:
        tree = ET.parse(project)
    except (OSError, ET.ParseError):
        raise ValueError("binary_target_project_unreadable") from None
    ns = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}
    output = None
    name = None
    extension = None
    for group in tree.findall("msbuild:PropertyGroup", ns):
        for node in group:
            if f"=='{CONFIGURATION}|" not in node.get("Condition", group.get("Condition", "")):
                continue
            key = node.tag.rsplit("}", 1)[-1]
            if key == "OutDir":
                output = node.text
            elif key == "TargetName":
                name = node.text
            elif key == "TargetExt":
                extension = node.text
    if (not output or name != Path(EXECUTABLE_NAME).stem or extension != ".exe"
            or (Path(output) / EXECUTABLE_NAME).resolve() != executable):
        raise ValueError("binary_target_output_not_verified")
    return {"configuration": CONFIGURATION, "build_directory": str(build),
            "generator": generator.group(1).strip(),
            "files": [_file_record(cache), _file_record(project)]}


def _snapshot(executable: Path, profile_path: Path, root: Path) -> dict:
    root = root.resolve()
    executable = executable.resolve()
    profile_path = profile_path.resolve()
    profile = validate_profile(_load_json(profile_path))
    remote_identity = _remote_transport_identity(profile, root)
    profile_record = _file_record(profile_path)
    # Check the parsed bytes and the recorded bytes describe one input.
    if profile != validate_profile(_load_json(profile_path)):
        raise ValueError("profile_changed_while_hashing")
    if not executable.is_file() or executable.name.casefold() != EXECUTABLE_NAME.casefold():
        raise ValueError("runtime_binary_missing_or_wrong_name")
    head = _git_head(root)
    identity = _binary_identity(executable)
    if identity.get("configuration") != CONFIGURATION:
        raise ValueError("runtime_binary_configuration_not_verified")
    metadata = _build_metadata(executable, root)
    pdb = executable.with_suffix(".pdb")
    # The target PDB and every DLL deployed beside this harness are immutable inputs.
    # Match DLL companion PDBs only; unrelated test-executable PDBs are not runtime dependencies.
    runtime_paths = {executable, pdb}
    dlls = sorted(executable.parent.rglob("*.dll"))
    for dll in dlls:
        runtime_paths.add(dll)
        if dll.with_suffix(".pdb").is_file():
            runtime_paths.add(dll.with_suffix(".pdb"))
    files = [_file_record(path) for path in sorted(runtime_paths)]
    if next(item["sha256"] for item in files if item["path"] == str(executable)) != identity.get("binary_sha256"):
        raise ValueError("runtime_binary_changed_while_hashing")
    sources = {path.relative_to(root).as_posix(): _file_record(path)
               for path in _source_paths(root)}
    if head != _git_head(root):
        raise ValueError("git_head_changed_while_hashing")
    return {"head": head, "repository": str(root), "profile": profile,
            "profile_file": profile_record, "source_inputs": sources,
            "binary_identity": identity, "build_metadata": metadata,
            "runtime_files": files, "remote_transport_identity": remote_identity}


def freeze_inputs(output: Path, executable: Path, profile: Path, *, root: Path = ROOT) -> dict:
    output = output.resolve()
    if output.exists():
        raise ValueError("freeze_manifest_already_exists")
    snapshot = _snapshot(executable, profile, root)
    manifest = {"schema": 1, "scope": SCOPE,
                "created_utc": datetime.now(timezone.utc).isoformat(),
                "local_inputs": "FROZEN", "runtime_status": "NOT_RUN",
                "binary_source_equivalence": "UNKNOWN",
                "remote_target": "PENDING" if snapshot["profile"]["target"]["instance"] is None
                    else "BOUND_NOT_VERIFIED",
                "remote_resource_verification": "NOT_RUN",
                "bandwidth_unit_confirmation": "UNKNOWN",
                "inputs": snapshot}
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2, ensure_ascii=False, allow_nan=False)
            stream.write("\n")
    except FileExistsError:
        raise ValueError("freeze_manifest_already_exists") from None
    return manifest


def verify_inputs(manifest: Path, executable: Path, profile: Path, *,
                  require_remote: bool = False, instance: str | None = None,
                  service_url: str | None = None, transport: str | None = None,
                  target_config: Path | None = None, root: Path = ROOT) -> dict:
    frozen = _load_json(manifest)
    if (not isinstance(frozen, dict) or set(frozen) != {
            "schema", "scope", "created_utc", "local_inputs", "runtime_status",
            "binary_source_equivalence", "remote_target", "remote_resource_verification",
            "bandwidth_unit_confirmation", "inputs"}
            or type(frozen.get("schema")) is not int
            or frozen["schema"] != 1 or frozen.get("scope") != SCOPE
            or frozen.get("local_inputs") != "FROZEN" or frozen.get("runtime_status") != "NOT_RUN"
            or frozen.get("binary_source_equivalence") != "UNKNOWN"
            or frozen.get("remote_resource_verification") != "NOT_RUN"
            or frozen.get("bandwidth_unit_confirmation") != "UNKNOWN"):
        raise ValueError("invalid_b11_manifest")
    current = _snapshot(executable, profile, root)
    if frozen.get("inputs") != current:
        raise ValueError("frozen_inputs_changed")
    target = current["profile"]["target"]
    expected_state = "PENDING" if target["instance"] is None else "BOUND_NOT_VERIFIED"
    if frozen.get("remote_target") != expected_state:
        raise ValueError("invalid_b11_remote_state")
    if require_remote and target["instance"] is None:
        raise ValueError("b11_remote_target_pending")
    if (instance is not None and instance != target["instance"]
            or service_url is not None and service_url != target["service_url"]
            or transport is not None and transport != current["profile"].get("transport", "workbench")
            or target_config is not None and (current["profile"].get("transport", "workbench") != "ssh"
                or _target_config_path(str(target_config), root)
                    != _target_config_path(current["profile"]["target_config"], root))):
        raise ValueError("b11_runtime_target_mismatch")
    return frozen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    freeze = subparsers.add_parser("freeze")
    freeze.add_argument("--output", type=Path, required=True)
    verify = subparsers.add_parser("verify")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--require-remote", action="store_true")
    verify.add_argument("--instance")
    verify.add_argument("--service-url")
    verify.add_argument("--transport", choices=("workbench", "ssh"))
    verify.add_argument("--target-config", type=Path)
    for command in (freeze, verify):
        command.add_argument("--executable", type=Path, required=True)
        command.add_argument("--profile", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "freeze":
            result = freeze_inputs(args.output, args.executable, args.profile)
        else:
            result = verify_inputs(args.manifest, args.executable, args.profile,
                                   require_remote=args.require_remote, instance=args.instance,
                                   service_url=args.service_url, transport=args.transport,
                                   target_config=args.target_config)
    except (ValueError, OSError) as error:
        # Never echo profile contents, service credentials, or a raw subprocess error.
        reason = str(error) if type(error) is ValueError and re.fullmatch(r"[a-z0-9_]+", str(error)) else "input_freeze_io_error"
        print(json.dumps({"status": "REJECTED", "reason": reason}), file=sys.stderr)
        return 2
    print(json.dumps({"status": "FROZEN" if args.command == "freeze" else "VERIFIED",
                      "scope": SCOPE, "remote_target": result["remote_target"],
                      "runtime_status": result["runtime_status"],
                      "binary_source_equivalence": result["binary_source_equivalence"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
