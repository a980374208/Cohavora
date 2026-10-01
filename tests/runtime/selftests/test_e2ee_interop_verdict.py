"""Offline checks: decoded media alone must never pass an encrypted run."""
import importlib.util
from pathlib import Path
import unittest

script = Path(__file__).resolve().parents[1] / "tools/media/invoke_e2ee_interop.py"
spec = importlib.util.spec_from_file_location("e2ee_interop", script)
interop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(interop)


def observed():
    native = [{"event": "closed"}, {"event": "decode_counts", "video_frames": 5, "audio_samples": 480}]
    flutter = [{"event": "closed"}]
    for kind in ["audio", "video"]:
        flutter.append({"event": "rtp", "kind": kind.upper(), "direction": "rx",
                        "counters": {"framesDecoded": 5, "totalSamplesReceived": 480}})
        for direction in ["tx", "rx"]:
            native.append({"event": "crypto_counts", "kind": kind, "direction": direction, "input": 5, "output": 5})
    for direction in ["tx", "rx"]:
        flutter.append({"event": "crypto_state", "direction": direction, "state": "kOk"})
    for events in [native, flutter]:
        for event in ["data_received", "stream_received"]:
            events.append({"event": event, "content_valid": True})
    return native, flutter


class VerdictTests(unittest.TestCase):
    def test_relay_marker_requires_content_first_frame_and_progress(self):
        rows = [dict(event='decode_counts', video_frames=n, relay_first_frame_correct=True,
            relay_marker_invalid=0, relay_marker_advances=n) for n in (1, 8)]
        self.assertTrue(all(interop.relay_marker_checks(rows).values()))
        self.assertFalse(all(interop.relay_marker_checks([]).values()))
        for changes in [dict(relay_first_frame_correct=False), dict(relay_marker_invalid=1),
                        dict(relay_marker_advances=1)]:
            self.assertFalse(all(interop.relay_marker_checks([rows[0], dict(rows[1], **changes)]).values()))

    def test_rpc_needs_both_handlers_and_both_valid_responses(self):
        native, flutter = observed()
        self.assertFalse(all(interop.rpc_checks(native, flutter).values()))
        for rows in [native, flutter]:
            rows.extend({'event': event, 'content_valid': True} for event in ['rpc_handler', 'rpc_response'])
        self.assertTrue(all(interop.rpc_checks(native, flutter).values()))
        flutter.append({'event': 'rpc_failed'})
        self.assertFalse(all(interop.rpc_checks(native, flutter).values()))

    def test_product_state_requires_authenticated_current_binding_and_typed_id(self):
        event = {'event': 'product_crypto_state', 'direction': 'rx', 'kind': 'audio',
                 'track_sid': 'TR_A', 'state': 6, 'binding_generation': 2, 'observed_room_generation': 2}
        key = 'product_state_rx_audio'
        self.assertFalse(interop.product_media_state_checks([event])[key])
        event['state'] = 1
        self.assertTrue(interop.product_media_state_checks([event])[key])
        event['observed_room_generation'] = 3
        self.assertFalse(interop.product_media_state_checks([event])[key])
        event['observed_room_generation'] = 2; event['track_sid'] = ''
        event['native_track_id'] = 'native-a'
        self.assertFalse(interop.product_media_state_checks([event])[key])

    def test_flutter_reconnect_requires_new_progress_not_reset_or_concealment(self):
        events = [{'event': 'rtp', 'kind': 'audio', 'direction': 'rx',
                   'generation': generation, 'track': 'a',
                   'counters': {'totalSamplesReceived': value, 'concealedSamples': concealed}}
                  for generation, value, concealed in [(1, 100, 0), (2, 10, 10), (2, 20, 20)]]
        key = 'flutter_post_reconnect_rx_audio'
        self.assertFalse(interop.flutter_reconnect_progress(events)[key])
        events[-1]['counters']['totalSamplesReceived'] = 30
        self.assertTrue(interop.flutter_reconnect_progress(events)[key])
        self.assertFalse(interop.flutter_reconnect_progress(events)['flutter_post_reconnect_rx_video'])

    def test_every_key_epoch_requires_bidirectional_progress(self):
        native, flutter = [], []
        for epoch in range(1, 4):
            for name, events in [("native", native), ("flutter", flutter)]:
                events.append({"event": "key_changed", "key_epoch": epoch})
                for value in [1, 2]:
                    for event in ["data_received", "stream_received"]:
                        events.append({"event": event, "key_epoch": epoch, "content_valid": True})
                    for kind in ["audio", "video"]:
                        for direction in ["tx", "rx"]:
                            events.append({"event": "crypto_counts" if name == "native" else "rtp",
                                "key_epoch": epoch, "kind": kind, "direction": direction,
                                "output": value, "counters": {key: value for key in
                                    ["framesDecoded", "totalSamplesReceived", "framesEncoded", "packetsSent"]}})
        self.assertTrue(all(interop.key_transition_checks(native, flutter, 3).values()))
        # Good final-epoch observations cannot hide an earlier stalled receiver.
        for event in flutter:
            if event.get("key_epoch") == 2 and event.get("direction") == "rx" and event.get("kind") == "video":
                event["counters"]["framesDecoded"] = 0
        checks = interop.key_transition_checks(native, flutter, 3)
        self.assertFalse(checks["epoch2_flutter_post_key_rx_video"])
        self.assertTrue(checks["epoch3_flutter_post_key_rx_video"])

    def test_reconnect_counter_reset_is_not_key_transition_progress(self):
        events = [{"event": "crypto_counts", "key_epoch": 1, "kind": "video", "direction": "rx",
                   "generation": generation, "track": "track", "output": value}
                  for generation, value in [(1, 100), (2, 0), (2, 0)]]
        self.assertFalse(interop.key_transition_checks(events, [], 1)["native_post_key_rx_video"])
        events[-1]["output"] = 1
        self.assertTrue(interop.key_transition_checks(events, [], 1)["native_post_key_rx_video"])

    def test_requested_codec_and_unassociated_crypto_state_are_not_proof(self):
        native, flutter = observed()
        for event in native + flutter: event['requested_codec'] = 'vp8'
        checks = interop.observed_profile_checks(native, flutter)
        self.assertFalse(any(checks.values()))

    def test_codec_and_crypto_are_verified_per_track_and_direction(self):
        native, flutter = [], []
        for kind, mime in [('audio', 'audio/opus'), ('video', 'video/vp8')]:
            for direction in ['tx', 'rx']:
                track = kind + direction
                native.append({'event': 'crypto_counts', 'kind': kind, 'direction': direction,
                               'input': 5, 'observed_codecs': [mime]})
                flutter += [{'event': 'rtp', 'kind': kind, 'direction': direction,
                             'observed_codec': mime, 'track': track},
                            {'event': 'crypto_state', 'direction': direction, 'track': track, 'state': 'kOk'}]
        self.assertTrue(all(interop.observed_profile_checks(native, flutter).values()))
        self.assertFalse(interop.observed_profile_checks(native, flutter, 'h264')['native_observed_tx_video_codec'])
        flutter = [e for e in flutter if not (e['event'] == 'crypto_state' and e['track'] == 'videorx')]
        self.assertFalse(interop.observed_profile_checks(native, flutter)['flutter_crypto_rx_video'])
        native[-1]['observed_codecs'].append('unknown')
        self.assertFalse(interop.observed_profile_checks(native, flutter)['native_observed_rx_video_codec'])

    def test_official_enum_case_and_complete_observations(self):
        self.assertTrue(all(interop.evaluate(*observed(), "on", [0, 0]).values()))

    def test_decode_without_native_crypto_cannot_pass(self):
        native, flutter = observed()
        native = [e for e in native if e["event"] != "crypto_counts"]
        self.assertFalse(all(interop.evaluate(native, flutter, "on", [0, 0]).values()))

    def test_one_way_video_does_not_pass(self):
        native, flutter = observed()
        for event in native:
            if event.get("kind") == "video" and event.get("direction") == "rx": event["output"] = 0
        self.assertFalse(interop.evaluate(native, flutter, "on", [0, 0])["native_crypto_rx_video"])

    def test_close_event_does_not_hide_process_crash(self):
        self.assertFalse(interop.evaluate(*observed(), "on", [0, 0xC0000005])["process_exit"])

    def test_uncaught_sampler_failure_is_not_ignored(self):
        native, flutter = observed()
        flutter.append({"event": "sample_failed", "type": "StateError"})
        self.assertFalse(interop.evaluate(native, flutter, "on", [0, 0])["no_operation_failure"])

    def test_missing_content_evidence_cannot_pass(self):
        native, flutter = observed()
        flutter = [e for e in flutter if e["event"] != "stream_received"]
        self.assertFalse(interop.evaluate(native, flutter, "on", [0, 0])["flutter_stream_received"])

    def test_required_channel_guard_needs_both_native_connections(self):
        self.assertFalse(interop.native_channel_guard_check([]))
        events = [{'event': 'native_channel_guard', 'publisher_required': True, 'subscriber_required': True}]
        self.assertTrue(interop.native_channel_guard_check(events))
        events.append({'event': 'native_channel_guard', 'publisher_required': True, 'subscriber_required': False})
        self.assertFalse(interop.native_channel_guard_check(events))

    def test_unbound_guard_requires_arriving_media_and_zero_real_decode(self):
        native, flutter = observed()
        native = [e for e in native if e['event'] != 'crypto_counts']
        for e in native:
            if e['event'] == 'decode_counts': e['video_frames'] = 0
        native.append({'event': 'native_channel_guard', 'publisher_required': True, 'subscriber_required': True})
        self.assertFalse(all(interop.unbound_channel_checks(native, flutter, [0, 0]).values()))
        native += [dict(event='guard_inbound_rtp', kind='video', packets=7, packets_available=True,
                        frames=0, frames_available=True),
                   dict(event='guard_inbound_rtp', kind='audio', packets=7, packets_available=True,
                        samples=480, samples_available=True, concealed=480, concealed_available=True)]
        self.assertTrue(all(interop.unbound_channel_checks(native, flutter, [0, 0]).values()))
        native[-1]['samples'] += 1
        self.assertFalse(interop.unbound_channel_checks(native, flutter, [0, 0])['plaintext_audio_not_decoded'])
        native[-2]['frames'] = 1
        self.assertFalse(interop.unbound_channel_checks(native, flutter, [0, 0])['plaintext_video_not_decoded'])

    def test_concealment_alone_is_not_audio_decode_proof(self):
        native, flutter = observed()
        for event in flutter:
            if event.get("kind") == "AUDIO": event["counters"]["concealedSamples"] = 480
        self.assertFalse(interop.evaluate(native, flutter, "on", [0, 0])["flutter_decoded_audio"])

    def test_negative_interval_ends_when_first_sender_changes(self):
        native = [{"event": "key_changed", "time_ms": 1000}]
        flutter = [
            {"event": "rtp", "time_utc": "1970-01-01T00:00:00.900Z", "key_epoch": 0},
            {"event": "data_received", "time_utc": "1970-01-01T00:00:01.100Z", "key_epoch": 0},
            {"event": "key_changed", "time_utc": "1970-01-01T00:00:02.000Z", "key_epoch": 1}]
        before_native, before_flutter = interop.negative_phase(native, flutter)
        self.assertEqual(before_native, [])
        self.assertEqual([e["event"] for e in before_flutter], ["rtp"])


if __name__ == "__main__":
    unittest.main()
