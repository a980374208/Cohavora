"""Predeclared three-product Off/On comparison; no credentials in artifacts."""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import time
import uuid

from invoke_e2ee_interop import ROOT, sha, read_events

BUDGET = {'cpu_percentage_points': 10.0, 'private_memory_mib': 128.0, 'decoded_fps_loss_percent': 10.0}
PEERS = {'publisher': 'product.jsonl', 'receiver': 'receiver/product.jsonl', 'late': 'late/product.jsonl'}


GAP_BOUNDS_US = [5000, 10000, 15000, 20000, 30000, 50000, 100000, 250000, 1000000]


def summarize_audio(rows, start, end):
    tracks = {}
    for event in rows:
        if event['event'] == 'product_pcm' and start <= event['time_ms'] <= end:
            tracks.setdefault(event['track'], []).append(event)
    if not tracks: raise ValueError('missing_actual_pcm')
    bins = [0] * (len(GAP_BOUNDS_US) + 1)
    frames = 0
    for samples in tracks.values():
        if len(samples) < 2 or samples[-1]['time_ms'] - samples[0]['time_ms'] < 40000:
            raise ValueError('incomplete_pcm_window')
        first, last = samples[0], samples[-1]
        if any(e['invalid_frames'] or e['clock_order_errors'] for e in samples):
            raise ValueError('invalid_pcm_observation')
        if last['samples'] <= first['samples'] or last['frames'] <= first['frames']:
            raise ValueError('no_pcm_progress')
        if len(first['gap_bins_us']) != len(bins) or len(last['gap_bins_us']) != len(bins):
            raise ValueError('invalid_pcm_histogram')
        delta = [b - a for a, b in zip(first['gap_bins_us'], last['gap_bins_us'])]
        if any(value < 0 for value in delta): raise ValueError('reset_pcm_counter')
        bins = [a + b for a, b in zip(bins, delta)]
        frames += last['frames'] - first['frames']
    count = sum(bins)
    if not count: raise ValueError('empty_pcm_intervals')
    cutoff = math.ceil(count * .95)
    cumulative = 0
    for index, value in enumerate(bins):
        cumulative += value
        if cumulative >= cutoff:
            if index == len(GAP_BOUNDS_US): raise ValueError('pcm_interval_outside_measurement_range')
            return {'pcm_gap_p95_upper_ms': GAP_BOUNDS_US[index] / 1000,
                    'pcm_interval_count': count, 'pcm_intervals_over_100ms': sum(bins[7:]),
                    'pcm_frames': frames, 'pcm_tracks': len(tracks)}
    raise ValueError('incomplete_pcm_distribution')


def observation_window(events):
    start = max(next(e['time_ms'] for e in rows if e['event'] == 'product_state' and e.get('state') == 5)
                for rows in events.values()) + 20000
    end = min(next(e['time_ms'] for e in rows if e['event'] == 'product_leave_requested')
              for rows in events.values())
    if end - start < 600000: raise ValueError('less_than_ten_common_minutes')
    # Fail if an observed audio/video stream stalls in any complete ten-second
    # portion; aggregate first/last counts alone cannot prove continuity.
    for rows in events.values():
        for begin in range(int(start), int(start) + 600000, 10000):
            window = [e for e in rows if begin <= e.get('time_ms', -1) < begin + 10000]
            for kind, count in [('product_pcm', 'samples'), ('product_rtp', 'framesDecoded')]:
                by_track = {}
                for event in window:
                    if event['event'] != kind: continue
                    if kind == 'product_rtp' and (event.get('direction') != 'rx' or event.get('kind') != 'video'): continue
                    by_track.setdefault(event['track'], []).append(event[count])
                if len(by_track) < 2 or any(len(values) < 2 or values[-1] <= values[0] for values in by_track.values()):
                    raise ValueError('ten_minute_media_stall_or_missing_stream')
    return {'warmup_seconds_after_last_join': 20, 'common_available_seconds': (end - start) / 1000,
            'verified_seconds': 600, 'continuity_windows_per_peer': 60}


def summarize(run):
    events = {role: read_events(run / file) for role, file in PEERS.items()}
    # A common window excludes every peer's startup and late-join work.
    start = max(next(e['time_ms'] for e in rows if e['event'] == 'product_state' and e.get('state') == 5)
                for rows in events.values()) + 20000
    end = start + 45000
    result = {}
    for role, rows in events.items():
        metrics = [e for e in rows if e['event'] == 'product_metrics' and start <= e['time_ms'] <= end]
        if len(metrics) < 15 or metrics[-1]['time_ms'] - metrics[0]['time_ms'] < 40000:
            raise ValueError('insufficient_steady_window')
        if any(e.get('audioJitterBufferAvailability') != 'VALID' for e in metrics):
            raise ValueError('unavailable_audio_buffer_delay')
        if any(e.get('cpuAvailability') != 'VALID' or e.get('memoryAvailability') != 'VALID' for e in metrics):
            raise ValueError('unavailable_resource_metric')
        def p95(name):
            values = sorted(float(e[name]) for e in metrics)
            return values[min(len(values) - 1, int(len(values) * .95))]
        frames = metrics[-1]['inboundVideoFramesDecoded'] - metrics[0]['inboundVideoFramesDecoded']
        elapsed = (metrics[-1]['time_ms'] - metrics[0]['time_ms']) / 1000
        if frames <= 0: raise ValueError('no_decoded_progress')
        result[role] = {'cpu_p95': p95('processCpuPercent'), 'private_mib_p95': p95('privateBytes') / 1048576,
                        'decoded_fps': frames / elapsed, 'samples': len(metrics), 'window_seconds': elapsed,
                        'audio_jitter_buffer_delay_ms_p95': p95('audioJitterBufferDelayMs'),
                        **summarize_audio(rows, start, end)}
    return result


def compare(trials):
    result = {}
    for role in PEERS:
        on = [trial['metrics'][role] for trial in trials if trial['mode'] == 'on']
        off = [trial['metrics'][role] for trial in trials if trial['mode'] == 'off']
        if len(on) != 2 or len(off) != 2: raise ValueError('incomplete_abba')
        def mean(group, name): return statistics.mean(item[name] for item in group)
        cpu = mean(on, 'cpu_p95') - mean(off, 'cpu_p95')
        memory = mean(on, 'private_mib_p95') - mean(off, 'private_mib_p95')
        baseline = mean(off, 'decoded_fps')
        if baseline <= 0: raise ValueError('invalid_off_decode_baseline')
        fps_loss = (1 - mean(on, 'decoded_fps') / baseline) * 100
        result[role] = {'cpu_delta_pp': cpu, 'private_memory_delta_mib': memory, 'decoded_fps_loss_percent': fps_loss,
            'observations': {'pcm_gap_p95_upper_delta_ms': mean(on, 'pcm_gap_p95_upper_ms') - mean(off, 'pcm_gap_p95_upper_ms'),
                'audio_jitter_buffer_delay_p95_delta_ms': mean(on, 'audio_jitter_buffer_delay_ms_p95') - mean(off, 'audio_jitter_buffer_delay_ms_p95')},
            'checks': {'cpu': cpu <= BUDGET['cpu_percentage_points'],
                'memory': memory <= BUDGET['private_memory_mib'], 'fps': fps_loss <= BUDGET['decoded_fps_loss_percent']}}
    return result


def main(observe=False):
    base = ROOT / 'docs/analysis/e2ee/evidence/e0-product-20261001'
    output = base / ('performance-' + uuid.uuid4().hex[:16]); output.mkdir()
    launcher = Path(__file__).with_name('invoke_e2ee_product.py')
    result = {'status': 'RUNNING', 'budget_approved_before_measurement': BUDGET,
        'order': ['off', 'on', 'on', 'off'], 'trial_seconds': 100,
        'warmup_seconds_after_last_join': 20, 'steady_window_seconds': 45,
        'statistic': 'mean of two trial p95 CPU/private-memory values; mean decoded frame rates',
        'pcm_gap_histogram_bounds_us': GAP_BOUNDS_US,
        'audio_observation_scope': 'actual decoded PCM callback interval histogram p95 upper bound; telemetry jitter-buffer delay p95; not mouth-to-ear latency or an approved audio-quality budget',
        'load': 'three RelWithDebInfo native products, H264 window share+microphone, initial chat/file/whiteboard; third late join',
        'excludes': ['camera load', 'synthetic source process CPU', 'long-term stability', 'latency acceptance', 'final E2EE completion'],
        'tool_sha256': sha(Path(__file__)), 'launcher_sha256': sha(launcher), 'trials': []}
    def save():
        temp = output / 'result.tmp'; temp.write_text(json.dumps(result, indent=2), encoding='utf-8')
        temp.replace(output / 'result.json')
    def run(mode, duration):
        cmd = [sys.executable, str(launcher), '--native-pair', '--whiteboard', '--late-product', '--steady', '--duration', str(duration)]
        if mode == 'off': cmd.append('--off')
        child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        try:
            stdout, stderr = child.communicate(timeout=duration + 100)
        except subprocess.TimeoutExpired:
            child.terminate(); child.communicate(timeout=20)
            raise RuntimeError('trial_timeout')
        # Print only structured fixture output; never arbitrary subprocess stderr.
        records = [json.loads(line) for line in stdout.splitlines() if line.startswith('{')]
        terminal = next((e for e in reversed(records) if 'status' in e), None)
        if not terminal: raise RuntimeError('missing_trial_verdict')
        print(json.dumps(terminal), flush=True)
        trial = Path(terminal['evidence'])
        manifest = json.loads((trial / 'result.json').read_text(encoding='utf-8'))
        result['trials'].append({'mode': mode, 'duration': duration, 'evidence': str(trial.relative_to(ROOT)),
            'verdict_sha256': sha(trial / 'result.json'), 'status': manifest['status']})
        save()
        if child.returncode or manifest['status'] != 'PASS': raise RuntimeError('functional_gate_failed')
        return trial
    save(); print(json.dumps({'event': 'performance_started', 'evidence': str(output)}), flush=True)
    try:
        for mode in result['order']:
            trial = run(mode, 100)
            result['trials'][-1]['metrics'] = summarize(trial); save()
        result['comparison'] = compare(result['trials'])
        result['performance_status'] = 'PASS' if all(all(v['checks'].values()) for v in result['comparison'].values()) else 'FAIL'
        if observe:
            observation = run('on', 640)
            observed = observation_window({role: read_events(observation / file) for role, file in PEERS.items()})
            result['observation'] = {'status': 'PASS', 'configured_seconds': 640, **observed,
                'evidence': str(observation.relative_to(ROOT)),
                'scope': '600 common seconds after warmup with per-track ten-second progress; no long-soak or 100-cycle claim'}
        result['status'] = result['performance_status']; save()
    except Exception as error:
        result.update(status='NEEDS_FIX', error_type=type(error).__name__); save()
    print(json.dumps({'status': result['status'], 'evidence': str(output)}), flush=True)
    return result['status'] == 'PASS'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--observe-ten-minutes', action='store_true')
    sys.exit(0 if main(parser.parse_args().observe_ten_minutes) else 1)
