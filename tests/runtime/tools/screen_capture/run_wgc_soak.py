"""Run the native same-process WGC soak and preserve independently checkable evidence."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from datetime import datetime, timezone


def save(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2), encoding='utf-8')
    temporary.replace(path)


def records(log, tag):
    prefix = '[' + tag + '] '
    return [json.loads(line[len(prefix):]) for line in log.splitlines()
            if line.startswith(prefix) and line.endswith('}')]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=3600)
    parser.add_argument('--cycles', type=int, default=100)
    args = parser.parse_args()
    if not 3600 <= args.seconds <= 86400 or not 100 <= args.cycles <= 1000:
        parser.error('seconds must be 3600..86400 and cycles 100..1000')
    root = Path(__file__).resolve().parents[4]
    exe = args.exe.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    paths = [exe, exe.with_suffix('.pdb'), Path(__file__).resolve(),
             root / 'tests/runtime/probes/test_desktop_capture_runtime.cpp',
             root / 'src/media/desktop_capture.cpp',
             root / 'src/media/wgc_window_capture.cpp',
             root / 'src/media/wgc_window_capture.h', root / 'CMakeLists.txt']
    hashes = {str(p): sha(p) for p in paths}
    env = dict(os.environ, LIVEKIT_TEST_SHARE_CYCLES=str(args.cycles),
               LIVEKIT_TEST_SHARE_SOAK_SECONDS=str(args.seconds))
    started = time.monotonic()
    status = dict(status='STARTING', runner_pid=os.getpid(), started_utc=datetime.now(timezone.utc).isoformat(),
                  seconds=args.seconds, cycles=args.cycles, hashes=hashes)
    save(output / 'provenance.json', status)
    log_path = output / 'capture.log'
    process = None
    try:
        with log_path.open('w', encoding='utf-8') as stdout, (output / 'stderr.log').open('w') as stderr:
            process = subprocess.Popen([str(exe), '--external-wgc-window-soak'],
                                       stdout=stdout, stderr=stderr, env=env, cwd=root)
            status.update(status='RUNNING', pid=process.pid)
            while process.poll() is None:
                elapsed = time.monotonic() - started
                samples = records(log_path.read_text(encoding='utf-8', errors='replace'), 'CAPTURE_LIFECYCLE')
                status.update(elapsed_seconds=elapsed, latest_sample=samples[-1] if samples else None)
                save(output / 'status.json', status)
                if elapsed > args.seconds + 600:
                    raise TimeoutError('Native soak exceeded duration plus 600 seconds')
                time.sleep(10)
        log = log_path.read_text(encoding='utf-8', errors='replace')
        samples = records(log, 'CAPTURE_LIFECYCLE')
        probes = records(log, 'CAPTURE_PROBE')
        checks = records(log, 'CAPTURE_SOAK_CHECK')
        stability = records(log, 'CAPTURE_STABILITY')
        expected = set(range(1, args.cycles + 1))
        phase_counts = {str(c): dict(collections.Counter(p['phase'] for p in probes if p['cycle'] == c))
                        for c in expected}
        stopped = [s for s in samples if s['phase'] == 'stopped']
        joined = [s for s in samples if s['phase'] == 'joined']
        tail = [s for s in samples if s['phase'] == 'post_stop_process_alive']
        baseline = [s for s in samples if s['phase'] == 'baseline']
        progress = records(log, 'CAPTURE_PROGRESS')
        gates = {
            'exit_zero': process.returncode == 0,
            'duration': bool(baseline and stopped) and
                        stopped[-1]['epoch_ms'] - baseline[0]['epoch_ms'] >= args.seconds * 1000,
            'active_progress': {s['cycle'] for s in progress} == expected,
            'single_capture_process': {s['pid'] for s in samples} == {process.pid},
            'stopped_cycles': len(stopped) == args.cycles and {s['cycle'] for s in stopped} == expected,
            'joined_cycles': len(joined) == args.cycles and {s['cycle'] for s in joined} == expected,
            'per_cycle_resources': len(checks) == args.cycles and {s['cycle'] for s in checks} == expected
                                   and all(s['status'] == 'PASS' for s in checks),
            'lifecycle_release': all(all(counts.get(p) == 1 for p in
                ('session_closed', 'frame_pool_closed', 'd3d_released', 'destroyed', 'joined'))
                for counts in phase_counts.values()),
            'stability': len(stability) == 1 and stability[0]['status'] == 'PASS',
            'tail_alive': len(tail) == 6,
            'native_functional': 'DESKTOP_CAPTURE_RUNTIME PASS' in log,
            'hashes_unchanged': all(p.exists() and sha(p) == hashes[str(p)] for p in paths),
        }
        status.update(status='PASS' if all(gates.values()) else 'FAIL', gates=gates,
                      stderr=(output / 'stderr.log').read_text(encoding='utf-8', errors='replace'),
                      exit_code=process.returncode, elapsed_seconds=time.monotonic() - started,
                      phase_counts=phase_counts, stability=stability,
                      first_stopped=stopped[0] if stopped else None,
                      last_stopped=stopped[-1] if stopped else None,
                      final_live=tail[-1] if tail else None)
    except Exception as error:
        status.update(status='FAIL', error=repr(error))
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        status['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save(output / 'result.json', status)
        save(output / 'status.json', status)
    return 0 if status['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
