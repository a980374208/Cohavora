"""Observe microphone energy, or test acoustic coupling to a bounded 997 Hz tone.

This never records PCM, changes Windows defaults/levels, disables product APM/DTX,
or supplies product acceptance credit. Energy/tone thresholds are fixed before
capture. A later product comparison needs this witness throughout the run.
"""
import argparse
import hashlib
import json
import subprocess
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKSPACE = HERE.parents[4]
CONDITIONS = {
    "schema": 1,
    "minimum_observation_seconds": 30,
    "settle_seconds": 2,
    "minimum_ac_rms": 0.005,
    "minimum_tone_fraction": 0.30,
    "maximum_peak": 0.95,
    "required_non_silent_block_fraction": 1.0,
    "required_tone_block_fraction": 1.0,
    "minimum_frame_coverage": 0.98,
    "maximum_packet_gap_ms": 200,
    "tone_hz": 997,
    "tone_peak": 0.025,
    "diagnostic_only": True,
    "release_eligible": False,
    "qualification_credit": 0,
    "pcm_persisted": False,
}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def save(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def summarize(capture_rows, tone_rows, capture_exit, tone_exit, run_id, mode="baseline"):
    starts = [r for r in capture_rows if r["event"] == "collector.started"]
    stops = [r for r in capture_rows if r["event"] == "collector.stopped"]
    intact = len(starts) == len(stops) == 1 and capture_exit == 0
    if not intact:
        return {"verdict": "COLLECTION_FAILED", "source_confirmed": False, "capture_exit": capture_exit}
    start = starts[0]
    sequence_ok = all(r.get("sequence") == i + 1 and r.get("run_id") == run_id for i, r in enumerate(capture_rows))
    all_samples = [r for r in capture_rows if r["event"] == "microphone.sample"]
    earliest = start["utc_ms"] + CONDITIONS["settle_seconds"] * 1000
    latest = stops[0]["utc_ms"]
    tone_ok = mode != "coupled"
    if tone_rows:
        tone_starts = [r for r in tone_rows if r["event"] == "collector.started"]
        tone_stops = [r for r in tone_rows if r["event"] == "collector.stopped"]
        tone_ok = (tone_exit == 0 and len(tone_starts) == len(tone_stops) == 1
                   and all(r.get("sequence") == i + 1 and r.get("run_id") == run_id for i, r in enumerate(tone_rows))
                   and not any(r.get("endpoint_muted") or r.get("endpoint_volume", 1) <= 0 for r in tone_rows))
        if tone_ok:
            earliest = max(earliest, tone_starts[0]["utc_ms"] + CONDITIONS["settle_seconds"] * 1000)
            latest = min(latest, tone_stops[0]["utc_ms"] - CONDITIONS["settle_seconds"] * 1000)
    selected = [r for r in all_samples if r["window_seconds"] >= .9
                and r["utc_ms"] - r["window_seconds"] * 1000 >= earliest and r["utc_ms"] <= latest]
    baseline_rows = [r for r in all_samples if r["utc_ms"] <= earliest]
    anchor = baseline_rows[-1] if baseline_rows else {"discontinuities": 0, "timestamp_errors": 0, "missing_device_frames": 0}
    duration = sum(r["window_seconds"] for r in selected)
    blocks = sum(r["blocks_100ms"] for r in selected)
    non_silent = sum(r["non_silent_blocks"] for r in selected)
    matching = sum(r["tone_matched_blocks"] for r in selected)
    frames = sum(r["window_frames"] for r in selected)
    rms = [r["rms"] for r in selected]
    frames_coverage = frames / (duration * start["sample_rate"]) if duration else 0
    non_silent_fraction = non_silent / blocks if blocks else 0
    tone_fraction = matching / blocks if blocks else 0
    counters = {key: selected[-1][key] - anchor[key] if selected else 0
                for key in ("discontinuities", "timestamp_errors", "missing_device_frames")}
    checks = {
        "collection_complete": intact and stops[0]["status"] == "COMPLETE" and sequence_ok,
        "configuration": start["configuration"] == "RelWithDebInfo",
        "conditions_match_collector": all(start.get(key) == CONDITIONS[expected]
            for key, expected in (("minimum_rms", "minimum_ac_rms"), ("minimum_tone_fraction", "minimum_tone_fraction"),
                                  ("maximum_peak", "maximum_peak"), ("tone_hz", "tone_hz"), ("tone_peak", "tone_peak"))),
        "observed_30_seconds": duration >= CONDITIONS["minimum_observation_seconds"],
        "endpoint_unmuted": not start["endpoint_muted"] and start["endpoint_volume"] > 0
            and all(not r["endpoint_muted"] and r["endpoint_volume"] > 0 for r in selected),
        "default_endpoint_unchanged": all(r["default_endpoint_unchanged"] for r in all_samples),
        "frame_coverage": frames_coverage >= CONDITIONS["minimum_frame_coverage"],
        "packet_continuity": bool(selected) and max(r["max_packet_gap_ms"] for r in selected) <= CONDITIONS["maximum_packet_gap_ms"],
        "device_continuity": all(v == 0 for v in counters.values()),
        "continuous_non_silent": non_silent_fraction >= CONDITIONS["required_non_silent_block_fraction"],
        "not_clipped": bool(selected) and max(r["peak"] for r in selected) < CONDITIONS["maximum_peak"],
    }
    if mode in ("coupled", "external-tone"):
        checks["known_tone_present"] = tone_ok and tone_fraction >= CONDITIONS["required_tone_block_fraction"]
    continuous = all(checks.values())
    confirmed = continuous and mode != "baseline"
    return {
        "verdict": "TONE_SOURCE_FAILED" if not tone_ok else "SOURCE_CONFIRMED" if confirmed else "NON_SILENT_INPUT_OBSERVED" if continuous else "SOURCE_NOT_CONFIRMED",
        "source_confirmed": confirmed,
        "continuous_input_observed": continuous,
        "scope": "raw shared WASAPI input; product APM output and negotiated DTX unobserved",
        "endpoint_sha256": start["endpoint_sha256"], "role": start["role"],
        "observed_seconds": duration, "observed_blocks": blocks, "frame_coverage": frames_coverage,
        "non_silent_fraction": non_silent_fraction, "tone_matched_fraction": tone_fraction,
        "rms_min": min(rms, default=0), "rms_max": max(rms, default=0),
        "packet_gap_max_ms": max((r["max_packet_gap_ms"] for r in selected), default=0),
        "device_counters": counters, "checks": checks,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("baseline", "coupled", "external-tone"), default="baseline")
    parser.add_argument("--seconds", type=int, default=45)
    parser.add_argument("--capture-endpoint", default="")
    parser.add_argument("--render-endpoint", default="")
    args = parser.parse_args()
    exe = args.executable.resolve(strict=True)
    if exe.parent.name != "RelWithDebInfo" or args.seconds < 45 or args.seconds > 30000:
        parser.error("RelWithDebInfo and at least 45 seconds required")
    args.output.mkdir(parents=True, exist_ok=False)
    run = uuid.uuid4().hex
    sources = [Path(__file__), HERE / "CMakeLists.txt", WORKSPACE / "tests/runtime/probes/product_microphone_input.cpp",
               WORKSPACE / "tests/runtime/probes/microphone_signal_metrics.h", exe, exe.with_suffix(".pdb")]
    frozen = {str(p): digest(p) for p in sources}
    save(args.output / "conditions.json", CONDITIONS)
    save(args.output / "inputs.json", {"run_id": run, "mode": args.mode, "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=WORKSPACE, text=True).strip(),
         "created_utc": datetime.now(timezone.utc).isoformat(), "configuration": "RelWithDebInfo", "seconds": args.seconds,
         "capture_selection": "explicit" if args.capture_endpoint else "product_default_eConsole_fallback_eMultimedia",
         "render_selection": "explicit" if args.render_endpoint else "default_eConsole", "source_hashes": frozen,
         "conditions_sha256": digest(args.output / "conditions.json"), "diagnostic_only": True, "release_eligible": False})
    capture_path = args.output / "microphone.jsonl"
    tone_path = args.output / "tone.jsonl"
    capture_command = [str(exe), "capture", run, str(capture_path), str(args.seconds)]
    if args.capture_endpoint:
        capture_command.append(args.capture_endpoint)
    capture = subprocess.Popen(capture_command)
    tone = None
    try:
        if args.mode == "coupled":
            time.sleep(3)
            tone_command = [str(exe), "tone", run, str(tone_path), str(args.seconds - 6)]
            if args.render_endpoint:
                tone_command.append(args.render_endpoint)
            if capture.poll() is None:
                tone = subprocess.Popen(tone_command)
        capture_exit = capture.wait(timeout=args.seconds + 15)
        tone_exit = tone.wait(timeout=10) if tone else None
        review = summarize(rows(capture_path), rows(tone_path) if tone_path.exists() else [], capture_exit, tone_exit, run, args.mode)
        if args.mode == "coupled" and tone is None:
            review["verdict"] = "TONE_SOURCE_FAILED"
            review["source_confirmed"] = False
        unchanged = all(digest(Path(p)) == expected for p, expected in frozen.items())
        review.update(run_id=run, mode=args.mode, diagnostic_only=True, release_eligible=False,
                      qualification_credit=0, formal_started=False, pcm_persisted=False, inputs_unchanged=unchanged,
                      evidence_hashes={str(p): digest(p) for p in (capture_path, tone_path) if p.exists()})
        if not unchanged:
            review.update(verdict="INPUTS_CHANGED", source_confirmed=False)
        save(args.output / "review.json", review)
        print(json.dumps(review, ensure_ascii=True))
        return 0 if review["source_confirmed"] else 3
    finally:
        for process in (capture, tone):
            if process and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    raise SystemExit(main())
