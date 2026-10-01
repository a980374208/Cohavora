"""Prepare the task-owned full backend checkout, retaining official references.

Run only after gclient sync. Compilation uses the pinned wrapper BUILD.gn and
its audio patch, rather than mixing patched headers with a different archive.
"""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import argparse

ROOT = Path(__file__).resolve().parents[4]
BUILD = ROOT / "build-e2ee-backend"
SOURCE = BUILD / "full/src"
WRAPPER = BUILD / "libwebrtc"
COMMIT = "aaeeee8077eb0a4cad1c9494e9c6433c751ef663"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if SOURCE.is_symlink() or SOURCE.resolve() != SOURCE.absolute():
        raise ValueError("backend checkout must be task-owned, not a reference link")
    head = subprocess.check_output(["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True).strip()
    if head != COMMIT:
        raise ValueError("unexpected backend commit")
    manifest = json.loads((BUILD / "patched/manifest.json").read_text())
    for name, key in [("frame_crypto_transformer.cc", "source_sha256"),
                      ("include/api/crypto/frame_crypto_transformer.h", "header_sha256")]:
        if sha(BUILD / "patched" / name) != manifest[key]:
            raise ValueError("crypto repair fingerprint mismatch")
    # Never reset an existing derivative checkout or silently apply twice.
    destination = SOURCE / "libwebrtc"
    if destination.exists():
        raise ValueError("wrapper destination already exists; inspect before preparing again")
    audio_patch = WRAPPER / "patches/custom_audio_source_m144.patch"
    target_patch = WRAPPER / "patches/add_libwebrtc_build_target.patch"
    subprocess.run(["git", "-C", str(SOURCE), "apply", "--check", str(target_patch)], check=True)
    subprocess.run(["git", "-C", str(SOURCE), "apply", "--check", str(audio_patch)], check=True)
    subprocess.run(["git", "-C", str(SOURCE), "apply", str(audio_patch)], check=True)
    subprocess.run(["git", "-C", str(SOURCE), "apply", str(target_patch)], check=True)
    destination.mkdir()
    for folder in ["include", "src"]:
        shutil.copytree(WRAPPER / folder, destination / folder)
    for name in ["BUILD.gn", "LICENSE"]:
        shutil.copy2(WRAPPER / name, destination / name)
    shutil.copy2(BUILD / "patched/frame_crypto_transformer.cc", SOURCE / "api/crypto/frame_crypto_transformer.cc")
    shutil.copy2(BUILD / "patched/include/api/crypto/frame_crypto_transformer.h", SOURCE / "api/crypto/frame_crypto_transformer.h")
    if "iv_header_sha256" in manifest:
        helper = BUILD / "patched/include/api/crypto/cohavora_iv_sequence.h"
        if sha(helper) != manifest["iv_header_sha256"]: raise ValueError("IV helper hash mismatch")
        shutil.copy2(helper, SOURCE / "api/crypto/cohavora_iv_sequence.h")
    manifest.update(wrapper=json.loads((BUILD / "libwebrtc-source.json").read_text()),
                    audio_patch_sha256=sha(audio_patch),
                    target_patch_sha256=sha(target_patch),
                    scope="derivative full backend; not the original Flutter release DLL")
    (BUILD / "full-prepared.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


def repair_window_focus():
    if SOURCE.is_symlink() or SOURCE.resolve() != SOURCE.absolute():
        raise ValueError('backend checkout must be task-owned')
    target = SOURCE / 'libwebrtc/src/rtc_desktop_capturer_impl.cc'
    baseline = WRAPPER / 'src/rtc_desktop_capturer_impl.cc'
    text = target.read_text(encoding='utf-8')
    old = '''      if (!capturer_->FocusOnSelectedSource()) {
        capture_state_ = CS_FAILED;
        return capture_state_;
      }'''
    new = '''      // Focusing is optional in DesktopCapturer. Windows can deny a
      // background process foreground activation while capture remains valid.
      // SelectSource and actual CaptureFrame results still determine failure.
      capturer_->FocusOnSelectedSource();'''
    manifest_path = BUILD / 'desktop-focus-repair.json'
    if manifest_path.exists():
        previous = json.loads(manifest_path.read_text(encoding='utf-8'))
        if sha(target) != previous['after_sha256']: raise ValueError('desktop repair input changed')
        return
    if sha(target) != sha(baseline) or text.count(old) != 1:
        raise ValueError('unexpected desktop wrapper input')
    before = sha(target)
    target.write_text(text.replace(old, new), encoding='utf-8', newline='\n')
    manifest_path.write_text(json.dumps({'scope': 'task-owned derivative window capture; optional focus failure no longer prevents capture',
        'before_sha256': before, 'after_sha256': sha(target), 'tool_sha256': sha(Path(__file__)),
        'crypto_changed': False}, indent=2), encoding='utf-8')


def refresh_crypto(patched):
    """Replace only verified task-owned crypto inputs; retain prior provenance."""
    if SOURCE.is_symlink() or SOURCE.resolve() != SOURCE.absolute():
        raise ValueError('backend checkout must be task-owned')
    metadata_path = BUILD / 'full-prepared.json'
    prior = json.loads(metadata_path.read_text(encoding='utf-8'))
    manifest = json.loads((patched / 'manifest.json').read_text(encoding='utf-8'))
    if manifest['upstream_commit'] != prior['upstream_commit']:
        raise ValueError('backend revision changed')
    files = [('frame_crypto_transformer.cc', 'api/crypto/frame_crypto_transformer.cc', 'source_sha256'),
             ('include/api/crypto/frame_crypto_transformer.h', 'api/crypto/frame_crypto_transformer.h', 'header_sha256'),
             ('include/api/crypto/cohavora_iv_sequence.h', 'api/crypto/cohavora_iv_sequence.h', 'iv_header_sha256')]
    for source, destination, field in files:
        if sha(patched / source) != manifest[field]: raise ValueError('new source hash mismatch')
        target = SOURCE / destination
        if field in prior:
            if sha(target) != prior[field]: raise ValueError('current derivative source hash mismatch')
        elif target.exists(): raise ValueError('unexpected existing derivative helper')
    archive = BUILD / ('full-prepared-' + prior['source_sha256'][:12] + '.json')
    if not archive.exists(): shutil.copy2(metadata_path, archive)
    for source, destination, field in files: shutil.copy2(patched / source, SOURCE / destination)
    prior.update(manifest)
    metadata_path.write_text(json.dumps(prior, indent=2), encoding='utf-8')


def stage_runtime():
    official = ROOT / "tests/runtime/tools/media/flutter_e2ee_peer/build/windows/x64/runner/Release"
    dll = SOURCE / "out-e2ee-release/libwebrtc.dll"
    if not dll.is_file():
        raise ValueError("DLL missing")
    destination = BUILD / ("flutter-repaired-" + sha(dll)[:12] + "-app-" + sha(official / "data/app.so")[:12])
    if destination.exists():
        raise ValueError("DLL missing or runtime already staged; inspect before replacing")
    shutil.copytree(official, destination)
    shutil.copy2(dll, destination / "libwebrtc.dll")
    inputs = json.loads((BUILD / "full-prepared.json").read_text())
    for source, field in [('api/crypto/frame_crypto_transformer.cc', 'source_sha256'),
                          ('api/crypto/frame_crypto_transformer.h', 'header_sha256'),
                          ('api/crypto/cohavora_iv_sequence.h', 'iv_header_sha256')]:
        if field in inputs and sha(SOURCE / source) != inputs[field]:
            raise ValueError('staged crypto source mismatch')
    metadata = {
        "kind": "derivative_full_backend", "configuration": "Release",
        "official_dll_sha256": sha(official / "libwebrtc.dll"),
        "dll_sha256": sha(dll),
        "executable_sha256": sha(destination / "e2ee_flutter_peer.exe"),
        "aot_sha256": sha(destination / "data/app.so"),
        "fixture_source_sha256": sha(ROOT / "tests/runtime/tools/media/flutter_e2ee_peer/lib/main.dart"),
        "build_args_sha256": sha(SOURCE / "out-e2ee-release/args.gn"),
        "inputs": inputs,
    }
    focus_repair = BUILD / 'desktop-focus-repair.json'
    if focus_repair.exists():
        metadata['desktop_capture_repair'] = json.loads(focus_repair.read_text(encoding='utf-8'))
        if sha(SOURCE / 'libwebrtc/src/rtc_desktop_capturer_impl.cc') != metadata['desktop_capture_repair']['after_sha256']:
            raise ValueError('desktop repair fingerprint mismatch')
    transport_root = BUILD / 'flutter-sdk-transport-v1'
    override = ROOT / 'tests/runtime/tools/media/flutter_e2ee_peer/pubspec_overrides.yaml'
    if override.exists():
        transport = json.loads((transport_root / 'transport-provenance.json').read_text(encoding='utf-8'))
        if sha(transport_root / 'lib/src/core/transport.dart') != transport['transport_after_sha256']:
            raise ValueError('transport repair fingerprint mismatch')
        for name, expected in transport['reference_files'].items():
            if name != 'lib/src/core/transport.dart' and sha(transport_root / name) != expected:
                raise ValueError('unexpected derivative SDK source change')
        metadata['sdk_transport_repair'] = transport
        metadata['dependency_override_sha256'] = sha(override)
    probe_root = BUILD / 'flutter-webrtc-frame-probe-v1'
    if override.exists() and 'flutter-webrtc-frame-probe-v1' in override.read_text():
        probe = json.loads((probe_root / 'frame-probe-provenance.json').read_text())
        for name, expected in {**probe['reference_files'], **probe['patched_files']}.items():
            if sha(probe_root / name) != expected: raise ValueError('frame probe source mismatch')
        metadata['memory_frame_probe'] = probe
        metadata['plugin_dll_sha256'] = sha(destination / 'flutter_webrtc_plugin.dll')
        metadata['camera_sample_dart_sha256'] = sha(ROOT / 'tests/runtime/tools/media/flutter_e2ee_peer/lib/camera_correspondence.dart')
    (destination / "backend-provenance.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    (BUILD / "flutter-repaired-current.json").write_text(json.dumps({"directory": destination.name}), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage-runtime", action="store_true")
    parser.add_argument("--refresh-crypto", type=Path)
    parser.add_argument("--repair-window-focus", action="store_true")
    args = parser.parse_args()
    if args.refresh_crypto: refresh_crypto(args.refresh_crypto)
    elif args.repair_window_focus: repair_window_focus()
    elif args.stage_runtime: stage_runtime()
    else: main()
