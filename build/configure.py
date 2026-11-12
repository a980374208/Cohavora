import os
import sys
import subprocess
import argparse
import json
import re
import platform
import shlex
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
    arguments = [str(argument) for argument in command]
    return subprocess.list2cmdline(arguments) if os.name == "nt" else shlex.join(arguments)


def resolve_vcpkg_root(project_root):
    configured = os.environ.get("VCPKG_ROOT")
    root = resolve_input_path(configured or project_root.parent / "vcpkg", project_root)
    if not (root / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
        configuration_error(
            f"vcpkg toolchain was not found under {root}. Set VCPKG_ROOT to "
            "a complete vcpkg checkout.")
    return root


def offline_environment(environment):
    asset_sources = environment.get("X_VCPKG_ASSET_SOURCES", "").rstrip(";")
    environment["X_VCPKG_ASSET_SOURCES"] = (
        f"{asset_sources};x-block-origin" if asset_sources else "x-block-origin")
    environment["VCPKG_BINARY_SOURCES"] = "clear;default,read"
    return environment


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

    is_windows = sys.platform == "win32"
    is_linux = sys.platform.startswith("linux")
    if not (is_windows or is_linux):
        configuration_error(f"Unsupported host platform: {sys.platform}")

    parser = argparse.ArgumentParser(description="Cohavora Project Configuration Script")
    default_build_dir = "out/build/manual" if is_windows else "out/build/ubuntu"
    parser.add_argument("-B", "--build-dir", type=str, default=default_build_dir,
                        help=f"Build directory (default: {default_build_dir})")
    parser.add_argument("-G", "--generator", type=str, help="CMake generator (e.g., 'Visual Studio 17 2022', 'Ninja')")
    parser.add_argument("-A", "--architecture", type=str, help="Generator architecture specifier (e.g., x64)")
    parser.add_argument("--webrtc-root", type=str, help="Custom path to WebRTC root")
    parser.add_argument("--qt-root", type=str,
                        help="Linux Qt 5.15.18 installation prefix")
    parser.add_argument("--tdesktop-libs", type=str,
                        help="Windows TDesktop Libraries/win64 directory")
    parser.add_argument("--build-type", type=str, choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"], help="CMake build type")
    dependency_mode = parser.add_mutually_exclusive_group()
    dependency_mode.add_argument(
        "--prepare-deps", action="store_true",
        help="Explicitly prepare project-local dependencies before configuring")
    dependency_mode.add_argument(
        "--offline", action="store_true",
        help="Use existing dependencies and prevent vcpkg origin downloads")
    parser.add_argument(
        "--vcpkg-only", action="store_true",
        help="On Linux, install the manifest's vcpkg dependencies without configuring CMake")
    libraries_input = parser.add_mutually_exclusive_group()
    libraries_input.add_argument(
        "--qt-archive", type=str,
        help="Pinned platform Qt/Libraries ZIP passed to dependency preparation")
    libraries_input.add_argument(
        "--libraries-src", type=str,
        help="Complete Qt/Libraries directory passed to dependency preparation")
    parser.add_argument(
        "--webrtc-archive", type=str,
        help="Pinned platform WebRTC archive passed to dependency preparation")

    args, extra_cmake_args = parser.parse_known_args()
    if any(
            argument == "--tdesktop-dir"
            or argument.startswith("--tdesktop-dir=")
            for argument in extra_cmake_args):
        configuration_error(
            "--tdesktop-dir was removed because the project always uses the "
            "in-tree TDesktop sources.")
    preparation_inputs = (
        args.qt_archive, args.libraries_src, args.webrtc_archive)
    if any(preparation_inputs) and not args.prepare_deps:
        configuration_error(
            "Dependency archive/source options require --prepare-deps.")
    if is_windows and args.vcpkg_only:
        configuration_error("--vcpkg-only is currently available on Linux only.")
    if args.vcpkg_only and args.prepare_deps:
        configuration_error("--vcpkg-only cannot be combined with --prepare-deps.")
    if is_windows and args.qt_root:
        configuration_error("--qt-root is for Linux; use --tdesktop-libs on Windows.")
    if is_linux and args.tdesktop_libs:
        configuration_error("--tdesktop-libs is for Windows; use --qt-root on Linux.")
    if args.vcpkg_only and extra_cmake_args:
        configuration_error("CMake arguments cannot be used with --vcpkg-only.")
    if args.vcpkg_only and any((
            args.generator, args.build_type, args.webrtc_root,
            args.qt_root, args.tdesktop_libs)):
        configuration_error(
            "Generator, build type, WebRTC and Qt options do not apply to --vcpkg-only.")

    cmake_executable = None if args.vcpkg_only else require_cmake()

    project_root = Path(__file__).resolve().parent.parent
    build_dir = project_root / args.build_dir
    qt_archive = (
        Path(args.qt_archive).expanduser().resolve()
        if args.qt_archive else None)
    libraries_source = (
        Path(args.libraries_src).expanduser().resolve()
        if args.libraries_src else None)
    webrtc_archive = (
        Path(args.webrtc_archive).expanduser().resolve()
        if args.webrtc_archive else None)
    if qt_archive and not qt_archive.is_file():
        configuration_error(f"Qt archive does not exist: {qt_archive}")
    if libraries_source and not libraries_source.is_dir():
        configuration_error(
            f"Libraries source directory does not exist: {libraries_source}")
    if webrtc_archive and not webrtc_archive.is_file():
        configuration_error(
            f"WebRTC package does not exist: {webrtc_archive}")

    cache_path = build_dir / "CMakeCache.txt"
    cached_definitions = {} if args.vcpkg_only else read_cmake_cache(cache_path)
    explicit_definitions = {}
    if args.webrtc_root:
        explicit_definitions["WEBRTC_ROOT"] = args.webrtc_root
    if args.qt_root:
        explicit_definitions["QT_ROOT"] = args.qt_root
    if args.tdesktop_libs:
        explicit_definitions["TDESKTOP_LIBS_DIR"] = args.tdesktop_libs
    explicit_definitions.update(parse_cmake_definitions(extra_cmake_args))

    effective_definitions = dict(cached_definitions)
    effective_definitions.update(explicit_definitions)
    deps_dir = resolve_input_path(
        effective_definitions.get("DEPS_DIR", project_root / "deps"), project_root)
    vcpkg_install_root = resolve_input_path(
        effective_definitions.get("VCPKG_INSTALLED_DIR",
                                  deps_dir / "vcpkg_installed"), project_root)
    vcpkg_root = (
        resolve_vcpkg_root(project_root)
        if is_linux and (args.vcpkg_only
                         or not effective_definitions.get("CMAKE_TOOLCHAIN_FILE"))
        else None)

    cached_generator = cached_definitions.get("CMAKE_GENERATOR")
    if args.generator and cached_generator and args.generator != cached_generator:
        configuration_error(
            f"Build directory already uses generator '{cached_generator}', "
            f"not '{args.generator}'. Choose a different build directory.")
    selected_generator = (
        args.generator
        or cached_generator
        or os.environ.get("CMAKE_GENERATOR")
        or ("Ninja" if is_linux else None))

    cached_architecture = normalize_architecture(
        cached_definitions.get("CMAKE_GENERATOR_PLATFORM"))
    requested_architecture = normalize_architecture(
        args.architecture
        or cached_architecture
        or (os.environ.get("VSCMD_ARG_TGT_ARCH") if is_windows else platform.machine())
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
    triplet_platform = "windows" if is_windows else "linux"
    target_triplet = (effective_definitions.get("VCPKG_TARGET_TRIPLET")
                      or ("x64-windows-static" if is_windows else "x64-linux"))
    if not (target_triplet.lower() == f"x64-{triplet_platform}"
            or target_triplet.lower().startswith(f"x64-{triplet_platform}-")):
        configuration_error(
            f"VCPKG_TARGET_TRIPLET='{target_triplet}' does not match this "
            f"x64 {triplet_platform} build.")
    if args.architecture and not generator_supports_platform(selected_generator):
        configuration_error(
            f"Generator '{selected_generator}' does not support CMake -A. "
            "Select an x64 compiler environment instead.")
    if is_linux and selected_generator and selected_generator.startswith("Visual Studio"):
        configuration_error("Visual Studio generators cannot configure a Linux build.")

    if args.vcpkg_only:
        vcpkg_executable = vcpkg_root / "vcpkg"
        if not vcpkg_executable.is_file() or not os.access(vcpkg_executable, os.X_OK):
            configuration_error(
                f"vcpkg executable was not found at {vcpkg_executable}. "
                f"Bootstrap it first with {vcpkg_root / 'bootstrap-vcpkg.sh'}.")
        cmd = [str(vcpkg_executable), "install",
               f"--triplet={target_triplet}",
               f"--x-manifest-root={project_root / 'build' / 'vcpkg'}",
               f"--x-install-root={vcpkg_install_root}"]
        environment = os.environ.copy()
        environment["VCPKG_ROOT"] = str(vcpkg_root)
        if not environment.get("VCPKG_DOWNLOADS"):
            environment["VCPKG_DOWNLOADS"] = str(deps_dir / "vcpkg_downloads")
        if args.offline:
            offline_environment(environment)
        print(f"[INFO] Installing Ubuntu vcpkg dependencies: {format_command(cmd)}")
        sys.stdout.flush()
        result = run_process(cmd, cwd=project_root, env=environment)
        if result.returncode != 0:
            configuration_error(f"vcpkg install failed (exit {result.returncode}).")
        print("[SUCCESS] Ubuntu vcpkg dependencies installed.")
        return

    webrtc_root = resolve_input_path(
        effective_definitions.get("WEBRTC_ROOT", os.environ.get("WEBRTC_ROOT", deps_dir / "webrtc")),
        project_root)
    tdesktop_libs_dir = resolve_input_path(
        effective_definitions.get(
            "TDESKTOP_LIBS_DIR",
            os.environ.get("TDESKTOP_LIBS_DIR", deps_dir / "Libraries" /
                           ("win64" if is_windows else "linux64"))),
        project_root)
    qt_root = resolve_input_path(
        effective_definitions.get("QT_ROOT", os.environ.get(
            "QT_ROOT", deps_dir / "Libraries" / "linux64"
            if is_linux else deps_dir / "Qt-5.15.18")), project_root)
    meeting_app_value = effective_definitions.get(
        "COHAVORA_BUILD_MEETING_APP",
        effective_definitions.get("LIVEKIT_BUILD_MEETING_APP", "ON"))
    needs_qt = cmake_is_true(meeting_app_value)

    print("==================================================")
    print("           Cohavora Project Configurator          ")
    print("==================================================")
    print(f"Project Root    : {project_root}")
    print(f"Build Directory : {build_dir}")

    local_deps_dir = deps_dir if is_linux else (project_root / "deps").resolve()
    local_webrtc_root = local_deps_dir / "webrtc"
    local_tdesktop_libs = local_deps_dir / "Libraries" / ("win64" if is_windows else "linux64")
    local_qt_root = (local_deps_dir / "Qt-5.15.18"
                     if is_windows else local_tdesktop_libs)
    if is_windows:
        webrtc_metadata = json.loads((
            project_root / "build" / "prepare" / "webrtc-package.json"
        ).read_text(encoding="utf-8"))
        webrtc_required_files = tuple(webrtc_metadata["required_files"])
        libraries_manifest = project_root / "build" / "prepare" / "libraries-required.txt"
        libraries_required_files = []
        for raw_line in libraries_manifest.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if line and not line.startswith("#"):
                libraries_required_files.append(line)
        libraries_required_files = tuple(libraries_required_files)
    else:
        webrtc_required_files = (
            "include/api/peer_connection_interface.h",
            "include/third_party/libyuv/include/libyuv.h",
            "generated/Release/build/chromeos_buildflags.h",
            "generated/Debug/build/chromeos_buildflags.h",
            "prefix/Release/boringssl_prefix_symbols.h",
            "prefix/Debug/boringssl_prefix_symbols.h",
            "lib/Release/libwebrtc.a",
            "lib/Debug/libwebrtc.a")
        libraries_required_files = (
            "lib/cmake/Qt5/Qt5Config.cmake",
            "lib/libQt5Core.a",
            "lib/libQt5Gui.a",
            "lib/libQt5Widgets.a",
            "lib/libQt5Network.a",
            "lib/libQt5Svg.a")

    def libraries_are_complete(root):
        return all(
            (root / relative).is_file()
            for relative in libraries_required_files)

    def missing_dependencies():
        missing = []
        if not all(
                (webrtc_root / relative).is_file()
                for relative in webrtc_required_files):
            missing.append(("WebRTC", webrtc_root, webrtc_root == local_webrtc_root))
        libraries_root = tdesktop_libs_dir if is_windows else qt_root
        libraries_complete = libraries_are_complete(libraries_root)
        if needs_qt and not libraries_complete:
            missing.append((
                "Qt/Libraries", libraries_root,
                libraries_root == (local_tdesktop_libs if is_windows else local_qt_root)))
        return missing

    missing = missing_dependencies()
    external_missing = [item for item in missing if not item[2]]
    if external_missing:
        for name, path, _ in external_missing:
            print(f"[ERROR] Configured {name} dependency is incomplete: {path}")
        print("[ERROR] Fix the configured dependency path before running CMake.")
        sys.exit(1)

    if args.prepare_deps:
        local_libraries_root = tdesktop_libs_dir if is_windows else qt_root
        expected_libraries_root = (local_tdesktop_libs if is_windows
                                   else local_qt_root)
        if (webrtc_root != local_webrtc_root
                or local_libraries_root != expected_libraries_root):
            configuration_error(
                "--prepare-deps manages the configured DEPS_DIR only. Remove "
                "custom WebRTC/Qt roots or prepare them separately.")
        prep_cmd = [
            sys.executable,
            str(project_root / "build" / "prepare" / "prepare.py")]
        if is_linux and deps_dir != (project_root / "deps").resolve():
            prep_cmd.extend(["--deps-dir", str(deps_dir)])
        if qt_archive:
            prep_cmd.extend(["--qt-archive", str(qt_archive)])
        if libraries_source:
            prep_cmd.extend(["--libraries-src", str(libraries_source)])
        if webrtc_archive:
            prep_cmd.extend(["--webrtc-archive", str(webrtc_archive)])
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
        if is_windows:
            print(
                "[ERROR] Run again with --prepare-deps to download the pinned "
                "archive, provide an explicit archive/source, or configure complete "
                "external paths.")
        else:
            print("[ERROR] Run python3 build/prepare/prepare.py or retry with "
                  "--prepare-deps; custom SDKs may use --webrtc-root and --qt-root.")
        sys.exit(1)

    cmd = [cmake_executable, "-S", str(project_root), "-B", str(build_dir)]

    if args.generator or (is_linux and not cached_generator
                          and not os.environ.get("CMAKE_GENERATOR")):
        cmd.extend(["-G", selected_generator])

    if is_windows and not cache_path.is_file() and generator_supports_platform(selected_generator):
        cmd.extend(["-A", "x64"])

    if is_linux:
        if not effective_definitions.get("CMAKE_TOOLCHAIN_FILE"):
            cmd.append(f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}")
        if "VCPKG_TARGET_TRIPLET" not in effective_definitions:
            cmd.append(f"-DVCPKG_TARGET_TRIPLET={target_triplet}")
        if "VCPKG_INSTALLED_DIR" not in effective_definitions:
            cmd.append(f"-DVCPKG_INSTALLED_DIR={vcpkg_install_root}")

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
    if is_linux:
        cmd.append(f"-DQT_ROOT={qt_root}")
    if args.tdesktop_libs:
        cmd.append(f"-DTDESKTOP_LIBS_DIR={tdesktop_libs_dir}")

    # Append any remaining raw CMake arguments
    cmd.extend(extra_cmake_args)

    cmake_environment = os.environ.copy() if is_linux or args.offline else None
    if vcpkg_root:
        cmake_environment["VCPKG_ROOT"] = str(vcpkg_root)
    if is_linux and not cmake_environment.get("VCPKG_DOWNLOADS"):
        cmake_environment["VCPKG_DOWNLOADS"] = str(deps_dir / "vcpkg_downloads")
    if args.offline:
        offline_environment(cmake_environment)
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
