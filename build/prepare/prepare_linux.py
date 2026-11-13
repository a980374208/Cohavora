"""Install the pinned Ubuntu Qt and WebRTC release archives into ./deps."""

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from contextlib import contextmanager
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent
METADATA_PATH = SCRIPT_DIR / "linux-packages.json"
_validated_archives = set()
HASH_CACHE_VERSION = 1


class PrepareError(Exception):
    pass


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_identity(path):
    info = path.stat()
    return [info.st_size, info.st_mtime_ns, info.st_ctime_ns,
            info.st_dev, info.st_ino]


def package_identity(package):
    return hashlib.sha256(json.dumps(
        package, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def archive_identity(path, package):
    return (str(path.resolve()), tuple(file_identity(path)),
            package_identity(package))


class FileHashes:
    """Local stat cache, not a substitute for explicit full integrity checks."""

    def __init__(self, root, package, kind, files, cache_dir=None,
                 verify_full=False):
        self.root = root
        self.package = package
        self.kind = kind
        self.files = sorted(files)
        self.entries = {}
        self.cached = {}
        if cache_dir is not None and not verify_full:
            try:
                value = json.loads(self.cache_path(cache_dir, root).read_text(
                    encoding="utf-8"))
                if (isinstance(value, dict)
                        and value.get("binding") == self.binding(root)
                        and isinstance(value.get("entries"), dict)):
                    self.cached = value["entries"]
            except (OSError, ValueError, TypeError):
                pass

    def binding(self, root):
        return {"version": HASH_CACHE_VERSION, "root": str(root.resolve()),
                "package": package_identity(self.package), "kind": self.kind,
                "files": self.files}

    def cache_path(self, cache_dir, root):
        name = hashlib.sha256((str(root.resolve()) + "\0" + self.kind).encode()).hexdigest()
        return cache_dir / ("hash-" + name + ".json")

    def digest(self, relative):
        if relative in self.entries:
            return self.entries[relative]["sha256"]
        path = self.root / relative
        before = file_identity(path)
        cached = self.cached.get(relative)
        if (isinstance(cached, dict) and cached.get("stat") == before
                and isinstance(cached.get("sha256"), str)
                and re.fullmatch(r"[0-9a-f]{64}", cached["sha256"])):
            digest = cached["sha256"]
        else:
            digest = sha256_file(path)
        if file_identity(path) != before:
            raise PrepareError(f"Dependency changed during verification: {path}")
        self.entries[relative] = {"stat": before, "sha256": digest}
        return digest

    def save(self, cache_dir, root=None):
        if cache_dir is None:
            return
        root = self.root if root is None else root
        temporary_path = None
        try:
            cache_dir.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                    mode="w", encoding="utf-8", prefix=".hash-", suffix=".tmp",
                    dir=cache_dir, delete=False) as temporary:
                temporary_path = Path(temporary.name)
                json.dump({"binding": self.binding(root), "entries": self.entries},
                          temporary, sort_keys=True)
                temporary.write("\n")
            os.replace(temporary_path, self.cache_path(cache_dir, root))
        except OSError:
            # An unwritable optional cache must not invalidate verified files.
            pass
        finally:
            if temporary_path is not None:
                try:
                    temporary_path.unlink(missing_ok=True)
                except OSError:
                    pass


def safe_parts(name):
    if (not name or name.startswith("/") or "\\" in name
            or "\0" in name or "\n" in name or "\r" in name):
        raise PrepareError(f"Unsafe archive path: {name!r}")
    parts = tuple(name.rstrip("/").split("/"))
    if not parts or any(part in ("", ".", "..") for part in parts):
        raise PrepareError(f"Unsafe archive path: {name!r}")
    return parts


def load_metadata():
    metadata = json.loads(METADATA_PATH.read_text(encoding="utf-8"))
    if metadata.get("schema_version") != 1:
        raise PrepareError("Unsupported Ubuntu package metadata schema")
    for kind in ("qt", "webrtc"):
        package = metadata[kind]
        archive = package["archive"]
        if (Path(archive).name != archive
                or not archive.endswith(".zip" if kind == "qt" else ".tar.gz")):
            raise PrepareError(f"Invalid {kind} archive name")
        url = urllib.parse.urlparse(package["url"])
        if (url.scheme != "https" or url.netloc != "github.com"
                or not url.path.endswith("/" + archive)):
            raise PrepareError(f"Invalid {kind} download URL")
        if (not isinstance(package["size"], int) or package["size"] <= 0
                or not isinstance(package["file_count"], int)
                or package["file_count"] <= 0):
            raise PrepareError(f"Invalid {kind} archive size or file count")
        if not re.fullmatch(r"[0-9a-f]{64}", package["sha256"]):
            raise PrepareError(f"Invalid {kind} archive SHA-256")
        safe_parts(package["archive_prefix"])
        required = package["required_files"]
        if not isinstance(required, dict) or not required:
            raise PrepareError(f"Invalid {kind} required-file list")
        for relative, digest in required.items():
            safe_parts(relative)
            if not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise PrepareError(f"Invalid {kind} required-file SHA-256")
    return metadata


def validate_archive(path, package):
    if not path.is_file():
        raise PrepareError(f"Archive does not exist: {path}")
    if path.stat().st_size != package["size"]:
        raise PrepareError(f"Archive size mismatch: {path}")
    identity = archive_identity(path, package)
    if identity in _validated_archives:
        return
    actual = sha256_file(path)
    if actual != package["sha256"]:
        raise PrepareError(
            f"Archive SHA-256 mismatch: {path}; expected {package['sha256']}, got {actual}")
    if archive_identity(path, package) != identity:
        raise PrepareError(f"Archive changed during verification: {path}")
    _validated_archives.add(identity)


def download_archive(deps_dir, package):
    cache_dir = deps_dir / ".archives"
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive_path = cache_dir / package["archive"]
    if archive_path.exists():
        validate_archive(archive_path, package)
        print(f"[CACHE] Verified {archive_path}")
        return archive_path

    last_error = None
    for attempt in range(1, 4):
        with tempfile.NamedTemporaryFile(
                mode="wb", prefix=f".{package['archive']}.", suffix=".part",
                dir=cache_dir, delete=False) as temporary:
            temporary_path = Path(temporary.name)
        try:
            request = urllib.request.Request(
                package["url"],
                headers={"User-Agent": "Cohavora-Ubuntu-dependency-preparer/1"})
            print(f"[DOWNLOAD] {package['url']} (attempt {attempt}/3)")
            with temporary_path.open("wb") as destination:
                with urllib.request.urlopen(request, timeout=60) as response:
                    downloaded = 0
                    digest = hashlib.sha256()
                    while chunk := response.read(1024 * 1024):
                        downloaded += len(chunk)
                        if downloaded > package["size"]:
                            raise PrepareError("Download exceeded the pinned archive size")
                        destination.write(chunk)
                        digest.update(chunk)
            if downloaded != package["size"] or digest.hexdigest() != package["sha256"]:
                raise PrepareError("Downloaded archive size or SHA-256 mismatch")
            os.replace(temporary_path, archive_path)
            _validated_archives.add(archive_identity(archive_path, package))
            return archive_path
        except (OSError, urllib.error.URLError, PrepareError) as exc:
            last_error = exc
            if attempt < 3:
                time.sleep(attempt)
        finally:
            temporary_path.unlink(missing_ok=True)
    raise PrepareError(f"Failed to download {package['archive']}: {last_error}")


def archive_relative(parts, prefix, is_dir):
    prefix_parts = safe_parts(prefix)
    if parts == prefix_parts or prefix_parts[:len(parts)] == parts:
        if not is_dir:
            raise PrepareError("Archive prefix contains a regular file")
        return None
    if parts[:len(prefix_parts)] != prefix_parts:
        raise PrepareError(f"Unexpected archive path: {'/'.join(parts)}")
    return Path(*parts[len(prefix_parts):])


def mode_for_file(mode):
    return 0o644 | (mode & 0o111)


def extract_zip(archive_path, package, candidate):
    count = 0
    seen = set()
    with zipfile.ZipFile(archive_path) as archive:
        entries = archive.infolist()
        for entry in entries:
            parts = safe_parts(entry.filename)
            relative = archive_relative(
                parts, package["archive_prefix"], entry.is_dir())
            if relative is None:
                continue
            if relative in seen:
                raise PrepareError(f"Duplicate archive entry: {entry.filename}")
            seen.add(relative)
            file_type = stat.S_IFMT(entry.external_attr >> 16)
            if entry.is_dir():
                if file_type not in (0, stat.S_IFDIR):
                    raise PrepareError(f"Unsupported ZIP entry: {entry.filename}")
                continue
            if file_type not in (0, stat.S_IFREG):
                raise PrepareError(f"Unsupported ZIP entry: {entry.filename}")
            count += 1
        if count != package["file_count"]:
            raise PrepareError(
                f"Qt archive file count mismatch: {count} != {package['file_count']}")

        for entry in entries:
            relative = archive_relative(
                safe_parts(entry.filename), package["archive_prefix"],
                entry.is_dir())
            if relative is None:
                continue
            destination = candidate / relative
            if entry.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, destination.open("wb") as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
            destination.chmod(mode_for_file(entry.external_attr >> 16))


def extract_tar(archive_path, package, candidate):
    count = 0
    seen = set()
    with tarfile.open(archive_path, "r:gz") as archive:
        members = archive.getmembers()
        for member in members:
            parts = safe_parts(member.name)
            relative = archive_relative(
                parts, package["archive_prefix"], member.isdir())
            if relative is None:
                continue
            if relative in seen:
                raise PrepareError(f"Duplicate archive entry: {member.name}")
            seen.add(relative)
            if not (member.isdir() or member.isfile()):
                raise PrepareError(f"Unsupported TAR entry: {member.name}")
            if member.isfile():
                count += 1
        if count != package["file_count"]:
            raise PrepareError(
                f"WebRTC archive file count mismatch: {count} != {package['file_count']}")

        for member in members:
            relative = archive_relative(
                safe_parts(member.name), package["archive_prefix"],
                member.isdir())
            if relative is None:
                continue
            destination = candidate / relative
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            source = archive.extractfile(member)
            if source is None:
                raise PrepareError(f"Unable to read TAR entry: {member.name}")
            with source, destination.open("wb") as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
            destination.chmod(mode_for_file(member.mode))


def inventory(root):
    files = set()
    for directory, dirs, names in os.walk(root, followlinks=False):
        parent = Path(directory)
        for name in dirs:
            if (parent / name).is_symlink():
                raise PrepareError(f"Dependency tree contains a symlink: {parent / name}")
        for name in names:
            path = parent / name
            if path.is_symlink() or not path.is_file():
                raise PrepareError(f"Dependency tree contains a non-file: {path}")
            files.add(path.relative_to(root).as_posix())
    return files


def verify_webrtc_manifest(root, files, hashes=None):
    manifest = root / "metadata/checksums.sha256"
    expected = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  \./(.+)", line)
        if not match:
            raise PrepareError("Invalid WebRTC checksum manifest entry")
        relative = match.group(2)
        safe_parts(relative)
        if relative in expected:
            raise PrepareError(f"Duplicate WebRTC checksum: {relative}")
        expected[relative] = match.group(1)
    if set(expected) != files - {"metadata/checksums.sha256"}:
        raise PrepareError("WebRTC checksum manifest does not cover the package")
    for relative, digest in expected.items():
        actual = hashes.digest(relative) if hashes else sha256_file(root / relative)
        if actual != digest:
            raise PrepareError(f"WebRTC file checksum mismatch: {relative}")


def validate_install(root, package, kind, cache_dir=None, verify_full=False):
    if not root.is_dir() or root.is_symlink():
        raise PrepareError(f"Expected a regular dependency directory: {root}")
    files = inventory(root)
    if len(files) != package["file_count"]:
        raise PrepareError(
            f"{kind} file count mismatch in {root}: {len(files)} != {package['file_count']}")
    hashes = FileHashes(root, package, kind, files, cache_dir, verify_full)
    for relative, digest in package["required_files"].items():
        path = root / relative
        if relative not in files or hashes.digest(relative) != digest:
            raise PrepareError(f"{kind} required file mismatch: {path}")
    if kind == "webrtc":
        verify_webrtc_manifest(root, files, hashes)
    hashes.save(cache_dir)
    return hashes


@contextmanager
def dependency_lock(deps_dir, kind):
    # Keep --help and pure validation importable on non-POSIX hosts.
    try:
        import fcntl
    except ImportError as exc:
        raise PrepareError("Ubuntu dependency installation requires POSIX file locks") from exc
    lock_dir = deps_dir / ".prepare-cache"
    lock_dir.mkdir(parents=True, exist_ok=True)
    with (lock_dir / f"{kind}.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def find_qt_source(path):
    for candidate in (path, path / "Libraries/linux64", path / "linux64"):
        if (candidate / "lib/cmake/Qt5/Qt5Config.cmake").is_file():
            return candidate
    raise PrepareError(f"No Linux Qt installation found under {path}")


def remove_path(path):
    if path.is_symlink():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def atomic_replace_path(staged_dir, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    backup = target.parent / f".{target.name}.backup-{os.getpid()}"
    remove_path(backup)
    had_target = target.exists() or target.is_symlink()
    if had_target:
        target.rename(backup)
    try:
        staged_dir.rename(target)
    except Exception:
        if (had_target and (backup.exists() or backup.is_symlink())
                and not (target.exists() or target.is_symlink())):
            backup.rename(target)
        raise
    else:
        remove_path(backup)


def prepare_one(deps_dir, kind, package, archive_override=None, source=None,
                verify_full=False, no_repair=False):
    target = (deps_dir / "Libraries/linux64" if kind == "qt"
              else deps_dir / "webrtc")
    cache_dir = deps_dir / ".prepare-cache"
    with dependency_lock(deps_dir, kind):
        if target.exists() or target.is_symlink():
            try:
                validate_install(target, package, kind, cache_dir, verify_full)
            except PrepareError as exc:
                if no_repair:
                    raise PrepareError(
                        f"Existing {target} does not match the pinned {kind} package: {exc}. "
                        "The existing tree was preserved. Move it aside manually before "
                        "preparing dependencies.") from exc
                print(f"[REPAIR] Replacing mismatched {kind} in {target}: {exc}")
            else:
                print(f"[REUSE] Verified {kind} in {target}")
                return target

        if source:
            source_root = find_qt_source(source)
            validate_install(source_root, package, kind, cache_dir, verify_full)
            archive_path = None
        else:
            archive_path = archive_override or download_archive(deps_dir, package)
            validate_archive(archive_path, package)

        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
                prefix=f".{kind}-stage-", dir=target.parent) as temporary:
            candidate = Path(temporary) / "payload"
            candidate.mkdir()
            if source:
                shutil.copytree(source_root, candidate, dirs_exist_ok=True)
            elif kind == "qt":
                extract_zip(archive_path, package, candidate)
            else:
                extract_tar(archive_path, package, candidate)
            hashes = validate_install(candidate, package, kind, verify_full=True)
            atomic_replace_path(candidate, target)
            hashes.save(cache_dir, target)
        print(f"[READY] Installed {kind} in {target}")
        return target


def main():
    parser = argparse.ArgumentParser(
        description="Prepare pinned Ubuntu Qt and WebRTC SDKs in ./deps")
    qt_input = parser.add_mutually_exclusive_group()
    qt_input.add_argument("--qt-archive", type=Path,
                          help="Local copy of the pinned Ubuntu Qt ZIP")
    qt_input.add_argument("--libraries-src", type=Path,
                          help="Complete extracted Ubuntu Qt installation")
    parser.add_argument("--webrtc-archive", type=Path,
                        help="Local copy of the pinned Ubuntu WebRTC tar.gz")
    parser.add_argument("--deps-dir", type=Path, default=PROJECT_ROOT / "deps",
                        help="Dependency installation directory (default: ./deps)")
    parser.add_argument("--verify-full", action="store_true",
                        help="Hash all verified files instead of using the local stat cache")
    parser.add_argument("--no-repair", "--norepair", dest="no_repair", action="store_true",
                        help="Preserve an existing mismatched dependency tree and fail instead of replacing it")
    args = parser.parse_args()
    try:
        packages = load_metadata()
        deps_dir = args.deps_dir.expanduser()
        if not deps_dir.is_absolute():
            deps_dir = PROJECT_ROOT / deps_dir
        deps_dir = deps_dir.resolve()
        deps_dir.mkdir(parents=True, exist_ok=True)
        qt_archive = args.qt_archive.expanduser().resolve() if args.qt_archive else None
        webrtc_archive = (args.webrtc_archive.expanduser().resolve()
                          if args.webrtc_archive else None)
        qt_source = args.libraries_src.expanduser().resolve() if args.libraries_src else None
        if qt_archive:
            validate_archive(qt_archive, packages["qt"])
        if webrtc_archive:
            validate_archive(webrtc_archive, packages["webrtc"])
        if qt_source:
            validate_install(find_qt_source(qt_source), packages["qt"], "qt",
                             deps_dir / ".prepare-cache", args.verify_full)
        print(f"[INFO] Ubuntu dependency directory: {deps_dir}")
        prepare_one(deps_dir, "qt", packages["qt"], qt_archive, qt_source,
                    args.verify_full, args.no_repair)
        prepare_one(deps_dir, "webrtc", packages["webrtc"], webrtc_archive,
                    verify_full=args.verify_full, no_repair=args.no_repair)
        print("[SUCCESS] Ubuntu Qt and WebRTC are ready")
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile,
            tarfile.TarError, urllib.error.URLError, PrepareError) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


if __name__ == "__main__":
    main()
