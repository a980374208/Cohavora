"""Lost-reply recovery must retain observer identity and the original deadline."""
import base64
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/product_acceptance"))
from product_pilot_context import REMOTE_ACK_RECOVERY, fence, validate_ack
from product_aliyun_transport import AliyunRemoteCommandError, execute


def context():
    return dict(run_id="a" * 32, cycle=3, cycle_id="b" * 32,
                operation_id="c" * 32, action="leave", phase="uia_observed", pid=123,
                process_run_id="d" * 32, anonymous_session_id="e" * 32,
                participant_sha256="f" * 64)


class ContextRecoveryContracts(unittest.TestCase):
    def invoke_fence(self, responses):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "current-operation.json"
            value = context()
            path.write_text(json.dumps(value))
            with patch("product_aliyun_transport.target", return_value={"remote_root": "/fixture"}), \
                    patch("product_aliyun_transport.execute", side_effect=responses) as execute, \
                    patch("product_pilot_context.time.sleep"):
                fence(path)
            return execute.call_args_list, json.loads((path.parent / "context-acks.jsonl").read_text())

    def test_normal_reply_does_not_recover(self):
        calls, ack = self.invoke_fence([json.dumps(dict(context=context(), sequence=9))])
        self.assertEqual(len(calls), 1)
        self.assertFalse(ack["transport_recovered"])

    def test_lost_reply_recovers_exact_receipt_without_republishing(self):
        receipt = dict(context=context(), sequence=9,
                       recovery_proof=dict(context_written_ns=100, ack_written_ns=200))
        calls, ack = self.invoke_fence([RuntimeError("transport failed"), json.dumps(receipt)])
        self.assertEqual(len(calls), 2)
        self.assertNotIn("write_text", calls[1].args[0])
        self.assertNotIn("replace(", calls[1].args[0])
        self.assertTrue(ack["transport_recovered"])
        self.assertEqual(ack["context"], context())

    def test_conflicting_reply_is_rejected_without_retry(self):
        conflicting = dict(context(), operation_id="0" * 32)
        with TemporaryDirectory() as directory:
            path = Path(directory) / "current-operation.json"
            path.write_text(json.dumps(context()))
            with patch("product_aliyun_transport.target", return_value={"remote_root": "/fixture"}), \
                    patch("product_aliyun_transport.execute", return_value=json.dumps(
                        dict(context=conflicting, sequence=9))) as execute:
                with self.assertRaisesRegex(ValueError, "ack_mismatch"):
                    fence(path)
                self.assertEqual(execute.call_count, 1)
            self.assertFalse((path.parent / "context-acks.jsonl").exists())

    def test_transient_receipt_read_failures_retry_without_republishing(self):
        receipt = dict(context=context(), sequence=9,
                       recovery_proof=dict(context_written_ns=100, ack_written_ns=200))
        calls, ack = self.invoke_fence([RuntimeError("primary"),
            subprocess.TimeoutExpired("sensitive command", 15), "invalid JSON", json.dumps(receipt)])
        self.assertEqual(len(calls), 4)
        for call in calls[1:]:
            self.assertNotIn("write_text", call.args[0])
            self.assertNotIn("replace(", call.args[0])
            self.assertLessEqual(call.kwargs["timeout"], 15)
        self.assertTrue(ack["transport_recovered"])

    def test_cli_timeout_with_successful_remote_exit_retries_readonly_receipt(self):
        reply_lost = AliyunRemoteCommandError(
            subprocess.CompletedProcess([], 0, "\n", ""), False, None)
        cli_timed_out_after_remote_success = AliyunRemoteCommandError(
            subprocess.CompletedProcess([], 124, "private marker", "private timeout"), True, 0)
        receipt = dict(context=context(), sequence=9,
                       recovery_proof=dict(context_written_ns=100, ack_written_ns=200))
        calls, ack = self.invoke_fence([reply_lost, cli_timed_out_after_remote_success,
                                       json.dumps(receipt)])
        self.assertEqual(len(calls), 3)
        for call in calls[1:]:
            self.assertNotIn("write_text", call.args[0])
            self.assertNotIn("replace(", call.args[0])
            self.assertLessEqual(call.kwargs["timeout"], 15)
        self.assertTrue(ack["transport_recovered"])
        self.assertEqual(ack["context"], context())

    def test_retrieval_failure_is_bounded_and_records_only_safe_fields(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "current-operation.json"
            path.write_text(json.dumps(context()))
            with patch("product_aliyun_transport.target", return_value={"remote_root": "/fixture"}), \
                    patch("product_aliyun_transport.execute", side_effect=RuntimeError("secret output")) as run, \
                    patch("product_pilot_context.time.sleep"):
                with self.assertRaises(RuntimeError):
                    fence(path)
            self.assertEqual(run.call_count, 4)
            self.assertFalse((path.parent / "context-acks.jsonl").exists())
            evidence = (path.parent / "context-fence-attempts.jsonl").read_text()
            self.assertNotIn("secret output", evidence)
            self.assertNotIn("/fixture", evidence)
            self.assertEqual(json.loads(evidence)["verdict"], "FAILED")

    def test_recovery_rejection_and_session_limit_are_not_repeated(self):
        remote_rejected = AliyunRemoteCommandError(
            subprocess.CompletedProcess([], 0, "private data", "secret"), True, 1)
        limited = AliyunRemoteCommandError(
            subprocess.CompletedProcess([], 1, "", "Forbidden.SessionLimit"), False, None)
        for responses in ([RuntimeError("primary"), remote_rejected], [limited, limited]):
            with self.subTest(responses=responses), TemporaryDirectory() as directory:
                path = Path(directory) / "current-operation.json"
                path.write_text(json.dumps(context()))
                with patch("product_aliyun_transport.target", return_value={"remote_root": "/fixture"}), \
                        patch("product_aliyun_transport.execute", side_effect=responses) as run:
                    with self.assertRaises(AliyunRemoteCommandError):
                        fence(path)
                self.assertEqual(run.call_count, 2)
                self.assertFalse((path.parent / "context-acks.jsonl").exists())

    def test_recovery_budget_exhaustion_stops_before_another_call(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "current-operation.json"
            path.write_text(json.dumps(context()))
            with patch("product_aliyun_transport.target", return_value={"remote_root": "/fixture"}), \
                    patch("product_aliyun_transport.execute", side_effect=RuntimeError("primary")) as run, \
                    patch("product_pilot_context.time.monotonic", side_effect=[0, 1, 2, 48]):
                with self.assertRaisesRegex(RuntimeError, "budget_exhausted"):
                    fence(path)
            self.assertEqual(run.call_count, 1)

    def test_invalid_recovered_identity_is_terminal_without_more_reads(self):
        receipt = dict(context=dict(context(), operation_id="0" * 32), sequence=9,
                       recovery_proof=dict(context_written_ns=100, ack_written_ns=200))
        with TemporaryDirectory() as directory:
            path = Path(directory) / "current-operation.json"
            path.write_text(json.dumps(context()))
            with patch("product_aliyun_transport.target", return_value={"remote_root": "/fixture"}), \
                    patch("product_aliyun_transport.execute", side_effect=[RuntimeError("primary"), json.dumps(receipt)]) as run:
                with self.assertRaisesRegex(ValueError, "ack_mismatch"):
                    fence(path)
            self.assertEqual(run.call_count, 2)
            self.assertFalse((path.parent / "context-acks.jsonl").exists())

    def test_recovery_rejects_missing_late_or_invalid_clock_proof(self):
        proofs = ({}, dict(context_written_ns=100, ack_written_ns=10_000_000_101),
                  dict(context_written_ns=200, ack_written_ns=100),
                  dict(context_written_ns=True, ack_written_ns=200))
        for proof in proofs:
            with self.subTest(proof=proof), self.assertRaisesRegex(ValueError, "deadline_unproven"):
                validate_ack(dict(context=context(), sequence=9, recovery_proof=proof), context(), True)

    def test_recovery_rejects_wrong_lifecycle_or_boolean_sequence(self):
        for changed in (dict(context(), anonymous_session_id="0" * 32),
                        dict(context(), cycle_id="0" * 32)):
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, "ack_mismatch"):
                validate_ack(dict(context=changed, sequence=9), context(), True)
        with self.assertRaisesRegex(ValueError, "ack_mismatch"):
            validate_ack(dict(context=context(), sequence=True), context())

    def run_remote_recovery(self, changed_context=False, late=False):
        with TemporaryDirectory() as directory:
            root = Path(directory) / ("pilot-" + context()["run_id"][:8])
            root.mkdir()
            (root / "ready.json").write_text(json.dumps(dict(run_id=context()["run_id"])))
            actual = dict(context(), operation_id="0" * 32) if changed_context else context()
            (root / "context.json").write_text(json.dumps(actual))
            (root / "context-ack.json").write_text(json.dumps(dict(context=context(), sequence=9)))
            issued = 1_700_000_000_000_000_000
            os.utime(root / "context.json", ns=(issued, issued))
            accepted = issued + (10_001_000_000 if late else 1_000_000_000)
            os.utime(root / "context-ack.json", ns=(accepted, accepted))
            before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.iterdir()}
            payload = base64.b64encode(json.dumps(context()).encode()).decode()
            result = subprocess.run([sys.executable, "-c", REMOTE_ACK_RECOVERY, directory, payload],
                                    capture_output=True, text=True, check=False)
            after = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.iterdir()}
            self.assertEqual(before, after)
            return result

    def test_remote_recovery_reads_exact_ack_without_changing_files(self):
        result = self.run_remote_recovery()
        self.assertEqual(result.returncode, 0, result.stderr)
        validate_ack(json.loads(result.stdout), context(), True)

    def test_remote_recovery_rejects_replaced_context(self):
        result = self.run_remote_recovery(changed_context=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(result.stdout)

    def test_remote_recovery_rejects_ack_outside_original_window(self):
        result = self.run_remote_recovery(late=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(result.stdout)


class TransportFailureEvidence(unittest.TestCase):
    def test_failure_classification_preserves_exit_marker_without_raw_output(self):
        cases = ((1, False, None), (0, False, None), (0, True, 3), (1, True, 0))
        for cli_code, marker_seen, remote_code in cases:
            with self.subTest(case=(cli_code, marker_seen, remote_code)):
                def reply(argv, **kwargs):
                    marker = re.search(r"PRODUCT_EXIT_[a-f0-9]+:", argv[-1]).group()
                    output = "private-value"
                    if marker_seen:
                        output += "\n" + marker + str(remote_code) + "\n"
                    response = dict(instance_id="i-test", command=argv[-1], exit_code=0,
                                    stdout=output, stderr="secret-value")
                    return subprocess.CompletedProcess(argv, cli_code, json.dumps(response), "secret-value")
                with patch("product_aliyun_transport.target", return_value=dict(instance_id="i-test", region="test")), \
                        patch("product_aliyun_transport.subprocess.run", side_effect=reply):
                    with self.assertRaises(AliyunRemoteCommandError) as caught:
                        execute("sensitive-command")
                detail = caught.exception.diagnostics
                self.assertEqual(detail["cli_returncode"], cli_code)
                self.assertEqual(detail["exit_marker_seen"], marker_seen)
                self.assertEqual(detail["remote_exit_code"], remote_code)
                self.assertNotIn("private-value", json.dumps(detail))
                self.assertNotIn("secret-value", json.dumps(detail))
                self.assertNotIn("sensitive-command", json.dumps(detail))

    def test_success_reply_is_unchanged(self):
        def reply(argv, **kwargs):
            self.assertEqual(kwargs["encoding"], "utf-8")
            self.assertEqual(argv[argv.index("--output") + 1], "json")
            marker = re.search(r"PRODUCT_EXIT_[a-f0-9]+:", argv[-1]).group()
            response = dict(instance_id="i-test", command=argv[-1], exit_code=0,
                            stdout='payload\n' + marker + '0\n', stderr='')
            return subprocess.CompletedProcess(argv, 0, json.dumps(response), '')
        with patch("product_aliyun_transport.target", return_value=dict(instance_id="i-test", region="test")), \
                patch("product_aliyun_transport.subprocess.run", side_effect=reply):
            self.assertEqual(execute("read-only"), "payload")

    def test_command_echo_cannot_supply_missing_remote_exit_marker(self):
        def reply(argv, **kwargs):
            # The echoed command contains the expected marker; stdout does not.
            response = dict(instance_id="i-test", command=argv[-1], exit_code=0,
                            stdout="private-payload", output=argv[-1], stderr="")
            return subprocess.CompletedProcess(argv, 0, json.dumps(response), "")
        with patch("product_aliyun_transport.target", return_value=dict(instance_id="i-test", region="test")), \
                patch("product_aliyun_transport.subprocess.run", side_effect=reply):
            with self.assertRaises(AliyunRemoteCommandError) as caught:
                execute("sensitive-command")
        self.assertFalse(caught.exception.diagnostics["exit_marker_seen"])
        self.assertNotIn("private-payload", json.dumps(caught.exception.diagnostics))

    def test_structured_identity_schema_and_exit_rejections_are_safe(self):
        cases = ({"instance_id": "i-other"}, {"command": "secret-other-command"},
                 {"exit_code": 1}, {"exit_code": True}, {"stdout": None})
        for changed in cases:
            with self.subTest(changed=changed):
                def reply(argv, **kwargs):
                    marker = re.search(r"PRODUCT_EXIT_[a-f0-9]+:", argv[-1]).group()
                    response = dict(instance_id="i-test", command=argv[-1], exit_code=0,
                                    stdout="private-value\n" + marker + "0\n")
                    response.update(changed)
                    return subprocess.CompletedProcess(argv, 0, json.dumps(response), "secret-value")
                with patch("product_aliyun_transport.target", return_value=dict(instance_id="i-test", region="test")), \
                        patch("product_aliyun_transport.subprocess.run", side_effect=reply):
                    with self.assertRaises(AliyunRemoteCommandError) as caught:
                        execute("sensitive-command")
                detail = json.dumps(caught.exception.diagnostics)
                for secret in ("private-value", "secret-value", "secret-other-command", "sensitive-command"):
                    self.assertNotIn(secret, detail)

    def test_unstructured_reply_is_rejected_even_with_an_exit_marker(self):
        def reply(argv, **kwargs):
            marker = re.search(r"PRODUCT_EXIT_[a-f0-9]+:", argv[-1]).group()
            return subprocess.CompletedProcess(argv, 0, "private-value\n" + marker + "0\n", "")
        with patch("product_aliyun_transport.target", return_value=dict(instance_id="i-test", region="test")), \
                patch("product_aliyun_transport.subprocess.run", side_effect=reply):
            with self.assertRaises(AliyunRemoteCommandError) as caught:
                execute("read-only")
        self.assertEqual(caught.exception.diagnostics["response_status"], "MALFORMED_JSON")

    def test_exit_marker_requires_the_original_exact_zero_status(self):
        for status in ("00", "0extra", "0\nprivate-tail"):
            with self.subTest(status=status):
                def reply(argv, **kwargs):
                    marker = re.search(r"PRODUCT_EXIT_[a-f0-9]+:", argv[-1]).group()
                    response = dict(instance_id="i-test", command=argv[-1], exit_code=0,
                                    stdout="payload\n" + marker + status + "\n")
                    return subprocess.CompletedProcess(argv, 0, json.dumps(response), "")
                with patch("product_aliyun_transport.target", return_value=dict(instance_id="i-test", region="test")), \
                        patch("product_aliyun_transport.subprocess.run", side_effect=reply):
                    with self.assertRaises(AliyunRemoteCommandError):
                        execute("read-only")


if __name__ == "__main__":
    unittest.main()
