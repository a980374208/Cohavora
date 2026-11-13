"""Fetch and build the fork-backed Windows x64 Cohavora WebRTC SDK.

Use the copy in a v4 build-support archive. Without --execute this prints a
plan and never clones, configures or builds anything.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def checked_support(root, manifest):
    for relative, expected in manifest["files"].items():
        path = root / relative
        if not path.is_file() or file_hash(path) != expected:
            raise ValueError("Build-support checksum mismatch: " + relative)


def command(argv, cwd, environment):
    print(subprocess.list2cmdline([str(value) for value in argv]), flush=True)
    subprocess.run([str(value) for value in argv], cwd=cwd, env=environment, check=True)


def git_revision(path):
    return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()


def clone_pinned(spec, destination, environment):
    if destination.exists():
        raise ValueError("Use a new dependency directory: " + str(destination))
    command(["git", "clone", spec["url"], destination], destination.parent, environment)
    command(["git", "-C", destination, "checkout", "--detach", spec["revision"]],
            destination.parent, environment)
    if git_revision(destination) != spec["revision"]:
        raise ValueError("Dependency revision mismatch: " + str(destination))


def gn_path(path):
    return str(path).replace("\\", "/").replace("$", "\\$").replace('"', '\\"')


def build_dependencies(args, manifest, source, dependencies, environment):
    for key, folder in (("x265", "x265-src"), ("onevpl", "onevpl-src")):
        clone_pinned(manifest["dependencies"][key], dependencies / folder, environment)
    for config in args.configurations:
        runtime = "MultiThreadedDebug" if config == "Debug" else "MultiThreaded"
        x265_build = dependencies / ("x265-" + config + "-build")
        x265_install = dependencies / ("x265-" + config + "-install")
        command([args.cmake, "-S", dependencies / "x265-src/source", "-B", x265_build,
                 "-G", args.generator, "-A", "x64", "-DSTATIC_LINK_CRT=ON",
                 "-DCMAKE_BUILD_TYPE=" + config,
                 "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
                 "-DCMAKE_MSVC_RUNTIME_LIBRARY=" + runtime,
                 "-DENABLE_SHARED=OFF", "-DENABLE_CLI=OFF", "-DHIGH_BIT_DEPTH=OFF",
                 "-DEXPORT_C_API=ON", "-DENABLE_ASSEMBLY=ON", "-DNASM_EXECUTABLE=" + args.nasm,
                 "-DENABLE_HDR10_PLUS=OFF", "-DENABLE_SVT_HEVC=OFF",
                 "-DENABLE_TESTS=OFF", "-DNATIVE_BUILD=OFF"], source, environment)
        command([args.cmake, "--build", x265_build, "--config", config,
                 "--target", "x265-static", "--parallel", args.jobs], source, environment)
        command([args.cmake, "--install", x265_build, "--config", config,
                 "--prefix", x265_install], source, environment)
        vpl_build = dependencies / ("onevpl-" + config + "-build")
        command([args.cmake, "-S", dependencies / "onevpl-src", "-B", vpl_build,
                 "-G", args.generator, "-A", "x64", "-DBUILD_SHARED_LIBS=OFF",
                 "-DCMAKE_BUILD_TYPE=" + config,
                 "-DUSE_MSVC_STATIC_RUNTIME=ON", "-DBUILD_TESTS=OFF", "-DBUILD_EXAMPLES=OFF",
                 "-DBUILD_EXPERIMENTAL=OFF",
                 "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"], source, environment)
        command([args.cmake, "--build", vpl_build, "--config", config,
                 "--target", "VPL", "--parallel", args.jobs], source, environment)


def assemble_sdk(args, support, source, dependencies, manifest, revision):
    source = source.resolve()
    destination = args.sdk_output.resolve() if args.sdk_output else args.workspace.resolve() / "sdk"
    if destination == source or source in destination.parents or destination in source.parents:
        raise ValueError("SDK output and source checkout must be independent directories")
    if destination.exists():
        raise ValueError("SDK output already exists: " + str(destination))
    destination.mkdir(parents=True)
    # Export source headers from the fetched checkout, including its checked-out
    # third-party dependencies. Never descend into Git databases or build trees.
    for directory, names, files in os.walk(source):
        names[:] = [name for name in names if name not in (".git", "out", ".cipd", "node_modules")]
        for name in files:
            path = Path(directory) / name
            if path.suffix.lower() not in (".h", ".hpp", ".inc", ".inl"):
                continue
            target = destination / "include" / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
    configurations = {}
    for config in args.configurations:
        out = source / "out" / ("cohavora-v4-" + config.lower())
        paths = {
            "webrtc.lib": out / "obj/webrtc.lib",
            "x265-static.lib": dependencies / ("x265-" + config + "-install/lib/x265-static.lib"),
            "vpld.lib" if config == "Debug" else "vpl.lib":
                dependencies / ("onevpl-" + config + "-build") / config /
                ("vpld.lib" if config == "Debug" else "vpl.lib"),
        }
        for name, path in paths.items():
            target = destination / "lib" / config / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
        shutil.copytree(out / "gen", destination / ("include/out-x64-" + config.lower() + "/gen"))
        shutil.copytree(support / "prefix-headers" / config,
                        destination / "include/boringssl-prefix" / config)
        args_target = destination / "metadata/build" / config / "args.gn"
        args_target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out / "args.gn", args_target)
        license_target = destination / "licenses" / config / "LICENSE.md"
        license_target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out / "LICENSE.md", license_target)
        configurations[config] = {
            "library_sha256": file_hash(paths["webrtc.lib"]),
            "runtime_library": "MTd_StaticDebug" if config == "Debug" else "MT_StaticRelease",
            "iterator_debug_level": 2 if config == "Debug" else 0,
            "external_static_libraries": {
                key: {"relative_path": "lib/" + config + "/" + name,
                      "sha256": file_hash(paths[name])}
                for key, name in (("x265", "x265-static.lib"),
                                  ("vpl", "vpld.lib" if config == "Debug" else "vpl.lib"))
            },
        }
    shutil.copytree(support / "licenses", destination / "licenses/build-support")
    for key, filename in (("x265", "COPYING"), ("onevpl", "LICENSE")):
        target = destination / "licenses/external" / key / filename
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(dependencies / ("x265-src" if key == "x265" else "onevpl-src") / filename, target)
    profile = {
        "schema_version": 1, "package_id": "webrtc-" + revision[:10] + "-local-build",
        "boringssl_prefix": "cohavora_bssl", "compile_definitions": ["RTC_ENABLE_H265"],
        "source": {"repository": manifest["source"]["repository"],
                   "branch": manifest["source"]["branch"], "webrtc": revision},
        "configurations": configurations,
        "scope": "Build and SDK assembly only; unit/runtime/hardware validation NOT_RUN",
    }
    write_json(destination / "metadata/package.json", profile)
    return destination


def build(args):
    support = args.support_root.resolve()
    manifest = read_json(support / "metadata/package.json")
    checked_support(support, manifest)
    if manifest.get("support_format") != "fork-build-v4":
        raise ValueError("This entry requires a v4 fork build-support archive")
    workspace = args.workspace.resolve()
    source = workspace / "src"
    sdk_output = args.sdk_output.resolve() if args.sdk_output else workspace / "sdk"
    if sdk_output == source or source in sdk_output.parents or sdk_output in source.parents:
        raise ValueError("SDK output and source checkout must be independent directories")
    dependencies = args.dependencies_root.resolve() if args.dependencies_root else workspace / "encoder-deps"
    requested = manifest["source"]["revision"] if args.revision == "release" else args.revision
    plan = {
        "repository": manifest["source"]["repository"], "branch": manifest["source"]["branch"],
        "revision": requested, "source": str(source), "dependencies": str(dependencies),
        "configurations": args.configurations, "depot_tools": str(args.depot_tools.resolve()),
        "steps": ["clone fork", "gclient sync + runhooks", "verify nested revisions + apply prefix patches",
                  "build pinned x265/oneVPL with matching CRT", "generate GN arguments",
                  "build webrtc libraries + generate notices", "assemble SDK"],
        "execute": args.execute,
    }
    print(json.dumps(plan, indent=2), flush=True)
    if not args.execute:
        return plan
    if os.name != "nt":
        raise ValueError("This build-support version targets Windows x64 only")
    if source.exists() or (workspace / ".gclient").exists() or dependencies.exists():
        raise ValueError("Use a new workspace and dependency directory; existing checkouts are never reset")
    if args.sdk_output and args.sdk_output.resolve().exists():
        raise ValueError("Use a new --sdk-output directory")
    depot = args.depot_tools.resolve()
    for tool in ("gclient.py", "gn.py", "autoninja.py"):
        if not (depot / tool).is_file():
            raise ValueError("Missing depot_tools entry: " + tool)
    environment = os.environ.copy()
    environment["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
    environment["PATH"] = str(depot) + os.pathsep + environment.get("PATH", "")
    workspace.mkdir(parents=True, exist_ok=True)
    dependencies.mkdir(parents=True)
    command(["git", "clone", "--branch", manifest["source"]["branch"],
             manifest["source"]["repository"], source], workspace, environment)
    if requested != "branch":
        command(["git", "-C", source, "checkout", "--detach", requested], workspace, environment)
    revision = git_revision(source)
    solution = {"name": "src", "url": manifest["source"]["repository"],
                "managed": False, "custom_deps": {}, "custom_vars": {}}
    (workspace / ".gclient").write_text("solutions = " + repr([solution]) + "\n", encoding="utf-8")
    command([sys.executable, depot / "gclient.py", "sync", "--nohooks", "--revision", "src@" + revision],
            workspace, environment)
    for key, relative in (("build", "build"), ("third_party", "third_party"),
                          ("boringssl", "third_party/boringssl/src")):
        if git_revision(source / relative) != manifest["revisions"][key]:
            raise ValueError("Nested " + key + " revision changed; regenerate support patches/prefix headers")
    for relative, patch_name in (("build", "chromium-build.patch"),
                                  ("third_party", "chromium-third-party-boringssl.patch")):
        patch = support / "patches" / patch_name
        command(["git", "-C", source / relative, "apply", "--check", patch], source, environment)
        command(["git", "-C", source / relative, "apply", patch], source, environment)
    command([sys.executable, depot / "gclient.py", "runhooks"], workspace, environment)
    build_dependencies(args, manifest, source, dependencies, environment)
    for config in args.configurations:
        prefix = source / "out/cohavora-v4-prefix" / config
        shutil.copytree(support / "prefix-headers" / config, prefix)
        out = source / "out" / ("cohavora-v4-" + config.lower())
        out.mkdir(parents=True)
        template = (support / "configs" / config / "args.gn.in").read_text(encoding="utf-8")
        template = template.replace("@SUPPORT_ROOT@", gn_path(support)).replace(
            "@DEPENDENCIES_ROOT@", gn_path(dependencies))
        (out / "args.gn").write_text(template, encoding="utf-8")
        command([sys.executable, depot / "gn.py", "gen", out], source, environment)
        command([sys.executable, depot / "autoninja.py", "-C", out, "webrtc", "-j", args.jobs],
                source, environment)
        command([sys.executable, source / "tools_webrtc/libs/generate_licenses.py",
                 "--target", "//:webrtc", out, out], source, environment)
    sdk = assemble_sdk(args, support, source, dependencies, manifest, revision)
    result = {"status": "PASS", "source_revision": revision, "sdk": str(sdk),
              "scope": "Build and SDK assembly only; tests/runtime/hardware NOT_RUN"}
    write_json(workspace / "build-result.json", result)
    return result


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--support-root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--depot-tools", type=Path, required=True)
    parser.add_argument("--dependencies-root", type=Path)
    parser.add_argument("--sdk-output", type=Path)
    parser.add_argument("--revision", default="branch",
                        help="branch (latest fork branch), release (shipped commit), or an explicit commit")
    parser.add_argument("--configurations", nargs="+", choices=("Debug", "Release"),
                        default=["Debug", "Release"])
    parser.add_argument("--cmake", default="cmake", help="CMake 3.31.x recommended; old x265 rejects CMake 4")
    parser.add_argument("--generator", default="Visual Studio 17 2022")
    parser.add_argument("--nasm", default="nasm", help="NASM executable path or executable on PATH")
    parser.add_argument("--jobs", type=str, default="8")
    parser.add_argument("--execute", action="store_true")
    return parser.parse_args()


if __name__ == "__main__":
    try:
        build(parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("[ERROR] " + str(error), file=sys.stderr)
        sys.exit(1)
