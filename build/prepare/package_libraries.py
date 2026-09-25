"""Create the pinned Windows Qt/Libraries dependency archive."""

import argparse
import hashlib
import json
import os
import shutil
import sys
import zipfile
from pathlib import Path, PurePosixPath


SCRIPT_DIR = Path(__file__).parent.resolve()
ROOT_DIR = SCRIPT_DIR.parent.parent.resolve()
REQUIRED_MANIFEST = SCRIPT_DIR / "libraries-required.txt"
ARCHIVE_METADATA = SCRIPT_DIR / "libraries-archive.json"
DEFAULT_SOURCE = ROOT_DIR / "deps" / "Libraries" / "win64"
DEFAULT_OUTPUT = (
    ROOT_DIR
    / "out"
    / "dependency-archives"
    / "cohavora-libraries-win64-qt-5.15.18-20260925.zip"
)
SOURCE_BUILD_GUIDE = SCRIPT_DIR / "README.md"
SOURCE_BUILD_GUIDE_ASSET_NAME = (
    "cohavora-libraries-win64-qt-5.15.18-20260925-source-build.md"
)
RELEASE_TAG = "dependencies-qt-5.15.18-20260925"
ARCHIVE_URL = (
    "https://github.com/a980374208/Cohavora/releases/download/"
    f"{RELEASE_TAG}/{DEFAULT_OUTPUT.name}"
)
SOURCE_BUILD_GUIDE_URL = (
    "https://github.com/a980374208/Cohavora/releases/download/"
    f"{RELEASE_TAG}/{SOURCE_BUILD_GUIDE_ASSET_NAME}"
)
ARCHIVE_PREFIX = PurePosixPath("Libraries/win64")
FIXED_TIMESTAMP = (2026, 9, 25, 0, 0, 0)
IGNORED_SUFFIXES = {".obj", ".pch", ".tlog"}


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def required_files():
    return tuple(
        line
        for raw_line in REQUIRED_MANIFEST.read_text(encoding="utf-8").splitlines()
        if (line := raw_line.strip()) and not line.startswith("#")
    )


def required_files_fingerprint(source_root: Path, required) -> str:
    digest = hashlib.sha256()
    for relative in required:
        path = source_root / relative
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_sha256(path).encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest()


def should_package(path: Path, source_root: Path) -> bool:
    relative = path.relative_to(source_root)
    if any(part.lower().startswith(".git") for part in relative.parts):
        return False
    return path.suffix.lower() not in IGNORED_SUFFIXES


def source_files(source_root: Path):
    return sorted(
        (path for path in source_root.rglob("*")
         if path.is_file() and should_package(path, source_root)),
        key=lambda path: path.relative_to(source_root).as_posix(),
    )


def ensure_output_outside_source(source_root: Path, output: Path):
    try:
        common = os.path.commonpath((str(source_root), str(output)))
    except ValueError:
        return
    if common == str(source_root):
        raise ValueError("Archive output must be outside the Libraries source tree")


def write_archive(source_root: Path, output: Path, files):
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.parent / f".{output.name}.{os.getpid()}.tmp"
    temporary.unlink(missing_ok=True)
    total_bytes = sum(path.stat().st_size for path in files)
    written_bytes = 0
    next_report = 512 * 1024 * 1024
    try:
        with zipfile.ZipFile(
                temporary,
                mode="w",
                compression=zipfile.ZIP_DEFLATED,
                compresslevel=6,
                allowZip64=True) as archive:
            for index, path in enumerate(files, 1):
                relative = path.relative_to(source_root).as_posix()
                archive_name = str(ARCHIVE_PREFIX / PurePosixPath(relative))
                info = zipfile.ZipInfo(archive_name, date_time=FIXED_TIMESTAMP)
                info.compress_type = zipfile.ZIP_DEFLATED
                info._compresslevel = 6
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                with path.open("rb") as source, archive.open(
                        info, mode="w", force_zip64=True) as destination:
                    shutil.copyfileobj(source, destination, 1024 * 1024)
                written_bytes += path.stat().st_size
                if written_bytes >= next_report or index == len(files):
                    print(
                        f"[PACKAGE] {index}/{len(files)} files, "
                        f"{written_bytes / (1024 ** 3):.2f}/"
                        f"{total_bytes / (1024 ** 3):.2f} GiB read",
                        flush=True,
                    )
                    next_report += 512 * 1024 * 1024
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def verify_archive(path: Path):
    with zipfile.ZipFile(path, "r") as archive:
        members = [member for member in archive.infolist() if not member.is_dir()]
        for index, member in enumerate(members, 1):
            with archive.open(member, "r") as source:
                while source.read(1024 * 1024):
                    pass
            if index % 1000 == 0 or index == len(members):
                print(f"[VERIFY] CRC {index}/{len(members)} files", flush=True)


def write_metadata(
        output: Path, source_root: Path, files, required,
        source_build_guide: Path):
    metadata = {
        "schema_version": 1,
        "archive": output.name,
        "url": ARCHIVE_URL,
        "size": output.stat().st_size,
        "sha256": file_sha256(output),
        "required_manifest_sha256": file_sha256(REQUIRED_MANIFEST),
        "required_files_sha256": required_files_fingerprint(
            source_root, required),
        "file_count": len(files),
        "archive_prefix": str(ARCHIVE_PREFIX),
        "source_build_guide": source_build_guide.name,
        "source_build_guide_url": SOURCE_BUILD_GUIDE_URL,
        "source_build_guide_sha256": file_sha256(source_build_guide),
    }
    temporary = ARCHIVE_METADATA.with_name(
        f".{ARCHIVE_METADATA.name}.{os.getpid()}.tmp")
    temporary.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, ARCHIVE_METADATA)
    return metadata


def main():
    parser = argparse.ArgumentParser(
        description="Create the pinned Cohavora Qt/Libraries ZIP archive")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    source_root = args.source.resolve()
    output = args.output.resolve()
    if not source_root.is_dir():
        raise ValueError(f"Libraries source does not exist: {source_root}")
    ensure_output_outside_source(source_root, output)

    required = required_files()
    missing = [relative for relative in required
               if not (source_root / relative).is_file()]
    if missing:
        details = "\n".join(f"  - {source_root / item}" for item in missing)
        raise ValueError(f"Required dependency artifacts are missing:\n{details}")

    files = source_files(source_root)
    print(f"[PACKAGE] Source: {source_root}", flush=True)
    print(f"[PACKAGE] Output: {output}", flush=True)
    print(f"[PACKAGE] Files:  {len(files)}", flush=True)
    write_archive(source_root, output, files)
    verify_archive(output)
    if not SOURCE_BUILD_GUIDE.is_file():
        raise ValueError(
            f"Source build guide does not exist: {SOURCE_BUILD_GUIDE}")
    source_build_guide = output.with_name(SOURCE_BUILD_GUIDE_ASSET_NAME)
    shutil.copyfile(SOURCE_BUILD_GUIDE, source_build_guide)
    metadata = write_metadata(
        output, source_root, files, required, source_build_guide)
    print(f"[PASS] SHA-256: {metadata['sha256']}")
    print(f"[PASS] Size: {metadata['size']} bytes")
    print(f"[PASS] Source build guide: {source_build_guide}")
    print(f"[PASS] Metadata: {ARCHIVE_METADATA}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile) as exception:
        print(f"[ERROR] {exception}", file=sys.stderr)
        raise SystemExit(1)
