"""Real Windows capture -> ScreenShareSession -> ECS -> independent receiver."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import queue
import threading
import time
import uuid

# Explicit sibling-domain dependency; keep direct script execution supported.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "product_acceptance"))

from invoke_screen_share_quality_probe import credentials
from product_aliyun_transport import execute


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    parser.add_argument('--binary', type=Path, default=Path(
        'out/build/windows-vs2026-dev/RelWithDebInfo/test_screen_share_runtime.exe'))
    parser.add_argument('--check-binary-only', action='store_true')
    parser.add_argument('--gdi', action='store_true')
    parser.add_argument('--collect-fps-failures', action='store_true')
    parser.add_argument('--soak-seconds', type=int, default=0, choices=[0, 1800])
    args = parser.parse_args()
    if args.collect_fps_failures and (not args.gdi or args.soak_seconds):
        parser.error('--collect-fps-failures is only a GDI diagnostic; FPS failures still fail the run')
    if not args.check_binary_only and args.output is None:
        parser.error('--output is required for a runtime run')

    verifier = Path(__file__).resolve().parents[1] / 'diagnostics' / 'verify_runtime_binary.ps1'
    check = subprocess.run(['pwsh', '-NoProfile', '-File', str(verifier),
                            '-Executable', str(args.binary.resolve()),
                            '-ExpectedExecutableName', 'test_screen_share_runtime.exe',
                            '-Configuration', 'RelWithDebInfo'],
                           capture_output=True, text=True,
                           creationflags=subprocess.CREATE_NO_WINDOW)
    if check.returncode:
        messages = (line.split('|', 1)[1].strip() for line in check.stderr.splitlines()
                    if line.lstrip().startswith('| '))
        detail = next((line for line in messages if line and not line.startswith('~')),
                      'unknown error')
        parser.exit(2, f'RelWithDebInfo binary verification failed: {detail}\n')
    binary_identity = json.loads(check.stdout)
    if args.check_binary_only:
        print(json.dumps(binary_identity))
        return 0

    args.output.mkdir(parents=True, exist_ok=False)
    binary = Path(binary_identity['binary_path'])
    room = 'quality-hot-' + uuid.uuid4().hex[:12]
    auth = credentials(room)
    env = os.environ.copy()
    # This probe shares only PatternWindow, a dedicated generated black/white
    # fixture. Never inherit the older probe's full-desktop capture switch.
    for key in ('LIVEKIT_TEST_WGC_SCREEN', 'LIVEKIT_TEST_GDI_WINDOW', 'LIVEKIT_TEST_FIRST_FRAME_ONLY',
                'LIVEKIT_TEST_DIAG_ROOT', 'LIVEKIT_TEST_MEDIA_DIAGNOSTICS'):
        env.pop(key, None)
    env.update(LIVEKIT_URL='ws://123.56.225.164:17880', LIVEKIT_TOKEN=auth['publisher'],
               LIVEKIT_PEER_TOKEN=auth['receiver'], LIVEKIT_TEST_ALLOW_INSECURE='1',
               LIVEKIT_TEST_QUALITY_HOT='1')
    if args.gdi:
        env['LIVEKIT_TEST_GDI_WINDOW'] = '1'
    env.pop('LIVEKIT_TEST_COLLECT_FPS_FAILURES', None)
    if args.collect_fps_failures:
        env['LIVEKIT_TEST_COLLECT_FPS_FAILURES'] = '1'
    env.pop('LIVEKIT_TEST_QUALITY_SOAK_SECONDS', None)
    if args.soak_seconds:
        env['LIVEKIT_TEST_QUALITY_SOAK_SECONDS'] = str(args.soak_seconds)
    result = dict(status='RUNNING', started_utc=datetime.now(timezone.utc).isoformat(),
                  run_id=room, gdi=args.gdi, soak_seconds=args.soak_seconds,
                  collect_fps_failures=args.collect_fps_failures,
                  resource_input_kind='generated_window_harness_phase_not_uia',
                  binary_sha256=binary_identity['binary_sha256'],
                  binary_identity=binary_identity,
                  server=execute("docker inspect --format '{{.Image}}' livekit && docker exec livekit sha256sum /livekit-server"),
                  inputs={p: hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in [
                      'src/core/room.cpp', 'src/core/screen_share_session.cpp', 'src/media/desktop_capture.cpp',
                      'src/core/local_video_track.cpp', 'src/rtc/webrtc_manager.cpp',
                      'tests/runtime/probes/test_screen_share_runtime.cpp']})
    manifest = args.output / 'summary.json'
    manifest.write_text(json.dumps(result, indent=2), encoding='utf-8')
    if hashlib.sha256(binary.read_bytes()).hexdigest() != result['binary_sha256']:
        result['status'] = 'FAIL'
        result['failure_reason'] = 'binary_changed_before_launch'
        result['finished_utc'] = datetime.now(timezone.utc).isoformat()
        manifest.write_text(json.dumps(result, indent=2), encoding='utf-8')
        parser.exit(2, 'Runtime harness changed after binary verification\n')
    child = subprocess.Popen([str(binary.resolve())], env=env, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    # Reuse the existing process/WDDM sampler. Its action-file input is a
    # harness phase record here, not evidence of UI automation.
    actions = args.output / 'uia-actions.jsonl'
    def phase(value):
        with actions.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(dict(run_id=room, pid=child.pid, cycle=0,
                operation_id='quality-runtime', action='generated_window_quality', phase=value)) + '\n')
    phase('starting')
    sampler = subprocess.Popen(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', 'tests/runtime/tools/product_acceptance/product_pilot_resources.ps1', '-UiaDirectory', str(args.output.resolve()),
        '-Destination', str((args.output / 'resources.jsonl').resolve()), '-RunId', room,
        '-MaximumSeconds', str(args.soak_seconds + 720)], stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
    allowed = ('[QUALITY_RUNTIME] ', '[CAPTURE_PROBE] ', '[LIFECYCLE] ', '[RESULT] ', '[FAILURE] ',
               '[SCREEN_DELIVERY_STATE] ', '[REMOTE_FRAME] ')
    pending = queue.Queue()
    def reader():
        for raw in child.stdout:
            line = raw.decode('utf-8', errors='replace').strip()
            if line.startswith(allowed): pending.put(line)
        pending.put(None)
    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    lines = []
    deadline = time.monotonic() + args.soak_seconds + 700
    try:
        with (args.output / 'events.log').open('w', encoding='utf-8', buffering=1) as stream:
            while time.monotonic() < deadline:
                try: line = pending.get(timeout=1)
                except queue.Empty: continue
                if line is None: break
                lines.append(line)
                stream.write(line + '\n')
                if line.startswith('[QUALITY_RUNTIME] '):
                    record = json.loads(line.split(' ', 1)[1])
                    if record.get('event') == 'soak_start': phase('soak')
                    elif 'stage' in record: phase('switching')
                    elif record.get('status') == 'PASS' and 'switches' in record: phase('released')
            else:
                result['timeout'] = True
                child.kill()
        child.wait(timeout=15)
    finally:
        if child.poll() is None: child.kill(); child.wait(timeout=10)
        child.stdout.close()
        try: sampler.wait(timeout=15)
        except subprocess.TimeoutExpired: sampler.terminate(); sampler.wait(timeout=10)
        result['resource_sampler_exit'] = sampler.returncode
    result['exit_code'] = child.returncode
    result['binary_sha256_after'] = hashlib.sha256(binary.read_bytes()).hexdigest()
    result['binary_unchanged'] = result['binary_sha256_after'] == result['binary_sha256']
    result['status'] = ('PASS' if child.returncode == 0 and result['binary_unchanged']
                        and '[RESULT] SCREEN_SHARE_L3 PASS' in lines else 'FAIL')
    result['finished_utc'] = datetime.now(timezone.utc).isoformat()
    manifest.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(result['status'])
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
