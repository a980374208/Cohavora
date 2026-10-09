"""Offline high-resolution source/task/artifact binding, without services or media."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

TOOLS = Path(__file__).resolve().parents[1] / "tools/meeting"
sys.path.insert(0, str(TOOLS))
import b11_highres_input_freeze as freeze


class HighresFreezeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        target = json.loads((TOOLS / "b11_hd_layer_profile.json").read_text(encoding="utf-8"))["target"]
        self.profile = freeze.make_profile("camera", target, "target.json", "source-identity.json")
        self.profile_path = self.write_json("profile.json", self.profile)
        self.exe = self.write("build/RelWithDebInfo/" + freeze.EXECUTABLE_NAME, "receiver")
        self.write(str(self.exe.relative_to(self.root).with_suffix(".pdb")), "receiver pdb")
        self.publisher = self.write("build/RelWithDebInfo/test_screen_share_runtime.exe", "publisher")
        self.symbols = self.write("build/RelWithDebInfo/test_screen_share_runtime.pdb", "publisher pdb")
        self.ready = self.write_json("publisher-ready.json", {"backend": "synthetic_i420"})
        self.server = self.write_json("server-snapshot.json", {
            "ok": True, "target_identity_hash": hashlib.sha256(b"highres-camera").hexdigest()[:16],
            "target_track_count": 1, "tracks": [{"sid_hash": "c" * 16, "source": "camera",
                "width": 2560, "height": 1440, "layers": [
                    {"width": layer["width"], "height": layer["height"],
                        "quality": {"low": 0, "medium": 1, "high": 2}[layer["quality"]]}
                    for layer in freeze.LAYERS["camera"].values()]}]})
        self.source = {"schema": 1, "kind": "B11_HIGHRES_SOURCE_IDENTITY", "scenario": "camera",
            "task": {"task_id": "OFFLINE", "room": "b11-highres-offline"}, "target": target,
            "target_identity": "highres-camera", "source": deepcopy(freeze.SOURCES["camera"]),
            "layers": deepcopy(freeze.LAYERS["camera"]), "publisher_configuration": "RelWithDebInfo",
            "remote_prerequisite": {"config_sha256": "a" * 64, "sfu_image": "sha256:" + "b" * 64},
            "artifacts": {"publisher_binary": freeze.base._file_record(self.publisher),
                "publisher_symbols": freeze.base._file_record(self.symbols),
                "publisher_ready": freeze.base._file_record(self.ready),
                "server_snapshot": freeze.base._file_record(self.server), "extra": []}}
        self.source_path = self.write_json("source-identity.json", self.source)
        self.shared_source = self.write("src/source_dimensions.cpp", "source chain")
        for name in freeze.TOOL_INPUTS:
            self.write(name, "offline input")
        self.manifest = self.root / "freeze.json"
        self.addCleanup(patch.stopall)
        patch.object(freeze.base, "_git_head", return_value="a" * 40).start()
        patch.object(freeze.base, "_source_paths", return_value=[self.shared_source]).start()
        patch.object(freeze.base, "_binary_identity", side_effect=lambda path: {
            "configuration": "RelWithDebInfo", "binary_sha256": freeze.base.sha256(path)}).start()
        self.publisher_identity_patch = patch.object(freeze, "_publisher_binary_identity", side_effect=lambda path, _: {
            "configuration": "RelWithDebInfo", "binary_sha256": freeze.base.sha256(path)})
        self.publisher_identity_patch.start()
        patch.object(freeze.base, "_build_metadata", return_value={"configuration": "RelWithDebInfo"}).start()
        self.transport = {"host": "192.0.2.10", "transport": "ssh", "key_public_fingerprint": "SHA256:offline"}
        patch.object(freeze.base, "_remote_transport_identity", side_effect=lambda *_: dict(self.transport)).start()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def write_json(self, name, value):
        return self.write(name, json.dumps(value))

    def freeze(self):
        return freeze.freeze_inputs(self.manifest, self.exe, self.profile_path, root=self.root)

    def verify(self, **kwargs):
        return freeze.verify_inputs(self.manifest, self.exe, self.profile_path, root=self.root, **kwargs)

    def test_new_scope_has_no_formal_credit(self):
        value = self.freeze()
        self.assertEqual(value, self.verify())
        self.assertEqual(value["formal_b11_status"], "NOT_RUN")
        self.assertEqual(value["binary_source_equivalence"], "UNKNOWN")
        self.assertEqual(value["inputs"]["source_track_sid_hash"], "c" * 16)
        self.assertEqual(value["scope"], "B11_HIGHRES_PIXEL_LAYER_SELECTION_DIAGNOSTIC_V2")
        self.assertEqual(value["inputs"]["profile"]["quality_contract"]["policy_version"], 2)

    def test_four_k_scope_preserves_two_k_contract_and_requires_real_capture(self):
        two_k = deepcopy(self.profile)
        for scenario in ("camera4k","screen4k"):
            profile = freeze.make_profile(scenario,self.profile["target"],"target.json","source-identity.json")
            self.assertEqual(profile["scope"],freeze.FOUR_K_SCOPE)
            self.assertEqual(freeze.validate_profile(profile),profile)
            self.assertEqual(profile["quality_contract"]["gpu_required_scope"],"measurement_steps")
            self.assertTrue(profile["quality_contract"]["require_gpu_resource_evidence"])
            source = deepcopy(self.source)
            source.update(scenario=scenario,source=deepcopy(freeze.SOURCES[scenario]),layers=deepcopy(freeze.LAYERS[scenario]))
            freeze.validate_source_identity(source,profile)
            source["source"]["capture_backend"] = "synthetic_i420"
            with self.assertRaisesRegex(ValueError,"invalid_highres_source_identity"):
                freeze.validate_source_identity(source,profile)
            changed = deepcopy(profile)
            changed["quality_contract"]["require_gpu_resource_evidence"] = False
            with self.assertRaisesRegex(ValueError,"invalid_highres_profile"):
                freeze.validate_profile(changed)
        self.assertEqual(self.profile,two_k)
        self.assertNotIn("require_gpu_resource_evidence",two_k["quality_contract"])

    def test_four_k_ready_and_server_dimensions_are_bound_independently(self):
        for scenario in ("camera4k","screen4k"):
            source = deepcopy(self.source)
            source.update(scenario=scenario,source=deepcopy(freeze.SOURCES[scenario]),layers=deepcopy(freeze.LAYERS[scenario]))
            snapshot = json.loads(self.server.read_text())
            snapshot["tracks"][0].update(source=source["source"]["source"],width=3840,height=2160,layers=[
                {"width":layer["width"],"height":layer["height"],
                 "quality":{"low":0,"medium":1,"high":2}[layer["quality"]]}
                for layer in source["layers"].values()])
            self.assertEqual(freeze._source_track_hash(snapshot,source),"c"*16)
            ready = {"schema":1,"event":"ready","source_kind":freeze.scenario_kind(scenario),
                "source_width":3840,"source_height":2160,"target_fps":source["source"]["source_fps"],
                "capture_backend":"wgc_window","source_scope":"owned_window_native_fixture",
                "capturer_id":1,"captured_frames":9,"capture_frames":10,"capture_size_mismatches":0,
                "identity_hash":snapshot["target_identity_hash"],"publication_sid_hash":"c"*16}
            freeze._validate_four_k_ready(ready,source,"c"*16)
            for mutation in ({"source_width":2560},{"capture_backend":"synthetic_i420"},{"capturer_id":True},
                             {"capture_frames":0},{"source_scope":"screen_share_session"},{"capture_size_mismatches":1}):
                with self.subTest(mutation=mutation),self.assertRaisesRegex(ValueError,"highres_native_four_k_capture_mismatch"):
                    freeze._validate_four_k_ready({**ready,**mutation},source,"c"*16)
            snapshot["tracks"][0]["width"] = 2560
            with self.assertRaisesRegex(ValueError,"invalid_highres_server_track"):
                freeze._source_track_hash(snapshot,source)

    def test_new_contract_rejects_old_720_policy_and_preserves_closed_plan(self):
        self.assertEqual([step["request"] for step in self.profile["probe"]["steps"]],
                         ["grid_low", "grid_large", "main_highest", "grid_large"])
        self.assertEqual(self.profile["quality_contract"]["grid_maximum_dimensions"], [2560, 1440])
        self.assertEqual(self.profile["quality_contract"]["main_maximum_dimensions"], [3840, 2160])
        self.assertIsNone(self.profile["quality_contract"]["main_minimum_dimensions"])
        for mutate in (lambda value: value.update(scope="B11_HIGHRES_UNIFIED_DEMAND_DIAGNOSTIC"),
                       lambda value: value["quality_contract"].update(main_minimum_dimensions=[1280, 720]),
                       lambda value: value["quality_contract"].update(grid_maximum_dimensions=[1280, 720]),
                       lambda value: value["probe"].update(steps=value["probe"]["steps"][1:3])):
            value = deepcopy(self.profile)
            mutate(value)
            with self.assertRaisesRegex(ValueError, "invalid_highres_profile"):
                freeze.validate_profile(value)

    def test_publisher_identity_has_its_own_exact_name_and_configuration_gate(self):
        self.publisher_identity_patch.stop()
        identity = {"configuration": "RelWithDebInfo", "binary_sha256": freeze.base.sha256(self.publisher),
            "binary_path": str(self.publisher)}
        with patch.object(freeze.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(identity))) as command:
            self.assertEqual(freeze._publisher_binary_identity(self.publisher, self.root), identity)
            arguments = command.call_args.args[0]
            self.assertEqual(arguments[arguments.index("-ExpectedExecutableName") + 1], "test_screen_share_runtime.exe")
            self.assertEqual(arguments[arguments.index("-Configuration") + 1], "RelWithDebInfo")
        for key, value in (("configuration", "Debug"), ("binary_sha256", "0" * 64),
                ("binary_path", str(self.exe))):
            invalid = {**identity, key: value}
            with patch.object(freeze.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(invalid))), \
                    self.assertRaisesRegex(ValueError, "highres_publisher_binary_not_verified"):
                freeze._publisher_binary_identity(self.publisher, self.root)
        with self.assertRaisesRegex(ValueError, "highres_publisher_binary_not_verified"):
            freeze._publisher_binary_identity(self.exe, self.root)

    def test_profile_durations_caps_thresholds_and_auto_are_closed(self):
        for mutation in (lambda v: v["probe"]["steps"][0].update(seconds=10),
                lambda v: v["probe"]["steps"][1].update(window_width=1600),
                lambda v: v["probe"]["steps"][1].update(layout="grid4"),
                lambda v: v["probe"].update(minimum_remote_videos=17),
                lambda v: v["quality_contract"].update(minimum_frame_rate_ratio=0.7),
                lambda v: v.update(api_secret="offline")):
            value = deepcopy(self.profile)
            mutation(value)
            with self.assertRaises(ValueError):
                freeze.validate_profile(value)

    def test_camera_profile_keeps_source_on_all_fifteen_remote_seats(self):
        self.assertEqual(self.profile["probe"]["minimum_remote_videos"], 15)
        self.assertTrue(all(step["layout"] == "grid16"
            for step in self.profile["probe"]["steps"] if step["request"].startswith("grid")))
        stored = json.loads((TOOLS / "b11_highres_camera_profile.json").read_text(encoding="utf-8"))
        self.assertEqual(stored["probe"], self.profile["probe"])
        freeze.validate_profile(stored)
        screen = json.loads((TOOLS / "b11_highres_screen_profile.json").read_text(encoding="utf-8"))
        self.assertEqual(screen["probe"], freeze.PROBES["screen"])
        for scenario in ("camera4k", "screen4k"):
            stored = json.loads((TOOLS / f"b11_highres_{scenario}_profile.json").read_text(encoding="utf-8"))
            self.assertEqual(stored["probe"], freeze.PROBES[scenario])
            freeze.validate_profile(stored)

    def test_missing_720_camera_layer_cannot_be_invented(self):
        value = deepcopy(self.source)
        value["layers"]["medium"].update(width=1280, height=720)
        with self.assertRaisesRegex(ValueError, "invalid_highres_source_identity"):
            freeze.validate_source_identity(value, self.profile)
        profile = freeze.make_profile("screen", self.profile["target"], "target.json", "source.json")
        value.update(scenario="screen", source=deepcopy(freeze.SOURCES["screen"]), layers=deepcopy(freeze.LAYERS["screen"]))
        value["layers"]["low"]["source_fps"] = 20
        with self.assertRaisesRegex(ValueError, "invalid_highres_source_identity"):
            freeze.validate_source_identity(value, profile)

    def test_screen_high_alias_freezes_its_real_medium_announcement(self):
        source = deepcopy(self.source)
        source.update(scenario="screen", source=deepcopy(freeze.SOURCES["screen"]),
                      layers=deepcopy(freeze.LAYERS["screen"]))
        snapshot = json.loads(self.server.read_text())
        snapshot["tracks"][0].update(source="screen_share", layers=[
            {"width": layer["width"], "height": layer["height"],
             "quality": {"low": 0, "medium": 1, "high": 2}[layer["quality"]]}
            for layer in source["layers"].values()])
        self.assertEqual(source["layers"]["high"]["quality"], "medium")
        self.assertEqual(freeze._source_track_hash(snapshot, source), "c" * 16)
        snapshot["tracks"][0]["layers"][1]["quality"] = 2
        with self.assertRaisesRegex(ValueError, "highres_server_layers_mismatch"):
            freeze._source_track_hash(snapshot, source)

    def test_remote_source_type_dimensions_and_layer_snapshot_are_required(self):
        snapshot = json.loads(self.server.read_text())
        for mutation in (lambda v: v.update(target_track_count=2),
                lambda v: v["tracks"][0].update(source="screen_share"),
                lambda v: v["tracks"][0].update(width=1280),
                lambda v: v["tracks"][0]["layers"][1].update(width=1280, height=720)):
            value = deepcopy(snapshot)
            mutation(value)
            with self.assertRaises(ValueError):
                freeze._source_track_hash(value, self.source)

    def test_artifacts_and_source_chain_cannot_drift(self):
        self.freeze()
        for path in (self.publisher, self.symbols, self.ready, self.server, self.source_path,
                self.exe, self.exe.with_suffix(".pdb"), self.shared_source,
                self.root / "tests/runtime/tools/meeting/b11_highres_layer_probe.py"):
            content = path.read_bytes()
            path.write_bytes(content + b" ")
            with self.subTest(path=path.name), self.assertRaises(ValueError):
                self.verify()
            path.write_bytes(content)

    def test_transport_and_target_arguments_cannot_drift(self):
        self.freeze()
        with self.assertRaisesRegex(ValueError, "highres_runtime_target_mismatch"):
            self.verify(service_url="ws://192.0.2.11:17880")
        self.transport["key_public_fingerprint"] = "SHA256:changed"
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()

    def test_existing_manifest_cannot_be_overwritten_or_promoted(self):
        value = self.freeze()
        before = self.manifest.read_bytes()
        with self.assertRaisesRegex(ValueError, "freeze_manifest_already_exists"):
            self.freeze()
        self.assertEqual(before, self.manifest.read_bytes())
        value["runtime_status"] = "PASS"
        self.write_json("freeze.json", value)
        with self.assertRaisesRegex(ValueError, "invalid_highres_manifest"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
