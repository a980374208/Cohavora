"""Workbench transport for the bounded product acceptance run; no credentials in output."""
import argparse
import base64
import json
from pathlib import Path
import re
import shlex
import subprocess


class AliyunRemoteCommandError(RuntimeError):
    """Bounded transport evidence without command, output, or credentials."""
    def __init__(self, result, marker_seen, remote_exit_code):
        super().__init__("ALIYUN_REMOTE_COMMAND_FAILED")
        session_limit = any("Forbidden.SessionLimit" in output
                            for output in (result.stdout, result.stderr))
        self.diagnostics = dict(cli_returncode=result.returncode,
            exit_marker_seen=marker_seen, remote_exit_code=remote_exit_code,
            stdout_chars=len(result.stdout), stderr_chars=len(result.stderr),
            service_code="Forbidden.SessionLimit" if session_limit else "UNCLASSIFIED")


def target():
    value = json.loads(Path(__file__).with_name("product_aliyun_target.json").read_text())
    if not re.fullmatch(r"i-[a-z0-9]+", value["instance_id"]):
        raise ValueError("invalid_instance")
    if not re.fullmatch(r"/[a-zA-Z0-9/_-]+", value["remote_root"]):
        raise ValueError("invalid_remote_root")
    return value


def execute(command, timeout=45):
    cfg = target()
    # A marker verifies the remote exit status even if the CLI returns zero.
    import uuid
    marker = "PRODUCT_EXIT_" + uuid.uuid4().hex + ":"
    payload = base64.b64encode(command.encode()).decode()
    wrapped = ("bash -c " + shlex.quote("echo " + payload + " | base64 -d | bash")
               + "; result=$?; printf '\\n" + marker + "%s\\n' \"$result\"")
    result = subprocess.run(["workbench", "exec", "-i", cfg["instance_id"],
        "-r", cfg["region"], "--output", "json", "--timeout", str(max(1, int(timeout - 5))), "-c", wrapped],
        capture_output=True, text=True, encoding="utf-8", timeout=timeout)
    # The JSON command field also contains our marker. Only the independently
    # identified remote stdout may supply the command's exit proof.
    response_status = "MALFORMED_JSON"
    remote_stdout, envelope_exit_code = "", None
    try:
        response = json.loads(result.stdout)
    except json.JSONDecodeError:
        response = None
    if isinstance(response, dict):
        response_status = "INVALID_RESPONSE_SCHEMA"
        if type(response.get("exit_code")) is int and isinstance(response.get("stdout"), str):
            envelope_exit_code = response["exit_code"]
            if response.get("instance_id") != cfg["instance_id"] or response.get("command") != wrapped:
                response_status = "RESPONSE_IDENTITY_MISMATCH"
            else:
                response_status = "VALID"
                remote_stdout = response["stdout"]
    output, found, status = remote_stdout.rpartition(marker)
    remote_exit_code = int(status.strip()) if found and re.fullmatch(r"[0-9]{1,3}", status.strip()) else None
    if result.returncode or response_status != "VALID" or envelope_exit_code != 0 or not found or status.strip() != "0":
        error = AliyunRemoteCommandError(result, bool(found), remote_exit_code)
        error.diagnostics.update(response_status=response_status, envelope_exit_code=envelope_exit_code)
        raise error
    return output.rstrip()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command")
    args = parser.parse_args()
    try:
        print(execute(args.command))
    except Exception as error:
        parser.exit(1, type(error).__name__ + ":ALIYUN_TRANSPORT_FAILED\n")
