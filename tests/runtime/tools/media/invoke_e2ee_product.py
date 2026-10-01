"""Real Qt window/Coordinator admission and key recovery against the pinned Flutter peer.

Credentials enter child environments only. This is a product-media subgate,
not the final E2EE verdict or the business-data/performance matrix.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import secrets
import sys
import time
import uuid

from invoke_e2ee_interop import ROOT, InteractiveFixtureProcess, credentials, read_events, sha


def prepare_private_diagnostics(output):
    # The production sink intentionally rejects workspace-inherited Users ACLs.
    # Restrict only a newly created fixture log directory, never existing data.
    directory = output / 'diagnostics'
    directory.mkdir()
    import ntsecuritycon, win32api, win32con, win32security
    token = win32security.OpenProcessToken(win32api.GetCurrentProcess(), win32con.TOKEN_QUERY)
    try:
        user = win32security.GetTokenInformation(token, win32security.TokenUser)[0]
    finally:
        token.Close()
    acl = win32security.ACL()
    for sid in [user, win32security.ConvertStringSidToSid('S-1-5-18'),
                win32security.ConvertStringSidToSid('S-1-5-32-544')]:
        acl.AddAccessAllowedAceEx(win32security.ACL_REVISION,
            win32con.CONTAINER_INHERIT_ACE | win32con.OBJECT_INHERIT_ACE,
            ntsecuritycon.FILE_ALL_ACCESS, sid)
    win32security.SetNamedSecurityInfo(str(directory), win32security.SE_FILE_OBJECT,
        win32security.DACL_SECURITY_INFORMATION | win32security.PROTECTED_DACL_SECURITY_INFORMATION,
        None, None, acl, None)



def evaluate_camera_content(product, flutter):
    matches = [e for e in product if e.get('event') == 'product_camera_correspondence']
    finished = [e for e in product if e.get('event') == 'product_camera_content_finished']
    checks = {
        'camera_content_no_mismatch': bool(matches) and all(e.get('valid') is True and 0 <= e.get('mean_luma_error', 256) <= 8 for e in matches),
        'camera_content_sampling_ok': not any(e.get('event') == 'product_camera_sample_failed' for e in flutter),
        'camera_content_finished': len(finished) == 1 and finished[0].get('errors') == 0 and finished[0].get('pending') == 0,
    }
    for epoch in (4, 6):
        checks[f'camera_content_epoch_{epoch}'] = sum(e.get('epoch') == epoch and e.get('valid') is True for e in matches) >= 3
        checks[f'camera_content_sent_{epoch}'] = sum(e.get('event') == 'product_camera_sample_sent' and e.get('epoch') == epoch for e in flutter) >= 3
    return checks


def evaluate(product, flutter, exit_codes, business=False, camera=None):
    recovery = [e for e in product if e['event'] == 'product_recovery_result' and e.get('installed') is True]
    cutoff = recovery[0]['time_ms'] if len(recovery) == 1 else float('inf')
    checks = {
        'process_exit': exit_codes == [0, 0],
        'product_joined': any(e['event'] == 'product_state' and e.get('state') == 5 for e in product),
        'product_closed': any(e['event'] == 'product_closed' for e in product),
        'flutter_closed': any(e['event'] == 'closed' for e in flutter),
        'no_flutter_error': not any(e['event'] in {'join_failed', 'sample_failed', 'close_failed', 'ui_error'} for e in flutter),
        'panel_recovery_submitted': sum(e['event'] == 'product_recovery_submitted' for e in product) == 1,
        'coordinator_recovery_installed': len(recovery) == 1,
        'no_product_error': not any(e['event'] in {'product_error', 'product_source_missing',
            'product_recovery_unavailable', 'product_recovery_dialog_missing', 'product_shutdown_timeout'} for e in product),
    }
    from datetime import datetime
    def when(e):
        return e.get('time_ms') or datetime.fromisoformat(e['time_utc']).timestamp() * 1000
    for phase in ('before', 'after'):
        selected = [e for e in product if (when(e) > cutoff) == (phase == 'after')]
        snapshots = [e for e in selected if e['event'] == 'product_media_status']
        for receiving in (False, True):
            for video in (False, True):
                name = f"{phase}_product_{'rx' if receiving else 'tx'}_{'video' if video else 'audio'}"
                checks[name] = any(e.get('epoch') == (6 if phase == 'after' else 4) and any(
                    t.get('receiving') is receiving and t.get('video') is video and t.get('protected') is True
                    for t in e.get('tracks', [])) for e in snapshots)
        renders = [e['submits'] for e in selected if e['event'] == 'product_render']
        checks[f'{phase}_product_render_progress'] = len(renders) >= 2 and renders[-1] > renders[0]
        for kind, metric in [('video', 'framesDecoded'), ('audio', 'totalSamplesReceived')]:
            tracks = {}
            for e in flutter:
                if e['event'] != 'rtp' or e.get('direction') != 'rx' or e.get('kind', '').lower() != kind: continue
                if (when(e) > cutoff) != (phase == 'after'): continue
                value = e.get('counters', {}).get(metric)
                if isinstance(value, (float, int)): tracks.setdefault(e.get('track'), []).append(value)
            checks[f'{phase}_flutter_decodes_{kind}'] = any(len(v) >= 2 and v[-1] > v[0] for v in tracks.values())
    if business:
        for name, events in [('native', product), ('flutter', flutter)]:
            checks[name + '_business_no_failure'] = not any(e['event'] in {
                'product_business_failed', 'product_business_send_failed'} for e in events)
            for epoch in (4, 6):
                for kind in ('chat', 'file'):
                    received = [e for e in events if e['event'] == f'product_{kind}_received' and e.get('epoch') == epoch]
                    checks[f'{name}_{epoch}_{kind}'] = len(received) == 1 and received[0].get('valid') is True
    if camera:
        checks['camera_no_publish_failure'] = not any(e['event'] == 'publish_failed' for e in flutter)
        if camera == 'flutter':
            checks['flutter_camera_published'] = any(e['event'] == 'published' and e.get('source') == 'camera' for e in flutter)
        for epoch in (4, 6):
            checks[f'camera_{epoch}_native_video_tracks'] = any(e['event'] == 'product_media_status' and e.get('epoch') == epoch
                and sum(t.get('video') is True and t.get('receiving') is False and t.get('protected') is True
                    for t in e.get('tracks', [])) >= (2 if camera == 'native' else 1) for e in product)
            if camera == 'native':
                decoded = {}
                for e in flutter:
                    if e['event'] != 'rtp' or e.get('direction') != 'rx' or e.get('kind', '').lower() != 'video': continue
                    if (when(e) > cutoff) != (epoch == 6): continue
                    value = e.get('counters', {}).get('framesDecoded')
                    if isinstance(value, (int, float)): decoded.setdefault(e.get('track'), []).append(value)
                checks[f'camera_{epoch}_flutter_two_video_decoders'] = sum(len(v) >= 2 and v[-1] > v[0] for v in decoded.values()) >= 2
    return checks


def evaluate_pair(first, second, exit_codes, whiteboard=False, wrong_key=False):
    checks = {'process_exit': exit_codes == [0, 0]}
    for index, events in enumerate((first, second)):
        prefix = f'peer{index}_'
        checks[prefix + 'joined'] = any(e['event'] == 'product_state' and e.get('state') == 5 for e in events)
        checks[prefix + 'closed'] = any(e['event'] == 'product_closed' for e in events)
        checks[prefix + 'recovered_once'] = sum(e['event'] == 'product_recovery_result' and e.get('installed') is True for e in events) == 1
        checks[prefix + 'no_failure'] = not any(e['event'] in {
            'product_error', 'product_source_missing', 'product_business_send_failed',
            'product_recovery_unavailable', 'product_recovery_dialog_missing', 'product_shutdown_timeout'} for e in events)
        for epoch in (4, 6):
            if whiteboard:
                for field in ('object_valid', 'asset_valid'):
                    checks[f'{prefix}{epoch}_{field}'] = any(e['event'] == 'product_whiteboard_observed'
                        and e.get('epoch') == epoch and e.get(field) is True for e in events)
            for kind in ('chat', 'file'):
                received = [e for e in events if e['event'] == f'product_{kind}_received' and e.get('epoch') == epoch]
                checks[f'{prefix}{epoch}_{kind}'] = (not received if wrong_key and epoch == 4
                    else len(received) == 1 and received[0].get('valid') is True)
            for rx in (False, True):
                for video in (False, True):
                    checks[f'{prefix}{epoch}_rx{rx}_video{video}'] = any(
                        e['event'] == 'product_media_status' and e.get('epoch') == epoch and any(
                            t.get('receiving') is rx and t.get('video') is video and t.get('protected') is True
                            for t in e.get('tracks', [])) for e in events)
                    if wrong_key and epoch == 4 and rx:
                        samples = [e for e in events if e['event'] == 'product_media_status' and e.get('epoch') == 4]
                        checks[f'{prefix}{epoch}_rx{rx}_video{video}'] = len(samples) >= 3 and not any(
                            t.get('receiving') is True and t.get('video') is video and t.get('protected') is True
                            for e in samples for t in e.get('tracks', []))
        if wrong_key:
            recoveries = [e for e in events if e['event'] == 'product_recovery_result']
            cutoff = recoveries[0]['time_ms'] if recoveries else float('inf')
            renders = [e for e in events if e['event'] == 'product_render' and e['time_ms'] < cutoff]
            checks[prefix + 'wrong_key_no_video_render'] = bool(renders) and all(e.get('submits') == 0 for e in renders)
            checks[prefix + 'wrong_key_business_attempted'] = any(e['event'] == 'product_business_sent' and e.get('epoch') == 4 for e in events)
    return checks


def prepare_whiteboard_room(room, authority='publisher'):
    # Only create the uniquely named fixture room; do not alter server config.
    from invoke_screen_share_quality_probe import REMOTE_AUTH, remote_python
    return remote_python(REMOTE_AUTH + f'''
import urllib.request
room = {room!r}
authority = {authority!r}
metadata = json.dumps(dict(detail=dict(info=dict(systemGenerated=dict(meetingID=room,
    creatorUserID=authority),creatorDefinedMeeting=dict(hostUserID=authority)))))
request = urllib.request.Request('http://127.0.0.1:17880/twirp/livekit.RoomService/CreateRoom',
    data=json.dumps(dict(name=room,metadata=metadata)).encode(),headers={{
    'Authorization':'Bearer '+token('e2ee-fixture-admin',dict(roomCreate=True)), 'Content-Type':'application/json'}})
response=json.load(urllib.request.urlopen(request,timeout=10))
print(json.dumps(dict(created=response.get('name')==room)))
''')


def evaluate_late(events):
    checks = {'late_joined': any(e['event'] == 'product_state' and e.get('state') == 5 for e in events),
        'late_closed': any(e['event'] == 'product_closed' for e in events),
        'late_recovered': sum(e['event'] == 'product_recovery_result' and e.get('installed') is True for e in events) == 1}
    for epoch in (4, 6):
        for field in ('object_valid', 'asset_valid'):
            checks[f'late_{epoch}_{field}'] = any(e['event'] == 'product_whiteboard_observed'
                and e.get('epoch') == epoch and e.get(field) is True for e in events)
        for video in (False, True):
            checks[f'late_{epoch}_two_rx_video{video}'] = any(e['event'] == 'product_media_status'
                and e.get('epoch') == epoch and sum(t.get('receiving') is True and t.get('video') is video
                    and t.get('protected') is True for t in e.get('tracks', [])) >= 2 for e in events)
    for kind in ('chat', 'file'):
        received = [e for e in events if e['event'] == f'product_{kind}_received' and e.get('epoch') == 6]
        checks[f'late_6_two_{kind}'] = len(received) == 2 and all(e.get('valid') is True for e in received)
    return checks


def read_live_events(path):
    if not path.exists(): return []
    data = path.read_bytes()
    # The writer may be between JSON bytes and its newline. Only defer that
    # incomplete tail; malformed complete records must still fail the run.
    complete = data.rsplit(b'\n', 1)[0] if b'\n' in data else b''
    return [json.loads(line) for line in complete.splitlines() if line.strip()]


def evaluate_reconnect(events):
    requests = [e for e in events if e['event'] == 'product_reconnect_requested']
    cutoff = requests[0]['time_ms'] if len(requests) == 1 else float('inf')
    states = [e for e in events if e['event'] == 'product_state']
    initial = [e.get('generation', 0) for e in events if e['event'] == 'product_media_status' and e['time_ms'] < cutoff]
    checks = {'reconnect_requested_once': len(requests) == 1,
        'product_reconnecting': any(e.get('state') == 6 and e['time_ms'] > cutoff for e in states),
        'product_rejoined': any(e.get('state') == 5 and e['time_ms'] > cutoff for e in states)}
    for rx in (False, True):
        for video in (False, True):
            checks[f'reconnect_new_generation_rx{rx}_video{video}'] = bool(initial) and any(
                e['event'] == 'product_media_status' and e.get('epoch') == 6 and e.get('generation', 0) > max(initial)
                and any(t.get('receiving') is rx and t.get('video') is video and t.get('protected') is True
                    for t in e.get('tracks', [])) for e in events)
    return checks


def evaluate_inflight(events):
    checks = {}
    for event in ('sent', 'partial_received', 'send_cancelled', 'receive_cancelled'):
        checks['inflight_' + event] = sum(e['event'] == 'product_inflight_' + event for e in events) == 1
    checks['inflight_no_delivery'] = not any(e['event'] == 'product_inflight_completed' for e in events)
    partial = [e['time_ms'] for e in events if e['event'] == 'product_inflight_partial_received']
    recovery = [e['time_ms'] for e in events if e['event'] == 'product_recovery_submitted']
    checks['inflight_recovery_after_partial'] = len(partial) == len(recovery) == 1 and partial[0] <= recovery[0]
    return checks


def evaluate_flutter_board(product, flutter, authority, partial=False):
    checks = {'flutter_board_no_failure': not any(e['event'] == 'product_board_failed' for e in flutter)}
    for epoch in (4, 6):
        for field in ('object_valid', 'asset_valid', 'flutter_object_valid'):
            observed = [e for e in product if e['event'] == 'product_whiteboard_observed' and e.get('epoch') == epoch]
            checks[f'native_board_{epoch}_{field}'] = (bool(observed) and not any(e.get(field) is True for e in observed)
                if partial and epoch == 4 and field == 'asset_valid' else any(e.get(field) is True for e in observed))
        for kind in (('authority_sent',) if authority else ('operation_received', 'snapshot_received', 'asset_received')):
            rows = [e for e in flutter if e['event'] == 'product_board_' + kind and e.get('epoch') == epoch]
            checks[f'flutter_board_{epoch}_{kind}'] = bool(rows) and (authority or all(e.get('valid') is True for e in rows))
    if partial:
        received = [e['time_ms'] for e in product if e['event'] == 'product_board_partial_received']
        recovery = [e['time_ms'] for e in product if e['event'] == 'product_recovery_submitted']
        checks['partial_received_before_recovery'] = len(received) == len(recovery) == 1 and received[0] < recovery[0]
        checks['partial_sent'] = sum(e['event'] == 'product_board_partial_sent' for e in flutter) == 1
        checks['old_tail_sent_after_rotation'] = sum(e['event'] == 'product_board_old_tail_sent' and e.get('epoch') == 6 for e in flutter) == 1
        from datetime import datetime
        tails = [datetime.fromisoformat(e['time_utc']).timestamp() * 1000 for e in flutter if e['event'] == 'product_board_old_tail_sent']
        checks['rotation_before_asset_expiry'] = bool(received and recovery and tails) and 0 < recovery[0] - received[0] < 10000 and 0 < tails[0] - received[0] < 10000
        delivered = [e['time_ms'] for e in product if e['event'] == 'product_board_old_tail_received']
        checks['old_tail_received_before_expiry'] = len(delivered) == 1 and bool(received and recovery) and recovery[0] < delivered[0] < received[0] + 10000
    return checks


def evaluate_steady(events, encrypted, expected_video=1, business=True, whiteboard=False, media_until=float('inf')):
    checks = {
        'joined': any(e['event'] == 'product_state' and e.get('state') == 5 for e in events),
        'closed': any(e['event'] == 'product_closed' for e in events),
        'no_recovery': not any(e['event'] == 'product_recovery_submitted' for e in events),
        'no_error': not any(e['event'] in {'product_error', 'product_business_send_failed', 'product_shutdown_timeout'} for e in events),
    }
    # Remote-track retirement clears per-track telemetry. Compare media only
    # while every peer is still running; process close checks use full evidence.
    media_events = [e for e in events if e.get('time_ms', 0) < media_until]
    renders = [e.get('submits', 0) for e in media_events if e['event'] == 'product_render']
    metrics = [e for e in media_events if e['event'] == 'product_metrics']
    checks['renders_progress'] = len(renders) >= 2 and renders[-1] > renders[0]
    decodes = [e.get('inboundVideoFramesDecoded', 0) for e in metrics]
    checks['decodes_progress'] = len(decodes) >= 2 and decodes[-1] > decodes[0]
    statuses = [e for e in events if e['event'] == 'product_media_status']
    modes = [e for e in events if e['event'] == 'product_mode']
    checks['mode_explicit'] = bool(modes) and all(e.get('manager_present') is encrypted for e in modes)
    decoded_tracks = {}
    for e in media_events:
        if e['event'] == 'product_rtp' and e.get('direction') == 'rx' and e.get('kind') == 'video':
            decoded_tracks.setdefault(e.get('track'), []).append(e.get('framesDecoded', 0))
    checks['every_video_decodes'] = sum(len(v) >= 2 and v[-1] > v[0] for v in decoded_tracks.values()) >= expected_video
    if encrypted:
        checks['current_video_protection'] = any(sum(t.get('receiving') is True and t.get('video') is True
            and t.get('protected') is True for t in e.get('tracks', [])) >= expected_video for e in statuses)
        checks['current_audio_protection'] = any(any(t.get('receiving') is True and t.get('video') is False
            and t.get('protected') is True for t in e.get('tracks', [])) for e in statuses)
    else:
        checks['no_crypto_bindings'] = all(not e.get('tracks') for e in statuses)
        checks['video_bindings'] = any(e['event'] == 'product_render' and e.get('bindings', 0) >= expected_video for e in events)
    if business:
        for kind in ('chat', 'file'):
            rows = [e for e in events if e['event'] == f'product_{kind}_received' and e.get('epoch') == 4]
            checks[kind + '_delivered'] = bool(rows) and all(e.get('valid') is True for e in rows)
    if whiteboard:
        for field in ('object_valid', 'asset_valid'):
            checks[field] = any(e['event'] == 'product_whiteboard_observed' and e.get('epoch') == 4 and e.get(field) is True for e in events)
    return checks


def evaluate_lifecycle(product, flutter, codes):
    from datetime import datetime
    def when(event):
        return event['time_ms'] if 'time_ms' in event else datetime.fromisoformat(event['time_utc']).timestamp() * 1000
    checks = {'process_exit': codes == [0, 0]}
    for name, rows in [('native', product), ('flutter', flutter)]:
        checks[name + '_closed'] = any(e['event'] in {'closed', 'product_closed'} for e in rows)
        checks[name + '_no_error'] = not any(e['event'] in {'product_error', 'sample_failed', 'publish_failed', 'product_shutdown_timeout'} for e in rows)
        for action in ('stop', 'retired', 'restart'):
            checks[name + '_share_' + action] = [e.get('cycle') for e in rows if e['event'] == 'product_share_' + action] == [1, 2, 3]
        states = [e.get('muted') for e in rows if e['event'] == 'product_audio_state']
        checks[name + '_mute_restore'] = True in states and False in states[states.index(True) + 1:]
        checks[name + '_no_key_change'] = not any(e['event'] in {'key_changed', 'product_recovery_submitted'} for e in rows)
        tracks = {}
        for e in rows:
            if name == 'native' and e['event'] == 'product_rtp' and e.get('direction') == 'rx' and e.get('kind') == 'video':
                tracks.setdefault(e['track'], []).append(e.get('framesDecoded', 0))
            if name == 'flutter' and e['event'] == 'rtp' and e.get('direction') == 'rx' and e.get('kind') == 'VIDEO':
                tracks.setdefault(e['track'], []).append(e.get('counters', {}).get('framesDecoded', 0))
        checks[name + '_four_distinct_received_videos'] = sum(len(v) >= 2 and v[-1] > v[0] for v in tracks.values()) >= 4
        unmuted = [when(e) for e in rows if e['event'] == 'product_audio_control' and e.get('muted') is False]
        muted = [when(e) for e in rows if e['event'] == 'product_audio_control' and e.get('muted') is True]
        for phase in ('before', 'after'):
            samples = []
            for e in rows:
                if not muted or not unmuted: continue
                if phase == 'before' and when(e) >= muted[0]: continue
                if phase == 'after' and when(e) <= unmuted[0] + 2000: continue
                if e['event'] == 'product_pcm': samples.append(e['samples'])
            checks[name + '_decoded_audio_' + phase] = len(samples) >= 2 and samples[-1] > samples[0]
    statuses = [e for e in product if e['event'] == 'product_media_status']
    for rx in (False, True):
        ids = {t['binding_id'] for e in statuses for t in e.get('tracks', [])
               if t.get('receiving') is rx and t.get('video') is True and t.get('protected') is True}
        checks['native_protected_video_bindings_' + str(rx)] = len(ids) >= 4
    checks['native_sender_binding_retirement'] = all(e.get('sender_video_bindings') == 0 for e in product if e['event'] == 'product_share_retired')
    retirement = [e for e in product if e['event'] == 'product_retirement']
    checks['native_shutdown_owner_retirement'] = len(retirement) == 1 and all(
        retirement[0].get(name + '_observed', 0) > 0 and retirement[0].get(name + '_alive', -1) == 0
        for name in ('rooms', 'managers', 'media_observations'))
    checks['native_auto_share_once'] = sum(e['event'] == 'product_auto_share_ready' for e in product) == 1
    checks['native_cleanup_queue_drained'] = len(retirement) == 1 and retirement[0].get('cleanup_jobs_pending', -1) == 0
    for direction in ('rx', 'tx'):
        checks['flutter_crypto_video_' + direction] = len({e['track'] for e in flutter
            if e['event'] == 'crypto_state' and e.get('direction') == direction
            and e.get('state') == 'kOk' and e.get('track', '').startswith('TR_V')}) >= 4
    return checks


def evaluate_negative_peer(events):
    received = [e for e in events if e['event'] == 'rtp' and e.get('direction') == 'rx']
    crypto = [e for e in events if e['event'] == 'crypto_state' and e.get('direction') == 'rx']
    return {
        'connected': any(e['event'] == 'connected' for e in events),
        'closed': any(e['event'] == 'closed' for e in events),
        'subscribed': len({e.get('track') for e in events if e['event'] == 'subscribed'}) >= 2,
        'crypto_rejection_observed': bool(crypto) and any(e.get('state') in {'kMissingKey', 'kDecryptionFailed'} for e in crypto),
        'no_authenticated_receive': not any(e.get('state') == 'kOk' for e in crypto),
        'no_decoded_video': not any(e.get('counters', {}).get('framesDecoded', 0) > 0 for e in received),
        'no_decoded_audio_energy': not any(e.get('counters', {}).get('totalAudioEnergy', 0) > 0 for e in received),
        'no_data_delivery': not any(e['event'] in {'data_received', 'stream_received', 'product_chat_received', 'product_file_received'} for e in events),
    }


def evaluate_byte_streams(native, flutter, kind="byte"):
    if kind not in ("byte", "text"):
        raise ValueError("unsupported stream evidence kind")
    checks = {}
    for name, rows in [('native', native), ('flutter', flutter)]:
        checks[name + f'_{kind}_no_failure'] = not any(e['event'] == f'product_{kind}_failed' for e in rows)
        for epoch in [4, 6]:
            sent = [e for e in rows if e['event'] == f'product_{kind}_sent' and e.get('epoch') == epoch]
            received = [e for e in rows if e['event'] == f'product_{kind}_received' and e.get('epoch') == epoch]
            checks[f'{name}_{kind}_{epoch}_sent'] = len(sent) == 1 and sent[0].get('streams') == 2
            checks[f'{name}_{kind}_{epoch}_received'] = (len(received) == 2
                and {e.get('index') for e in received} == {0, 1} and all(e.get('valid') is True for e in received))
        checks[name + f'_{kind}_only_expected_receipts'] = sum(e['event'] == f'product_{kind}_received' for e in rows) == 4
    return checks


def run(duration, debug_native=False, native_pair=False, whiteboard=False, late_product=False, reconnect=False, wrong_key=False, flutter_business=False, camera=None, codec='h264', inflight=False, flutter_board=None, steady=False, off=False, lifecycle=False, mixed_negative=None, inflight_board=False, output_evidence=False, byte_streams=False, camera_content=False):
    if flutter_board: whiteboard = True
    locator = json.loads((ROOT / 'build-e2ee-backend/flutter-repaired-current.json').read_text())['directory']
    if Path(locator).name != locator or not locator.startswith('flutter-repaired-'): raise ValueError('locator')
    flutter = ROOT / 'build-e2ee-backend' / locator / 'e2ee_flutter_peer.exe'
    backend = json.loads(flutter.with_name('backend-provenance.json').read_text())
    if (backend['configuration'] != 'Release' or sha(flutter.with_name('libwebrtc.dll')) != backend['dll_sha256']
            or sha(flutter.parent / 'data/app.so') != backend['aot_sha256']): raise ValueError('fingerprint')
    if backend.get('memory_frame_probe') and sha(flutter.with_name('flutter_webrtc_plugin.dll')) != backend['plugin_dll_sha256']: raise ValueError('plugin fingerprint')
    native = ROOT / 'build-debug/RelWithDebInfo/test_main_window_latency.exe'
    run_id = 'product-e2ee-' + uuid.uuid4().hex[:16]
    output = ROOT / 'docs/analysis/e2ee/evidence/e0-product-20261001' / run_id
    output.mkdir()
    files = [native, flutter, flutter.with_name('libwebrtc.dll'), flutter.parent / 'data/app.so', Path(__file__),
        Path(__file__).with_name('e2ee_output_scan.py'), Path(__file__).with_name('invoke_e2ee_interop.py'),
        ROOT / 'tests/ui/test_main_window_latency.cpp',
        ROOT / 'tests/runtime/probes/e2ee_product_runtime.h', ROOT / 'src/core/meeting_coordinator.cpp',
        ROOT / 'tests/runtime/probes/e2ee_product_lifecycle.h',
        ROOT / 'tests/runtime/probes/e2ee_camera_correspondence.h',
        ROOT / 'tests/runtime/probes/e2ee_frame_signature.h',
        ROOT / 'tests/runtime/tools/media/flutter_e2ee_peer/lib/camera_correspondence.dart',
        ROOT / 'tests/runtime/probes/e2ee_output_evidence.h',
        ROOT / 'tests/runtime/probes/e2ee_invitation_evidence.h',
        ROOT / 'tests/runtime/probes/e2ee_product_byte_stream.h',
        ROOT / 'src/net/session_manager.cpp',
        ROOT / 'src/core/room.cpp', ROOT / 'src/core/participant_event.h', ROOT / 'src/core/meeting_session_runtime.h',
        ROOT / 'src/core/whiteboard/whiteboard_runtime.cpp', ROOT / 'src/core/whiteboard/whiteboard_runtime.h',
        ROOT / 'tests/runtime/tools/media/flutter_e2ee_peer/lib/main.dart',
        ROOT / 'tests/runtime/tools/media/flutter_e2ee_peer/lib/product_whiteboard.dart',
        ROOT / 'src/ui/meeting_encryption_panel.h', ROOT / 'src/ui/meeting_encryption_dialog.h',
        ROOT / 'src/ui/meeting_auto_share.h', ROOT / 'src/ui/meeting_main_window.cpp',
        ROOT / 'src/ui/meeting_room_window.cpp', ROOT / 'src/e2ee/frame_cryptor.cpp', ROOT / 'src/e2ee/key_provider.cpp']
    # Bind each run to the consumed source overlay and staged Flutter lineage.
    cache = (ROOT / 'build-debug/CMakeCache.txt').read_text(encoding='utf-8')
    package_line = next(line for line in cache.splitlines() if line.startswith('COHAVORA_E2EE_BACKEND_PACKAGE_DIR:PATH='))
    package = Path(package_line.split('=', 1)[1])
    profile_path = ROOT / 'build/prepare/e2ee-backend-profile.json'
    profile = json.loads(profile_path.read_text(encoding='utf-8'))
    files += [profile_path, package / 'package.json', flutter.parent / 'backend-provenance.json']
    for name, expected in profile['files'].items():
        path = package / name
        if sha(path) != expected: raise RuntimeError('native_backend_profile_mismatch')
        files.append(path)
    manifest = {'status': 'STARTING', 'scope': 'real Qt/Coordinator media admission and panel recovery',
        'native_configuration': 'RelWithDebInfo', 'flutter_configuration': 'Release', 'run_id': run_id,
        'sha256': {str(p.relative_to(ROOT)): sha(p) for p in files},
        'not_covered': ['business data and RPC', 'camera publish', 'reconnect and three-party matrix', 'performance and soak', 'final E2EE completion']}
    if flutter_business:
        manifest['scope'] += '; official Flutter SDK bidirectional product chat and fragmented file at epochs 4/6'
        manifest['not_covered'][0] = 'whiteboard and RPC'
    if native_pair:
        manifest.update(scope='two real Qt/Coordinator peers: bidirectional chat and fragmented file before/after panel recovery',
            flutter_configuration=None,
            not_covered=['whiteboard and RPC', 'camera publish', 'reconnect and three-party matrix', 'performance and soak', 'final E2EE completion'])
    if whiteboard:
        manifest['scope'] += '; whiteboard authority operations, snapshot projection and PNG asset'
        manifest['not_covered'][0] = 'RPC and whiteboard late-join snapshot'
    if late_product:
        manifest['scope'] += '; third product joins after committed image and receives snapshot, asset and post-recovery business'
        manifest['not_covered'] = ['RPC', 'camera publish', 'reconnect', 'performance and soak', 'final E2EE completion']
    if reconnect: manifest['scope'] += '; product full reconnect before panel recovery'
    if wrong_key: manifest['scope'] += '; mismatched initial keys reject media/chat/file then recover through Qt'
    if inflight: manifest['scope'] += '; cancel bidirectional in-flight 1 MiB files on recovery after observed partial reception'
    if flutter_board: manifest['scope'] += f'; official SDK whiteboard operations/snapshots/PNG assets, authority={flutter_board}'
    if steady:
        manifest['scope'] = f"{'three' if late_product else 'two'} native product steady {'Off' if off else 'Required'} functional load; metrics retained, no performance verdict"
        manifest['mode'] = 'off' if off else 'on'
        manifest['duration_seconds'] = duration
    if camera_content:
        if not backend.get('memory_frame_probe'): raise ValueError('memory probe provenance missing')
        manifest['camera_content_thresholds'] = {'cells': 64, 'contrast_min': 16, 'mean_error_max': 8, 'time_window_ms': 5000}
    if camera:
        manifest['scope'] += f'; {camera} real camera, codec={codec}; no recording'
        manifest['camera_publisher'] = camera
        manifest['codec'] = codec
        manifest['not_covered'] = [item for item in manifest['not_covered'] if item != 'camera publish']
    if byte_streams:
        manifest['scope'] += '; concurrent ByteStream and UTF-8 TextStream, two streams per direction at epochs 4/6'
    if inflight_board:
        manifest['scope'] += '; partial whiteboard asset before rotation, old tail after rotation must not assemble'
    def save():
        temporary = output / 'result.tmp'
        temporary.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        temporary.replace(output / 'result.json')
    save()
    children = []
    source = None
    negative_key = None
    try:
        if output_evidence: prepare_private_diagnostics(output)
        auth = credentials(run_id)
        if whiteboard and prepare_whiteboard_room(run_id, 'receiver' if flutter_board == 'flutter' else 'publisher') != {'created': True}: raise RuntimeError('room_setup')
        source = InteractiveFixtureProcess([sys.executable, str(Path(__file__).with_name('invoke_e2ee_interop.py')),
            '--source-window', '--focus-source', '--source-evidence', str(output / 'source.jsonl'),
            '--source-owner-pid', str(os.getpid())])
        time.sleep(2)  # Fixture creation only; never a key transition synchronization.
        env = dict(os.environ, LIVEKIT_URL='ws://123.56.225.164:17880', LIVEKIT_TEST_ALLOW_INSECURE='1',
            E2EE_TEST_KEY=secrets.token_urlsafe(32), E2EE_NEXT_TEST_KEY=secrets.token_urlsafe(32),
            E2EE_MODE='on', E2EE_RUN_ID=run_id, E2EE_EVIDENCE_DIR=str(output), E2EE_DURATION=str(duration),
            E2EE_PUBLISH='1', E2EE_CODEC=codec, E2EE_KEY_STATE='present', E2EE_VIDEO_SOURCE='window',
            E2EE_KEY_ACTION='replace', E2EE_KEY_ROUNDS='1', E2EE_RATCHET_WINDOW='16', E2EE_FAILURE_TOLERANCE='-1',
            E2EE_KEY_ACTION_AT_MS=str(int((time.time() + duration / 2) * 1000)), E2EE_RPC='0')
        if flutter_business: env['E2EE_PRODUCT_BUSINESS'] = '1'
        if camera: env['E2EE_NO_SCREENSHOT'] = '1'
        if camera_content: env['E2EE_CAMERA_CONTENT'] = '1'
        if inflight: env['E2EE_PRODUCT_INFLIGHT'] = '1'
        if output_evidence: env['E2EE_OUTPUT_EVIDENCE'] = '1'
        if byte_streams: env['E2EE_PRODUCT_BYTE_STREAMS'] = '1'
        if inflight_board: env.update(E2EE_PRODUCT_BOARD_PARTIAL='1', E2EE_KEY_TRIGGER=str(output / 'key-trigger'))
        if steady: env.update(E2EE_PRODUCT_STEADY='1', E2EE_KEY_ACTION='none')
        if lifecycle:
            env.update(E2EE_PRODUCT_LIFECYCLE='1', E2EE_PRODUCT_STEADY='1', E2EE_KEY_ACTION='none',
                E2EE_LIFECYCLE_START_MS=str(int(time.time() * 1000)))
            manifest['scope'] = 'native/Flutter microphone mute recovery and three screen stop/restart cycles, fixed current key; no soak claim'
        if off: env['E2EE_MODE'] = 'off'
        if whiteboard: env['E2EE_PRODUCT_WHITEBOARD'] = '1'
        if flutter_board:
            import struct, zlib
            env['E2EE_FLUTTER_AUTHORITY'] = '1' if flutter_board == 'flutter' else '0'
            for epoch, color in [(4, bytes([25, 80, 190])), (6, bytes([190, 80, 25]))]:
                def chunk(kind, data):
                    return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
                png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 320, 180, 8, 2, 0, 0, 0))
                png += chunk(b'IDAT', zlib.compress((b'\0' + color * 320) * 180)) + chunk(b'IEND', b'')
                asset = output / f'public-whiteboard-{epoch}.png'; asset.write_bytes(png)
                env[f'E2EE_WHITEBOARD_ASSET{epoch}'] = str(asset)
                manifest['sha256'][str(asset.relative_to(ROOT))] = sha(asset)
        if wrong_key: env['E2EE_PRODUCT_NEGATIVE'] = '1'
        if reconnect:
            env.update(E2EE_PRODUCT_RECONNECT='full', E2EE_RECONNECT_AT_MS=str(int((time.time() + duration / 3) * 1000)))
        if native_pair:
            peer_output = output / 'receiver'
            peer_output.mkdir()
            if output_evidence: prepare_private_diagnostics(peer_output)
            env['E2EE_PRODUCT_BUSINESS'] = '1'
            children.append(InteractiveFixtureProcess([str(native), '--e2ee-product-runtime', '--debug'], cwd=native.parent,
                env=dict(env, LIVEKIT_TOKEN=auth['receiver'], E2EE_PRODUCT_IDENTITY='receiver', E2EE_EVIDENCE_DIR=str(peer_output))))
        else:
            children.append(InteractiveFixtureProcess([str(flutter)], cwd=flutter.parent,
                env=dict(env, LIVEKIT_TOKEN=auth['receiver'], E2EE_PRODUCT_CAMERA='1' if camera == 'flutter' else '0',
                    E2EE_VIDEO_SOURCE='camera' if camera == 'flutter' else 'window')))
        command = [str(native), '--e2ee-product-runtime', '--debug']
        if debug_native:
            debugger = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
            stack = output / 'native-crash-stack.txt'
            commands = output / 'stack-only.cdb'
            commands.write_text(f'sxi out\n.logopen "{stack}"\nsxe -c ".ecxr; kn; .logclose; q" av\ng\n', encoding='ascii')
            command = [debugger, '-G', '-cf', str(commands)] + command
        publisher_env = dict(env, LIVEKIT_TOKEN=auth['publisher'])
        publisher_env['E2EE_PRODUCT_CAMERA'] = '1' if camera == 'native' else '0'
        if wrong_key: publisher_env['E2EE_TEST_KEY'] = secrets.token_urlsafe(32)
        children.append(InteractiveFixtureProcess(command, cwd=native.parent, env=publisher_env))
        if mixed_negative:
            negative_output = output / 'negative'; negative_output.mkdir()
            negative_key = secrets.token_urlsafe(32)
            children.append(InteractiveFixtureProcess([str(flutter)], cwd=flutter.parent,
                env=dict(env, LIVEKIT_TOKEN=auth['late'], E2EE_EVIDENCE_DIR=str(negative_output),
                    E2EE_TEST_KEY=negative_key, E2EE_KEY_STATE='missing' if mixed_negative == 'missing' else 'present',
                    E2EE_KEY_ACTION='none', E2EE_PUBLISH='0', E2EE_FAILURE_TOLERANCE='0')))
            manifest['mixed_negative'] = mixed_negative
            manifest['negative_failure_tolerance'] = 0
        manifest.update(status='RUNNING', pids=[p.pid for p in children]); save()
        print(json.dumps({'event': 'started', 'evidence': str(output)}), flush=True)
        deadline = time.monotonic() + duration + 45
        late_started = False
        while any(p.poll() is None for p in children) and time.monotonic() < deadline:
            if inflight_board and not (output / 'key-trigger').exists():
                rows = read_live_events(output / 'product.jsonl')
                partial = any(e['event'] == 'product_board_partial_received' for e in rows)
                flutter_rows = read_live_events(output / 'flutter.jsonl')
                initial_checks = evaluate(rows, flutter_rows, [])
                if partial and all(value for name, value in initial_checks.items() if name.startswith('before_')):
                    (output / 'key-trigger').write_text('rotate', encoding='ascii')
            if late_product and not late_started and any(e['event'] == 'product_whiteboard_observed'
                    and e.get('epoch') == 4 and e.get('asset_valid') is True
                    for e in read_live_events(output / 'product.jsonl')):
                late_started = True
                late_output = output / 'late'; late_output.mkdir()
                children.append(InteractiveFixtureProcess([str(native), '--e2ee-product-runtime', '--debug'], cwd=native.parent,
                    env=dict(env, LIVEKIT_TOKEN=auth['late'], E2EE_PRODUCT_IDENTITY='late', E2EE_PRODUCT_OBSERVER='1',
                        E2EE_EVIDENCE_DIR=str(late_output))))
                manifest['late_start_time_ms'] = int(time.time() * 1000)
                manifest['pids'] = [p.pid for p in children]; save()
            time.sleep(1)
        for child in children:
            if child.poll() is None: child.terminate(); child.wait(timeout=15)
        codes = [p.returncode for p in children]
        manifest['exit_codes'] = codes
        save()
        checks = (evaluate_pair(read_events(output / 'product.jsonl'), read_events(output / 'receiver/product.jsonl'), codes[:2], whiteboard, wrong_key)
            if native_pair else evaluate(read_events(output / 'product.jsonl'), read_events(output / 'flutter.jsonl'), codes[:2], flutter_business, camera))
        if mixed_negative:
            checks['three_process_exit'] = codes == [0, 0, 0]
            checks.update({'negative_' + k: v for k, v in evaluate_negative_peer(read_events(output / 'negative/flutter.jsonl')).items()})
        if late_product:
            checks['three_process_exit'] = codes == [0, 0, 0]
            checks['late_started_before_rotation'] = manifest.get('late_start_time_ms', float('inf')) < int(env['E2EE_KEY_ACTION_AT_MS'])
            checks.update(evaluate_late(read_events(output / 'late/product.jsonl')))
        if reconnect:
            checks.update(evaluate_reconnect(read_events(output / 'product.jsonl')))
        if inflight:
            for name, path in [('publisher', output / 'product.jsonl'), ('receiver', output / 'receiver/product.jsonl')]:
                checks.update({name + '_' + k: v for k, v in evaluate_inflight(read_events(path)).items()})
        if flutter_board:
            checks.update(evaluate_flutter_board(read_events(output / 'product.jsonl'),
                read_events(output / 'flutter.jsonl'), flutter_board == 'flutter', inflight_board))
        if steady:
            checks = {'process_exit': codes == ([0, 0, 0] if late_product else [0, 0])}
            peers = [('publisher', output / 'product.jsonl'), ('receiver', output / 'receiver/product.jsonl')]
            if late_product: peers.append(('late', output / 'late/product.jsonl'))
            peer_events = [(name, read_events(path)) for name, path in peers]
            media_until = min((e['time_ms'] for _, rows in peer_events for e in rows
                if e['event'] == 'product_leave_requested'), default=float('inf'))
            manifest['common_media_window_end_ms'] = media_until if media_until != float('inf') else None
            for name, rows in peer_events:
                checks.update({name + '_' + k: v for k, v in evaluate_steady(rows, not off,
                    2 if late_product else 1, name != 'late', whiteboard, media_until).items()})
        from e2ee_output_scan import scan_outputs
        if camera_content:
            checks.update(evaluate_camera_content(read_events(output / 'product.jsonl'), read_events(output / 'flutter.jsonl')))
        if lifecycle:
            checks = evaluate_lifecycle(read_events(output / 'product.jsonl'), read_events(output / 'flutter.jsonl'), codes)
        if output_evidence:
            for name, directory in [('native', output)] + ([('receiver', output / 'receiver')] if native_pair else []):
                output_events = [e for e in read_events(directory / 'product.jsonl') if e['event'] == 'product_output_evidence']
                checks[name + '_actual_persistence_and_exports'] = len(output_events) == 1 and output_events[0].get('valid') is True
                invitations = [e for e in read_events(directory / 'product.jsonl') if e['event'] == 'product_invitation_evidence']
                checks[name + '_invitation_secret_free'] = len(invitations) == 2 and {e.get('epoch') for e in invitations} == {4, 6} and all(e.get('valid') is True for e in invitations)
                checks[name + '_settings_file_present'] = bool(list((directory / 'settings').rglob('*.ini')))
        if byte_streams:
            checks.update(evaluate_byte_streams(read_events(output / 'product.jsonl'), read_events(output / 'flutter.jsonl')))
            checks.update(evaluate_byte_streams(read_events(output / 'product.jsonl'), read_events(output / 'flutter.jsonl'), 'text'))
        manifest['secret_output_scan'] = scan_outputs(output,
            [env['E2EE_TEST_KEY'], env['E2EE_NEXT_TEST_KEY'], publisher_env['E2EE_TEST_KEY'], negative_key])
        checks['secret_output_scan'] = manifest['secret_output_scan']['status'] == 'PASS'
        manifest.update(status='PASS' if all(checks.values()) else 'NEEDS_FIX', checks=checks, exit_codes=codes)
        save()
        print(json.dumps({'status': manifest['status'], 'evidence': str(output), 'failed': [k for k,v in checks.items() if not v]}), flush=True)
        return all(checks.values())
    except Exception as error:
        manifest.update(status='BLOCKED', error_type=type(error).__name__); save()
        print(json.dumps({'status': 'BLOCKED', 'error_type': type(error).__name__, 'evidence': str(output)}), flush=True)
        return False
    finally:
        for child in children + ([source] if source else []):
            if child.poll() is None: child.terminate(); child.wait(timeout=15)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--duration', type=int, default=60)
    parser.add_argument('--debug-native', action='store_true')
    parser.add_argument('--native-pair', action='store_true')
    parser.add_argument('--whiteboard', action='store_true')
    parser.add_argument('--late-product', action='store_true')
    parser.add_argument('--reconnect', action='store_true')
    parser.add_argument('--wrong-key', action='store_true')
    parser.add_argument('--flutter-business', action='store_true')
    parser.add_argument('--camera-content', action='store_true')
    parser.add_argument('--camera', choices=['native', 'flutter'])
    parser.add_argument('--codec', choices=['h264', 'vp8'], default='h264')
    parser.add_argument('--inflight', action='store_true')
    parser.add_argument('--flutter-board', choices=['native', 'flutter'])
    parser.add_argument('--steady', action='store_true')
    parser.add_argument('--off', action='store_true')
    parser.add_argument('--lifecycle', action='store_true')
    parser.add_argument('--mixed-negative', choices=['wrong', 'missing'])
    parser.add_argument('--inflight-board', action='store_true')
    parser.add_argument('--output-evidence', action='store_true')
    parser.add_argument('--byte-streams', action='store_true')
    args = parser.parse_args()
    if args.byte_streams and (args.native_pair or args.lifecycle or args.mixed_negative or args.flutter_board):
        parser.error('byte streams require a Flutter peer with ordinary key replacement')
    if args.inflight_board and (args.flutter_board != 'flutter' or args.lifecycle or args.mixed_negative):
        parser.error('inflight board requires Flutter authority without other scenarios')
    if args.mixed_negative and (not args.flutter_business or args.native_pair or args.lifecycle or args.camera or args.flutter_board or args.late_product):
        parser.error('mixed negative requires isolated Flutter business profile')
    if args.lifecycle and (args.duration < 95 or args.native_pair or args.camera or args.steady or args.inflight or args.reconnect or args.wrong_key or args.flutter_board):
        parser.error('lifecycle requires a Flutter peer, duration >=95, and no recovery or camera scenario')
    if not 50 <= args.duration <= (900 if args.steady else 180): parser.error('duration outside profile bounds')
    if args.off and not args.steady: parser.error('off requires explicit steady profile')
    if args.steady and (not args.native_pair or args.inflight or args.wrong_key or args.reconnect or args.camera):
        parser.error('steady requires native-pair without recovery scenarios')
    if args.whiteboard and not args.native_pair: parser.error('whiteboard requires native-pair')
    if args.flutter_business and args.native_pair: parser.error('flutter-business requires Flutter peer')
    if args.camera_content and not args.camera: parser.error('camera-content requires camera publisher')
    if args.camera and args.native_pair: parser.error('camera requires Flutter peer')
    if args.flutter_board and (args.native_pair or args.late_product or args.wrong_key):
        parser.error('flutter-board requires ordinary Flutter peer')
    if args.inflight and (not args.native_pair or args.wrong_key or args.reconnect or args.late_product):
        parser.error('inflight requires native-pair without wrong-key/reconnect/late-product')
    if args.late_product and not args.whiteboard: parser.error('late-product requires whiteboard')
    if args.wrong_key and (not args.native_pair or args.whiteboard or args.reconnect): parser.error('wrong-key requires plain native-pair')
    sys.exit(0 if run(args.duration, args.debug_native, args.native_pair, args.whiteboard, args.late_product, args.reconnect, args.wrong_key, args.flutter_business, args.camera, args.codec, args.inflight, args.flutter_board, args.steady, args.off, args.lifecycle, args.mixed_negative, args.inflight_board, args.output_evidence, args.byte_streams, args.camera_content) else 1)
