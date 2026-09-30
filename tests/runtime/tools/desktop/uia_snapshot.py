"""One-shot UIA reads; the caller never loads UIA or retains provider references.

Use native probes and OS counters for routine samples. UI reads are explicit
checkpoints using exact (possibly qualified) AutomationIds, never full trees.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

WORKER = Path(__file__).with_name('uia_snapshot_worker.ps1')
MINIMUM_INTERVAL_SECONDS = 60
STATE_DIRECTORY = Path(tempfile.gettempdir()) / 'cohavora-uia-checkpoints'


@contextmanager
def _checkpoint_slot(pid, start_ticks, executable):
    # Shared by CLI invocations as well as Python callers. IDs are deliberately
    # excluded from the key so changing the requested control cannot poll faster.
    identity = f'{pid}:{start_ticks}:{os.path.normcase(str(executable))}'
    key = hashlib.sha256(identity.encode('utf-8')).hexdigest()
    STATE_DIRECTORY.mkdir(parents=True, exist_ok=True)
    fd = os.open(STATE_DIRECTORY / (key + '.state'), os.O_CREAT | os.O_RDWR, 0o600)
    with os.fdopen(fd, 'r+b') as state:
        if os.name == 'nt':
            import msvcrt
            lock = lambda: msvcrt.locking(state.fileno(), msvcrt.LK_NBLCK, 1)
            unlock = lambda: msvcrt.locking(state.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            lock = lambda: fcntl.flock(state, fcntl.LOCK_EX | fcntl.LOCK_NB)
            unlock = lambda: fcntl.flock(state, fcntl.LOCK_UN)
        try:
            lock()
        except OSError:
            raise RuntimeError('UIA checkpoint already active for this product') from None
        try:
            state.seek(1)
            previous = state.read().decode('ascii')
            now = time.monotonic()
            if previous and now - float(previous) < MINIMUM_INTERVAL_SECONDS:
                raise RuntimeError('UIA display checks require at least 60 seconds between attempts; use native probes for routine sampling')
            state.seek(0)
            state.write(b'\0' + str(now).encode('ascii'))
            state.truncate()
            state.flush()
            # Hold the lock through child exit. Failed attempts also consume the
            # interval; do not turn retries or concurrent callers into polling.
            yield
        finally:
            state.seek(0)
            unlock()


def snapshot(pid, start_ticks, executable, automation_ids, timeout=15):
    ids = list(automation_ids)
    if not 1 <= timeout <= 30 or pid <= 0 or start_ticks <= 0:
        raise ValueError('invalid identity or timeout')
    if not 1 <= len(ids) <= 16 or len(set(ids)) != len(ids) or any(
        not isinstance(i, str) or not i.strip() or len(i) > 512 for i in ids
    ):
        raise ValueError('provide 1..16 unique exact AutomationIds')
    request = dict(request_id=uuid.uuid4().hex, product_pid=pid,
                   start_ticks=start_ticks, executable=str(Path(executable).resolve()),
                   automation_ids=ids)
    with _checkpoint_slot(pid, start_ticks, request['executable']), tempfile.TemporaryDirectory(prefix='uia-checkpoint-') as directory:
        root = Path(directory)
        (root/'request.json').write_text(json.dumps(request), encoding='utf-8')
        # File arguments avoid interpolating caller data into PowerShell code.
        with (root/'stderr.txt').open('wb') as errors:
            child = subprocess.Popen([
                'powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                '-File', str(WORKER), '-RequestFile', str(root/'request.json'),
                '-ResultFile', str(root/'result.json')], stdout=subprocess.DEVNULL,
                stderr=errors, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            try:
                code = child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                # This exact child belongs to us; never terminate the product.
                child.kill()
                child.wait()
                raise TimeoutError('UIA checkpoint timed out; owned client exited') from None
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait()
        if code:
            raise RuntimeError('UIA checkpoint failed; no automatic retry or tree fallback')
        result_path = root/'result.json'
        if result_path.stat().st_size > 256 * 1024:
            raise RuntimeError('UIA checkpoint result too large')
        result = json.loads(result_path.read_text(encoding='utf-8-sig'))
        if (result.get('request_id'), result.get('product_pid'), result.get('client_pid')) != (
            request['request_id'], pid, child.pid
        ) or [v.get('automation_id') for v in result.get('values', [])] != ids:
            raise RuntimeError('UIA checkpoint identity mismatch')
        result['client_exit_code'] = code
        return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--start-ticks', type=int, required=True)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--automation-id', action='append', required=True)
    parser.add_argument('--timeout', type=int, default=15)
    args = parser.parse_args()
    print(json.dumps(snapshot(args.pid, args.start_ticks, args.executable,
                              args.automation_id, args.timeout), ensure_ascii=False))
