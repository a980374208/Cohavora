"""Copy the resolved Flutter plugin into a task-owned memory-only frame probe.

No external SDK/cache files are edited. Retain a complete source fingerprint.
"""
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[4]
FIXTURE = ROOT / 'tests/runtime/tools/media/flutter_e2ee_peer'
DESTINATION = ROOT / 'build-e2ee-backend/flutter-webrtc-frame-probe-v1'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    config = json.loads((FIXTURE / '.dart_tool/package_config.json').read_text(encoding='utf-8'))
    from urllib.parse import unquote, urlparse
    uri = next(p['rootUri'] for p in config['packages'] if p['name'] == 'flutter_webrtc')
    if not uri.startswith('file:///'): raise ValueError('expected original absolute plugin package')
    source = Path(unquote(urlparse(uri).path).lstrip('/')).resolve()
    if not source.is_dir() or DESTINATION.exists(): raise ValueError('inspect plugin source/destination first')
    shutil.copytree(source, DESTINATION,
        ignore=shutil.ignore_patterns('.git', '.dart_tool', 'build', 'example', 'test', 'test_driver'))
    original = {str(p.relative_to(DESTINATION)).replace('\\', '/'): sha(p)
        for p in DESTINATION.rglob('*') if p.is_file()}
    target = DESTINATION / 'common/cpp/src/flutter_webrtc.cc'
    text = target.read_text(encoding='utf-8')
    text = '#include "e2ee_flutter_frame_sample.h"\n' + text
    anchor = '  if (method_call.method_name().compare("initialize") == 0) {'
    if text.count(anchor) != 1: raise ValueError('unexpected method-dispatch source')
    text = text.replace(anchor, '''  if (method_call.method_name() == "e2eeMemoryFrameSample") {
    if (!method_call.arguments()) {
      result->Error("invalid_arguments", "Track required"); return;
    }
    const auto params = GetValue<EncodableMap>(*method_call.arguments());
    auto track = MediaTrackForId(findString(params, "trackId"));
    if (!track || track->kind().std_string() != "video") {
      result->Error("invalid_track", "Video track required"); return;
    }
    memory_frame_worker_.Start(static_cast<RTCVideoTrack*>(track.get()), std::move(result));
    return;
  }
''' + anchor)
    target.write_text(text, encoding='utf-8')
    changed = {'common/cpp/src/flutter_webrtc.cc': sha(target)}
    for name in ('e2ee_flutter_frame_sample.h', 'e2ee_frame_signature.h'):
        dest = DESTINATION / 'common/cpp/include' / name
        shutil.copy2(ROOT / 'tests/runtime/probes' / name, dest)
        changed[str(dest.relative_to(DESTINATION)).replace('\\', '/')] = sha(dest)
    plugin_header = DESTINATION / 'common/cpp/include/flutter_webrtc.h'
    header = plugin_header.read_text(encoding='utf-8').replace('#include "flutter_common.h"', '#include "flutter_common.h"\n#include "e2ee_flutter_frame_sample.h"').replace('  void initLoggerCallback', '  E2eeMemoryFrameWorker memory_frame_worker_;\n  void initLoggerCallback')
    plugin_header.write_text(header, encoding='utf-8')
    changed['common/cpp/include/flutter_webrtc.h'] = sha(plugin_header)
    manifest = {'scope': 'Task-owned Windows plugin with bounded in-memory luma summary method; no image files',
        'reference_directory': str(source), 'reference_files': original, 'patched_files': changed,
        'generator_sha256': sha(Path(__file__))}
    (DESTINATION / 'frame-probe-provenance.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    override = FIXTURE / 'pubspec_overrides.yaml'
    existing = override.read_text(encoding='utf-8')
    if 'flutter_webrtc:' in existing: raise ValueError('existing plugin override requires review')
    override.write_text(existing.rstrip() + '\n  flutter_webrtc:\n    path: ../../../../../build-e2ee-backend/flutter-webrtc-frame-probe-v1\n', encoding='utf-8')
    print(json.dumps({'status': 'prepared', 'directory': str(DESTINATION), 'copied_files': len(original)}))


if __name__ == '__main__':
    main()
