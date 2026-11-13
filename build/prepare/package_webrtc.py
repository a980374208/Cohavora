"""Build the pinned dual-configuration WebRTC SDK archive.

The archive reuses the verified LiveKit SDK header payload, replaces both
libraries and generated headers with the prefixed builds, and embeds the
inputs and verification evidence needed to reproduce the package.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from urllib.parse import urlsplit


ARCHIVE_PREFIX = "cohavora-webrtc-win-x64"
PACKAGE_ID = "webrtc-51ef663-cohavora-bssl-dual-v3"
PREFIX_NAME = "cohavora_bssl"
FIXED_ZIP_TIME = (2026, 9, 26, 0, 0, 0)
# Custom ZipInfo objects previously used zlib's default level (6), regardless
# of the ZipFile constructor's compresslevel. Keep that behavior explicit.
ZIP_COMPRESSION_LEVEL = 6
EXPECTED_LIBRARY_HASHES = {
    "Release": "5f0cf912e593c9a279ee61a7e41400e38d633d0a8921216bed76cfd00d8b9022",
    "Debug": "a6502024b585ad604c18cc1071c26a8ef6d75a565b97da824a700c67888ca2d5",
}
EXPECTED_ARGS_HASHES = {
    "Release": "b7832d4226d1a2e9e9f9de1ca70c6e27fd3536766fab086880e4d1a015b55b19",
    "Debug": "90543ecb6eada83ac2319d1fa803541acbf1d3ea1661456e63e4907cdd91ad7d",
}
DEBUG_ARGS_SNAPSHOT = (
    'target_os = "win"\r\n'
    'target_cpu = "x64"\r\n'
    '\r\n'
    'is_debug = true\r\n'
    'is_clang = true\r\n'
    'is_component_build = false\r\n'
    'use_custom_libcxx = false\r\n'
    'enable_iterator_debugging = true\r\n'
    'symbol_level = 2\r\n'
    '\r\n'
    'rtc_include_tests = false\r\n'
    'rtc_build_examples = false\r\n'
    'rtc_build_tools = false\r\n'
    'rtc_enable_protobuf = false\r\n'
    'rtc_use_h264 = true\r\n'
    'ffmpeg_branding = "Chrome"\r\n'
    'rtc_libvpx_build_vp9 = true\r\n'
    'enable_libaom = true\r\n'
    '\r\n'
    'use_siso = false\r\n'
    'use_remoteexec = false\r\n'
    '\r\n'
    'boringssl_symbol_prefix = "cohavora_bssl"\r\n'
    'boringssl_prefix_headers = "//out/cohavora-boringssl-prefix-debug/include"'
).encode("utf-8")
COMMON_REQUIRED_FILES = (
    "include/api/peer_connection_interface.h",
    "include/api/video/i420_buffer.h",
    "include/third_party/libyuv/include/libyuv.h",
    "include/third_party/boringssl/src/include/openssl/ssl.h",
)
CONFIGURATION_REQUIRED_FILES = (
    "lib/{config}/webrtc.lib",
    "include/boringssl-prefix/{config}/boringssl_prefix_symbols.h",
    "include/boringssl-prefix/{config}/boringssl_prefix_symbols_asm.h",
    "include/boringssl-prefix/{config}/boringssl_prefix_symbols_nasm.inc",
    "include/out-x64-{config_lower}/gen/build/chromeos_buildflags.h",
    "licenses/{config}/LICENSE.md",
    "metadata/build/{config}/args.gn",
    "metadata/evidence/{config}/verification.json",
)


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bytes_hash(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def run(command, cwd: Path) -> bytes:
    return subprocess.check_output(command, cwd=cwd, stderr=subprocess.STDOUT)


def git_revision(repository: Path) -> str:
    return run(["git", "rev-parse", "HEAD"], repository).decode().strip()


def git_diff(repository: Path, relative_path: str) -> bytes:
    return run(["git", "diff", "--", relative_path], repository)


def effective_gn_args(python: Path, depot_tools: Path, source: Path,
                      args_snapshot: bytes) -> bytes:
    environment = os.environ.copy()
    environment["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
    with tempfile.TemporaryDirectory(
            prefix=".cohavora-package-gn-", dir=source / "out") as temp_dir:
        output = Path(temp_dir)
        (output / "args.gn").write_bytes(args_snapshot)
        subprocess.check_output(
            [str(python), str(depot_tools / "gn.py"), "gen", str(output)],
            cwd=source,
            env=environment,
            stderr=subprocess.STDOUT,
        )
        return subprocess.check_output(
            [str(python), str(depot_tools / "gn.py"), "args", str(output),
             "--list", "--short"],
            cwd=source,
            env=environment,
            stderr=subprocess.STDOUT,
        )


def boringssl_flags(output: Path) -> bytes:
    records = []
    ninja_root = output / "obj/third_party/boringssl"
    for ninja in sorted(ninja_root.glob("*.ninja")):
        matching = []
        for line in ninja.read_text(encoding="utf-8", errors="replace").splitlines():
            if (line.startswith(("defines =", "include_dirs =", "cflags =",
                                 "cflags_c =", "cflags_cc ="))
                    and ("BORINGSSL" in line or ninja.name == "boringssl.ninja")):
                matching.append(line)
        if matching:
            records.append(f"[{ninja.name}]")
            records.extend(matching)
    for ninja in sorted(output.glob("*.ninja")):
        for line in ninja.read_text(encoding="utf-8", errors="replace").splitlines():
            if "BORINGSSL_PREFIX" in line or "cohavora-boringssl-prefix" in line:
                records.append(f"[{ninja.name}] {line}")
    return ("\n".join(records) + "\n").encode("utf-8")


def add_file(mapping, archive_path: str, source_path: Path):
    mapping[archive_path.replace("\\", "/")] = ("file", source_path)


def add_bytes(mapping, archive_path: str, content: bytes):
    mapping[archive_path.replace("\\", "/")] = ("bytes", content)


def source_hash(source) -> str:
    kind, value = source
    if kind == "file":
        return file_hash(value)
    if kind == "zip":
        archive, member = value
        digest = hashlib.sha256()
        with archive.open(member, "r") as input_file:
            for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    return bytes_hash(value)


def required_fingerprint(required_files, file_hashes) -> str:
    digest = hashlib.sha256()
    for relative in required_files:
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_hashes[relative].encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest()


def zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, FIXED_ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    # Python 3.8 has no public per-member compression-level property.
    info._compresslevel = ZIP_COMPRESSION_LEVEL
    info.external_attr = 0o100644 << 16
    info.create_system = 3
    return info


def write_source(archive: zipfile.ZipFile, name: str, source):
    kind, value = source
    with archive.open(zip_info(name), "w", force_zip64=True) as destination:
        if kind == "file":
            with value.open("rb") as input_file:
                shutil.copyfileobj(input_file, destination, 1024 * 1024)
        elif kind == "zip":
            base_archive, member = value
            with base_archive.open(member, "r") as input_file:
                shutil.copyfileobj(input_file, destination, 1024 * 1024)
        else:
            destination.write(value)


def discover_base_prefix(archive: zipfile.ZipFile) -> str:
    anchor = "include/api/peer_connection_interface.h"
    prefixes = {
        member.filename.replace("\\", "/")[:-len(anchor)]
        for member in archive.infolist()
        if not member.is_dir()
        and member.filename.replace("\\", "/").endswith(anchor)
    }
    if len(prefixes) != 1:
        raise ValueError("Base archive has no unique WebRTC SDK root")
    return prefixes.pop()


def base_entries(archive: zipfile.ZipFile):
    excluded_roots = (
        "lib/",
        "include/out-x64-release/gen/",
    )
    excluded_files = {
        "args.gn",
        "desktop_capture.ninja",
        "LICENSE.md",
        "webrtc.ninja",
    }
    prefix = discover_base_prefix(archive)
    seen = set()
    for member in archive.infolist():
        if member.is_dir():
            continue
        name = member.filename.replace("\\", "/")
        if not name.startswith(prefix):
            continue
        relative = name[len(prefix):]
        if (not relative or name.startswith("/") or ":" in name
                or any(part in ("", ".", "..") for part in name.split("/"))):
            raise ValueError(f"Unsafe base archive member: {member.filename}")
        if relative in excluded_files or relative.startswith(excluded_roots):
            continue
        normalized = relative.casefold()
        if normalized in seen:
            raise ValueError(f"Duplicate base archive member: {member.filename}")
        seen.add(normalized)
        yield relative, ("zip", (archive, member))


def build_package(args):
    if getattr(args, "sdk_root", None):
        return build_prepared_package(args)
    base_archive = args.base_archive.resolve()
    if not base_archive.is_file():
        raise ValueError(f"Missing package input: {base_archive}")
    # Keep the source ZIP open while hashes and output members stream from it.
    with zipfile.ZipFile(base_archive, "r") as base:
        return build_package_from_base(args, base)


def json_bytes(value) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def prepared_relative(path: Path, root: Path) -> str:
    relative = path.relative_to(root).as_posix()
    if (path.is_symlink() or ":" in relative or "\\" in relative
            or any(part in ("", ".", "..") for part in relative.split("/"))):
        raise ValueError(f"Unsafe SDK payload path: {relative}")
    return relative


def public_gn_args(content: str) -> bytes:
    """Keep compile switches without publishing machine-local dependency paths."""
    path_names = {
        "rtc_nvenc_include_dir": "NVENC_INCLUDE_DIR",
        "rtc_qsv_include_dir": "VPL_INCLUDE_DIR",
        "rtc_qsv_vpl_library": "VPL_STATIC_LIBRARY",
        "rtc_x265_include_dir": "X265_INCLUDE_DIR",
        "rtc_x265_library": "X265_STATIC_LIBRARY",
        "rtc_x265_license_file": "X265_LICENSE_FILE",
        "rtc_amf_include_dir": "AMF_INCLUDE_DIR",
        "rtc_amf_test_ffmpeg_include_dir": "FFMPEG_INCLUDE_DIR",
    }
    lines = []
    for line in content.splitlines():
        match = re.match(r'^(\s*)(\w+)\s*=\s*"([^"\n]*)"\s*$', line)
        if match:
            indent, name, value = match.groups()
            if re.match(r"^[A-Za-z]:[/\\]", value) or (
                    value.startswith(("/", "\\")) and not value.startswith("//")):
                token = path_names.get(name, name.upper())
                line = f'{indent}{name} = "@{token}@"'
        if re.search(r"[A-Za-z]:[/\\]", line):
            raise ValueError("Unrecognized machine-local path in GN snapshot")
        lines.append(line)
    return ("\n".join(lines) + "\n").encode("utf-8")


def write_checked_source(archive, name, source, expected_hash):
    digest = hashlib.sha256()
    with archive.open(zip_info(name), "w", force_zip64=True) as destination:
        if source[0] == "file":
            with source[1].open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
                    destination.write(block)
        else:
            digest.update(source[1])
            destination.write(source[1])
    if digest.hexdigest() != expected_hash:
        raise ValueError(f"SDK input changed during packaging: {name}")


def public_source_equivalence(path, args, source, profile):
    receipt = json.loads(path.read_text(encoding="utf-8-sig"))
    allowed = {
        "schema_version", "status", "scope", "source_repo", "source_branch",
        "source_revision", "build_receipt_revision", "release_tag", "sdk_package_id",
        "original_sdk_package_id", "explanation", "commit_changed_files", "checked_files",
        "counts", "checks", "configurations", "new_build", "native_unit_execution",
        "new_runtime_validation",
    }
    if not isinstance(receipt, dict) or set(receipt) - allowed:
        raise ValueError("Unknown fields in public source equivalence receipt")
    if (receipt.get("schema_version") != 1 or receipt.get("status") != "PASS"
            or receipt.get("source_revision") != source["webrtc"]
            or receipt.get("build_receipt_revision") != source.get("build_receipt_revision")
            or receipt.get("source_repo") != args.source_url
            or receipt.get("source_branch") != args.source_branch
            or receipt.get("release_tag") != args.release_tag
            or receipt.get("sdk_package_id") != args.package_id):
        raise ValueError("Source equivalence receipt does not match this release")
    def check_text(value):
        if isinstance(value, dict):
            for key, item in value.items():
                check_text(key)
                check_text(item)
        elif isinstance(value, list):
            for item in value:
                check_text(item)
        elif isinstance(value, str) and re.search(r"(?<![A-Za-z0-9_])[A-Za-z]:[/\\]|\\\\|file://", value):
            raise ValueError("Machine-local path in public source equivalence receipt")
    check_text(receipt)
    counts = receipt.get("counts")
    checks = receipt.get("checks")
    if (not isinstance(counts, dict) or not counts or any(type(value) is not int or value < 0 for value in counts.values())
            or not isinstance(checks, dict) or not checks or any(value is not True for value in checks.values())):
        raise ValueError("Source equivalence checks are incomplete")
    if any(value for name, value in counts.items() if "mismatch" in name):
        raise ValueError("Source equivalence receipt reports mismatched inputs")
    checked = receipt.get("checked_files")
    if not isinstance(checked, list) or not checked:
        raise ValueError("Source equivalence receipt has no checked files")
    for item in checked:
        if not isinstance(item, dict) or set(item) != {"path", "sha256"}:
            raise ValueError("Invalid checked source file record")
        relative = item["path"]
        if (not isinstance(relative, str) or relative.startswith(("/", "\\"))
                or ":" in relative or "\\" in relative
                or any(part in ("", ".", "..") for part in relative.split("/"))
                or not re.fullmatch(r"[0-9a-f]{64}", item["sha256"])):
            raise ValueError("Unsafe checked source file record")
        if file_hash(args.source_root.resolve() / relative) != item["sha256"]:
            raise ValueError(f"Source equivalence input has changed: {relative}")
    configurations = receipt.get("configurations", {})
    if set(configurations) != {"Debug", "Release"}:
        raise ValueError("Source equivalence receipt requires both configurations")
    config_fields = {"existing_build_status", "library_sha256", "current_build_library_matches_sdk",
                     "runtime_library", "iterator_debug_level", "x265_sha256", "vpl_sha256"}
    for name, cfg in configurations.items():
        expected = profile["configurations"][name]
        if (set(cfg) != config_fields or cfg["existing_build_status"] != "PASS"
                or cfg["current_build_library_matches_sdk"] is not True
                or any(cfg[key] != expected[key] for key in (
                    "library_sha256", "runtime_library", "iterator_debug_level"))
                or any(cfg[key + "_sha256"] != expected["external_static_libraries"][key]["sha256"]
                       for key in ("x265", "vpl"))):
            raise ValueError(f"Source equivalence receipt configuration mismatch: {name}")
    return json_bytes(receipt)


def build_prepared_package(args):
    """Publish an existing dual-config codec SDK without its private ledgers."""
    sdk_root = args.sdk_root.resolve()
    source_root = args.source_root.resolve()
    output = args.output.resolve()
    metadata_output = args.metadata_output.resolve()
    if (output == metadata_output or output == sdk_root or sdk_root in output.parents
            or metadata_output == sdk_root or sdk_root in metadata_output.parents):
        raise ValueError("Archive and metadata outputs must be distinct and outside the SDK")
    for value in (args.package_id, args.release_tag, args.source_branch):
        if not re.fullmatch(r"[A-Za-z0-9_.\-/]+", value) or ".." in value:
            raise ValueError("Invalid package, release or branch identifier")
    source_url = urlsplit(args.source_url)
    if (source_url.scheme != "https" or source_url.hostname != "github.com"
            or source_url.username or source_url.password or source_url.query
            or source_url.fragment):
        raise ValueError("Source URL must be a public GitHub HTTPS repository URL")
    profile = json.loads((sdk_root / "metadata/package.json").read_text(encoding="utf-8-sig"))
    if (profile.get("schema_version") != 1 or profile.get("boringssl_prefix") != PREFIX_NAME
            or profile.get("compile_definitions") != ["RTC_ENABLE_H265"]
            or set(profile.get("configurations", {})) != {"Debug", "Release"}):
        raise ValueError("Prepared SDK profile is not a dual-config H265/prefixed SDK")
    if run(["git", "status", "--porcelain", "--untracked-files=no"], source_root).strip():
        raise ValueError("Source checkout has uncommitted tracked changes")
    revision = git_revision(source_root)
    source = {
        "url": args.source_url,
        "branch": args.source_branch,
        "webrtc": revision,
        "build": git_revision(source_root / "build"),
        "third_party": git_revision(source_root / "third_party"),
        "boringssl": git_revision(source_root / "third_party/boringssl/src"),
        "deps_sha256": file_hash(source_root / "DEPS"),
    }
    provenance_path = sdk_root / "metadata/provenance.json"
    provenance = (json.loads(provenance_path.read_text(encoding="utf-8-sig"))
                  if provenance_path.is_file() else {})
    historical_revision = provenance.get("source_revision")
    if historical_revision:
        if not re.fullmatch(r"[0-9a-fA-F]{40}", historical_revision):
            raise ValueError("Invalid historical source revision")
        source["build_receipt_revision"] = historical_revision

    entries = {}
    seen = set()
    for directory in ("include", "lib", "licenses"):
        root = sdk_root / directory
        if not root.is_dir() or root.is_symlink():
            raise ValueError(f"Missing or linked SDK payload directory: {directory}")
        for path in sorted(root.rglob("*")):
            if path.is_symlink():
                raise ValueError("Linked files are not allowed in the SDK payload")
            if not path.is_file():
                continue
            relative = prepared_relative(path, sdk_root)
            if relative.casefold() in seen:
                raise ValueError(f"Duplicate SDK payload path: {relative}")
            seen.add(relative.casefold())
            add_file(entries, relative, path)
    file_hashes = {name: source_hash(value) for name, value in sorted(entries.items())}
    frozen_files = provenance.get("files", {})
    for relative, digest in file_hashes.items():
        if frozen_files and frozen_files.get(relative, {}).get("sha256", "").lower() != digest:
            raise ValueError(f"Prepared SDK differs from its frozen payload: {relative}")
    equivalence_path = getattr(args, "source_equivalence", None)
    if equivalence_path:
        content = public_source_equivalence(equivalence_path, args, source, profile)
        add_bytes(entries, "metadata/source-equivalence.json", content)
        file_hashes["metadata/source-equivalence.json"] = bytes_hash(content)
        source["equivalence_receipt_sha256"] = bytes_hash(content)
    elif historical_revision and historical_revision != revision:
        raise ValueError("Changed source revision requires a reviewed --source-equivalence receipt")

    configuration_data = {}
    required_files = list(COMMON_REQUIRED_FILES)
    for name in ("Debug", "Release"):
        cfg = dict(profile["configurations"][name])
        expected_abi = ("MTd_StaticDebug", 2) if name == "Debug" else ("MT_StaticRelease", 0)
        if (cfg.get("runtime_library"), cfg.get("iterator_debug_level")) != expected_abi:
            raise ValueError(f"Incorrect runtime/iterator ABI: {name}")
        library = f"lib/{name}/webrtc.lib"
        if file_hashes.get(library) != cfg.get("library_sha256", "").lower():
            raise ValueError(f"Prepared {name} WebRTC library hash mismatch")
        external = cfg.get("external_static_libraries", {})
        if set(external) != {"x265", "vpl"}:
            raise ValueError(f"Missing {name} x265/oneVPL static libraries")
        for entry in external.values():
            relative = entry.get("relative_path", "")
            if (not relative.startswith(f"lib/{name}/") or ".." in relative.split("/")
                    or file_hashes.get(relative) != entry.get("sha256", "").lower()):
                raise ValueError(f"Invalid {name} external static library")
            required_files.append(relative)
        # Reconstruct the public configuration, never forward arbitrary metadata.
        configuration_data[name] = {
            "library_sha256": file_hashes[library],
            "library_size": (sdk_root / library).stat().st_size,
            "runtime_library": expected_abi[0],
            "iterator_debug_level": expected_abi[1],
            "is_debug": name == "Debug",
            "symbol_level": 2 if name == "Debug" else 0,
            "external_static_libraries": {
                key: {"relative_path": entry["relative_path"],
                      "sha256": file_hashes[entry["relative_path"]]}
                for key, entry in sorted(external.items())},
        }
        args_path = sdk_root / f"metadata/build/{name}/args.gn"
        args_bytes = public_gn_args(args_path.read_text(encoding="utf-8-sig"))
        relative = f"metadata/build/{name}/args.gn"
        add_bytes(entries, relative, args_bytes)
        file_hashes[relative] = bytes_hash(args_bytes)
        configuration_data[name]["args_gn_sha256"] = file_hashes[relative]
        configuration_data[name]["original_args_gn_sha256"] = file_hash(args_path)
        verification = {
            "scope": "Existing build reuse and payload hash validation only",
            "status": "PACKAGING_ONLY",
            "library_sha256": file_hashes[library],
            "symbol_verification": "NOT_RUN_FOR_THIS_RELEASE",
            "clean_checkout_rebuild": "NOT_RUN",
            "runtime": "NOT_RUN_FOR_THIS_RELEASE",
        }
        relative = f"metadata/evidence/{name}/verification.json"
        content = json_bytes(verification)
        add_bytes(entries, relative, content)
        file_hashes[relative] = bytes_hash(content)
        required_files.extend(template.format(config=name, config_lower=name.lower())
                              for template in CONFIGURATION_REQUIRED_FILES)
    missing = sorted(set(required_files) - set(entries))
    if missing:
        raise ValueError("Prepared SDK is missing required files: " + ", ".join(missing))
    required_hashes = {relative: file_hashes[relative] for relative in required_files}
    manifest = {
        "schema_version": 1,
        "package_id": args.package_id,
        "archive_prefix": ARCHIVE_PREFIX,
        "platform": "windows-x64",
        "boringssl_prefix": PREFIX_NAME,
        "compile_definitions": ["RTC_ENABLE_H265"],
        "source": source,
        "configurations": configuration_data,
        "required_files": required_files,
        "required_file_hashes": required_hashes,
        "required_files_sha256": required_fingerprint(required_files, required_hashes),
        "files": dict(sorted(file_hashes.items())),
        "scope": "Existing dual-config SDK archive; clean rebuild and runtime NOT_RUN",
    }
    manifest_bytes = json_bytes(manifest)
    add_bytes(entries, "metadata/package.json", manifest_bytes)
    file_hashes["metadata/package.json"] = bytes_hash(manifest_bytes)
    checksum_bytes = "".join(f"{digest}  {name}\n" for name, digest in sorted(file_hashes.items())).encode("utf-8")
    add_bytes(entries, "metadata/checksums.sha256", checksum_bytes)
    file_hashes["metadata/checksums.sha256"] = bytes_hash(checksum_bytes)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    try:
        with zipfile.ZipFile(temporary, "w", allowZip64=True,
                             compression=zipfile.ZIP_DEFLATED,
                             compresslevel=ZIP_COMPRESSION_LEVEL) as archive:
            for relative, entry in sorted(entries.items()):
                write_checked_source(archive, f"{ARCHIVE_PREFIX}/{relative}", entry, file_hashes[relative])
        if (git_revision(source_root) != revision or
                run(["git", "status", "--porcelain", "--untracked-files=no"], source_root).strip()):
            raise ValueError("Source checkout changed during packaging")
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    metadata = {key: manifest[key] for key in (
        "schema_version", "package_id", "archive_prefix", "boringssl_prefix",
        "compile_definitions", "source", "configurations", "required_files", "required_files_sha256")}
    metadata.update({
        "archive": output.name,
        "download_url": f"https://github.com/a980374208/Cohavora/releases/download/{args.release_tag}/{output.name}",
        "size": output.stat().st_size,
        "sha256": file_hash(output),
        "file_count": len(entries),
        "manifest_sha256": bytes_hash(manifest_bytes),
    })
    metadata_output.parent.mkdir(parents=True, exist_ok=True)
    metadata_temporary = metadata_output.with_name(f".{metadata_output.name}.{os.getpid()}.tmp")
    try:
        metadata_temporary.write_bytes(json_bytes(metadata))
        os.replace(metadata_temporary, metadata_output)
    finally:
        metadata_temporary.unlink(missing_ok=True)
    print(json.dumps({"archive": str(output), "metadata": str(metadata_output),
                      "size": metadata["size"], "sha256": metadata["sha256"],
                      "file_count": metadata["file_count"]}, indent=2))
    return metadata


def build_package_from_base(args, base):
    source_root = args.source_root.resolve()
    release_out = args.release_out.resolve()
    debug_out = args.debug_out.resolve()
    release_work = args.release_work.resolve()
    debug_work = args.debug_work.resolve()
    base_archive = args.base_archive.resolve()
    output = args.output.resolve()

    inputs = (
        base_archive,
        release_out / "obj/webrtc.lib",
        debug_out / "obj/webrtc.lib",
        release_out / "args.gn",
        debug_out / "args.gn",
        release_out / "LICENSE.md",
        debug_out / "LICENSE.md",
        release_work / "verification.json",
        release_work / "verification-union.json",
        debug_work / "verification-debug.json",
    )
    missing = [str(path) for path in inputs if not path.is_file()]
    if missing:
        raise ValueError("Missing package inputs:\n  - " + "\n  - ".join(missing))

    entries = {}
    for relative, source in base_entries(base):
        entries[relative] = source

    configuration_data = {}
    for name, output_dir, work_dir, license_path in (
            ("Release", release_out, release_work, release_out / "LICENSE.md"),
            ("Debug", debug_out, debug_work, debug_out / "LICENSE.md")):
        lower = name.lower()
        library_path = output_dir / "obj/webrtc.lib"
        library_sha256 = file_hash(library_path)
        if library_sha256 != EXPECTED_LIBRARY_HASHES[name]:
            raise ValueError(
                f"{name} WebRTC library hash does not match the verified input")
        args_path = output_dir / "args.gn"
        args_bytes = (
            DEBUG_ARGS_SNAPSHOT if name == "Debug" else args_path.read_bytes()
        )
        args_sha256 = bytes_hash(args_bytes)
        if args_sha256 != EXPECTED_ARGS_HASHES[name]:
            raise ValueError(
                f"{name} args snapshot hash does not match the verified input")
        observed_args_hash = file_hash(args_path)
        if observed_args_hash != args_sha256:
            print(
                f"[WARN] {name} args.gn changed after the verified build; "
                f"packaging checkpoint snapshot {args_sha256} instead of "
                f"{observed_args_hash}",
                file=sys.stderr,
            )
        add_file(entries, f"lib/{name}/webrtc.lib", library_path)
        add_file(entries, f"licenses/{name}/LICENSE.md", license_path)
        add_bytes(entries, f"metadata/build/{name}/args.gn", args_bytes)
        add_bytes(
            entries,
            f"metadata/build/{name}/effective-args.txt",
            effective_gn_args(
                args.python, args.depot_tools, source_root, args_bytes),
        )
        add_bytes(entries, f"metadata/build/{name}/boringssl-flags.txt",
                  boringssl_flags(output_dir))
        for generated in sorted((output_dir / "gen").rglob("*.h")):
            relative = generated.relative_to(output_dir / "gen").as_posix()
            add_file(entries, f"include/out-x64-{lower}/gen/{relative}", generated)
        prefix_hashes = {}
        for header in (
                "boringssl_prefix_symbols.h",
                "boringssl_prefix_symbols_asm.h",
                "boringssl_prefix_symbols_nasm.inc"):
            header_path = work_dir / "include" / header
            add_file(entries, f"include/boringssl-prefix/{name}/{header}", header_path)
            prefix_hashes[header] = file_hash(header_path)

        evidence_files = (
            ("symbols.txt", work_dir / ("symbols.txt" if name == "Release"
                                         else "symbols-debug.txt")),
            ("symbols-raw.txt", work_dir / ("symbols-raw.txt" if name == "Release"
                                             else "symbols-debug-raw.txt")),
            ("symbols-excluded.txt", work_dir / (
                "symbols.txt.excluded.txt" if name == "Release"
                else "symbols-debug.txt.excluded.txt")),
            ("new-webrtc-symbols.txt", work_dir / (
                "new-webrtc-symbols.txt" if name == "Release"
                else "new-webrtc-debug-symbols.txt")),
            ("verification.json", work_dir / (
                "verification.json" if name == "Release"
                else "verification-debug.json")),
        )
        evidence_hashes = {}
        for archive_name, evidence_path in evidence_files:
            add_file(entries, f"metadata/evidence/{name}/{archive_name}", evidence_path)
            evidence_hashes[archive_name] = file_hash(evidence_path)
        if name == "Release":
            add_file(entries, "metadata/evidence/Release/verification-union.json",
                     work_dir / "verification-union.json")
            evidence_hashes["verification-union.json"] = file_hash(
                work_dir / "verification-union.json")
        else:
            add_file(entries, "metadata/evidence/Debug/symbols-union.txt",
                     work_dir / "symbols-union.txt")
            evidence_hashes["symbols-union.txt"] = file_hash(
                work_dir / "symbols-union.txt")

        configuration_data[name] = {
            "args_gn_sha256": args_sha256,
            "is_debug": name == "Debug",
            "iterator_debug_level": 2 if name == "Debug" else 0,
            "library_sha256": library_sha256,
            "library_size": library_path.stat().st_size,
            "license_sha256": file_hash(license_path),
            "prefix_headers": prefix_hashes,
            "runtime_library": "MTd_StaticDebug" if name == "Debug"
                               else "MT_StaticRelease",
            "symbol_level": 2 if name == "Debug" else 0,
            "verification": evidence_hashes,
        }

    patch_files = tuple(sorted(args.patches_dir.glob("*.patch")))
    expected_patch_names = {
        "add_deps.patch",
        "add_licenses.patch",
        "external_audio_source.patch",
        "ssl_verify_callback_with_native_handle.patch",
        "windows_silence_warnings.patch",
    }
    selected_patches = [path for path in patch_files if path.name in expected_patch_names]
    if {path.name for path in selected_patches} != expected_patch_names:
        raise ValueError("The five LiveKit patch inputs are incomplete")
    patch_hashes = {}
    for patch in selected_patches:
        add_file(entries, f"metadata/patches/livekit/{patch.name}", patch)
        patch_hashes[patch.name] = file_hash(patch)

    upstream_patch_names = {
        "0001-fix-audio-copy-red-iterator-underflow.patch",
    }
    upstream_patches = tuple(sorted(args.upstream_patches_dir.glob("*.patch")))
    selected_upstream_patches = [
        path for path in upstream_patches if path.name in upstream_patch_names
    ]
    if {path.name for path in selected_upstream_patches} != upstream_patch_names:
        raise ValueError("The AudioEncoderCopyRed upstream backport is missing")
    for patch in selected_upstream_patches:
        add_file(entries, f"metadata/patches/upstream/{patch.name}", patch)
        patch_hashes[f"upstream/{patch.name}"] = file_hash(patch)

    boringssl_diff = git_diff(source_root / "third_party", "boringssl/BUILD.gn")
    compiler_diff = git_diff(source_root / "build", "config/compiler/BUILD.gn")
    if not boringssl_diff:
        raise ValueError("BoringSSL GN integration diff is empty")
    add_bytes(entries, "metadata/patches/boringssl-build-gn.diff", boringssl_diff)
    add_bytes(entries, "metadata/patches/compiler-build-gn.diff", compiler_diff)
    patch_hashes["boringssl-build-gn.diff"] = bytes_hash(boringssl_diff)
    patch_hashes["compiler-build-gn.diff"] = bytes_hash(compiler_diff)

    helper_files = (
        args.prefix_kit / "boringssl_prefix_symbols.py",
        args.prefix_kit / "read_symbols_pe_compat.go",
        Path(__file__).resolve(),
    )
    helper_hashes = {}
    for helper in helper_files:
        add_file(entries, f"metadata/tools/{helper.name}", helper)
        helper_hashes[helper.name] = file_hash(helper)

    replay_commands = {
        "release_build": subprocess.list2cmdline([
            str(args.depot_tools / "autoninja.bat"), "-C", str(release_out),
            "webrtc",
        ]),
        "debug_build": subprocess.list2cmdline([
            str(args.depot_tools / "autoninja.bat"), "-C", str(debug_out),
            "webrtc",
        ]),
        "release_license": subprocess.list2cmdline([
            str(args.python), "tools_webrtc/libs/generate_licenses.py",
            "--target", "//:webrtc", str(release_out), str(release_out),
        ]),
        "debug_license": subprocess.list2cmdline([
            str(args.python), "tools_webrtc/libs/generate_licenses.py",
            "--target", "//:webrtc", str(debug_out), str(debug_out),
        ]),
        "debug_materialize": subprocess.list2cmdline([
            str(args.python),
            str(args.prefix_kit / "boringssl_prefix_symbols.py"),
            "materialize", "--allow-webrtc", "--out",
            str(debug_work / "webrtc-debug-prefixed-inspection.lib"),
            str(debug_out / "obj/webrtc.lib"),
        ]),
        "debug_read_symbols": subprocess.list2cmdline([
            str(args.go), "run", "-mod=readonly",
            str(args.prefix_kit / "read_symbols_pe_compat.go"),
            "-obj-file-format", "pe", "-out",
            str(debug_work / "new-webrtc-debug-symbols.txt"),
            str(debug_work / "webrtc-debug-prefixed-inspection.lib"),
        ]),
        "debug_verify": subprocess.list2cmdline([
            str(args.python),
            str(args.prefix_kit / "boringssl_prefix_symbols.py"),
            "verify", "--symbols", str(debug_work / "symbols-debug.txt"),
            "--all-symbols", str(debug_work / "symbols-union.txt"),
            "--new-symbols",
            str(debug_work / "new-webrtc-debug-symbols.txt"),
            "--library", str(debug_out / "obj/webrtc.lib"),
            "--prefix", PREFIX_NAME,
        ]) + " > " + subprocess.list2cmdline([
            str(debug_work / "verification-debug.json")
        ]),
        "release_union_verify": subprocess.list2cmdline([
            str(args.python),
            str(args.prefix_kit / "boringssl_prefix_symbols.py"),
            "verify", "--symbols", str(release_work / "symbols.txt"),
            "--all-symbols", str(debug_work / "symbols-union.txt"),
            "--new-symbols", str(release_work / "new-webrtc-symbols.txt"),
            "--library", str(release_out / "obj/webrtc.lib"),
            "--prefix", PREFIX_NAME,
        ]) + " > " + subprocess.list2cmdline([
            str(release_work / "verification-union.json")
        ]),
        "package": subprocess.list2cmdline([
            str(args.python), str(Path(__file__).resolve()), *sys.argv[1:]
        ]),
    }
    replay_bytes = (
        json.dumps(replay_commands, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    add_bytes(entries, "metadata/replay-commands.json", replay_bytes)

    required_files = list(COMMON_REQUIRED_FILES)
    for config in ("Release", "Debug"):
        required_files.extend(
            template.format(config=config, config_lower=config.lower())
            for template in CONFIGURATION_REQUIRED_FILES
        )
    required_hashes = {relative: source_hash(entries[relative])
                       for relative in required_files}
    manifest = {
        "schema_version": 1,
        "package_id": PACKAGE_ID,
        "archive_prefix": ARCHIVE_PREFIX,
        "platform": "windows-x64",
        "boringssl_prefix": PREFIX_NAME,
        "source": {
            "webrtc": git_revision(source_root),
            "build": git_revision(source_root / "build"),
            "boringssl": git_revision(source_root / "third_party/boringssl/src"),
            "livekit_patch_source": "client-sdk-rust@71e3a03e53205b65c6ca04f21339a58fc1a40eae",
            "livekit_tag": "webrtc-51ef663",
            "upstream_backports": [
                "765f70d55483e2a5bafd11cbf9d8deb5f1a99b19",
            ],
        },
        "toolchain": {
            "clang": "llvmorg-22-init-14273-gea10026b-2",
            "visual_studio": "VS2022 Professional",
            "msvc": "14.44.35207",
            "windows_sdk": "10.0.26100.0",
        },
        "base_sdk": {
            "archive": base_archive.name,
            "sha256": file_hash(base_archive),
        },
        "configurations": configuration_data,
        "patches": patch_hashes,
        "tools": helper_hashes,
        "replay_commands_sha256": bytes_hash(replay_bytes),
        "required_files": required_files,
        "required_file_hashes": required_hashes,
        "required_files_sha256": required_fingerprint(required_files, required_hashes),
    }
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    add_bytes(entries, "metadata/package.json", manifest_bytes)

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    temporary.unlink(missing_ok=True)
    try:
        with zipfile.ZipFile(temporary, "w", allowZip64=True,
                             compression=zipfile.ZIP_DEFLATED,
                             compresslevel=ZIP_COMPRESSION_LEVEL) as archive:
            for relative in sorted(entries):
                write_source(archive, f"{ARCHIVE_PREFIX}/{relative}", entries[relative])
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)

    archive_hash = file_hash(output)
    metadata = {
        "schema_version": 1,
        "package_id": PACKAGE_ID,
        "archive": output.name,
        "archive_prefix": ARCHIVE_PREFIX,
        "size": output.stat().st_size,
        "sha256": archive_hash,
        "file_count": len(entries),
        "manifest_sha256": bytes_hash(manifest_bytes),
        "required_files": required_files,
        "required_files_sha256": manifest["required_files_sha256"],
        "boringssl_prefix": PREFIX_NAME,
        "source": manifest["source"],
        "configurations": configuration_data,
    }
    metadata_bytes = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("utf-8")
    args.metadata_output.parent.mkdir(parents=True, exist_ok=True)
    args.metadata_output.write_bytes(metadata_bytes)
    print(json.dumps({
        "archive": str(output),
        "size": output.stat().st_size,
        "sha256": archive_hash,
        "file_count": len(entries),
        "metadata": str(args.metadata_output),
    }, indent=2))


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build the Cohavora dual-config prefixed WebRTC package")
    parser.add_argument("--sdk-root", type=Path,
                        help="package an existing codec SDK as v4; no compilation or legacy evidence reuse")
    parser.add_argument("--package-id", default="webrtc-b417823-cohavora-bssl-dual-v4")
    parser.add_argument("--release-tag", default="webrtc-m144-cohavora-bssl-dual-v4")
    parser.add_argument("--source-url", default="https://github.com/a980374208/webrtc_hardEncoder")
    parser.add_argument("--source-branch", default="m_144_hard_encoder")
    parser.add_argument("--source-equivalence", type=Path,
                        help="reviewed public-only source/build equivalence receipt")
    parser.add_argument("--base-archive", type=Path)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--release-out", type=Path)
    parser.add_argument("--debug-out", type=Path)
    parser.add_argument("--release-work", type=Path)
    parser.add_argument("--debug-work", type=Path)
    parser.add_argument("--patches-dir", type=Path)
    parser.add_argument("--upstream-patches-dir", type=Path)
    parser.add_argument("--prefix-kit", type=Path)
    parser.add_argument("--depot-tools", type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument(
        "--go", type=Path,
        help="Go command for replay text only (default: COHAVORA_GO, PATH, or go)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata-output", type=Path, required=True)
    args = parser.parse_args()
    if not args.sdk_root:
        missing = ["--" + name.replace("_", "-") for name in (
            "base_archive", "release_out", "debug_out", "release_work", "debug_work",
            "patches_dir", "upstream_patches_dir", "prefix_kit", "depot_tools")
            if getattr(args, name) is None]
        if missing:
            parser.error("legacy v3 mode requires " + ", ".join(missing))
    # Go is recorded in replay text, never executed by this packager. A machine
    # that only assembles verified artifacts therefore does not need Go.
    selected_go = args.go or os.environ.get("COHAVORA_GO") or shutil.which("go") or "go"
    args.go = Path(selected_go).expanduser()
    return args


if __name__ == "__main__":
    try:
        build_package(parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile) as exception:
        print(f"[ERROR] {exception}", file=sys.stderr)
        sys.exit(1)
