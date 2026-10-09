"""Bound SSH transport for B11; CLI output never includes SSH diagnostics or keys."""
from __future__ import annotations

import argparse
import base64
import hashlib
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[4]
FIELDS = {"schema", "transport", "host", "port", "user", "key_path", "known_hosts_path"}
MAX_METADATA_BYTES = 1024 * 1024


class RemoteError(ValueError):
    def __init__(self, reason: str, exit_code: int = 1):
        super().__init__(reason)
        self.reason = reason
        self.exit_code = exit_code if 1 <= exit_code <= 255 else 1


def _pairs(items):
    value = {}
    for key, item in items:
        if key in value:
            raise RemoteError("duplicate_target_config_key")
        value[key] = item
    return value


def _json(data: bytes):
    try:
        return json.loads(data.decode("utf-8-sig"), object_pairs_hook=_pairs,
                          parse_constant=lambda _: (_ for _ in ()).throw(
                              RemoteError("non_finite_json")))
    except (UnicodeError, json.JSONDecodeError):
        raise RemoteError("invalid_json_input") from None


def _local_file(value, reason: str) -> Path:
    if type(value) is not str or not value or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise RemoteError(reason)
    path = Path(value)
    if not path.is_absolute():
        raise RemoteError(reason)
    path = path.resolve()
    if not path.is_file():
        raise RemoteError(reason)
    return path


def validate_target_config(config: dict) -> dict:
    """Validate a closed credential-location schema without reading the key."""
    if (type(config) is not dict or set(config) != FIELDS
            or type(config.get("schema")) is not int or config["schema"] != 1
            or config.get("transport") != "ssh"
            or type(config.get("port")) is not int or not 1 <= config["port"] <= 65535
            or type(config.get("host")) is not str
            or type(config.get("user")) is not str
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]{0,31}", config["user"])):
        raise RemoteError("invalid_ssh_target_config")
    try:
        host = ipaddress.ip_address(config["host"])
        if host.is_unspecified or host.is_multicast or "%" in config["host"]:
            raise ValueError
    except ValueError:
        raise RemoteError("invalid_ssh_target_host") from None
    _local_file(config["key_path"], "ssh_key_file_missing_or_invalid")
    known_hosts = _local_file(config["known_hosts_path"], "ssh_known_hosts_missing_or_invalid")
    if not known_hosts.is_relative_to(ROOT.resolve()):
        raise RemoteError("ssh_known_hosts_outside_workspace")
    return dict(config)


def load_target_config(path: Path) -> dict:
    try:
        return validate_target_config(_json(Path(path).read_bytes()))
    except OSError:
        raise RemoteError("target_config_unreadable") from None


def _config(value: dict | Path) -> dict:
    return validate_target_config(value) if type(value) is dict else load_target_config(Path(value))


def _run(args: list[str], *, data: bytes | None = None, timeout: int = 90):
    try:
        result = subprocess.run(args, input=data, capture_output=True, timeout=timeout,
                                check=False, shell=False)
    except subprocess.TimeoutExpired:
        raise RemoteError("ssh_operation_timeout") from None
    except OSError:
        raise RemoteError("ssh_tool_unavailable") from None
    if result.returncode:
        raise RemoteError("ssh_operation_failed", result.returncode)
    return result


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def snapshot_identity(config: dict | Path) -> dict:
    """Freeze binding, public-key fingerprint and trusted hosts; no network access."""
    try:
        if type(config) is dict:
            value = validate_target_config(config)
            raw_config = json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=False).encode("utf-8")
        else:
            try:
                raw_config = Path(config).read_bytes()
            except OSError:
                raise RemoteError("target_config_unreadable") from None
            # Parse and hash the same bytes even if the path changes during keygen.
            value = validate_target_config(_json(raw_config))
        result = _run(["ssh-keygen", "-y", "-f", value["key_path"]], data=b"", timeout=10)
        parts = result.stdout.split()
        if len(parts) < 2 or not parts[0].startswith((b"ssh-", b"ecdsa-", b"sk-")):
            raise RemoteError("ssh_public_key_identity_unavailable")
        blob = base64.b64decode(parts[1], validate=True)
        if not blob:
            raise RemoteError("ssh_public_key_identity_unavailable")
        fingerprint = "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode("ascii").rstrip("=")
        return {"transport": "ssh", "host": value["host"], "port": value["port"],
                "user": value["user"], "key_path": value["key_path"],
                "known_hosts_path": value["known_hosts_path"],
                "config_sha256": hashlib.sha256(raw_config).hexdigest(),
                "key_public_fingerprint": fingerprint,
                "known_hosts_sha256": _hash_file(Path(value["known_hosts_path"]))}
    except (OSError, ValueError) as error:
        if isinstance(error, RemoteError):
            raise
        raise RemoteError("ssh_public_key_identity_unavailable") from None


def _options(config: dict, *, scp=False) -> list[str]:
    return ["-P" if scp else "-p", str(config["port"]), "-i", config["key_path"],
            "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "IdentitiesOnly=yes",
            "-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=" + config["known_hosts_path"],
            "-o", "GlobalKnownHostsFile=none"]


def _safe_metadata(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if re.search(r"secret|password|authorization|api_key|private_key|token", key, re.I):
                raise RemoteError("unsafe_remote_metadata")
            _safe_metadata(item)
    elif isinstance(value, list):
        for item in value:
            _safe_metadata(item)
    elif isinstance(value, str):
        if (any(ord(c) < 32 or ord(c) == 127 for c in value)
                or "PRIVATE KEY" in value or re.search(r"eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.", value)):
            raise RemoteError("unsafe_remote_metadata")


def execute(config: dict | Path, command: bytes) -> dict:
    """Run a caller-owned shell script via stdin and expose only JSON metadata."""
    value = _config(config)
    if type(command) is not bytes or not command or b"\0" in command:
        raise RemoteError("invalid_remote_command")
    try:
        command.decode("utf-8-sig")
    except UnicodeError:
        raise RemoteError("invalid_remote_command") from None
    result = _run(["ssh", "-T", *_options(value), value["user"] + "@" + value["host"], "sh -s"],
                  data=command)
    if len(result.stdout) > MAX_METADATA_BYTES:
        raise RemoteError("remote_metadata_too_large")
    metadata = _json(result.stdout)
    if type(metadata) is not dict:
        raise RemoteError("invalid_remote_metadata")
    _safe_metadata(metadata)
    return {"ok": True, "operation": "exec", "returncode": 0,
            "stdout": json.dumps(metadata, ensure_ascii=True, separators=(",", ":"))}


def _remote_path(value: str) -> str:
    if type(value) is not str or not re.fullmatch(r"/[A-Za-z0-9_./-]+", value):
        raise RemoteError("invalid_remote_transfer_path")
    parts = PurePosixPath(value).parts
    if ".." in parts or "." in value.split("/") or "//" in value or value.endswith("/"):
        raise RemoteError("invalid_remote_transfer_path")
    if (not re.fullmatch(r"/tmp/(?:soak-render|b11)-[A-Za-z0-9_-]+/[A-Za-z0-9_./-]+", value)
            and value != "/tmp/soak_low_bandwidth_publishers.py"):
        raise RemoteError("remote_transfer_outside_task_directory")
    return value


def _scp_target(config: dict, path: str) -> str:
    host = "[" + config["host"] + "]" if ":" in config["host"] else config["host"]
    return config["user"] + "@" + host + ":" + path


def upload(config: dict | Path, local: Path, remote: str, *, overwrite: bool = False) -> dict:
    value = _config(config)
    source = _local_file(str(Path(local)), "upload_source_missing_or_invalid")
    if source.samefile(Path(value["key_path"])):
        raise RemoteError("ssh_private_key_transfer_forbidden")
    destination = _remote_path(remote)
    if type(overwrite) is not bool:
        raise RemoteError("invalid_overwrite_flag")
    stage = destination + ".b11-stage-" + uuid.uuid4().hex
    _run(["scp", *_options(value, scp=True), str(source), _scp_target(value, stage)])
    script = ("python3 - <<'PY'\nimport json, os, pathlib\n"
              + "stage = pathlib.Path(" + repr(stage) + ")\n"
              + "destination = pathlib.Path(" + repr(destination) + ")\n"
              + "try:\n    stage.chmod(0o600)\n"
              + ("    os.replace(stage, destination)\n" if overwrite else "    os.link(stage, destination)\n")
              + "    print(json.dumps({'committed': True}))\n"
              + "finally:\n    stage.unlink(missing_ok=True)\nPY\n")
    execute(value, script.encode("utf-8"))
    return {"ok": True, "operation": "upload", "returncode": 0}


def download(config: dict | Path, remote: str, local: Path, *, overwrite: bool = False) -> dict:
    value = _config(config)
    source = _remote_path(remote)
    destination = Path(local)
    if (not destination.is_absolute() or not destination.parent.is_dir()
            or destination.is_symlink() or destination.is_dir() or type(overwrite) is not bool
            or any(ord(c) < 32 or ord(c) == 127 for c in str(destination))):
        raise RemoteError("invalid_download_destination")
    for protected in (Path(value["key_path"]), Path(value["known_hosts_path"])):
        if (destination.resolve() == protected.resolve()
                or destination.exists() and destination.samefile(protected)):
            raise RemoteError("ssh_identity_file_overwrite_forbidden")
    if destination.exists() and not overwrite:
        raise RemoteError("download_destination_exists")
    descriptor, stage_name = tempfile.mkstemp(prefix=".b11-download-", dir=destination.parent)
    os.close(descriptor)
    stage = Path(stage_name)
    try:
        _run(["scp", *_options(value, scp=True), _scp_target(value, source), str(stage)])
        if not stage.is_file():
            raise RemoteError("download_file_missing")
        stage.chmod(0o600)
        if overwrite:
            os.replace(stage, destination)
        else:
            os.link(stage, destination)
    except FileExistsError:
        raise RemoteError("download_destination_exists") from None
    except OSError:
        raise RemoteError("download_commit_failed") from None
    finally:
        stage.unlink(missing_ok=True)
    return {"ok": True, "operation": "download", "returncode": 0}


def _bound_cli_config(args) -> dict | Path:
    expected = {"config_sha256": args.expected_config_sha256,
                "known_hosts_sha256": args.expected_known_hosts_sha256,
                "key_public_fingerprint": args.expected_key_public_fingerprint}
    supplied = [value is not None for value in expected.values()]
    if not any(supplied):
        return args.target_config
    if not all(supplied):
        raise RemoteError("incomplete_expected_ssh_identity")
    if (any(not re.fullmatch(r"[0-9a-f]{64}", expected[field])
            for field in ("config_sha256", "known_hosts_sha256"))
            or not re.fullmatch(r"SHA256:[A-Za-z0-9+/]{43}", expected["key_public_fingerprint"])):
        raise RemoteError("invalid_expected_ssh_identity")
    identity = snapshot_identity(args.target_config)
    if any(identity.get(field) != value for field, value in expected.items()):
        raise RemoteError("frozen_ssh_identity_changed")
    # Keep the exact host, user and paths that passed verification for every operation.
    return {"schema": 1, **{field: identity[field] for field in FIELDS - {"schema"}}}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="operation", required=True)
    for operation in ("exec", "upload", "download"):
        command = subcommands.add_parser(operation)
        command.add_argument("--target-config", type=Path, required=True)
        command.add_argument("--expected-config-sha256")
        command.add_argument("--expected-known-hosts-sha256")
        command.add_argument("--expected-key-public-fingerprint")
        if operation == "exec":
            command.add_argument("--command-file", type=Path, required=True)
        else:
            command.add_argument("--local-path", type=Path, required=True)
            command.add_argument("--remote-path", required=True)
            command.add_argument("--overwrite", action="store_true")
    args = parser.parse_args(argv)
    try:
        config = _bound_cli_config(args)
        if args.operation == "exec":
            result = execute(config, args.command_file.read_bytes())
        elif args.operation == "upload":
            result = upload(config, args.local_path, args.remote_path, overwrite=args.overwrite)
        else:
            result = download(config, args.remote_path, args.local_path, overwrite=args.overwrite)
    except RemoteError as error:
        print(json.dumps({"ok": False, "operation": args.operation, "reason": error.reason}))
        return error.exit_code
    except OSError:
        print(json.dumps({"ok": False, "operation": args.operation, "reason": "local_input_unreadable"}))
        return 1
    print(json.dumps(result, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
