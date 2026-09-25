'''
LiveKit C++ Client Dependencies Preparation Script
Location: build/prepare/prepare.py
- Self-contained Windows dependency preparation.
- Downloads and deployments are verified before replacing the active tree.
- Telegram Desktop UI is integrated in src/ui/tdesktop as Git submodules.
'''

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from contextlib import contextmanager
from pathlib import Path


script_dir = Path(__file__).parent.resolve()
root_dir = script_dir.parent.parent.resolve()
deps_dir = root_dir / "deps"
cache_keys_dir = root_dir / ".cache_keys"
cache_keys_dir.mkdir(parents=True, exist_ok=True)

WEBRTC_RELEASE_URL = (
    "https://github.com/livekit/rust-sdks/releases/download/"
    "webrtc-51ef663/webrtc-win-x64-release.zip"
)
WEBRTC_RELEASE_SHA256 = (
    "0a56a5c91b3b7b4222082b8a09388d16a802cde22d67295beb24c55700755b30"
)
WEBRTC_RELEASE_ARCHIVE = (
    f"webrtc-win-x64-release-{WEBRTC_RELEASE_SHA256[:12]}.zip"
)
WEBRTC_RELEASE_CACHE_ID = (
    f"v3:{WEBRTC_RELEASE_URL}:{WEBRTC_RELEASE_SHA256}"
)
WEBRTC_REQUIRED_FILES = (
    "include/api/peer_connection_interface.h",
    "include/api/video/i420_buffer.h",
    "include/third_party/libyuv/include/libyuv.h",
    "include/third_party/boringssl/src/include/openssl/ssl.h",
    "lib/Release/webrtc.lib",
    "lib/Debug/webrtc.lib",
)


def load_required_manifest(path: Path):
    return tuple(
        line
        for raw_line in path.read_text(encoding="utf-8").splitlines()
        if (line := raw_line.strip()) and not line.startswith("#")
    )


LIBRARIES_REQUIRED_FILES = load_required_manifest(
    script_dir / "libraries-required.txt"
)
LIBRARIES_ARCHIVE_METADATA_PATH = script_dir / "libraries-archive.json"


def error(message):
    print(f"[ERROR] {message}")
    sys.exit(1)


def load_libraries_archive_metadata(path: Path):
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exception:
        raise ValueError(
            f"Unable to read Libraries archive metadata {path}: {exception}"
        ) from exception
    required_keys = {
        "schema_version",
        "archive",
        "url",
        "size",
        "sha256",
        "required_manifest_sha256",
        "required_files_sha256",
        "file_count",
        "archive_prefix",
        "source_build_guide",
        "source_build_guide_url",
        "source_build_guide_sha256",
    }
    missing = sorted(required_keys - metadata.keys())
    if missing:
        raise ValueError(
            f"Libraries archive metadata is missing: {', '.join(missing)}")
    if metadata["schema_version"] != 1:
        raise ValueError("Unsupported Libraries archive metadata schema")
    if (not isinstance(metadata["archive"], str)
            or Path(metadata["archive"]).name != metadata["archive"]
            or not metadata["archive"].endswith(".zip")):
        raise ValueError("Libraries archive metadata has an invalid file name")
    if (not isinstance(metadata["sha256"], str)
            or not re.fullmatch(r"[0-9a-f]{64}", metadata["sha256"])):
        raise ValueError("Libraries archive metadata has an invalid SHA-256")
    for field in (
            "required_manifest_sha256",
            "required_files_sha256",
            "source_build_guide_sha256"):
        if (not isinstance(metadata[field], str)
                or not re.fullmatch(r"[0-9a-f]{64}", metadata[field])):
            raise ValueError(
                f"Libraries archive metadata has an invalid {field}")
    if (not isinstance(metadata["url"], str)
            or not re.fullmatch(r"https://github\.com/[^\s]+", metadata["url"])
            or not metadata["url"].endswith("/" + metadata["archive"])):
        raise ValueError("Libraries archive metadata has an invalid URL")
    if (not isinstance(metadata["source_build_guide"], str)
            or Path(metadata["source_build_guide"]).name
            != metadata["source_build_guide"]
            or not metadata["source_build_guide"].endswith(".md")):
        raise ValueError(
            "Libraries archive metadata has an invalid source build guide")
    expected_guide_url = (
        metadata["url"].rsplit("/", 1)[0]
        + "/"
        + metadata["source_build_guide"]
    )
    if metadata["source_build_guide_url"] != expected_guide_url:
        raise ValueError(
            "Libraries archive metadata has an invalid source build guide URL")
    if not isinstance(metadata["size"], int) or metadata["size"] <= 0:
        raise ValueError("Libraries archive metadata has an invalid size")
    if not isinstance(metadata["file_count"], int) or metadata["file_count"] <= 0:
        raise ValueError("Libraries archive metadata has an invalid file count")
    if metadata["archive_prefix"] != "Libraries/win64":
        raise ValueError("Libraries archive metadata has an invalid prefix")
    manifest_hash = hashlib.sha256(path.parent.joinpath(
        "libraries-required.txt").read_bytes()).hexdigest()
    if metadata["required_manifest_sha256"] != manifest_hash:
        raise ValueError(
            "Libraries archive metadata does not match libraries-required.txt")
    source_build_guide = script_dir / "README.md"
    if (not source_build_guide.is_file()
            or hashlib.sha256(source_build_guide.read_bytes()).hexdigest()
            != metadata["source_build_guide_sha256"]):
        raise ValueError(
            "Libraries archive metadata does not match the source build guide")
    return metadata


LIBRARIES_ARCHIVE_METADATA = load_libraries_archive_metadata(
    LIBRARIES_ARCHIVE_METADATA_PATH)
LIBRARIES_ARCHIVE_NAME = LIBRARIES_ARCHIVE_METADATA["archive"]
LIBRARIES_ARCHIVE_URL = LIBRARIES_ARCHIVE_METADATA["url"]
LIBRARIES_ARCHIVE_SHA256 = LIBRARIES_ARCHIVE_METADATA["sha256"]
LIBRARIES_ARCHIVE_SIZE = LIBRARIES_ARCHIVE_METADATA["size"]
QT_RELEASE_CACHE_ID = (
    f"v4:qt-5.15.18:{LIBRARIES_ARCHIVE_NAME}:"
    f"{LIBRARIES_ARCHIVE_SHA256}"
)
_validated_libraries_archives = set()


def compute_string_hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def compute_file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def missing_required_files(root: Path, required_files):
    return [relative for relative in required_files if not (root / relative).is_file()]


def format_missing(root: Path, missing) -> str:
    details = "\n".join(f"  - {root / relative}" for relative in missing)
    return f"Required dependency artifacts are missing:\n{details}"


def required_files_fingerprint(root: Path, required_files) -> str:
    digest = hashlib.sha256()
    for relative in required_files:
        path = root / relative
        if not path.is_file():
            error(format_missing(root, [relative]))
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(compute_file_hash(path).encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest()


def check_cache_key(stage_name: str, expected_key: str) -> str:
    key_file = cache_keys_dir / stage_name
    if not key_file.is_file():
        return "NotFound"
    try:
        content = key_file.read_text(encoding="utf-8").strip()
        return "Good" if content == expected_key else "Stale"
    except OSError:
        return "Stale"


def write_cache_key(stage_name: str, key: str):
    key_file = cache_keys_dir / stage_name
    temp_key = cache_keys_dir / f".{stage_name}.{os.getpid()}.tmp"
    temp_key.write_text(key, encoding="utf-8")
    os.replace(temp_key, key_file)


@contextmanager
def stage_lock(stage_name: str, timeout_seconds: int = 60):
    lock_dir = cache_keys_dir / f"{stage_name}.lock"
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            lock_dir.mkdir()
            (lock_dir / "owner").write_text(str(os.getpid()), encoding="ascii")
            break
        except FileExistsError:
            try:
                age = time.time() - lock_dir.stat().st_mtime
            except FileNotFoundError:
                continue
            if age > 30 * 60:
                shutil.rmtree(lock_dir, ignore_errors=True)
                continue
            if time.monotonic() >= deadline:
                error(f"Timed out waiting for dependency stage lock: {lock_dir}")
            time.sleep(0.25)

    try:
        yield
    finally:
        shutil.rmtree(lock_dir, ignore_errors=True)


def validate_zip(path: Path, expected_sha256: str = None):
    if not path.is_file():
        raise ValueError(f"Dependency archive does not exist: {path}")
    if expected_sha256:
        actual = compute_file_hash(path)
        if actual.lower() != expected_sha256.lower():
            raise ValueError(
                f"SHA-256 mismatch for {path}: expected {expected_sha256}, "
                f"got {actual}"
            )
    if not zipfile.is_zipfile(path):
        raise ValueError(f"Dependency archive is not a valid ZIP file: {path}")
    with zipfile.ZipFile(path, "r") as archive:
        damaged = archive.testzip()
        if damaged:
            raise ValueError(f"Dependency archive contains a damaged entry: {damaged}")


def normalized_zip_files(path: Path):
    with zipfile.ZipFile(path, "r") as archive:
        return {
            member.filename.replace("\\", "/").strip("/").lower()
            for member in archive.infolist()
            if not member.is_dir()
        }


def validate_libraries_archive(path: Path):
    stat = path.stat()
    identity = (str(path.resolve()), stat.st_size, stat.st_mtime_ns)
    if identity in _validated_libraries_archives:
        return
    if stat.st_size != LIBRARIES_ARCHIVE_SIZE:
        raise ValueError(
            f"Qt/Libraries archive size mismatch for {path}: expected "
            f"{LIBRARIES_ARCHIVE_SIZE}, got {stat.st_size}. The pinned archive "
            f"is {LIBRARIES_ARCHIVE_NAME}.")
    validate_zip(path, LIBRARIES_ARCHIVE_SHA256)
    archive_files = normalized_zip_files(path)
    if len(archive_files) != LIBRARIES_ARCHIVE_METADATA["file_count"]:
        raise ValueError(
            f"Qt/Libraries archive file count mismatch for {path}: expected "
            f"{LIBRARIES_ARCHIVE_METADATA['file_count']}, got "
            f"{len(archive_files)}")
    anchor = LIBRARIES_REQUIRED_FILES[0].replace("\\", "/").lower()
    prefixes = {
        member[: -len(anchor)]
        for member in archive_files
        if member.endswith(anchor)
    }
    required = tuple(
        relative.replace("\\", "/").lower()
        for relative in LIBRARIES_REQUIRED_FILES
    )
    for prefix in prefixes:
        if all(f"{prefix}{relative}" in archive_files for relative in required):
            _validated_libraries_archives.add(identity)
            return
    raise ValueError(
        f"Qt/Libraries archive is missing required project artifacts: {path}"
    )


def download_libraries_archive() -> Path:
    archive_path = cache_keys_dir / LIBRARIES_ARCHIVE_NAME
    if archive_path.is_file():
        try:
            validate_libraries_archive(archive_path)
            return archive_path
        except (OSError, ValueError, zipfile.BadZipFile):
            archive_path.unlink(missing_ok=True)
            print("[CACHE] Removed an invalid Qt/Libraries archive.")

    last_error = None
    for attempt in range(1, 4):
        part_path = cache_keys_dir / (
            f".{LIBRARIES_ARCHIVE_NAME}.{os.getpid()}.{attempt}.part"
        )
        part_path.unlink(missing_ok=True)
        print(
            f"[DOWNLOAD] Downloading {LIBRARIES_ARCHIVE_URL} -> "
            f"{archive_path} (attempt {attempt}/3)..."
        )
        try:
            request = urllib.request.Request(
                LIBRARIES_ARCHIVE_URL,
                headers={"User-Agent": "Cohavora-dependency-preparer/1"},
            )
            with urllib.request.urlopen(request, timeout=60) as response:
                with part_path.open("wb") as destination:
                    shutil.copyfileobj(response, destination, 1024 * 1024)
            validate_libraries_archive(part_path)
            os.replace(part_path, archive_path)
            return archive_path
        except (OSError, ValueError, urllib.error.URLError, zipfile.BadZipFile) as exc:
            last_error = exc
            part_path.unlink(missing_ok=True)
            if attempt < 3:
                time.sleep(attempt)

    error(
        f"Failed to download and verify {LIBRARIES_ARCHIVE_URL}: {last_error}")


def validate_debug_webrtc_source(path: Path):
    if path.is_dir():
        find_webrtc_library(path)
        return
    if path.suffix.lower() == ".lib":
        return
    if path.suffix.lower() != ".zip":
        raise ValueError(
            "Debug WebRTC source must be a directory, .lib, or ZIP archive: "
            f"{path}"
        )
    validate_zip(path)
    if not any(
        member.rsplit("/", 1)[-1] == "webrtc.lib"
        for member in normalized_zip_files(path)
    ):
        raise ValueError(f"Debug WebRTC archive contains no webrtc.lib: {path}")


def download_release_archive() -> Path:
    archive_path = cache_keys_dir / WEBRTC_RELEASE_ARCHIVE
    if archive_path.exists():
        try:
            validate_zip(archive_path, WEBRTC_RELEASE_SHA256)
            return archive_path
        except (OSError, ValueError, zipfile.BadZipFile):
            archive_path.unlink(missing_ok=True)
            print("[CACHE] Removed an invalid cached WebRTC archive.")

    legacy_archive = cache_keys_dir / "webrtc-windows-x64.zip"
    if legacy_archive.is_file():
        try:
            validate_zip(legacy_archive, WEBRTC_RELEASE_SHA256)
            shutil.copy2(legacy_archive, archive_path)
            return archive_path
        except (OSError, ValueError, zipfile.BadZipFile):
            print("[CACHE] Ignoring the legacy WebRTC archive because validation failed.")

    last_error = None
    for attempt in range(1, 4):
        part_path = cache_keys_dir / (
            f".{WEBRTC_RELEASE_ARCHIVE}.{os.getpid()}.{attempt}.part"
        )
        part_path.unlink(missing_ok=True)
        print(
            f"[DOWNLOAD] Downloading {WEBRTC_RELEASE_URL} -> {archive_path} "
            f"(attempt {attempt}/3)..."
        )
        try:
            request = urllib.request.Request(
                WEBRTC_RELEASE_URL,
                headers={"User-Agent": "Cohavora-dependency-preparer/1"},
            )
            with urllib.request.urlopen(request, timeout=30) as response:
                with part_path.open("wb") as destination:
                    shutil.copyfileobj(response, destination, 1024 * 1024)
            validate_zip(part_path, WEBRTC_RELEASE_SHA256)
            os.replace(part_path, archive_path)
            return archive_path
        except (OSError, ValueError, urllib.error.URLError, zipfile.BadZipFile) as exc:
            last_error = exc
            part_path.unlink(missing_ok=True)
            if attempt < 3:
                time.sleep(attempt)

    error(f"Failed to download and verify {WEBRTC_RELEASE_URL}: {last_error}")


def safe_extract_zip(archive_path: Path, extract_to: Path):
    validate_zip(archive_path)
    extract_to.mkdir(parents=True, exist_ok=True)
    extract_root = extract_to.resolve()
    with zipfile.ZipFile(archive_path, "r") as archive:
        for member in archive.infolist():
            destination = (extract_to / member.filename).resolve()
            try:
                common = os.path.commonpath((str(extract_root), str(destination)))
            except ValueError:
                error(f"Archive entry escapes the extraction root: {member.filename}")
            if common != str(extract_root):
                error(f"Archive entry escapes the extraction root: {member.filename}")
        archive.extractall(extract_to)


def atomic_replace_directory(staged_dir: Path, target_dir: Path):
    target_dir.parent.mkdir(parents=True, exist_ok=True)
    backup_dir = target_dir.parent / f".{target_dir.name}.backup-{os.getpid()}"
    if backup_dir.exists():
        shutil.rmtree(backup_dir)

    had_target = target_dir.exists()
    if had_target:
        target_dir.rename(backup_dir)
    try:
        staged_dir.rename(target_dir)
    except Exception:
        if had_target and backup_dir.exists() and not target_dir.exists():
            backup_dir.rename(target_dir)
        raise
    else:
        if backup_dir.exists():
            shutil.rmtree(backup_dir)


def find_webrtc_library(source_dir: Path) -> Path:
    candidates = sorted(source_dir.glob("**/webrtc.lib"))
    if not candidates:
        error(f"No webrtc.lib found in {source_dir}")
    return candidates[0]


def find_webrtc_payload(extract_root: Path) -> Path:
    candidates = (extract_root, extract_root / "win-x64-release")
    for candidate in candidates:
        if (candidate / "include/api/peer_connection_interface.h").is_file():
            return candidate
    for header in extract_root.glob("**/include/api/peer_connection_interface.h"):
        return header.parents[2]
    error(f"WebRTC archive has no recognizable SDK root: {extract_root}")


def debug_source_fingerprint(source_path: Path) -> str:
    if source_path.is_dir():
        return compute_file_hash(find_webrtc_library(source_path))
    if source_path.is_file():
        return compute_file_hash(source_path)
    error(f"Debug WebRTC source does not exist: {source_path}")


def install_debug_webrtc(source_path: Path, target_lib: Path):
    target_lib.parent.mkdir(parents=True, exist_ok=True)
    if source_path.is_dir():
        shutil.copy2(find_webrtc_library(source_path), target_lib)
        return
    if source_path.suffix.lower() == ".lib":
        shutil.copy2(source_path, target_lib)
        return
    validate_zip(source_path)
    with tempfile.TemporaryDirectory(
        prefix="webrtc-debug-", dir=cache_keys_dir
    ) as temp_dir:
        safe_extract_zip(source_path, Path(temp_dir))
        shutil.copy2(find_webrtc_library(Path(temp_dir)), target_lib)


def normalize_webrtc_layout(candidate: Path):
    release_dir = candidate / "lib/Release"
    debug_dir = candidate / "lib/Debug"
    release_dir.mkdir(parents=True, exist_ok=True)
    debug_dir.mkdir(parents=True, exist_ok=True)
    root_lib = candidate / "lib/webrtc.lib"
    release_lib = release_dir / "webrtc.lib"
    if root_lib.is_file():
        shutil.copy2(root_lib, release_lib)
    if not release_lib.is_file():
        error(f"WebRTC release library is missing from staged SDK: {candidate}")


def prepare_webrtc(debug_archive_path: Path = None):
    stage_name = "webrtc"
    target_dir = deps_dir / "webrtc"
    release_key = compute_string_hash(WEBRTC_RELEASE_CACHE_ID)
    debug_key = None
    if debug_archive_path:
        debug_key = compute_string_hash(
            f"v3:{debug_source_fingerprint(debug_archive_path)}"
        )

    missing = missing_required_files(target_dir, WEBRTC_REQUIRED_FILES)
    release_needs_install = (
        check_cache_key(stage_name, release_key) != "Good" or bool(missing)
    )
    debug_needs_install = bool(
        debug_archive_path
        and (
            check_cache_key("webrtc-debug", debug_key) != "Good"
            or not (target_dir / "lib/Debug/webrtc.lib").is_file()
        )
    )
    if not release_needs_install and not debug_needs_install:
        print("[STAGE: WebRTC] OK (verified Release and Debug SDK artifacts)")
        return target_dir

    with stage_lock(stage_name):
        missing = missing_required_files(target_dir, WEBRTC_REQUIRED_FILES)
        release_needs_install = (
            check_cache_key(stage_name, release_key) != "Good" or bool(missing)
        )
        debug_needs_install = bool(
            debug_archive_path
            and (
                check_cache_key("webrtc-debug", debug_key) != "Good"
                or not (target_dir / "lib/Debug/webrtc.lib").is_file()
            )
        )
        if not release_needs_install and not debug_needs_install:
            return target_dir

        target_dir.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix=".webrtc-stage-", dir=target_dir.parent
        ) as temp_dir:
            temp_root = Path(temp_dir)
            candidate = temp_root / "candidate"
            if release_needs_install:
                archive_path = download_release_archive()
                extracted = temp_root / "extracted"
                print(f"[EXTRACT] Extracting {archive_path.name} into staging...")
                safe_extract_zip(archive_path, extracted)
                shutil.copytree(find_webrtc_payload(extracted), candidate)
            else:
                shutil.copytree(target_dir, candidate)

            normalize_webrtc_layout(candidate)
            debug_target = candidate / "lib/Debug/webrtc.lib"
            if debug_archive_path:
                print(
                    f"[INSTALL] Staging Debug WebRTC library from "
                    f"{debug_archive_path}..."
                )
                install_debug_webrtc(debug_archive_path, debug_target)
            elif (target_dir / "lib/Debug/webrtc.lib").is_file():
                # A release refresh must not erase a user-provided Debug build.
                shutil.copy2(target_dir / "lib/Debug/webrtc.lib", debug_target)
            elif not debug_target.is_file():
                shutil.copy2(candidate / "lib/Release/webrtc.lib", debug_target)
                print(" -> Initialized Debug WebRTC with the Release ABI binary")

            missing = missing_required_files(candidate, WEBRTC_REQUIRED_FILES)
            if missing:
                error(format_missing(candidate, missing))

            atomic_replace_directory(candidate, target_dir)

        write_cache_key(stage_name, release_key)
        if debug_key:
            write_cache_key("webrtc-debug", debug_key)

    print(f" -> WebRTC C++ deployed transactionally and verified in {target_dir}")
    return target_dir


def ensure_zlib_alias(libraries_root: Path):
    zlib_dir = libraries_root / "zlib"
    for config in ("Release", "Debug"):
        config_dir = zlib_dir / config
        if not config_dir.is_dir():
            continue
        zlib_static = config_dir / (
            "zlibstaticd.lib" if config == "Debug" else "zlibstatic.lib"
        )
        libzs = config_dir / ("libzsd.lib" if config == "Debug" else "libzs.lib")
        if zlib_static.is_file() and not libzs.exists():
            shutil.copy2(zlib_static, libzs)


def find_libraries_root(source_root: Path) -> Path:
    candidates = (
        source_root,
        source_root / "win64",
        source_root / "Libraries/win64",
    )
    for candidate in candidates:
        if (candidate / "Qt-5.15.18").is_dir():
            return candidate
    matches = sorted(source_root.glob("**/Qt-5.15.18"), key=lambda path: len(path.parts))
    if matches:
        return matches[0].parent
    error(f"No Qt-5.15.18 SDK root found under {source_root}")


def preflight_inputs(
        qt_archive_path: Path = None,
        libraries_source: Path = None,
        debug_archive_path: Path = None):
    target_libraries = deps_dir / "Libraries/win64"
    if qt_archive_path:
        validate_libraries_archive(qt_archive_path)
    elif libraries_source:
        source_root = find_libraries_root(libraries_source)
        missing = missing_required_files(source_root, LIBRARIES_REQUIRED_FILES)
        if missing:
            error(format_missing(source_root, missing))
    else:
        # A missing local tree is repaired from the pinned release archive by
        # prepare_libraries(). Preflight remains side-effect free.
        pass

    if debug_archive_path:
        validate_debug_webrtc_source(debug_archive_path)


def prepare_libraries(qt_archive_path: Path = None, libraries_source: Path = None):
    stage_name = "libraries"
    target_dir = deps_dir / "Libraries/win64"

    source_root = None
    if qt_archive_path:
        validate_libraries_archive(qt_archive_path)
        input_identity = f"archive:{compute_file_hash(qt_archive_path)}"
    elif libraries_source:
        source_root = find_libraries_root(libraries_source)
        source_missing = missing_required_files(source_root, LIBRARIES_REQUIRED_FILES)
        if source_missing:
            error(format_missing(source_root, source_missing))
        input_identity = (
            "directory:"
            + required_files_fingerprint(source_root, LIBRARIES_REQUIRED_FILES)
        )
    else:
        target_missing = missing_required_files(target_dir, LIBRARIES_REQUIRED_FILES)
        if target_missing:
            qt_archive_path = download_libraries_archive()
            input_identity = f"archive:{LIBRARIES_ARCHIVE_SHA256}"
        else:
            input_identity = (
                "preinstalled:"
                + required_files_fingerprint(target_dir, LIBRARIES_REQUIRED_FILES)
            )

    expected_key = compute_string_hash(f"{QT_RELEASE_CACHE_ID}:{input_identity}")
    missing = missing_required_files(target_dir, LIBRARIES_REQUIRED_FILES)
    if check_cache_key(stage_name, expected_key) == "Good" and not missing:
        print("[STAGE: Libraries] OK (verified Qt and third-party artifacts)")
        return target_dir

    if not qt_archive_path and not libraries_source:
        write_cache_key(stage_name, expected_key)
        print(f" -> Existing libraries verified in {target_dir}")
        return target_dir

    with stage_lock(stage_name):
        missing = missing_required_files(target_dir, LIBRARIES_REQUIRED_FILES)
        if check_cache_key(stage_name, expected_key) == "Good" and not missing:
            return target_dir

        target_dir.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix=".libraries-stage-", dir=target_dir.parent
        ) as temp_dir:
            temp_root = Path(temp_dir)
            candidate = temp_root / "candidate"
            if qt_archive_path:
                extracted = temp_root / "extracted"
                print(f"[EXTRACT] Extracting {qt_archive_path} into staging...")
                safe_extract_zip(qt_archive_path, extracted)
                source_root = find_libraries_root(extracted)
            else:
                print(f"[INSTALL] Staging Libraries from {source_root}...")

            ignore = shutil.ignore_patterns(".git*", "*.obj", "*.tlog", "*.pch")
            shutil.copytree(source_root, candidate, ignore=ignore)
            ensure_zlib_alias(candidate)
            missing = missing_required_files(candidate, LIBRARIES_REQUIRED_FILES)
            if missing:
                error(format_missing(candidate, missing))

            atomic_replace_directory(candidate, target_dir)

        write_cache_key(stage_name, expected_key)

    print(f" -> Libraries deployed transactionally and verified in {target_dir}")
    return target_dir


def main():
    import argparse

    parser = argparse.ArgumentParser(description="LiveKit C++ Dependencies Pipeline")
    libraries_input = parser.add_mutually_exclusive_group()
    libraries_input.add_argument(
        "--qt-archive",
        type=str,
        help=f"Path to pinned {LIBRARIES_ARCHIVE_NAME}",
    )
    libraries_input.add_argument(
        "--libraries-src",
        type=str,
        help="Path to a complete external Libraries directory",
    )
    parser.add_argument(
        "--webrtc-debug-archive",
        type=str,
        help="Path to a custom Debug webrtc ZIP, directory, or .lib",
    )
    args = parser.parse_args()

    print("==================================================")
    print("  LiveKit C++ Self-Contained Dependency Pipeline  ")
    print("  (Native In-Source UI | Pure C++ Toolchain)      ")
    print("==================================================")
    print(f"Project Root  : {root_dir}")
    print(f"Target Deps   : {deps_dir}\n")
    print(
        "Command       : "
        + subprocess.list2cmdline(
            [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]]
        )
        + "\n"
    )

    qt_archive = Path(args.qt_archive).resolve() if args.qt_archive else None
    libraries_source = (
        Path(args.libraries_src).resolve() if args.libraries_src else None
    )
    webrtc_debug = (
        Path(args.webrtc_debug_archive).resolve()
        if args.webrtc_debug_archive
        else None
    )
    if qt_archive and not qt_archive.is_file():
        error(f"Qt archive does not exist: {qt_archive}")
    if libraries_source and not libraries_source.is_dir():
        error(f"Libraries source directory does not exist: {libraries_source}")
    if webrtc_debug and not webrtc_debug.exists():
        error(f"Debug WebRTC source does not exist: {webrtc_debug}")

    preflight_inputs(qt_archive, libraries_source, webrtc_debug)
    deps_dir.mkdir(parents=True, exist_ok=True)

    prepare_webrtc(webrtc_debug)
    prepare_libraries(qt_archive, libraries_source)

    print("\n==================================================")
    print("[SUCCESS] All project dependencies are ready in ./deps!")
    print("==================================================")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile) as exception:
        error(str(exception))
