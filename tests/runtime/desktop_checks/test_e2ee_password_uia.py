"""One external UIA checkpoint on a password field containing a PUBLIC marker."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/runtime/tools/media'))
from invoke_e2ee_interop import InteractiveFixtureProcess


def run():
    output = ROOT / 'docs/analysis/e2ee/evidence/e0-product-20261001' / ('uia-password-' + uuid.uuid4().hex[:16])
    output.mkdir()
    native = ROOT / 'build-debug/RelWithDebInfo/test_main_window_latency.exe'
    worker = ROOT / 'tests/runtime/tools/desktop/e2ee_password_uia_worker.ps1'
    ready = output / 'fixture.json'
    result_file = output / 'uia.json'
    result = {'status': 'NOT_RUN', 'configuration': 'RelWithDebInfo',
        'scope': 'one external Windows UIA password checkpoint; public marker only',
        'sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (native, worker, Path(__file__), ROOT / 'src/ui/meeting_encryption_dialog.h',
                      ROOT / 'tests/ui/test_main_window_latency.cpp')}}
    fixture = probe = None
    try:
        fixture = InteractiveFixtureProcess([str(native), '--e2ee-uia-fixture'], cwd=native.parent,
            env=dict(os.environ, E2EE_UIA_READY=str(ready)))
        deadline = time.monotonic() + 10
        identity = None
        while time.monotonic() < deadline and fixture.poll() is None:
            if ready.exists():
                try: identity = json.loads(ready.read_text(encoding='utf-8'))
                except json.JSONDecodeError: pass  # Only fixture file publication, never a UIA retry.
                if identity: break
            time.sleep(.1)
        if not identity or identity.get('pid') != fixture.pid: raise RuntimeError('fixture_identity')
        probe = InteractiveFixtureProcess(['powershell.exe', '-NoProfile', '-NonInteractive',
            '-ExecutionPolicy', 'Bypass', '-File', str(worker), '-RequestFile', str(ready), '-ResultFile', str(result_file)])
        probe.wait(timeout=15)
        observation = json.loads(result_file.read_text(encoding='utf-8-sig'))
        result.update(observation=observation, worker_exit=probe.returncode)
        result['status'] = 'PASS' if probe.returncode == 0 and observation.get('status') == 'PASS' else 'FAIL'
    except Exception as error:
        result.update(status='FAIL', error_type=type(error).__name__)
    finally:
        for child in (probe, fixture):
            if child and child.poll() is None: child.terminate(); child.wait(timeout=10)
        (output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'status': result['status'], 'evidence': str(output)}))
    return result['status'] == 'PASS'


if __name__ == '__main__': sys.exit(0 if run() else 1)
