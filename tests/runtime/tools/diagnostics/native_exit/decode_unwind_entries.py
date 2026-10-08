"""Decode bounded NXENTRY2 scalar evidence; never read PXTRACE1 or payloads.

Caller PCs require the same owned process's frozen module map for attribution.
An empty log cannot exclude loader bypass or a write failure; counts in the live
observer API are not implicitly available after process termination.
"""
import argparse
import json
from pathlib import Path
import struct

RECORD = struct.Struct("<8sIIQQQQIIQ")
KINDS = {1: "pthread_exit"}


def decode(path: Path) -> dict:
    size = path.stat().st_size
    if size > 64 * RECORD.size or size % RECORD.size:
        raise ValueError("invalid_length_or_partial_write")
    data = path.read_bytes()
    if len(data) != size:
        raise ValueError("file_changed_during_read")
    events, sequences, pids = [], set(), set()
    for values in RECORD.iter_unpack(data):
        magic, version, kind, pid, tid, sequence, caller, failures, finalizing, monotonic_ns = values
        if (magic != b"NXENTRY2" or version != 2 or kind not in KINDS
                or not pid or not tid or sequence >= 64 or sequence in sequences
                or finalizing not in (0, 1, 2)):
            raise ValueError("invalid_record")
        sequences.add(sequence)
        pids.add(pid)
        events.append({"kind": KINDS[kind], "pid": pid, "tid": tid,
                       "sequence": sequence, "caller_pc": hex(caller),
                       "write_failures_before": failures,
                       "python_finalizing": (False if finalizing == 0 else True if finalizing == 1 else "UNKNOWN"),
                       "monotonic_ns": monotonic_ns,
                       "clock_query_ok": monotonic_ns != 0})
    if len(pids) > 1:
        raise ValueError("mixed_process_records")
    return {"status": "SCALAR_ENTRIES_CAPTURED" if events else "NO_ENTRY_INCONCLUSIVE",
            "events": events, "record_bytes": RECORD.size,
            "sequence_is_entry_order_not_write_order": True,
            "cap_reached": 63 in sequences, "diagnostic_only": True,
            "native_exit_repair": "NOT_PROVEN"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("entry_file", type=Path)
    args = parser.parse_args()
    try:
        result = decode(args.entry_file)
    except (OSError, ValueError) as error:
        print(json.dumps({"status": "INVALID_ENTRY_EVIDENCE", "error_type": type(error).__name__}))
        return 2
    print(json.dumps(result, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
