"""Bounded maintenance gate. Retain the candidate only after every case passes."""
import json
from pathlib import Path
import subprocess
import sys
# Explicit sibling-domain dependency; keep direct script execution supported.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "product_acceptance"))

from product_aliyun_transport import execute


def main():
    root = Path('out/screen-share-quality/s0-server-v7-retry2-gate')
    root.mkdir(exist_ok=False)
    helper = '/root/livekit-quality-candidate-20260929/guarded_quality_rollout_v7-retry2.py'
    rollout = json.loads(execute('python3 ' + helper + ' apply', timeout=55))
    (root / 'apply.json').write_text(json.dumps(rollout, indent=2))
    if rollout.get('status') != 'VALIDATING':
        print('MAINTENANCE_NOT_STARTED', rollout.get('status'), flush=True)
        return 2
    accepted = False
    try:
        for name, args in [
            ('backup', ['--codecs', 'vp9', '--simulcast', '--production', '--backup']),
            ('simulcast', ['--codecs', 'vp8', 'h264', '--simulcast', '--production']),
            ('single', ['--codecs', 'vp8', 'h264', 'vp9', 'av1', '--production']),
            ('camera', ['--codecs', 'vp8', 'h264', '--camera']),
            ('weak', ['--codecs', 'vp8', '--simulcast', '--production', '--weak']),
        ]:
            print('START', name, flush=True)
            with (root / (name + '.log')).open('w', encoding='utf-8') as log:
                result = subprocess.run([sys.executable, 'tests/runtime/tools/screen_capture/invoke_screen_share_quality_probe.py',
                    '--binary', 'out/build/windows-vs2026-dev/Debug/test_screen_share_quality_runtime.exe',
                    '--output', str(root / name), *args], stdout=log, stderr=subprocess.STDOUT, timeout=650)
            print('RESULT', name, result.returncode, flush=True)
            if result.returncode:
                return 1
        status = json.loads(execute('python3 ' + helper + ' accept'))
        (root / 'accept.json').write_text(json.dumps(status, indent=2))
        accepted = status.get('status') == 'ACCEPTED'
        return 0 if accepted else 1
    finally:
        if not accepted:
            execute('python3 ' + helper + ' rollback', timeout=55)
            # A successful command alone is not proof of service recovery.
            code = "import json,pathlib,subprocess; s=json.loads(pathlib.Path('/root/livekit-quality-candidate-20260929/rollout-v7-retry2.json').read_text()); d=json.loads(subprocess.check_output(['docker','inspect','livekit']))[0]; assert s['phase']=='rolled_back' and d['Id']==s['original_id'] and d['State']['Running']; print(json.dumps(dict(status='ROLLED_BACK',original_id=d['Id'])))"
            restored = execute("python3 - <<'PY'\n" + code + "\nPY")
            (root / 'rollback.json').write_text(restored)
            print('ROLLED_BACK_VERIFIED', flush=True)


if __name__ == '__main__':
    raise SystemExit(main())
