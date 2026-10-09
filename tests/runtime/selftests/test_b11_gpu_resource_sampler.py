"""Offline GPU evidence identity, missing-counter, aggregation and lifetime tests."""
from copy import deepcopy
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/meeting"))
import b11_gpu_resource_sampler as gpu


def counter_data(pid=123):
    prefix = f"pid_{pid}_luid_0x00000000_0x0000abcd_phys_0"
    return {"counter_statuses": {name: "AVAILABLE" for name in ("engines", "dedicated", "shared")},
        "engines": [{"name": prefix + "_eng_0_engtype_3D", "status": 0, "value": 20.0},
            {"name": prefix + "_eng_1_engtype_Copy", "status": 1, "value": 30.0}],
        "dedicated": [{"name": prefix, "status": 0, "value": 1048576}],
        "shared": [{"name": prefix, "status": 0, "value": 2097152}]}


class Backend:
    def __init__(self, data=None):
        self.identity = {"start_ticks": 1234567890, "image": r"E:\fixture\RelWithDebInfo\receiver.exe"}
        self.data = counter_data() if data is None else data
        self.alive_responses = []
        self.close_count = self.collect_count = 0

    def alive(self):
        return self.alive_responses.pop(0) if self.alive_responses else True

    def collect(self):
        self.collect_count += 1
        return deepcopy(self.data)

    def close(self):
        self.close_count += 1


class GpuResourceSamplerTests(unittest.TestCase):
    def sampler(self, data=None):
        backend = Backend(data)
        sampler = gpu.GpuResourceSampler(123, _backend=backend)
        self.addCleanup(sampler.close)
        return sampler, backend

    def interval(self, sampler):
        self.assertEqual(sampler.sample(10)["gpu_resource_status"], "BASELINE")
        return sampler.sample(11)

    def test_reports_per_engine_and_maximum_without_summing(self):
        sampler, _ = self.sampler()
        row = self.interval(sampler)
        self.assertEqual(row["gpu_resource_status"], "AVAILABLE")
        self.assertEqual(row["gpu_busiest_engine_percent"], 30.0)
        self.assertEqual(len(row["gpu_engine_instances"]), 2)
        self.assertEqual(row["gpu_sample_interval_seconds"], 1)
        self.assertEqual(row["gpu_process_memory_instances"][0]["adapter_luid"], "0x00000000_0x0000abcd")
        self.assertEqual(row["gpu_dedicated_bytes"], 1048576)
        self.assertEqual(row["gpu_shared_bytes"], 2097152)

    def test_does_not_use_other_pid_or_whole_machine_activity(self):
        data = counter_data(3123)
        sampler, _ = self.sampler(data)
        row = sampler.sample(10)
        self.assertEqual(row["gpu_resource_status"], "UNKNOWN")
        self.assertIsNone(row["gpu_busiest_engine_percent"])
        self.assertEqual(row["gpu_engine_instances"], [])
        self.assertIsNone(row["gpu_dedicated_bytes"])

    def test_no_engine_instances_is_unknown_not_zero(self):
        data = counter_data()
        data["engines"] = []
        sampler, _ = self.sampler(data)
        row = self.interval(sampler)
        self.assertEqual(row["gpu_engine_status"], "UNKNOWN")
        self.assertEqual(row["gpu_resource_status"], "UNKNOWN")
        self.assertIsNone(row["gpu_busiest_engine_percent"])
        self.assertEqual(row["gpu_memory_status"], "AVAILABLE")

    def test_zero_activity_is_available_when_measured(self):
        data = counter_data()
        for counter in data["engines"]:
            counter["value"] = 0.0
        sampler, _ = self.sampler(data)
        self.assertEqual(self.interval(sampler)["gpu_busiest_engine_percent"], 0)

    def test_invalid_counter_does_not_hide_in_good_rows(self):
        for invalid in (float("nan"), -1, None, True):
            with self.subTest(invalid=invalid):
                data = counter_data()
                data["engines"][1]["value"] = invalid
                sampler, _ = self.sampler(data)
                row = self.interval(sampler)
                self.assertEqual(row["gpu_engine_status"], "UNKNOWN")
                self.assertIsNone(row["gpu_busiest_engine_percent"])

    def test_missing_shared_adapter_is_partial_unknown(self):
        data = counter_data()
        data["shared"] = []
        sampler, _ = self.sampler(data)
        sampler.sample(10)
        row = sampler.sample(11)
        self.assertEqual(row["gpu_engine_status"], "AVAILABLE")
        self.assertEqual(row["gpu_memory_status"], "UNKNOWN")
        self.assertIsNone(row["gpu_shared_bytes"])
        self.assertEqual(row["gpu_process_memory_instances"][0]["dedicated_bytes"], 1048576)
        self.assertEqual(row["gpu_dedicated_bytes"], 1048576)
        summary = gpu.gpu_resource_summary([row])
        self.assertEqual(summary["engine_status"], "AVAILABLE")
        self.assertEqual(summary["dedicated_status"], "AVAILABLE")
        self.assertEqual(summary["shared_status"], "UNKNOWN")
        self.assertEqual(summary["memory_status"], "UNKNOWN")

    def test_distinct_adapters_sum_memory_but_do_not_sum_engines(self):
        data = counter_data()
        other = counter_data()
        for kind in ("engines", "dedicated", "shared"):
            for item in other[kind]:
                item["name"] = item["name"].replace("0000abcd", "0000ef01")
            data[kind].extend(other[kind])
        sampler, _ = self.sampler(data)
        row = self.interval(sampler)
        self.assertEqual(row["gpu_busiest_engine_percent"], 30.0)
        self.assertEqual(row["gpu_dedicated_bytes"], 2097152)
        self.assertEqual(len(row["gpu_process_memory_instances"]), 2)

    def test_alias_adapter_rows_do_not_double_count(self):
        data = counter_data()
        alias = deepcopy(data["dedicated"][0])
        alias["name"] += "#1"
        data["dedicated"].append(alias)
        sampler, _ = self.sampler(data)
        row = sampler.sample(10)
        self.assertEqual(row["gpu_memory_status"], "UNKNOWN")
        self.assertIsNone(row["gpu_dedicated_bytes"])

    def test_process_exit_before_query_never_collects(self):
        sampler, backend = self.sampler()
        backend.alive_responses = [False]
        row = sampler.sample(10)
        self.assertEqual(backend.collect_count, 0)
        self.assertEqual(row["gpu_resource_reason"], "target_process_exited")
        self.assertFalse(row["gpu_process_alive"])

    def test_process_exit_during_query_discards_counters(self):
        sampler, backend = self.sampler()
        backend.alive_responses = [True, False]
        row = sampler.sample(10)
        self.assertEqual(row["gpu_resource_reason"], "target_process_exited_during_query")
        self.assertEqual(row["gpu_engine_instances"], [])
        self.assertIsNone(row["gpu_dedicated_bytes"])

    def test_close_and_context_are_idempotent(self):
        backend = Backend()
        with gpu.GpuResourceSampler(123, _backend=backend) as sampler:
            sampler.sample(10)
        sampler.close()
        self.assertEqual(backend.close_count, 1)
        self.assertEqual(sampler.sample(11)["gpu_resource_reason"], "sampler_closed")
        self.assertEqual(backend.collect_count, 1)

    def test_summary_accepts_mixed_resource_rows_without_performance_pass(self):
        sampler, _ = self.sampler()
        samples = [{**sampler.sample(10), "private_bytes": 500},
            {**sampler.sample(11), "private_bytes": 600}, {**sampler.sample(12), "private_bytes": 700}]
        summary = gpu.gpu_resource_summary(samples)
        self.assertEqual(summary["status"], "AVAILABLE")
        self.assertEqual(summary["performance_acceptance_status"], "NOT_EVALUATED")
        self.assertEqual(summary["interval_sample_count"], 2)
        self.assertEqual(summary["fields"]["gpu_busiest_engine_percent"]["mean"], 30)
        self.assertEqual(summary["unknown_samples"], 0)

    def test_summary_preserves_late_failure_and_identity_changes(self):
        sampler, _ = self.sampler()
        samples = [sampler.sample(10), sampler.sample(11), sampler.sample(12)]
        for key, value in (("gpu_resource_status", "UNKNOWN"), ("gpu_process_start_ticks", 7),
                ("gpu_process_image", "another.exe"), ("gpu_resource_pid", 124)):
            with self.subTest(key=key):
                changed = deepcopy(samples)
                changed[-1][key] = value
                self.assertEqual(gpu.gpu_resource_summary(changed)["status"], "UNKNOWN")

    def test_pdh_and_process_handles_close_once(self):
        backend = gpu._WindowsPdhBackend.__new__(gpu._WindowsPdhBackend)
        calls = []
        backend.query, backend.process = 17, 18
        backend.pdh = SimpleNamespace(PdhCloseQuery=lambda query: calls.append(("query", query)))
        backend.kernel = SimpleNamespace(CloseHandle=lambda handle: calls.append(("process", handle)))
        backend.close()
        backend.close()
        self.assertEqual(calls, [("query", 17), ("process", 18)])

    def test_invalid_clock_is_unknown_without_querying(self):
        sampler, backend = self.sampler()
        row = sampler.sample(float("nan"))
        self.assertEqual(row["gpu_resource_reason"], "monotonic_timestamp_invalid")
        self.assertEqual(backend.collect_count, 0)

    def test_counter_failure_resets_interval_and_late_baseline_stays_unknown(self):
        sampler, backend = self.sampler()
        samples = [sampler.sample(10), sampler.sample(11)]
        backend.data = {"error": "pdh_collect_failed_0x800007d5"}
        samples.append(sampler.sample(12))
        backend.data = counter_data()
        samples.append(sampler.sample(13))
        samples.append(sampler.sample(14))
        self.assertEqual(samples[3]["gpu_resource_status"], "BASELINE")
        self.assertEqual(samples[4]["gpu_sample_interval_seconds"], 1)
        self.assertEqual(gpu.gpu_resource_summary(samples)["status"], "UNKNOWN")

    def test_baseline_only_is_not_complete(self):
        sampler, _ = self.sampler()
        self.assertEqual(gpu.gpu_resource_summary([sampler.sample(10)])["status"], "UNKNOWN")
        self.assertEqual(gpu.gpu_resource_summary([])["status"], "UNKNOWN")


if __name__ == "__main__":
    unittest.main()
