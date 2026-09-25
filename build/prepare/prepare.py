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
import urllib.parse
import urllib.request
import zipfile
from contextlib import contextmanager
from pathlib import Path


script_dir = Path(__file__).parent.resolve()
root_dir = script_dir.parent.parent.resolve()
deps_dir = root_dir / "deps"
cache_keys_dir = root_dir / ".cache_keys"
cache_keys_dir.mkdir(parents=True, exist_ok=True)

WEBRTC_PACKAGE_METADATA_PATH = script_dir / "webrtc-package.json"


def load_webrtc_package_metadata(path: Path):
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exception:
        raise ValueError(
            f"Unable to read WebRTC package metadata {path}: {exception}"
        ) from exception
    required_keys = {
        "schema_version",
        "package_id",
        "archive",
        "download_url",
        "archive_prefix",
        "size",
        "sha256",
        "file_count",
        "manifest_sha256",
        "required_files",
        "required_files_sha256",
        "boringssl_prefix",
        "source",
        "configurations",
    }
    missing = sorted(required_keys - metadata.keys())
    if missing:
        raise ValueError(
            f"WebRTC package metadata is missing: {', '.join(missing)}")
    if metadata["schema_version"] != 1:
        raise ValueError("Unsupported WebRTC package metadata schema")
    if (not isinstance(metadata["archive"], str)
            or Path(metadata["archive"]).name != metadata["archive"]
            or not metadata["archive"].endswith(".zip")):
        raise ValueError("WebRTC package metadata has an invalid archive name")
    if (not isinstance(metadata["archive_prefix"], str)
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*",
                                metadata["archive_prefix"])):
        raise ValueError("WebRTC package metadata has an invalid archive prefix")
    download_url = metadata["download_url"]
    if not isinstance(download_url, str):
        raise ValueError("WebRTC package metadata has an invalid download URL")
    parsed_url = urllib.parse.urlparse(download_url)
    if (parsed_url.scheme != "https"
            or parsed_url.netloc != "github.com"
            or not parsed_url.path.endswith("/" + metadata["archive"])):
        raise ValueError("WebRTC package metadata has an invalid download URL")
    for field in ("sha256", "manifest_sha256", "required_files_sha256"):
        if (not isinstance(metadata[field], str)
                or not re.fullmatch(r"[0-9a-f]{64}", metadata[field])):
            raise ValueError(
                f"WebRTC package metadata has an invalid {field}")
    if not isinstance(metadata["size"], int) or metadata["size"] <= 0:
        raise ValueError("WebRTC package metadata has an invalid size")
    if not isinstance(metadata["file_count"], int) or metadata["file_count"] <= 0:
        raise ValueError("WebRTC package metadata has an invalid file count")
    if metadata["boringssl_prefix"] != "cohavora_bssl":
        raise ValueError("WebRTC package metadata has an unexpected symbol prefix")
    if set(metadata["configurations"]) != {"Debug", "Release"}:
        raise ValueError("WebRTC package must contain Debug and Release configurations")
    required_files = metadata["required_files"]
    if (not isinstance(required_files, list) or not required_files
            or len(required_files) != len(set(required_files))):
        raise ValueError("WebRTC package metadata has invalid required files")
    for relative in required_files:
        candidate = Path(relative)
        if (not isinstance(relative, str) or candidate.is_absolute()
                or ".." in candidate.parts or "\\" in relative):
            raise ValueError(
                f"WebRTC package metadata has an unsafe path: {relative}")
    return metadata


WEBRTC_PACKAGE_METADATA = load_webrtc_package_metadata(
    WEBRTC_PACKAGE_METADATA_PATH)
WEBRTC_PACKAGE_ARCHIVE = WEBRTC_PACKAGE_METADATA["archive"]
WEBRTC_PACKAGE_URL = WEBRTC_PACKAGE_METADATA["download_url"]
WEBRTC_PACKAGE_SHA256 = WEBRTC_PACKAGE_METADATA["sha256"]
WEBRTC_PACKAGE_SIZE = WEBRTC_PACKAGE_METADATA["size"]
WEBRTC_PACKAGE_CACHE_ID = (
    f"v4:{WEBRTC_PACKAGE_METADATA['package_id']}:{WEBRTC_PACKAGE_SHA256}"
)
WEBRTC_REQUIRED_FILES = tuple(WEBRTC_PACKAGE_METADATA["required_files"])
_validated_webrtc_archives = set()


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


def validate_webrtc_archive(path: Path):
    stat = path.stat()
    identity = (str(path.resolve()), stat.st_size, stat.st_mtime_ns)
    if identity in _validated_webrtc_archives:
        return
    if stat.st_size != WEBRTC_PACKAGE_SIZE:
        raise ValueError(
            f"WebRTC package size mismatch for {path}: expected "
            f"{WEBRTC_PACKAGE_SIZE}, got {stat.st_size}")
    validate_zip(path, WEBRTC_PACKAGE_SHA256)
    archive_files = normalized_zip_files(path)
    if len(archive_files) != WEBRTC_PACKAGE_METADATA["file_count"]:
        raise ValueError(
            f"WebRTC package file count mismatch for {path}: expected "
            f"{WEBRTC_PACKAGE_METADATA['file_count']}, got {len(archive_files)}")
    prefix = WEBRTC_PACKAGE_METADATA["archive_prefix"].lower() + "/"
    required = {
        prefix + relative.lower() for relative in WEBRTC_REQUIRED_FILES
    }
    manifest_name = prefix + "metadata/package.json"
    missing = sorted(required - archive_files)
    if manifest_name not in archive_files:
        missing.append(manifest_name)
    if missing:
        raise ValueError(
            "WebRTC package is missing required artifacts:\n  - "
            + "\n  - ".join(missing))
    with zipfile.ZipFile(path, "r") as archive:
        manifest_bytes = archive.read(
            WEBRTC_PACKAGE_METADATA["archive_prefix"] + "/metadata/package.json")
    manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
    if manifest_hash != WEBRTC_PACKAGE_METADATA["manifest_sha256"]:
        raise ValueError("WebRTC package manifest hash does not match metadata")
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exception:
        raise ValueError("WebRTC package manifest is invalid") from exception
    for field in ("package_id", "archive_prefix", "boringssl_prefix",
                  "source", "configurations", "required_files",
                  "required_files_sha256"):
        if manifest.get(field) != WEBRTC_PACKAGE_METADATA[field]:
            raise ValueError(
                f"WebRTC package manifest disagrees with metadata: {field}")
    _validated_webrtc_archives.add(identity)


def cache_webrtc_archive(source_path: Path) -> Path:
    validate_webrtc_archive(source_path)
    archive_path = cache_keys_dir / WEBRTC_PACKAGE_ARCHIVE
    if source_path.resolve() == archive_path.resolve():
        return archive_path
    cache_keys_dir.mkdir(parents=True, exist_ok=True)
    part_path = cache_keys_dir / f".{WEBRTC_PACKAGE_ARCHIVE}.{os.getpid()}.part"
    part_path.unlink(missing_ok=True)
    try:
        shutil.copy2(source_path, part_path)
        validate_webrtc_archive(part_path)
        os.replace(part_path, archive_path)
    finally:
        part_path.unlink(missing_ok=True)
    return archive_path


def download_webrtc_archive() -> Path:
    archive_path = cache_keys_dir / WEBRTC_PACKAGE_ARCHIVE
    if archive_path.is_file():
        try:
            validate_webrtc_archive(archive_path)
            return archive_path
        except (OSError, ValueError, zipfile.BadZipFile):
            archive_path.unlink(missing_ok=True)
            print("[CACHE] Removed an invalid WebRTC archive.")

    last_error = None
    for attempt in range(1, 4):
        part_path = cache_keys_dir / (
            f".{WEBRTC_PACKAGE_ARCHIVE}.{os.getpid()}.{attempt}.part"
        )
        part_path.unlink(missing_ok=True)
        print(
            f"[DOWNLOAD] Downloading {WEBRTC_PACKAGE_URL} -> "
            f"{archive_path} (attempt {attempt}/3)..."
        )
        try:
            request = urllib.request.Request(
                WEBRTC_PACKAGE_URL,
                headers={"User-Agent": "Cohavora-dependency-preparer/1"},
            )
            with urllib.request.urlopen(request, timeout=60) as response:
                with part_path.open("wb") as destination:
                    shutil.copyfileobj(response, destination, 1024 * 1024)
            validate_webrtc_archive(part_path)
            os.replace(part_path, archive_path)
            return archive_path
        except (OSError, ValueError, urllib.error.URLError,
                zipfile.BadZipFile) as exception:
            last_error = exception
            part_path.unlink(missing_ok=True)
            if attempt < 3:
                time.sleep(attempt)

    error(f"Failed to download and verify {WEBRTC_PACKAGE_URL}: {last_error}")


def resolve_webrtc_archive(package_path: Path = None) -> Path:
    if package_path:
        return cache_webrtc_archive(package_path)
    return download_webrtc_archive()


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


def find_webrtc_payload(extract_root: Path) -> Path:
    candidate = extract_root / WEBRTC_PACKAGE_METADATA["archive_prefix"]
    if (candidate / "metadata/package.json").is_file():
        return candidate
    raise ValueError(f"WebRTC package has no expected SDK root: {extract_root}")


def webrtc_install_is_valid(target_dir: Path) -> bool:
    if missing_required_files(target_dir, WEBRTC_REQUIRED_FILES):
        return False
    return (
        required_files_fingerprint(target_dir, WEBRTC_REQUIRED_FILES)
        == WEBRTC_PACKAGE_METADATA["required_files_sha256"]
    )


def prepare_webrtc(package_archive_path: Path = None):
    stage_name = "webrtc"
    target_dir = deps_dir / "webrtc"
    expected_key = compute_string_hash(WEBRTC_PACKAGE_CACHE_ID)
    if (check_cache_key(stage_name, expected_key) == "Good"
            and webrtc_install_is_valid(target_dir)):
        print("[STAGE: WebRTC] OK (verified dual-config prefixed SDK)")
        return target_dir

    archive_path = resolve_webrtc_archive(package_archive_path)
    with stage_lock(stage_name):
        if (check_cache_key(stage_name, expected_key) == "Good"
                and webrtc_install_is_valid(target_dir)):
            return target_dir

        target_dir.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix=".webrtc-stage-", dir=target_dir.parent
        ) as temp_dir:
            temp_root = Path(temp_dir)
            candidate = temp_root / "candidate"
            extracted = temp_root / "extracted"
            print(f"[EXTRACT] Extracting {archive_path.name} into staging...")
            safe_extract_zip(archive_path, extracted)
            shutil.copytree(find_webrtc_payload(extracted), candidate)
            if not webrtc_install_is_valid(candidate):
                raise ValueError(
                    "Staged WebRTC package content fingerprint does not match "
                    "the pinned dual-config package")

            atomic_replace_directory(candidate, target_dir)

        write_cache_key(stage_name, expected_key)

    print(
        f" -> Dual-config prefixed WebRTC deployed transactionally and verified "
        f"in {target_dir}")
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
        webrtc_archive_path: Path = None):
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

    if webrtc_archive_path:
        validate_webrtc_archive(webrtc_archive_path)
    elif not webrtc_install_is_valid(deps_dir / "webrtc"):
        resolve_webrtc_archive()


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
        "--webrtc-archive",
        type=str,
        help=f"Path to pinned dual-config {WEBRTC_PACKAGE_ARCHIVE}",
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
    webrtc_archive = (
        Path(args.webrtc_archive).resolve()
        if args.webrtc_archive
        else None
    )
    if qt_archive and not qt_archive.is_file():
        error(f"Qt archive does not exist: {qt_archive}")
    if libraries_source and not libraries_source.is_dir():
        error(f"Libraries source directory does not exist: {libraries_source}")
    if webrtc_archive and not webrtc_archive.is_file():
        error(f"WebRTC package does not exist: {webrtc_archive}")

    preflight_inputs(qt_archive, libraries_source, webrtc_archive)
    deps_dir.mkdir(parents=True, exist_ok=True)

    prepare_webrtc(webrtc_archive)
    prepare_libraries(qt_archive, libraries_source)

    print("\n==================================================")
    print("[SUCCESS] All project dependencies are ready in ./deps!")
    print("==================================================")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile) as exception:
        error(str(exception))
