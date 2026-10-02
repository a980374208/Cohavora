"""Task-owned animated source -> production capture -> ECS -> distinct Room receiver.

The two native Room clients run in one process; this is not cross-device evidence.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid

from invoke_screen_share_quality_probe import credentials, remote_python, REMOTE_AUTH

BACKENDS = ('dxgi-screen', 'wgc-screen', 'gdi-screen', 'wgc-window', 'gdi-window',
            'dxgi-screen-fallback-wgc', 'wgc-screen-fallback-gdi')
CASES = BACKENDS + ('complex-wgc-window', 'complex-gdi-window',
                    'cost-wgc-window', 'cost-gdi-window',
                    'complex-wgc-screen', 'cost-wgc-screen')
INPUTS = ('src/media/desktop_capture.cpp', 'src/media/desktop_capture.h',
          'src/media/screen_capture_fallback.h', 'src/media/screen_share_quality.h',
          'src/core/room.cpp', 'src/core/screen_share_session.cpp',
          'src/core/local_video_track.cpp', 'src/rtc/webrtc_manager.cpp',
          'tests/runtime/probes/test_screen_share_runtime.cpp',
          'tests/runtime/probes/share_quality_probe.h')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def cost_metrics(lines):
    perf = [json.loads(x.split(' ', 1)[1]) for x in lines if x.startswith('[SHARE_PERF] ')]
    if len(perf) != 1:
        raise ValueError('one_share_cost_summary_required')
    perf = perf[0]
    for key in ('baseline_seconds', 'active_seconds', 'baseline_cpu_core_percent',
                'active_cpu_core_percent', 'cpu_increment_core_percent', 'capture_fps', 'remote_fps'):
        if not isinstance(perf.get(key), (int, float)) or not math.isfinite(perf[key]):
            raise ValueError('invalid_cost_counter:' + key)
    if perf['baseline_seconds'] < 10 or perf['active_seconds'] < 30:
        raise ValueError('cost_window_incomplete')
    samples = [json.loads(x.split(' ', 1)[1]) for x in lines if x.startswith('[SHARE_RTC] ')]
    metrics = {}
    for role, streams, fields in (
            ('sender', 'outbound', ('frames_encoded', 'frames_sent', 'bytes_sent', 'packets_sent')),
            ('receiver', 'inbound', ('frames_decoded', 'frames_received', 'bytes_received', 'packets_received'))):
        rows = [x for x in samples if x['role'] == role]
        first = next(x for x in rows if x['phase'] == 'start')
        last = [x for x in rows if x['phase'] == 'end'][-1]
        if any(x['screen_sid'] != first['screen_sid'] or x['rtc_track_id'] != first['rtc_track_id']
               or x['successful_pc'] < 1 or x['timed_out_pc'] or x['rejected_pc'] for x in rows):
            raise ValueError('cost_track_or_stats_changed')
        seconds = (last['epoch_ms'] - first['epoch_ms']) / 1000
        if seconds < 30:
            raise ValueError('rtc_cost_window_incomplete')
        def selected(sample):
            if role == 'sender':
                mids = {x['mid'] for x in sample['senders'] if x['track_id'] == sample['rtc_track_id']}
                return [x for x in sample[streams] if x['mid'] is not None and x['mid'] in mids]
            return [x for x in sample[streams] if x['track_identifier'] == sample['rtc_track_id']]
        starts = {x['id']: x for x in selected(first)}
        ends = {x['id']: x for x in selected(last)}
        if not starts or starts.keys() != ends.keys():
            raise ValueError('screen_rtp_streams_missing_or_replaced:' + role)
        totals = dict.fromkeys(fields, 0)
        layers = []
        codecs = {x['id']: x['mime_type'] for x in last['codecs']}
        for sid, end in ends.items():
            delta = {}
            for field in fields:
                a, b = starts[sid][field], end[field]
                if a is None or b is None or b < a:
                    raise ValueError('unavailable_or_reset_rtp_counter:' + role + ':' + field)
                delta[field] = b - a
                totals[field] += b - a
            layers.append(dict(id=sid, rid=end.get('rid'), codec=codecs.get(end['codec_id']), delta=delta))
        if any(totals[x] <= 0 for x in fields):
            raise ValueError('screen_rtp_cost_no_progress:' + role)
        metrics[role] = dict(seconds=seconds, layers=layers, delta=totals,
            rates_per_second={key: value / seconds for key, value in totals.items()})
    return dict(process=perf, screen_rtp=metrics,
        boundary='CPU covers two native Rooms and the generated source in one process; RTP rates exclude protocol overhead. No cost budget is asserted.')


def snapshot(room):
    return remote_python(REMOTE_AUTH + '''
import urllib.request, urllib.error, subprocess
request=urllib.request.Request('http://127.0.0.1:17880/twirp/livekit.RoomService/ListParticipants',
    data=json.dumps(dict(room=ROOM)).encode(),headers={'Authorization':'Bearer '+token('capture-admin',
    dict(roomAdmin=True,room=ROOM)),'Content-Type':'application/json'})
try:participants=json.load(urllib.request.urlopen(request,timeout=10)).get('participants',[])
except urllib.error.HTTPError as error:
    if error.code==404:participants=[]
    else:raise
service=json.loads(subprocess.check_output(['docker','inspect','livekit']))[0]
print(json.dumps(dict(participants=len(participants),running=service['State']['Running'],
    container_id=service['Id'],image=service['Image'],
    binary=subprocess.check_output(['docker','exec','livekit','sha256sum','/livekit-server'],text=True).split()[0],
    config_sha256=hashlib.sha256(pathlib.Path(CONFIG_PATH).read_bytes()).hexdigest(),
    health=urllib.request.urlopen('http://127.0.0.1:17880',timeout=5).status)))
'''.replace('ROOM', repr(room)))


def run(binary, identity, root, name):
    directory = root / name
    directory.mkdir(exist_ok=False)
    room = 'capture-' + uuid.uuid4().hex
    result = dict(status='RUNNING', case=name, run_id=room,
        started_utc=datetime.now(timezone.utc).isoformat(), configuration='RelWithDebInfo',
        binary_identity=identity, inputs={p: digest(p) for p in INPUTS},
        runner_sha256=digest(__file__), receiver_topology='two_distinct_native_Rooms_in_one_process',
        head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip())
    manifest = directory / 'result.json'
    def save():
        manifest.write_text(json.dumps(result, indent=2), encoding='utf-8')
    save()
    child = None
    try:
        result['server_before'] = snapshot(room)
        auth = credentials(room)
        env = {k: v for k, v in os.environ.items() if not k.startswith('LIVEKIT_TEST_')}
        env.update(LIVEKIT_URL='ws://123.56.225.164:17880', LIVEKIT_TOKEN=auth['publisher'],
            LIVEKIT_PEER_TOKEN=auth['receiver'], LIVEKIT_TEST_ALLOW_INSECURE='1',
            LIVEKIT_TEST_CAPTURE_BACKEND=name.removeprefix('complex-').removeprefix('cost-'))
        args = [str(binary)]
        if name.startswith('complex-'):
            env['LIVEKIT_TEST_SHARE_QUALITY'] = '1'
            args.append('--performance-probe')
        elif name.startswith('cost-'):
            args.append('--performance-probe')
        if name in ('complex-wgc-screen', 'cost-wgc-screen'):
            env['LIVEKIT_TEST_WGC_SCREEN'] = '1'
        if digest(binary) != identity['binary_sha256']:
            raise RuntimeError('binary_changed_before_launch')
        child = subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        result['pid'] = child.pid
        pending = queue.Queue()
        allowed = ('[RESULT]', '[FAILURE]', '[BACKEND_RUNTIME]', '[CAPTURE_PROBE]',
                   '[REMOTE_FRAME]', '[SCREEN_DELIVERY_STATE]', '[CLEANUP_STATE]',
                   '[SHARE_QUALITY]', '[SHARE_QUALITY_SAMPLE]', '[SHARE_RTC]', '[SHARE_PERF]')
        def read():
            for raw in child.stdout:
                line = raw.decode('utf-8', errors='replace').strip()
                if line.startswith(allowed):
                    pending.put(line)
            pending.put(None)
        threading.Thread(target=read, daemon=True).start()
        lines = []
        deadline = time.monotonic() + 180
        with (directory / 'events.log').open('w', encoding='utf-8', buffering=1) as stream:
            while time.monotonic() < deadline:
                try:
                    line = pending.get(timeout=1)
                except queue.Empty:
                    continue
                if line is None:
                    break
                lines.append(line)
                stream.write(line + '\n')
            else:
                result['timeout'] = True
                child.kill()
        child.wait(timeout=15)
        result['exit_code'] = child.returncode
        events = [json.loads(x.split(' ', 1)[1]) for x in lines if x.startswith('[CAPTURE_PROBE] ')]
        result['capture_events'] = events
        result['server_after'] = snapshot(room)
        before, after = result['server_before'], result['server_after']
        result['checks'] = dict(native_gate=child.returncode == 0 and '[RESULT] SCREEN_SHARE_L3 PASS' in lines,
            binary_unchanged=digest(binary) == identity['binary_sha256'],
            stopped_joined=any(x['phase'] == 'joined' for x in events),
            destroyed=any(x['phase'] == 'destroyed' for x in events),
            no_close_failure=not any(x['phase'] == 'close_failed' for x in events),
            room_empty=after['participants'] == 0,
            service_unchanged=all(before[k] == after[k] for k in ('container_id','image','binary','config_sha256')),
            service_healthy=after['running'] and after['health'] == 200)
        if name.startswith('complex-'):
            result['quality'] = [json.loads(x.split(' ', 1)[1]) for x in lines
                                 if x.startswith('[SHARE_QUALITY] ')]
            result['checks']['quality_gate'] = (len(result['quality']) == 1 and
                                                result['quality'][0]['status'] == 'PASS')
        if name.startswith(('complex-', 'cost-')):
            path = [x['capturer_id'] for x in events if x['phase'] == 'backend_frame']
            result['checks']['requested_backend_used'] = path == [3 if 'gdi-' in name else 1]
        if name.startswith('cost-'):
            result['cost'] = cost_metrics(lines)
            result['checks']['measured_cost'] = True
            result['checks']['quality_profiler_disabled'] = not any(
                x.startswith('[SHARE_QUALITY') for x in lines)
        wgc = any(x['phase'] == 'backend_frame' and x['capturer_id'] == 1 for x in events)
        if wgc:
            result['checks']['wgc_released'] = all(any(x['phase'] == phase for x in events)
                for phase in ('session_closed', 'frame_pool_closed', 'd3d_released'))
        result['status'] = 'PASS' if all(result['checks'].values()) else 'FAIL'
    except Exception as error:
        result.update(status='FAIL', failure=type(error).__name__ + ':' + str(error))
    finally:
        if child is not None:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
            child.stdout.close()
        result['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(name + ' ' + result['status'], flush=True)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    binary = args.binary.resolve()
    verifier = Path(__file__).resolve().parents[1] / 'diagnostics' / 'verify_runtime_binary.ps1'
    check = subprocess.run(['pwsh','-NoProfile','-File',str(verifier),'-Executable',str(binary),
        '-ExpectedExecutableName','test_screen_share_runtime.exe','-Configuration','RelWithDebInfo'],
        capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
    if check.returncode:
        parser.exit(2, 'RelWithDebInfo binary verification failed\n')
    identity = json.loads(check.stdout)
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    for name in args.case or BACKENDS:
        results.append(run(binary, identity, args.output, name))
        if results[-1]['status'] != 'PASS':
            break
    index = dict(status='PASS' if len(results) == len(args.case or BACKENDS) and
        all(x['status'] == 'PASS' for x in results) else 'FAIL',
        cases=[dict(case=x['case'],status=x['status'],result=x['case']+'/result.json',
            sha256=digest(args.output/x['case']/'result.json')) for x in results])
    (args.output/'index.json').write_text(json.dumps(index,indent=2),encoding='utf-8')
    return 0 if index['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
