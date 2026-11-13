'''
LiveKit C++ Client Dependencies Preparation Script
Location: build/prepare/prepare.py
- Self-contained Windows dependency preparation.
- Downloads and deployments are verified before replacing the active tree.
- Telegram Desktop UI is integrated in src/ui/tdesktop as Git submodules.
'''

import hashlib
import errno
import json
import os
import re
import shutil
import stat
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

if sys.platform.startswith("linux") and __name__ == "__main__":
    from prepare_linux import main as prepare_linux_main

    prepare_linux_main()
    raise SystemExit(0)


script_dir = Path(__file__).parent.resolve()
root_dir = script_dir.parent.parent.resolve()
deps_dir = root_dir / "deps"
cache_keys_dir = root_dir / ".cache_keys"

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


_validated_webrtc_archives = set()
_validated_zip_hashes = {}
VERIFY_FULL = False


def load_required_manifest(path: Path):
    return tuple(
        line
        for raw_line in path.read_text(encoding="utf-8").splitlines()
        if (line := raw_line.strip()) and not line.startswith("#")
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
    local_guide_field = "local_source_build_guide_sha256"
    if local_guide_field in metadata:
        if (not isinstance(metadata[local_guide_field], str)
                or not re.fullmatch(r"[0-9a-f]{64}", metadata[local_guide_field])):
            raise ValueError(
                f"Libraries archive metadata has an invalid {local_guide_field}")
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
    local_guide_name = metadata.get("local_source_build_guide", "README.md")
    if (not isinstance(local_guide_name, str)
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*\.md", local_guide_name)):
        raise ValueError("Libraries archive metadata has an invalid local source build guide")
    source_build_guide = path.parent / local_guide_name
    # Older schema-1 metadata used the release guide fingerprint for the local
    # copy as well. The optional local fingerprint allows that copy to evolve
    # without changing the independently pinned release asset.
    local_guide_sha256 = metadata.get(
        local_guide_field, metadata["source_build_guide_sha256"])
    local_guide_hash_mode = metadata.get("local_source_build_guide_hash_mode", "raw")
    if (local_guide_hash_mode not in ("raw", "lf")
            or (local_guide_hash_mode != "raw" and local_guide_field not in metadata)):
        raise ValueError("Libraries archive metadata has an invalid local guide hash mode")
    local_guide_bytes = source_build_guide.read_bytes() if source_build_guide.is_file() else None
    # Normalize only checkout line endings for the local guide. Published assets
    # retain their independently pinned, exact-byte fingerprints.
    if local_guide_bytes is not None and local_guide_hash_mode == "lf":
        local_guide_bytes = local_guide_bytes.replace(b"\r\n", b"\n")
    if (not source_build_guide.is_file()
            or hashlib.sha256(local_guide_bytes).hexdigest()
            != local_guide_sha256):
        raise ValueError(
            "Libraries archive metadata does not match the local source build guide")
    return metadata


_validated_libraries_archives = set()


def initialize_metadata():
    """Load reviewed inputs only for an actual preparation, never at import/help."""
    global WEBRTC_PACKAGE_METADATA, WEBRTC_PACKAGE_ARCHIVE, WEBRTC_PACKAGE_URL
    global WEBRTC_PACKAGE_SHA256, WEBRTC_PACKAGE_SIZE, WEBRTC_PACKAGE_CACHE_ID
    global WEBRTC_REQUIRED_FILES, LIBRARIES_REQUIRED_FILES
    global LIBRARIES_ARCHIVE_METADATA, LIBRARIES_ARCHIVE_NAME, LIBRARIES_ARCHIVE_URL
    global LIBRARIES_ARCHIVE_SHA256, LIBRARIES_ARCHIVE_SIZE, QT_RELEASE_CACHE_ID
    WEBRTC_PACKAGE_METADATA = load_webrtc_package_metadata(WEBRTC_PACKAGE_METADATA_PATH)
    WEBRTC_PACKAGE_ARCHIVE = WEBRTC_PACKAGE_METADATA["archive"]
    WEBRTC_PACKAGE_URL = WEBRTC_PACKAGE_METADATA["download_url"]
    WEBRTC_PACKAGE_SHA256 = WEBRTC_PACKAGE_METADATA["sha256"]
    WEBRTC_PACKAGE_SIZE = WEBRTC_PACKAGE_METADATA["size"]
    WEBRTC_PACKAGE_CACHE_ID = f"v4:{WEBRTC_PACKAGE_METADATA['package_id']}:{WEBRTC_PACKAGE_SHA256}"
    WEBRTC_REQUIRED_FILES = tuple(WEBRTC_PACKAGE_METADATA["required_files"])
    LIBRARIES_REQUIRED_FILES = load_required_manifest(script_dir / "libraries-required.txt")
    LIBRARIES_ARCHIVE_METADATA = load_libraries_archive_metadata(LIBRARIES_ARCHIVE_METADATA_PATH)
    LIBRARIES_ARCHIVE_NAME = LIBRARIES_ARCHIVE_METADATA["archive"]
    LIBRARIES_ARCHIVE_URL = LIBRARIES_ARCHIVE_METADATA["url"]
    LIBRARIES_ARCHIVE_SHA256 = LIBRARIES_ARCHIVE_METADATA["sha256"]
    LIBRARIES_ARCHIVE_SIZE = LIBRARIES_ARCHIVE_METADATA["size"]
    QT_RELEASE_CACHE_ID = f"v4:qt-5.15.18:{LIBRARIES_ARCHIVE_NAME}:{LIBRARIES_ARCHIVE_SHA256}"


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


def file_identity(path: Path):
    info = path.stat()
    return [info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_dev, info.st_ino]


def write_json_atomic(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix=".verification-",
                                     suffix=".tmp", dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(value, stream, sort_keys=True)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def required_files_fingerprint(root: Path, required_files, metadata=None,
                               verify_full=None, cache_receipt=True) -> str:
    """Reuse local stat receipts; --verify-full always reads the actual bytes.

    These receipts optimize trusted local reuse. They are not cryptographic proof
    against a writer able to preserve filesystem metadata or edit the receipt.
    """
    required_files = tuple(required_files)
    binding = {"schema_version": 1, "root": str(root.resolve()),
               "files": required_files, "metadata": metadata}
    cache_file = cache_keys_dir / "verified" / (compute_string_hash(
        json.dumps([binding["root"], required_files], sort_keys=True)) + ".json")
    snapshots = {}
    for relative in required_files:
        path = root / relative
        if not path.is_file() or path.is_symlink():
            raise ValueError(format_missing(root, [relative]))
        snapshots[relative] = file_identity(path)
    binding_hash = compute_string_hash(json.dumps(binding, sort_keys=True))
    full = VERIFY_FULL if verify_full is None else verify_full
    if not full and cache_receipt:
        try:
            receipt = json.loads(cache_file.read_text(encoding="utf-8"))
            fingerprint = receipt["fingerprint"]
            if (receipt["binding"] == binding_hash and receipt["stats"] == snapshots
                    and isinstance(fingerprint, str)
                    and re.fullmatch(r"[0-9a-f]{64}", fingerprint)):
                return fingerprint
        except (OSError, ValueError, KeyError, TypeError):
            pass
    digest = hashlib.sha256()
    for relative in required_files:
        path = root / relative
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(compute_file_hash(path).encode("ascii"))
        digest.update(b"\0")
    fingerprint = digest.hexdigest()
    # Never publish a receipt for files modified while they were being read.
    if snapshots != {relative: file_identity(root / relative) for relative in required_files}:
        raise ValueError(f"Dependency files changed during verification: {root}")
    if cache_receipt:
        write_json_atomic(cache_file, {"binding": binding_hash, "stats": snapshots,
                                       "fingerprint": fingerprint})
    return fingerprint


def seed_fingerprint_receipt(root, required_files, metadata, fingerprint):
    """Bind a fully verified candidate to its new location after same-disk rename."""
    binding = {"schema_version": 1, "root": str(root.resolve()),
               "files": tuple(required_files), "metadata": metadata}
    cache_file = cache_keys_dir / "verified" / (compute_string_hash(
        json.dumps([binding["root"], tuple(required_files)], sort_keys=True)) + ".json")
    write_json_atomic(cache_file, {
        "binding": compute_string_hash(json.dumps(binding, sort_keys=True)),
        "stats": {relative: file_identity(root / relative) for relative in required_files},
        "fingerprint": fingerprint})


def state_dir_for(target_deps_dir: Path) -> Path:
    resolved = target_deps_dir.resolve()
    if resolved == (root_dir / "deps").resolve():
        return cache_keys_dir
    return resolved / ".prepare-cache"


def check_cache_key(stage_name: str, expected_key: str, state_dir: Path = None) -> str:
    base_dir = state_dir or cache_keys_dir
    key_file = base_dir / stage_name
    if not key_file.is_file():
        return "NotFound"
    try:
        content = key_file.read_text(encoding="utf-8").strip()
        return "Good" if content == expected_key else "Stale"
    except OSError:
        return "Stale"


def write_cache_key(stage_name: str, key: str, state_dir: Path = None):
    base_dir = state_dir or cache_keys_dir
    base_dir.mkdir(parents=True, exist_ok=True)
    key_file = base_dir / stage_name
    temp_key = base_dir / f".{stage_name}.{os.getpid()}.tmp"
    temp_key.write_text(key, encoding="utf-8")
    os.replace(temp_key, key_file)


@contextmanager
def stage_lock(stage_name: str, timeout_seconds=None, lock_dir: Path = None):
    """A process-owned lock, released by the OS even when a preparer crashes."""
    base_dir = lock_dir or cache_keys_dir
    base_dir.mkdir(parents=True, exist_ok=True)
    lock_path = base_dir / f"{stage_name}.lck"
    deadline = None if timeout_seconds is None else time.monotonic() + timeout_seconds
    with lock_path.open("a+b") as stream:
        # msvcrt locks a byte range. Keep this inode/file alive across releases.
        if stream.seek(0, os.SEEK_END) == 0:
            stream.write(b"\0")
            stream.flush()
        while True:
            try:
                stream.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError as exc:
                if exc.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                    raise
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError(f"Timed out waiting for dependency stage lock: {lock_path}") from exc
                time.sleep(0.1)
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def validate_zip(path: Path, expected_sha256: str = None):
    if not path.is_file():
        raise ValueError(f"Dependency archive does not exist: {path}")
    if expected_sha256:
        identity = (str(path.resolve()), *file_identity(path))
        actual = _validated_zip_hashes.get(identity)
        if actual is None:
            actual = compute_file_hash(path)
        if actual.lower() != expected_sha256.lower():
            raise ValueError(
                f"SHA-256 mismatch for {path}: expected {expected_sha256}, "
                f"got {actual}"
            )
        _validated_zip_hashes[identity] = actual
    if not zipfile.is_zipfile(path):
        raise ValueError(f"Dependency archive is not a valid ZIP file: {path}")
    # Extraction reads every member and verifies its CRC; do not decompress twice.


def normalized_zip_files(path: Path):
    with zipfile.ZipFile(path, "r") as archive:
        return {
            member.filename.rstrip("/").lower()
            for member in checked_zip_entries(archive)
            if not member.is_dir()
        }


def checked_zip_entries(archive):
    seen = set()
    entries = archive.infolist()
    for member in entries:
        name = member.orig_filename
        if name != member.filename:
            raise ValueError(f"Unsafe normalized archive path: {name!r}")
        parts = name.rstrip("/").split("/")
        if (not name or name.startswith("/") or "\\" in name
                or any(ord(c) < 32 for c in name)
                or any(part in ("", ".", "..") or ":" in part
                       or part.endswith((".", " ")) for part in parts)
                or any(re.fullmatch(r"(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", part)
                       for part in parts)):
            raise ValueError(f"Unsafe archive path: {name!r}")
        normalized = "/".join(parts).casefold()
        if normalized in seen:
            raise ValueError(f"Duplicate archive entry: {name}")
        seen.add(normalized)
        file_type = stat.S_IFMT(member.external_attr >> 16)
        allowed = (0, stat.S_IFDIR) if member.is_dir() else (0, stat.S_IFREG)
        if file_type not in allowed:
            raise ValueError(f"Unsupported ZIP entry type: {name}")
    return entries


def validate_libraries_archive(path: Path):
    info = path.stat()
    identity = (str(path.resolve()), *file_identity(path),
                compute_string_hash(json.dumps(LIBRARIES_ARCHIVE_METADATA, sort_keys=True)))
    if identity in _validated_libraries_archives:
        return
    if info.st_size != LIBRARIES_ARCHIVE_SIZE:
        raise ValueError(
            f"Qt/Libraries archive size mismatch for {path}: expected "
            f"{LIBRARIES_ARCHIVE_SIZE}, got {info.st_size}. The pinned archive "
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
    return download_archive(LIBRARIES_ARCHIVE_NAME, LIBRARIES_ARCHIVE_URL,
                            LIBRARIES_ARCHIVE_SIZE, LIBRARIES_ARCHIVE_SHA256,
                            validate_libraries_archive)


def validate_webrtc_archive(path: Path):
    info = path.stat()
    identity = (str(path.resolve()), *file_identity(path),
                compute_string_hash(json.dumps(WEBRTC_PACKAGE_METADATA, sort_keys=True)))
    if identity in _validated_webrtc_archives:
        return
    if info.st_size != WEBRTC_PACKAGE_SIZE:
        raise ValueError(
            f"WebRTC package size mismatch for {path}: expected "
            f"{WEBRTC_PACKAGE_SIZE}, got {info.st_size}")
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
    with stage_lock("archive-" + compute_string_hash(WEBRTC_PACKAGE_ARCHIVE)[:16]):
        return _cache_webrtc_archive(source_path)


def _cache_webrtc_archive(source_path: Path) -> Path:
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
    return download_archive(WEBRTC_PACKAGE_ARCHIVE, WEBRTC_PACKAGE_URL,
                            WEBRTC_PACKAGE_SIZE, WEBRTC_PACKAGE_SHA256,
                            validate_webrtc_archive)


def download_archive(name, url, expected_size, expected_hash, validator):
    with stage_lock("archive-" + compute_string_hash(name)[:16]):
        archive_path = cache_keys_dir / name
        if archive_path.is_file():
            try:
                validator(archive_path)
                return archive_path
            except (OSError, ValueError, zipfile.BadZipFile):
                archive_path.unlink()
                print(f"[CACHE] Removed an invalid archive: {name}")
        last_error = None
        for attempt in range(1, 4):
            part_path = cache_keys_dir / f".{name}.{os.getpid()}.{attempt}.part"
            print(f"[DOWNLOAD] {url} (attempt {attempt}/3)")
            try:
                digest = hashlib.sha256()
                downloaded = 0
                request = urllib.request.Request(url, headers={
                    "User-Agent": "Cohavora-dependency-preparer/1"})
                with urllib.request.urlopen(request, timeout=60) as response:
                    with part_path.open("wb") as destination:
                        while chunk := response.read(1024 * 1024):
                            downloaded += len(chunk)
                            if downloaded > expected_size:
                                raise ValueError("Download exceeded the pinned archive size")
                            destination.write(chunk)
                            digest.update(chunk)
                actual_hash = digest.hexdigest()
                if downloaded != expected_size or actual_hash != expected_hash:
                    raise ValueError(f"Downloaded archive size or SHA-256 mismatch: {name}")
                _validated_zip_hashes[(str(part_path.resolve()), *file_identity(part_path))] = actual_hash
                validator(part_path)
                os.replace(part_path, archive_path)
                _validated_zip_hashes[(str(archive_path.resolve()), *file_identity(archive_path))] = actual_hash
                return archive_path
            except (OSError, ValueError, urllib.error.URLError, zipfile.BadZipFile) as exc:
                last_error = exc
                if attempt < 3:
                    time.sleep(attempt)
            finally:
                part_path.unlink(missing_ok=True)
        raise ValueError(f"Failed to download and verify {url}: {last_error}")


def resolve_webrtc_archive(package_path: Path = None) -> Path:
    if package_path:
        return cache_webrtc_archive(package_path)
    return download_webrtc_archive()


def safe_extract_zip(archive_path: Path, extract_to: Path):
    extract_to.mkdir(parents=True, exist_ok=True)
    extract_root = extract_to.resolve()
    with zipfile.ZipFile(archive_path, "r") as archive:
        for member in checked_zip_entries(archive):
            destination = (extract_to / member.filename).resolve()
            try:
                common = os.path.commonpath((str(extract_root), str(destination)))
            except ValueError:
                raise ValueError(f"Archive entry escapes the extraction root: {member.filename}")
            if common != str(extract_root):
                raise ValueError(f"Archive entry escapes the extraction root: {member.filename}")
        archive.extractall(extract_to)


def remove_path(path: Path):
    if path.is_symlink():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def atomic_replace_directory(staged_dir: Path, target_dir: Path):
    target_dir.parent.mkdir(parents=True, exist_ok=True)
    backup_dir = target_dir.parent / f".{target_dir.name}.backup-{os.getpid()}"
    remove_path(backup_dir)

    had_target = target_dir.exists() or target_dir.is_symlink()
    if had_target:
        try:
            target_dir.rename(backup_dir)
        except PermissionError as exc:
            raise PermissionError(
                f"Cannot replace dependencies at {target_dir}. Close Visual Studio "
                "or stop builds/linkers using these files, then retry.") from exc
    try:
        staged_dir.rename(target_dir)
    except Exception as exc:
        if (had_target and (backup_dir.exists() or backup_dir.is_symlink())
                and not (target_dir.exists() or target_dir.is_symlink())):
            backup_dir.rename(target_dir)
        if isinstance(exc, PermissionError):
            raise PermissionError(
                f"Cannot install dependencies at {target_dir}. Close Visual Studio "
                "or stop builds/linkers using these files, then retry.") from exc
        raise
    else:
        remove_path(backup_dir)


def find_webrtc_payload(extract_root: Path) -> Path:
    candidate = extract_root / WEBRTC_PACKAGE_METADATA["archive_prefix"]
    if (candidate / "metadata/package.json").is_file():
        return candidate
    raise ValueError(f"WebRTC package has no expected SDK root: {extract_root}")


def webrtc_install_is_valid(target_dir: Path, verify_full=None, cache_receipt=True) -> bool:
    if missing_required_files(target_dir, WEBRTC_REQUIRED_FILES):
        return False
    manifest = target_dir / "metadata/package.json"
    if (not manifest.is_file() or manifest.is_symlink()
            or hashlib.sha256(manifest.read_bytes()).hexdigest()
            != WEBRTC_PACKAGE_METADATA["manifest_sha256"]):
        return False
    return (
        required_files_fingerprint(target_dir, WEBRTC_REQUIRED_FILES,
                                   WEBRTC_PACKAGE_METADATA, verify_full, cache_receipt)
        == WEBRTC_PACKAGE_METADATA["required_files_sha256"]
    )


def is_existing_webrtc_valid(target_dir: Path) -> bool:
    if target_dir.is_symlink() or not target_dir.is_dir():
        return False
    try:
        return webrtc_install_is_valid(target_dir)
    except (OSError, ValueError):
        return False


def prepare_webrtc(package_archive_path: Path = None, target_deps_dir: Path = None,
                   no_repair: bool = False):
    stage_name = "webrtc"
    active_deps_dir = target_deps_dir or deps_dir
    state_dir = state_dir_for(active_deps_dir)
    target_dir = active_deps_dir / "webrtc"
    expected_key = compute_string_hash(WEBRTC_PACKAGE_CACHE_ID)
    with stage_lock(stage_name, lock_dir=state_dir):
        if is_existing_webrtc_valid(target_dir):
            if check_cache_key(stage_name, expected_key, state_dir) != "Good":
                write_cache_key(stage_name, expected_key, state_dir)
            print("[STAGE: WebRTC] OK (local cache; use --verify-full for full hashing)")
            return target_dir

        if target_dir.exists() or target_dir.is_symlink():
            if no_repair:
                raise ValueError(
                    f"Existing {target_dir} does not match the pinned WebRTC package. "
                    "The existing tree was preserved. Move it aside manually before "
                    "preparing dependencies.")
            print(f"[REPAIR] Replacing mismatched WebRTC in {target_dir}...")

        archive_path = resolve_webrtc_archive(package_archive_path)

        target_dir.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix=".webrtc-stage-", dir=target_dir.parent
        ) as temp_dir:
            temp_root = Path(temp_dir)
            candidate = temp_root / "candidate"
            extracted = temp_root / "extracted"
            print(f"[EXTRACT] Extracting {archive_path.name} into staging...")
            safe_extract_zip(archive_path, extracted)
            find_webrtc_payload(extracted).rename(candidate)
            if not webrtc_install_is_valid(candidate, verify_full=True, cache_receipt=False):
                raise ValueError(
                    "Staged WebRTC package content fingerprint does not match "
                    "the pinned dual-config package")

            atomic_replace_directory(candidate, target_dir)
            seed_fingerprint_receipt(target_dir, WEBRTC_REQUIRED_FILES,
                                     WEBRTC_PACKAGE_METADATA,
                                     WEBRTC_PACKAGE_METADATA["required_files_sha256"])

        write_cache_key(stage_name, expected_key, state_dir)

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


def prepare_libraries(qt_archive_path: Path = None, libraries_source: Path = None,
                      target_deps_dir: Path = None, no_repair: bool = False):
    stage_name = "libraries"
    active_deps_dir = target_deps_dir or deps_dir
    state_dir = state_dir_for(active_deps_dir)
    target_dir = active_deps_dir / "Libraries/win64"
    state_path = state_dir / "libraries-state.json"
    binding = compute_string_hash(json.dumps({
        "root": str(target_dir.resolve()), "metadata": LIBRARIES_ARCHIVE_METADATA,
        "required_files": LIBRARIES_REQUIRED_FILES}, sort_keys=True))
    with stage_lock(stage_name, lock_dir=state_dir):
        source_root = None
        if qt_archive_path:
            validate_libraries_archive(qt_archive_path)
            expected_fingerprint = LIBRARIES_ARCHIVE_METADATA["required_files_sha256"]
        elif libraries_source:
            source_root = find_libraries_root(libraries_source)
            expected_fingerprint = required_files_fingerprint(
                source_root, LIBRARIES_REQUIRED_FILES, LIBRARIES_ARCHIVE_METADATA)
        else:
            expected_fingerprint = LIBRARIES_ARCHIVE_METADATA["required_files_sha256"]
            try:
                state = json.loads(state_path.read_text(encoding="utf-8"))
                if (state["binding"] == binding
                        and re.fullmatch(r"[0-9a-f]{64}", state["fingerprint"])):
                    expected_fingerprint = state["fingerprint"]
            except (OSError, ValueError, KeyError, TypeError):
                pass

        if not target_dir.is_symlink() and target_dir.is_dir():
            missing = missing_required_files(target_dir, LIBRARIES_REQUIRED_FILES)
            if not missing:
                try:
                    actual_fingerprint = required_files_fingerprint(
                        target_dir, LIBRARIES_REQUIRED_FILES, LIBRARIES_ARCHIVE_METADATA)
                except (OSError, ValueError):
                    actual_fingerprint = None
                if (actual_fingerprint is not None
                        and not qt_archive_path and not libraries_source
                        and actual_fingerprint != expected_fingerprint
                        and any(check_cache_key(stage_name, compute_string_hash(
                            f"{QT_RELEASE_CACHE_ID}:{kind}:{actual_fingerprint}"), state_dir) == "Good"
                                for kind in ("preinstalled", "directory"))):
                    # Migrate only an unchanged tree whose content fingerprint was
                    # already recorded by the previous preparer, never a new baseline.
                    expected_fingerprint = actual_fingerprint
                if actual_fingerprint == expected_fingerprint:
                    write_json_atomic(state_path, {"binding": binding,
                                                   "fingerprint": expected_fingerprint})
                    write_cache_key(stage_name, compute_string_hash(
                        f"{QT_RELEASE_CACHE_ID}:{expected_fingerprint}"), state_dir)
                    print("[STAGE: Libraries] OK (local cache; use --verify-full for full hashing)")
                    return target_dir

        if target_dir.exists() or target_dir.is_symlink():
            if no_repair:
                raise ValueError(
                    f"Existing {target_dir} does not match the expected Libraries package. "
                    "The existing tree was preserved. Move it aside manually before "
                    "preparing dependencies.")
            print(f"[REPAIR] Replacing mismatched Libraries in {target_dir}...")

        if not qt_archive_path and not libraries_source:
            qt_archive_path = download_libraries_archive()
            expected_fingerprint = LIBRARIES_ARCHIVE_METADATA["required_files_sha256"]
        target_dir.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".libraries-stage-", dir=target_dir.parent) as temp_dir:
            temp_root = Path(temp_dir)
            candidate = temp_root / "candidate"
            if qt_archive_path:
                extracted = temp_root / "extracted"
                print(f"[EXTRACT] Extracting {qt_archive_path} into staging...")
                safe_extract_zip(qt_archive_path, extracted)
                source_root = find_libraries_root(extracted)
                # Only our temporary extraction may be moved. External sources stay intact.
                source_root.rename(candidate)
                ignore = shutil.ignore_patterns(".git*", "*.obj", "*.tlog", "*.pch")
                for directory, dirs, files in os.walk(candidate, topdown=True):
                    ignored = ignore(directory, dirs + files)
                    for name in ignored:
                        path = Path(directory) / name
                        if path.is_dir():
                            shutil.rmtree(path)
                            dirs.remove(name)
                        else:
                            path.unlink()
            else:
                ignore = shutil.ignore_patterns(".git*", "*.obj", "*.tlog", "*.pch")
                shutil.copytree(source_root, candidate, ignore=ignore)
            ensure_zlib_alias(candidate)
            candidate_fingerprint = required_files_fingerprint(
                candidate, LIBRARIES_REQUIRED_FILES, LIBRARIES_ARCHIVE_METADATA,
                verify_full=True, cache_receipt=False)
            if candidate_fingerprint != expected_fingerprint:
                raise ValueError("Staged Libraries content fingerprint does not match its verified source")
            atomic_replace_directory(candidate, target_dir)
            seed_fingerprint_receipt(target_dir, LIBRARIES_REQUIRED_FILES,
                                     LIBRARIES_ARCHIVE_METADATA, candidate_fingerprint)
        write_json_atomic(state_path, {"binding": binding, "fingerprint": expected_fingerprint})
        write_cache_key(stage_name, compute_string_hash(f"{QT_RELEASE_CACHE_ID}:{expected_fingerprint}"), state_dir)
    print(f" -> Libraries deployed transactionally and verified in {target_dir}")
    return target_dir


def main():
    import argparse

    parser = argparse.ArgumentParser(description="LiveKit C++ Dependencies Pipeline")
    libraries_input = parser.add_mutually_exclusive_group()
    libraries_input.add_argument(
        "--qt-archive",
        type=str,
        help="Path to the pinned Qt/Libraries archive",
    )
    libraries_input.add_argument(
        "--libraries-src",
        type=str,
        help="Path to a complete external Libraries directory",
    )
    parser.add_argument(
        "--webrtc-archive",
        type=str,
        help="Path to the pinned dual-config WebRTC archive",
    )
    parser.add_argument(
        "--deps-dir",
        type=Path,
        default=root_dir / "deps",
        help="Dependency installation directory (default: ./deps)",
    )
    parser.add_argument("--verify-full", action="store_true",
                        help="Hash every required installed file; bypass local stat receipts")
    parser.add_argument("--no-repair", "--norepair", dest="no_repair", action="store_true",
                        help="Preserve an existing mismatched dependency tree and fail instead of replacing it")
    args = parser.parse_args()
    global VERIFY_FULL
    VERIFY_FULL = args.verify_full
    initialize_metadata()
    cache_keys_dir.mkdir(parents=True, exist_ok=True)

    target_deps_dir = args.deps_dir.expanduser()
    if not target_deps_dir.is_absolute():
        target_deps_dir = root_dir / target_deps_dir
    target_deps_dir = target_deps_dir.resolve()

    print("==================================================")
    print("  LiveKit C++ Self-Contained Dependency Pipeline  ")
    print("  (Native In-Source UI | Pure C++ Toolchain)      ")
    print("==================================================")
    print(f"Project Root  : {root_dir}")
    print(f"Target Deps   : {target_deps_dir}\n")
    print(
        "Command       : "
        + subprocess.list2cmdline(
            [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]]
        )
        + "\n"
    )

    qt_archive = Path(args.qt_archive).expanduser().resolve() if args.qt_archive else None
    libraries_source = (
        Path(args.libraries_src).expanduser().resolve() if args.libraries_src else None
    )
    webrtc_archive = (
        Path(args.webrtc_archive).expanduser().resolve()
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
    target_deps_dir.mkdir(parents=True, exist_ok=True)

    prepare_webrtc(webrtc_archive, target_deps_dir, args.no_repair)
    prepare_libraries(qt_archive, libraries_source, target_deps_dir, args.no_repair)

    print("\n==================================================")
    print(f"[SUCCESS] All project dependencies are ready in {target_deps_dir}!")
    print("==================================================")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile) as exception:
        error(str(exception))
