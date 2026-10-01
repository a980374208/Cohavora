import sys
from datetime import datetime, timezone
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/media'))
from invoke_e2ee_product import evaluate_lifecycle


def evidence():
    native, flutter = [dict(event='product_retirement', time_ms=100000,
        rooms_observed=1, rooms_alive=0, managers_observed=1, managers_alive=0,
        media_observations_observed=10, media_observations_alive=0, cleanup_jobs_pending=0),
        dict(event='product_auto_share_ready', time_ms=0)], []
    for rows in (native, flutter):
        rows.extend([dict(event='product_audio_state', muted=True, time_ms=10000),
                     dict(event='product_audio_state', muted=False, time_ms=15000),
                     dict(event='product_audio_control', muted=True, time_ms=10000),
                     dict(event='product_audio_control', muted=False, time_ms=15000),
                     dict(event='product_closed', time_ms=100000)])
        for cycle in (1, 2, 3):
            for action in ('stop', 'retired', 'restart'):
                rows.append(dict(event='product_share_' + action, cycle=cycle,
                                 sender_video_bindings=0, time_ms=cycle * 20000))
    for index in range(4):
        for count in (1, 10):
            native.append(dict(event='product_rtp', direction='rx', kind='video', track=str(index), framesDecoded=count, time_ms=100))
            flutter.append(dict(event='rtp', direction='rx', kind='VIDEO', track=str(index), counters={'framesDecoded': count}, time_ms=100))
        native.append(dict(event='product_media_status', tracks=[dict(binding_id=index * 2 + int(rx), receiving=rx,
            video=True, protected=True) for rx in (False, True)], time_ms=100))
        for direction in ('rx', 'tx'):
            flutter.append(dict(event='crypto_state', direction=direction, track='TR_V' + str(index), state='kOk', time_ms=100))
    for time, samples in [(1000, 1), (2000, 100), (18000, 200), (19000, 300)]:
        native.append(dict(event='product_pcm', samples=samples, time_ms=time))
        flutter.append(dict(event='product_pcm', samples=samples, time_ms=time))
    for row in flutter:
        row['time_utc'] = datetime.fromtimestamp(row.pop('time_ms') / 1000, timezone.utc).isoformat()
    return native, flutter


class LifecycleVerdictTests(unittest.TestCase):
    def test_complete_evidence_passes(self):
        self.assertTrue(all(evaluate_lifecycle(*evidence(), [0, 0]).values()))

    def test_missing_or_live_shutdown_owners_fail(self):
        for name in ('rooms', 'managers', 'media_observations'):
            native, flutter = evidence()
            native[0][name + '_alive'] = 1
            self.assertFalse(evaluate_lifecycle(native, flutter, [0, 0])['native_shutdown_owner_retirement'])
        native, flutter = evidence()
        self.assertFalse(evaluate_lifecycle(native[1:], flutter, [0, 0])['native_shutdown_owner_retirement'])
        native, flutter = evidence()
        native[0]['cleanup_jobs_pending'] = 1
        self.assertFalse(evaluate_lifecycle(native, flutter, [0, 0])['native_cleanup_queue_drained'])
        native, flutter = evidence()
        native.append(dict(event='product_auto_share_ready', time_ms=500))
        self.assertFalse(evaluate_lifecycle(native, flutter, [0, 0])['native_auto_share_once'])

    def test_no_post_unmute_pcm_fails(self):
        native, flutter = evidence()
        native = [e for e in native if not (e['event'] == 'product_pcm' and e['time_ms'] > 15000)]
        self.assertFalse(evaluate_lifecycle(native, flutter, [0, 0])['native_decoded_audio_after'])

    def test_surviving_sender_binding_and_reused_receivers_fail(self):
        native, flutter = evidence()
        for event in native:
            if event['event'] == 'product_share_retired': event['sender_video_bindings'] = 1
            if event['event'] == 'product_rtp': event['track'] = 'same-track'
        checks = evaluate_lifecycle(native, flutter, [0, 0])
        self.assertFalse(checks['native_sender_binding_retirement'])
        self.assertFalse(checks['native_four_distinct_received_videos'])


if __name__ == '__main__': unittest.main()
