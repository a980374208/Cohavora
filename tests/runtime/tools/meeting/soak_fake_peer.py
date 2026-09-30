"""Local fault fixture for the soak supervisor; never connects to an SFU."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time


def atomic_json(path: Path, value: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value), encoding="utf-8")
    # Windows readers/scanners can briefly hold a handle without FILE_SHARE_DELETE.
    # Preserve complete snapshots and retry the replacement, never truncate the file.
    deadline = time.monotonic() + 0.2
    while True:
        try:
            os.replace(temporary, path)
            return
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.005)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--soak-directory", type=Path, required=True)
    parser.add_argument(
        "--mode",
        choices=("normal", "crash", "hang", "strand-hang", "reject",
                 "wrong-run", "exit-zero-early", "missing-stop", "load-disappears",
                 "media-freeze", "counters-reset-then-progress", "wrong-command-noack"),
        default="normal",
    )
    args = parser.parse_args()
    output = args.soak_directory
    output.mkdir(parents=True, exist_ok=True)
    (output / "fake_peer.pid").write_text(str(os.getpid()), encoding="ascii")
    run_id = os.environ["COHAVORA_SOAK_RUN_ID"]
    status = {
        "schema": 1,
        "run_id": run_id if args.mode != "wrong-run" else "unrelated-run",
        "pid": os.getpid(),
        "heartbeat_seq": 0,
        "command_seq": 0,
        "command_status": "applied",
        "state": "connected",
        "runtime_seq": 0,
        "policy_revision": 1,
        "requested": 4,
        "selected": 4,
        "actual": 4,
        "bound": 4,
        "selected_not_bound": 0,
        "bound_not_selected": 0,
        "remote_video_count": 20,
        "render_submits": 1,
        "decoded_frames": 1,
        "audio_frames": None,
        "sharing": False,
        "error_code": "",
    }
    start = time.monotonic()
    stopped_at = None
    reset_at = None
    while True:
        elapsed = time.monotonic() - start
        inject = elapsed >= 0.18
        if inject and args.mode == "crash":
            return 23
        if inject and args.mode == "exit-zero-early":
            return 0
        if inject and args.mode == "hang":
            time.sleep(0.01)
            continue
        command_path = output / "command.json"
        try:
            command = json.loads(command_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            command = {}
        if (args.mode != "wrong-command-noack" and command.get("run_id") == run_id
                and command.get("seq", 0) > status["command_seq"]):
            status["command_seq"] = command["seq"]
            status["policy_revision"] += 1
            if args.mode == "reject":
                status["command_status"] = "rejected"
                status["error_code"] = "fixture_rejected"
            elif command.get("action") in ("stop", "shutdown", "close"):
                status["state"] = "stopping"
                status["command_status"] = "pending"
                if args.mode != "missing-stop":
                    status["state"] = "stopped"
                    status["command_status"] = "applied"
                    for key in ("requested", "selected", "actual", "bound"):
                        status[key] = 0
                    stopped_at = time.monotonic()
            else:
                status["command_status"] = "applied"
        status["heartbeat_seq"] += 1
        if not (inject and args.mode == "strand-hang"):
            status["runtime_seq"] += 1
        if inject and args.mode == "load-disappears":
            status["remote_video_count"] = 0
        if inject and args.mode == "counters-reset-then-progress" and reset_at is None:
            status["render_submits"] = 0
            status["decoded_frames"] = 0
            reset_at = time.monotonic()
        if (not (inject and args.mode == "media-freeze") and
                (reset_at is None or time.monotonic() - reset_at >= 0.06)):
            status["render_submits"] += 1
            status["decoded_frames"] += 1
        atomic_json(output / "status.json", status)
        if stopped_at is not None and time.monotonic() - stopped_at >= 0.15:
            return 0
        time.sleep(0.01)


if __name__ == "__main__":
    raise SystemExit(main())
