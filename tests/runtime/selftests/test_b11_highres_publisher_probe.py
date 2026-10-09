"""Offline live-sender evidence, freshness, binding and adaptive RID correlation."""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch


TOOLS = Path(__file__).resolve().parents[1] / "tools" / "meeting"
sys.path.insert(0, str(TOOLS))
try:
    import b11_highres_publisher_probe as probe
    import b11_highres_input_freeze as freeze
finally:
    sys.path.remove(str(TOOLS))


SID_HASH = "a" * 16


def source(ready, kind="camera"):
    if kind in ("camera4k", "screen4k"):
        return {"scenario":kind,"target_identity":"b11highres_fixture_source",
            "source":copy.deepcopy(freeze.SOURCES[kind]),"layers":copy.deepcopy(freeze.LAYERS[kind]),
            "artifacts":{"publisher_ready":{"path":str(ready)}}}
    layers = {"low": {"rid": "q", "width": 320, "height": 180, "source_fps": 15},
              "medium": {"rid": "h", "width": 640, "height": 360, "source_fps": 20},
              "high": {"rid": "f", "width": 2560, "height": 1440, "source_fps": 30}}
    if kind == "screen":
        layers = {"low": {"rid": "q", "width": 1280, "height": 720, "source_fps": 3},
                  "high": {"rid": "h", "width": 2560, "height": 1440, "source_fps": 20}}
    return {"scenario": kind, "target_identity": "b11highres_fixture_source",
            "source": {"source": "camera" if kind == "camera" else "screen_share",
                       "width": 2560, "height": 1440, "source_fps": 30 if kind == "camera" else 20},
            "layers": layers, "artifacts": {"publisher_ready": {"path": str(ready)}}}


def stream(rid="q", width=240, height=135, fps=15):
    return {"rid": rid, "width": width, "height": height, "fps": fps, "frames_encoded": 150,
            "width_available": True, "height_available": True, "fps_available": True,
            "frames_encoded_available": True}


def raw_sample(value, elapsed=99, outbound=None):
    result = {"schema": 1, "event": "sample", "identity_hash": hashlib.sha256(
                value["target_identity"].encode()).hexdigest()[:16],
            "publication_sid_hash": SID_HASH, "source_kind": freeze.scenario_kind(value["scenario"]),
            "source_width": value["source"]["width"], "source_height": value["source"]["height"],
            "target_fps": value["source"]["source_fps"], "elapsed_seconds": elapsed,
            "captured_frames": 3000, "outbound": [stream()] if outbound is None else outbound}
    if value["scenario"].endswith("4k"):
        result.update(capture_backend="wgc_window",source_scope="owned_window_native_fixture",
            capturer_id=1,capture_frames=3000,capture_size_mismatches=0)
    return result


def track(value, width=240, height=135):
    return {"sid_hash": SID_HASH, "source": value["source"]["source"], "sink_active": True,
            "sink_frame_dimensions_available": True, "sink_frame_width": width, "sink_frame_height": height}


class PublisherProbeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="b11-publisher-probe-selftest-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ready = self.root / "source-ready.json"
        self.ready.write_text("{}", encoding="utf-8")
        os.utime(self.ready, (900, 900))
        self.raw = self.root / ".publisher-stdout.tmp"
        self.raw.write_bytes(b"")
        self.source = source(self.ready)
        self.clock = patch.object(probe.time, "time", return_value=1000)
        self.clock.start()
        self.addCleanup(self.clock.stop)
        self.monotonic = patch.object(probe.time, "monotonic", return_value=500)
        self.monotonic_clock = self.monotonic.start()
        self.addCleanup(self.monotonic.stop)

    def sampler(self, value=None):
        sampler = probe.PublisherProbeSampler(self.raw, self.source if value is None else value, SID_HASH)
        self.addCleanup(sampler.close)
        return sampler

    def append(self, value):
        with self.raw.open("ab") as output:
            output.write(b"SOURCE_PROBE " + json.dumps(value).encode() + b"\n")

    def safe(self, outbound=None, value=None):
        value = self.source if value is None else value
        self.append(raw_sample(value, outbound=outbound))
        return self.sampler(value).sample(500)

    def test_partial_line_waits_for_newline_and_incrementally_reads_only_appended_bytes(self):
        row = b"SOURCE_PROBE " + json.dumps(raw_sample(self.source)).encode()
        self.raw.write_bytes(b"unsafe native detail\nSOURCE_PROBE {\"event\":\"ready\"}\n" + row[:45])
        sampler = self.sampler()
        self.assertIsNone(sampler.sample(500))
        with self.raw.open("ab") as output:
            output.write(row[45:])
        self.assertIsNone(sampler.sample(500))
        with self.raw.open("ab") as output:
            output.write(b"\r\nSOURCE_PROBE {\"event\":\"result\"}\n")
        self.assertEqual(sampler.sample(500)["outbound"][0]["width"], 240)
        # Re-reading an unchanged file would violate elapsed monotonicity.
        self.monotonic_clock.return_value = 501
        self.assertEqual(sampler.sample(501)["age_seconds"], 2)

    def test_ready_anchor_rejects_already_old_samples_and_never_refreshes_cached_evidence(self):
        self.append(raw_sample(self.source, elapsed=90))
        self.assertIsNone(self.sampler().sample(500))
        self.raw.write_bytes(b"")
        self.append(raw_sample(self.source))
        sampler = self.sampler()
        first = sampler.sample(500)
        self.assertEqual(first["age_seconds"], 1)
        first["outbound"][0]["width"] = 999
        self.monotonic_clock.return_value = 502
        self.assertEqual(sampler.sample(502)["outbound"][0]["width"], 240)
        self.monotonic_clock.return_value = 503.001
        self.assertIsNone(sampler.sample(503.001))
        self.append(raw_sample(self.source, elapsed=104))
        self.monotonic_clock.return_value = 505
        self.assertEqual(sampler.sample(505)["age_seconds"], 1)

    def test_sample_identity_publication_source_dimensions_and_fps_are_strictly_bound(self):
        mutations = {"identity_hash": "b" * 16, "publication_sid_hash": "c" * 16,
                     "source_kind": "screen", "source_width": 1280, "source_height": 720,
                     "target_fps": 20}
        for name, value in mutations.items():
            with self.subTest(field=name):
                self.raw.write_bytes(b"")
                row = raw_sample(self.source)
                row[name] = value
                self.append(row)
                with self.assertRaisesRegex(ValueError, "^publisher_probe_invalid_sample$"):
                    self.sampler().sample(500)

    def test_four_k_wgc_camera_and_screen_have_real_kind_and_full_dimensions(self):
        for scenario, rid, fps in (("camera4k","f",30),("screen4k","h",20)):
            self.raw.write_bytes(b"")
            value = source(self.ready,scenario)
            sample = self.safe(value=value,outbound=[stream(rid,3840,2160,fps)])
            self.assertEqual(sample["source_kind"],freeze.scenario_kind(scenario))
            self.assertEqual(sample["capture_backend"],"wgc_window")
            self.assertEqual(sample["capturer_id"],1)
            self.assertEqual(probe.match_layer(track(value,3840,2160),sample,value),"high")

    def test_four_k_probe_rejects_synthetic_wrong_capture_and_two_k_source(self):
        value = source(self.ready,"camera4k")
        for mutation in ({"source_width":2560},{"source_height":1440},
                {"capture_backend":"synthetic_i420"},{"source_scope":"screen_share_session"},
                {"capturer_id":True},{"capturer_id":0},{"capture_frames":2999},{"capture_size_mismatches":1}):
            self.raw.write_bytes(b"")
            raw = raw_sample(value,outbound=[stream("f",3840,2160,30)])
            raw.update(mutation)
            self.append(raw)
            with self.subTest(mutation=mutation),self.assertRaisesRegex(ValueError,"^publisher_probe_invalid_sample$"):
                self.sampler(value).sample(500)

    def test_four_k_independent_counters_allow_inflight_capture_and_reject_regression(self):
        value = source(self.ready,"camera4k")
        raw = raw_sample(value,outbound=[stream("f",3840,2160,30)])
        raw["capture_frames"] = 4000
        self.append(raw)
        sampler = self.sampler(value)
        sample = sampler.sample(500)
        self.assertEqual(sample["captured_frames"],3000)
        self.assertEqual(sample["capture_frames"],4000)
        self.assertEqual(probe.match_layer(track(value,3840,2160),sample,value),"high")
        raw.update(elapsed_seconds=100,captured_frames=3001,capture_frames=3999)
        self.append(raw)
        self.monotonic_clock.return_value = 501
        with self.assertRaisesRegex(ValueError,"publisher_probe_backend_capture_counter_regressed"):
            sampler.sample(501)

    def test_only_whitelist_fields_survive_and_unknown_rid_is_sanitized(self):
        item = stream(rid="unknown_sender_rid")
        item.update(stats_stream_hash="unsafe stream", bytes_sent=100, arbitrary="secret value")
        row = raw_sample(self.source, outbound=[item])
        row.update(raw_identity="unsafe identity", credential="secret value")
        self.append(row)
        sample = self.sampler().sample(500)
        self.assertEqual(set(sample), {"age_seconds", "identity_hash", "publication_sid_hash", "source_kind",
                                      "elapsed_seconds", "captured_frames", "outbound"})
        self.assertEqual(set(sample["outbound"][0]), set(stream()))
        self.assertEqual(sample["outbound"][0]["rid"], "")
        self.assertNotIn("unsafe", json.dumps(sample))
        self.assertNotIn("secret", json.dumps(sample))
        self.assertIsNone(probe.match_layer(track(self.source), sample, self.source))

    def test_failure_and_invalid_fields_raise_safe_errors_and_stay_failed(self):
        rows = [({"event": "failure", "code": "raw-secret-failure"}, "publisher_probe_sender_failed"),
                ({**raw_sample(self.source), "captured_frames": True}, "publisher_probe_invalid_sample"),
                (raw_sample(self.source, outbound=[{**stream(), "fps": float("nan")}]), "publisher_probe_invalid_stream"),
                (raw_sample(self.source, outbound=[{**stream(), "fps_available": 1}]), "publisher_probe_invalid_stream")]
        for row, reason in rows:
            with self.subTest(reason=reason):
                self.raw.write_bytes(b"")
                self.append(row)
                sampler = self.sampler()
                for _ in range(2):
                    with self.assertRaisesRegex(ValueError, "^" + reason + "$") as caught:
                        sampler.sample(500)
                    self.assertNotIn("raw-secret", str(caught.exception))

    def test_future_elapsed_and_repeated_or_regressed_elapsed_do_not_produce_evidence(self):
        self.append(raw_sample(self.source, elapsed=101))
        with self.assertRaisesRegex(ValueError, "publisher_probe_future_sample"):
            self.sampler().sample(500)
        for elapsed in (99, 98):
            with self.subTest(elapsed=elapsed):
                self.monotonic_clock.return_value = 500
                self.raw.write_bytes(b"")
                self.append(raw_sample(self.source, elapsed=99))
                sampler = self.sampler()
                self.assertIsNotNone(sampler.sample(500))
                self.append(raw_sample(self.source, elapsed=elapsed))
                self.monotonic_clock.return_value = 501
                with self.assertRaisesRegex(ValueError, "publisher_probe_elapsed_not_increasing"):
                    sampler.sample(501)

    def test_line_completed_during_read_uses_completion_clock_without_future_grace(self):
        self.append(raw_sample(self.source, elapsed=100.006))
        with patch.object(probe.time, "monotonic", side_effect=[500, 500.010]):
            sample = self.sampler().sample(500)
        self.assertAlmostEqual(sample["age_seconds"], 0.004)
        self.raw.write_bytes(b"")
        self.append(raw_sample(self.source, elapsed=100.011))
        with patch.object(probe.time, "monotonic", side_effect=[500, 500.010]):
            with self.assertRaisesRegex(ValueError, "^publisher_probe_future_sample$"):
                self.sampler().sample(500)

    def test_read_time_is_included_in_freshness_and_clock_regression_fails_closed(self):
        self.append(raw_sample(self.source, elapsed=97.001))
        with patch.object(probe.time, "monotonic", side_effect=[500, 500.010]):
            self.assertIsNone(self.sampler().sample(500))
        with patch.object(probe.time, "monotonic", side_effect=[500, 499.999]):
            with self.assertRaisesRegex(ValueError, "^publisher_probe_invalid_clock$"):
                self.sampler().sample(500)

    def test_scheduled_gap_before_read_uses_actual_completion_clock(self):
        sampler = self.sampler()
        self.assertIsNone(sampler.sample(500))
        self.append(raw_sample(self.source, elapsed=100.008))
        with patch.object(probe.time, "monotonic", side_effect=[500.006, 500.010]):
            sample = sampler.sample(500)
        self.assertAlmostEqual(sample["age_seconds"], 0.002)

    def test_scheduled_gap_has_no_future_grace_or_freshness_extension(self):
        for elapsed, expected in ((100.011, "future"), (97.009, "stale")):
            with self.subTest(elapsed=elapsed):
                self.raw.write_bytes(b"")
                sampler = self.sampler()
                self.assertIsNone(sampler.sample(500))
                self.append(raw_sample(self.source, elapsed=elapsed))
                with patch.object(probe.time, "monotonic", side_effect=[500.006, 500.010]):
                    if expected == "future":
                        with self.assertRaisesRegex(ValueError, "^publisher_probe_future_sample$"):
                            sampler.sample(500)
                    else:
                        self.assertIsNone(sampler.sample(500))

    def test_first_anchor_uses_actual_entry_clock_and_rejects_clock_retreat(self):
        # Wall clock corresponds to entry 500.006, not the caller's earlier 500.
        self.append(raw_sample(self.source, elapsed=100.002))
        sampler = self.sampler()
        with patch.object(probe.time, "monotonic", side_effect=[500.006, 500.010]):
            self.assertAlmostEqual(sampler.sample(500)["age_seconds"], 0.002)
        with patch.object(probe.time, "monotonic", side_effect=[500.009, 500.010]):
            with self.assertRaisesRegex(ValueError, "^publisher_probe_invalid_clock$"):
                sampler.sample(500)

    def test_appending_live_file_is_bounded_to_initial_size_and_next_call_consumes_tail(self):
        sampler = self.sampler()
        self.assertIsNone(sampler.sample(500))
        self.append(raw_sample(self.source, elapsed=99))
        underlying = sampler._file
        wrapped = Mock(wraps=underlying)
        appended = False

        def read(count):
            nonlocal appended
            if not appended:
                appended = True
                self.append(raw_sample(self.source, elapsed=100.25))
            return underlying.read(count)

        wrapped.read.side_effect = read
        sampler._file = wrapped
        self.assertEqual(sampler.sample(500)["elapsed_seconds"], 99)
        self.monotonic_clock.return_value = 500.5
        self.assertEqual(sampler.sample(500.5)["elapsed_seconds"], 100.25)

    def test_safe_error_reason_accepts_exact_codes_and_never_raw_prefix_or_os_errors(self):
        self.assertEqual(probe.safe_error_reason(ValueError("publisher_probe_future_sample")),
                         "publisher_probe_future_sample")
        for error in (ValueError("publisher_probe_future_sample raw-secret"),
                      ValueError("publisher_probe_secret/token"), OSError("raw-secret")):
            self.assertIsNone(probe.safe_error_reason(error))

    def test_single_adaptive_low_stream_matches_below_announced_size_despite_inactive_higher_streams(self):
        sample = self.safe(outbound=[stream(), stream("h", 640, 360, 0), stream("f", 2560, 1440, 0)])
        self.assertEqual(probe.match_layer(track(self.source), sample, self.source), "low")
        self.assertNotEqual((sample["outbound"][0]["width"], sample["outbound"][0]["height"]), (320, 180))

    def test_active_equal_dimensions_are_ambiguous_even_for_an_unknown_or_duplicate_rid(self):
        for rid in ("h", "q", "unknown"):
            with self.subTest(rid=rid):
                self.raw.write_bytes(b"")
                sample = self.safe(outbound=[stream(), stream(rid)])
                self.assertIsNone(probe.match_layer(track(self.source), sample, self.source))

    def test_match_rechecks_freshness_source_publication_and_available_active_dimensions(self):
        sample = self.safe()
        for name, value in (("age_seconds", 3.001), ("identity_hash", "b" * 16),
                            ("publication_sid_hash", "b" * 16), ("source_kind", "screen")):
            with self.subTest(field=name):
                changed = {**sample, name: value}
                self.assertIsNone(probe.match_layer(track(self.source), changed, self.source))
        for name, value in (("width_available", False), ("height_available", False),
                            ("fps_available", False), ("fps", 0)):
            with self.subTest(field=name):
                changed = copy.deepcopy(sample)
                changed["outbound"][0][name] = value
                self.assertIsNone(probe.match_layer(track(self.source), changed, self.source))
        self.assertIsNone(probe.match_layer(track(self.source, 320, 180), sample, self.source))
        self.assertIsNone(probe.match_layer({**track(self.source), "sink_active": False}, sample, self.source))

    def test_candidate_cannot_exceed_its_frozen_layer_and_screen_high_uses_frozen_h_rid(self):
        sample = self.safe(outbound=[stream("q", 640, 360, 15)])
        self.assertIsNone(probe.match_layer(track(self.source, 640, 360), sample, self.source))
        self.raw.write_bytes(b"")
        screen = source(self.ready, "screen")
        sample = self.safe(value=screen, outbound=[stream("q", 1280, 720, 0), stream("h", 1920, 1080, 20)])
        self.assertEqual(probe.match_layer(track(screen, 1920, 1080), sample, screen), "high")
        sample["outbound"][1]["rid"] = "f"
        self.assertIsNone(probe.match_layer(track(screen, 1920, 1080), sample, screen))


if __name__ == "__main__":
    unittest.main()
