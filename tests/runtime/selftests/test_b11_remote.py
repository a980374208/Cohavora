"""Offline SSH binding/transport contracts; mocks never connect to an instance."""
from __future__ import annotations

import base64
from contextlib import redirect_stdout
from copy import deepcopy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1] / "tools/meeting"
SPEC = importlib.util.spec_from_file_location("b11_remote", HERE / "b11_remote.py")
assert SPEC and SPEC.loader
remote = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = remote
SPEC.loader.exec_module(remote)


class RemoteTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.key = self.root / "identity.pem"
        self.key.write_bytes(b"private-key-sentinel-never-print")
        self.hosts = self.root / "known_hosts"
        self.hosts.write_text("192.0.2.10 ssh-ed25519 fixture\n", encoding="ascii")
        self.config = {"schema": 1, "transport": "ssh", "host": "192.0.2.10", "port": 22,
                       "user": "root", "key_path": str(self.key), "known_hosts_path": str(self.hosts)}
        self.config_path = self.root / "target.json"
        self.config_path.write_text(json.dumps(self.config, indent=2), encoding="utf-8")
        self.addCleanup(patch.stopall)
        patch.object(remote, "ROOT", self.root).start()

    @staticmethod
    def success(stdout=b'{"accepted":true}'):
        return subprocess.CompletedProcess([], 0, stdout, b"ignored-diagnostics")

    def test_closed_schema_rejects_secrets_missing_fields_duplicate_and_nonfinite(self):
        for value in ({**self.config, "secret": "sentinel"},
                      {key: item for key, item in self.config.items() if key != "port"}):
            with self.subTest(value=value), self.assertRaisesRegex(remote.RemoteError, "invalid_ssh_target_config"):
                remote.validate_target_config(value)
        self.config_path.write_text('{"schema":1,"schema":1}', encoding="utf-8")
        with self.assertRaisesRegex(remote.RemoteError, "duplicate_target_config_key"):
            remote.load_target_config(self.config_path)
        self.config_path.write_text('{"schema":NaN}', encoding="utf-8")
        with self.assertRaisesRegex(remote.RemoteError, "non_finite_json"):
            remote.load_target_config(self.config_path)

    def test_config_rejects_typed_port_user_host_and_controls_without_subprocess(self):
        for field, value in (("schema", True), ("port", True), ("port", "22"), ("port", 0),
                             ("port", 65536), ("user", "root;id"), ("user", "-root"),
                             ("host", "192.0.2.10\n"), ("host", "example.test"),
                             ("host", "0.0.0.0"), ("key_path", str(self.key) + "\0")):
            with self.subTest(field=field, value=value), patch.object(remote.subprocess, "run") as run:
                with self.assertRaises(remote.RemoteError):
                    remote.validate_target_config({**self.config, field: value})
                run.assert_not_called()

    def test_key_must_exist_and_known_hosts_must_be_workspace_file(self):
        self.key.unlink()
        with self.assertRaisesRegex(remote.RemoteError, "ssh_key_file_missing_or_invalid"):
            remote.validate_target_config(self.config)
        self.key.write_bytes(b"private-key-sentinel-never-print")
        with patch.object(remote, "ROOT", self.root / "other-workspace"):
            with self.assertRaisesRegex(remote.RemoteError, "ssh_known_hosts_outside_workspace"):
                remote.validate_target_config(self.config)

    def test_snapshot_freezes_public_fingerprint_and_exact_config_without_key_read(self):
        blob = b"offline-public-key-blob"
        response = self.success(b"ssh-ed25519 " + base64.b64encode(blob) + b" comment\n")
        original_read = Path.read_bytes

        def guarded_read(path):
            if path == self.key:
                self.fail("private key read by Python")
            return original_read(path)

        with patch.object(remote.subprocess, "run", return_value=response) as run, \
                patch.object(Path, "read_bytes", guarded_read):
            identity = remote.snapshot_identity(self.config_path)
        self.assertEqual(identity["config_sha256"], hashlib.sha256(self.config_path.read_bytes()).hexdigest())
        self.assertEqual(identity["known_hosts_sha256"], hashlib.sha256(self.hosts.read_bytes()).hexdigest())
        expected = "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")
        self.assertEqual(identity["key_public_fingerprint"], expected)
        self.assertNotIn("offline-public-key-blob", json.dumps(identity))
        self.assertNotIn("private-key-sentinel", json.dumps(identity))
        self.assertEqual(run.call_args.args[0], ["ssh-keygen", "-y", "-f", str(self.key)])
        self.assertEqual(run.call_args.kwargs["input"], b"")

    def test_snapshot_dict_hash_is_canonical_and_known_hosts_drift_changes_identity(self):
        response = self.success(b"ssh-ed25519 " + base64.b64encode(b"public") + b"\n")
        with patch.object(remote.subprocess, "run", return_value=response):
            first = remote.snapshot_identity(self.config)
            second = remote.snapshot_identity(dict(reversed(list(self.config.items()))))
            self.assertEqual(first, second)
            self.hosts.write_text("changed\n", encoding="ascii")
            third = remote.snapshot_identity(self.config)
        self.assertNotEqual(first["known_hosts_sha256"], third["known_hosts_sha256"])

    def frozen_identity(self):
        fingerprint = "SHA256:" + base64.b64encode(hashlib.sha256(b"public").digest()).decode().rstrip("=")
        return {key: self.config[key] for key in remote.FIELDS - {"schema"}} | {
            "config_sha256": hashlib.sha256(self.config_path.read_bytes()).hexdigest(),
            "known_hosts_sha256": hashlib.sha256(self.hosts.read_bytes()).hexdigest(),
            "key_public_fingerprint": fingerprint}

    @staticmethod
    def expected_arguments(identity):
        arguments = []
        for field in ("config_sha256", "known_hosts_sha256", "key_public_fingerprint"):
            arguments.extend(["--expected-" + field.replace("_", "-"), identity[field]])
        return arguments

    def cli_arguments(self, operation):
        arguments = [operation, "--target-config", str(self.config_path)]
        if operation == "exec":
            command = self.root / "command.sh"
            command.write_text("owned\n", encoding="ascii")
            return arguments + ["--command-file", str(command)]
        local = self.root / ("sampler.py" if operation == "upload" else "metrics.csv")
        if operation == "upload":
            local.write_text("owned\n", encoding="ascii")
        return arguments + ["--local-path", str(local), "--remote-path", "/tmp/b11-test/owned-file"]

    def test_snapshot_uses_one_config_read_for_parsed_target_and_hash(self):
        original = self.config_path.read_bytes()
        original_read = Path.read_bytes
        config_reads = []

        def racing_read(path):
            if path == self.key:
                self.fail("private key read by Python")
            data = original_read(path)
            if path == self.config_path:
                config_reads.append(data)
                path.write_text(json.dumps({**self.config, "host": "192.0.2.11"}), encoding="utf-8")
            return data

        response = self.success(b"ssh-ed25519 " + base64.b64encode(b"public") + b"\n")
        with patch.object(Path, "read_bytes", racing_read), \
                patch.object(remote.subprocess, "run", return_value=response):
            identity = remote.snapshot_identity(self.config_path)
        self.assertEqual(config_reads, [original])
        self.assertEqual(identity["host"], "192.0.2.10")
        self.assertEqual(identity["config_sha256"], hashlib.sha256(original).hexdigest())

    def test_cli_expected_identity_requires_complete_valid_arguments_before_snapshot(self):
        identity = self.frozen_identity()
        for arguments, reason in ((["--expected-config-sha256", identity["config_sha256"]],
                                   "incomplete_expected_ssh_identity"),
                                  (self.expected_arguments({**identity, "config_sha256": "bad"}),
                                   "invalid_expected_ssh_identity"),
                                  (self.expected_arguments({**identity, "key_public_fingerprint": "bad"}),
                                   "invalid_expected_ssh_identity")):
            with self.subTest(reason=reason), patch.object(remote, "snapshot_identity") as snapshot, \
                    patch.object(remote.subprocess, "run") as run, redirect_stdout(io.StringIO()) as output:
                code = remote.main(self.cli_arguments("exec") + arguments)
                self.assertEqual(code, 1)
                self.assertEqual(json.loads(output.getvalue())["reason"], reason)
                snapshot.assert_not_called()
                run.assert_not_called()

    def test_cli_frozen_identity_mismatch_rejects_every_operation_before_network(self):
        identity = self.frozen_identity()
        response = self.success(b"ssh-ed25519 " + base64.b64encode(b"public") + b"\n")
        for operation in ("exec", "upload", "download"):
            for field, replacement in (("config_sha256", "0" * 64),
                                       ("known_hosts_sha256", "0" * 64),
                                       ("key_public_fingerprint", "SHA256:" + "A" * 43)):
                with self.subTest(operation=operation, field=field), \
                        patch.object(remote.subprocess, "run", return_value=response) as run, \
                        redirect_stdout(io.StringIO()) as output:
                    code = remote.main(self.cli_arguments(operation) + self.expected_arguments(
                        {**identity, field: replacement}))
                    self.assertEqual(code, 1)
                    self.assertEqual(json.loads(output.getvalue()), {
                        "ok": False, "operation": operation, "reason": "frozen_ssh_identity_changed"})
                    self.assertEqual(run.call_count, 1)
                    self.assertEqual(run.call_args.args[0][0], "ssh-keygen")
                    self.assertNotIn("private-key-sentinel", output.getvalue())

    def test_cli_operations_keep_verified_target_when_config_path_changes(self):
        identity = self.frozen_identity()

        def snapshot_then_change(path):
            self.assertEqual(path, self.config_path)
            path.write_text(json.dumps({**self.config, "host": "192.0.2.11"}), encoding="utf-8")
            return deepcopy(identity)

        for operation in ("exec", "upload", "download"):
            with self.subTest(operation=operation), \
                    patch.object(remote, "snapshot_identity", side_effect=snapshot_then_change) as snapshot, \
                    patch.object(remote, "load_target_config", side_effect=AssertionError("config path reread")), \
                    patch.object(remote.subprocess, "run", return_value=self.success()) as run, \
                    redirect_stdout(io.StringIO()) as output:
                code = remote.main(self.cli_arguments(operation) + self.expected_arguments(identity))
                self.assertEqual(code, 0)
                self.assertTrue(json.loads(output.getvalue())["ok"])
                snapshot.assert_called_once_with(self.config_path)
                self.assertTrue(run.call_args_list)
                for call in run.call_args_list:
                    arguments = call.args[0]
                    self.assertTrue(any("root@192.0.2.10" in item for item in arguments))
                    self.assertFalse(any("192.0.2.11" in item for item in arguments))

    def test_exec_preserves_script_stdin_and_forces_strict_noninteractive_ssh(self):
        command = b"python3 - <<'PY'\nprint('`$(secret)')\nPY\n"
        with patch.object(remote.subprocess, "run", return_value=self.success()) as run:
            result = remote.execute(self.config, command)
        args = run.call_args.args[0]
        self.assertEqual(args[-2:], ["root@192.0.2.10", "sh -s"])
        for option in ("BatchMode=yes", "IdentitiesOnly=yes", "StrictHostKeyChecking=yes",
                       "ConnectTimeout=10", "GlobalKnownHostsFile=none",
                       "UserKnownHostsFile=" + str(self.hosts)):
            self.assertIn(option, args)
        self.assertEqual(run.call_args.kwargs["input"], command)
        self.assertFalse(run.call_args.kwargs["shell"])
        self.assertEqual(json.loads(result["stdout"]), {"accepted": True})

    def test_exec_rejects_nonjson_array_secret_jwt_control_and_large_metadata(self):
        values = (b"unexpected banner", b"[]", b'{"secret":"sentinel"}',
                  b'{"nested":{"LIVEKIT_SOAK_TOKEN":"sentinel"}}',
                  b'{"detail":"eyJhbGciOiJIUzI1NiJ9.e30.signature"}',
                  b'{"detail":"line\\ncontrol"}', b"x" * (remote.MAX_METADATA_BYTES + 1))
        for stdout in values:
            with self.subTest(stdout=stdout[:40]), patch.object(remote.subprocess, "run", return_value=self.success(stdout)):
                with self.assertRaises(remote.RemoteError):
                    remote.execute(self.config, b"caller_owned_script\n")

    def test_exec_rejects_empty_nonbytes_and_nul_command_before_launch(self):
        for command in (b"", "text", b"echo\0unexpected"):
            with self.subTest(command=command), patch.object(remote.subprocess, "run") as run:
                with self.assertRaisesRegex(remote.RemoteError, "invalid_remote_command"):
                    remote.execute(self.config, command)
                run.assert_not_called()

    def test_failures_never_expose_stderr_or_retry(self):
        failed = subprocess.CompletedProcess([], 255, b"secret-output", b"PRIVATE KEY secret-stderr")
        with patch.object(remote.subprocess, "run", return_value=failed) as run:
            with self.assertRaisesRegex(remote.RemoteError, "^ssh_operation_failed$") as raised:
                remote.execute(self.config, b"owned\n")
        self.assertEqual(raised.exception.exit_code, 255)
        self.assertEqual(run.call_count, 1)

    def test_timeout_and_missing_tool_have_safe_reasons(self):
        for error, reason in ((subprocess.TimeoutExpired("ssh private", 10), "ssh_operation_timeout"),
                              (FileNotFoundError("secret path"), "ssh_tool_unavailable")):
            with self.subTest(reason=reason), patch.object(remote.subprocess, "run", side_effect=error):
                with self.assertRaisesRegex(remote.RemoteError, "^" + reason + "$"):
                    remote.execute(self.config, b"owned\n")

    def test_transfer_path_rejects_shell_traversal_and_unrelated_paths(self):
        for value in ("/tmp/soak-render-test/../../root/file", "/tmp/soak-render-test/file;id",
                      "/tmp/soak-render-test/file name", "/tmp/soak-render-test/./file",
                      "/tmp/soak-render-test//file", "/root/livekit.yaml", "relative", "/tmp/file"):
            with self.subTest(value=value), self.assertRaises(remote.RemoteError):
                remote._remote_path(value)

    def test_upload_stages_then_atomically_links_and_explicit_overwrite_replaces(self):
        source = self.root / "sampler.py"
        source.write_text("fixture\n", encoding="ascii")
        for overwrite in (False, True):
            with self.subTest(overwrite=overwrite), patch.object(remote.subprocess, "run", return_value=self.success(b'{"committed":true}')) as run:
                remote.upload(self.config, source, "/tmp/soak-render-test/sampler.py", overwrite=overwrite)
            self.assertEqual(run.call_count, 2)
            scp_args = run.call_args_list[0].args[0]
            self.assertEqual(scp_args[0], "scp")
            self.assertIn(".b11-stage-", scp_args[-1])
            script = run.call_args_list[1].kwargs["input"].decode()
            self.assertIn("os.replace(stage, destination)" if overwrite else "os.link(stage, destination)", script)
            self.assertIn("stage.unlink(missing_ok=True)", script)

    def test_private_key_cannot_be_transferred_or_overwritten_even_explicitly(self):
        alias = self.root / "key-hardlink.pem"
        os.link(self.key, alias)
        for source in (self.key, alias):
            with self.subTest(source=source), patch.object(remote.subprocess, "run") as run:
                with self.assertRaisesRegex(remote.RemoteError, "ssh_private_key_transfer_forbidden"):
                    remote.upload(self.config, source, "/tmp/b11-test/identity.pem", overwrite=True)
                with self.assertRaisesRegex(remote.RemoteError, "ssh_identity_file_overwrite_forbidden"):
                    remote.download(self.config, "/tmp/b11-test/file", source, overwrite=True)
                run.assert_not_called()
        with patch.object(remote.subprocess, "run") as run:
            with self.assertRaisesRegex(remote.RemoteError, "ssh_identity_file_overwrite_forbidden"):
                remote.download(self.config, "/tmp/b11-test/file", self.hosts, overwrite=True)
            run.assert_not_called()

    def test_download_stages_and_refuses_overwrite_without_remote_call(self):
        destination = self.root / "metrics.csv"

        def scp_fixture(args, **kwargs):
            Path(args[-1]).write_bytes(b"downloaded-evidence")
            return self.success(b"")

        with patch.object(remote.subprocess, "run", side_effect=scp_fixture):
            remote.download(self.config, "/tmp/soak-render-test/metrics.csv", destination)
        self.assertEqual(destination.read_bytes(), b"downloaded-evidence")
        with patch.object(remote.subprocess, "run") as run:
            with self.assertRaisesRegex(remote.RemoteError, "download_destination_exists"):
                remote.download(self.config, "/tmp/soak-render-test/metrics.csv", destination)
            run.assert_not_called()
        self.assertEqual(list(self.root.glob(".b11-download-*")), [])

    def test_download_concurrent_destination_is_preserved_and_stage_removed(self):
        destination = self.root / "metrics.csv"

        def racing_scp(args, **kwargs):
            Path(args[-1]).write_bytes(b"new-evidence")
            destination.write_bytes(b"existing-evidence")
            return self.success(b"")

        with patch.object(remote.subprocess, "run", side_effect=racing_scp):
            with self.assertRaisesRegex(remote.RemoteError, "download_destination_exists"):
                remote.download(self.config, "/tmp/b11-test/metrics.csv", destination)
        self.assertEqual(destination.read_bytes(), b"existing-evidence")
        self.assertEqual(list(self.root.glob(".b11-download-*")), [])

    def test_cli_failure_prints_only_safe_reason_and_returns_ssh_exitcode(self):
        command = self.root / "command.sh"
        command.write_text("owned\n", encoding="ascii")
        failed = subprocess.CompletedProcess([], 255, b"secret-output", b"secret-stderr")
        output = io.StringIO()
        with patch.object(remote.subprocess, "run", return_value=failed), redirect_stdout(output):
            code = remote.main(["exec", "--target-config", str(self.config_path), "--command-file", str(command)])
        self.assertEqual(code, 255)
        self.assertEqual(json.loads(output.getvalue()), {"ok": False, "operation": "exec", "reason": "ssh_operation_failed"})
        self.assertNotIn("secret", output.getvalue())


if __name__ == "__main__":
    unittest.main()
