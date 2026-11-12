#!/usr/bin/env python3
"""Build local Ubuntu packages with source identity and dependency notices.

This production packaging entry does not install, sign, or publish anything.
Machine-specific Qt source locations are explicit command-line inputs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
from datetime import datetime, timezone

SOURCE_PATHS = [
    ".gitignore", "CMakeLists.txt", "CMakePresets.json", "LICENSE", "README.md",
    "configure.bat", "configure.sh", "build", "cmake", "src", "packaging",
    "samples/CMakeLists.txt", "samples/simple_room.cpp",
]


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def git(root, *args):
    return run(["git", "-C", str(root), *args], stdout=subprocess.PIPE).stdout


def source_identity(root):
    entries = []
    repositories = []

    def collect(repository, prefix=""):
        repositories.append({"path": prefix or ".", "head":
            git(repository, "rev-parse", "HEAD").decode().strip()})
        gitlinks = set()
        pathspec = ["--", *SOURCE_PATHS] if not prefix else []
        for record in git(repository, "ls-files", "--stage", "-z", *pathspec).split(b"\0"):
            if record.startswith(b"160000 "):
                gitlinks.add(os.fsdecode(record.split(b"\t", 1)[1]))
        names = set(git(repository, "ls-files", "--cached", "--others",
                        "--exclude-standard", "-z", *pathspec).split(b"\0")) - {b""}
        for raw in sorted(names):
            name = os.fsdecode(raw)
            path = repository / name
            relative = prefix + name
            if name in gitlinks:
                if (path / ".git").exists():
                    collect(path, relative + "/")
                else:
                    entries.append({"path": relative, "kind": "uninitialized-submodule"})
            elif path.is_symlink():
                entries.append({"path": relative, "kind": "symlink", "target": os.readlink(path)})
            elif path.is_file():
                entries.append({"path": relative, "kind": "file", "sha256": digest(path),
                                "executable": bool(path.stat().st_mode & 0o111)})
            else:
                entries.append({"path": relative, "kind": "missing"})
    collect(root)
    canonical = json.dumps(entries, ensure_ascii=True, sort_keys=True,
                           separators=(",", ":")).encode()
    return {"tree_sha256": hashlib.sha256(canonical).hexdigest(),
            "repositories": repositories, "files": entries,
            "scope": "Build/source/packaging inputs, recursive initialized source submodules",
            "root_pathspecs": SOURCE_PATHS,
            "excluded": ["out", "deps", ".codex", "samples/acceptance", "private configuration"]}


def collect_qt_notices(roots, destination):
    records = []
    for index, root in enumerate(roots):
        if not root.is_dir():
            raise RuntimeError("Qt source root is unavailable")
        for path in sorted(root.rglob("*")):
            relative = path.relative_to(root)
            if any(part in {".git", "tests", "examples", "doc"} for part in relative.parts):
                continue
            if path.is_symlink() or not path.is_file():
                continue
            name = path.name.lower()
            if not (name.startswith(("license", "copying", "copyright"))
                    or name == "qt_attribution.json"):
                continue
            target = destination / ("source-" + str(index)) / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            records.append({"path": target.relative_to(destination).as_posix(), "sha256": digest(target)})
    if not records:
        raise RuntimeError("No Qt license notices found; provide the matching Qt source roots")
    (destination / "notice-inventory.json").write_text(
        json.dumps({"schema": 1, "files": records}, indent=2) + "\n")


def linked_input_identity(build):
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    roots = [("build", build)]
    for key, label in [("QT_ROOT", "qt"), ("WEBRTC_ROOT", "webrtc"),
                       ("VCPKG_INSTALLED_DIR", "vcpkg")]:
        if cache.get(key):
            roots.append((label, Path(cache[key]).resolve()))
    lines = run(["ninja", "-C", str(build), "-t", "commands", "cohavora_app"],
                stdout=subprocess.PIPE, text=True).stdout.splitlines()
    tokens = shlex.split(lines[-1])
    inputs = {}
    for token in tokens:
        if not token.endswith((".a", ".o", ".so")):
            continue
        path = Path(token)
        path = (path if path.is_absolute() else build / path).resolve()
        if not path.is_file():
            continue
        relative = None
        for label, base in roots:
            if path.is_relative_to(base):
                relative = label + "/" + path.relative_to(base).as_posix()
                break
        # Keep system-library identity without embedding the developer's paths.
        if relative is None:
            relative = "system/" + path.name
        inputs[relative] = {"path": relative, "sha256": digest(path), "bytes": path.stat().st_size}
    if not inputs:
        raise RuntimeError("No link inputs found in the configured Ninja product target")
    return sorted(inputs.values(), key=lambda entry: entry["path"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--qt-source-root", action="append", type=Path, default=[])
    parser.add_argument("--parallel", type=int, default=2)
    parser.add_argument("--generator", choices=["DEB", "TGZ", "all"], default="all")
    parser.add_argument("--output-directory", type=Path,
                        help="New, empty directory for review candidates; preserves existing packages")
    parser.add_argument("--verify-inputs", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    if args.parallel < 1 or not (build / "CMakeCache.txt").is_file():
        parser.error("Use an existing configured Linux build and a positive parallel count")
    metadata = build / "packaging/linux"
    if args.verify_inputs:
        recorded = json.loads((metadata / "source-provenance.json").read_text())
        if (recorded["source"] != source_identity(root)
                or recorded["cmake_cache_sha256"] != digest(build / "CMakeCache.txt")
                or recorded["link_inputs"] != linked_input_identity(build)
                or recorded["preinstall_binary_sha256"] != digest(build / "src/app/Cohavora")):
            raise RuntimeError("Source/build inputs no longer match the prepared package identity")
        return
    output = args.output_directory.resolve() if args.output_directory else build / "packages"
    if args.output_directory and (output.exists() and (not output.is_dir() or any(output.iterdir()))):
        parser.error("--output-directory must be a new or empty directory")
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    before = source_identity(root)
    run(["cmake", "--build", str(build), "--target", "cohavora_app", "--parallel", str(args.parallel)])
    after = source_identity(root)
    if before != after:
        raise RuntimeError("Source inputs changed during the build; rebuild from stable inputs")
    qt_notices = metadata / "third-party/qt"
    # Retain notices already provided by a matching SDK; source roots supplement them.
    if args.qt_source_root:
        if qt_notices.exists():
            shutil.rmtree(qt_notices)
        collect_qt_notices([path.resolve() for path in args.qt_source_root], qt_notices)
    metadata.mkdir(parents=True, exist_ok=True)
    provenance = {"schema": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
                  "source": after, "preinstall_binary_sha256": digest(build / "src/app/Cohavora"),
                  "cmake_cache_sha256": digest(build / "CMakeCache.txt"),
                  "link_inputs": linked_input_identity(build),
                  "binary_scope": "Before install RPATH removal and package debug splitting; inspect final package separately"}
    (metadata / "source-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    for generator in (["DEB", "TGZ"] if args.generator == "all" else [args.generator]):
        run(["cpack", "--config", str(build / "CPackConfig.cmake"), "-G", generator,
             "-D", "CPACK_PACKAGE_DIRECTORY=" + str(output)], cwd=build)
    if source_identity(root) != after:
        raise RuntimeError("Source inputs changed during packaging; packages are not review candidates")
    print("Local packages created in", output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("Packaging failed:", str(error), file=sys.stderr)
        sys.exit(1)
