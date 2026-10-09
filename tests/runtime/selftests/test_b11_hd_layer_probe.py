"""Offline verdict tests; no service/UI is launched."""
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/meeting"))
import b11_hd_layer_probe as probe
import b11_hd_input_freeze as freeze


def rows(quality="high"):
    contract = freeze.QUALITY_CONTRACT
    size = contract[quality]
    fps = size["source_fps"]
    data = []
    for second in range(61):
        track = {"sid_hash": "1" * 16, "rtc_track_hash": "2" * 16,
            "binding_rtc_track_hash": "2" * 16, "current_binding_serial": 1,
            "sink_binding_serial": 1, "sink_binding_count": 1,
            "sink_active": True, "sink_frame_dimensions_available": True,
            "sink_frame_age_ms": 50, "sink_frame_width": size["width"],
            "sink_frame_height": size["height"], "intent_present": True,
            "intent_subscribed": True, "desired_enabled": True, "desired_quality": quality,
            "desired_width": {"low": 280, "medium": 400, "high": 1280}[quality],
            "desired_height": {"low": 158, "medium": 225, "high": 720}[quality],
            "desired_max_fps": 15 if quality == "low" else 30,
            "seat_present": True,
            "seat_quality": {"low": "p180", "medium": "p360", "high": "p720"}[quality],
            "seat_width": {"low": 280, "medium": 400, "high": 1280}[quality],
            "seat_height": {"low": 158, "medium": 225, "high": 720}[quality],
            "source_width": 1280, "source_height": 720,
            "window_width": 1600 if quality == "medium" else 1120,
            "window_height": 1000 if quality == "medium" else 720,
            "viewport_stage_width": 1600 if quality == "medium" else 1120,
            "viewport_stage_height": 900 if quality == "medium" else 632,
            "viewport_device_pixel_ratio": 1.0,
            "publication_subscribed": True, "subscription_error": False,
            "stats_bytes_available": True, "stats_packets_available": True, "stats_decoded_available": True,
            "stats_stream_hash": "4" * 16, "focused": quality == "high", "seat_role": "main" if quality == "high" else "grid",
            "render_frame_dimensions_available": True, "render_frame_width": size["width"], "render_frame_height": size["height"],
            "sink_on_frame_count": 100 + fps * second,
            "sink_delivered_frame_count": 100 + fps * second,
            "stats_bytes": 1000 + 1000 * second, "stats_packets": 100 + 10 * second,
            "stats_decoded": 100 + fps * second}
        data.append({"elapsed_s": float(second), "target": track,
            "state": "connected", "selected_not_bound": 0, "bound_not_selected": 0,
            "render_timer_active": True, "video_stage_visible": True, "demand_reason": "pinned" if quality == "high" else "visible",
            "render_submits": 100 + second * fps, "render_router_submitted": 100 + second * fps,
            "render_delivered_to_gpu": 100 + second * fps, "stats_sample_seq": second,
            "layout_mode": "speaker" if quality == "high" else "grid",
            "pinned_sid_hash": "1" * 16 if quality == "high" else "",
            "focused_sid_hash": "1" * 16 if quality == "high" else "",
            "resource_pid": 77, "resource_status": "AVAILABLE",
            "command_seq": 1, "command_status": "applied",
            "private_bytes": 100_000_000, "working_set_bytes": 90_000_000,
            "handles": 100, "cpu_time_seconds": float(second),
            "cpu_percent_of_one_core": 10.0, "cpu_percent_of_host": 1.0,
            "page": 0, "page_size": 16, "selected": 16, "bound": 16})
    return data


class HdVerdictTests(unittest.TestCase):
    def test_actual_fps_and_dimensions_pass(self):
        for quality in ("low", "medium", "high"):
            result = probe.evaluate_step(rows(quality), quality, freeze.QUALITY_CONTRACT)
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["native_fps"], freeze.QUALITY_CONTRACT[quality]["source_fps"])

    def test_frozen_pixel_driven_stages_pass_with_independent_resource_summary(self):
        for step in freeze.PROBE["steps"]:
            quality = step["quality"]
            result = probe.evaluate_step(rows(quality), quality, freeze.QUALITY_CONTRACT, step=step)
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["delivered_fps"], freeze.QUALITY_CONTRACT[quality]["source_fps"])
            self.assertEqual(result["client_resources"]["status"], "AVAILABLE")
            self.assertEqual(result["expected_window"], [step["window_width"], step["window_height"]])

    def test_resize_ack_without_expected_source_and_pixel_intent_is_not_ready(self):
        step = freeze.PROBE["steps"][1]
        for mutation in (lambda t: t.update(window_width=1120),
                         lambda t: t.update(source_width=0),
                         lambda t: t.update(viewport_device_pixel_ratio=1.5),
                         lambda t: t.update(desired_width=280, desired_height=158, seat_width=280, seat_height=158),
                         lambda t: t.update(seat_quality="p180")):
            data = rows("medium")[0]
            mutation(data["target"])
            self.assertFalse(probe.ready(data, data["target"], "medium", freeze.QUALITY_CONTRACT, step=step))

    def test_main_requires_720_floor_even_with_high_intent(self):
        data = rows()[0]
        data["target"].update(desired_width=960, desired_height=540, seat_width=960, seat_height=540)
        self.assertFalse(probe.ready(data, data["target"], "high", freeze.QUALITY_CONTRACT,
                                     step=freeze.PROBE["steps"][2]))

    def test_delivered_frame_rate_is_gated(self):
        data = rows()
        for index, row in enumerate(data):
            row["target"]["sink_delivered_frame_count"] = 100 + index * 15
        self.assertIn("frame_rate_below_frozen_minimum", probe.evaluate_step(
            data, "high", freeze.QUALITY_CONTRACT)["reason"])

    def test_stage_resources_cannot_hide_failed_reads(self):
        data = rows("medium")
        data[30].update(resource_status="UNKNOWN", private_bytes=None)
        result = probe.evaluate_step(data, "medium", freeze.QUALITY_CONTRACT, step=freeze.PROBE["steps"][1])
        self.assertEqual(result["client_resources"]["status"], "UNKNOWN")
        self.assertEqual(result["status"], "INCONCLUSIVE")

    def test_missing_intent_keeps_original_phase_failure(self):
        data = rows("medium")
        data[30]["target"]["desired_width"] = None
        self.assertIn("dimensions_or_view_state_mismatch", probe.evaluate_step(
            data, "medium", freeze.QUALITY_CONTRACT, step=freeze.PROBE["steps"][1])["reason"])

    def test_phase_command_ack_must_remain_stable(self):
        data = rows("medium")
        data[30]["command_seq"] = 2
        self.assertIn("step_command_ack_changed", probe.evaluate_step(
            data, "medium", freeze.QUALITY_CONTRACT, step=freeze.PROBE["steps"][1])["reason"])

    def test_source_metadata_cannot_replace_30fps_receipt(self):
        data = rows()
        for index, row in enumerate(data):
            row["target"]["sink_on_frame_count"] = 100 + index * 15
            row["target"]["stats_decoded"] = 100 + index * 15
        result = probe.evaluate_step(data, "high", freeze.QUALITY_CONTRACT)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("frame_rate_below_frozen_minimum", result["reason"])

    def test_one_wrong_dimension_is_preserved_failure(self):
        data = rows()
        data[20]["target"]["sink_frame_width"] = 320
        self.assertEqual(probe.evaluate_step(data, "high", freeze.QUALITY_CONTRACT)["status"], "FAIL")

    def test_counter_reset_and_binding_replacement_fail(self):
        for field in ("stats_decoded", "sink_on_frame_count", "stats_packets", "stats_bytes"):
            data = rows()
            data[30]["target"][field] = 0
            self.assertIn(field + "_reset", probe.evaluate_step(data, "high", freeze.QUALITY_CONTRACT)["reason"])
        data = rows()
        data[30]["target"]["current_binding_serial"] = 2
        self.assertIn("target_binding_changed", probe.evaluate_step(data, "high", freeze.QUALITY_CONTRACT)["reason"])

    def test_request_high_with_low_frames_never_ready(self):
        data = rows()[0]
        data["target"]["sink_frame_width"] = 320
        data["target"]["sink_frame_height"] = 180
        self.assertFalse(probe.ready(data, data["target"], "high", freeze.QUALITY_CONTRACT))

    def test_manual_unpin_during_hd_window_fails(self):
        data = rows()
        data[20]["pinned_sid_hash"] = ""
        self.assertIn("dimensions_or_view_state_mismatch", probe.evaluate_step(data, "high", freeze.QUALITY_CONTRACT)["reason"])

    def test_stale_frame_and_missing_rtp_never_ready(self):
        data = rows()[0]
        data["target"]["sink_frame_age_ms"] = 3001
        self.assertFalse(probe.ready(data, data["target"], "high", freeze.QUALITY_CONTRACT))
        data = rows()[0]
        data["target"]["stats_decoded_available"] = False
        self.assertFalse(probe.ready(data, data["target"], "high", freeze.QUALITY_CONTRACT))

    def test_sampling_gap_is_not_media_acceptance(self):
        data = rows()
        del data[20:25]
        self.assertIn("sample_gap", probe.evaluate_step(data, "high", freeze.QUALITY_CONTRACT)["reason"])

    def test_short_observation_stays_inconclusive(self):
        self.assertEqual(probe.evaluate_step(rows()[:10], "high", freeze.QUALITY_CONTRACT)["status"], "INCONCLUSIVE")

    def test_identity_is_exact_and_duplicates_rejected(self):
        expected = hashlib.sha256(b"b11hd_test_pub_0").hexdigest()[:16]
        status = {"selected_tracks": [{"identity_hash": "3" * 16}, {"identity_hash": expected}]}
        self.assertIs(probe.target_track(status, expected), status["selected_tracks"][1])
        status["selected_tracks"].append({"identity_hash": expected})
        with self.assertRaises(probe.soak.StopRun):
            probe.target_track(status, expected)

    def test_render_stall_does_not_pass_with_receiving_frames(self):
        data = rows()
        for row in data:
            row["render_delivered_to_gpu"] = 100
        self.assertIn("render_delivered_to_gpu_not_progressing", probe.evaluate_step(data, "high", freeze.QUALITY_CONTRACT)["reason"])

    def test_missing_canvas_geometry_stays_unknown(self):
        data = rows()
        for row in data:
            row["target"]["render_frame_dimensions_available"] = False
        self.assertEqual(probe.evaluate_step(data, "high", freeze.QUALITY_CONTRACT)["status"], "INCONCLUSIVE")


if __name__ == "__main__":
    unittest.main()
