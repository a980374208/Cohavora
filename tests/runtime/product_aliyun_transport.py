"""Workbench transport for the bounded product acceptance run; no credentials in output."""
import argparse
import base64
import json
from pathlib import Path
import re
import shlex
import subprocess


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
        "-r", cfg["region"], "--timeout", str(timeout - 5), "-c", wrapped],
        capture_output=True, text=True, timeout=timeout)
    output, found, status = result.stdout.rpartition(marker)
    if result.returncode or not found or status.strip() != "0":
        raise RuntimeError("ALIYUN_REMOTE_COMMAND_FAILED")
    return output.rstrip()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command")
    args = parser.parse_args()
    try:
        print(execute(args.command))
    except Exception as error:
        parser.exit(1, type(error).__name__ + ":ALIYUN_TRANSPORT_FAILED\n")
