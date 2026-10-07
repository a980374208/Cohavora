"""Offline smoke checks with a synthetic peer; these never establish L3 acceptance."""

from __future__ import annotations

import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile


TOOLS = Path(__file__).resolve().parents[1] / "tools/meeting"
SPEC = importlib.util.spec_from_file_location("meeting_soak_smoke_target", TOOLS / "meeting_soak.py")
assert SPEC is not None and SPEC.loader is not None
soak = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = soak
SPEC.loader.exec_module(soak)


class OfflineSmokeTests(unittest.TestCase):
    def test_short_synthetic_schedule_keeps_evidence_and_acceptance_boundaries(self) -> None:
        with tempfile.TemporaryDirectory(prefix="soak-offline-smoke-") as directory:
            output = Path(directory) / "run"
            output.mkdir()
            # An adapter may leave arbitrary logs beside its typed status. They
            # must remain on disk without becoming trusted evidence in the ZIP.
            unrelated = output / "adapter-debug.log"
            marker = b"synthetic-private-detail-not-allowed-in-evidence"
            unrelated.write_bytes(marker)
            plan = [
                {"at_s": 0, "action": "grid4", "phase": "steady", "cycle": 0},
                {"at_s": .1, "action": "next_page", "phase": "mixed", "cycle": 1},
                {"at_s": .2, "action": "pin", "phase": "mixed", "cycle": 1},
                {"at_s": .3, "action": "unpin", "phase": "mixed", "cycle": 1},
            ]
            result = soak.run_session(
                output=output,
                command=[sys.executable, str(TOOLS / "soak_fake_peer.py"),
                         "--soak-directory", str(output), "--mode", "normal"],
                plan=plan,
                duration_seconds=.45,
                heartbeat_timeout=.3,
                runtime_timeout=.3,
                command_timeout=.4,
                sample_interval=.02,
                startup_timeout=2,
                shutdown_timeout=.5,
                min_remote_videos=17,
                memory_warmup=0,
                self_test=True,
            )
            self.assertEqual(result["status"], "PASS", result)
            self.assertEqual(result["l3_status"], "NOT_RUN")
            self.assertEqual(result["completed_commands"], len(plan))
            self.assertFalse(result["forced_termination"])
            self.assertEqual(result["exit_code"], 0)
            self.assertEqual(result["memory"]["gate"], "NOT_CONFIGURED")
            for phase in ("steady", "mixed_grid9"):
                self.assertEqual(result["memory"][phase]["status"], "INSUFFICIENT_DATA")
            self.assertIn("gpu_memory_not_measured", result["limitations"])
            self.assertIn("shared_media_delivery_not_gated", result["limitations"])
            metadata = json.loads((output / "run.json").read_text(encoding="utf-8"))
            self.assertTrue(metadata["self_test"])
            self.assertLessEqual(metadata["duration_seconds"], 1)
            events = [json.loads(line) for line in (output / "events.jsonl")
                      .read_text(encoding="utf-8").splitlines()]
            converged = [event["action"] for event in events
                         if event["event"] == "command_converged"]
            self.assertEqual(converged, [step["action"] for step in plan])
            with (output / "metrics.csv").open(encoding="utf-8", newline="") as stream:
                reader = csv.DictReader(stream)
                self.assertTrue({"elapsed_s", "action", "render_submits", "decoded_frames",
                                 "audio_frames"}.issubset(reader.fieldnames or ()))
                rows = list(reader)
            self.assertTrue(rows)
            self.assertTrue(all(row["audio_frames"] == "" for row in rows))
            self.assertGreater(int(rows[-1]["render_submits"]), int(rows[0]["render_submits"]))
            self.assertGreater(int(rows[-1]["decoded_frames"]), int(rows[0]["decoded_frames"]))
            self.assertEqual(unrelated.read_bytes(), marker)
            self.assertFalse((output / "tracks.jsonl").exists())

            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["run_id"], result["run_id"])
            self.assertEqual(manifest["status"], "PASS")
            profile = json.loads((output / "profile.json").read_text(encoding="utf-8"))
            for name, scope in (("summary", result), ("run", metadata),
                                ("profile", profile), ("manifest", manifest)):
                with self.subTest(evidence_scope=name):
                    self.assertIs(scope["diagnostic_only"], True)
                    self.assertIs(scope["release_eligible"], False)
                    self.assertIs(type(scope["qualification_credit"]), int)
                    self.assertEqual(scope["qualification_credit"], 0)
                    if name in ("summary", "manifest") or "l3_status" in scope:
                        self.assertEqual(scope["l3_status"], "NOT_RUN")
            entries = {entry["name"]: entry for entry in manifest["files"]}
            self.assertTrue({"run.json", "plan.json", "profile.json", "events.jsonl",
                             "metrics.csv", "summary.json", "last-status.json"}.issubset(entries))
            archive_path = output.with_suffix(".zip")
            with zipfile.ZipFile(archive_path) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(set(archive.namelist()), set(entries) | {"manifest.json"})
                self.assertEqual(archive.read("manifest.json"), (output / "manifest.json").read_bytes())
                self.assertEqual(json.loads(archive.read("summary.json")), result)
                for name, entry in entries.items():
                    payload = archive.read(name)
                    self.assertEqual(payload, (output / name).read_bytes(), name)
                    self.assertEqual(len(payload), entry["size"], name)
                    self.assertEqual(hashlib.sha256(payload).hexdigest(), entry["sha256"], name)
                    self.assertNotIn(marker, payload, name)
            self.assertEqual(hashlib.sha256(archive_path.read_bytes()).hexdigest(),
                             archive_path.with_suffix(".zip.sha256").read_text(encoding="ascii").strip())

    def test_existing_evidence_is_rejected_without_overwriting_or_launching(self) -> None:
        with tempfile.TemporaryDirectory(prefix="soak-offline-smoke-reuse-") as directory:
            output = Path(directory) / "run"
            output.mkdir()
            existing = {
                output / "run.json": b'{"run_id":"previous-offline-evidence"}\n',
                output / "summary.json": b'{"status":"FAIL"}\n',
                output / "metrics.csv": b"elapsed_s,private_bytes\n0,123\n",
                output.with_suffix(".zip"): b"previous archive must be preserved",
                output.with_suffix(".zip.sha256"): b"previous digest must be preserved\n",
            }
            for path, payload in existing.items():
                path.write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "run_directory_already_used"):
                soak.run_session(
                    output=output,
                    command=[sys.executable, str(TOOLS / "soak_fake_peer.py"),
                             "--soak-directory", str(output)],
                    plan=[{"at_s": 0, "action": "grid4", "phase": "steady", "cycle": 0}],
                    duration_seconds=.1, self_test=True,
                )
            for path, payload in existing.items():
                self.assertEqual(path.read_bytes(), payload, str(path))
            self.assertFalse((output / "fake_peer.pid").exists())


if __name__ == "__main__":
    unittest.main()
