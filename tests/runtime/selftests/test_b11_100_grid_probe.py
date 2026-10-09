"""Direct offline evidence tests; no observer, cloud, or desktop is launched."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools" / "meeting"
sys.path.insert(0, str(TOOLS))
import b11_100_grid_probe as probe
import b11_100_input_freeze as freeze
import meeting_render_probe as render

RUN_ID, PID = "grid100_direct_selftest", 44001
PREFIX = "b11grid100_fixture"
SELECTED_SEQUENCES = (0, 1, 2, 3, 4, 5, 8, 12, 17, 26, 35, 44, 58, 66, 83, 99)


def digest(value):
    return hashlib.sha256(value.encode("utf-8")).hexdigest()[:16]


def allowed_identities():
    return {digest(f"{PREFIX}_pub_{index}") for index in range(100)}


def track(sequence, second=0, quality="low", height=None):
    layer = freeze.QUALITY_CONTRACT[quality]
    # Asset creation order is concurrent. Tests deliberately exercise both real
    # low-layer heights without claiming an identity-to-asset assignment.
    if height is None:
        height = (180 if sequence % 3 == 0 else 150) if quality == "low" else layer["heights"][0]
    fps = layer["source_fps"]
    result = {name: True for name in render.TRACK_BOOL_FIELDS}
    result.update({name: 0 for name in render.TRACK_COUNT_FIELDS})
    result.update(
        sid_hash=digest(f"publication-{sequence}"),
        identity_hash=digest(f"{PREFIX}_pub_{sequence}"),
        rtc_track_hash=digest(f"rtc-{sequence}"),
        binding_rtc_track_hash=digest(f"rtc-{sequence}"),
        publication_media_track_hash=digest(f"rtc-{sequence}"),
        stats_stream_hash=digest(f"stats-{sequence}"),
        subscription_dirty=False, settings_dirty=False, subscription_error=False,
        sent_subscribed=True, intent_policy_revision=57,
        current_binding_serial=sequence + 1, sink_binding_serial=sequence + 1,
        sink_binding_count=1, sink_on_frame_age_ms=50, sink_frame_age_ms=50,
        stats_match_count=1, stats_lost=0,
        sink_on_frame_count=100 + fps * second,
        sink_delivered_frame_count=100 + fps * second,
        stats_decoded=100 + fps * second, stats_received=100 + fps * second,
        stats_bytes=1000 + 1000 * second, stats_packets=100 + 10 * second,
        desired_enabled=True, desired_quality=quality, desired_width=280,
        desired_height=141 if quality == "low" else 250,
        desired_max_fps=15 if quality == "low" else 30,
        seat_present=True, seat_role="grid", seat_quality={"low": "p180", "medium": "p360", "high": "p720"}[quality],
        focused=False, sink_frame_width=layer["width"], sink_frame_height=height,
        sink_frame_dimensions_available=True, render_frame_width=layer["width"],
        render_frame_height=height, render_frame_rotation=0,
        render_frame_dimensions_available=True,
    )
    return result


def inbound(sequence, second=0, quality="low", *, historical=False):
    source = track(sequence, second, quality)
    result = dict(
        report_index=0, track_hash=source["rtc_track_hash"],
        stats_id_hash=digest(f"inbound-id-{sequence}"),
        ssrc_hash=digest(f"inbound-ssrc-{sequence}"), mid_hash=digest(f"mid-{sequence}"),
        mapped_sid_hash=source["sid_hash"], mapped_binding_serial=source["sink_binding_serial"],
        mapped_binding_count=1, mapped_binding_current=True, mapped_sink_active=True,
        mapped_sink_on_frame_count=source["sink_on_frame_count"],
        mapped_sink_delivered_frame_count=source["sink_delivered_frame_count"],
        mapped_sink_frame_age_ms=50, mapped_sink_frame_width=source["sink_frame_width"],
        mapped_sink_frame_height=source["sink_frame_height"],
        mapped_sink_frame_dimensions_available=True,
        frame_width=source["sink_frame_width"], frame_height=source["sink_frame_height"],
        frame_width_available=True, frame_height_available=True,
        frames_per_second=freeze.QUALITY_CONTRACT[quality]["source_fps"],
        frames_per_second_available=True, bytes=source["stats_bytes"],
        packets=source["stats_packets"], decoded=source["stats_decoded"],
        received=source["stats_received"], lost=0,
        bytes_available=True, packets_available=True, decoded_available=True,
        received_available=True, lost_available=True,
    )
    if historical:
        result.update(mapped_sid_hash="", mapped_binding_serial=0, mapped_binding_count=0,
            mapped_binding_current=False, mapped_sink_active=False,
            mapped_sink_on_frame_count=0, mapped_sink_delivered_frame_count=0,
            mapped_sink_frame_age_ms=-1, mapped_sink_frame_width=0, mapped_sink_frame_height=0,
            mapped_sink_frame_dimensions_available=False, frames_per_second=0,
            bytes=1000, packets=100, decoded=100, received=100)
    return result


def raw_status(second=0, quality="low", historical_count=0):
    selected = [track(sequence, second, quality) for sequence in SELECTED_SEQUENCES]
    streams = [inbound(sequence, second, quality) for sequence in SELECTED_SEQUENCES]
    streams.extend(inbound(100 + sequence, historical=True) for sequence in range(historical_count))
    frames = 100 + len(selected) * freeze.QUALITY_CONTRACT[quality]["source_fps"] * second
    return dict(schema=1, run_id=RUN_ID, pid=PID, heartbeat_seq=second + 1,
        runtime_seq=100 + second, policy_revision=57, command_seq=1,
        command_status="applied", state="connected", error_code="",
        requested=16, selected=16, actual=16, bound=16,
        selected_not_bound=0, bound_not_selected=0, remote_video_count=100,
        render_submits=frames, decoded_frames=frames,
        render_expected_bindings=16, render_hidden_bindings=0,
        render_router_submitted=frames, render_router_rejected_binding=0,
        render_delivered_to_gpu=frames, render_attached_tracks=16,
        track_frames_received=frames, lease_rejected_frames=0,
        selected_native_sinks=16, selected_native_recent=16, selected_active_leases=16,
        duplicate_same_track=0, duplicate_new_track=0,
        render_timer_active=True, video_stage_visible=True, canvas_visible=True,
        renderer_ready=True, render_backend="dx11", audio_frames=None,
        page=0, page_size=16, page_count=7, layout_mode="grid", demand_reason="visible",
        focused_sid_hash="", pinned_sid_hash="", stats_sample_seq=second + 1,
        stats_age_ms=50, stats_video_streams=len(streams), stats_track_ids_available=len(streams),
        selected_tracks=selected, inbound_streams=streams,
        selected_fingerprint=hashlib.sha256(b"".join(
            f"publication-{sequence}".encode() + b"\0" for sequence in SELECTED_SEQUENCES)).hexdigest(),
        elapsed_s=float(second), sharing=False,
        policy_measurement_point="session_video_policy_convergence",
    )


def validated(raw):
    result = probe.validate_status(raw, RUN_ID, PID)
    result["elapsed_s"] = raw["elapsed_s"]
    return result


def sample_rows(quality="low", historical_count=0, seconds=30):
    return [validated(raw_status(second, quality, historical_count))
            for second in range(seconds + 1)]


class GridEvidenceTest(unittest.TestCase):
    def evaluate(self, rows, frozen_grid=None):
        return probe.evaluate_grid(rows, frozen_grid or probe.freeze_grid(rows[0]),
                                   freeze.QUALITY_CONTRACT, allowed_identities())

    def test_automatic_low_receives_legal_150_and_180_heights_at_15_fps(self):
        rows = sample_rows()
        result = self.evaluate(rows)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual({item["height"] for item in result["tracks"]}, {150, 180})
        self.assertEqual({item["native_fps"] for item in result["tracks"]}, {15})
        self.assertEqual({item["minimum_fps"] for item in result["tracks"]}, {13.5})
        self.assertTrue(all(item["source_assignment"] == "UNKNOWN_CONCURRENT_PUBLISH_ORDER"
                            for item in result["tracks"]))

    def test_automatic_medium_receives_360_at_20_fps(self):
        result = self.evaluate(sample_rows("medium"))
        self.assertEqual(result["status"], "PASS")
        self.assertEqual({(item["width"], item["height"], item["native_fps"])
                          for item in result["tracks"]}, {(640, 360, 20)})

    def test_grid_policy_does_not_accept_forced_720_high(self):
        status = validated(raw_status(quality="high"))
        self.assertFalse(probe.ready(status, freeze.QUALITY_CONTRACT, allowed_identities()))

    def test_stationary_historical_reports_do_not_imply_100_active_subscriptions(self):
        rows = sample_rows(historical_count=100)
        self.assertEqual(len(rows[0]["inbound_streams"]), 116)
        result = self.evaluate(rows)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["invisible_subscription"]["nonselected_active_streams"], 0)

    def test_unknown_and_duplicate_publisher_identity_are_not_ready(self):
        for identity in (digest("other-room-publisher"), track(SELECTED_SEQUENCES[1])["identity_hash"]):
            with self.subTest(identity=identity):
                raw = raw_status()
                raw["selected_tracks"][0]["identity_hash"] = identity
                self.assertFalse(probe.ready(validated(raw), freeze.QUALITY_CONTRACT, allowed_identities()))

    def test_binding_quality_and_legal_geometry_are_immutable_after_freeze(self):
        for mutation in ("binding", "quality", "legal_geometry", "stats_stream"):
            with self.subTest(mutation=mutation):
                rows = sample_rows()
                frozen = probe.freeze_grid(rows[0])
                for row in rows[15:]:
                    current = row["selected_tracks"][0]
                    if mutation == "binding":
                        current["current_binding_serial"] += 100
                        current["sink_binding_serial"] += 100
                    elif mutation == "quality":
                        current.update(desired_quality="medium", seat_quality="p360",
                                       sink_frame_width=640, sink_frame_height=360,
                                       render_frame_width=640, render_frame_height=360)
                    elif mutation == "legal_geometry":
                        current.update(sink_frame_height=150, render_frame_height=150)
                    else:
                        current["stats_stream_hash"] = digest("replacement-stats")
                result = self.evaluate(rows, frozen)
                self.assertEqual(result["status"], "FAIL")
                self.assertIn("grid_binding_geometry_or_quality_changed", result["reason"])

    def test_every_selected_track_requires_native_delivered_and_decoded_fps(self):
        for field in ("sink_on_frame_count", "sink_delivered_frame_count", "stats_decoded"):
            with self.subTest(field=field):
                rows = sample_rows()
                for index, row in enumerate(rows):
                    # Only one of 16 tracks is slow; aggregate render throughput is unchanged.
                    row["selected_tracks"][7][field] = 100 + 13 * index
                result = self.evaluate(rows)
                self.assertEqual(result["status"], "FAIL")
                self.assertIn("track_fps_below_frozen_minimum", result["reason"])

    def test_per_track_packet_byte_and_frame_counters_must_not_stall_or_reset(self):
        for field in ("stats_bytes", "stats_packets", "sink_on_frame_count",
                      "sink_delivered_frame_count", "stats_decoded"):
            for behavior in ("stall", "reset"):
                with self.subTest(field=field, behavior=behavior):
                    rows = sample_rows()
                    baseline = rows[0]["selected_tracks"][0][field]
                    if behavior == "stall":
                        for row in rows:
                            row["selected_tracks"][0][field] = baseline
                    else:
                        rows[15]["selected_tracks"][0][field] = 0
                    result = self.evaluate(rows)
                    self.assertEqual(result["status"], "FAIL")
                    self.assertIn(field + "_not_progressing", result["reason"])

    def test_router_and_gpu_progress_required_even_when_native_decode_progresses(self):
        for field in ("render_submits", "render_router_submitted", "render_delivered_to_gpu"):
            with self.subTest(field=field):
                rows = sample_rows()
                for row in rows:
                    row[field] = rows[0][field]
                result = self.evaluate(rows)
                self.assertEqual(result["status"], "FAIL")
                self.assertIn(field + "_not_progressing", result["reason"])

    def test_lost_selected_sid_is_failure(self):
        rows = sample_rows()
        frozen = probe.freeze_grid(rows[0])
        rows[15]["selected_tracks"].pop()
        result = self.evaluate(rows, frozen)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("selected_track_disappeared", result["reason"])

    def test_missing_canvas_stale_sink_stale_stats_or_lost_publication_cannot_pass(self):
        for field in ("canvas_dimensions", "canvas_visible", "sink_age", "stats_age",
                      "publication", "subscription_error"):
            with self.subTest(field=field):
                rows = sample_rows()
                row = rows[15]
                current = row["selected_tracks"][0]
                if field == "canvas_dimensions":
                    current["render_frame_dimensions_available"] = False
                elif field == "canvas_visible":
                    row["canvas_visible"] = False
                elif field == "sink_age":
                    current["sink_frame_age_ms"] = 3001
                elif field == "stats_age":
                    row["stats_age_ms"] = 3001
                elif field == "publication":
                    current["publication_subscribed"] = False
                else:
                    current["subscription_error"] = True
                result = self.evaluate(rows)
                self.assertEqual(result["status"], "FAIL")
                self.assertIn("dimensions_or_grid_state_mismatch", result["reason"])

    def test_observation_gap_is_failure(self):
        rows = sample_rows()
        for row in rows[15:]:
            row["elapsed_s"] += 4
        result = self.evaluate(rows)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("sample_gap", result["reason"])

    def test_too_short_observation_is_inconclusive(self):
        self.assertEqual(self.evaluate(sample_rows(seconds=10))["status"], "INCONCLUSIVE")

    def test_grid16_requires_requested_actual_attached_and_active_lease_counts(self):
        for field in ("requested", "actual", "render_attached_tracks", "selected_active_leases"):
            with self.subTest(field=field):
                rows = sample_rows()
                frozen = probe.freeze_grid(rows[0])
                rows[15][field] = 15
                result = self.evaluate(rows, frozen)
                self.assertEqual(result["status"], "FAIL")
                self.assertIn("dimensions_or_grid_state_mismatch", result["reason"])

    def test_command_ack_and_sequence_remain_frozen_during_observation(self):
        for mutation in ("new_sequence", "pending", "rejected"):
            with self.subTest(mutation=mutation):
                rows = sample_rows()
                frozen = probe.freeze_grid(rows[0])
                for row in rows[15:]:
                    if mutation == "new_sequence":
                        row["command_seq"] += 1
                    else:
                        row["command_status"] = mutation
                result = self.evaluate(rows, frozen)
                self.assertEqual(result["status"], "FAIL")
                self.assertIn("grid_command_ack_changed", result["reason"])

    def test_json_archive_roundtrip_preserves_frozen_grid_verdict(self):
        rows = sample_rows()
        frozen = json.loads(json.dumps(probe.freeze_grid(rows[0])))
        self.assertEqual(self.evaluate(rows, frozen)["status"], "PASS")


class InvisibleSubscriptionTest(unittest.TestCase):
    def test_nonselected_current_active_bytes_or_packets_fail(self):
        for counter in ("bytes", "packets"):
            with self.subTest(counter=counter):
                rows = sample_rows()
                for index, row in enumerate(rows):
                    stream = inbound(77)
                    stream[counter] += index
                    row["inbound_streams"].append(stream)
                result = probe.evaluate_invisible(rows)
                self.assertEqual(result["status"], "FAIL")
                self.assertEqual(result["nonselected_active_streams"], 1)

    def test_unmapped_positive_bytes_or_packets_are_inconclusive(self):
        for counter in ("bytes", "packets"):
            with self.subTest(counter=counter):
                rows = sample_rows(historical_count=1)
                for index, row in enumerate(rows):
                    row["inbound_streams"][-1][counter] += index
                result = probe.evaluate_invisible(rows)
                self.assertEqual(result["status"], "INCONCLUSIVE")
                self.assertEqual(result["unmapped_progressing_streams"], 1)

    def test_unmapped_progress_makes_whole_grid_inconclusive(self):
        rows = sample_rows(historical_count=1)
        for index, row in enumerate(rows):
            row["inbound_streams"][-1]["bytes"] += index
        result = probe.evaluate_grid(rows, probe.freeze_grid(rows[0]),
                                     freeze.QUALITY_CONTRACT, allowed_identities())
        self.assertEqual(result["status"], "INCONCLUSIVE")
        self.assertEqual(result["reason"], "rtp_attribution_gap")

    def test_unavailable_counters_or_counter_reset_are_inconclusive(self):
        for mutation in ("unavailable", "reset"):
            with self.subTest(mutation=mutation):
                rows = sample_rows(historical_count=1)
                stream = rows[15]["inbound_streams"][-1]
                stream["bytes_available" if mutation == "unavailable" else "bytes"] = False if mutation == "unavailable" else 0
                self.assertEqual(probe.evaluate_invisible(rows)["status"], "INCONCLUSIVE")

    def test_last_new_report_cannot_pass_without_following_baseline(self):
        rows = sample_rows()
        rows[-1]["inbound_streams"].append(inbound(77))
        result = probe.evaluate_invisible(rows)
        self.assertEqual(result["status"], "INCONCLUSIVE")
        self.assertEqual(result["unbaselined_final_streams"], 1)

    def test_new_report_that_disappears_cannot_be_declared_stationary(self):
        rows = sample_rows()
        rows[15]["inbound_streams"].append(inbound(77))
        self.assertEqual(probe.evaluate_invisible(rows)["status"], "INCONCLUSIVE")

    def test_new_stationary_report_closes_when_following_report_exists(self):
        rows = sample_rows()
        for row in rows[15:]:
            row["inbound_streams"].append(inbound(77))
        result = probe.evaluate_invisible(rows)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["unbaselined_final_streams"], 0)

    def test_distinct_rtp_intervals_required(self):
        rows = sample_rows()
        for row in rows:
            row["stats_sample_seq"] = 1
        self.assertEqual(probe.evaluate_invisible(rows)["status"], "INCONCLUSIVE")


class ClosedStatusTest(unittest.TestCase):
    def test_400_stream_boundary_retains_all_reports_and_rejects_401(self):
        status = validated(raw_status(historical_count=384))
        self.assertEqual(len(status["inbound_streams"]), 400)
        with self.assertRaises(probe.soak.StopRun):
            validated(raw_status(historical_count=385))

    def test_every_chunk_is_closed_validated(self):
        for index in (0, 31, 32, 63, 64, 115):
            for field, value in (("stats_id_hash", "raw-publication-secret"),
                                 ("bytes", True), ("packets_available", 1),
                                 ("report_index", 17), ("frame_width", -1),
                                 ("frames_per_second", float("nan"))):
                with self.subTest(index=index, field=field):
                    raw = raw_status(historical_count=100)
                    raw["inbound_streams"][index][field] = value
                    with self.assertRaises(probe.soak.StopRun):
                        validated(raw)

    def test_duplicate_report_across_chunks_is_rejected(self):
        raw = raw_status(historical_count=100)
        raw["inbound_streams"][64] = copy.deepcopy(raw["inbound_streams"][0])
        with self.assertRaises(probe.soak.StopRun):
            validated(raw)

    def test_closed_validator_does_not_retain_unknown_raw_strings(self):
        raw = raw_status(historical_count=100)
        raw["credentials"] = "do-not-export-this-secret"
        raw["inbound_streams"][64]["track_identifier"] = "do-not-export-this-secret"
        raw["selected_tracks"][0]["identity"] = "do-not-export-this-secret"
        self.assertNotIn("do-not-export-this-secret", json.dumps(validated(raw)))

    def test_required_render_state_and_counters_cannot_be_absent(self):
        for field in ("render_timer_active", "video_stage_visible", "canvas_visible",
                      "renderer_ready", "render_router_submitted", "render_delivered_to_gpu",
                      "render_attached_tracks", "selected_active_leases"):
            with self.subTest(field=field):
                raw = raw_status()
                del raw[field]
                with self.assertRaises(probe.soak.StopRun):
                    validated(raw)

    def test_wrong_process_or_run_is_rejected(self):
        for field, value in (("pid", PID + 1), ("run_id", "different-run")):
            with self.subTest(field=field):
                raw = raw_status()
                raw[field] = value
                with self.assertRaises(probe.soak.StopRun):
                    validated(raw)

    def test_json_above_old_64k_limit_is_readable_and_validated(self):
        raw = raw_status(historical_count=100)
        data = json.dumps(raw).encode("utf-8")
        self.assertGreater(len(data), 65536)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "status.json"
            path.write_bytes(data)
            self.assertEqual(len(validated(probe.read_json(path))["inbound_streams"]), 116)

    def test_json_one_mib_boundary_and_oversize_rejection(self):
        data = json.dumps(raw_status()).encode("utf-8")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "status.json"
            path.write_bytes(data + b" " * (probe.MAX_STATUS_BYTES - len(data)))
            self.assertEqual(probe.read_json(path)["pid"], PID)
            path.write_bytes(data + b" " * (probe.MAX_STATUS_BYTES + 1 - len(data)))
            with self.assertRaises(probe.soak.StopRun):
                probe.read_json(path)


if __name__ == "__main__":
    unittest.main()
