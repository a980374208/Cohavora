#!/usr/bin/env python3
"""Isolate the two data symbols missed by Chromium's JPEG function mangler.

The SDK stays untouched. Fail closed if a different Qt/WebRTC pair has any
additional symbol collisions; their JPEG ABIs are not interchangeable.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid


RENAMES = {
    "jpeg_natural_order": "cohavora_webrtc_jpeg_natural_order",
    "jpeg_nbits_table": "cohavora_webrtc_jpeg_nbits_table",
}


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def symbols(nm: str, archive: Path) -> Counter:
    result = subprocess.run([nm, "--format=posix", "--extern-only", str(archive)],
                            capture_output=True, text=True, check=True)
    table: Counter = Counter()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and len(fields[1]) == 1:
            # Retain type and size/address as well as multiplicity, so the
            # post-transform audit detects changes beyond the two names.
            table[tuple(fields)] += 1
    if not table:
        raise RuntimeError("No archive symbols found")
    return table


def definitions(table: Counter) -> set[str]:
    return {entry[0] for entry in table if entry[1] not in {"U", "w", "v"}}


def publish_pair(archive: Path, manifest: Path, output: Path) -> None:
    """Publish the completion manifest last, restoring old files on failure.

    Two filesystem renames are not an atomic pair. Callers must only consume
    this result after the command succeeds, using the manifest's output hash.
    """
    targets = ((archive, output), (manifest, output.with_suffix(".json")))
    backups = []
    published = []
    try:
        for _, target in targets:
            if target.exists():
                backup = output.parent / f".{target.name}.backup-{uuid.uuid4().hex}"
                target.replace(backup)
                backups.append((backup, target))
        for staged, target in targets:
            staged.replace(target)
            published.append(target)
    except Exception as exception:
        failures = []
        saved_targets = {target for _, target in backups}
        for target in reversed(published):
            if target not in saved_targets:
                try:
                    target.unlink(missing_ok=True)
                except OSError as exc:
                    failures.append(str(exc))
        for backup, target in reversed(backups):
            try:
                backup.replace(target)
            except OSError as exc:
                failures.append(f"Restore {backup} to {target}: {exc}")
        if failures:
            raise RuntimeError("JPEG output rollback needs manual recovery; "
                               + "; ".join(failures)) from exception
        raise
    else:
        for backup, _ in backups:
            backup.unlink()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qt-jpeg", type=Path, action="append", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--objcopy", required=True)
    args = parser.parse_args()
    source = args.archive.resolve(strict=True)
    output = args.output.resolve()
    if source in (output, output.with_suffix(".json")):
        raise RuntimeError("The SDK archive must never be overwritten")
    source_hash = digest(source)
    original = symbols(args.nm, source)
    qt_defined: set[str] = set()
    qt_hashes = {}
    for archive in args.qt_jpeg:
        qt_defined |= definitions(symbols(args.nm, archive))
        qt_hashes[archive.name] = digest(archive)
    collisions = definitions(original) & qt_defined
    if collisions != set(RENAMES):
        raise RuntimeError("Qt/WebRTC JPEG symbol collision set changed; ABI review required: "
                           + ", ".join(sorted(collisions)))
    if not {"chromium_jpeg_CreateDecompress", "chromium_jpeg_std_error"} <= definitions(original):
        raise RuntimeError("The SDK must already provide Chromium-mangled JPEG functions")
    if set(RENAMES.values()) & {entry[0] for entry in original}:
        raise RuntimeError("The requested JPEG namespace already exists")
    for name in RENAMES:
        count = sum(count for entry, count in original.items()
                    if entry[0] == name and entry[1] not in {"U", "w", "v"})
        if count != 1:
            raise RuntimeError("Expected exactly one definition of " + name)
    expected = Counter({(RENAMES.get(entry[0], entry[0]), *entry[1:]): count
                        for entry, count in original.items()})
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="jpeg-isolation-", dir=output.parent) as work:
        temporary = Path(work) / output.name
        command = [args.objcopy, "--enable-deterministic-archives"]
        for old, new in RENAMES.items():
            command += ["--redefine-sym", old + "=" + new]
        subprocess.run(command + [str(source), str(temporary)], check=True)
        actual = symbols(args.nm, temporary)
        if actual != expected or definitions(actual) & qt_defined:
            raise RuntimeError("JPEG isolation changed unexpected symbols or left collisions")
        if digest(source) != source_hash:
            raise RuntimeError("SDK archive changed during isolation")
        manifest = {
            "schema": 1,
            "purpose": "Linux static Qt/WebRTC JPEG data-symbol isolation",
            "source_sha256": source_hash,
            "output_sha256": digest(temporary),
            "qt_archive_sha256": qt_hashes,
            "renames": RENAMES,
            "other_global_symbols_unchanged": True,
            "remaining_qt_collisions": [],
        }
        manifest_path = Path(work) / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        publish_pair(temporary, manifest_path, output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exception:
        print(f"[ERROR] JPEG isolation failed: {exception}", file=sys.stderr)
        if isinstance(exception, subprocess.CalledProcessError) and exception.stderr:
            print(exception.stderr.strip(), file=sys.stderr)
        raise SystemExit(1) from exception
