"""Build the reproducible WebRTC compile-support archive."""

import argparse
import hashlib
import json
import re
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
        # Git warnings are diagnostic text, never patch or status payload.
        stderr=subprocess.PIPE,
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
    if args.v4:
        return build_v4(args)
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

    add_bytes("README.md", (
        "# Cohavora WebRTC compile support\n\n"
        "This archive supplements the matching WebRTC SDK package; it does "
        "not include a WebRTC source checkout. Obtain the revisions listed "
        "in metadata/package.json. The patches directory contains the SDK "
        "changes, configs contains the GN arguments, prefix-kit contains "
        "the BoringSSL symbol-prefix helpers, and packaging contains the "
        "SDK archive scripts and package metadata.\n\n"
        "Use the matching MSVC architecture and runtime ABI recorded in the "
        "manifest. Verify archive entries against metadata/checksums.sha256 "
        "before using them. Preserve the source licenses and notices.\n"
    ).encode("utf-8"))

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


def gn_strings(path):
    values = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        match = re.fullmatch(r'\s*(\w+)\s*=\s*"([^"]*)"\s*', line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def portable_gn_args(path, configuration):
    replacements = {
        "boringssl_prefix_headers": "//out/cohavora-v4-prefix/" + configuration,
        "rtc_nvenc_include_dir": "@SUPPORT_ROOT@/headers/nvenc",
        "rtc_amf_include_dir": "@SUPPORT_ROOT@/headers",
        "rtc_qsv_include_dir": "@DEPENDENCIES_ROOT@/onevpl-src/api",
        "rtc_qsv_vpl_library": "@DEPENDENCIES_ROOT@/onevpl-" + configuration + "-build/" +
            configuration + ("/vpld.lib" if configuration == "Debug" else "/vpl.lib"),
        "rtc_x265_include_dir": "@DEPENDENCIES_ROOT@/x265-" + configuration + "-install/include",
        "rtc_x265_library": "@DEPENDENCIES_ROOT@/x265-" + configuration + "-install/lib/x265-static.lib",
        "rtc_x265_license_file": "@DEPENDENCIES_ROOT@/x265-src/COPYING",
    }
    lines = ["# Portable v4 SDK configuration; build_webrtc.py expands the path tokens."]
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        match = re.match(r"\s*(\w+)\s*=", line)
        key = match.group(1) if match else None
        if key == "rtc_amf_test_ffmpeg_include_dir":
            continue
        if key in replacements:
            lines.append(key + " = " + json.dumps(replacements[key]))
        elif key and key.endswith("_build_tests"):
            lines.append(key + " = false")
        else:
            lines.append(line)
    content = "\n".join(lines) + "\n"
    if re.search(r"[A-Za-z]:[/\\]", content):
        raise ValueError("A nonportable machine path remains in " + str(path))
    if set(replacements) - set(gn_strings(path)):
        raise ValueError("Missing codec GN configuration inputs: " + str(path))
    return content.encode("utf-8")


def build_v4(args):
    source = args.source_root.resolve()
    sdk = args.sdk_root.resolve()
    project = args.project_root.resolve() if args.project_root else Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    prefix_kit = args.prefix_kit.resolve()
    sdk_data = json.loads(args.sdk_metadata.read_text(encoding="utf-8-sig"))
    revision = git(source, "rev-parse", "HEAD").decode().strip()
    if git(source, "diff", "--name-only") or git(source, "diff", "--cached", "--name-only"):
        raise ValueError("The fork checkout must have no tracked root changes")
    revisions = {
        "webrtc": revision,
        "build": git(source / "build", "rev-parse", "HEAD").decode().strip(),
        "third_party": git(source / "third_party", "rev-parse", "HEAD").decode().strip(),
        "boringssl": git(source / "third_party/boringssl/src", "rev-parse", "HEAD").decode().strip(),
    }
    sdk_source = sdk_data.get("source", {})
    sdk_revision = sdk_source.get("webrtc", sdk_source.get("revision"))
    if sdk_revision and sdk_revision != revision:
        raise ValueError("SDK metadata does not bind the current fork revision")
    if set(sdk_data.get("configurations", {})) != {"Debug", "Release"}:
        raise ValueError("The SDK metadata must contain both configurations")
    source_spec = {
        "repository": "https://github.com/a980374208/webrtc_hardEncoder.git",
        "branch": "m_144_hard_encoder", "revision": revision,
        "deps_sha256": sha256((source / "DEPS").read_bytes()),
    }
    branch = git(source, "branch", "--show-current").decode().strip()
    if branch != source_spec["branch"]:
        raise ValueError("Select the requested m_144_hard_encoder source branch")
    config_paths = {name: sdk / "metadata/build" / name / "args.gn" for name in ("Debug", "Release")}
    # The prebuilt SDK may intentionally omit private build ledgers. Its public
    # portable config can still be derived directly from the frozen build dir.
    config_paths = {name: path if path.is_file() else source / "out" /
                    ("EncoderDebug" if name == "Debug" else "ReleasePrefixed") / "args.gn"
                    for name, path in config_paths.items()}
    values = gn_strings(config_paths["Release"])
    amf_include = Path(values["rtc_amf_include_dir"])
    nvenc_include = Path(values["rtc_nvenc_include_dir"])
    x265_source = Path(values["rtc_x265_license_file"]).parent
    vpl_source = Path(values["rtc_qsv_include_dir"]).parent
    dependencies = {
        "x265": {"url": "https://bitbucket.org/multicoreware/x265_git.git",
                 "revision": git(x265_source, "rev-parse", "HEAD").decode().strip()},
        "onevpl": {"url": "https://github.com/intel/libvpl.git",
                   "revision": git(vpl_source, "rev-parse", "HEAD").decode().strip()},
        "amf_headers": {"source_url": "https://github.com/GPUOpen-LibrariesAndSDKs/AMF",
                        "version": "1.4.36", "delivery": "bundled public headers; per-file hashes below"},
        "nvenc_headers": {"source_url": "https://github.com/FFmpeg/nv-codec-headers",
                          "delivery": "bundled nvEncodeAPI.h; per-file hash below"},
    }
    entries = {}

    def add_file(relative, path):
        if not path.is_file():
            raise ValueError("Missing public support input: " + str(path))
        entries[relative] = path.read_bytes()

    add_file("build_webrtc.py", project / "build/prepare/build_webrtc.py")
    add_file("packaging/package_webrtc.py", project / "build/prepare/package_webrtc.py")
    add_file("packaging/package_webrtc_support.py", Path(__file__).resolve())
    add_file("packaging/webrtc-package.json", args.sdk_metadata)
    # Use the SDK's reviewed ABI/profile values; no private provenance or logs.
    profile = {key: sdk_data[key] for key in
               ("schema_version", "package_id", "boringssl_prefix", "configurations")}
    profile["compile_definitions"] = sdk_data.get("compile_definitions", ["RTC_ENABLE_H265"])
    entries["packaging/webrtc-profile.json"] = (json.dumps(profile, indent=2, sort_keys=True) + "\n").encode("utf-8")
    for config in ("Debug", "Release"):
        entries["configs/" + config + "/args.gn.in"] = portable_gn_args(config_paths[config], config)
        for name in ("boringssl_prefix_symbols.h", "boringssl_prefix_symbols_asm.h",
                     "boringssl_prefix_symbols_nasm.inc"):
            add_file("prefix-headers/" + config + "/" + name, sdk / "include/boringssl-prefix" / config / name)
    for relative, repository, tracked_path in (
            ("patches/chromium-build.patch", source / "build", "config/compiler/BUILD.gn"),
            ("patches/chromium-third-party-boringssl.patch", source / "third_party", "boringssl/BUILD.gn")):
        content = git(repository, "diff", "--binary", "--", tracked_path)
        if not content:
            raise ValueError("Missing matching nested checkout patch: " + relative)
        entries[relative] = content
    for name in ("boringssl_prefix_symbols.py", "read_symbols_pe_compat.go"):
        add_file("prefix-kit/" + name, prefix_kit / name)
    for path in sorted((amf_include / "AMF").rglob("*")):
        if path.is_file() and path.suffix.lower() in (".h", ".hpp", ".inl"):
            add_file("headers/AMF/" + path.relative_to(amf_include / "AMF").as_posix(), path)
    add_file("headers/nvenc/nvEncodeAPI.h", nvenc_include / "nvEncodeAPI.h")
    add_file("licenses/amf/LICENSE.txt", amf_include.parent / "licenses/amf/LICENSE.txt")
    add_file("licenses/nv-codec-headers/LICENSE", nvenc_include.parent.parent / "licenses/nv-codec-headers/LICENSE")
    add_file("licenses/x265/COPYING", x265_source / "COPYING")
    add_file("licenses/onevpl/LICENSE", vpl_source / "LICENSE")
    add_file("licenses/webrtc/LICENSE", source / "LICENSE")
    add_file("licenses/webrtc/PATENTS", source / "PATENTS")
    readme = """# Cohavora WebRTC Windows x64 构建支持 v4

源码直接使用 https://github.com/a980374208/webrtc_hardEncoder 的
`m_144_hard_encoder` 分支。本包不含 WebRTC 源码；不再从旧 aaeeee8 checkout
叠加旧 LiveKit/root 补丁。具体发布提交及嵌套依赖 revision 见 metadata/package.json。

## 环境

Windows x64、Python 3.8+、Git、Chromium depot_tools、Visual Studio 2022 C++
工具链及 Windows SDK 10.0.26100、NASM、CMake 3.31.x。已发包使用 MSVC 14.44；
不要混用 CRT、STL 或 Debug/Release ABI。x265 4.1 的旧 CMake policy 不兼容 CMake 4。
先按 Chromium Windows 构建要求准备本机工具链与 depot_tools。所有路径由参数传入。

## 从指定分支获取最新源码并编译

在本包根目录执行（路径请替换为本机安装位置；workspace 必须是新的目录）：

```powershell
python build_webrtc.py --workspace D:/work/cohavora-webrtc --depot-tools D:/tools/depot_tools --cmake D:/tools/cmake-3.31/bin/cmake.exe --nasm D:/tools/nasm/nasm.exe
python build_webrtc.py --workspace D:/work/cohavora-webrtc --depot-tools D:/tools/depot_tools --cmake D:/tools/cmake-3.31/bin/cmake.exe --nasm D:/tools/nasm/nasm.exe --execute
# 固定本次发布提交复现，使用另一个新工作目录：
python build_webrtc.py --workspace D:/work/cohavora-webrtc-release --depot-tools D:/tools/depot_tools --cmake D:/tools/cmake-3.31/bin/cmake.exe --nasm D:/tools/nasm/nasm.exe --revision release --execute
```

首条仅校验包并打印计划。第二条执行 fork clone、gclient sync/runhooks、嵌套补丁、
x265/oneVPL 的双配置静态 CRT 构建、GN/autoninja 和 SDK 组装。
默认 `--revision branch` 获取分支最新版本；复现本次源码使用 `--revision release`，
也可传完整 commit。分支以后更新了 DEPS/BoringSSL 时，入口会拒绝套用旧嵌套补丁和
prefix 头，需更新对应支持包。不会 reset 或覆盖既有 checkout。

默认产物为 workspace/sdk，含 include、lib/Debug、lib/Release、licenses 和 ABI profile；
也可用 --sdk-output 指定新目录。Debug `/MTd`、iterator level 2；Release `/MT`、level 0。
消费者须定义 `RTC_ENABLE_H265`，并链接对应配置的 webrtc.lib、x265-static.lib 与 vpl[d].lib。
这些库的消费端还须沿用对应 WebRTC 平台定义、系统库和 generated/prefix include 目录。
AMF/NVENC 只随包提供公共 SDK 头和授权声明，不包含 OBS/FFmpeg 运行时。

## 配置与校验

configs/*/args.gn.in 是去除本机路径后的模板，build_webrtc.py 自动展开。
SDK 构建不启用单测/硬件 smoke 目标；这些验证应按运行设备另行执行。
prefix-headers 是对应已固定 BoringSSL revision 的符号前缀头；prefix-kit 的符号工具
供更新 BoringSSL 后重新生成和验证使用，不沿用 v3 的验证结果。
metadata/checksums.sha256 覆盖本包内容（校验文件自身除外），入口还逐项校验 manifest 哈希。
packaging 包含 SDK 打包入口、匹配下载 metadata 和 ABI profile。

本次发布复用已编译的双配置产物；从全新 checkout 执行本包完整重建为 NOT_RUN。
离线计划/归档校验不等于设备、真实会议、长稳或 Production Ready。
请保留各源码仓库和本包 licenses 中的版权及授权声明。
"""
    entries["README.md"] = readme.encode("utf-8")
    hashes = {name: sha256(content) for name, content in sorted(entries.items())}
    manifest = {
        "schema_version": 1, "support_format": "fork-build-v4",
        "package_id": sdk_data["package_id"] + "-build-support",
        "sdk_package_id": sdk_data["package_id"], "source_checkout_included": False,
        "source": source_spec, "revisions": revisions, "dependencies": dependencies,
        "boringssl_prefix": "cohavora_bssl", "configurations": profile["configurations"],
        "toolchain": {"visual_studio": "2022", "msvc": "14.44", "windows_sdk": "10.0.26100.0",
                      "cmake_recommended": "3.31.x", "python_minimum": "3.8"},
        "fresh_checkout_rebuild": "NOT_RUN", "files": hashes,
    }
    entries["metadata/package.json"] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    entries["metadata/checksums.sha256"] = "".join(
        sha256(content) + "  " + name + "\n" for name, content in sorted(entries.items())).encode("ascii")
    archive_root = "cohavora-webrtc-build-support-m144-cohavora-bssl-dual-v4"
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise ValueError("Use a new support archive output path")
    temporary = output.with_name("." + output.name + ".tmp")
    if temporary.exists():
        raise ValueError("Temporary support archive already exists")
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, content in sorted(entries.items()):
                info = zipfile.ZipInfo(archive_root + "/" + name, (2026, 11, 13, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                info.create_system = 3
                archive.writestr(info, content)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    result = {"archive": str(output), "archive_root": archive_root, "file_count": len(entries),
              "sha256": sha256(output.read_bytes()), "size": output.stat().st_size,
              "source_revision": revision, "sdk_package_id": sdk_data["package_id"]}
    print(json.dumps(result, indent=2))
    return result


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--v4", action="store_true", help="fork-backed M144 build support")
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--patches-dir", type=Path)
    parser.add_argument("--upstream-patches-dir", type=Path)
    parser.add_argument("--prefix-kit", type=Path, required=True)
    parser.add_argument("--project-root", type=Path)
    parser.add_argument("--package-metadata", type=Path)
    parser.add_argument("--sdk-root", type=Path)
    parser.add_argument("--sdk-metadata", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    required = ("sdk_root", "sdk_metadata") if args.v4 else (
        "patches_dir", "upstream_patches_dir", "project_root", "package_metadata")
    for name in required:
        if getattr(args, name) is None:
            parser.error("missing --" + name.replace("_", "-"))
    return args


if __name__ == "__main__":
    try:
        build(parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError) as exception:
        print(f"[ERROR] {exception}", file=sys.stderr)
        sys.exit(1)
