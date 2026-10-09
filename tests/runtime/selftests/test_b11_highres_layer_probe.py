"""Offline request-versus-received layer, FPS and evidence boundary contracts."""
from copy import deepcopy
import hashlib
import math
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / "tools/meeting"
sys.path.insert(0, str(TOOLS))
import b11_highres_input_freeze as freeze
import b11_highres_layer_probe as probe


def rows(scenario="camera", request="grid_large", actual=None, fps=None):
    source = {"source": deepcopy(freeze.SOURCES[scenario]), "layers": deepcopy(freeze.LAYERS[scenario]),
              "scenario": scenario, "target_identity": "b11-highres-" + scenario}
    step = next((step for step in freeze.PROBES[scenario]["steps"] if step["request"] == request),
        {"layout": "pin_identity", "request": "main_small", "window_width": 1120, "window_height": 720, "seconds": 30})
    kind = freeze.scenario_kind(scenario)
    selected = "low" if request == "grid_low" or kind == "screen" and request == "main_small" \
        else "medium" if request == "grid_large" or scenario == "camera4k" and request == "main_small" else "high"
    actual = selected if actual is None else actual
    layer = source["layers"][actual]
    requested = source["layers"][selected]
    fps = layer["source_fps"] if fps is None else fps
    grid = request.startswith("grid")
    low = request == "grid_low"
    source_width, source_height = source["source"]["width"], source["source"]["height"]
    extent_width = step["window_width"] // 4 if grid else step["window_width"] if kind == "screen" else step["window_width"] * 3 // 4
    extent_height = (step["window_height"] - 192) // 4 if grid else step["window_height"] - 192
    cap_width, cap_height = (2560, 1440) if grid else (3840, 2160)
    scale = min(extent_width/source_width, extent_height/source_height, cap_width/source_width, cap_height/source_height, 1.0)
    seat_width, seat_height = math.floor(source_width*scale+0.5), math.floor(source_height*scale+0.5)
    desired_width, desired_height = requested["width"], requested["height"]
    result = []
    for seconds in range(31):
        track = {"sid_hash": "a" * 16, "source": source["source"]["source"], "source_width": source_width, "source_height": source_height,
            "sink_active": True, "sink_frame_dimensions_available": True, "sink_frame_age_ms": 10,
            "sink_frame_width": layer["width"], "sink_frame_height": layer["height"],
            "sink_binding_count": 1, "current_binding_serial": 1, "sink_binding_serial": 1,
            "rtc_track_hash": "b" * 16, "binding_rtc_track_hash": "b" * 16,
            "intent_present": True, "intent_subscribed": True, "desired_enabled": True,
            "publication_subscribed": True, "subscription_error": False,
            "stats_bytes_available": True, "stats_packets_available": True, "stats_decoded_available": True,
            "window_width": step["window_width"], "window_height": step["window_height"],
            "viewport_stage_width": step["window_width"],
            "viewport_stage_height": step["window_height"] - 192,
            "viewport_device_pixel_ratio": 1.0, "seat_present": True,
            "seat_width": seat_width, "seat_height": seat_height,
            "seat_subscription_width": desired_width, "seat_subscription_height": desired_height,
            "selected_layer_announced_width": desired_width, "selected_layer_announced_height": desired_height,
            "seat_selected_layer_quality": requested["quality"],
            "desired_width": desired_width, "desired_height": desired_height,
            "seat_role": "grid" if grid else "main", "focused": not grid,
            "show_local_participant": True, "show_local_screen_share": False,
            "desired_quality": requested["quality"], "desired_max_fps": 15 if min(desired_width, desired_height) <= 180 else 30,
            "seat_quality": probe._viewport_tier(seat_width, seat_height),
            "stats_stream_hash": "c" * 16, "sink_on_frame_count": 100+seconds*fps,
            "sink_delivered_frame_count": 100+seconds*fps, "stats_decoded": 100+seconds*fps,
            "stats_bytes": 1000+seconds*1000, "stats_packets": 100+seconds*100,
            "render_frame_dimensions_available": True,
            "render_frame_width": layer["width"], "render_frame_height": layer["height"]}
        stream = {"mapped_sid_hash": track["sid_hash"], "track_hash": track["rtc_track_hash"],
            "mapped_binding_current": True, "mapped_sink_active": True, "mapped_binding_serial": 1,
            "frame_width_available": True, "frame_height_available": True,
            "mapped_sink_frame_dimensions_available": True,
            "frame_width": layer["width"], "frame_height": layer["height"],
            "mapped_sink_frame_width": layer["width"], "mapped_sink_frame_height": layer["height"]}
        sender_sample = {"identity_hash": hashlib.sha256(source["target_identity"].encode()).hexdigest()[:16],
            "publication_sid_hash": track["sid_hash"], "source_kind": kind,
            "age_seconds": 0.1, "elapsed_seconds": seconds+5,
            "captured_frames": 100+seconds*source["source"]["source_fps"],
            "outbound": [{"rid": layer["rid"], "width": layer["width"], "height": layer["height"],
                "fps": fps, "frames_encoded": 100+seconds*fps, "width_available": True,
                "height_available": True, "fps_available": True, "frames_encoded_available": True}]}
        if scenario.endswith("4k"):
            sender_sample.update(capture_backend="wgc_window",source_scope="owned_window_native_fixture",
                capturer_id=1,capture_frames=sender_sample["captured_frames"],capture_size_mismatches=0)
        probe.correlate_publisher(track, sender_sample, source)
        row = {"target": track, "publisher_probe": sender_sample, "inbound_streams": [stream], "elapsed_s": seconds,
            "state": "connected", "selected_not_bound": 0, "bound_not_selected": 0,
            "render_timer_active": True, "video_stage_visible": True, "renderer_ready": True,
            "stats_age_ms": 10, "layout_mode": "grid" if grid else "speaker",
            "pinned_sid_hash": "" if grid or kind == "screen" else track["sid_hash"],
            "focused_sid_hash": "" if grid else track["sid_hash"],
            "demand_reason": "visible" if grid else "screen_share" if kind == "screen" else "pinned",
            "page": 0, "page_size": int(step["layout"][4:]) if grid else 16,
            "selected": int(step["layout"][4:])-1 if grid else 1 if kind == "screen" else 5,
            "bound": int(step["layout"][4:])-1 if grid else 1 if kind == "screen" else 5,
            "render_submits": 100+seconds*fps, "render_router_submitted": 100+seconds*fps,
            "render_delivered_to_gpu": 100+seconds*fps, "stats_sample_seq": seconds+1,
            "command_seq": 3, "command_status": "applied", "resource_pid": 123,
            "resource_status": "AVAILABLE"}
        row.update({name: 1 for name in probe.render.RESOURCE_COUNTER_FIELDS})
        result.append(row)
    return result, step, source


def received_dimensions(values, width, height):
    for row in values[0]:
        row["target"].update(sink_frame_width=width, sink_frame_height=height,
            render_frame_width=width, render_frame_height=height)
        row["inbound_streams"][0].update(frame_width=width, frame_height=height,
            mapped_sink_frame_width=width, mapped_sink_frame_height=height)
        row["publisher_probe"]["outbound"][0].update(width=width, height=height)
        probe.correlate_publisher(row["target"], row["publisher_probe"], values[2])
    return values


def add_gpu_evidence(values):
    for row in values[0]:
        row.update(gpu_resource_pid=123, gpu_process_start_ticks=123456, gpu_process_image="receiver.exe",
            gpu_process_alive=True, gpu_resource_status="AVAILABLE", gpu_engine_status="AVAILABLE",
            gpu_memory_status="AVAILABLE", gpu_dedicated_status="AVAILABLE", gpu_shared_status="AVAILABLE",
            gpu_sample_interval_seconds=1.0, gpu_busiest_engine_percent=4.0,
            gpu_dedicated_bytes=512*1024*1024, gpu_shared_bytes=32*1024*1024,
            gpu_engine_instances=[{"instance":"pid_123_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D",
                "utilization_percent":4.0}], gpu_process_memory_instances=[])
    return values


class HighresProbeTests(unittest.TestCase):
    def evaluate(self, values):
        data, step, source = values
        return probe.evaluate_step(data, step, source, "a" * 16, freeze.QUALITY_CONTRACT)

    def test_new_layer_fields_are_safe_typed_and_not_raw_strings(self):
        track = rows()[0][0]["target"]
        raw = {"state": "connected", "selected_tracks": [track]}
        with patch.object(probe.soak, "validate_status", return_value={"state": "connected"}), \
                patch.object(probe.hd, "validate_status", side_effect=lambda *_: deepcopy(raw)):
            accepted = probe.validate_status(raw, "test", 1)["selected_tracks"][0]
            self.assertEqual(accepted["seat_selected_layer_quality"], "medium")
            self.assertEqual(accepted["seat_subscription_width"], 640)
            for name, bad in (("seat_subscription_width", True),
                              ("selected_layer_announced_height", -1),
                              ("selected_layer_announced_width", "raw://private"),
                              ("seat_selected_layer_quality", "raw://private")):
                original = track[name]
                track[name] = bad
                with self.subTest(name=name), self.assertRaises(probe.soak.StopRun):
                    probe.validate_status(raw, "test", 1)
                track[name] = original

    def test_real_four_k_phases_preserve_wgc_frame_fps_and_gpu_contracts(self):
        for scenario in ("camera4k", "screen4k"):
            for request in ("main_small", "main_highest"):
                data, step, source = add_gpu_evidence(rows(scenario=scenario, request=request))
                result = probe.evaluate_step(data, step, source, "a"*16, freeze.quality_contract(scenario))
                self.assertEqual(result["status"], "PASS", result["reason"])
                self.assertEqual(result["media_status"], "PASS")
                self.assertEqual(result["gpu_resources"]["status"], "AVAILABLE")
                self.assertEqual(result["gpu_resources"]["performance_acceptance_status"], "NOT_EVALUATED")
                self.assertEqual(result["received_dimensions"], [(3840,2160)] if request == "main_highest"
                    else [(640,360)] if scenario == "camera4k" else [(1920,1080)])
                if request == "main_highest":
                    self.assertEqual(result["source_fps"], 30 if scenario == "camera4k" else 20)

    def test_four_k_declared_but_actual_two_k_or_low_fps_is_fail(self):
        for scenario in ("camera4k", "screen4k"):
            contract = freeze.quality_contract(scenario)
            data, step, source = received_dimensions(add_gpu_evidence(rows(scenario=scenario, request="main_highest")),2560,1440)
            result = probe.evaluate_step(data,step,source,"a"*16,contract)
            self.assertEqual(result["status"],"FAIL")
            self.assertEqual(result["actual_resolution_status"],"FAIL")
            data, step, source = add_gpu_evidence(rows(scenario=scenario,request="main_highest",fps=26 if scenario=="camera4k" else 17))
            result = probe.evaluate_step(data,step,source,"a"*16,contract)
            self.assertIn("frame_rate_below_frozen_minimum",result["reason"])

    def test_four_k_gpu_gap_preserves_media_pass_and_reports_inconclusive(self):
        data, step, source = add_gpu_evidence(rows(scenario="camera4k",request="main_highest"))
        data[-1]["gpu_resource_status"] = "UNKNOWN"
        result = probe.evaluate_step(data,step,source,"a"*16,freeze.quality_contract("camera4k"))
        self.assertEqual(result["media_status"],"PASS")
        self.assertEqual(result["gpu_resources"]["status"],"UNKNOWN")
        self.assertEqual(result["status"],"INCONCLUSIVE")
        self.assertEqual(result["reason"],"gpu_resource_evidence_incomplete")
        data[-1]["target"]["sink_frame_width"] = 1920
        self.assertEqual(probe.evaluate_step(data,step,source,"a"*16,freeze.quality_contract("camera4k"))["status"],"FAIL")

    def test_four_k_capture_scope_or_backend_cannot_be_metadata_only(self):
        for mutation in ({"capture_backend":"synthetic_i420"},{"source_scope":"synthetic_i420_fixture"},
                         {"capturer_id":0},{"capture_frames":0},{"capture_size_mismatches":1}):
            data, step, source = add_gpu_evidence(rows(scenario="camera4k",request="main_highest"))
            data[-1]["publisher_probe"].update(mutation)
            result = probe.evaluate_step(data,step,source,"a"*16,freeze.quality_contract("camera4k"))
            self.assertEqual(result["media_status"],"FAIL",mutation)
            self.assertIn("publisher_layer_evidence_mismatch",result["reason"])

    def test_four_k_api_capture_fps_is_independent_from_rtp_layer_fps(self):
        data, step, source = add_gpu_evidence(rows(scenario="camera4k",request="grid_low"))
        for index,row in enumerate(data):
            row["publisher_probe"].update(captured_frames=100+index*20,capture_frames=101+index*20)
        result = probe.evaluate_step(data,step,source,"a"*16,freeze.quality_contract("camera4k"))
        self.assertEqual(result["source_fps"],15)
        self.assertEqual(result["source_capture_fps"],20)
        self.assertEqual(result["source_capture_rate_status"],"FAIL")
        self.assertEqual(result["media_status"],"FAIL")

    def test_terminal_failure_before_media_plan_preserves_original_safe_cause(self):
        status = {name: 0 for name in ("heartbeat_seq", "runtime_seq", "command_seq",
            "policy_revision", "requested", "selected", "actual", "bound", "selected_not_bound",
            "bound_not_selected", "remote_video_count", "render_submits", "decoded_frames")}
        status.update(schema=1, run_id="terminal-test", pid=123, state="failed",
            command_status="applied", layout_mode="", demand_reason="",
            selected_tracks=[], inbound_streams=[], error_code="coordinator_error")
        with self.assertRaises(probe.ObserverFailed) as caught:
            probe.validate_status(status, "terminal-test", 123)
        error = caught.exception
        self.assertEqual((error.reason, error.status), ("observer_failed", "INCONCLUSIVE"))
        self.assertEqual(error.safe_status["state"], "failed")
        self.assertEqual(error.safe_status["error_code"], "coordinator_error")
        self.assertEqual(error.safe_status["media_projection_status"], "UNAVAILABLE")
        self.assertNotIn("layout_mode", error.safe_status)
        status["error_code"] = "raw websocket://private/token"
        with self.assertRaises(probe.ObserverFailed) as caught:
            probe.validate_status(status, "terminal-test", 123)
        self.assertEqual(caught.exception.safe_status["error_code"], "adapter_reported_error")
        with self.assertRaises(probe.soak.StopRun) as caught:
            probe.validate_status(status, "wrong-run", 123)
        self.assertEqual(caught.exception.reason, "status_identity_mismatch")

    def test_grid_floor_request_does_not_award_received_high_layer_pass(self):
        result = self.evaluate(rows(actual="high"))
        self.assertEqual(result["request_policy_status"], "PASS")
        self.assertEqual(result["actual_resolution_status"], "FAIL")
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("actual_grid_selected_layer_mismatch", result["reason"])
        self.assertEqual(result["source_fps"], 30)

    def test_large_grid_separates_viewport_demand_from_subscribed_layer(self):
        values = rows()
        result = self.evaluate(values)
        # 3840x1968 stage / 4 => 960x492, then fit the 16:9 source.
        # The production positive lround gives 875, rather than truncating 874.
        self.assertEqual(result["viewport_demand_dimensions"], [(875, 492)])
        self.assertEqual(result["requested_dimensions"], [(640, 360)])
        self.assertEqual(result["seat_subscription_dimensions"], [(640, 360)])
        self.assertEqual(result["selected_layer_announced_dimensions"], [(640, 360)])
        self.assertEqual(result["received_dimensions"], [(640, 360)])
        self.assertEqual(result["publisher_rids_correlated"], ["h"])
        self.assertEqual(result["source_fps"], 20)
        self.assertEqual(values[0][0]["target"]["desired_max_fps"], 30)
        self.assertEqual(result["remote_subscriptions"], [15])
        self.assertEqual(result["request_policy_status"], "PASS")
        self.assertEqual(result["actual_resolution_status"], "PASS")
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(values[1]["layout"], "grid16")
        for row in values[0]:
            row["target"].update(viewport_stage_width=3200, viewport_stage_height=1728,
                seat_width=768, seat_height=432)
        self.assertEqual(self.evaluate(values)["request_policy_status"], "PASS")

    def test_large_grid_rejects_wrong_viewport_or_selected_transport(self):
        baseline = rows()
        mutations = ({"window_width": 2566}, {"viewport_stage_width": 3841},
            {"viewport_stage_height": 0}, {"viewport_stage_height": True},
            {"seat_width": 874}, {"seat_width": 1280, "seat_height": 721},
            {"desired_width": 875, "desired_height": 492},
            {"desired_quality": "high"}, {"desired_max_fps": 20},
            {"seat_subscription_width": 320}, {"selected_layer_announced_width": 320},
            {"seat_selected_layer_quality": "high"})
        for mutation in mutations:
            values = deepcopy(baseline)
            for row in values[0]:
                row["target"].update(mutation)
            with self.subTest(mutation=mutation):
                result = self.evaluate(values)
                self.assertEqual(result["request_policy_status"], "FAIL")
                self.assertEqual(result["status"], "FAIL")

    def test_large_grid_real_received_1080_remains_fail(self):
        result = self.evaluate(received_dimensions(rows(actual="high"), 1920, 1080))
        self.assertEqual(result["viewport_demand_dimensions"], [(875, 492)])
        self.assertEqual(result["request_policy_status"], "PASS")
        self.assertEqual(result["actual_resolution_status"], "FAIL")
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("actual_grid_selected_layer_mismatch", result["reason"])

    def test_main_small_ceil_selects_source_highest_without_inventing_720(self):
        result = self.evaluate(rows(request="main_small"))
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["received_dimensions"], [(2560, 1440)])
        self.assertEqual(result["viewport_demand_dimensions"], [(840, 473)])
        self.assertEqual(result["requested_dimensions"], [(2560, 1440)])

    def test_floor_ceil_caps_and_unique_layer_are_distinct(self):
        values, grid, source = rows()
        track = deepcopy(values[0]["target"])
        main = {**grid, "request": "main_small", "layout": "pin_identity"}
        for width, height, expected in ((235, 132, "low"), (640, 360, "medium"),
                                         (875, 492, "medium"), (2560, 1440, "high")):
            track.update(seat_width=width, seat_height=height)
            self.assertEqual(probe.expected_layer(track, source, grid, freeze.QUALITY_CONTRACT), expected)
        # Main has no minimum 720p rule: a 400x225 seat chooses the 360p layer.
        for width, height, expected in ((235, 132, "low"), (400, 225, "medium"),
                                         (875, 492, "high"), (3000, 1688, "high")):
            track.update(seat_width=width, seat_height=height)
            self.assertEqual(probe.expected_layer(track, source, main, freeze.QUALITY_CONTRACT), expected)
        source["layers"]["high"].update(width=3840, height=2160)
        track.update(seat_width=3840, seat_height=2160)
        self.assertEqual(probe.expected_layer(track, source, grid, freeze.QUALITY_CONTRACT), "medium")
        self.assertEqual(probe.expected_layer(track, source, main, freeze.QUALITY_CONTRACT), "high")
        source["layers"]["high"].update(width=7680, height=4320)
        self.assertEqual(probe.expected_layer(track, source, main, freeze.QUALITY_CONTRACT), "medium")
        source["layers"] = {"high": source["layers"]["high"]}
        self.assertEqual(probe.expected_layer(track, source, grid, freeze.QUALITY_CONTRACT), "high")
        self.assertEqual(probe.expected_layer(track, source, main, freeze.QUALITY_CONTRACT), "high")

    def test_portrait_layer_caps_use_long_and_short_edges(self):
        values, grid, source = rows()
        for layer in source["layers"].values():
            layer["width"], layer["height"] = layer["height"], layer["width"]
        track = deepcopy(values[0]["target"])
        track.update(seat_width=492, seat_height=875)
        self.assertEqual(probe.expected_layer(track, source, grid, freeze.QUALITY_CONTRACT), "medium")
        main = {**grid, "request": "main_highest", "layout": "pin_identity"}
        track.update(seat_width=2160, seat_height=3840)
        source["layers"]["high"].update(width=2160, height=3840)
        self.assertEqual(probe.expected_layer(track, source, main, freeze.QUALITY_CONTRACT), "high")

    def test_low_camera_grid_and_2k_main_have_independent_actual_fps(self):
        for request, layer, fps in (("grid_low", "low", 15), ("main_highest", "high", 30)):
            result = self.evaluate(rows(request=request, actual=layer))
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["source_fps"], fps)

    def test_actual_dynamic_low_dimensions_are_distinct_from_announced_layer(self):
        values = rows(request="grid_low", actual="low")
        for row in values[0]:
            row["target"].update(sink_frame_width=240, sink_frame_height=135,
                render_frame_width=240, render_frame_height=135)
            row["inbound_streams"][0].update(frame_width=240, frame_height=135,
                mapped_sink_frame_width=240, mapped_sink_frame_height=135)
            row["publisher_probe"]["outbound"][0].update(width=240, height=135)
            probe.correlate_publisher(row["target"], row["publisher_probe"], values[2])
        result = self.evaluate(values)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["actual_layers"], ["low"])
        self.assertEqual(result["source_fps"], 15)
        self.assertEqual(result["announced_layer_dimensions"], [(320, 180)])
        self.assertEqual(result["received_dimensions"], [(240, 135)])
        self.assertEqual(result["publisher_rids_correlated"], ["q"])
        self.assertEqual(result["receiver_rid"], "UNKNOWN_NOT_EXPOSED")

    def test_fresh_sender_evidence_cannot_be_replaced_with_geometry_or_wrong_source(self):
        for mutate in (lambda sample: sample.update(age_seconds=3.1),
                       lambda sample: sample.update(identity_hash="0" * 16),
                       lambda sample: sample.update(publication_sid_hash="0" * 16),
                       lambda sample: sample.update(source_kind="screen")):
            values = rows(request="grid_low", actual="low")
            mutate(values[0][-1]["publisher_probe"])
            self.assertEqual(self.evaluate(values)["status"], "FAIL")
            self.assertIn("publisher_layer_evidence_mismatch", self.evaluate(values)["reason"])
        values = rows(request="grid_low", actual="low")
        values[0][-1]["publisher_probe"] = None
        self.assertEqual(self.evaluate(values)["status"], "FAIL")

    def test_main_selected_declaration_remains_strict_during_dynamic_encoding(self):
        for request, width, height in (("main_small", 960, 540), ("main_highest", 1920, 1080)):
            values = rows(request=request, actual="high")
            for row in values[0]:
                row["target"].update(sink_frame_width=width, sink_frame_height=height,
                    render_frame_width=width, render_frame_height=height)
                row["inbound_streams"][0].update(frame_width=width, frame_height=height,
                    mapped_sink_frame_width=width, mapped_sink_frame_height=height)
                row["publisher_probe"]["outbound"][0].update(width=width, height=height)
                probe.correlate_publisher(row["target"], row["publisher_probe"], values[2])
            self.assertEqual(self.evaluate(values)["actual_resolution_status"], "FAIL")

    def test_same_rid_changing_dimensions_does_not_award_stability_pass(self):
        values = rows(request="grid_low", actual="low")
        row = values[0][-1]
        row["target"].update(sink_frame_width=240, sink_frame_height=135,
            render_frame_width=240, render_frame_height=135)
        row["inbound_streams"][0].update(frame_width=240, frame_height=135,
            mapped_sink_frame_width=240, mapped_sink_frame_height=135)
        row["publisher_probe"]["outbound"][0].update(width=240, height=135)
        probe.correlate_publisher(row["target"], row["publisher_probe"], values[2])
        result = self.evaluate(values)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["actual_layers"], ["low"])
        self.assertIn("received_dimensions_changed_during_step", result["reason"])

    def test_screen_highest_uses_its_frozen_two_layer_h_rid(self):
        values = rows(scenario="screen", request="main_highest", actual="high")
        result = self.evaluate(values)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["publisher_rids_correlated"], ["h"])
        self.assertEqual(result["source_fps"], 20)
        self.assertEqual(result["selected_layer_qualities"], ["medium"])
        self.assertEqual(values[0][0]["target"]["desired_quality"], "medium")
        wrong = deepcopy(values)
        wrong[0][-1]["target"].update(desired_quality="high", seat_selected_layer_quality="high")
        self.assertEqual(self.evaluate(wrong)["request_policy_status"], "FAIL")
        self.assertEqual(self.evaluate(wrong)["status"], "FAIL")
        values[0][-1]["publisher_probe"]["outbound"][0]["rid"] = "f"
        self.assertEqual(self.evaluate(values)["status"], "FAIL")

    def test_grid_total_seats_requires_real_local_and_exact_remote_capacity(self):
        values = rows(request="grid_low", actual="low")
        result = self.evaluate(values)
        self.assertEqual(result["request_policy_status"], "PASS")
        self.assertEqual(result["remote_subscriptions"], [15])
        self.assertEqual(result["local_participant_seats"], [1])
        for selected, local, sharing in ((16, True, False), (14, True, False),
                                        (16, False, False), (14, True, True)):
            modified = deepcopy(values)
            modified[0][-1].update(selected=selected, bound=selected)
            modified[0][-1]["target"].update(show_local_participant=local,
                                           show_local_screen_share=sharing)
            self.assertEqual(self.evaluate(modified)["request_policy_status"], "FAIL")

    def test_screen_720_low_is_measured_against_3_fps_contract(self):
        result = self.evaluate(rows(scenario="screen", request="main_small", actual="low"))
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["source_fps"], 3)
        self.assertEqual(result["minimum_fps"], 2.7)

    def test_screen_auto_main_source_highest_and_high_fps_are_required(self):
        self.assertEqual(self.evaluate(rows(scenario="screen", request="main_highest"))["status"], "PASS")
        result = self.evaluate(rows(scenario="screen", request="main_highest", fps=17))
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("frame_rate_below_frozen_minimum", result["reason"])

    def test_binding_raw_stats_and_canvas_cannot_be_substituted(self):
        for mutation in (lambda row: row["target"].update(current_binding_serial=2),
                lambda row: row["inbound_streams"][0].update(frame_width=1280),
                lambda row: row["target"].update(render_frame_dimensions_available=False)):
            values = rows(request="main_highest")
            mutation(values[0][-1])
            self.assertEqual(self.evaluate(values)["status"], "FAIL")

    def test_incomplete_resources_and_sample_gaps_are_not_pass(self):
        values = rows(request="main_highest")
        values[0][-1]["resource_status"] = "UNKNOWN"
        self.assertEqual(self.evaluate(values)["status"], "INCONCLUSIVE")
        values = rows(request="main_highest")
        values[0][-1]["elapsed_s"] = 40
        self.assertIn("sample_gap", self.evaluate(values)["reason"])

    def test_screen_pin_is_not_auto_focus_evidence(self):
        values = rows(scenario="screen", request="main_small", actual="high")
        values[0][-1]["pinned_sid_hash"] = "a" * 16
        values[0][-1]["demand_reason"] = "pinned"
        self.assertEqual(self.evaluate(values)["request_policy_status"], "FAIL")


if __name__ == "__main__":
    unittest.main()
