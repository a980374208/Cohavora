"""Review product-owned DXGI budgets; standalone diagnostics have no release credit."""
import argparse
import hashlib
import json
from datetime import datetime
from pathlib import Path

SCOPE = "calling_process_all_enumerated_hardware_adapters_node0"
ALL_NODES_SCOPE = "calling_process_all_enumerated_hardware_adapters_all_nodes"
MAXIMUM_OBSERVER_GAP_MS = 2000
FIELDS = ("budget_bytes", "current_usage_bytes", "available_for_reservation_bytes", "current_reservation_bytes")


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def stamp(row):
    return row["utc_ms"] if "utc_ms" in row else datetime.fromisoformat(row["utc"].replace("Z", "+00:00")).timestamp() * 1000


def validate_snapshot(snapshot, expected_pid):
    if (type(expected_pid) is not int or expected_pid <= 0 or type(snapshot.get("pid")) is not int
            or snapshot.get("collector") != "dxgi_in_process_budget" or snapshot.get("pid") != expected_pid
            or snapshot.get("scope") not in (SCOPE, ALL_NODES_SCOPE) or snapshot.get("availability") != "VALID"
            or snapshot.get("initialization_hresult") != 0):
        raise ValueError("gpu_budget_owner_or_availability")
    adapters = snapshot.get("adapters")
    if snapshot["scope"] == ALL_NODES_SCOPE and snapshot.get("topology_current") is not True:
        raise ValueError("gpu_budget_adapter_topology_stale")
    if not isinstance(adapters, list) or not adapters:
        raise ValueError("gpu_budget_adapters_missing")
    ordinals = set()
    topology = []
    for adapter in adapters:
        ordinal = adapter.get("adapter_ordinal")
        if type(ordinal) is not int or ordinal < 0 or ordinal in ordinals:
            raise ValueError("gpu_budget_adapter_identity")
        ordinals.add(ordinal)
        if snapshot["scope"] == ALL_NODES_SCOPE:
            count = adapter.get("node_count")
            nodes = adapter.get("nodes")
            if (type(count) is not int or count < 1 or adapter.get("node_count_hresult") != 0
                    or not isinstance(nodes, list) or len(nodes) != count
                    or [n.get("node_index") for n in nodes] != list(range(count))
                    or any(type(n.get("node_index")) is not int for n in nodes)):
                raise ValueError("gpu_budget_node_topology_unavailable_or_incomplete")
            topology.append((ordinal, count))
        else:
            if type(adapter.get("node_index")) is not int or adapter["node_index"] != 0:
                raise ValueError("gpu_budget_adapter_identity")
            nodes = [adapter]
            topology.append(ordinal)
        for node in nodes:
            for group in ("local", "nonlocal"):
                values = node.get(group, {})
                if values.get("availability") != "VALID" or values.get("hresult") != 0:
                    raise ValueError("gpu_budget_group_unavailable")
                if any(type(values.get(key)) is not int or values[key] < 0 for key in FIELDS):
                    raise ValueError("gpu_budget_missing_or_invalid_number")
    if type(snapshot.get("elapsed_us")) is not int or snapshot["elapsed_us"] < 0:
        raise ValueError("gpu_budget_query_timing_missing")
    return tuple(sorted(topology))


def review_window(probes, run, pid, start, end, required_scope=None):
    if end <= start:
        raise ValueError("gpu_budget_window_invalid")
    selected = [row for row in probes if start <= stamp(row) <= end]
    times = [stamp(row) for row in selected]
    if (len(times) < 2 or times[0] > start + MAXIMUM_OBSERVER_GAP_MS
            or times[-1] < end - MAXIMUM_OBSERVER_GAP_MS
            or any(not 0 < b - a <= MAXIMUM_OBSERVER_GAP_MS for a, b in zip(times, times[1:]))):
        raise ValueError("gpu_budget_window_incomplete")
    topology = None
    measurements = {}
    for row in selected:
        if row.get("run_id") != run:
            raise ValueError("gpu_budget_run_mismatch")
        snapshot = row.get("gpu_budget", {})
        current = (snapshot.get("scope"), validate_snapshot(snapshot, pid))
        if required_scope is not None and snapshot.get("scope") != required_scope:
            raise ValueError("gpu_budget_required_node_scope_missing")
        if topology is None:
            topology = current
        if topology != current:
            raise ValueError("gpu_budget_topology_changed")
        for adapter in snapshot["adapters"]:
            ordinal = str(adapter["adapter_ordinal"])
            adapter_target = measurements.setdefault(ordinal, {})
            nodes = adapter["nodes"] if snapshot["scope"] == ALL_NODES_SCOPE else [adapter]
            for node in nodes:
                target = adapter_target.setdefault(str(node["node_index"]), {}) if snapshot["scope"] == ALL_NODES_SCOPE else adapter_target
                for group in ("local", "nonlocal"):
                    source = node[group]
                    summary = target.setdefault(group, {"samples": 0, "usage_above_budget_samples": 0, "zero_budget_samples": 0})
                    summary["samples"] += 1
                    summary["usage_above_budget_samples"] += source["budget_bytes"] > 0 and source["current_usage_bytes"] > source["budget_bytes"]
                    summary["zero_budget_samples"] += source["budget_bytes"] == 0
                    for field in FIELDS:
                        summary[field + "_min"] = min(summary.get(field + "_min", source[field]), source[field])
                        summary[field + "_max"] = max(summary.get(field + "_max", source[field]), source[field])
    return dict(samples=len(selected), observed_seconds=(times[-1] - times[0]) / 1000,
                scope=topology[0],
                max_observer_gap_ms=max(b-a for a,b in zip(times,times[1:])), adapters=measurements,
                query_elapsed_us_max=max(row["gpu_budget"]["elapsed_us"] for row in selected))


def review_cycle(probes, run, pid, actions, required_scope=None):
    """Bind budget coverage to one product lifecycle, including settled release."""
    if not actions or any(a.get("pid") != pid or a.get("run_id") != run for a in actions):
        raise ValueError("gpu_budget_product_identity_changed")
    if len({a.get("cycle_id") for a in actions}) != 1 or not actions[0].get("cycle_id"):
        raise ValueError("gpu_budget_cycle_identity_changed")
    endpoints = {}
    for action in ("join", "export"):
        observed = [a for a in actions if a.get("action") == action and a.get("phase") == "uia_observed"]
        if len(observed) != 1:
            raise ValueError("gpu_budget_cycle_endpoint_missing_or_duplicate:" + action)
        endpoints[action] = stamp(observed[0])
    return review_window(probes, run, pid, endpoints["join"], endpoints["export"], required_scope)


def review(root):
    plan = read(root / "plan.json")
    marker = read(root / "diagnostic-debugger.json")
    if not (plan.get("diagnostic_only") and plan.get("release_eligible") is False and marker.get("gpu_budget")
            and marker.get("run_id") == plan["run_id"] and marker.get("diagnostic_only") and marker.get("release_eligible") is False):
        raise ValueError("gpu_budget_diagnostic_identity_required")
    identity = read(root / "uia/product-identity.json")
    if Path(identity["executable"]).parent.name != "RelWithDebInfo":
        raise ValueError("gpu_budget_relwithdebinfo_required")
    pid = identity["pid"]
    actions = [json.loads(line) for line in (root / "uia/uia-actions.jsonl").read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    probes = [json.loads(line) for line in (root / "process-probe.jsonl").read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    if not probes or any(row.get("sequence") != i or row.get("run_id") != plan["run_id"] for i, row in enumerate(probes, 1)):
        raise ValueError("gpu_budget_probe_identity_or_sequence")
    result = dict(schema=1, run_id=plan["run_id"], verdict="GPU_BUDGET_SAMPLING_COMPLETE", product_pid=pid,
        configuration="RelWithDebInfo", diagnostic_only=True, release_eligible=False, qualification_credit=0,
        observer_max_gap_ms=MAXIMUM_OBSERVER_GAP_MS, cycles=[],
        aggregation="each enumerated adapter separately; aliases are not summed",
        unproven=["gpu_queue", "wgc_handle_ownership", "formal_8h_growth_and_release", "historical_pcm_failure_origin"])
    for cycle in range(1, plan["cycles"] + 1):
        relevant = [a for a in actions if a["cycle"] == cycle]
        result["cycles"].append(dict(cycle=cycle, **review_cycle(probes, plan["run_id"], pid, relevant)))
    result["scope"] = result["cycles"][0]["scope"]
    if any(c["scope"] != result["scope"] for c in result["cycles"]):
        raise ValueError("gpu_budget_scope_changed_between_cycles")
    if result["scope"] == SCOPE:
        result["unproven"].append("linked_adapter_nodes_other_than_0")
    result["evidence_hashes"] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (root / "plan.json", root / "process-probe.jsonl", root / "uia/product-identity.json", root / "uia/uia-actions.jsonl", root / "diagnostic-debugger.json")}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = review(args.root)
    except (ValueError, KeyError, OSError) as error:
        result = dict(verdict="GPU_BUDGET_SAMPLING_INCOMPLETE", reason=str(error),
                      diagnostic_only=True, release_eligible=False, qualification_credit=0)
    with (args.root / "gpu-budget-review.json").open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps(result))
    raise SystemExit(0 if result["verdict"] == "GPU_BUDGET_SAMPLING_COMPLETE" else 1)
