"""Archive this PILOT's immutable, committed checkpoint segments before retention.

Does not change the product's quota or sampling. Every copied segment is checked
against its manifest SHA-256. Final review must independently verify revisions.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time

def read_committed(path):
    # ReplaceFile/MoveFileEx can briefly deny a new Windows reader while an
    # atomic manifest replacement commits. Retry that access conflict with a
    # fixed bound; a persistent denial still fails the archive, never skips it.
    for attempt in range(21):
        try:
            return path.read_bytes()
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(.05)


def collect(args):
    args.output.mkdir(parents=True, exist_ok=False)
    known = set()
    total = 0
    sequence = 0
    deadline = time.monotonic() + args.seconds
    settled = None
    with (args.output / "collector.jsonl").open("x", encoding="utf-8", buffering=1) as log:
        def emit(event, **values):
            nonlocal sequence
            sequence += 1
            log.write(json.dumps(dict(run_id=args.run_id, sequence=sequence,
                collector="external_checkpoint_archive", event=event,
                utc=datetime.now(timezone.utc).isoformat(), **values)) + "\n")

        try:
            while time.monotonic() < deadline:
                if args.probe.exists():
                    # First complete row gives immutable process/run/root identity.
                    with args.probe.open(encoding="utf-8-sig") as stream:
                        first = stream.readline()
                    if not first.endswith("\n"):
                        time.sleep(.2)
                        continue
                    probe = json.loads(first)
                    if probe["run_id"] != args.run_id:
                        raise ValueError("run_identity_mismatch")
                    for manifest_path in Path(probe["history_root"]).glob("cohavora-telemetry-v2-*/manifest.json"):
                        try:
                            raw = read_committed(manifest_path)
                        except FileNotFoundError:
                            continue  # another inactive run was pruned
                        manifest = json.loads(raw)
                        if manifest.get("process_run_id") != probe["process_run_id"]:
                            continue
                        session = manifest["anonymous_session_id"]
                        if not re.fullmatch("[0-9a-f]{32}", session):
                            raise ValueError("invalid_session_id")
                        destination = args.output / session
                        destination.mkdir(exist_ok=True)
                        for entry in manifest["segments"]:
                            name = entry["file"]
                            if not re.fullmatch(r"segment-[0-9]{20}\.jsonl", name):
                                raise ValueError("invalid_segment_path")
                            key = (session, name, entry["sha256"])
                            if key in known:
                                continue
                            source = manifest_path.parent / name
                            if source.is_symlink():
                                raise ValueError("segment_link")
                            content = read_committed(source)
                            if len(content) != entry["size_bytes"] or hashlib.sha256(content).hexdigest() != entry["sha256"]:
                                raise ValueError("segment_integrity_mismatch")
                            if total + len(content) > args.maximum_bytes:
                                raise ValueError("external_archive_budget_exceeded")
                            with (destination / name).open("xb") as out:
                                out.write(content)
                            total += len(content)
                            known.add(key)
                            emit("segment.archived", session=session, **entry)
                        (destination / "manifest.json").write_bytes(raw)
                        (destination / ("manifest-%020d.json" % manifest["last_committed_revision"])).write_bytes(raw)
                interrupted = (args.result.parent.parent / "collector-stop.json").exists()
                if args.result.exists() or interrupted:
                    settled = settled or time.monotonic()
                    if time.monotonic() - settled >= 3:
                        emit("collector.stopped", status="INTERRUPTED" if interrupted else "COMPLETE",
                             segments=len(known), bytes=total)
                        return 1 if interrupted else 0
                time.sleep(.5)
            raise TimeoutError("pilot_result_not_observed")
        except Exception as error:
            emit("collector.error", error_type=type(error).__name__, reason=str(error))
            return 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--maximum-bytes", type=int, default=1024**3)
    raise SystemExit(collect(parser.parse_args()))
