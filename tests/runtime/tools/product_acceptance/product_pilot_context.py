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
import subprocess
import sys
import time


RECOVERY_ATTEMPTS = 3
RECOVERY_BUDGET_SECONDS = 45
RECOVERY_CALL_SECONDS = 15


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


REMOTE_ACK_RECOVERY = r'''
import base64,json,sys
from pathlib import Path
c=json.loads(base64.b64decode(sys.argv[2]))
run=c['run_id']
assert len(run)==32 and all(x in '0123456789abcdef' for x in run)
root=Path(sys.argv[1])/('pilot-'+run[:8])
assert json.loads((root/'ready.json').read_text())['run_id']==run
context_path=root/'context.json'
ack_path=root/'context-ack.json'
assert json.loads(context_path.read_text())==c
observed=json.loads(ack_path.read_text())
assert observed['context']==c
assert type(observed['sequence']) is int and observed['sequence']>0
issued=context_path.stat().st_mtime_ns
accepted=ack_path.stat().st_mtime_ns
assert 0<=accepted-issued<=10_000_000_000
observed['recovery_proof']={'context_written_ns':issued,'ack_written_ns':accepted}
print(json.dumps(observed))
'''


def validate_ack(ack, context, recovered=False):
    if ack.get("context") != context or type(ack.get("sequence")) is not int or ack["sequence"] <= 0:
        raise ValueError("remote_context_ack_mismatch")
    if recovered:
        proof = ack.get("recovery_proof") or {}
        issued, accepted = proof.get("context_written_ns"), proof.get("ack_written_ns")
        if type(issued) is not int or type(accepted) is not int or issued <= 0 or \
                not 0 <= accepted-issued <= 10_000_000_000:
            raise ValueError("remote_context_recovery_deadline_unproven")
    return ack


def fence(path):
    context = json.loads(path.read_text(encoding="utf-8-sig"))
    validate_context(context, context["run_id"])
    from product_aliyun_transport import AliyunRemoteCommandError, execute, target
    import shlex
    payload = base64.b64encode(json.dumps(context).encode()).decode()
    arguments = " " + shlex.quote(target()["remote_root"]) + " " + shlex.quote(payload)
    recovered = False
    attempts = []
    session_limit_failures = 0
    verdict = "FAILED"

    def request(stage, script, timeout):
        nonlocal session_limit_failures
        started = time.monotonic()
        entry = dict(stage=stage, readonly=stage == "receipt_recovery", timeout_seconds=timeout)
        try:
            return json.loads(execute("python3 -c " + shlex.quote(script) + arguments, timeout=timeout))
        except Exception as error:
            entry["error_type"] = type(error).__name__
            if isinstance(error, AliyunRemoteCommandError):
                entry["transport"] = error.diagnostics
                session_limit_failures += int(error.diagnostics["service_code"] == "Forbidden.SessionLimit")
            raise
        finally:
            entry["elapsed_seconds"] = time.monotonic() - started
            attempts.append(entry)

    try:
        try:
            ack = request("publish_and_wait", REMOTE_FENCE, 45)
        except (RuntimeError, subprocess.TimeoutExpired, json.JSONDecodeError):
            # Read only an already-issued receipt. Retrying retrieval never
            # republishes context or extends its original ten-second ACK window.
            deadline = time.monotonic() + RECOVERY_BUDGET_SECONDS
            for attempt in range(RECOVERY_ATTEMPTS):
                if attempt:
                    time.sleep(.5)
                remaining = int(deadline - time.monotonic())
                if remaining <= 5:
                    raise RuntimeError("context_recovery_budget_exhausted")
                try:
                    ack = request("receipt_recovery", REMOTE_ACK_RECOVERY,
                                  min(RECOVERY_CALL_SECONDS, remaining))
                    # Identity/deadline rejection is terminal, never retried.
                    validate_ack(ack, context, True)
                    recovered = True
                    break
                except (RuntimeError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
                    remote_rejected = isinstance(error, AliyunRemoteCommandError) and \
                        error.diagnostics["remote_exit_code"] not in (None, 0)
                    if remote_rejected or session_limit_failures >= 2 or attempt == RECOVERY_ATTEMPTS - 1:
                        raise
        validate_ack(ack, context, recovered)
        ack["transport_recovered"] = recovered
        with (path.parent / "context-acks.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps(ack) + "\n")
        verdict = "CONFIRMED"
    finally:
        record = dict(run_id=context["run_id"], cycle=context["cycle"],
            operation_id=context["operation_id"], action=context["action"], phase=context["phase"],
            verdict=verdict, transport_recovered=recovered, attempts=attempts)
        with (path.parent / "context-fence-attempts.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps(record) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path, required=True)
    try:
        fence(parser.parse_args().file)
    except Exception as error:
        print(type(error).__name__ + ":context_fence_failed", file=sys.stderr)
        raise SystemExit(1)
