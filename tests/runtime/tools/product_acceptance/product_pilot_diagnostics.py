"""Read only this PILOT's typed JSONL segments and retain a sequence witness.

Do not copy raw logs or attributes: the witness contains only validated run IDs,
integer sequence numbers, source sizes and SHA-256 digests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time
from product_pilot_privacy import validate_serialized_event
from product_pilot_run_budget import load_run_budget


def safe_event(event, process_run):
    validate_serialized_event(event)
    if event.get("schema_version") != 1 or event.get("process_run_id") != process_run:
        raise ValueError("diagnostic_identity")
    fields = ("event_sequence", "occurred_at_utc_ms", "monotonic_us", "pid")
    if any(type(event.get(k)) is not int or event[k] < 0 for k in fields):
        raise ValueError("diagnostic_numeric_field")
    name = event.get("event_name", "")
    if not re.fullmatch(r"[a-zA-Z0-9_.-]{1,96}", name):
        raise ValueError("diagnostic_event_code")
    result = {k: event[k] for k in fields}
    result.update(event_name=name, process_run_id=process_run)
    for key in ("anonymous_session_id", "operation_id", "parent_operation_id", "request_id"):
        if key in event:
            if not re.fullmatch(r"[a-zA-Z0-9_:.\-]{1,64}", event[key]):
                raise ValueError("diagnostic_context_id")
            result[key] = event[key]
    for key in ("session_generation", "room_generation", "recovery_epoch"):
        if key in event:
            if type(event[key]) is not int or event[key] < 0:
                raise ValueError("diagnostic_context_generation")
            result[key] = event[key]
    if event.get("severity") not in ("trace", "debug", "info", "warning", "error", "fatal"):
        raise ValueError("diagnostic_severity")
    result["severity"] = event["severity"]
    return result


def watch(root, seconds, run_budget=None):
    run = json.loads((root / "plan.json").read_text(encoding="utf-8-sig"))["run_id"] if run_budget else None
    budget = load_run_budget(run_budget, run, seconds)
    next_sequence, offsets = 1, {}
    destination = root / "diagnostic-events.jsonl"
    with destination.open("x", encoding="utf-8", buffering=1) as output:
        while not budget.expired():
            probe_path = root / "process-probe.jsonl"
            if not probe_path.exists():
                time.sleep(.2)
                continue
            with probe_path.open(encoding="utf-8-sig") as stream:
                first = stream.readline()
            if not first.endswith("\n"):
                time.sleep(.2)
                continue
            probe = json.loads(first)
            if run is not None and probe.get("run_id") != run:
                raise ValueError("diagnostic_probe_run_mismatch")
            process_run = probe["process_run_id"]
            if not re.fullmatch(r"[0-9a-f]{32}", process_run):
                raise ValueError("invalid_process_run")
            source = Path(probe["diagnostic_root"]) / ("run-" + process_run)
            if source.is_symlink():
                raise ValueError("diagnostic_link")
            for path in sorted(source.glob("segment-*.jsonl")):
                if not re.fullmatch(r"segment-[0-9]{6}\.jsonl", path.name) or path.is_symlink():
                    raise ValueError("unexpected_segment")
                if path.stat().st_size > 16 * 1024 * 1024:
                    raise ValueError("segment_budget")
                with path.open("rb") as stream:
                    stream.seek(offsets.get(path.name, 0))
                    for line in stream:
                        if not line.endswith(b"\n"):
                            break
                        event = safe_event(json.loads(line), process_run)
                        if event["event_sequence"] != next_sequence:
                            raise ValueError("diagnostic_sequence_gap")
                        next_sequence += 1
                        offsets[path.name] = offsets.get(path.name, 0) + len(line)
                        output.write(json.dumps(dict(event, run_id=probe["run_id"],
                            source_segment=path.name, raw_sha256=hashlib.sha256(line).hexdigest())) + "\n")
            interrupted = (root / "collector-stop.json").exists()
            if (root / "uia/uia-result.json").exists() or interrupted:
                (root / "diagnostic-watcher-result.json").write_text(json.dumps(dict(
                    run_id=probe["run_id"], process_run_id=process_run,
                    status="INTERRUPTED" if interrupted else "COMPLETE", events=next_sequence-1,
                    privacy_schema_violations=0,privacy_scope="typed schema and bounded token fields; no semantic secret classifier")))
                return
            time.sleep(1)
    raise TimeoutError("diagnostic_watch_timeout")


def collect(root):
    with (root / "process-probe.jsonl").open(encoding="utf-8-sig") as stream:
        probe = json.loads(stream.readline())
    process_run = probe["process_run_id"]
    if not re.fullmatch(r"[0-9a-f]{32}", process_run):
        raise ValueError("invalid_process_run")
    source = Path(probe["diagnostic_root"]) / ("run-" + process_run)
    if source.is_symlink():
        raise ValueError("diagnostic_link")
    segments = []
    for path in sorted(source.glob("segment-*.jsonl")):
        if not re.fullmatch(r"segment-[0-9]{6}\.jsonl", path.name) or path.is_symlink():
            raise ValueError("unexpected_segment")
        if path.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("segment_budget")
        content = path.read_bytes()
        if not content.endswith(b"\n"):
            raise ValueError("incomplete_diagnostic_line")
        sequences = []
        for line in content.splitlines():
            event = json.loads(line)
            sequence = event.get("event_sequence")
            if (event.get("schema_version") != 1 or
                event.get("process_run_id") != process_run or
                type(sequence) is not int or sequence < 1):
                raise ValueError("invalid_diagnostic_identity_or_sequence")
            sequences.append(sequence)
        segments.append(dict(file=path.name, size_bytes=len(content),
            sha256=hashlib.sha256(content).hexdigest(), sequences=sequences))
    if not segments:
        raise ValueError("no_diagnostic_segments")
    witness = dict(schema=1, run_id=probe["run_id"], process_run_id=process_run,
        collector="external_typed_diagnostic_sequence", segments=segments)
    with (root / "diagnostic-sequences.json").open("x", encoding="utf-8") as out:
        json.dump(witness, out, indent=2)
        out.write("\n")
    print(json.dumps(dict(segments=len(segments), events=sum(
        len(s["sequences"]) for s in segments))))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--watch", action="store_true")
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--run-budget", type=Path, help="Required by the production invoker; one shared QPC marker")
    args = parser.parse_args()
    if args.watch:
        watch(args.root, args.seconds, args.run_budget)
    else:
        collect(args.root)
