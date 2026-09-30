"""Offline contract checks; these are not desktop or media acceptance."""

import json
import hashlib
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

# Support direct execution and unittest discovery from any working directory.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/product_acceptance"))

import verify_product_acceptance as acceptance


class AcceptanceContractTests(unittest.TestCase):
    def test_bundle_hash_mismatch_fails_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            session = "b" * 32
            names = ["build.json", "environment.json", "diagnostics-health.json",
                     f"sessions/{session}/events.jsonl", f"sessions/{session}/stability.json",
                     f"sessions/{session}/telemetry/checkpoint.json",
                     f"sessions/{session}/telemetry/metrics.jsonl"]
            files = []
            for name in names:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                content = b"{}"
                target.write_bytes(content)
                files.append(dict(path=name, size_bytes=len(content),
                                  sha256=hashlib.sha256(content).hexdigest()))
            (root / "diagnostics-health.json").write_text(json.dumps(dict(
                timeline_omitted=0, timeline_invalid_lines=0, timeline_skipped_files=0,
                checkpoint_missing_revisions=0,
                persistent_losses=dict(availability="valid", dropped_records=0))))
            health = root / "diagnostics-health.json"
            files[2]["size_bytes"] = health.stat().st_size
            files[2]["sha256"] = hashlib.sha256(health.read_bytes()).hexdigest()
            (root / "manifest.json").write_text(json.dumps(dict(
                schema="cohavora-diagnostic-bundle", schema_version=1,
                session_complete=True, anonymous_session_id=session,
                files=files, missing=[])))
            acceptance.validate_bundle(root)
            health.write_text(json.dumps(dict(
                timeline_omitted=0, timeline_invalid_lines=0, timeline_skipped_files=0,
                checkpoint_missing_revisions=0,
                persistent_losses=dict(availability="missing"))))
            files[2]["size_bytes"] = health.stat().st_size
            files[2]["sha256"] = hashlib.sha256(health.read_bytes()).hexdigest()
            (root / "manifest.json").write_text(json.dumps(dict(
                schema="cohavora-diagnostic-bundle", schema_version=1,
                session_complete=True, anonymous_session_id=session,
                files=files, missing=["persistent_loss_summary"])))
            acceptance.validate_bundle(root)
            (root / "build.json").write_text("tampered")
            with self.assertRaisesRegex(acceptance.EvidenceError, "bundle_hash_mismatch"):
                acceptance.validate_bundle(root)

    def test_one_process_must_complete_all_100_lifecycles(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            origin = datetime(2026, 1, 1, tzinfo=timezone.utc)
            run_id = "a" * 32
            actions = []
            for cycle in range(1, 101):
                for offset, name in enumerate(acceptance.REQUIRED_ACTIONS):
                    op = f"{cycle:016x}{offset:016x}"
                    for phase_index, phase in enumerate(("requested", "uia_observed")):
                        actions.append(dict(run_id=run_id, cycle=cycle, pid=1234,
                                            operation_id=op, action=name, phase=phase,
                                            utc=(origin + timedelta(seconds=cycle * 290 +
                                                    offset * 2 + phase_index)).isoformat()))
            for phase_index, phase in enumerate(("requested", "uia_observed")):
                actions.append(dict(run_id=run_id, cycle=100, pid=1234,
                                    operation_id="f" * 32, action="process_exit", phase=phase,
                                    utc=(origin + timedelta(seconds=29020 + phase_index)).isoformat()))
            (root / "uia-result.json").write_text(json.dumps(dict(
                verdict="UIA_COMPLETE", cycles_requested=100, cycles_completed=100,
                minimum_seconds=28800, run_id=run_id, started_utc=origin.isoformat(),
                finished_utc=(origin + timedelta(seconds=29030)).isoformat())))
            action_path = root / "uia-actions.jsonl"
            action_path.write_text("".join(json.dumps(row) + "\n" for row in actions))
            self.assertEqual(len(acceptance.validate_uia(root)[2]), 100)
            action_path.write_text("".join(json.dumps(row) + "\n" for row in actions
                                           if not (row["cycle"] == 42 and row["action"] == "leave")))
            with self.assertRaisesRegex(acceptance.EvidenceError, "incomplete_uia_cycle"):
                acceptance.validate_uia(root)

    def test_retained_segments_can_start_after_zero_but_gap_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / "run-123"
            root.mkdir()
            (root / "segment-000015.jsonl").write_text(
                '{"event_sequence":100}\n{"event_sequence":102}\n')
            (root / "segment-000016.jsonl").write_text('{"event_sequence":103}\n')
            count, gaps = acceptance.validate_segments(root.parent)
            self.assertEqual(count, 1)
            self.assertEqual([(gap["first_missing"], gap["last_missing"])
                              for gap in gaps], [(1, 99), (101, 101)])

    def test_missing_gpu_and_queue_samples_cannot_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "resources.jsonl"
            path.write_text(json.dumps(dict(run_id="a" * 32, collector="external",
                                            cycle=1, pid=1234, utc=datetime.now(timezone.utc).isoformat(),
                                            phase="active", private_bytes=10, handles=3,
                                            threads=2, gpu_local_bytes=None,
                                            gpu_nonlocal_bytes=None, queue_depth=None,
                                            operation_id="b" * 32, wgc_handles=0)) + "\n")
            with self.assertRaisesRegex(acceptance.EvidenceError, "resource_gpu_local_bytes_missing"):
                acceptance.validate_resources(path, "a" * 32, {"max_queue_depth": 10},
                                              {1: {"join": {"requested": {"pid": 1234}}}}, {},
                                              {(1, "join"): "b" * 32})


if __name__ == "__main__":
    unittest.main()
