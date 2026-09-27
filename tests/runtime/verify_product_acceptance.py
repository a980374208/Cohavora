"""Fail-closed external acceptance for the real-product UIA lifecycle.

Inputs come from distinct observers. UIA proves control operations only; the
Room/SFU, receiver, capture, GPU and logging collectors must supply their own
run/cycle/operation-correlated records. This tool does not produce witnesses.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
from collections import defaultdict
from datetime import datetime, timedelta
from pathlib import Path, PurePosixPath

MIB = 1024 * 1024
REQUIRED_ACTIONS = ("join", "page", "share_start", "share_stop", "logging",
                    "leave", "export")
REQUIRED_WITNESSES = {
    "join": (("room", "room.joined"), ("sfu", "sfu.joined")),
    "share_start": (("sfu", "sfu.screen.published"),
                    ("receiver", "receiver.screen.frames"),
                    ("receiver", "receiver.screen.rtp_packets"),
                    ("receiver", "receiver.screen.decoded_frames"),
                    ("receiver", "receiver.screen.active_decoders"),
                    ("capture", "capture.backend")),
    "share_stop": (("sfu", "sfu.screen.unpublished"),
                   ("receiver", "receiver.screen.stopped")),
    "leave": (("room", "room.left"), ("sfu", "sfu.left"),
              ("telemetry", "telemetry.history.dropped_records"),
              ("diagnostic", "diagnostic.writer.dropped_records"),
              ("diagnostic", "diagnostic.writer.write_failures")),
}
LIMIT_NAMES = ("max_private_growth_mib", "max_gpu_growth_mib",
               "max_stop_private_delta_mib", "max_stop_gpu_delta_mib",
               "max_handles_growth", "max_threads_growth", "max_queue_depth",
               "max_audio_gap_ms", "max_cpu_overhead_pct", "max_frame_overhead_pct")
RESOURCE_FIELDS = ("private_bytes", "gpu_local_bytes", "gpu_nonlocal_bytes",
                   "handles", "threads", "queue_depth")
OPTIONAL_BUNDLE_MISSING = {"confirmed_crash_metadata", "historical_stability_run_detail",
                           "historical_stability_session_link", "symbol_identity",
                           "persistent_loss_summary"}


class EvidenceError(Exception):
    def __init__(self, code: str, failure: bool = False):
        super().__init__(code)
        self.code = code
        self.failure = failure


def require(condition: bool, code: str, *, failure: bool = False) -> None:
    if not condition:
        raise EvidenceError(code, failure)


def read_json(path: Path):
    require(path.is_file(), f"missing_{path.name}")
    return json.loads(path.read_text(encoding="utf-8-sig"))


def read_jsonl(path: Path):
    require(path.is_file(), f"missing_{path.name}")
    with path.open(encoding="utf-8-sig") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def instant(value: str) -> datetime:
    require(isinstance(value, str), "missing_timestamp")
    result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    require(result.tzinfo is not None, "timestamp_without_timezone")
    return result


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def validate_uia(directory: Path):
    result = read_json(directory / "uia-result.json")
    require(result.get("verdict") == "UIA_COMPLETE", "uia_not_complete")
    require(result.get("cycles_requested") == result.get("cycles_completed") == 100,
            "not_100_complete_lifecycles")
    require(result.get("minimum_seconds", 0) >= 28800 and
            (instant(result["finished_utc"]) - instant(result["started_utc"])).total_seconds() >= 28800,
            "eight_hour_window_not_met")
    run = result.get("run_id")
    require(isinstance(run, str) and re.fullmatch(r"[0-9a-f]{32}", run) is not None,
            "invalid_run_id")
    actions = read_jsonl(directory / "uia-actions.jsonl")
    groups = defaultdict(lambda: defaultdict(dict))
    for row in actions:
        require(row.get("run_id") == run and type(row.get("cycle")) is int and
                1 <= row["cycle"] <= 100, "uia_action_identity_mismatch", failure=True)
        action, phase = row.get("action"), row.get("phase")
        require(action in REQUIRED_ACTIONS or action in ("login", "process_exit"),
                "unknown_uia_action", failure=True)
        require(phase in ("requested", "uia_observed"), "invalid_uia_phase", failure=True)
        require(phase not in groups[row["cycle"]][action], "duplicate_uia_phase", failure=True)
        groups[row["cycle"]][action][phase] = row
    require(len(groups) == 100, "not_100_distinct_cycles")
    operations = {}
    product_pid = None
    previous_cycle_end = None
    for cycle in range(1, 101):
        group = groups[cycle]
        require(all(name in group for name in REQUIRED_ACTIONS), "incomplete_uia_cycle")
        previous = None
        pid = None
        for action in REQUIRED_ACTIONS:
            pair = group[action]
            require(set(pair) == {"requested", "uia_observed"}, "incomplete_uia_action")
            start, end = pair["requested"], pair["uia_observed"]
            require(start.get("operation_id") == end.get("operation_id") and
                    re.fullmatch(r"[0-9a-f]{32}", str(start.get("operation_id"))),
                    "operation_id_mismatch", failure=True)
            require(type(start.get("pid")) is int and start["pid"] == end.get("pid"),
                    "uia_pid_mismatch", failure=True)
            pid = start["pid"] if pid is None else pid
            require(start["pid"] == pid, "cycle_pid_changed", failure=True)
            began, ended = instant(start["utc"]), instant(end["utc"])
            require(began <= ended and (previous is None or previous <= began),
                    "uia_action_order_invalid", failure=True)
            previous = ended
            operations[cycle, action] = start["operation_id"]
        require(previous_cycle_end is None or
                previous_cycle_end <= instant(group["join"]["requested"]["utc"]),
                "cycles_overlap", failure=True)
        previous_cycle_end = previous
        product_pid = pid if product_pid is None else product_pid
        require(pid == product_pid, "product_pid_changed", failure=True)
        require(("process_exit" in group) == (cycle == 100),
                "process_exit_not_final", failure=True)
    exit_pair = groups[100]["process_exit"]
    require(set(exit_pair) == {"requested", "uia_observed"} and
            exit_pair["requested"].get("pid") == product_pid and
            exit_pair["uia_observed"].get("pid") == product_pid and
            instant(exit_pair["requested"]["utc"]) >= previous_cycle_end and
            instant(exit_pair["uia_observed"]["utc"]) >=
            instant(exit_pair["requested"]["utc"]),
            "process_exit_invalid", failure=True)
    operations[100, "process_exit"] = exit_pair["requested"]["operation_id"]
    require(re.fullmatch(r"[0-9a-f]{32}", str(operations[100, "process_exit"])),
            "process_exit_operation_invalid", failure=True)
    return run, operations, groups


def validate_witnesses(path: Path, run: str, operations, groups, max_audio_gap_ms: float):
    rows = read_jsonl(path)
    index = defaultdict(list)
    valid_operations = {(cycle, op) for (cycle, _), op in operations.items()}
    for row in rows:
        key = (row.get("cycle"), row.get("operation_id"), row.get("source"), row.get("event"))
        require(row.get("run_id") == run and row.get("collector") == "external" and
                key[:2] in valid_operations,
                "witness_identity_mismatch", failure=True)
        when = instant(row.get("utc"))
        cycle = row["cycle"]
        require(instant(groups[cycle]["join"]["requested"]["utc"]) <= when <=
                instant(groups[cycle]["export"]["uia_observed"]["utc"]),
                "witness_outside_cycle", failure=True)
        index[key].append(row)
    backends = {}
    for cycle in range(1, 101):
        for action, requirements in REQUIRED_WITNESSES.items():
            op = operations[cycle, action]
            next_action = {"join": "page", "share_start": "share_stop",
                           "share_stop": "logging", "leave": "export"}[action]
            began = instant(groups[cycle][action]["requested"]["utc"])
            deadline = instant(groups[cycle][next_action]["requested"]["utc"])
            for source, event in requirements:
                matches = index[cycle, op, source, event]
                require(len(matches) == 1, f"missing_or_duplicate_{event}")
                require(began <= instant(matches[0]["utc"]) <= deadline,
                        f"{event}_outside_action_window", failure=True)
                value = matches[0].get("value")
                if event == "capture.backend":
                    require(value in ("wgc", "dxgi", "gdi"), "capture_backend_not_observed")
                    backends[cycle] = value
                elif event.startswith("receiver.screen.") and event != "receiver.screen.stopped":
                    require(type(value) is int and value > 0, f"invalid_{event}")
                elif event.endswith((".dropped_records", ".write_failures")):
                    require(type(value) is int and value == 0,
                            f"nonzero_{event}", failure=True)
                else:
                    require(value is True, f"negative_{event}", failure=True)
        audio = index[cycle, operations[cycle, "join"], "receiver", "receiver.audio.window"]
        require(audio, "audio_continuity_missing")
        intervals = []
        for sample in audio:
            value = sample.get("value")
            require(isinstance(value, dict) and type(value.get("frames")) is int and
                    value["frames"] > 0 and type(value.get("max_gap_ms")) in (int, float) and
                    value["max_gap_ms"] <= max_audio_gap_ms, "audio_continuity_failed")
            intervals.append((instant(value["start_utc"]), instant(value["end_utc"])))
        intervals.sort()
        joined = instant(groups[cycle]["join"]["uia_observed"]["utc"])
        left = instant(groups[cycle]["leave"]["requested"]["utc"])
        require(intervals[0][0] <= joined + timedelta(seconds=30) and
                intervals[-1][1] >= left - timedelta(seconds=1) and
                all(a[0] <= a[1] and
                    (i == 0 or a[0] <= intervals[i-1][1] + timedelta(seconds=1))
                    for i, a in enumerate(intervals)), "audio_window_coverage_missing")
    return len(rows), backends


def limits_from(path: Path):
    limits = read_json(path)
    for name in LIMIT_NAMES:
        value = limits.get(name)
        require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
                f"invalid_limit_{name}")
    return limits


def validate_resources(path: Path, run: str, limits, groups, backends, operations):
    rows = read_jsonl(path)
    phases = defaultdict(dict)
    active = defaultdict(list)
    product_pid = groups[1]["join"]["requested"]["pid"]
    for row in rows:
        require(row.get("run_id") == run and row.get("collector") == "external" and
                type(row.get("cycle")) is int and 1 <= row["cycle"] <= 100 and
                row.get("pid") == product_pid and
                row.get("operation_id") in
                {op for (cycle, _), op in operations.items() if cycle == row.get("cycle")},
                "resource_identity_or_collector_invalid", failure=True)
        when = instant(row.get("utc"))
        for field in RESOURCE_FIELDS:
            require(type(row.get(field)) is int and row[field] >= 0,
                    f"resource_{field}_missing")
        require(type(row.get("wgc_handles")) is int and row["wgc_handles"] >= 0,
                "wgc_handle_count_missing")
        if row["wgc_handles"]:
            require(row.get("wgc_owner_pid") == product_pid, "wgc_handle_owner_unknown")
        if row.get("phase") in ("joined", "share_stopped", "room_released", "final_exit"):
            expected_action = {"joined": "join", "share_stopped": "share_stop",
                               "room_released": "leave", "final_exit": "process_exit"}[row["phase"]]
            require(row["operation_id"] == operations.get((row["cycle"], expected_action)),
                    "resource_operation_mismatch", failure=True)
            require(row["phase"] not in phases[row["cycle"]], "duplicate_resource_phase")
            phases[row["cycle"]][row["phase"]] = row
        elif row.get("phase") == "active":
            active[row["cycle"]].append(when)
        else:
            raise EvidenceError("unknown_resource_phase", failure=True)
        require(row["queue_depth"] <= limits["max_queue_depth"], "queue_depth_exceeded", failure=True)
    released_rows = []
    for cycle in range(1, 101):
        group = phases[cycle]
        require(set(group) == {"joined", "share_stopped", "room_released"} |
                ({"final_exit"} if cycle == 100 else set()),
                "resource_lifecycle_incomplete")
        before, after, released = (group[name] for name in
                                   ("joined", "share_stopped", "room_released"))
        join_end = instant(groups[cycle]["join"]["uia_observed"]["utc"])
        share_start = instant(groups[cycle]["share_start"]["requested"]["utc"])
        stop_end = instant(groups[cycle]["share_stop"]["uia_observed"]["utc"])
        log_start = instant(groups[cycle]["logging"]["requested"]["utc"])
        leave_end = instant(groups[cycle]["leave"]["uia_observed"]["utc"])
        export_end = instant(groups[cycle]["export"]["uia_observed"]["utc"])
        require(join_end <= instant(before["utc"]) <= share_start and
                stop_end <= instant(after["utc"]) <= log_start and
                leave_end <= instant(released["utc"]) <= export_end,
                "resource_phase_not_correlated")
        samples = sorted(active[cycle])
        require(samples and samples[0] <= join_end + timedelta(seconds=10) and
                samples[-1] >= leave_end - timedelta(seconds=10) and
                all(b - a <= timedelta(seconds=10) for a, b in zip(samples, samples[1:])),
                "continuous_resource_sampling_missing")
        require(before.get("layout") == after.get("layout") and before.get("layout") not in (None, "unknown"),
                "stop_layout_not_comparable")
        require(after["private_bytes"] - before["private_bytes"] <=
                limits["max_stop_private_delta_mib"] * MIB and
                after["gpu_local_bytes"] - before["gpu_local_bytes"] <=
                limits["max_stop_gpu_delta_mib"] * MIB,
                "share_stop_growth_exceeded", failure=True)
        require(released.get("process_alive") is True and
                released["wgc_handles"] == 0 and after["wgc_handles"] == 0,
                "room_or_wgc_release_not_confirmed", failure=True)
        if backends[cycle] == "wgc":
            require(any(row.get("cycle") == cycle and row.get("phase") == "active" and
                        row["wgc_handles"] > 0 for row in rows),
                    "wgc_active_handle_not_observed")
        released_rows.append(released)
    final = phases[100]["final_exit"]
    require(instant(final["utc"]) >=
            instant(groups[100]["process_exit"]["uia_observed"]["utc"]) and
            final.get("process_alive") is False and final["wgc_handles"] == 0 and
            all(final[field] == 0 for field in RESOURCE_FIELDS),
            "final_process_release_not_confirmed", failure=True)
    for field, limit in (("private_bytes", limits["max_private_growth_mib"] * MIB),
                         ("gpu_local_bytes", limits["max_gpu_growth_mib"] * MIB),
                         ("handles", limits["max_handles_growth"]),
                         ("threads", limits["max_threads_growth"])):
        growth = (statistics.median(row[field] for row in released_rows[-10:]) -
                  statistics.median(row[field] for row in released_rows[:10]))
        require(growth <= limit, f"{field}_growth_exceeded", failure=True)
    return len(rows)


def validate_bundle(path: Path):
    manifest_path = path / "manifest.json"
    manifest = read_json(manifest_path)
    require(manifest.get("schema") == "cohavora-diagnostic-bundle" and
            manifest.get("schema_version") == 1 and manifest.get("session_complete") is True,
            "support_bundle_incomplete")
    files = manifest.get("files")
    require(isinstance(files, list) and files, "support_manifest_empty")
    seen = set()
    for entry in files:
        name = entry.get("path")
        require(isinstance(name, str) and name and "\\" not in name and
                not PurePosixPath(name).is_absolute() and
                all(part not in ("", ".", "..") for part in name.split("/")) and
                name not in seen, "unsafe_bundle_path", failure=True)
        seen.add(name)
        artifact = path / name
        require(artifact.resolve().is_relative_to(path.resolve()) and
                not any(parent.is_symlink() for parent in
                        (artifact, *artifact.parents) if parent != path and
                        path in parent.parents),
                "bundle_link_escape", failure=True)
        require(artifact.is_file() and artifact.stat().st_size == entry.get("size_bytes") and
                digest(artifact) == entry.get("sha256"), "bundle_hash_mismatch", failure=True)
    session = manifest.get("anonymous_session_id")
    require(isinstance(session, str) and re.fullmatch(r"[0-9a-f]{32}", session),
            "support_session_id_invalid")
    required = {"build.json", "environment.json", "diagnostics-health.json",
                f"sessions/{session}/events.jsonl", f"sessions/{session}/stability.json",
                f"sessions/{session}/telemetry/checkpoint.json",
                f"sessions/{session}/telemetry/metrics.jsonl"}
    require(seen == required, "support_bundle_files_incomplete")
    missing = manifest.get("missing")
    require(isinstance(missing, list) and all(isinstance(item, str) for item in missing),
            "bundle_missing_list_invalid")
    require(set(missing) <= OPTIONAL_BUNDLE_MISSING, "bundle_required_evidence_missing")
    health = read_json(path / "diagnostics-health.json")
    require(all(health.get(key) == 0 for key in ("timeline_omitted", "timeline_invalid_lines",
                                               "timeline_skipped_files", "checkpoint_missing_revisions")),
            "diagnostic_gap_unexplained")
    losses = health.get("persistent_losses", {})
    if losses.get("availability") == "valid":
        require(losses.get("dropped_records") == 0,
                "persistent_losses_nonzero", failure=True)
    else:
        require(losses.get("availability") == "missing" and
                "persistent_loss_summary" in missing,
                "persistent_loss_summary_incomplete")
    return digest(manifest_path), missing


def validate_logging(uia: Path, run: str, review_path: Path, perf_path: Path,
                     limits, gaps, operations):
    hashes = []
    optional_missing = set()
    for cycle in range(1, 101):
        export = uia / f"export-{cycle:04d}"
        bundles = list(export.glob("cohavora-diagnostic-bundle-*"))
        require(len(bundles) == 1 and bundles[0].is_dir(), "support_export_missing")
        hashed, missing = validate_bundle(bundles[0])
        hashes.append(hashed)
        optional_missing.update(missing)
    review = read_json(review_path)
    require(review.get("run_id") == run and review.get("status") == "PASS" and
            review.get("reviewer") and review.get("manifest_sha256") == hashes,
            "independent_review_missing")
    require(review.get("accepted_sequence_gaps") == gaps,
            "diagnostic_sequence_gaps_not_reviewed")
    pairs = read_json(perf_path)
    require(isinstance(pairs, list) and len(pairs) == 100, "log_performance_pairs_missing")
    windows = read_jsonl(uia / "uia-log-windows.jsonl")
    require(len(windows) == 100, "log_uia_windows_missing")
    for cycle, pair in enumerate(pairs, 1):
        window = windows[cycle - 1]
        require(window.get("run_id") == run and window.get("cycle") == cycle and
                window.get("operation_id") == operations[cycle, "logging"],
                "log_uia_window_identity_mismatch", failure=True)
        off_start, off_end = instant(window["off_start_utc"]), instant(window["off_end_utc"])
        on_start, on_end = instant(window["on_start_utc"]), instant(window["on_end_utc"])
        require(off_start < off_end <= on_start < on_end and
                min((off_end - off_start).total_seconds(),
                    (on_end - on_start).total_seconds()) >= 30,
                "log_uia_window_duration_invalid")
        require(pair.get("run_id") == run and pair.get("cycle") == cycle and
                pair.get("operation_id") == operations[cycle, "logging"] and
                pair.get("collector") == "external" and pair.get("load_signature") and
                pair.get("on", {}).get("load_signature") == pair.get("off", {}).get("load_signature") ==
                pair["load_signature"], "log_load_not_paired")
        on, off = pair["on"], pair["off"]
        require(off_start <= instant(off["start_utc"]) < instant(off["end_utc"]) <= off_end and
                on_start <= instant(on["start_utc"]) < instant(on["end_utc"]) <= on_end,
                "log_performance_outside_uia_window", failure=True)
        require(all(type(side.get(name)) in (int, float) and side[name] > 0
                    for side in (on, off) for name in ("duration_s", "cpu_pct", "p95_video_ms")) and
                min(on["duration_s"], off["duration_s"]) >= 30,
                "log_performance_sample_missing")
        require(on["cpu_pct"] - off["cpu_pct"] <= limits["max_cpu_overhead_pct"] and
                100 * (on["p95_video_ms"] / off["p95_video_ms"] - 1) <=
                limits["max_frame_overhead_pct"], "log_performance_overhead_exceeded", failure=True)
    return optional_missing


def validate_segments(root: Path):
    require(root.is_dir(), "diagnostic_root_missing")
    runs = list(root.rglob("run-*"))
    require(runs, "diagnostic_segments_missing")
    count = 0
    gaps = []
    total_bytes = 0
    for run in runs:
        if not run.is_dir():
            continue
        segments = sorted(run.glob("segment-*.jsonl"))
        if not segments:
            continue
        indexes = [int(item.stem.split("-")[1]) for item in segments]
        require(indexes == list(range(indexes[0], indexes[0] + len(indexes))),
                "segment_index_gap")
        require(all(not item.is_symlink() and item.stat().st_size <= 10 * MIB
                    for item in segments), "segment_size_or_link_invalid", failure=True)
        total_bytes += sum(item.stat().st_size for item in segments)
        require(total_bytes <= 100 * MIB, "diagnostic_quota_exceeded", failure=True)
        previous = 0
        for segment in segments:
            with segment.open(encoding="utf-8") as stream:
                for line in stream:
                    record = json.loads(line)
                    sequence = record.get("event_sequence")
                    require(type(sequence) is int and sequence > previous,
                            "diagnostic_sequence_invalid", failure=True)
                    if sequence > previous + 1:
                        gaps.append({"run": run.name, "first_missing": previous + 1,
                                     "last_missing": sequence - 1, "reason": "unattributed"})
                    previous = sequence
        count += 1
    require(count == 1, "diagnostic_run_coverage_invalid")
    return count, gaps


def verify(args):
    report = {"verdict": "INCONCLUSIVE", "reason": "unknown", "limitations": [
        "slow_disk_not_verified", "power_loss_not_verified", "real_crash_recovery_not_verified",
        "release_symbol_replay_not_verified"]}
    try:
        run, operations, groups = validate_uia(args.uia)
        report["run_id"] = run
        limits = limits_from(args.limits)
        report["witness_rows"], backends = validate_witnesses(
            args.witnesses, run, operations, groups, limits["max_audio_gap_ms"])
        report["resource_rows"] = validate_resources(
            args.resources, run, limits, groups, backends, operations)
        report["diagnostic_runs"], gaps = validate_segments(args.diagnostics)
        report["sequence_gaps"] = len(gaps)
        report["optional_bundle_missing"] = sorted(validate_logging(
            args.uia, run, args.review, args.performance, limits, gaps, operations))
        report.update(verdict="PASS", reason="scoped_eight_hour_product_acceptance")
    except EvidenceError as error:
        report.update(verdict="FAIL" if error.failure else "INCONCLUSIVE", reason=error.code)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
        report.update(verdict="INCONCLUSIVE", reason="evidence_unreadable_or_invalid")
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["verdict"] == "PASS" else 1 if report["verdict"] == "FAIL" else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("uia", "witnesses", "resources", "diagnostics", "review", "performance", "limits", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    return verify(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
