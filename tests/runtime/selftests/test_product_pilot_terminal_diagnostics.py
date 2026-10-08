"""Real terminal diagnostic helper contracts; no product/runtime acceptance."""
from contextlib import redirect_stdout
from copy import deepcopy
import hashlib
import io
import json
import os
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/product_acceptance"))
import product_pilot_diagnostics as diagnostics
from product_pilot_diagnostics import collect, safe_event, watch
from verify_product_external import review_diagnostic_terminal


class TerminalDiagnosticContracts(unittest.TestCase):
    def delete_file(self, path):
        if os.name != "nt":
            path.unlink()
            return
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        delete = kernel.DeleteFileW
        delete.argtypes = [wintypes.LPCWSTR]
        delete.restype = wintypes.BOOL
        if not delete(str(path)):
            raise ctypes.WinError(ctypes.get_last_error())

    def fixture(self, root, outcome="success"):
        run, process, pid = "a" * 32, "b" * 32, 3245
        source = root / "raw" / ("run-" + process)
        source.mkdir(parents=True)
        (root / "uia").mkdir()
        (root / "process-probe.jsonl").write_text(json.dumps(dict(run_id=run,
            process_run_id=process, diagnostic_root=str(source.parent))) + "\n", encoding="utf-8")
        continuous = []
        for index in range(3):
            lines = []
            for sequence in (2 * index + 1, 2 * index + 2):
                event = dict(schema_version=1, process_run_id=process, pid=pid,
                    event_sequence=sequence, occurred_at_utc_ms=1000 + sequence,
                    monotonic_us=100000 + sequence, severity="info",
                    event_name="process.terminal" if sequence == 6 else "session.changed")
                if sequence == 6:
                    event["attributes"] = dict(outcome=outcome, drain_result="completed")
                line = (json.dumps(event) + "\n").encode()
                lines.append(line)
                continuous.append(dict(safe_event(event, process), run_id=run,
                    source_segment=f"segment-{index:06}.jsonl", raw_sha256=hashlib.sha256(line).hexdigest()))
            (source / f"segment-{index:06}.jsonl").write_bytes(b"".join(lines))
        identity = dict(run_id=run, pid=pid)
        watcher = dict(run_id=run, process_run_id=process, pid=pid, status="COMPLETE", events=6)
        return run, source, identity, continuous, watcher

    def witness(self, root):
        with redirect_stdout(io.StringIO()):
            collect(root)
        return json.loads((root / "diagnostic-sequences.json").read_text())

    def test_complete_native_retention_and_actual_terminal_helper_pass(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, _, identity, continuous, watcher = self.fixture(root)
            proof = review_diagnostic_terminal(root, identity, run, continuous, self.witness(root), watcher)
            self.assertTrue(proof["passed"])
            self.assertTrue(proof["complete_raw_native_retention"])
            self.assertEqual(proof["terminal_sequence"], 6)

    def test_legitimate_pruned_native_prefix_keeps_full_continuous_witness(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            (source / "segment-000000.jsonl").unlink()
            proof = review_diagnostic_terminal(root, identity, run, continuous, self.witness(root), watcher)
            self.assertTrue(proof["passed"])
            self.assertFalse(proof["complete_raw_native_retention"])
            self.assertEqual(proof["pruned_native_prefix_events"], 2)
            self.assertEqual(proof["native_retained_events"], 4)
            self.assertEqual([r["event_sequence"] for r in continuous], list(range(1, 7)))

    def test_unobserved_or_missing_continuous_prefix_still_fails(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            (source / "segment-000000.jsonl").unlink()
            witness = self.witness(root)
            watcher["events"] = 4
            with self.assertRaisesRegex(ValueError, "continuous_identity_sequence"):
                review_diagnostic_terminal(root, identity, run, continuous[2:], witness, watcher)

    def test_missing_retained_middle_segment_fails(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            (source / "segment-000001.jsonl").unlink()
            with self.assertRaisesRegex(ValueError, "terminal_segment_gap"):
                review_diagnostic_terminal(root, identity, run, continuous, self.witness(root), watcher)

    def test_missing_terminal_tail_cannot_pass_as_complete_suffix(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            (source / "segment-000002.jsonl").unlink()
            with self.assertRaisesRegex(ValueError, "terminal_tail_or_successful"):
                review_diagnostic_terminal(root, identity, run, continuous, self.witness(root), watcher)

    def test_native_tampering_after_watch_is_rejected_even_with_new_segment_hash(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            path = source / "segment-000001.jsonl"
            path.write_bytes(path.read_bytes().replace(b'"severity": "info"', b'"severity": "warning"'))
            with self.assertRaisesRegex(ValueError, "native_line_not_exactly_observed"):
                review_diagnostic_terminal(root, identity, run, continuous, self.witness(root), watcher)

    def test_claimed_line_hash_or_native_file_tampering_after_collect_fails(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            witness = self.witness(root)
            bad = deepcopy(witness)
            bad["segments"][0]["records"][0]["raw_sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "retained_suffix_sequence_or_identity"):
                review_diagnostic_terminal(root, identity, run, continuous, bad, watcher)
            path = source / "segment-000002.jsonl"
            path.write_bytes(path.read_bytes() + b"\n")
            with self.assertRaisesRegex(ValueError, "segment_hash_size_or_complete"):
                review_diagnostic_terminal(root, identity, run, continuous, witness, watcher)

    def test_wrong_run_pid_watcher_completion_and_failed_terminal_fail(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, _, identity, continuous, watcher = self.fixture(root)
            witness = self.witness(root)
            for changed in (dict(witness, run_id="c" * 32), dict(witness, pid=42), dict(witness, schema=1)):
                with self.assertRaisesRegex(ValueError, "witness_identity_or_completion"):
                    review_diagnostic_terminal(root, identity, run, continuous, changed, watcher)
            with self.assertRaisesRegex(ValueError, "witness_identity_or_completion"):
                review_diagnostic_terminal(root, identity, run, continuous, witness, dict(watcher, status="INTERRUPTED"))
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, _, identity, continuous, watcher = self.fixture(root, "failure")
            with self.assertRaisesRegex(ValueError, "terminal_tail_or_successful"):
                review_diagnostic_terminal(root, identity, run, continuous, self.witness(root), watcher)

    def test_collect_rejects_foreign_native_identity_and_boolean_schema(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            path = source / "segment-000001.jsonl"
            path.write_bytes(path.read_bytes().replace(("b" * 32).encode(), ("c" * 32).encode()))
            with self.assertRaisesRegex(ValueError, "diagnostic_identity"):
                self.witness(root)
        event = dict(schema_version=True, process_run_id="b" * 32)
        with self.assertRaisesRegex(ValueError, "diagnostic_identity"):
            safe_event(event, "b" * 32)

    def test_watch_rescans_native_terminal_after_result_publication_race(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            last = source / "segment-000002.jsonl"
            terminal = last.read_bytes().splitlines(keepends=True)[-1]
            last.write_bytes(last.read_bytes().splitlines(keepends=True)[0])
            result = root / "uia/uia-result.json"
            original_exists = Path.exists

            def exists(path):
                if path == result and not original_exists(path):
                    with last.open("ab") as stream:
                        stream.write(terminal)
                    result.write_text("{}")
                return original_exists(path)

            with patch.object(Path, "exists", exists):
                watch(root, 30)
            observed = [json.loads(s) for s in (root / "diagnostic-events.jsonl").read_text().splitlines()]
            actual_watcher = json.loads((root / "diagnostic-watcher-result.json").read_text())
            self.assertEqual(len(observed), 6)
            self.assertEqual(actual_watcher["events"], 6)
            self.assertTrue(review_diagnostic_terminal(root, identity, run, observed, self.witness(root), actual_watcher)["passed"])

    def test_watch_tolerates_pruned_observed_prefix_between_glob_and_open(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            (root / "uia/uia-result.json").write_text("{}")
            original_open = diagnostics.open_segment_reader
            prefix, openings = source / "segment-000000.jsonl", 0

            def open_path(path):
                nonlocal openings
                if path == prefix:
                    openings += 1
                    if openings == 2:
                        self.delete_file(prefix)
                return original_open(path)

            with patch.object(diagnostics, "open_segment_reader", open_path):
                watch(root, 30)
            observed = [json.loads(s) for s in (root / "diagnostic-events.jsonl").read_text().splitlines()]
            actual_watcher = json.loads((root / "diagnostic-watcher-result.json").read_text())
            proof = review_diagnostic_terminal(root, identity, run, observed, self.witness(root), actual_watcher)
            self.assertEqual(proof["pruned_native_prefix_events"], 2)
            self.assertEqual(proof["continuous_events"], 6)

    def watch_delete_during_read(self, delete_on_open):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            run, source, identity, continuous, watcher = self.fixture(root)
            (root / "uia/uia-result.json").write_text("{}")
            original_open = diagnostics.open_segment_reader
            prefix, openings, deletions = source / "segment-000000.jsonl", 0, 0

            def open_path(path):
                nonlocal openings, deletions
                stream = original_open(path)
                if path == prefix:
                    openings += 1
                    if openings == delete_on_open:
                        try:
                            self.delete_file(prefix)
                            deletions += 1
                        except BaseException:
                            stream.close()
                            raise
                return stream

            with patch.object(diagnostics, "open_segment_reader", open_path):
                watch(root, 30)
            self.assertEqual(deletions, 1)
            self.assertFalse(prefix.exists())
            observed = [json.loads(s) for s in (root / "diagnostic-events.jsonl").read_text().splitlines()]
            actual_watcher = json.loads((root / "diagnostic-watcher-result.json").read_text())
            self.assertEqual(observed, continuous)
            proof = review_diagnostic_terminal(root, identity, run, observed, self.witness(root), actual_watcher)
            self.assertTrue(proof["passed"])
            self.assertEqual(proof["pruned_native_prefix_events"], 2)

    @unittest.skipUnless(os.name == "nt", "Requires real Windows DeleteFileW semantics")
    def test_windows_delete_while_first_reader_is_open_keeps_unread_bytes(self):
        self.watch_delete_during_read(1)

    @unittest.skipUnless(os.name == "nt", "Requires real Windows DeleteFileW semantics")
    def test_windows_delete_while_observed_reader_is_open_does_not_block_pruning(self):
        self.watch_delete_during_read(2)

    def test_unobserved_segment_pruned_before_open_still_fails(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            _, source, _, _, _ = self.fixture(root)
            (root / "uia/uia-result.json").write_text("{}")
            original_open = diagnostics.open_segment_reader
            prefix = source / "segment-000000.jsonl"

            def open_path(path):
                if path == prefix:
                    self.delete_file(prefix)
                return original_open(path)

            with patch.object(diagnostics, "open_segment_reader", open_path):
                with self.assertRaises(FileNotFoundError):
                    watch(root, 30)
            self.assertFalse((root / "diagnostic-watcher-result.json").exists())

    @unittest.skipUnless(os.name == "nt", "Requires real Windows handle ownership")
    def test_windows_open_osfhandle_failure_closes_original_handle(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        get_information = kernel.GetHandleInformation
        get_information.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        get_information.restype = wintypes.BOOL
        captured = []

        def fail_transfer(handle, flags):
            captured.append(handle)
            raise OSError("injected CRT ownership transfer failure")

        with TemporaryDirectory() as directory:
            path = Path(directory) / "segment.jsonl"
            path.write_bytes(b"{}\n")
            with patch("msvcrt.open_osfhandle", fail_transfer):
                with self.assertRaisesRegex(OSError, "injected CRT"):
                    diagnostics.open_segment_reader(path)
            self.assertEqual(len(captured), 1)
            flags = wintypes.DWORD()
            ctypes.set_last_error(0)
            self.assertFalse(get_information(captured[0], ctypes.byref(flags)))
            self.assertEqual(ctypes.get_last_error(), 6)

    @unittest.skipUnless(os.name == "nt", "Requires real Windows CRT ownership")
    def test_windows_fdopen_failure_closes_transferred_descriptor(self):
        captured = []

        def fail_fdopen(descriptor, mode):
            captured.append(descriptor)
            raise OSError("injected stream construction failure")

        with TemporaryDirectory() as directory:
            path = Path(directory) / "segment.jsonl"
            path.write_bytes(b"{}\n")
            with patch.object(diagnostics.os, "fdopen", fail_fdopen):
                with self.assertRaisesRegex(OSError, "injected stream"):
                    diagnostics.open_segment_reader(path)
            self.assertEqual(len(captured), 1)
            with self.assertRaises(OSError):
                os.fstat(captured[0])

    def test_watch_terminal_rescan_is_bounded_even_if_probe_becomes_incomplete(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            (root / "uia/uia-result.json").write_text("{}")
            original_open = Path.open
            probe, openings, now = root / "process-probe.jsonl", 0, [0.0]

            def open_path(path, mode="r", *args, **kwargs):
                nonlocal openings
                if path == probe:
                    openings += 1
                    if openings > 1:
                        return io.StringIO("{")
                return original_open(path, mode, *args, **kwargs)

            def sleep(seconds):
                now[0] += seconds

            with patch.object(Path, "open", open_path), \
                    patch("product_pilot_diagnostics.time.monotonic", side_effect=lambda: now[0]), \
                    patch("product_pilot_diagnostics.time.sleep", side_effect=sleep):
                with self.assertRaisesRegex(TimeoutError, "diagnostic_terminal_drain_timeout"):
                    watch(root, 4)
            self.assertGreaterEqual(now[0], 3)
            self.assertLess(now[0], 3.3)

    def test_watch_seen_stop_marker_stays_interrupted_after_marker_disappears(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            (root / "uia/uia-result.json").write_text("{}")
            stop = root / "collector-stop.json"
            stop.write_text("{}")
            original_exists, checks = Path.exists, 0

            def exists(path):
                nonlocal checks
                if path == stop:
                    checks += 1
                    if checks == 2:
                        stop.unlink()
                return original_exists(path)

            with patch.object(Path, "exists", exists):
                watch(root, 30)
            self.assertEqual(json.loads((root / "diagnostic-watcher-result.json").read_text())["status"], "INTERRUPTED")


if __name__ == "__main__":
    unittest.main()
