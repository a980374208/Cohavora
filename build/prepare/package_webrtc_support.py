"""Build the reproducible WebRTC compile-support archive."""

import argparse
import hashlib
import json
import subprocess
import sys
import zipfile
from pathlib import Path


PACKAGE_ID = "webrtc-51ef663-cohavora-bssl-dual-v3-build-support"
SDK_PACKAGE_ID = "webrtc-51ef663-cohavora-bssl-dual-v3"
ARCHIVE_ROOT = "cohavora-webrtc-build-support-aaeeee8-cohavora-bssl-dual-v3"
FIXED_ZIP_TIME = (2026, 9, 26, 0, 0, 0)
UPSTREAM_PATCHES = {
    "0001-fix-audio-copy-red-iterator-underflow.patch",
}
REVISIONS = {
    "webrtc": "aaeeee8077eb0a4cad1c9494e9c6433c751ef663",
    "build": "be1a8f6dcd7df7e46320192c5e2f364e50d79bbf",
    "third_party": "a3b630c291f1cf3b711687b1c4be322d3015512e",
    "boringssl": "b94d71f87ff943a617d77f3ff029f9a01a1ec6bc",
}


def sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def git(repository: Path, *arguments: str) -> bytes:
    return subprocess.check_output(
        ["git", "-C", str(repository), *arguments],
        stderr=subprocess.STDOUT,
    )


def checked_revision(repository: Path, expected: str) -> str:
    actual = git(repository, "rev-parse", "HEAD").decode().strip()
    if actual != expected:
        raise ValueError(
            f"Unexpected revision for {repository}: expected {expected}, got {actual}")
    return actual


def zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(f"{ARCHIVE_ROOT}/{name}", FIXED_ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    info.create_system = 3
    return info


def build(args):
    source = args.source_root.resolve()
    patches = args.patches_dir.resolve()
    upstream_patches = args.upstream_patches_dir.resolve()
    prefix_kit = args.prefix_kit.resolve()
    project = args.project_root.resolve()
    package_metadata = args.package_metadata.resolve()
    output = args.output.resolve()

    repositories = {
        "webrtc": source,
        "build": source / "build",
        "third_party": source / "third_party",
        "boringssl": source / "third_party/boringssl/src",
    }
    observed_revisions = {
        name: checked_revision(repositories[name], revision)
        for name, revision in REVISIONS.items()
    }
    package_data = json.loads(package_metadata.read_text(encoding="utf-8"))
    if package_data.get("package_id") != SDK_PACKAGE_ID:
        raise ValueError(
            f"Unexpected SDK metadata package_id: expected {SDK_PACKAGE_ID}")

    entries = {}

    def add_file(name: str, path: Path):
        if not path.is_file():
            raise ValueError(f"Missing support input: {path}")
        entries[name] = path.read_bytes()

    def add_bytes(name: str, content: bytes):
        entries[name] = content

    documentation = project / "docs/dependencies/webrtc/BUILDING_PREFIXED_WEBRTC.md"
    add_file("README.md", documentation)
    add_file("docs/BUILDING_PREFIXED_WEBRTC.md", documentation)
    for name in (
        "BoringSSL-CRT符号-人工修复步骤.md",
        "WebRTC-Release-前缀重编步骤.md",
    ):
        add_file(f"docs/{name}", prefix_kit / name)

    for patch in sorted(patches.glob("*.patch")):
        add_file(f"patches/livekit/{patch.name}", patch)
    selected_upstream_patches = [
        patch for patch in sorted(upstream_patches.glob("*.patch"))
        if patch.name in UPSTREAM_PATCHES
    ]
    if {patch.name for patch in selected_upstream_patches} != UPSTREAM_PATCHES:
        raise ValueError("The AudioEncoderCopyRed upstream backport is missing")
    for patch in selected_upstream_patches:
        add_file(f"patches/upstream/{patch.name}", patch)

    add_bytes("patches/checkout/webrtc-root.patch", git(source, "diff", "--binary"))
    add_bytes(
        "patches/checkout/chromium-build.patch",
        git(source / "build", "diff", "--binary"),
    )
    add_bytes(
        "patches/checkout/chromium-third-party-boringssl.patch",
        git(source / "third_party", "diff", "--binary", "--", "boringssl/BUILD.gn"),
    )

    for path in sorted(prefix_kit.iterdir()):
        if path.is_file():
            add_file(f"prefix-kit/{path.name}", path)

    add_file("configs/Release/args.gn", source / "out/ReleasePrefixed/args.gn")
    add_file("configs/Debug/args.gn", source / "out/DebugPrefixed/args.gn")
    add_file("packaging/package_webrtc.py", project / "build/prepare/package_webrtc.py")
    add_file("packaging/webrtc-package.json", package_metadata)
    add_file("packaging/package_webrtc_support.py", Path(__file__).resolve())

    content_hashes = {name: sha256(content) for name, content in sorted(entries.items())}
    manifest = {
        "schema_version": 1,
        "package_id": PACKAGE_ID,
        "source_checkout_included": False,
        "source_checkout_note": (
            "WebRTC checkout source is intentionally excluded; use README.md "
            "and the pinned revisions to obtain it."
        ),
        "revisions": observed_revisions,
        "boringssl_prefix": "cohavora_bssl",
        "upstream_backports": [
            "765f70d55483e2a5bafd11cbf9d8deb5f1a99b19",
        ],
        "configurations": {
            "Release": {"runtime_library": "MT", "iterator_debug_level": 0},
            "Debug": {"runtime_library": "MTd", "iterator_debug_level": 2},
        },
        "files": content_hashes,
    }
    add_bytes(
        "metadata/package.json",
        (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )
    checksums = "".join(
        f"{sha256(content)}  {name}\n" for name, content in sorted(entries.items())
    ).encode("utf-8")
    add_bytes("metadata/checksums.sha256", checksums)

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp")
    temporary.unlink(missing_ok=True)
    try:
        with zipfile.ZipFile(
            temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            for name, content in sorted(entries.items()):
                archive.writestr(zip_info(name), content)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)

    print(json.dumps({
        "archive": str(output),
        "file_count": len(entries),
        "sha256": sha256(output.read_bytes()),
        "size": output.stat().st_size,
    }, indent=2))


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--patches-dir", type=Path, required=True)
    parser.add_argument("--upstream-patches-dir", type=Path, required=True)
    parser.add_argument("--prefix-kit", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--package-metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


if __name__ == "__main__":
    try:
        build(parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError) as exception:
        print(f"[ERROR] {exception}", file=sys.stderr)
        sys.exit(1)
