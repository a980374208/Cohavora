#!/usr/bin/env python3
"""Create and verify offline Cohavora update manifests using OpenSSL Ed25519."""

from __future__ import annotations

import argparse
import base64
import binascii
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit


ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")
MAX_COMPONENT = 2_147_483_647
MAX_PAYLOAD_BYTES = 48 * 1024
MAX_ENVELOPE_BYTES = 96 * 1024
MAX_NOTES_BYTES = 8 * 1024
MAX_URLS = 8
MAX_URL_BYTES = 4096
MAX_INSTALLER_BYTES = 2 * 1024 * 1024 * 1024
HTTP_PRIMARY_HOST = "123.56.225.164"
GITHUB_RELEASES = "https://github.com/a980374208/Cohavora/releases/download"


class ManifestError(ValueError):
    pass


def utf8_size(value: str) -> int:
    try:
        return len(value.encode("utf-8"))
    except UnicodeEncodeError as exc:
        raise ManifestError("text contains an invalid Unicode surrogate") from exc


def validate_version(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", value):
        raise ManifestError("version must be X.Y.Z with no leading zeroes")
    if any(len(part) > 10 or int(part) > MAX_COMPONENT for part in value.split(".")):
        raise ManifestError("each version component must be <= 2147483647")
    return value


def validate_url(value: str) -> str:
    if not isinstance(value, str) or not value or utf8_size(value) > MAX_URL_BYTES:
        raise ManifestError("each download URL must be nonempty and at most 4096 UTF-8 bytes")
    if "\\" in value or "#" in value or any(ord(c) <= 32 or ord(c) == 127 for c in value):
        raise ManifestError("download URL contains a fragment, backslash, or whitespace/control character")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as exc:
        raise ManifestError("malformed download URL") from exc
    if not parsed.hostname or "@" in parsed.netloc:
        raise ManifestError("download URLs must have a host and no credentials")
    if parsed.scheme == "https":
        if port is not None and port != 443:
            raise ManifestError("HTTPS download URLs must use port 443")
    elif parsed.scheme == "http":
        if parsed.netloc not in (HTTP_PRIMARY_HOST, f"{HTTP_PRIMARY_HOST}:80"):
            raise ManifestError("HTTP download URLs must use the configured primary host 123.56.225.164 on port 80")
        # Match the client's fixed static Web origin. ASCII object names avoid
        # percent-decoding, path normalization and proxy traversal ambiguity.
        if not parsed.path.startswith("/downloads/") or "?" in value:
            raise ManifestError("HTTP download URLs must name an object below /downloads/ with no query")
        segments = parsed.path[len("/downloads/"):].split("/")
        if any(not re.fullmatch(r"[A-Za-z0-9._-]+", segment) or segment in (".", "..") for segment in segments):
            raise ManifestError("HTTP object paths must use ASCII letters, digits, dot, underscore or hyphen without empty or dot segments")
    else:
        raise ManifestError("download URLs must use HTTPS or the fixed HTTP primary Web origin")
    return value


def validate_timestamp(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value):
        raise ManifestError("published_at must be UTC YYYY-MM-DDTHH:MM:SSZ")
    try:
        dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError as exc:
        raise ManifestError("published_at is not a valid UTC timestamp") from exc
    return value


def validate_notes(notes: object) -> str:
    if not isinstance(notes, str) or utf8_size(notes) > MAX_NOTES_BYTES:
        raise ManifestError("notes must be text of at most 8192 UTF-8 bytes")
    if any((ord(c) < 32 and c not in "\n\r\t") or ord(c) == 127 for c in notes):
        raise ManifestError("notes contains unsupported control characters")
    return notes


def validate_payload(payload: object) -> dict:
    if not isinstance(payload, dict):
        raise ManifestError("payload must be a JSON object")
    if type(payload.get("schema")) is not int or payload["schema"] != 1:
        raise ManifestError("schema must be 1")
    if payload.get("product") != "Cohavora" or payload.get("channel") != "stable":
        raise ManifestError("product/channel must be Cohavora/stable")
    validate_version(payload.get("version"))
    validate_timestamp(payload.get("published_at"))
    validate_notes(payload.get("notes"))
    # Legacy signed manifests omit this field and mean false. Do not coerce
    # strings, integers or null into an update policy.
    if type(payload.get("force_update", False)) is not bool:
        raise ManifestError("force_update must be a JSON boolean")
    artifact = payload.get("windows-x64")
    if not isinstance(artifact, dict):
        raise ManifestError("windows-x64 must be a JSON object")
    size = artifact.get("size")
    if type(size) is not int or not 0 < size <= MAX_INSTALLER_BYTES:
        raise ManifestError("installer size must be between 1 byte and 2 GiB")
    digest = artifact.get("sha256")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ManifestError("sha256 must be 64 lowercase hexadecimal characters")
    urls = artifact.get("urls")
    if not isinstance(urls, list) or not 1 <= len(urls) <= MAX_URLS:
        raise ManifestError("urls must contain between 1 and 8 download URLs")
    for url in urls:
        validate_url(url)
    if len(set(urls)) != len(urls):
        raise ManifestError("duplicate download URLs are not allowed")
    return payload


def run_openssl(executable: str, *args: str) -> bytes:
    try:
        result = subprocess.run(
            [executable, *args], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=30,
        )
    except FileNotFoundError as exc:
        raise ManifestError("OpenSSL not found; pass --openssl with its executable path") from exc
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ManifestError("OpenSSL could not complete the operation") from exc
    if result.returncode:
        # OpenSSL diagnostics contain no private key contents for these commands.
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise ManifestError(f"OpenSSL failed: {detail or 'nonzero exit status'}")
    return result.stdout


def public_key_hex(executable: str, private_key: Path) -> str:
    encoded = run_openssl(executable, "pkey", "-in", str(private_key), "-pubout", "-outform", "DER")
    if len(encoded) != len(ED25519_SPKI_PREFIX) + 32 or not encoded.startswith(ED25519_SPKI_PREFIX):
        raise ManifestError("the private key must be Ed25519")
    return encoded[len(ED25519_SPKI_PREFIX):].hex()


def reject_private_key_in_repository(path: Path) -> None:
    for parent in (path.parent, *path.parents):
        if (parent / ".git").exists():
            raise ManifestError("generate the private key outside a Git working tree")


def create_private_key(executable: str, path: Path) -> str:
    path = path.resolve()
    reject_private_key_in_repository(path)
    if path.exists():
        raise ManifestError("private key already exists; it will not be overwritten")
    # Do not create directories implicitly: the publisher chooses a protected location.
    encoded = run_openssl(executable, "genpkey", "-algorithm", "ED25519")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(encoded)
    return public_key_hex(executable, path)


def installer_identity(path: Path) -> tuple[int, str]:
    if not 0 < path.stat().st_size <= MAX_INSTALLER_BYTES:
        raise ManifestError("installer size must be between 1 byte and 2 GiB")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(chunk)
            if size > MAX_INSTALLER_BYTES:
                raise ManifestError("installer exceeds 2 GiB")
            digest.update(chunk)
    if not 0 < size <= MAX_INSTALLER_BYTES:
        raise ManifestError("installer size must be between 1 byte and 2 GiB")
    return size, digest.hexdigest()


def sign_payload(executable: str, private_key: Path, payload: bytes) -> bytes:
    # Ed25519 requires a seekable -in file; avoid stdin and signing a digest instead.
    with tempfile.TemporaryDirectory(prefix="cohavora-update-") as temporary:
        payload_path = Path(temporary) / "payload.json"
        payload_path.write_bytes(payload)
        signature = run_openssl(executable, "pkeyutl", "-sign", "-rawin", "-inkey", str(private_key), "-in", str(payload_path))
    if len(signature) != 64:
        raise ManifestError("OpenSSL did not return a 64-byte Ed25519 signature")
    return signature


def verify_signature(executable: str, key_hex: str, payload: bytes, signature: bytes) -> None:
    if not re.fullmatch(r"[0-9a-fA-F]{64}", key_hex):
        raise ManifestError("public key must be exactly 32 bytes as 64 hexadecimal characters")
    with tempfile.TemporaryDirectory(prefix="cohavora-update-") as temporary:
        directory = Path(temporary)
        key_path = directory / "public.der"
        payload_path = directory / "payload.json"
        signature_path = directory / "signature.bin"
        key_path.write_bytes(ED25519_SPKI_PREFIX + bytes.fromhex(key_hex))
        payload_path.write_bytes(payload)
        signature_path.write_bytes(signature)
        run_openssl(executable, "pkeyutl", "-verify", "-rawin", "-pubin", "-keyform", "DER", "-inkey", str(key_path), "-in", str(payload_path), "-sigfile", str(signature_path))


def atomic_write(path: Path, data: bytes) -> None:
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent, delete=False) as stream:
            temporary_path = Path(stream.name)
            stream.write(data)
        os.replace(temporary_path, path)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def release_settings(args: argparse.Namespace) -> dict:
    if args.config:
        with args.config.open("rb") as stream:
            encoded = stream.read(MAX_PAYLOAD_BYTES + 1)
        if len(encoded) > MAX_PAYLOAD_BYTES:
            raise ManifestError("release config exceeds 48 KiB")
        if encoded.startswith(b"\xef\xbb\xbf"):
            encoded = encoded[3:]
        config = load_json(encoded)
        if not isinstance(config, dict):
            raise ManifestError("release config must be a JSON object")
        required = {"latest_version", "force_update", "download_url"}
        optional = {"fallback_download_url", "release_notes", "published_at"}
        if not required <= set(config) or set(config) - required - optional:
            raise ManifestError("release config requires latest_version, force_update and download_url; optional fields are fallback_download_url, release_notes and published_at")
        version = validate_version(config["latest_version"])
        if type(config["force_update"]) is not bool:
            raise ManifestError("release config force_update must be a JSON boolean")
        primary = validate_url(config["download_url"])
        fallback = validate_url(config.get("fallback_download_url", f"{GITHUB_RELEASES}/v{version}/Cohavora-{version}-x64-Setup.exe"))
        if urlsplit(fallback).scheme != "https":
            raise ManifestError("fallback_download_url must use HTTPS")
        if primary == fallback:
            raise ManifestError("primary and fallback download URLs must differ")
        notes = validate_notes(config.get("release_notes", ""))
        published_at = validate_timestamp(config["published_at"]) if "published_at" in config else None
        return {"version": version, "force_update": config["force_update"],
                "urls": [primary, fallback], "notes": notes, "published_at": published_at}
    return {"version": validate_version(args.version), "force_update": bool(args.force_update),
            "urls": args.url, "notes": args.notes_file.read_text(encoding="utf-8-sig") if args.notes_file else (args.notes or ""),
            "published_at": args.published_at}


def create_manifest(args: argparse.Namespace) -> None:
    settings = release_settings(args)
    private_key = args.private_key.resolve()
    key_hex = public_key_hex(args.openssl, private_key)
    size, digest = installer_identity(args.installer)
    published_at = settings["published_at"] or dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    payload = validate_payload({
        "schema": 1, "product": "Cohavora", "channel": "stable",
        "version": settings["version"], "force_update": settings["force_update"],
        "published_at": published_at, "notes": settings["notes"],
        "windows-x64": {"size": size, "sha256": digest, "urls": settings["urls"]},
    })
    payload_bytes = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    if len(payload_bytes) > MAX_PAYLOAD_BYTES:
        raise ManifestError("payload exceeds 48 KiB")
    signature = sign_payload(args.openssl, private_key, payload_bytes)
    verify_signature(args.openssl, key_hex, payload_bytes, signature)
    envelope = {
        "payload": base64.b64encode(payload_bytes).decode("ascii"),
        "signature": base64.b64encode(signature).decode("ascii"),
    }
    output = (json.dumps(envelope, separators=(",", ":")) + "\n").encode("utf-8")
    if len(output) > MAX_ENVELOPE_BYTES:
        raise ManifestError("manifest exceeds 96 KiB")
    inputs = {args.installer.resolve(), private_key}
    if args.config:
        inputs.add(args.config.resolve())
    if args.notes_file:
        inputs.add(args.notes_file.resolve())
    if args.output.resolve() in inputs:
        raise ManifestError("output must not overwrite the installer, private key or input config/notes")
    atomic_write(args.output, output)
    print(f"Created {args.output}: Cohavora {settings['version']}, {size} bytes, SHA-256 {digest}")


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    output = {}
    for key, value in pairs:
        if key in output:
            raise ManifestError(f"duplicate JSON field: {key}")
        output[key] = value
    return output


def load_json(encoded: bytes) -> object:
    try:
        return json.loads(encoded.decode("utf-8"), object_pairs_hook=unique_object,
                          parse_constant=reject_json_constant)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ManifestError("manifest contains invalid UTF-8 JSON") from exc


def reject_json_constant(value: str) -> None:
    raise ManifestError(f"invalid JSON constant: {value}")


def decode_base64(value: object, field: str) -> bytes:
    if not isinstance(value, str):
        raise ManifestError(f"{field} must be base64 text")
    try:
        decoded = base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ManifestError(f"{field} contains invalid base64") from exc
    if base64.b64encode(decoded).decode("ascii") != value:
        raise ManifestError(f"{field} must use canonical base64")
    return decoded


def verify_manifest(args: argparse.Namespace) -> None:
    with args.manifest.open("rb") as stream:
        envelope_bytes = stream.read(MAX_ENVELOPE_BYTES + 1)
    if len(envelope_bytes) > MAX_ENVELOPE_BYTES:
        raise ManifestError("manifest exceeds 96 KiB")
    envelope = load_json(envelope_bytes)
    if not isinstance(envelope, dict) or set(envelope) != {"payload", "signature"}:
        raise ManifestError("manifest envelope must contain exactly payload and signature")
    payload_bytes = decode_base64(envelope.get("payload"), "payload")
    signature = decode_base64(envelope.get("signature"), "signature")
    if not payload_bytes or len(payload_bytes) > MAX_PAYLOAD_BYTES or len(signature) != 64:
        raise ManifestError("invalid payload/signature length")
    verify_signature(args.openssl, args.public_key_hex, payload_bytes, signature)
    payload = validate_payload(load_json(payload_bytes))
    if args.installer:
        size, digest = installer_identity(args.installer)
        artifact = payload["windows-x64"]
        if size != artifact["size"] or digest != artifact["sha256"]:
            raise ManifestError("installer size or SHA-256 does not match the signed manifest")
    suffix = " and local installer" if args.installer else ""
    print(f"Verified signed Cohavora {payload['version']} manifest{suffix}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openssl", default="openssl", help="OpenSSL 1.1.1+ executable (default: PATH)")
    commands = parser.add_subparsers(dest="command", required=True)
    keygen = commands.add_parser("keygen", help="generate an Ed25519 private key outside Git; print raw public key hex")
    keygen.add_argument("--private-key", type=Path, required=True)
    export = commands.add_parser("public-key", help="print raw Ed25519 public key hex from an existing private key")
    export.add_argument("--private-key", type=Path, required=True)
    create = commands.add_parser("create", help="hash an installer and create a signed latest.json")
    create.add_argument("--private-key", type=Path, required=True)
    create.add_argument("--installer", type=Path, required=True)
    create.add_argument("--config", type=Path, help="release-config.json with version, policy and ordered download sources")
    create.add_argument("--version")
    create.add_argument("--url", action="append", help="ordered HTTPS or fixed-primary HTTP installer URL; repeat for fallback")
    create.add_argument("--force-update", action="store_true", default=None, help="record force_update=true (client enforcement is deferred)")
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--published-at", help="UTC YYYY-MM-DDTHH:MM:SSZ (default: current UTC)")
    notes = create.add_mutually_exclusive_group()
    notes.add_argument("--notes", help="plain text release notes")
    notes.add_argument("--notes-file", type=Path, help="UTF-8 plain text release notes file")
    verify = commands.add_parser("verify", help="verify signature/schema and optionally a local installer")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--public-key-hex", required=True)
    verify.add_argument("--installer", type=Path)
    args = parser.parse_args(argv)
    if args.command == "create":
        if args.config:
            if any(getattr(args, field) is not None for field in ("version", "url", "force_update", "notes", "notes_file", "published_at")):
                parser.error("--config cannot be combined with --version, --url, --force-update, --notes, --notes-file or --published-at")
        elif args.version is None or args.url is None:
            parser.error("create requires --config or both --version and --url")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.command == "keygen":
            print(create_private_key(args.openssl, args.private_key))
        elif args.command == "public-key":
            print(public_key_hex(args.openssl, args.private_key))
        elif args.command == "create":
            create_manifest(args)
        elif args.command == "verify":
            verify_manifest(args)
    except (ManifestError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
