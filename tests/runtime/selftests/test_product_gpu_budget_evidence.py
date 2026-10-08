"""Reject budget data attributed to a different process or incomplete observer."""
import copy
import importlib.util
import json
import hashlib
import unittest
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

PATH = Path(__file__).resolve().parents[1] / "tools/product_acceptance/analyze_product_gpu_budget.py"
SPEC = importlib.util.spec_from_file_location("gpu_budget_evidence", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
sys.path.insert(0, str(PATH.parent))
from release_product_acceptance import FULL_MEDIA_GPU_CHECKS, evaluate, full_media_gpu_gaps
from product_pilot_scheduler import load_scheduler_policy


def snapshot():
    values = dict(availability="VALID", hresult=0, budget_bytes=100, current_usage_bytes=10,
                  available_for_reservation_bytes=80, current_reservation_bytes=0)
    return dict(collector="dxgi_in_process_budget", pid=123, scope=MODULE.SCOPE,
                availability="VALID", initialization_hresult=0, elapsed_us=15,
                adapters=[{"adapter_ordinal": 0, "node_index": 0, "local": values, "nonlocal": copy.deepcopy(values)}])


def all_nodes_snapshot():
    data = snapshot()
    data["scope"] = MODULE.ALL_NODES_SCOPE
    data["topology_current"] = True
    node0 = data["adapters"][0]
    del node0["adapter_ordinal"]
    node1 = copy.deepcopy(node0)
    node1["node_index"] = 1
    data["adapters"] = [dict(adapter_ordinal=0, node_count=2, node_count_hresult=0, nodes=[node0, node1])]
    return data


def complete_gpu_report(checks, backend="dxgi"):
    path=PATH.parent/"product_gpu_queue_limits.json"
    policy=json.loads(path.read_text())
    digest=hashlib.sha256(path.read_bytes()).hexdigest()
    return dict(deferred=[],gpu_queue_frozen_limits=dict(sha256=digest,policy=policy),
        cycles=[dict(checks=checks,details=dict(backend_observed=[backend],gpu_queue_coverage=dict(policy_sha256=digest)))])


class BudgetTests(unittest.TestCase):
    def test_external_process_budget_is_rejected(self):
        self.assertEqual(MODULE.validate_snapshot(snapshot(), 123), (0,))
        with self.assertRaises(ValueError):
            MODULE.validate_snapshot(snapshot(), 124)

    def test_unknown_failed_and_missing_values_cannot_become_zero(self):
        for key, value in (("availability", "UNAVAILABLE"), ("hresult", 2147500037),
                           ("budget_bytes", None), ("current_usage_bytes", -1), ("current_reservation_bytes", False)):
            data = snapshot()
            data["adapters"][0]["local"][key] = value
            with self.assertRaises(ValueError, msg=key):
                MODULE.validate_snapshot(data, 123)

    def test_actual_zero_is_preserved_as_valid_measurement(self):
        data = snapshot()
        data["adapters"][0]["nonlocal"]["budget_bytes"] = 0
        self.assertEqual(MODULE.validate_snapshot(data, 123), (0,))

    def test_sparse_window_and_missing_gpu_rows_cannot_pass(self):
        probes = [dict(utc_ms=i * 1000, run_id="run", gpu_budget=snapshot()) for i in range(5)]
        result = MODULE.review_window(probes, "run", 123, 0, 4000)
        self.assertEqual(result["samples"], 5)
        with self.assertRaises(ValueError):
            MODULE.review_window([probes[0], probes[-1]], "run", 123, 0, 4000)
        del probes[2]["gpu_budget"]
        with self.assertRaises(ValueError):
            MODULE.review_window(probes, "run", 123, 0, 4000)

    def test_adapter_ordinals_and_node_scope_are_bounded(self):
        data = snapshot()
        data["adapters"].append(copy.deepcopy(data["adapters"][0]))
        with self.assertRaises(ValueError):
            MODULE.validate_snapshot(data, 123)
        data = snapshot()
        data["adapters"][0]["node_index"] = 1
        with self.assertRaises(ValueError):
            MODULE.validate_snapshot(data, 123)

    def test_lifecycle_identity_and_observed_endpoints_are_required(self):
        probes = [dict(utc_ms=i * 1000, run_id="run", gpu_budget=snapshot()) for i in range(5)]
        actions = [dict(run_id="run", pid=123, cycle_id="cycle", utc_ms=i * 4000,
                        action=name, phase="uia_observed") for i, name in enumerate(("join", "export"))]
        self.assertEqual(MODULE.review_cycle(probes, "run", 123, actions)["samples"], 5)
        for key, value in (("pid", 124), ("run_id", "other"), ("cycle_id", "other"),
                           ("phase", "requested"), ("utc_ms", 0)):
            changed = copy.deepcopy(actions)
            changed[-1][key] = value
            with self.assertRaises(ValueError, msg=key):
                MODULE.review_cycle(probes, "run", 123, changed)
        with self.assertRaises(ValueError):
            MODULE.review_cycle(probes, "run", 123, actions + [actions[-1]])

    def test_all_nodes_require_actual_count_complete_groups_and_current_topology(self):
        data = all_nodes_snapshot()
        self.assertEqual(MODULE.validate_snapshot(data, 123), ((0, 2),))
        for key, value in (("node_count", 0), ("node_count", True), ("node_count_hresult", 2147500037),
                           ("nodes", data["adapters"][0]["nodes"][:1])):
            changed = copy.deepcopy(data)
            changed["adapters"][0][key] = value
            with self.assertRaises(ValueError, msg=key):
                MODULE.validate_snapshot(changed, 123)
        changed = copy.deepcopy(data)
        changed["adapters"][0]["nodes"][1]["nonlocal"]["current_usage_bytes"] = None
        with self.assertRaises(ValueError):
            MODULE.validate_snapshot(changed, 123)
        data["topology_current"] = False
        with self.assertRaises(ValueError):
            MODULE.validate_snapshot(data, 123)

    def test_node0_history_cannot_substitute_all_nodes_and_aliases_are_not_summed(self):
        probes = [dict(utc_ms=i * 1000, run_id="run", gpu_budget=snapshot()) for i in range(5)]
        with self.assertRaisesRegex(ValueError, "gpu_budget_required_node_scope_missing"):
            MODULE.review_window(probes, "run", 123, 0, 4000, MODULE.ALL_NODES_SCOPE)
        data = all_nodes_snapshot()
        alias = copy.deepcopy(data["adapters"][0])
        alias["adapter_ordinal"] = 1
        data["adapters"].append(alias)
        for probe in probes:
            probe["gpu_budget"] = copy.deepcopy(data)
        result = MODULE.review_window(probes, "run", 123, 0, 4000, MODULE.ALL_NODES_SCOPE)
        self.assertEqual(set(result["adapters"]), {"0", "1"})
        self.assertEqual(set(result["adapters"]["0"]), {"0", "1"})
        self.assertEqual(result["adapters"]["0"]["1"]["local"]["current_usage_bytes_max"], 10)
        probes[2]["gpu_budget"]["adapters"][0]["node_count"] = 1
        probes[2]["gpu_budget"]["adapters"][0]["nodes"].pop()
        with self.assertRaisesRegex(ValueError, "gpu_budget_topology_changed"):
            MODULE.review_window(probes, "run", 123, 0, 4000, MODULE.ALL_NODES_SCOPE)

    def test_new_budget_coverage_does_not_close_complete_gpu_gate(self):
        report = dict(deferred=["dxgi_linked_adapter_nodes_other_than_0", "gpu_queue", "wgc_handle_ownership"],
                      cycles=[dict(checks=dict(dxgi_node0_budget_coverage="PASS"),
                                   details=dict(backend_observed=["dxgi"]))])
        gaps = full_media_gpu_gaps(report)
        self.assertIn("dxgi_linked_adapter_nodes_other_than_0", gaps)
        self.assertIn("gpu_queue_coverage", gaps)
        self.assertNotIn("wgc_handle_ownership", gaps)

    def test_three_limited_pilots_cannot_authorize_complete_formal_run(self):
        with TemporaryDirectory() as directory:
            roots = [Path(directory) / str(i) for i in range(3)]
            for i, root in enumerate(roots):
                root.mkdir()
                run_id = f"{i:032x}"
                scheduler_path = PATH.parent / "product_pilot_scheduler_policy.json"
                scheduler = load_scheduler_policy(scheduler_path)
                (root / "collector-scheduler-policy.json").write_bytes(scheduler_path.read_bytes())
                (root / "uia").mkdir()
                gpu = complete_gpu_report(dict(dxgi_node0_budget_coverage="PASS"))
                external = dict(gpu, run_id=run_id, verdict="PASS_WITH_DEFERRED", cycle_counts={"PASS": 2},
                                final_checks={"collector_scheduler_policy_complete": True},
                                collector_scheduler_policy={"passed": True, "policy_sha256": scheduler["sha256"]})
                files = {
                    "plan.json": dict(run_id=run_id, cycles=2, mode="Pilot", load={},
                                      collector_scheduler_policy=scheduler,
                                      gpu_queue_limits_sha256=gpu["gpu_queue_frozen_limits"]["sha256"]),
                    "runner-exit.json": dict(run_id=run_id, verdict="EVIDENCE_COMPLETE", exit_code=0),
                    "executed-inputs.json": {str(scheduler_path): scheduler["sha256"]},
                    "pilot-review.json": dict(run_id=run_id, verdict="PILOT_PASS"),
                    "uia/uia-result.json": {}, "external-review.json": external}
                for name, value in files.items():
                    (root / name).write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "full_media_gpu_not_closed:"):
                evaluate(roots, {}, require_full_media_gpu=True)

    def test_wgc_release_is_required_only_when_observed(self):
        checks = {name: "PASS" for name in FULL_MEDIA_GPU_CHECKS}
        report = complete_gpu_report(checks,"wgc")
        self.assertEqual(full_media_gpu_gaps(report), ["wgc_handle_ownership"])
        checks["wgc_ownership_released"] = "PASS"
        self.assertEqual(full_media_gpu_gaps(report), [])
        report["deferred"] = ["wgc_handle_ownership"]
        self.assertEqual(full_media_gpu_gaps(report), ["wgc_handle_ownership"])

    def test_missing_microphone_pcm_cannot_be_deferred_in_complete_gate(self):
        checks = {name: "PASS" for name in FULL_MEDIA_GPU_CHECKS}
        report = complete_gpu_report(checks)
        self.assertEqual(full_media_gpu_gaps(report), [])
        checks["outbound_audio_pcm_continuity"] = "DEFERRED"
        self.assertEqual(full_media_gpu_gaps(report), ["outbound_audio_pcm_continuity"])

    def test_frozen_gpu_hash_and_scope_cannot_be_inferred_from_green_check(self):
        checks={name:"PASS" for name in FULL_MEDIA_GPU_CHECKS}
        report=complete_gpu_report(checks)
        report['cycles'][0]['details']['gpu_queue_coverage']['policy_sha256']='0'*64
        self.assertEqual(full_media_gpu_gaps(report),['gpu_queue_frozen_limits'])
        report=complete_gpu_report(checks)
        report['gpu_queue_frozen_limits']['policy']['diagnostic_only']=True
        self.assertEqual(full_media_gpu_gaps(report),['gpu_queue_frozen_limits'])


if __name__ == "__main__":
    unittest.main()
