import os
import sys
import subprocess
import argparse
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description="LiveKit C++ Client Project Configuration Script")
    parser.add_argument("-B", "--build-dir", type=str, default="build-debug", help="Build directory (default: build-debug)")
    parser.add_argument("-G", "--generator", type=str, help="CMake generator (e.g., 'Visual Studio 17 2022', 'Ninja')")
    parser.add_argument("-A", "--architecture", type=str, help="Generator architecture specifier (e.g., x64)")
    parser.add_argument("--webrtc-root", type=str, help="Custom path to WebRTC root")
    parser.add_argument("--tdesktop-dir", type=str, help="Custom path to TDesktop repository directory")
    parser.add_argument("--tdesktop-libs", type=str, help="Custom path to TDesktop Libraries/win64 directory")
    parser.add_argument("--build-type", type=str, choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"], help="CMake build type")

    args, extra_cmake_args = parser.parse_known_args()

    project_root = Path(__file__).parent.resolve()
    build_dir = project_root / args.build_dir

    print("==================================================")
    print("      LiveKit C++ Project Configurator            ")
    print("==================================================")
    print(f"Project Root    : {project_root}")
    print(f"Build Directory : {build_dir}")

    # Pre-check dependency files directly in project-local ./deps
    deps_dir = project_root / "deps"
    qt_lib = deps_dir / "Libraries" / "win64" / "Qt-5.15.18" / "lib" / "Qt5Core.lib"
    webrtc_lib_root = deps_dir / "webrtc" / "lib" / "webrtc.lib"
    webrtc_lib_rel = deps_dir / "webrtc" / "lib" / "Release" / "webrtc.lib"
    webrtc_lib_dbg = deps_dir / "webrtc" / "lib" / "Debug" / "webrtc.lib"
    has_webrtc = webrtc_lib_root.exists() or webrtc_lib_rel.exists() or webrtc_lib_dbg.exists()

    if not qt_lib.exists() or not has_webrtc:
        print("\n[WARN] Project local dependencies in './deps' are missing or incomplete.")
        print("[INFO] Invoking build/prepare/win.bat to prepare dependencies into ./deps...\n")
        prep_cmd = ["cmd", "/c", str(project_root / "build" / "prepare" / "win.bat")]
        res_prep = subprocess.run(prep_cmd)
        if res_prep.returncode != 0:
            print("\n[ERROR] Dependency preparation failed. Cannot proceed with CMake configuration.")
            sys.exit(1)

    cmd = ["cmake", "-S", str(project_root), "-B", str(build_dir)]

    if args.generator:
        cmd.extend(["-G", args.generator])
        if "Visual Studio" in args.generator and args.architecture:
            cmd.extend(["-A", args.architecture])

    if args.build_type:
        cmd.append(f"-DCMAKE_BUILD_TYPE={args.build_type}")

    if args.webrtc_root:
        cmd.append(f"-DWEBRTC_ROOT={args.webrtc_root}")
    if args.tdesktop_dir:
        cmd.append(f"-DTDESKTOP_DIR={args.tdesktop_dir}")
    if args.tdesktop_libs:
        cmd.append(f"-DTDESKTOP_LIBS_DIR={args.tdesktop_libs}")

    # Append any remaining raw CMake arguments
    cmd.extend(extra_cmake_args)

    print(f"\n[INFO] Executing CMake Command:")
    print(f"  {' '.join(cmd)}\n")

    res = subprocess.run(cmd)
    if res.returncode != 0:
        print("[ERROR] CMake configuration failed.")
        sys.exit(res.returncode)

    print("[SUCCESS] Project configuration finished successfully!")

if __name__ == "__main__":
    main()
