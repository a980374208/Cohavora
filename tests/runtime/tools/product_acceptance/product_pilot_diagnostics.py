"""Read only this PILOT's typed JSONL segments and retain a sequence witness.

Do not copy raw logs or attributes: the witness contains only validated run IDs,
integer sequence numbers, source sizes and SHA-256 digests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import time
from product_pilot_privacy import validate_serialized_event
from product_pilot_run_budget import load_run_budget


def open_segment_reader(path):
    """Keep native retention free to unlink a closed segment while we read it."""
    if os.name != "nt":
        return path.open("rb")
    import ctypes
    from ctypes import wintypes
    import msvcrt

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                       wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    close = kernel.CloseHandle
    close.argtypes = [wintypes.HANDLE]
    close.restype = wintypes.BOOL
    # GENERIC_READ; FILE_SHARE_READ | WRITE | DELETE; OPEN_EXISTING.
    handle = create(str(path), 0x80000000, 0x1 | 0x2 | 0x4, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except BaseException:
        close(handle)
        raise
    # The CRT now owns the handle; fdopen takes ownership only on success.
    try:
        return os.fdopen(descriptor, "rb")
    except BaseException:
        os.close(descriptor)
        raise


def safe_event(event, process_run):
    validate_serialized_event(event)
    if type(event.get("schema_version")) is not int or event["schema_version"] != 1 or event.get("process_run_id") != process_run:
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


def diagnostic_source(root):
    with (root / "process-probe.jsonl").open(encoding="utf-8-sig") as stream:
        first = stream.readline()
    if not first.endswith("\n"):
        raise ValueError("diagnostic_probe_first_row_incomplete")
    probe = json.loads(first)
    process_run = probe["process_run_id"]
    if not isinstance(process_run, str) or not re.fullmatch(r"[0-9a-f]{32}", process_run):
        raise ValueError("invalid_process_run")
    source = Path(probe["diagnostic_root"]) / ("run-" + process_run)
    if source.is_symlink():
        raise ValueError("diagnostic_link")
    return probe, source


def watch(root, seconds, run_budget=None):
    # The marker exists before collectors launch. Never substitute a private
    # deadline when the production invocation supplied a marker path.
    run = json.loads((root / "plan.json").read_text(encoding="utf-8-sig"))["run_id"] if run_budget else None
    budget = load_run_budget(run_budget, run, seconds)
    next_sequence, offsets, terminal_observed, observed_pid = 1, {}, False, None
    terminal_drain_deadline = None
    interrupted_seen = False
    destination = root / "diagnostic-events.jsonl"
    with destination.open("x", encoding="utf-8", buffering=1) as output:
        while terminal_observed or not budget.expired():
            if terminal_drain_deadline is not None and time.monotonic() >= terminal_drain_deadline:
                raise TimeoutError("diagnostic_terminal_drain_timeout")
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
                try:
                    with open_segment_reader(path) as stream:
                        if os.fstat(stream.fileno()).st_size > 16 * 1024 * 1024:
                            raise ValueError("segment_budget")
                        stream.seek(offsets.get(path.name, 0))
                        for line in stream:
                            if not line.endswith(b"\n"):
                                break
                            event = safe_event(json.loads(line), process_run)
                            observed_pid = event["pid"] if observed_pid is None else observed_pid
                            if event["pid"] != observed_pid or observed_pid <= 0:
                                raise ValueError("diagnostic_pid_changed")
                            if event["event_sequence"] != next_sequence:
                                raise ValueError("diagnostic_sequence_gap")
                            next_sequence += 1
                            offsets[path.name] = offsets.get(path.name, 0) + len(line)
                            output.write(json.dumps(dict(event, run_id=probe["run_id"],
                                source_segment=path.name, raw_sha256=hashlib.sha256(line).hexdigest())) + "\n")
                except FileNotFoundError:
                    if not offsets.get(path.name):
                        raise  # An unobserved segment cannot be called retained.
                    # Retention may prune an already-watched closed segment
                    # between glob and open. The next exact sequence and the
                    # final native terminal anchor still reject any lost tail.
                    continue
            interrupted = (root / "collector-stop.json").exists()
            interrupted_seen = interrupted_seen or interrupted
            if (root / "uia/uia-result.json").exists() or interrupted or terminal_observed:
                if not terminal_observed:
                    if not interrupted and budget.expired():
                        raise TimeoutError("diagnostic_result_not_observed_before_run_deadline")
                    # UIA publishes its result after the owned product exits.
                    # Rescan once after that publication to include the final
                    # drained native line which could race the preceding read.
                    terminal_observed = True
                    terminal_drain_deadline = time.monotonic() + 3
                    continue
                (root / "diagnostic-watcher-result.json").write_text(json.dumps(dict(
                    run_id=probe["run_id"], process_run_id=process_run, pid=observed_pid,
                    status="INTERRUPTED" if interrupted_seen else "COMPLETE", events=next_sequence-1,
                    privacy_schema_violations=0,privacy_scope="typed schema and bounded token fields; no semantic secret classifier")))
                return
            time.sleep(1)
    raise TimeoutError("diagnostic_watch_timeout")


def collect(root):
    probe, source = diagnostic_source(root)
    process_run = probe["process_run_id"]
    segments = []
    last_event = None
    pid = None
    for path in sorted(source.glob("segment-*.jsonl")):
        if not re.fullmatch(r"segment-[0-9]{6}\.jsonl", path.name) or path.is_symlink():
            raise ValueError("unexpected_segment")
        with open_segment_reader(path) as stream:
            if os.fstat(stream.fileno()).st_size > 16 * 1024 * 1024:
                raise ValueError("segment_budget")
            content = stream.read()
        if not content.endswith(b"\n"):
            raise ValueError("incomplete_diagnostic_line")
        sequences, rows = [], []
        for line in content.splitlines(keepends=True):
            event = json.loads(line)
            sequence = event.get("event_sequence")
            bounded = safe_event(event, process_run)
            if type(sequence) is not int or sequence < 1 or type(bounded["pid"]) is not int or bounded["pid"] <= 0:
                raise ValueError("invalid_diagnostic_identity_or_sequence")
            pid = bounded["pid"] if pid is None else pid
            if bounded["pid"] != pid:
                raise ValueError("diagnostic_pid_changed")
            sequences.append(sequence)
            rows.append(dict(event_sequence=sequence, raw_sha256=hashlib.sha256(line).hexdigest()))
            last_event = event
        segments.append(dict(file=path.name, size_bytes=len(content),
            sha256=hashlib.sha256(content).hexdigest(), sequences=sequences, records=rows))
    if not segments or last_event is None:
        raise ValueError("no_diagnostic_segments")
    witness = dict(schema=2, run_id=probe["run_id"], process_run_id=process_run, pid=pid,
        collector="external_typed_diagnostic_sequence", segments=segments,
        terminal_anchor=dict(event_sequence=last_event["event_sequence"], event_name=last_event["event_name"],
            outcome=last_event.get("attributes", {}).get("outcome"),
            drain_result=last_event.get("attributes", {}).get("drain_result"),
            raw_sha256=segments[-1]["records"][-1]["raw_sha256"]))
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
