"""One shared runtime deadline and bounded collector drain; no runtime run."""
from argparse import Namespace
from contextlib import redirect_stdout
from copy import deepcopy
import hashlib
import io
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/product_acceptance"))
from product_pilot_run_budget import RunBudget, load_run_budget, query_qpc, validate_perf_counter_mapping
from product_pilot_checkpoints import collect
from product_pilot_archive import encode_segment
from verify_product_external import archive_session


class SyntheticClock:
    def __init__(self, result, result_at=2, stop_at=None):
        self.seconds, self.result, self.result_at, self.stop_at = 0, result, result_at, stop_at
        self.on_sleep = None

    def expired(self):
        return self.seconds >= 4

    def sleep(self, seconds):
        self.seconds += seconds
        if self.on_sleep:
            self.on_sleep()
        if self.result_at is not None and self.seconds >= self.result_at:
            self.result.write_text("{}")
        if self.stop_at is not None and self.seconds >= self.stop_at:
            (self.result.parent.parent / "collector-stop.json").write_text("{}")


class RunBudgetContracts(unittest.TestCase):
    def marker(self):
        return dict(schema=1, run_id="a" * 32, maximum_seconds=4,
            start_qpc_ticks=1000, frequency_hz=1000, clock_source="QueryPerformanceCounter")

    def test_shared_qpc_deadline_includes_elapsed_time_before_collector_start(self):
        with patch("product_pilot_run_budget.query_qpc", side_effect=[(3000, 1000), (4999, 1000), (5000, 1000)]):
            budget = RunBudget(self.marker(), "a" * 32, 4)
            self.assertFalse(budget.expired())
            self.assertTrue(budget.expired())

    def test_marker_identity_duration_schema_frequency_and_future_start_fail(self):
        for key, value in (("schema", True), ("run_id", "b" * 32), ("maximum_seconds", 5),
                ("maximum_seconds", True), ("start_qpc_ticks", True), ("start_qpc_ticks", 0), ("frequency_hz", True),
                ("clock_source", "UtcNow")):
            marker = self.marker()
            marker[key] = value
            with self.subTest(key=key, value=value), patch("product_pilot_run_budget.query_qpc", return_value=(2000, 1000)):
                with self.assertRaisesRegex(ValueError, "marker_identity_or_contract"):
                    RunBudget(marker, "a" * 32, 4)
        for counter, frequency in ((999, 1000), (2000, 1001)):
            with patch("product_pilot_run_budget.query_qpc", return_value=(counter, frequency)):
                with self.assertRaisesRegex(ValueError, "frequency_or_start_mismatch"):
                    RunBudget(self.marker(), "a" * 32, 4)

    def test_supplied_missing_marker_never_falls_back_to_local_duration(self):
        with TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                load_run_budget(Path(directory) / "missing.json", "a" * 32, 4)

    @unittest.skipUnless(sys.platform == "win32", "requires actual Windows QPC")
    def test_actual_windows_qpc_matches_perf_counter_and_json_marker(self):
        mapping = validate_perf_counter_mapping()
        counter, frequency = query_qpc()
        marker = dict(self.marker(), start_qpc_ticks=counter, frequency_hz=frequency)
        with TemporaryDirectory() as directory:
            path = Path(directory) / "run-clock.json"
            path.write_text(json.dumps(marker), encoding="utf-8")
            budget = load_run_budget(path, "a" * 32, 4)
            self.assertGreater(budget.remaining_seconds(), 0)
            self.assertLessEqual(budget.remaining_seconds(), 4)
        self.assertEqual(mapping["mapping"], "same QPC epoch")
        print(json.dumps(dict(actual_windows_qpc_mapping=mapping)))

    def collector_fixture(self, root, result_at=2, stop_at=None):
        result = root / "uia/uia-result.json"
        result.parent.mkdir()
        args = Namespace(probe=root / "missing-probe.jsonl", result=result,
            output=root / "archive", run_id="a" * 32, seconds=4,
            maximum_bytes=1024, run_budget=root / "run-clock.json")
        return args, SyntheticClock(result, result_at, stop_at)

    def run_collector(self, args, clock):
        with patch("product_pilot_checkpoints.load_run_budget", return_value=clock), \
                patch("product_pilot_checkpoints.time.monotonic", side_effect=lambda: clock.seconds), \
                patch("product_pilot_checkpoints.time.sleep", side_effect=clock.sleep):
            code = collect(args)
        rows = [json.loads(s) for s in (args.output / "collector.jsonl").read_text().splitlines()]
        return code, rows

    def test_result_inside_runtime_budget_drains_three_seconds_without_extending_live_budget(self):
        with TemporaryDirectory() as directory:
            args, clock = self.collector_fixture(Path(directory))
            code, rows = self.run_collector(args, clock)
            self.assertEqual(code, 0)
            self.assertEqual(rows[-1]["status"], "COMPLETE")
            self.assertEqual(clock.seconds, 5)
            self.assertTrue(clock.expired())

    def test_result_only_at_deadline_still_times_out(self):
        with TemporaryDirectory() as directory:
            args, clock = self.collector_fixture(Path(directory), result_at=4)
            code, rows = self.run_collector(args, clock)
            self.assertEqual(code, 1)
            self.assertEqual(clock.seconds, 4)
            self.assertEqual(rows[-1]["reason"], "pilot_result_not_observed")

    def test_stop_marker_during_drain_cannot_be_promoted_to_complete(self):
        with TemporaryDirectory() as directory:
            args, clock = self.collector_fixture(Path(directory), stop_at=3)
            code, rows = self.run_collector(args, clock)
            self.assertEqual(code, 1)
            self.assertEqual(rows[-1]["status"], "INTERRUPTED")

    def test_seen_stop_marker_is_latched_if_marker_disappears_during_drain(self):
        with TemporaryDirectory() as directory:
            args, clock = self.collector_fixture(Path(directory))
            stop = args.result.parent.parent / "collector-stop.json"

            def transient_stop():
                if clock.seconds == 2.5:
                    stop.write_text("{}")
                elif clock.seconds >= 3 and stop.exists():
                    stop.unlink()

            clock.on_sleep = transient_stop
            code, rows = self.run_collector(args, clock)
            self.assertEqual(code, 1)
            self.assertFalse(stop.exists())
            self.assertEqual(rows[-1]["status"], "INTERRUPTED")

    def test_unchanged_manifests_are_not_rewritten_and_all_revision_snapshots_remain(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            args, clock = self.collector_fixture(root)
            session, process = "b" * 32, "c" * 32
            native = root / "native/cohavora-telemetry-v2-current"
            native.mkdir(parents=True)
            entries, compressed_total = [], 0
            for start, end in ((1, 3), (4, 4)):
                name = f"segment-{end:020}.jsonl"
                payload = b"".join((json.dumps(dict(revision=i, session_generation=7,
                    key="queue.depth", value=0)) + "\n").encode() for i in range(start, end + 1))
                (native / name).write_bytes(payload)
                stored, _ = encode_segment(name, payload)
                compressed_total += len(stored)
                entries.append(dict(file=name, first_revision=start, last_revision=end,
                    size_bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest()))
            manifest = dict(process_run_id=process, anonymous_session_id=session,
                last_committed_revision=3, session_generation=7, session_complete=True,
                pruned_records=0, segments=entries[:1])
            native_manifest = native / "manifest.json"
            native_manifest.write_text(json.dumps(manifest))
            args.probe = root / "probe.jsonl"
            args.probe.write_text(json.dumps(dict(run_id=args.run_id, process_run_id=process,
                history_root=str(native.parent))) + "\n")
            args.maximum_bytes = compressed_total

            def revision_change():
                if clock.seconds >= 1 and manifest["last_committed_revision"] == 3:
                    manifest.update(last_committed_revision=4, segments=entries)
                    native_manifest.write_text(json.dumps(manifest))

            clock.on_sleep = revision_change
            writes = []
            original_write = Path.write_bytes

            def write_bytes(path, data):
                if path.parent == args.output / session:
                    writes.append(path.name)
                return original_write(path, data)

            with patch.object(Path, "write_bytes", write_bytes):
                code, rows = self.run_collector(args, clock)
            self.assertEqual(code, 0)
            self.assertEqual(writes.count("manifest.json"), 2)
            for revision in (3, 4):
                self.assertEqual(writes.count(f"manifest-{revision:020}.json"), 1)
            archived = [r for r in rows if r["event"] == "segment.archived"]
            self.assertEqual(archive_session(args.output, session, archived)["last_revision"], 4)
            self.assertEqual(rows[-1]["bytes"], compressed_total)


if __name__ == "__main__":
    unittest.main()
