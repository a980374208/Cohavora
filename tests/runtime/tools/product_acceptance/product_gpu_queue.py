"""WDDM packet-counter observations; these do not invent hardware queue bounds."""
from analyze_product_gpu_budget import MAXIMUM_OBSERVER_GAP_MS, stamp

SCOPE = "process_all_enumerated_hardware_adapters_all_scheduler_nodes"
UINT32_MAX = (1 << 32) - 1


def uint32(value):
    return type(value) is int and 0 <= value <= UINT32_MAX


def validate_snapshot(snapshot, pid):
    if (type(pid) is not int or pid <= 0 or type(snapshot.get("pid")) is not int
            or snapshot.get("pid") != pid or snapshot.get("scope") != SCOPE
            or snapshot.get("collector") != "wddm_process_packet_statistics"
            or snapshot.get("availability") != "VALID" or snapshot.get("initialization_hresult") != 0
            or snapshot.get("query_available") is not True or snapshot.get("process_handle_available") is not True
            or snapshot.get("process_error") != 0 or snapshot.get("topology_current") is not True):
        raise ValueError("gpu_queue_owner_or_api_unavailable")
    adapters = snapshot.get("adapters")
    if not isinstance(adapters, list) or not adapters:
        raise ValueError("gpu_queue_adapters_missing")
    topology, ordinals = [], set()
    for adapter in adapters:
        ordinal, count, nodes = (adapter.get(k) for k in ("adapter_ordinal", "scheduler_node_count", "nodes"))
        if (type(ordinal) is not int or ordinal < 0 or ordinal in ordinals or type(count) is not int or count < 1
                or adapter.get("adapter_ntstatus") != 0 or not isinstance(nodes, list) or len(nodes) != count
                or [node.get("node_index") for node in nodes] != list(range(count))
                or any(type(node.get("node_index")) is not int for node in nodes)):
            raise ValueError("gpu_queue_scheduler_topology_incomplete")
        ordinals.add(ordinal)
        topology.append((ordinal, count))
        for node in nodes:
            if (node.get("availability") != "VALID" or node.get("ntstatus") != 0
                    or type(node.get("running_time_raw")) is not int or node["running_time_raw"] < 0):
                raise ValueError("gpu_queue_node_query_unavailable")
            for group, count in (("queue_packets", 8), ("dma_packets", 4)):
                packets = node.get(group)
                if (not isinstance(packets, list) or [p.get("type") for p in packets] != list(range(count))
                        or any(type(p.get("type")) is not int for p in packets)):
                    raise ValueError("gpu_queue_packet_types_missing")
                for packet in packets:
                    fields = ("submitted", "completed") if group == "queue_packets" else ("submitted", "completed", "preempted", "faulted")
                    if any(not uint32(packet.get(key)) for key in fields):
                        raise ValueError("gpu_queue_packet_value_unknown")
                    if group == "queue_packets":
                        outstanding = packet.get("submitted_minus_completed")
                        if (not uint32(outstanding) or packet["completed"] > packet["submitted"]
                                or outstanding != packet["submitted"] - packet["completed"]):
                            raise ValueError("gpu_queue_counter_order_unknown")
    if type(snapshot.get("elapsed_us")) is not int or snapshot["elapsed_us"] < 0:
        raise ValueError("gpu_queue_query_timing_unknown")
    return tuple(sorted(topology))


def review_cycle(probes, run, pid, actions, active_only=False):
    """Review actual scheduler samples, preserving the unresolved bounds gate."""
    if (not actions or any(a.get("run_id") != run or a.get("pid") != pid for a in actions)
            or len({a.get("cycle_id") for a in actions}) != 1 or not actions[0].get("cycle_id")):
        raise ValueError("gpu_queue_cycle_identity_changed")
    endpoints = {}
    for action, phase in (("join", "uia_observed"), ("leave", "requested"), ("export", "uia_observed")):
        matches = [a for a in actions if a.get("action") == action and a.get("phase") == phase]
        if len(matches) != 1:
            raise ValueError("gpu_queue_cycle_endpoint_missing_or_duplicate")
        endpoints[action] = stamp(matches[0])
    start, active_end, end = (endpoints[k] for k in ("join", "leave", "export"))
    if not start < active_end < end:
        raise ValueError("gpu_queue_window_invalid")
    if active_only:
        end = active_end
    rows = [p for p in probes if start <= stamp(p) <= end]
    times = [stamp(p) for p in rows]
    if (len(times) < 2 or times[0] > start + MAXIMUM_OBSERVER_GAP_MS
            or times[-1] < end - MAXIMUM_OBSERVER_GAP_MS
            or any(not 0 < b - a <= MAXIMUM_OBSERVER_GAP_MS for a, b in zip(times, times[1:]))):
        raise ValueError("gpu_queue_window_incomplete")
    topology, summaries, previous = None, {}, {}
    active_progress = False
    for row in rows:
        if row.get("run_id") != run:
            raise ValueError("gpu_queue_run_mismatch")
        snapshot = row.get("gpu_queue", {})
        current = validate_snapshot(snapshot, pid)
        if topology is not None and current != topology:
            raise ValueError("gpu_queue_topology_changed")
        topology = current
        for adapter in snapshot["adapters"]:
            adapter_summary = summaries.setdefault(str(adapter["adapter_ordinal"]), {})
            for node in adapter["nodes"]:
                node_summary = adapter_summary.setdefault(str(node["node_index"]), dict(queue_packets={}, dma_packets={},
                    maximum_observed_submitted_minus_completed=0))
                for group in ("queue_packets", "dma_packets"):
                    for packet in node[group]:
                        key = (adapter["adapter_ordinal"], node["node_index"], group, packet["type"])
                        fields = ("submitted", "completed") if group == "queue_packets" else ("submitted", "completed", "preempted", "faulted")
                        before = previous.get(key)
                        if before is not None:
                            if any(packet[field] < before[field] for field in fields):
                                raise ValueError("gpu_queue_counters_reset_or_wrapped")
                            if stamp(row) <= active_end and packet["submitted"] > before["submitted"]:
                                active_progress = True
                        previous[key] = packet
                        summary = node_summary[group].setdefault(str(packet["type"]), {})
                        for field in fields:
                            summary.setdefault(field + "_first", packet[field])
                            summary[field + "_last"] = packet[field]
                            summary[field + "_delta"] = packet[field] - summary[field + "_first"]
                        if group == "queue_packets":
                            summary["maximum_observed_submitted_minus_completed"] = max(
                                summary.get("maximum_observed_submitted_minus_completed", 0), packet["submitted_minus_completed"])
                observed = sum(p["submitted_minus_completed"] for p in node["queue_packets"])
                node_summary["maximum_observed_submitted_minus_completed"] = max(
                    node_summary["maximum_observed_submitted_minus_completed"], observed)
    if not active_progress:
        raise ValueError("gpu_queue_active_packet_progress_unproven")
    return dict(scope=SCOPE, window="active_only" if active_only else "full_cycle", samples=len(rows), observed_seconds=(times[-1] - times[0]) / 1000,
        max_observer_gap_ms=max(b - a for a, b in zip(times, times[1:])), adapters=summaries,
        query_elapsed_us_max=max(p["gpu_queue"]["elapsed_us"] for p in rows), active_packet_progress_observed=True,
        queue_bounds="NOT_FROZEN", release_eligible=False,
        limitations=["sampled WDDM packet arithmetic is not a hardware queue depth or unsampled peak",
                     "adapter aliases are never summed", "zero counters alone do not prove active workload"])


def review_diagnostic_bounds(probes, run, pid, actions, observation, policy):
    """Apply pre-run approximate policy only to diagnostic packet observations."""
    names = ("maximum_observed_pending_packets_per_scheduler_node",
             "maximum_room_release_pending_packets_per_scheduler_node", "maximum_dma_faults_delta")
    if (policy.get("scope") != SCOPE or policy.get("status") != "PROVISIONAL_USER_REQUESTED_DIAGNOSTIC"
            or policy.get("diagnostic_only") is not True or policy.get("release_eligible") is not False
            or any(not uint32(policy.get(name)) for name in names)):
        raise ValueError("gpu_queue_provisional_policy_invalid")
    peak = max(n["maximum_observed_submitted_minus_completed"] for a in observation["adapters"].values() for n in a.values())
    # Compare deltas within each adapter/node, never sum aliases or preemptions.
    fault_delta = max(sum(p["faulted_delta"] for p in n["dma_packets"].values())
                      for a in observation["adapters"].values() for n in a.values())
    endpoints = {}
    for action, phase in (("leave", "uia_observed"), ("export", "requested")):
        matches = [a for a in actions if a.get("action") == action and a.get("phase") == phase]
        if len(matches) != 1:
            raise ValueError("gpu_queue_release_endpoint_missing_or_duplicate")
        endpoints[action] = stamp(matches[0])
    start, end = endpoints["leave"] + 5000, endpoints["export"]
    rows = [p for p in probes if start <= stamp(p) < end]
    times = [stamp(p) for p in rows]
    if (end <= start or len(times) < 2 or times[0] > start + MAXIMUM_OBSERVER_GAP_MS
            or times[-1] < end - MAXIMUM_OBSERVER_GAP_MS
            or any(not 0 < b - a <= MAXIMUM_OBSERVER_GAP_MS for a, b in zip(times, times[1:]))):
        raise ValueError("gpu_queue_release_window_incomplete")
    release_peak = 0
    for row in rows:
        if row.get("run_id") != run:
            raise ValueError("gpu_queue_release_run_mismatch")
        validate_snapshot(row.get("gpu_queue", {}), pid)
        release_peak = max(release_peak, max(sum(p["submitted_minus_completed"] for p in node["queue_packets"])
            for adapter in row["gpu_queue"]["adapters"] for node in adapter["nodes"]))
    checks = dict(sampled_pending=peak <= policy[names[0]], release_pending=release_peak <= policy[names[1]],
                  dma_faults=fault_delta <= policy[names[2]])
    return dict(passed=all(checks.values()), checks=checks, policy=policy,
        maximum_observed_pending_per_node=peak, maximum_release_pending_per_node=release_peak,
        maximum_dma_faults_delta_per_node=fault_delta, release_window_samples=len(rows),
        diagnostic_only=True, release_eligible=False, qualification_credit=0)
