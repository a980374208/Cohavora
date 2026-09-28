"""Explicit observer context fence; never fabricates native/server operation IDs.

UIA sends this before each action and after observing its result. The receiver
acknowledges the context before UIA continues. Native IDs come from the product
probe and remain separate from the test driver's operation ID.
"""
import argparse
import base64
import json
from pathlib import Path
import re
import sys


def validate_context(value, run):
    allowed = {"run_id", "cycle", "cycle_id", "operation_id", "action", "phase",
               "pid", "process_run_id", "anonymous_session_id", "participant_sha256"}
    if set(value) != allowed or value["run_id"] != run:
        raise ValueError("context_schema_or_run")
    for key in ("run_id", "cycle_id", "operation_id", "process_run_id"):
        if not re.fullmatch(r"[0-9a-f]{32}", str(value[key])):
            raise ValueError("context_" + key)
    if value["anonymous_session_id"] is not None and not re.fullmatch(
            r"[0-9a-f]{32}", str(value["anonymous_session_id"])):
        raise ValueError("context_session")
    if value["participant_sha256"] is not None and not re.fullmatch(r"[0-9a-f]{64}", str(value["participant_sha256"])):
        raise ValueError("context_participant")
    if type(value["cycle"]) is not int or not 1 <= value["cycle"] <= 100:
        raise ValueError("context_cycle")
    if type(value["pid"]) is not int or value["pid"] <= 0:
        raise ValueError("context_pid")
    if value["phase"] not in ("requested", "uia_observed") or value["action"] not in (
            "login", "join", "page", "share_start", "share_stop", "logging", "leave", "export", "process_exit"):
        raise ValueError("context_action")
    return value


REMOTE_FENCE = r'''
import json,sys,time
from pathlib import Path
import base64
c=json.loads(base64.b64decode(sys.argv[2]))
run=c['run_id']
assert len(run)==32 and all(x in '0123456789abcdef' for x in run)
root=Path(sys.argv[1])/('pilot-'+run[:8])
assert json.loads((root/'ready.json').read_text())['run_id']==run
temporary=root/'context.tmp'
temporary.write_text(json.dumps(c))
temporary.replace(root/'context.json')
deadline=time.monotonic()+10
while time.monotonic()<deadline:
    ack=root/'context-ack.json'
    if ack.exists():
        observed=json.loads(ack.read_text())
        if observed['context']==c:
            print(json.dumps(observed));sys.exit(0)
    time.sleep(.05)
sys.exit(3)
'''


def fence(path):
    context = json.loads(path.read_text(encoding="utf-8-sig"))
    validate_context(context, context["run_id"])
    from product_aliyun_transport import execute, target
    import shlex
    payload = base64.b64encode(json.dumps(context).encode()).decode()
    ack = json.loads(execute("python3 -c " + shlex.quote(REMOTE_FENCE) + " "
        + shlex.quote(target()["remote_root"]) + " " + shlex.quote(payload)))
    if ack.get("context") != context or type(ack.get("sequence")) is not int:
        raise ValueError("remote_context_ack_mismatch")
    with (path.parent / "context-acks.jsonl").open("a", encoding="utf-8") as output:
        output.write(json.dumps(ack) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path, required=True)
    try:
        fence(parser.parse_args().file)
    except Exception as error:
        print(type(error).__name__ + ":context_fence_failed", file=sys.stderr)
        raise SystemExit(1)
