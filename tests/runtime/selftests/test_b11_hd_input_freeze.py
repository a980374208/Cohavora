"""Offline independent HD freeze contracts; no SSH, SFU or media execution."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / "tools/meeting"
sys.path.insert(0, str(TOOLS))
import b11_hd_input_freeze as hd


def source_identity(checkpoint: Path):
    assets = []
    for prefix in ("crescent", "neon", "tunnel"):
        for quality, kbps in (("low", 150), ("medium", 600), ("high", 2000)):
            layer = hd.QUALITY_CONTRACT[quality]
            height = 150 if quality == "low" and prefix != "neon" else layer["height"]
            assets.append({"prefix": prefix, "quality": quality,
                           "path": f"pkg/provider/resources/{prefix}_{layer['height']}_{kbps}.ivf",
                           "lfs_sha256": "a" * 64, "size": 1000, "embedded_offset": 100,
                           "embedded_match": True,
                           "ivf": {"fourcc": "VP80", "width": layer["width"], "height": height,
                                   "timebase_denominator": layer["source_fps"],
                                   "timebase_numerator": 1, "frame_count": 30},
                           "first_keyframe_dimensions": [layer["width"], height],
                           "advertised_dimensions": [layer["width"], layer["height"]],
                           "advertised_dimensions_match_asset": height == layer["height"],
                           "publisher_fps": layer["source_fps"], "advertised_bitrate_bps": kbps * 1000})
    names = ["pkg/provider/embeds.go", "pkg/provider/vp8looper.go",
             "pkg/loadtester/loadtest.go", "pkg/loadtester/loadtester.go"]
    return {"schema": 1, "kind": "B11_HD_SOURCE_IDENTITY", "tag": "v2.18.8",
            "cli_path": "/home/ubuntu/.local/bin/lk", "cli_version": hd.CLI_VERSION,
            "cli_sha256": hd.CLI_SHA256, "identity_format": "<identity_prefix>_pub_<sequence>",
            "hd_sequence": 0, "first_vp8_prefix_per_process": "neon",
            "source_evidence": [{"path": name,
                                 "url": "https://raw.githubusercontent.com/livekit/livekit-cli/v2.18.8/" + name,
                                 "sha256": "b" * 64, "matches": [{"line": 1, "text": "offline fixture"}]}
                                for name in names],
            "assets": assets,
            "load_test_flags": ["--identity-prefix", "--publishers", "--subscribers",
                                "--video-resolution", "--video-codec", "--duration",
                                "--num-per-second", "--no-simulcast"],
            "runtime_status": "NOT_RUN", "source_equivalence": "OFFICIAL_ASSETS_MATCH_REMOTE_CLI_BYTES",
            "fps_evidence": "SPEC_AND_FRAME_DURATION_SOURCE; RECEIVE_FPS_NOT_RUN",
            "remote_prerequisite": {"instance": "ssh:192.0.2.10", "service_url": "ws://192.0.2.10:17880",
                                    "config_path": "/home/ubuntu/livekit.yml", "sfu_container": "livekit",
                                    "config_sha256": "c" * 64, "sfu_image": "sha256:" + "d" * 64,
                                    "logical_cpus_observed": 16, "mem_total_kib_observed": 64418860,
                                    "configured_egress_mbps": 20,
                                    "configured_egress_evidence_source": "USER_CONFIRMED",
                                    "provider_api_independently_verified": False,
                                    "prerequisite_checkpoint_file": hd.base._file_record(checkpoint)}}


class HdFreezeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.profile = json.loads((TOOLS / "b11_hd_layer_profile.json").read_text(encoding="utf-8"))
        self.profile_path = self.root / "profile.json"
        self.source_path = self.root / "out/b11-hd/source-identity.json"
        self.checkpoint = self.write("checkpoint.json", "offline prior prerequisite\n")
        self.source = source_identity(self.checkpoint)
        self.write_json(self.source_path, self.source)
        self.write_json(self.profile_path, self.profile)
        self.exe = self.write("build/RelWithDebInfo/" + hd.EXECUTABLE_NAME, "offline binary")
        self.exe.with_suffix(".pdb").write_bytes(b"offline pdb")
        self.shared_source = self.write("src/renderer.cpp", "offline current source")
        for name in hd.TOOL_INPUTS:
            self.write(name, "offline tool")
        self.manifest = self.root / "manifest.json"
        self.addCleanup(patch.stopall)
        patch.object(hd.base, "_git_head", return_value="a" * 40).start()
        patch.object(hd.base, "_source_paths", return_value=[self.shared_source]).start()
        patch.object(hd.base, "_binary_identity", side_effect=lambda path: {
            "configuration": "RelWithDebInfo", "binary_sha256": hd.base.sha256(path)}).start()
        patch.object(hd.base, "_build_metadata", return_value={"configuration": "RelWithDebInfo"}).start()
        self.remote = {"transport": "ssh", "host": "192.0.2.10", "key_public_fingerprint": "SHA256:offline"}
        patch.object(hd.base, "_remote_transport_identity", side_effect=lambda *_: dict(self.remote)).start()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def write_json(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2), encoding="utf-8")

    def freeze(self):
        return hd.freeze_inputs(self.manifest, self.exe, self.profile_path, root=self.root)

    def verify(self, **kwargs):
        return hd.verify_inputs(self.manifest, self.exe, self.profile_path, root=self.root, **kwargs)

    def test_independent_hd_scope_and_runtime_unknown_are_preserved(self):
        result = self.freeze()
        self.assertEqual(self.verify(), result)
        self.assertEqual(result["scope"], "B11_HD_LAYER_DIAGNOSTIC")
        self.assertEqual(result["formal_b11_status"], "NOT_RUN")
        self.assertEqual(result["runtime_status"], "NOT_RUN")
        self.assertEqual(result["binary_source_equivalence"], "UNKNOWN")
        self.assertEqual(result["inputs"]["hd_source_identity"], "b11hd_RUN01_pub_0")
        with self.assertRaisesRegex(ValueError, "invalid_b11_profile"):
            hd.base.validate_profile(self.profile)

    def test_manifest_cannot_be_overwritten_or_promoted_to_pass(self):
        self.freeze()
        content = self.manifest.read_bytes()
        with self.assertRaisesRegex(ValueError, "freeze_manifest_already_exists"):
            self.freeze()
        self.assertEqual(self.manifest.read_bytes(), content)
        changed = json.loads(content)
        changed["formal_b11_status"] = "PASS"
        self.write_json(self.manifest, changed)
        with self.assertRaisesRegex(ValueError, "invalid_hd_manifest"):
            self.verify()

    def test_publisher_cardinality_layer_mode_and_codec_are_closed(self):
        for role, key, replacement in (("hd", "count", 17), ("hd", "resolution", "low"),
                                       ("hd", "simulcast", False), ("background", "resolution", "high"),
                                       ("background", "count", 15), ("hd", "codec", "h264"),
                                       ("hd", "count", True)):
            with self.subTest(role=role, key=key):
                value = deepcopy(self.profile)
                value["publishers"][role][key] = replacement
                with self.assertRaisesRegex(ValueError, "invalid_hd_publishers"):
                    hd.validate_profile(value)

    def test_identity_binding_requires_two_distinct_matching_run_prefixes(self):
        value = deepcopy(self.profile)
        value["publishers"]["background"]["identity_prefix"] = "b11low_OTHER"
        with self.assertRaisesRegex(ValueError, "hd_identity_run_mismatch"):
            hd.validate_profile(value)
        value["publishers"]["background"]["identity_prefix"] = "b11hd_RUN01"
        with self.assertRaisesRegex(ValueError, "invalid_hd_identity_prefix"):
            hd.validate_profile(value)

    def test_unknown_fields_and_credentials_are_rejected(self):
        for parent in (None, "publishers"):
            value = deepcopy(self.profile)
            owner = value if parent is None else value[parent]["hd"]
            owner["api_secret"] = "offline-secret"
            with self.subTest(parent=parent), self.assertRaises(ValueError):
                hd.validate_profile(value)

    def test_thresholds_and_steps_cannot_silently_relax(self):
        for key, replacement in (("minimum_frame_rate_ratio", 0.8), ("dimension_match_ratio", 0.9),
                                 ("minimum_observation_seconds", 5), ("maximum_frame_age_ms", 5000)):
            value = deepcopy(self.profile)
            value["quality_contract"][key] = replacement
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "invalid_hd_profile"):
                hd.validate_profile(value)
        value = deepcopy(self.profile)
        value["probe"]["steps"][1]["seconds"] = 20
        with self.assertRaisesRegex(ValueError, "invalid_hd_profile"):
            hd.validate_profile(value)
        value = deepcopy(self.profile)
        value["probe"]["receiver_arguments"] = []
        with self.assertRaisesRegex(ValueError, "invalid_hd_profile"):
            hd.validate_profile(value)

    def test_source_assets_require_all_nine_and_actual_embedded_dimensions(self):
        for mutation in (lambda v: v["assets"].pop(),
                         lambda v: v["assets"][0].update(embedded_match=False),
                         lambda v: v["assets"][2]["ivf"].update(width=320),
                         lambda v: v["assets"][2].update(first_keyframe_dimensions=[320, 180]),
                         lambda v: v["assets"][2].update(publisher_fps=15)):
            value = deepcopy(self.source)
            mutation(value)
            with self.assertRaises(ValueError):
                hd._validate_source_identity(value)

    def test_source_cli_and_unit_provenance_cannot_be_promoted(self):
        for owner, key, replacement in ((None, "cli_sha256", "0" * 64),
                                        (None, "first_vp8_prefix_per_process", "crescent"),
                                        ("remote_prerequisite", "configured_egress_mbps", 160),
                                        ("remote_prerequisite", "provider_api_independently_verified", True)):
            value = deepcopy(self.source)
            (value if owner is None else value[owner])[key] = replacement
            with self.subTest(key=key), self.assertRaises(ValueError):
                hd._validate_source_identity(value)

    def test_source_definition_and_build_inputs_are_frozen(self):
        self.freeze()
        for path in (self.source_path, self.shared_source, self.exe, self.exe.with_suffix(".pdb"),
                     self.root / hd.TOOL_INPUTS[1]):
            before = path.read_bytes()
            path.write_bytes(before + b" ")
            with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
                self.verify()
            path.write_bytes(before)

    def test_prerequisite_file_hash_and_target_match_are_required(self):
        self.checkpoint.write_text("different prerequisite", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "hd_prerequisite_checkpoint_changed"):
            self.freeze()
        self.source["remote_prerequisite"]["instance"] = "ins-wrongtarget"
        self.write_json(self.source_path, self.source)
        with self.assertRaisesRegex(ValueError, "hd_source_target_mismatch"):
            self.freeze()

    def test_ssh_identity_and_cli_target_arguments_cannot_drift(self):
        self.freeze()
        for kwargs in ({"instance": "ins-wrongtarget"}, {"service_url": "ws://192.0.2.11:17880"},
                       {"target_config": self.root / "different.json"}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ValueError, "hd_runtime_target_mismatch"):
                self.verify(**kwargs)
        self.remote["key_public_fingerprint"] = "SHA256:changed"
        with self.assertRaisesRegex(ValueError, "frozen_inputs_changed"):
            self.verify()

    def test_profile_duplicate_json_key_fails_closed(self):
        text = self.profile_path.read_text(encoding="utf-8")
        self.profile_path.write_text(text[:-1] + ',"scope":"B11_GRID16_PREFLIGHT"}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate_json_key"):
            self.freeze()


if __name__ == "__main__":
    unittest.main()
