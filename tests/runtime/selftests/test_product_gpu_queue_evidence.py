"""Reject fabricated ownership, scheduler coverage, zeroes, resets and bounds."""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/product_acceptance"))
from product_gpu_queue import SCOPE, review_cycle, validate_snapshot, review_diagnostic_bounds
from release_product_acceptance import FULL_MEDIA_GPU_CHECKS, full_media_gpu_gaps


def snapshot():
    node = dict(node_index=0, availability="VALID", ntstatus=0, running_time_raw=0,
        queue_packets=[dict(type=i, submitted=0, completed=0, submitted_minus_completed=0) for i in range(8)],
        dma_packets=[dict(type=i, submitted=0, completed=0, preempted=0, faulted=0) for i in range(4)])
    next_node = copy.deepcopy(node)
    next_node["node_index"] = 1
    return dict(collector="wddm_process_packet_statistics", pid=123, scope=SCOPE, availability="VALID",
        initialization_hresult=0, query_available=True, process_handle_available=True, process_error=0,
        topology_current=True, elapsed_us=100,
        adapters=[dict(adapter_ordinal=0, scheduler_node_count=2, adapter_ntstatus=0, nodes=[node, next_node])])


def evidence():
    actions = [dict(run_id="run", pid=123, cycle_id="cycle", action=name, phase=phase, utc_ms=utc)
        for name, phase, utc in (("join", "uia_observed", 0), ("leave", "requested", 3000), ("export", "uia_observed", 5000))]
    probes = []
    for i in range(6):
        data = snapshot()
        packet = data["adapters"][0]["nodes"][0]["queue_packets"][0]
        packet.update(submitted=i + 3, completed=i + 2, submitted_minus_completed=1)
        probes.append(dict(run_id="run", utc_ms=i * 1000, gpu_queue=data))
    return probes, actions


def diagnostic_evidence():
    probes, actions = evidence()
    actions[-1]["utc_ms"] = 15000
    actions += [dict(run_id="run", pid=123, cycle_id="cycle", action=name, phase=phase, utc_ms=utc)
        for name, phase, utc in (("leave", "uia_observed", 3500), ("export", "requested", 14500))]
    for i in range(6, 16):
        data = copy.deepcopy(probes[-1]["gpu_queue"])
        data["adapters"][0]["nodes"][0]["queue_packets"][0].update(submitted=i + 3, completed=i + 2)
        probes.append(dict(run_id="run", utc_ms=i * 1000, gpu_queue=data))
    policy = dict(scope=SCOPE, status="PROVISIONAL_USER_REQUESTED_DIAGNOSTIC", diagnostic_only=True, release_eligible=False,
        maximum_observed_pending_packets_per_scheduler_node=64,
        maximum_room_release_pending_packets_per_scheduler_node=4, maximum_dma_faults_delta=0)
    return probes, actions, policy


class QueueEvidenceTests(unittest.TestCase):
    def test_process_and_scheduler_identity_are_required(self):
        self.assertEqual(validate_snapshot(snapshot(), 123), ((0, 2),))
        with self.assertRaises(ValueError):
            validate_snapshot(snapshot(), 124)
        data = snapshot()
        data["scope"] = "calling_process_all_enumerated_hardware_adapters_all_nodes"
        with self.assertRaises(ValueError):
            validate_snapshot(data, 123)

    def test_failed_api_and_unknown_values_cannot_become_zero(self):
        for key, value in (("availability", "UNAVAILABLE"), ("query_available", False),
                           ("process_handle_available", False), ("topology_current", False)):
            data = snapshot()
            data[key] = value
            with self.assertRaises(ValueError):
                validate_snapshot(data, 123)
        for key, value in (("submitted", None), ("completed", False), ("submitted_minus_completed", -1)):
            data = snapshot()
            data["adapters"][0]["nodes"][0]["queue_packets"][0][key] = value
            with self.assertRaises(ValueError):
                validate_snapshot(data, 123)

    def test_all_scheduler_nodes_and_packet_types_are_observed(self):
        data = snapshot()
        data["adapters"][0]["nodes"].pop()
        with self.assertRaises(ValueError):
            validate_snapshot(data, 123)
        data = snapshot()
        data["adapters"][0]["nodes"][1]["dma_packets"].pop()
        with self.assertRaises(ValueError):
            validate_snapshot(data, 123)

    def test_counter_order_cannot_be_clamped_or_wrapped(self):
        data = snapshot()
        data["adapters"][0]["nodes"][0]["queue_packets"][0].update(
            submitted=0, completed=1, submitted_minus_completed=None)
        with self.assertRaisesRegex(ValueError, "gpu_queue_counter_order_unknown"):
            validate_snapshot(data, 123)
        data["adapters"][0]["nodes"][0]["queue_packets"][0]["submitted_minus_completed"] = 0
        with self.assertRaisesRegex(ValueError, "gpu_queue_counter_order_unknown"):
            validate_snapshot(data, 123)

    def test_complete_live_window_preserves_packet_observations_only(self):
        probes, actions = evidence()
        result = review_cycle(probes, "run", 123, actions)
        self.assertTrue(result["active_packet_progress_observed"])
        self.assertEqual(result["samples"], 6)
        self.assertEqual(result["adapters"]["0"]["0"]["queue_packets"]["0"]["submitted_delta"], 5)
        self.assertEqual(result["queue_bounds"], "NOT_FROZEN")
        self.assertIs(result["release_eligible"], False)

    def test_active_only_keeps_release_unavailability_and_requires_its_own_coverage(self):
        probes,actions=evidence()
        probes[-1]["gpu_queue"]["availability"]="UNAVAILABLE"
        with self.assertRaisesRegex(ValueError,"api_unavailable"):
            review_cycle(probes,"run",123,actions)
        proof=review_cycle(probes,"run",123,actions,active_only=True)
        self.assertEqual(proof["samples"],4)
        self.assertEqual(proof["window"],"active_only")
        probes[1]["gpu_queue"]["availability"]="UNAVAILABLE"
        with self.assertRaisesRegex(ValueError,"api_unavailable"):
            review_cycle(probes,"run",123,actions,active_only=True)

    def test_zero_or_stale_positive_counters_do_not_prove_gpu_work(self):
        probes, actions = evidence()
        for row in probes:
            row["gpu_queue"] = copy.deepcopy(probes[0]["gpu_queue"])
        with self.assertRaisesRegex(ValueError, "gpu_queue_active_packet_progress_unproven"):
            review_cycle(probes, "run", 123, actions)
        for row in probes:
            row["gpu_queue"] = snapshot()
        with self.assertRaisesRegex(ValueError, "gpu_queue_active_packet_progress_unproven"):
            review_cycle(probes, "run", 123, actions)

    def test_release_only_progress_cannot_prove_joined_gpu_work(self):
        probes, actions = evidence()
        for row in probes[:4]:
            row["gpu_queue"] = snapshot()
        with self.assertRaisesRegex(ValueError, "gpu_queue_active_packet_progress_unproven"):
            review_cycle(probes, "run", 123, actions)

    def test_sparse_window_and_counter_reset_remain_failures(self):
        probes, actions = evidence()
        with self.assertRaisesRegex(ValueError, "gpu_queue_window_incomplete"):
            review_cycle([probes[0], probes[-1]], "run", 123, actions)
        probes[3]["gpu_queue"] = snapshot()
        with self.assertRaisesRegex(ValueError, "gpu_queue_counters_reset_or_wrapped"):
            review_cycle(probes, "run", 123, actions)

    def test_adapter_aliases_are_separate_and_observations_cannot_close_bounds(self):
        probes, actions = evidence()
        for row in probes:
            alias = copy.deepcopy(row["gpu_queue"]["adapters"][0])
            alias["adapter_ordinal"] = 1
            row["gpu_queue"]["adapters"].append(alias)
        result = review_cycle(probes, "run", 123, actions)
        self.assertEqual(set(result["adapters"]), {"0", "1"})
        self.assertEqual(result["adapters"]["0"]["0"]["maximum_observed_submitted_minus_completed"], 1)
        checks = {key: "PASS" for key in FULL_MEDIA_GPU_CHECKS if key != "gpu_queue_coverage"}
        checks["gpu_scheduler_packets_coverage"] = "PASS"
        report = dict(deferred=["gpu_queue"], cycles=[dict(checks=checks, details=dict(backend_observed=["dxgi"]))])
        self.assertEqual(full_media_gpu_gaps(report), ["gpu_queue", "gpu_queue_coverage", "gpu_queue_frozen_limits"])

    def test_provisional_limits_are_diagnostic_only_and_fail_when_exceeded(self):
        probes, actions, policy = diagnostic_evidence()
        observation = review_cycle(probes, "run", 123, actions)
        result = review_diagnostic_bounds(probes, "run", 123, actions, observation, policy)
        self.assertTrue(result["passed"])
        self.assertFalse(result["release_eligible"])
        for row in probes:
            packet = row["gpu_queue"]["adapters"][0]["nodes"][0]["queue_packets"][0]
            packet["submitted"] = packet["completed"] + 65
            packet["submitted_minus_completed"] = 65
        result = review_diagnostic_bounds(probes, "run", 123, actions, review_cycle(probes, "run", 123, actions), policy)
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"]["sampled_pending"])
        self.assertFalse(result["checks"]["release_pending"])

    def test_dma_faults_and_missing_release_window_cannot_be_suppressed(self):
        probes, actions, policy = diagnostic_evidence()
        probes[-1]["gpu_queue"]["adapters"][0]["nodes"][0]["dma_packets"][0]["faulted"] = 1
        observation = review_cycle(probes, "run", 123, actions)
        result = review_diagnostic_bounds(probes, "run", 123, actions, observation, policy)
        self.assertFalse(result["checks"]["dma_faults"])
        with self.assertRaisesRegex(ValueError, "gpu_queue_release_window_incomplete"):
            review_diagnostic_bounds(probes[:8], "run", 123, actions, observation, policy)

    def test_approximate_policy_cannot_be_promoted_to_formal(self):
        probes, actions, policy = diagnostic_evidence()
        observation = review_cycle(probes, "run", 123, actions)
        for key, value in (("diagnostic_only", False), ("release_eligible", True),
                           ("maximum_observed_pending_packets_per_scheduler_node", True)):
            changed = copy.deepcopy(policy)
            changed[key] = value
            with self.assertRaisesRegex(ValueError, "gpu_queue_provisional_policy_invalid"):
                review_diagnostic_bounds(probes, "run", 123, actions, observation, changed)


if __name__ == "__main__":
    unittest.main()
