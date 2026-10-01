"""Product acceptance must include current-install protection, not just rendering."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/media'))
from invoke_e2ee_product import evaluate, evaluate_pair, evaluate_late, evaluate_reconnect, evaluate_inflight, evaluate_flutter_board, evaluate_steady, evaluate_byte_streams

def evidence():
    product = [dict(event='product_state', state=5, time_ms=1),
        dict(event='product_recovery_submitted', time_ms=50),
        dict(event='product_recovery_result', installed=True, time_ms=51),
        dict(event='product_closed', time_ms=100)]
    flutter = [dict(event='closed', time_ms=100)]
    for epoch, times in [(4, [10, 20]), (6, [70, 80])]:
        for t in times:
            product.append(dict(event='product_media_status', time_ms=t, epoch=epoch, tracks=[
                dict(receiving=rx, video=video, protected=True) for rx in [False, True] for video in [False, True]]))
            product.append(dict(event='product_render', time_ms=t, submits=t))
            for kind in ['AUDIO', 'VIDEO']:
                flutter.append(dict(event='rtp', time_ms=t, direction='rx', track=kind, kind=kind,
                    counters={'totalSamplesReceived' if kind == 'AUDIO' else 'framesDecoded': t}))
    return product, flutter

class ProductVerdictTests(unittest.TestCase):
    def test_byte_streams_require_every_epoch_and_both_concurrent_streams(self):
        rows = []
        for epoch in (4, 6):
            rows.append(dict(event='product_byte_sent', epoch=epoch, streams=2))
            rows.extend(dict(event='product_byte_received', epoch=epoch, index=index, valid=True) for index in (0, 1))
        self.assertTrue(all(evaluate_byte_streams(rows, rows).values()))
        self.assertFalse(all(evaluate_byte_streams(rows, rows[:-1]).values()))
        duplicate = [dict(e) for e in rows]
        duplicate[-1]['index'] = 0
        self.assertFalse(all(evaluate_byte_streams(duplicate, rows).values()))
        corrupt = [dict(e) for e in rows]
        corrupt[-1]['valid'] = False
        self.assertFalse(all(evaluate_byte_streams(rows, corrupt).values()))
        self.assertFalse(all(evaluate_byte_streams(rows + [dict(event='product_byte_failed')], rows).values()))

    def test_text_streams_cannot_borrow_byte_evidence(self):
        byte_rows = []
        for epoch in (4, 6):
            byte_rows.append(dict(event='product_byte_sent', epoch=epoch, streams=2))
            byte_rows.extend(dict(event='product_byte_received', epoch=epoch, index=i, valid=True) for i in (0, 1))
        self.assertFalse(all(evaluate_byte_streams(byte_rows, byte_rows, 'text').values()))
        text_rows = [dict(e, event=e['event'].replace('_byte_', '_text_')) for e in byte_rows]
        self.assertTrue(all(evaluate_byte_streams(text_rows, text_rows, 'text').values()))
        for bad in (text_rows[:-1], text_rows + [text_rows[-1]],
                    text_rows + [dict(event='product_text_failed')]):
            self.assertFalse(all(evaluate_byte_streams(text_rows, bad, 'text').values()))
        corrupt = [dict(e) for e in text_rows]
        corrupt[-1]['valid'] = False
        self.assertFalse(all(evaluate_byte_streams(corrupt, text_rows, 'text').values()))

    def test_partial_board_requires_receipt_and_rotation_before_expiry(self):
        product = [dict(event='product_whiteboard_observed', epoch=epoch,
                        object_valid=True, flutter_object_valid=True, asset_valid=epoch == 6)
                   for epoch in (4, 6)]
        product += [dict(event='product_board_partial_received', time_ms=1000),
                    dict(event='product_recovery_submitted', time_ms=2000),
                    dict(event='product_board_old_tail_received', time_ms=3000)]
        flutter = [dict(event='product_board_authority_sent', epoch=epoch) for epoch in (4, 6)]
        flutter += [dict(event='product_board_partial_sent', epoch=4),
                    dict(event='product_board_old_tail_sent', epoch=6, time_utc='1970-01-01T00:00:03+00:00')]
        self.assertTrue(all(evaluate_flutter_board(product, flutter, True, True).values()))
        product[0]['asset_valid'] = True
        self.assertFalse(evaluate_flutter_board(product, flutter, True, True)['native_board_4_asset_valid'])
        product[0]['asset_valid'] = False
        flutter[-1]['time_utc'] = '1970-01-01T00:00:12+00:00'
        self.assertFalse(evaluate_flutter_board(product, flutter, True, True)['rotation_before_asset_expiry'])

    def test_off_gate_does_not_reuse_required_protection_verdict(self):
        events = [dict(event='product_state', state=5), dict(event='product_closed'),
            dict(event='product_mode', manager_present=False),
            dict(event='product_media_status', enabled=False, tracks=[])]
        for count in (1, 20):
            events.append(dict(event='product_render', submits=count, bindings=1))
            events.append(dict(event='product_metrics', inboundVideoFramesDecoded=count))
            events.append(dict(event='product_rtp', direction='rx', kind='video', track='video1', framesDecoded=count))
        self.assertTrue(all(evaluate_steady(events, False, business=False).values()))
        self.assertFalse(all(evaluate_steady(events, True, business=False).values()))
        events.append(dict(event='product_render', submits=0, time_ms=101))
        events.append(dict(event='product_metrics', inboundVideoFramesDecoded=0, time_ms=101))
        self.assertFalse(evaluate_steady(events, False, business=False)['renders_progress'])
        self.assertTrue(all(evaluate_steady(events, False, business=False, media_until=100).values()))
        events.append(dict(event='product_recovery_submitted'))
        self.assertFalse(evaluate_steady(events, False, business=False)['no_recovery'])
    def test_flutter_board_requires_content_and_actual_native_projection(self):
        p, f = [], []
        for epoch in (4, 6):
            p.append(dict(event='product_whiteboard_observed', epoch=epoch,
                object_valid=True, asset_valid=True, flutter_object_valid=True))
            for kind in ('operation_received', 'snapshot_received', 'asset_received'):
                f.append(dict(event='product_board_' + kind, epoch=epoch, valid=True))
        self.assertTrue(all(evaluate_flutter_board(p, f, False).values()))
        f[-1]['valid'] = False
        self.assertFalse(all(evaluate_flutter_board(p, f, False).values()))
        self.assertFalse(all(evaluate_flutter_board([], f, True).values()))
    def test_inflight_requires_partial_then_recovery_and_explicit_cancellation(self):
        events = [dict(event='product_inflight_' + name, time_ms=i + 1)
            for i, name in enumerate(('sent', 'partial_received', 'send_cancelled', 'receive_cancelled'))]
        events.append(dict(event='product_recovery_submitted', time_ms=5))
        self.assertTrue(all(evaluate_inflight(events).values()))
        self.assertFalse(all(evaluate_inflight(events[1:]).values()))
        events.append(dict(event='product_inflight_completed', time_ms=6))
        self.assertFalse(evaluate_inflight(events)['inflight_no_delivery'])
    def test_camera_requires_camera_publish_and_two_native_decoders(self):
        p, f = evidence()
        self.assertFalse(evaluate(p, f, [0, 0], camera='flutter')['flutter_camera_published'])
        f.append(dict(event='published', kind='video', source='camera'))
        self.assertTrue(all(evaluate(p, f, [0, 0], camera='flutter').values()))
        self.assertFalse(evaluate(p, f, [0, 0], camera='native')['camera_4_flutter_two_video_decoders'])
        for e in p:
            if e['event'] == 'product_media_status':
                e['tracks'].append(dict(receiving=False, video=True, protected=True))
        f.extend([dict(e, track='second-video') for e in list(f) if e['event'] == 'rtp' and e.get('kind') == 'VIDEO'])
        self.assertTrue(all(evaluate(p, f, [0, 0], camera='native').values()))
    def test_flutter_business_requires_both_deliveries_and_content(self):
        p, f = evidence()
        self.assertFalse(all(evaluate(p, f, [0, 0], business=True).values()))
        for events in (p, f):
            for epoch in (4, 6):
                for kind in ('chat', 'file'):
                    events.append(dict(event=f'product_{kind}_received', epoch=epoch, valid=True, time_ms=20 if epoch == 4 else 80))
        self.assertTrue(all(evaluate(p, f, [0, 0], business=True).values()))
        f[-1]['valid'] = False
        self.assertFalse(evaluate(p, f, [0, 0], business=True)['flutter_6_file'])
    def test_complete_scoped_evidence(self):
        p, f = evidence()
        self.assertTrue(all(evaluate(p, f, [0, 0]).values()))
    def test_old_epoch_cannot_pass_recovery(self):
        p, f = evidence()
        for e in p:
            if e['event'] == 'product_media_status': e['epoch'] = 4
        self.assertFalse(evaluate(p, f, [0, 0])['after_product_rx_video'])
    def test_render_and_ok_without_protected_frame_fail(self):
        p, f = evidence()
        for e in p:
            for t in e.get('tracks', []): t.update(protected=False, report=1)
        self.assertFalse(evaluate(p, f, [0, 0])['before_product_rx_video'])
    def test_install_alone_and_static_decodes_fail(self):
        p, f = evidence()
        for e in f:
            if e['event'] == 'rtp': e['counters'] = {'framesDecoded': 10, 'totalSamplesReceived': 10}
        self.assertFalse(evaluate(p, f, [0, 0])['after_flutter_decodes_video'])
    def test_empty_and_nonzero_exit_fail(self):
        self.assertFalse(all(evaluate([], [], [0, 0]).values()))
        p, f = evidence()
        self.assertFalse(evaluate(p, f, [0, 1])['process_exit'])

    def test_pair_requires_remote_content_each_phase(self):
        p, _ = evidence()
        for epoch in (4, 6):
            for kind in ('chat', 'file'):
                p.append(dict(event=f'product_{kind}_received', epoch=epoch, valid=True))
        self.assertTrue(all(evaluate_pair(p, p, [0, 0]).values()))
        missing = [e for e in p if not (e['event'] == 'product_file_received' and e['epoch'] == 6)]
        self.assertFalse(evaluate_pair(p, missing, [0, 0])['peer1_6_file'])
        invalid = [dict(e, valid=False) if e['event'] == 'product_chat_received' else e for e in p]
        self.assertFalse(evaluate_pair(p, invalid, [0, 0])['peer1_4_chat'])
        self.assertFalse(evaluate_pair(p, p, [0, 0], whiteboard=True)['peer1_6_asset_valid'])
        for epoch in (4, 6):
            p.append(dict(event='product_whiteboard_observed', epoch=epoch, object_valid=True, asset_valid=True))
        self.assertTrue(all(evaluate_pair(p, p, [0, 0], whiteboard=True).values()))

    def test_late_peer_requires_two_sources_and_snapshot_assets(self):
        p, _ = evidence()
        for e in p:
            if e['event'] == 'product_media_status': e['tracks'] *= 2
        for epoch in (4, 6):
            p.append(dict(event='product_whiteboard_observed', epoch=epoch, object_valid=True, asset_valid=True))
        for kind in ('chat', 'file'):
            p.extend([dict(event=f'product_{kind}_received', epoch=6, valid=True) for _ in range(2)])
        self.assertTrue(all(evaluate_late(p).values()))
        for e in p:
            if e['event'] == 'product_media_status': e['tracks'] = e['tracks'][:4]
        self.assertFalse(evaluate_late(p)['late_6_two_rx_videoTrue'])
        self.assertFalse(all(evaluate_late([]).values()))

    def test_reconnect_rejects_stale_generation_protection(self):
        p, _ = evidence()
        p.extend([dict(event='product_reconnect_requested', time_ms=30),
            dict(event='product_state', state=6, time_ms=31), dict(event='product_state', state=5, time_ms=40)])
        for e in p:
            if e['event'] == 'product_media_status': e['generation'] = 1 if e['epoch'] == 4 else 2
        self.assertTrue(all(evaluate_reconnect(p).values()))
        for e in p:
            if e['event'] == 'product_media_status': e['generation'] = 1
        self.assertFalse(evaluate_reconnect(p)['reconnect_new_generation_rxTrue_videoTrue'])

    def test_wrong_key_requires_actual_attempts_and_zero_delivery(self):
        p, _ = evidence()
        for e in p:
            if e['event'] == 'product_media_status' and e['epoch'] == 4:
                for t in e['tracks']:
                    if t['receiving']: t['protected'] = False
            if e['event'] == 'product_render' and e['time_ms'] < 50: e['submits'] = 0
        p.append(dict(next(e for e in p if e['event'] == 'product_media_status'), time_ms=25))
        p.append(dict(event='product_business_sent', epoch=4))
        for kind in ('chat', 'file'): p.append(dict(event=f'product_{kind}_received', epoch=6, valid=True))
        self.assertTrue(all(evaluate_pair(p, p, [0, 0], wrong_key=True).values()))
        leaked = p + [dict(event='product_chat_received', epoch=4, valid=True)]
        self.assertFalse(evaluate_pair(p, leaked, [0, 0], wrong_key=True)['peer1_4_chat'])

if __name__ == '__main__': unittest.main()
