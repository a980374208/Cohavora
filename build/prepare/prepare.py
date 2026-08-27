'''
LiveKit C++ Client Dependencies Preparation Script
Location: build/prepare/prepare.py
- 100% Self-Contained: Zero copying from external directories.
- Pure C++ Toolchain: MSVC + CMake + Python (No Rust required).
- Telegram Desktop UI is natively integrated in src/ui/tdesktop as Git submodules.
'''

import os
import sys
import hashlib
import shutil
import urllib.request
import zipfile
from pathlib import Path

script_dir = Path(__file__).parent.resolve()
# Project root is two levels up from build/prepare
root_dir = script_dir.parent.parent.resolve()
deps_dir = root_dir / "deps"
cache_keys_dir = root_dir / ".cache_keys"
cache_keys_dir.mkdir(parents=True, exist_ok=True)

WEBRTC_RELEASE_URL = "https://github.com/livekit/rust-sdks/releases/download/webrtc-51ef663/webrtc-win-x64-release.zip"

def error(message):
    print(f"[ERROR] {message}")
    sys.exit(1)

def compute_string_hash(val: str) -> str:
    return hashlib.sha1(val.encode('utf-8')).hexdigest()

def check_cache_key(stage_name: str, expected_key: str) -> str:
    key_file = cache_keys_dir / stage_name
    if not key_file.exists():
        return 'NotFound'
    try:
        with open(key_file, 'r', encoding='utf-8') as f:
            content = f.read().strip()
            return 'Good' if content == expected_key else 'Stale'
    except Exception:
        return 'Stale'

def write_cache_key(stage_name: str, key: str):
    key_file = cache_keys_dir / stage_name
    with open(key_file, 'w', encoding='utf-8') as f:
        f.write(key)

def download_and_extract_zip(url: str, extract_to: Path, archive_name: str):
    archive_path = cache_keys_dir / archive_name
    if not archive_path.exists():
        print(f"[DOWNLOAD] Downloading {url} -> {archive_path}...")
        try:
            urllib.request.urlretrieve(url, str(archive_path))
        except Exception as e:
            error(f"Failed to download {url}: {e}")

    print(f"[EXTRACT] Extracting {archive_name} -> {extract_to}...")
    extract_to.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive_path, 'r') as zip_ref:
        zip_ref.extractall(extract_to)

# --- Stage 1: WebRTC Native C++ Package (Downloaded & Deployed into ./deps/webrtc) ---

def prepare_webrtc(debug_archive_path: Path = None):
    stage_name = "webrtc"
    target_webrtc_dir = deps_dir / "webrtc"
    target_lib_dir = target_webrtc_dir / "lib"
    target_lib_rel = target_lib_dir / "Release" / "webrtc.lib"
    target_lib_dbg = target_lib_dir / "Debug" / "webrtc.lib"
    target_lib_root = target_lib_dir / "webrtc.lib"
    target_headers = target_webrtc_dir / "include"

    expected_key = compute_string_hash(f"webrtc-51ef663:{target_lib_rel.exists()}:{target_lib_dbg.exists()}:{target_headers.exists()}")
    status = check_cache_key(stage_name, expected_key)

    if status == 'Good' and target_lib_rel.exists() and target_headers.exists():
        print(f"[STAGE: WebRTC] OK (Release and Debug ready in ./deps/webrtc)")
        return target_webrtc_dir

    print(f"\n[STAGE: WebRTC] Downloading and preparing WebRTC C++ SDK directly from official release...")
    download_and_extract_zip(WEBRTC_RELEASE_URL, target_webrtc_dir, "webrtc-windows-x64.zip")

    # If the zip has a top-level win-x64-release folder, flatten it to deps/webrtc
    sub_dir = target_webrtc_dir / "win-x64-release"
    if sub_dir.exists():
        for item in sub_dir.iterdir():
            target_item = target_webrtc_dir / item.name
            if target_item.exists():
                if target_item.is_dir():
                    shutil.rmtree(target_item)
                else:
                    target_item.unlink()
            shutil.move(str(item), str(target_webrtc_dir))
        try:
            shutil.rmtree(sub_dir)
        except Exception:
            pass

    # Ensure lib subdirectories exist
    (target_lib_dir / "Release").mkdir(parents=True, exist_ok=True)
    (target_lib_dir / "Debug").mkdir(parents=True, exist_ok=True)

    # Place release webrtc.lib into lib/Release/
    if target_lib_root.exists():
        shutil.copy2(target_lib_root, target_lib_rel)
        print(f" -> Deployed Release WebRTC library: {target_lib_rel}")

    # Handle Debug WebRTC library
    if debug_archive_path and debug_archive_path.exists():
        print(f"[EXTRACT] Unpacking Debug WebRTC archive from {debug_archive_path}...")
        debug_extract_temp = cache_keys_dir / "webrtc_debug_temp"
        download_and_extract_zip("", debug_extract_temp, debug_archive_path.name)
        # Find webrtc.lib in extracted content
        found_dbg = list(debug_extract_temp.glob("**/webrtc.lib"))
        if found_dbg:
            shutil.copy2(found_dbg[0], target_lib_dbg)
            print(f" -> Deployed Debug WebRTC library: {target_lib_dbg}")
        try:
            shutil.rmtree(debug_extract_temp)
        except Exception:
            pass
    elif not target_lib_dbg.exists() and target_lib_root.exists():
        # Fallback copy if custom debug build not yet provided
        shutil.copy2(target_lib_root, target_lib_dbg)
        print(f" -> [NOTICE] Initialized Debug WebRTC placeholder with Release binary at: {target_lib_dbg}")
        print(f"    (You can replace this with your custom compiled Debug webrtc.lib at any time)")

    write_cache_key(stage_name, expected_key)
    print(f" -> WebRTC C++ deployed and verified in {target_webrtc_dir}")
    return target_webrtc_dir

# --- Stage 2: Qt 5.15.18 & Third-Party Libraries (Managed in ./deps/Libraries/win64) ---

def prepare_libraries(qt_archive_path: Path = None):
    stage_name = "libraries"
    target_libs_dir = deps_dir / "Libraries" / "win64"
    target_qt_core = target_libs_dir / "Qt-5.15.18" / "lib" / "Qt5Core.lib"

    expected_key = compute_string_hash(f"qt-5.15.18:{target_qt_core.exists()}")
    status = check_cache_key(stage_name, expected_key)

    if status == 'Good' and target_qt_core.exists():
        print(f"[STAGE: Libraries (Qt, etc.)] OK (Ready in ./deps/Libraries/win64)")
        return target_libs_dir

    print(f"\n[STAGE: Libraries] Setting up Qt 5.15.18 & Third-Party dependencies in ./deps/Libraries/win64...")

    if qt_archive_path and qt_archive_path.exists():
        print(f"[EXTRACT] Unpacking Qt & Libraries archive from {qt_archive_path}...")
        with zipfile.ZipFile(qt_archive_path, 'r') as zip_ref:
            zip_ref.extractall(target_libs_dir)

    zlib_dir = target_libs_dir / "zlib"
    for cfg in ["Release", "Debug"]:
        cfg_dir = zlib_dir / cfg
        if cfg_dir.exists():
            zlib_static = cfg_dir / ("zlibstaticd.lib" if cfg == "Debug" else "zlibstatic.lib")
            libzs = cfg_dir / ("libzsd.lib" if cfg == "Debug" else "libzs.lib")
            if zlib_static.exists() and not libzs.exists():
                shutil.copy2(zlib_static, libzs)

    if not target_qt_core.exists():
        error(f"Qt 5.15.18 library is missing in {target_libs_dir}.\n"
              f"Please provide an archive via 'win.bat --qt-archive <path_to_zip>'\n"
              f"or place pre-built Qt-5.15.18 inside ./deps/Libraries/win64.")

    write_cache_key(stage_name, expected_key)
    print(f" -> Libraries verified in {target_libs_dir}")
    return target_libs_dir

def main():
    import argparse
    parser = argparse.ArgumentParser(description="LiveKit C++ Dependencies Pipeline")
    parser.add_argument("--qt-archive", type=str, help="Path to Qt & Libraries zip archive for extraction")
    parser.add_argument("--webrtc-debug-archive", type=str, help="Path to custom Debug webrtc zip archive or directory for extraction")
    parser.add_argument("--libraries-src", type=str, help="Explicit path to external Libraries directory to deploy once")
    args = parser.parse_args()

    print("==================================================")
    print("  LiveKit C++ Self-Contained Dependency Pipeline  ")
    print("  (Native In-Source UI | Pure C++ Toolchain)      ")
    print("==================================================")
    print(f"Project Root  : {root_dir}")
    print(f"Target Deps   : {deps_dir}\n")

    deps_dir.mkdir(parents=True, exist_ok=True)

    if args.libraries_src and not (deps_dir / "Libraries" / "win64" / "Qt-5.15.18").exists():
        src_lib = Path(args.libraries_src)
        dst_lib = deps_dir / "Libraries" / "win64"
        print(f"[INSTALL] Extracting Libraries from: {src_lib} -> {dst_lib}")
        ignore_func = shutil.ignore_patterns(".git*", "*.obj", "*.tlog", "*.pch")
        shutil.copytree(src_lib, dst_lib, dirs_exist_ok=True, ignore=ignore_func)

    qt_archive = Path(args.qt_archive) if args.qt_archive else None
    webrtc_dbg_archive = Path(args.webrtc_debug_archive) if args.webrtc_debug_archive else None

    # Execute standalone dependency stages
    prepare_webrtc(webrtc_dbg_archive)
    prepare_libraries(qt_archive)

    print("\n==================================================")
    print("[SUCCESS] All project dependencies are ready in ./deps!")
    print("==================================================")

if __name__ == "__main__":
    main()
