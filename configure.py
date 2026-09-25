import os
import sys
import subprocess
import argparse
import re
import shutil
from pathlib import Path


FALSE_CMAKE_VALUES = {"", "0", "FALSE", "OFF", "NO", "N", "IGNORE", "NOTFOUND"}


def read_cmake_cache(cache_path):
    values = {}
    if not cache_path.is_file():
        return values
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        values[key] = value
    return values


def parse_cmake_definitions(arguments):
    values = {}
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-D" and index + 1 < len(arguments):
            index += 1
            definition = arguments[index]
        elif argument.startswith("-D"):
            definition = argument[2:]
        else:
            index += 1
            continue

        if "=" in definition:
            key_and_type, value = definition.split("=", 1)
            values[key_and_type.split(":", 1)[0]] = value
        index += 1
    return values


def cmake_is_true(value):
    normalized = str(value).strip().upper()
    return normalized not in FALSE_CMAKE_VALUES and not normalized.endswith("-NOTFOUND")


def resolve_input_path(value, project_root):
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = project_root / path
    return path.resolve()


def normalize_architecture(value):
    normalized = str(value or "").strip().lower().replace("-", "_")
    aliases = {
        "amd64": "x64",
        "x86_64": "x64",
        "win32": "x86",
        "aarch64": "arm64",
    }
    return aliases.get(normalized, normalized)


def is_multi_config_generator(generator):
    if not generator:
        return os.name == "nt"
    return (
        generator.startswith("Visual Studio")
        or generator == "Xcode"
        or generator == "Ninja Multi-Config"
    )


def generator_supports_platform(generator):
    return not generator or generator.startswith("Visual Studio")


def configuration_error(message):
    print(f"[ERROR] {message}")
    sys.exit(2)


def format_command(command):
    return subprocess.list2cmdline([str(argument) for argument in command])


def run_process(command, **kwargs):
    try:
        return subprocess.run(command, **kwargs)
    except OSError as exception:
        configuration_error(
            f"Failed to start '{command[0]}': {exception}")


def require_cmake():
    executable = shutil.which("cmake")
    if not executable:
        configuration_error(
            "CMake was not found in PATH. Install CMake 3.25 or newer.")
    result = run_process(
        [executable, "--version"], capture_output=True, text=True)
    if result.returncode != 0:
        configuration_error(
            f"Unable to query CMake version (exit {result.returncode}).")
    match = re.search(r"cmake version (\d+)\.(\d+)\.(\d+)", result.stdout)
    if not match:
        configuration_error("Unable to parse the installed CMake version.")
    version = tuple(int(part) for part in match.groups())
    if version < (3, 25, 0):
        configuration_error(
            f"CMake {'.'.join(map(str, version))} is too old; 3.25+ is required.")
    return executable


def main():
    if sys.version_info < (3, 8):
        configuration_error("Python 3.8 or newer is required.")

    parser = argparse.ArgumentParser(description="Cohavora Project Configuration Script")
    parser.add_argument("-B", "--build-dir", type=str, default="out/build/manual", help="Build directory (default: out/build/manual)")
    parser.add_argument("-G", "--generator", type=str, help="CMake generator (e.g., 'Visual Studio 17 2022', 'Ninja')")
    parser.add_argument("-A", "--architecture", type=str, help="Generator architecture specifier (e.g., x64)")
    parser.add_argument("--webrtc-root", type=str, help="Custom path to WebRTC root")
    parser.add_argument("--tdesktop-libs", type=str, help="Custom path to TDesktop Libraries/win64 directory")
    parser.add_argument("--build-type", type=str, choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"], help="CMake build type")
    dependency_mode = parser.add_mutually_exclusive_group()
    dependency_mode.add_argument(
        "--prepare-deps", action="store_true",
        help="Explicitly prepare project-local dependencies before configuring")
    dependency_mode.add_argument(
        "--offline", action="store_true",
        help="Use existing dependencies and prevent vcpkg origin downloads")
    libraries_input = parser.add_mutually_exclusive_group()
    libraries_input.add_argument(
        "--qt-archive", type=str,
        help="Pinned Qt/Libraries ZIP passed to dependency preparation")
    libraries_input.add_argument(
        "--libraries-src", type=str,
        help="Complete Qt/Libraries directory passed to dependency preparation")
    parser.add_argument(
        "--webrtc-debug-archive", type=str,
        help="Debug WebRTC ZIP, directory, or .lib passed to dependency preparation")

    args, extra_cmake_args = parser.parse_known_args()
    if any(
            argument == "--tdesktop-dir"
            or argument.startswith("--tdesktop-dir=")
            for argument in extra_cmake_args):
        configuration_error(
            "--tdesktop-dir was removed because the project always uses the "
            "in-tree TDesktop sources.")
    preparation_inputs = (
        args.qt_archive, args.libraries_src, args.webrtc_debug_archive)
    if any(preparation_inputs) and not args.prepare_deps:
        configuration_error(
            "Dependency archive/source options require --prepare-deps.")

    cmake_executable = require_cmake()

    project_root = Path(__file__).parent.resolve()
    build_dir = project_root / args.build_dir
    qt_archive = (
        Path(args.qt_archive).expanduser().resolve()
        if args.qt_archive else None)
    libraries_source = (
        Path(args.libraries_src).expanduser().resolve()
        if args.libraries_src else None)
    webrtc_debug = (
        Path(args.webrtc_debug_archive).expanduser().resolve()
        if args.webrtc_debug_archive else None)
    if qt_archive and not qt_archive.is_file():
        configuration_error(f"Qt archive does not exist: {qt_archive}")
    if libraries_source and not libraries_source.is_dir():
        configuration_error(
            f"Libraries source directory does not exist: {libraries_source}")
    if webrtc_debug and not webrtc_debug.exists():
        configuration_error(
            f"Debug WebRTC source does not exist: {webrtc_debug}")

    cache_path = build_dir / "CMakeCache.txt"
    cached_definitions = read_cmake_cache(cache_path)
    explicit_definitions = {}
    if args.webrtc_root:
        explicit_definitions["WEBRTC_ROOT"] = args.webrtc_root
    if args.tdesktop_libs:
        explicit_definitions["TDESKTOP_LIBS_DIR"] = args.tdesktop_libs
    explicit_definitions.update(parse_cmake_definitions(extra_cmake_args))

    effective_definitions = dict(cached_definitions)
    effective_definitions.update(explicit_definitions)

    cached_generator = cached_definitions.get("CMAKE_GENERATOR")
    if args.generator and cached_generator and args.generator != cached_generator:
        configuration_error(
            f"Build directory already uses generator '{cached_generator}', "
            f"not '{args.generator}'. Choose a different build directory.")
    selected_generator = (
        args.generator
        or cached_generator
        or os.environ.get("CMAKE_GENERATOR"))

    cached_architecture = normalize_architecture(
        cached_definitions.get("CMAKE_GENERATOR_PLATFORM"))
    requested_architecture = normalize_architecture(
        args.architecture
        or cached_architecture
        or os.environ.get("VSCMD_ARG_TGT_ARCH")
        or "x64")
    if (args.architecture and cached_architecture
            and normalize_architecture(args.architecture) != cached_architecture):
        configuration_error(
            f"Build directory already uses architecture '{cached_architecture}', "
            f"not '{normalize_architecture(args.architecture)}'. Choose a different "
            "build directory.")
    dependency_architecture = normalize_architecture(
        effective_definitions.get("COHAVORA_DEPENDENCY_ARCHITECTURE", "x64"))
    if dependency_architecture != "x64":
        configuration_error(
            "COHAVORA_DEPENDENCY_ARCHITECTURE currently supports only x64.")
    if requested_architecture != dependency_architecture:
        configuration_error(
            f"Requested architecture '{requested_architecture}' does not match "
            "the bundled x64 WebRTC and Qt dependencies.")
    target_triplet = effective_definitions.get("VCPKG_TARGET_TRIPLET", "")
    if target_triplet and not target_triplet.lower().startswith("x64-"):
        configuration_error(
            f"VCPKG_TARGET_TRIPLET='{target_triplet}' does not match the x64 "
            "WebRTC and Qt dependencies.")
    if args.architecture and not generator_supports_platform(selected_generator):
        configuration_error(
            f"Generator '{selected_generator}' does not support CMake -A. "
            "Select an x64 compiler environment instead.")

    deps_dir = resolve_input_path(
        effective_definitions.get("DEPS_DIR", project_root / "deps"), project_root)
    webrtc_root = resolve_input_path(
        effective_definitions.get("WEBRTC_ROOT", os.environ.get("WEBRTC_ROOT", deps_dir / "webrtc")),
        project_root)
    tdesktop_libs_dir = resolve_input_path(
        effective_definitions.get(
            "TDESKTOP_LIBS_DIR",
            os.environ.get("TDESKTOP_LIBS_DIR", deps_dir / "Libraries" / "win64")),
        project_root)
    meeting_app_value = effective_definitions.get(
        "COHAVORA_BUILD_MEETING_APP",
        effective_definitions.get("LIVEKIT_BUILD_MEETING_APP", "ON"))
    build_testing_value = effective_definitions.get("BUILD_TESTING", "ON")
    qt_tests_value = effective_definitions.get("COHAVORA_BUILD_QT_TESTS", "ON")
    needs_qt = (
        cmake_is_true(meeting_app_value)
        or (cmake_is_true(build_testing_value) and cmake_is_true(qt_tests_value)))

    print("==================================================")
    print("           Cohavora Project Configurator          ")
    print("==================================================")
    print(f"Project Root    : {project_root}")
    print(f"Build Directory : {build_dir}")

    local_deps_dir = (project_root / "deps").resolve()
    local_webrtc_root = local_deps_dir / "webrtc"
    local_tdesktop_libs = local_deps_dir / "Libraries" / "win64"
    libraries_manifest = project_root / "build" / "prepare" / "libraries-required.txt"
    libraries_required_files = []
    for raw_line in libraries_manifest.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line and not line.startswith("#"):
            libraries_required_files.append(line)
    libraries_required_files = tuple(libraries_required_files)

    def libraries_are_complete(root):
        return all(
            (root / relative).is_file()
            for relative in libraries_required_files)

    def missing_dependencies():
        webrtc_root_lib = webrtc_root / "lib" / "webrtc.lib"
        has_webrtc_release = (
            (webrtc_root / "lib" / "Release" / "webrtc.lib").is_file()
            or webrtc_root_lib.is_file())
        has_webrtc_debug = (
            (webrtc_root / "lib" / "Debug" / "webrtc.lib").is_file()
            or (webrtc_root / "lib" / "webrtc_d.lib").is_file()
            or webrtc_root_lib.is_file())
        missing = []
        if not (webrtc_root / "include").is_dir() or not (has_webrtc_release and has_webrtc_debug):
            missing.append(("WebRTC", webrtc_root, webrtc_root == local_webrtc_root))
        libraries_complete = libraries_are_complete(tdesktop_libs_dir)
        if needs_qt and not libraries_complete:
            missing.append((
                "Qt/Libraries", tdesktop_libs_dir,
                tdesktop_libs_dir == local_tdesktop_libs))
        return missing

    missing = missing_dependencies()
    external_missing = [item for item in missing if not item[2]]
    if external_missing:
        for name, path, _ in external_missing:
            print(f"[ERROR] Configured {name} dependency is incomplete: {path}")
        print("[ERROR] Fix the configured dependency path before running CMake.")
        sys.exit(1)

    if args.prepare_deps:
        if (webrtc_root != local_webrtc_root
                or tdesktop_libs_dir != local_tdesktop_libs):
            configuration_error(
                "--prepare-deps only manages project-local ./deps. Remove the "
                "custom dependency paths or prepare them separately.")
        prep_cmd = [
            sys.executable,
            str(project_root / "build" / "prepare" / "prepare.py")]
        if qt_archive:
            prep_cmd.extend(["--qt-archive", str(qt_archive)])
        if libraries_source:
            prep_cmd.extend(["--libraries-src", str(libraries_source)])
        if webrtc_debug:
            prep_cmd.extend(["--webrtc-debug-archive", str(webrtc_debug)])
        print("\n[INFO] Preparing project-local dependencies:")
        print(f"  {format_command(prep_cmd)}\n")
        sys.stdout.flush()
        res_prep = run_process(prep_cmd)
        if res_prep.returncode != 0:
            print("\n[ERROR] Dependency preparation failed. Cannot proceed with CMake configuration.")
            sys.exit(res_prep.returncode)
        missing = missing_dependencies()
        if missing:
            for name, path, _ in missing:
                print(f"[ERROR] Prepared {name} dependency is still incomplete: {path}")
            sys.exit(1)
    elif missing:
        for name, path, _ in missing:
            print(f"[ERROR] Required {name} dependency is incomplete: {path}")
        print(
            "[ERROR] Run again with --prepare-deps to download the pinned "
            "archive, provide an explicit archive/source, or configure complete "
            "external paths.")
        sys.exit(1)

    cmd = [cmake_executable, "-S", str(project_root), "-B", str(build_dir)]

    if args.generator:
        cmd.extend(["-G", args.generator])

    if not cache_path.is_file() and generator_supports_platform(selected_generator):
        cmd.extend(["-A", "x64"])

    if args.build_type:
        multi_config = (
            bool(cached_definitions.get("CMAKE_CONFIGURATION_TYPES"))
            or is_multi_config_generator(selected_generator))
        if multi_config:
            print(
                f"[INFO] {selected_generator or 'The default Windows generator'} "
                f"is multi-config; {args.build_type} will be selected at build time.")
        else:
            cmd.append(f"-DCMAKE_BUILD_TYPE={args.build_type}")

    if args.webrtc_root:
        cmd.append(f"-DWEBRTC_ROOT={webrtc_root}")
    if args.tdesktop_libs:
        cmd.append(f"-DTDESKTOP_LIBS_DIR={tdesktop_libs_dir}")

    # Append any remaining raw CMake arguments
    cmd.extend(extra_cmake_args)

    cmake_environment = None
    if args.offline:
        cmake_environment = os.environ.copy()
        asset_sources = cmake_environment.get(
            "X_VCPKG_ASSET_SOURCES", "").rstrip(";")
        cmake_environment["X_VCPKG_ASSET_SOURCES"] = (
            f"{asset_sources};x-block-origin"
            if asset_sources else "x-block-origin")
        cmake_environment["VCPKG_BINARY_SOURCES"] = "clear;default,read"
        print("[INFO] Offline mode: vcpkg origin downloads are disabled.")

    print("\n[INFO] Executing CMake Command:")
    print(f"  {format_command(cmd)}\n")
    sys.stdout.flush()

    res = run_process(cmd, env=cmake_environment)
    if res.returncode != 0:
        print("[ERROR] CMake configuration failed.")
        sys.exit(res.returncode)

    configured = read_cmake_cache(cache_path)
    if args.build_type and configured.get("CMAKE_CONFIGURATION_TYPES"):
        build_command = [
            cmake_executable, "--build", str(build_dir),
            "--config", args.build_type]
        print("[INFO] Build selected configuration with:")
        print(f"  {format_command(build_command)}")

    print("[SUCCESS] Project configuration finished successfully!")

if __name__ == "__main__":
    main()
