"""Build the pinned dual-configuration WebRTC SDK archive.

The archive reuses the verified LiveKit SDK header payload, replaces both
libraries and generated headers with the prefixed builds, and embeds the
inputs and verification evidence needed to reproduce the package.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


ARCHIVE_PREFIX = "cohavora-webrtc-win-x64"
PACKAGE_ID = "webrtc-51ef663-cohavora-bssl-dual-v2"
PREFIX_NAME = "cohavora_bssl"
FIXED_ZIP_TIME = (2026, 9, 25, 0, 0, 0)
EXPECTED_LIBRARY_HASHES = {
    "Release": "5f0cf912e593c9a279ee61a7e41400e38d633d0a8921216bed76cfd00d8b9022",
    "Debug": "e85d3418a4a169631118d30f21e0935e3a8160b449c6dd855490ca36536a61e2",
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
    return file_hash(value) if kind == "file" else bytes_hash(value)


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
    info.external_attr = 0o100644 << 16
    info.create_system = 3
    return info


def write_source(archive: zipfile.ZipFile, name: str, source):
    kind, value = source
    with archive.open(zip_info(name), "w", force_zip64=True) as destination:
        if kind == "file":
            with value.open("rb") as input_file:
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


def base_entries(base_archive: Path):
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
    with zipfile.ZipFile(base_archive, "r") as archive:
        prefix = discover_base_prefix(archive)
        for member in archive.infolist():
            if member.is_dir():
                continue
            name = member.filename.replace("\\", "/")
            if not name.startswith(prefix):
                continue
            relative = name[len(prefix):]
            if relative in excluded_files or relative.startswith(excluded_roots):
                continue
            yield relative, archive.read(member)


def build_package(args):
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
    for relative, content in base_entries(base_archive):
        add_bytes(entries, relative, content)

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
                             compresslevel=9) as archive:
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
    parser.add_argument("--base-archive", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--release-out", type=Path, required=True)
    parser.add_argument("--debug-out", type=Path, required=True)
    parser.add_argument("--release-work", type=Path, required=True)
    parser.add_argument("--debug-work", type=Path, required=True)
    parser.add_argument("--patches-dir", type=Path, required=True)
    parser.add_argument("--prefix-kit", type=Path, required=True)
    parser.add_argument("--depot-tools", type=Path, required=True)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument(
        "--go", type=Path,
        default=Path(r"C:\Program Files\Go\bin\go.exe"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata-output", type=Path, required=True)
    return parser.parse_args()


if __name__ == "__main__":
    try:
        build_package(parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile) as exception:
        print(f"[ERROR] {exception}", file=sys.stderr)
        sys.exit(1)
