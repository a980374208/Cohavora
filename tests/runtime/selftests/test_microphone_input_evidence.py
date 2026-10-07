"""Input evidence must fail closed on silence, source failure, or missing rows."""
import copy
import importlib.util
import unittest
from pathlib import Path

PATH = Path(__file__).resolve().parents[1] / "tools/media/microphone_input/probe_microphone_input.py"
SPEC = importlib.util.spec_from_file_location("microphone_input_witness", PATH)
WITNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WITNESS)
RUN = "1" * 32


def capture_rows():
    data = [{"event": "collector.started", "utc_ms": 0, "configuration": "RelWithDebInfo",
             "endpoint_sha256": "2" * 64, "role": "eConsole", "sample_rate": 48000,
             "endpoint_muted": False, "endpoint_volume": .5,
             "minimum_rms": .005, "minimum_tone_fraction": .30, "maximum_peak": .95,
             "tone_hz": 997, "tone_peak": .025}]
    for second in range(1, 46):
        data.append({"event": "microphone.sample", "utc_ms": second * 1000,
                     "window_seconds": 1, "window_frames": 48000, "blocks_100ms": 10,
                     "non_silent_blocks": 10, "tone_matched_blocks": 10, "rms": .02, "peak": .03,
                     "max_packet_gap_ms": 11, "discontinuities": 0, "timestamp_errors": 0,
                     "missing_device_frames": 0, "endpoint_muted": False,
                     "endpoint_volume": .5, "default_endpoint_unchanged": True})
    data.append({"event": "collector.stopped", "utc_ms": 45000, "status": "COMPLETE"})
    for i, row in enumerate(data, 1):
        row.update(sequence=i, run_id=RUN)
    return data


class EvidenceTests(unittest.TestCase):
    def review(self, data, mode="external-tone", tone=None, exit_code=0):
        return WITNESS.summarize(data, tone or [], exit_code, 0, RUN, mode)

    def test_known_source_requires_complete_continuous_evidence(self):
        self.assertTrue(self.review(capture_rows())["source_confirmed"])
        for key, value in (("non_silent_blocks", 9), ("tone_matched_blocks", 9),
                           ("max_packet_gap_ms", 201), ("default_endpoint_unchanged", False)):
            data = capture_rows()
            data[20][key] = value
            self.assertFalse(self.review(data)["source_confirmed"], key)

    def test_baseline_cannot_certify_known_fixture(self):
        result = self.review(capture_rows(), "baseline")
        self.assertTrue(result["continuous_input_observed"])
        self.assertFalse(result["source_confirmed"])

    def test_missing_or_truncated_collection_cannot_pass(self):
        data = capture_rows()
        del data[20]
        self.assertFalse(self.review(data)["source_confirmed"])
        self.assertFalse(self.review(capture_rows()[:-1])["source_confirmed"])
        self.assertFalse(self.review(capture_rows(), exit_code=1)["source_confirmed"])

    def test_muted_render_cannot_prove_acoustic_source(self):
        tone = [{"event": "collector.started", "utc_ms": 1000, "endpoint_muted": True},
                {"event": "collector.stopped", "utc_ms": 44000, "status": "COMPLETE"}]
        for i, row in enumerate(tone, 1):
            row.update(sequence=i, run_id=RUN)
        result = self.review(capture_rows(), "coupled", tone)
        self.assertFalse(result["source_confirmed"])
        self.assertEqual(result["verdict"], "TONE_SOURCE_FAILED")

    def test_unobserved_coupled_source_cannot_pass(self):
        self.assertFalse(self.review(capture_rows(), "coupled")["source_confirmed"])

    def test_collector_policy_mismatch_cannot_pass(self):
        data = copy.deepcopy(capture_rows())
        data[0]["minimum_rms"] = .001
        self.assertFalse(self.review(data)["source_confirmed"])


if __name__ == "__main__":
    unittest.main()
