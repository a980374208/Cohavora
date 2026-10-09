import importlib.util
from copy import deepcopy
from pathlib import Path
import unittest

PATH = Path(__file__).resolve().parents[1] / "tools/screen_capture/b11_product_ui_4k.py"
SPEC = importlib.util.spec_from_file_location("b11_product_ui_4k", PATH)
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def samples():
    rows = []
    for i in range(31):
        p = {"share_state": 2, "quality_status": 0, "resolution": 5, "fps": 20,
             "applied_width": 3840, "applied_height": 2160, "capture_backend": "wgc",
             "source_width": 3840, "source_height": 2160,
             "source_client_width": 3840, "source_client_height": 2160,
             "owned_source_matches": 1, "capture_failures": 0, "elapsed_s": i,
             "capture_frames": 20 * i, "source_frames": 20 * i,
             "preview_frames": 20 * i, "sid_hash": "1234567890abcdef"}
        p.update({key: True for key in probe.SAME_FIELDS})
        r = {"connected": True, "attached": True, "dimensions_request_accepted": True,
             "same_sid": True, "same_track": True, "subscriptions": 1, "removed": 0,
             "unsubscribed": 0, "width": 3840, "height": 2160, "frame_age_ms": 25,
             "sid_hash": p["sid_hash"], "elapsed_s": i, "frames": 20 * i,
             "frames_4k": 20 * i, "frames_1080p": 0}
        rows.append({"publisher": p, "receiver": r})
    return rows


class ProductUi4kEvidence(unittest.TestCase):
    def setUp(self):
        self.rows = samples()

    def rejected(self, code):
        with self.assertRaisesRegex(probe.RunFailure, "^" + code + "$"):
            probe.evaluate_step(self.rows, 5, 3840, 2160)

    def test_actual_owned_4k_window_and_decoded_frames(self):
        result = probe.evaluate_step(self.rows, 5, 3840, 2160)
        self.assertEqual(result["received_decoded_fps"], 20)
        self.assertEqual(result["exact_capture_instance_identity"], "UNKNOWN")
        self.assertEqual(result["gpu_performance_status"], "NOT_EVALUATED")

    def test_smaller_source_cannot_claim_real_4k(self):
        self.rows[5]["publisher"]["source_width"] = 2560
        self.rejected("owned_wgc_source_not_verified")

    def test_previous_layer_frames_cannot_count_as_4k_fps(self):
        for i, row in enumerate(self.rows):
            row["receiver"]["frames_4k"] = 19 * i
            row["receiver"]["frames_1080p"] = i
        self.rejected("unexpected_frame_dimensions_in_window")

    def test_publication_replacement_is_not_same_track(self):
        self.rows[10]["publisher"]["same_track"] = False
        self.rejected("publisher_identity_changed")

    def test_wrong_room_media_cannot_pass_by_dimensions(self):
        self.rows[10]["receiver"]["sid_hash"] = "ffffffffffffffff"
        self.rejected("publication_correlation_failed")

    def test_counter_regression_is_rejected(self):
        self.rows[10]["receiver"]["frames"] = 0
        self.rejected("measurement_counter_regression")

    def test_status_gap_is_rejected(self):
        self.rows[11]["receiver"]["elapsed_s"] += 2
        self.rejected("measurement_time_gap_or_regression")

    def test_low_fps_is_not_made_pass_by_fresh_last_frame(self):
        for i, row in enumerate(self.rows):
            row["receiver"]["frames"] = row["receiver"]["frames_4k"] = 17 * i
        self.rejected("frame_rate_below_frozen_threshold")

    def test_truncated_measurement_is_rejected(self):
        self.rows = self.rows[:-1]
        self.rejected("measurement_window_incomplete")


def quiet_samples():
    return [{"publisher": {"share_state": 0, "local_share_tracks": 0,
                           "utc_ms": 100000 + i * 500, "elapsed_s": 10 + i * 0.5,
                           "heartbeat_seq": 100 + i * 2, "capture_frames": 600},
             "receiver": {"attached": False, "removed": 1,
                          "utc_ms": 100100 + i * 500, "elapsed_s": 9 + i * 0.5,
                          "sample_seq": 90 + i * 2, "frames": 500,
                          "callback_frames": 505, "post_detach_callbacks": 5}}
            for i in range(6)]


class ProductUiQuietEvidence(unittest.TestCase):
    def setUp(self):
        self.rows = quiet_samples()

    def rejected(self, code):
        with self.assertRaisesRegex(probe.RunFailure, "^" + code + "$"):
            probe.evaluate_quiet(self.rows)

    def test_actual_window_records_progress_and_callback_baselines(self):
        result = probe.evaluate_quiet(self.rows)
        self.assertEqual(result["scope"], "B11_PRODUCT_UI_4K_DIAGNOSTIC_V2")
        self.assertEqual(result["status"], "PASS")
        for role in ("publisher", "receiver"):
            self.assertEqual(result[role]["elapsed_seconds"], 2.5)
            self.assertEqual(result[role]["utc_seconds"], 2.5)
            self.assertEqual(result[role]["maximum_elapsed_gap_seconds"], 0.5)
            self.assertTrue(all(delta == 0 for delta in result[role]["counter_delta"].values()))
        # Prior in-flight callbacks remain visible; PASS only covers this quiet window.
        self.assertEqual(result["receiver"]["counter_baseline"]["post_detach_callbacks"], 5)
        self.assertEqual(result["receiver"]["counter_last"]["post_detach_callbacks"], 5)

    def test_repeated_publisher_or_receiver_json_cannot_prove_silence(self):
        for role in ("publisher", "receiver"):
            with self.subTest(role=role):
                self.rows = quiet_samples()
                self.rows[2][role] = deepcopy(self.rows[1][role])
                self.rejected("quiet_status_did_not_advance")

    def test_sequence_and_both_clocks_must_advance_independently(self):
        for role, key in (("publisher", "heartbeat_seq"), ("receiver", "sample_seq"),
                          ("publisher", "utc_ms"), ("receiver", "elapsed_s")):
            with self.subTest(role=role, key=key):
                self.rows = quiet_samples()
                self.rows[2][role][key] = self.rows[1][role][key]
                self.rejected("quiet_status_did_not_advance")

    def test_long_gap_is_rejected_even_when_counters_remain_constant(self):
        for role in ("publisher", "receiver"):
            with self.subTest(role=role):
                self.rows = quiet_samples()
                self.rows[2][role]["elapsed_s"] += 1
                self.rows[2][role]["utc_ms"] += 1000
                self.rejected("quiet_measurement_time_gap")

    def test_detached_late_callback_is_visible_despite_static_accepted_frames(self):
        for row in self.rows[3:]:
            row["receiver"]["callback_frames"] += 1
            row["receiver"]["post_detach_callbacks"] += 1
        self.assertEqual(len({row["receiver"]["frames"] for row in self.rows}), 1)
        self.assertTrue(all(row["receiver"]["attached"] is False for row in self.rows))
        self.rejected("frames_after_share_stop")

    def test_capture_or_receiver_counter_change_is_not_silent(self):
        for role, key in (("publisher", "capture_frames"), ("receiver", "frames"),
                          ("receiver", "callback_frames"), ("receiver", "post_detach_callbacks")):
            with self.subTest(role=role, key=key):
                self.rows = quiet_samples()
                self.rows[3][role][key] += 1
                self.rejected("frames_after_share_stop")

    def test_short_real_window_cannot_use_supervisor_wait_as_coverage(self):
        self.rows = self.rows[:4]
        self.rejected("quiet_measurement_window_incomplete")

    def test_missing_callback_counter_cannot_reuse_old_receiver_evidence(self):
        del self.rows[0]["receiver"]["callback_frames"]
        self.rejected("quiet_measurement_field_invalid")

    def test_binding_must_remain_detached_and_publisher_stopped(self):
        self.rows[3]["receiver"]["attached"] = True
        self.rejected("quiet_receiver_binding_changed")
        self.rows = quiet_samples()
        self.rows[3]["publisher"]["local_share_tracks"] = 1
        self.rejected("local_share_not_stopped")

    def test_non_finite_or_bool_counter_cannot_be_used_as_progress(self):
        for replacement in (float("nan"), float("inf"), True, None):
            with self.subTest(replacement=replacement):
                self.rows = quiet_samples()
                self.rows[3]["receiver"]["callback_frames"] = replacement
                self.rejected("quiet_measurement_field_invalid")


if __name__ == "__main__":
    unittest.main()
