"""Derive paired windows from actual CPU counters and render histogram deltas.

The native histogram uses 1 ms upper bounds and an explicit >=1000 ms overflow
bucket. Cumulative percentiles are deliberately not used as window samples.
"""
import math
from datetime import datetime, timezone


def stamp(row):
    return row["utc_ms"] / 1000 if "utc_ms" in row else datetime.fromisoformat(
        row["utc"].replace("Z", "+00:00")).timestamp()


def window_p95(first, last):
    before = first["render_interval_histogram_ms"]
    after = last["render_interval_histogram_ms"]
    if any(not str(k).isdigit() or not 0 <= int(k) <= 1000
           or type(v) is not int or v < 0 for h in (before, after) for k, v in h.items()):
        raise ValueError("invalid_render_histogram")
    delta = {int(k): after.get(k, 0) - before.get(k, 0) for k in set(before) | set(after)}
    if any(v < 0 for v in delta.values()):
        raise ValueError("render_binding_or_counter_reset")
    count = sum(delta.values())
    if count < 100:
        raise ValueError("render_interval_window_insufficient")
    rank, total = math.ceil(count * .95), 0
    for bucket, value in sorted(delta.items()):
        total += value
        if total >= rank:
            if bucket in (0, 1000):
                raise ValueError("render_p95_unbounded")
            return dict(samples=count, lower_ms=bucket-1, upper_ms=bucket)
    raise ValueError("render_interval_window_empty")


def paired_window(window, probes, resources):
    result = {}
    if not window.get("production_and_persistence"):
        raise ValueError("retention_only_pair_is_not_full_logging_comparison")
    for side in ("off", "on"):
        begin = datetime.fromisoformat(window[side + "_start_utc"].replace("Z", "+00:00")).timestamp()
        end = datetime.fromisoformat(window[side + "_end_utc"].replace("Z", "+00:00")).timestamp()
        samples = [p for p in probes if begin <= p.get("source_utc_ms", 0) / 1000 <= stamp(p) <= end
                   and stamp(p) - p["source_utc_ms"] / 1000 <= 2]
        cpu = [r for r in resources if begin <= stamp(r) <= end and r.get("cpu_seconds") is not None]
        if len(samples) < 30 or len(cpu) < 30:
            raise ValueError("performance_window_sample_coverage")
        start = max(samples[0]["source_utc_ms"] / 1000, stamp(cpu[0]))
        finish = min(samples[-1]["source_utc_ms"] / 1000, stamp(cpu[-1]))
        if finish - start < 30:
            raise ValueError("performance_window_less_than_30_seconds")
        routes = set()
        for row in samples:
            if row["diagnostic"].get("benchmark_production_paused") != (side == "off") or \
                    row["diagnostic"]["retention_enabled"] != (side == "on"):
                raise ValueError("logging_production_or_persistence_state_not_confirmed")
            values = {m["key"]: m["value"] for m in row["metrics"] if m["availability"] == "VALID"}
            route = tuple(values.get(k) for k in ("video.pipeline.inbound_rtp_streams",
                "video.pipeline.active_decode_streams", "network.rtp.inbound.streams"))
            if any(type(v) is not int or v <= 0 for v in route):
                raise ValueError("performance_active_routes_unavailable")
            routes.add(route)
        if len(routes) != 1:
            raise ValueError("performance_active_routes_changed")
        p95 = window_p95(samples[0], samples[-1])
        elapsed = stamp(cpu[-1]) - stamp(cpu[0])
        processors = cpu[0]["logical_processors"]
        if not processors or any(r["logical_processors"] != processors for r in cpu):
            raise ValueError("cpu_processor_count_changed")
        cpu_pct = 100 * (cpu[-1]["cpu_seconds"] - cpu[0]["cpu_seconds"]) / elapsed / processors
        if cpu_pct < 0:
            raise ValueError("cpu_counter_regressed")
        result[side] = dict(duration_s=finish-start, cpu_pct=cpu_pct, p95=p95,
            actual_routes=list(next(iter(routes))),
            start_utc=datetime.fromtimestamp(start, timezone.utc).isoformat(),
            end_utc=datetime.fromtimestamp(finish, timezone.utc).isoformat())
    if result["off"]["actual_routes"] != result["on"]["actual_routes"]:
        raise ValueError("performance_load_not_paired")
    result["cpu_increase_percentage_points"] = result["on"]["cpu_pct"] - result["off"]["cpu_pct"]
    # Use the worst case permitted by quantization, not equality of buckets.
    result["p95_increase_upper_bound_percent"] = 100 * (
        result["on"]["p95"]["upper_ms"] / result["off"]["p95"]["lower_ms"] - 1)
    return result
