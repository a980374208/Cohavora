"""Run a bounded set of low-bitrate LiveKit file publishers on the ECS."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

import yaml


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--room", required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--count", type=int, default=17)
    args = parser.parse_args()
    if not args.room.startswith("soak-") or not 1 <= args.count <= 100:
        parser.error("invalid room or publisher count")
    if not args.source.is_file():
        parser.error("video source missing")
    args.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    status_path = args.directory / "publishers-status.json"
    config = yaml.safe_load(Path("/root/livekit.yaml").read_text())
    key, secret = next(iter(config["keys"].items()))
    env = os.environ.copy()
    env.update(LIVEKIT_URL="http://127.0.0.1:17880",
               LIVEKIT_API_KEY=key, LIVEKIT_API_SECRET=secret)
    children: list[subprocess.Popen] = []
    stopping = False

    def stop(_signal, _frame):
        nonlocal stopping
        stopping = True

    def start_ticks(pid: int):
        try:
            return Path(f"/proc/{pid}/stat").read_text().split()[21]
        except FileNotFoundError:
            return None

    def write_status(state: str, error: str = ""):
        value = {"schema": 1, "room": args.room, "state": state,
                 "expected": args.count, "started": len(children),
                 "alive": sum(child.poll() is None for child in children),
                 "children": [{"pid": child.pid, "start_ticks": start_ticks(child.pid)}
                              for child in children],
                 "error": error, "updated_unix": time.time()}
        temporary = status_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(value))
        temporary.replace(status_path)

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    error = ""
    try:
        for index in range(args.count):
            if stopping:
                break
            child = subprocess.Popen([
                "lk", "room", "join", "--publish", str(args.source), "--fps", "8",
                "--identity", f"{args.room}-pub-{index:02d}", args.room, "--yes"],
                env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL)
            children.append(child)
            write_status("STARTING")
            time.sleep(.5)
            if child.poll() is not None:
                error = f"publisher_{index}_exited_{child.returncode}"
                break
        if not error and not stopping:
            write_status("RUNNING")
        while not stopping and not error:
            for index, child in enumerate(children):
                if child.poll() is not None:
                    error = f"publisher_{index}_exited_{child.returncode}"
                    break
            if not error:
                write_status("RUNNING")
                time.sleep(2)
    finally:
        write_status("STOPPING", error)
        for child in children:
            if child.poll() is None:
                child.send_signal(signal.SIGINT)
        for child in children:
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
        write_status("FAILED" if error else "STOPPED", error)
    return 1 if error else 0


if __name__ == "__main__":
    raise SystemExit(main())
