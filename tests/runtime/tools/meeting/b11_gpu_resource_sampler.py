"""Pinned-process WDDM GPU counters for B11 diagnostics; no acceptance thresholds.

PDH uses English counter paths even on localized Windows. GPU percentages are
reported per engine and as the busiest engine, never summed across engines.
Memory totals cover distinct adapter LUID/physical-index instances only.
"""
from __future__ import annotations

import ctypes
import math
import os
import re
import statistics
import time


GPU_RESOURCE_FIELDS = (
    "gpu_resource_pid", "gpu_resource_sample_seq", "gpu_process_start_ticks",
    "gpu_process_image", "gpu_process_alive", "gpu_resource_status",
    "gpu_resource_reason", "gpu_engine_status", "gpu_memory_status",
    "gpu_dedicated_status", "gpu_shared_status",
    "gpu_sample_interval_seconds", "gpu_engine_instances",
    "gpu_busiest_engine_percent", "gpu_busiest_engine_instance",
    "gpu_process_memory_instances", "gpu_dedicated_bytes", "gpu_shared_bytes",
    "gpu_counter_statuses",
)
_ENGINE = re.compile(r"^pid_(\d+)_luid_(0x[0-9a-f]+)_(0x[0-9a-f]+)_phys_(\d+)_eng_(\d+)_engtype_(.+?)(?:#\d+)?$", re.I)
_MEMORY = re.compile(r"^pid_(\d+)_luid_(0x[0-9a-f]+)_(0x[0-9a-f]+)_phys_(\d+)(?:#\d+)?$", re.I)
_COUNTERS = {
    "engines": (r"\GPU Engine(*)\Utilization Percentage", 0x200 | 0x8000),
    "dedicated": (r"\GPU Process Memory(*)\Dedicated Usage", 0x400),
    "shared": (r"\GPU Process Memory(*)\Shared Usage", 0x400),
}


def _number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def _instance(name, expression, pid):
    match = expression.fullmatch(name) if isinstance(name, str) else None
    if not match or int(match[1]) != pid:
        return None
    return {"instance": name, "pid": pid,
        "adapter_luid": "0x%08x_0x%08x" % (int(match[2], 16), int(match[3], 16)),
        "physical_adapter_index": int(match[4]), **(
            {"engine_index": int(match[5]), "engine_type": match[6]}
            if expression is _ENGINE else {})}


def _decode_counters(raw, pid):
    engines, invalid_engines = [], False
    engine_keys = set()
    for item in raw.get("engines", []):
        identity = _instance(item.get("name"), _ENGINE, pid)
        if identity is None:
            continue
        key = (identity["adapter_luid"], identity["physical_adapter_index"], identity["engine_index"])
        if key in engine_keys or item.get("status") not in (0, 1) or not _number(item.get("value")):
            invalid_engines = True
            continue
        engine_keys.add(key)
        engines.append({**identity, "utilization_percent": item["value"]})
    memory = {"dedicated": {}, "shared": {}}
    invalid_memory = {"dedicated": False, "shared": False}
    for kind in memory:
        for item in raw.get(kind, []):
            identity = _instance(item.get("name"), _MEMORY, pid)
            if identity is None:
                continue
            key = (identity["adapter_luid"], identity["physical_adapter_index"])
            if key in memory[kind] or item.get("status") not in (0, 1) or \
                    type(item.get("value")) is not int or item["value"] < 0:
                invalid_memory[kind] = True
                continue
            memory[kind][key] = {**identity, "bytes": item["value"]}
    memory_rows = []
    for key in sorted(set(memory["dedicated"]) | set(memory["shared"])):
        dedicated, shared = memory["dedicated"].get(key), memory["shared"].get(key)
        identity = dedicated or shared
        memory_rows.append({"adapter_luid": identity["adapter_luid"],
            "physical_adapter_index": identity["physical_adapter_index"],
            "dedicated_instance": dedicated["instance"] if dedicated else None,
            "shared_instance": shared["instance"] if shared else None,
            "dedicated_bytes": dedicated["bytes"] if dedicated else None,
            "shared_bytes": shared["bytes"] if shared else None})
    statuses = raw.get("counter_statuses", {})
    engine_ok = bool(engines) and not invalid_engines and statuses.get("engines") == "AVAILABLE"
    adapter_keys = set(memory["dedicated"]) | set(memory["shared"])
    memory_kind_ok = {kind: bool(adapter_keys) and not invalid_memory[kind] and
        set(memory[kind]) == adapter_keys and statuses.get(kind) == "AVAILABLE" for kind in memory}
    memory_ok = all(memory_kind_ok.values())
    busiest = max(engines, key=lambda value: value["utilization_percent"], default=None)
    return {"gpu_engine_status": "AVAILABLE" if engine_ok else "UNKNOWN",
        "gpu_memory_status": "AVAILABLE" if memory_ok else "UNKNOWN",
        "gpu_dedicated_status": "AVAILABLE" if memory_kind_ok["dedicated"] else "UNKNOWN",
        "gpu_shared_status": "AVAILABLE" if memory_kind_ok["shared"] else "UNKNOWN",
        "gpu_engine_instances": engines,
        "gpu_busiest_engine_percent": busiest["utilization_percent"] if engine_ok else None,
        "gpu_busiest_engine_instance": busiest["instance"] if engine_ok else None,
        "gpu_process_memory_instances": memory_rows,
        "gpu_dedicated_bytes": sum(value["dedicated_bytes"] for value in memory_rows) if memory_kind_ok["dedicated"] else None,
        "gpu_shared_bytes": sum(value["shared_bytes"] for value in memory_rows) if memory_kind_ok["shared"] else None}


class _WindowsPdhBackend:
    """Own both the target process handle and one PDH query until close()."""
    def __init__(self, pid):
        from ctypes import wintypes as w
        self.pid, self.process, self.query = pid, None, None
        self.counters, self.counter_statuses = {}, {}
        self.identity = {"start_ticks": None, "image": None}
        self.error = ""
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.pdh = ctypes.WinDLL("pdh", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.kernel.OpenProcess.restype = w.HANDLE
        self.kernel.CloseHandle.argtypes = [w.HANDLE]
        self.kernel.CloseHandle.restype = w.BOOL
        self.kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
        self.kernel.WaitForSingleObject.restype = w.DWORD
        self.kernel.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
        self.kernel.GetProcessTimes.restype = w.BOOL
        self.kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
        self.kernel.QueryFullProcessImageNameW.restype = w.BOOL
        self.pdh.PdhOpenQueryW.argtypes = [w.LPCWSTR, ctypes.c_size_t, ctypes.POINTER(w.HANDLE)]
        self.pdh.PdhOpenQueryW.restype = ctypes.c_uint32
        self.pdh.PdhAddEnglishCounterW.argtypes = [w.HANDLE, w.LPCWSTR, ctypes.c_size_t, ctypes.POINTER(w.HANDLE)]
        self.pdh.PdhAddEnglishCounterW.restype = ctypes.c_uint32
        self.pdh.PdhCollectQueryData.argtypes = [w.HANDLE]
        self.pdh.PdhCollectQueryData.restype = ctypes.c_uint32
        self.pdh.PdhCloseQuery.argtypes = [w.HANDLE]
        self.pdh.PdhCloseQuery.restype = ctypes.c_uint32
        self.pdh.PdhGetFormattedCounterArrayW.argtypes = [w.HANDLE, w.DWORD,
            ctypes.POINTER(w.DWORD), ctypes.POINTER(w.DWORD), ctypes.c_void_p]
        self.pdh.PdhGetFormattedCounterArrayW.restype = ctypes.c_uint32

        class ValueUnion(ctypes.Union):
            _fields_ = [("doubleValue", ctypes.c_double), ("largeValue", ctypes.c_int64)]
        class Value(ctypes.Structure):
            _fields_ = [("CStatus", w.DWORD), ("value", ValueUnion)]
        class Item(ctypes.Structure):
            _fields_ = [("szName", w.LPWSTR), ("FmtValue", Value)]
        self.Item = Item
        try:
            self.process = self.kernel.OpenProcess(0x1000 | 0x100000, False, pid)
            if not self.process:
                raise RuntimeError("target_process_open_failed")
            process_times = [w.FILETIME() for _ in range(4)]
            if not self.kernel.GetProcessTimes(self.process, *(ctypes.byref(value) for value in process_times)):
                raise RuntimeError("target_process_identity_unavailable")
            creation = process_times[0]
            self.identity["start_ticks"] = (creation.dwHighDateTime << 32) | creation.dwLowDateTime
            path, length = ctypes.create_unicode_buffer(32768), w.DWORD(32768)
            if not self.kernel.QueryFullProcessImageNameW(self.process, 0, path, ctypes.byref(length)):
                raise RuntimeError("target_process_image_unavailable")
            self.identity["image"] = path.value
            query = w.HANDLE()
            result = self.pdh.PdhOpenQueryW(None, 0, ctypes.byref(query))
            if result:
                raise RuntimeError("pdh_open_failed_0x%08x" % result)
            self.query = query
            for name, (counter_path, _) in _COUNTERS.items():
                counter = w.HANDLE()
                result = self.pdh.PdhAddEnglishCounterW(query, counter_path, 0, ctypes.byref(counter))
                if result:
                    self.counter_statuses[name] = "pdh_add_failed_0x%08x" % result
                else:
                    self.counters[name] = counter
        except Exception:
            self.close()
            raise

    def alive(self):
        return bool(self.process) and self.kernel.WaitForSingleObject(self.process, 0) == 0x102

    def _array(self, counter, format_code):
        from ctypes import wintypes as w
        size, count = w.DWORD(), w.DWORD()
        result = self.pdh.PdhGetFormattedCounterArrayW(counter, format_code, ctypes.byref(size), ctypes.byref(count), None)
        if result == 0 and count.value == 0:
            return [], "AVAILABLE"
        if result != 0x800007D2:
            return [], "pdh_array_failed_0x%08x" % result
        for _ in range(3):
            if not 0 < size.value <= 16 * 1024 * 1024:
                return [], "pdh_array_size_invalid"
            buffer = ctypes.create_string_buffer(size.value)
            result = self.pdh.PdhGetFormattedCounterArrayW(counter, format_code, ctypes.byref(size), ctypes.byref(count), buffer)
            if result == 0x800007D2:
                continue
            if result:
                return [], "pdh_array_failed_0x%08x" % result
            if count.value * ctypes.sizeof(self.Item) > ctypes.sizeof(buffer):
                return [], "pdh_array_count_invalid"
            items = ctypes.cast(buffer, ctypes.POINTER(self.Item))
            return [{"name": items[index].szName, "status": int(items[index].FmtValue.CStatus),
                "value": items[index].FmtValue.value.largeValue if format_code & 0x400 else
                    items[index].FmtValue.value.doubleValue} for index in range(count.value)], "AVAILABLE"
        return [], "pdh_array_instances_unstable"

    def collect(self):
        result = {"counter_statuses": dict(self.counter_statuses)}
        if not self.query:
            return {**result, "error": "pdh_query_closed"}
        status = self.pdh.PdhCollectQueryData(self.query)
        if status:
            return {**result, "error": "pdh_collect_failed_0x%08x" % status}
        for name, counter in self.counters.items():
            result[name], result["counter_statuses"][name] = self._array(counter, _COUNTERS[name][1])
        return result

    def close(self):
        if self.query:
            self.pdh.PdhCloseQuery(self.query)
            self.query = None
        if self.process:
            self.kernel.CloseHandle(self.process)
            self.process = None


class GpuResourceSampler:
    def __init__(self, pid, *, _backend=None):
        if type(pid) is not int or pid <= 0:
            raise ValueError("GPU target PID must be a positive integer")
        self.pid, self.sequence, self.previous_monotonic = pid, 0, None
        self.backend, self.closed, self.error = None, False, ""
        self.identity = {"start_ticks": None, "image": None}
        if _backend is not None:
            self.backend = _backend
        elif os.name != "nt":
            self.error = "wddm_requires_windows"
        else:
            try:
                self.backend = _WindowsPdhBackend(pid)
            except (OSError, RuntimeError, AttributeError) as error:
                self.error = str(error) if isinstance(error, RuntimeError) else "wddm_initialization_unavailable"
        if self.backend is not None:
            self.identity = dict(self.backend.identity)

    def sample(self, now=None):
        self.sequence += 1
        now = time.monotonic() if now is None else now
        result = dict.fromkeys(GPU_RESOURCE_FIELDS)
        result.update(gpu_resource_pid=self.pid, gpu_resource_sample_seq=self.sequence,
            gpu_process_start_ticks=self.identity["start_ticks"], gpu_process_image=self.identity["image"],
            gpu_process_alive=None, gpu_resource_status="UNKNOWN", gpu_resource_reason=self.error,
            gpu_engine_status="UNKNOWN", gpu_memory_status="UNKNOWN",
            gpu_dedicated_status="UNKNOWN", gpu_shared_status="UNKNOWN", gpu_engine_instances=[],
            gpu_process_memory_instances=[], gpu_counter_statuses={})
        if self.closed:
            result["gpu_resource_reason"] = "sampler_closed"
            return result
        if self.backend is None:
            return result
        if not _number(now):
            self.previous_monotonic = None
            result["gpu_resource_reason"] = "monotonic_timestamp_invalid"
            return result
        try:
            alive = self.backend.alive()
            result["gpu_process_alive"] = alive
            if not alive:
                result["gpu_resource_reason"] = "target_process_exited"
                return result
            raw = self.backend.collect()
            if not self.backend.alive():
                result.update(gpu_process_alive=False, gpu_resource_reason="target_process_exited_during_query")
                return result
            result["gpu_counter_statuses"] = raw.get("counter_statuses", {})
            if raw.get("error"):
                self.previous_monotonic = None
                result["gpu_resource_reason"] = raw["error"]
                return result
            result.update(_decode_counters(raw, self.pid))
        except (OSError, RuntimeError, AttributeError):
            self.previous_monotonic = None
            result["gpu_resource_reason"] = "wddm_query_unavailable"
            return result
        interval = now - self.previous_monotonic if _number(now) and self.previous_monotonic is not None else None
        baseline = self.previous_monotonic is None
        self.previous_monotonic = now if _number(now) else None
        if _number(interval) and interval > 0:
            result["gpu_sample_interval_seconds"] = interval
        if baseline and result["gpu_memory_status"] == "AVAILABLE":
            result.update(gpu_engine_status="BASELINE", gpu_busiest_engine_percent=None,
                gpu_busiest_engine_instance=None, gpu_resource_status="BASELINE",
                gpu_resource_reason="gpu_engine_interval_not_yet_available")
        elif result["gpu_engine_status"] == result["gpu_memory_status"] == "AVAILABLE" and \
                result["gpu_sample_interval_seconds"] is not None and self.identity.get("start_ticks") and self.identity.get("image"):
            result.update(gpu_resource_status="AVAILABLE", gpu_resource_reason="")
        else:
            result["gpu_resource_reason"] = "target_gpu_counter_evidence_incomplete"
        return result

    def close(self):
        if not self.closed:
            self.closed = True
            if self.backend is not None:
                self.backend.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def gpu_resource_summary(samples):
    """Coverage and observed counters, never GPU performance acceptance."""
    samples = list(samples)
    pid = samples[0].get("gpu_resource_pid") if samples else None
    start = samples[0].get("gpu_process_start_ticks") if samples else None
    image = samples[0].get("gpu_process_image") if samples else None
    identity_consistent = type(pid) is int and pid > 0 and type(start) is int and start > 0 and \
        isinstance(image, str) and bool(image) and all(row.get("gpu_resource_pid") == pid and
            row.get("gpu_process_start_ticks") == start and row.get("gpu_process_image") == image for row in samples)
    usable = samples[1:] if samples and samples[0].get("gpu_resource_status") == "BASELINE" else samples
    incomplete = [row for row in usable if row.get("gpu_resource_status") != "AVAILABLE"]
    fields = {}
    for name in ("gpu_busiest_engine_percent", "gpu_dedicated_bytes", "gpu_shared_bytes"):
        values = [row[name] for row in usable if _number(row.get(name))]
        fields[name] = {"valid_samples": len(values), "peak": max(values, default=None),
            "mean": statistics.fmean(values) if values else None}
    per_engine = {}
    for row in usable:
        for engine in row.get("gpu_engine_instances", []):
            if _number(engine.get("utilization_percent")):
                per_engine.setdefault(engine["instance"], []).append(engine["utilization_percent"])
    metric_status = {name: "AVAILABLE" if usable and identity_consistent and all(
        row.get(name) == "AVAILABLE" for row in usable) else "UNKNOWN"
        for name in ("gpu_engine_status", "gpu_dedicated_status", "gpu_shared_status")}
    complete = bool(usable) and identity_consistent and not incomplete and all(
        field["valid_samples"] == len(usable) for field in fields.values())
    return {"status": "AVAILABLE" if complete else "UNKNOWN", "performance_acceptance_status": "NOT_EVALUATED",
        "reason": "pinned_process_wddm_counters_collected" if complete else "gpu_evidence_incomplete",
        "engine_status": metric_status["gpu_engine_status"],
        "dedicated_status": metric_status["gpu_dedicated_status"],
        "shared_status": metric_status["gpu_shared_status"],
        "memory_status": "AVAILABLE" if metric_status["gpu_dedicated_status"] ==
            metric_status["gpu_shared_status"] == "AVAILABLE" else "UNKNOWN",
        "maximum_sample_interval_seconds": max((row["gpu_sample_interval_seconds"] for row in usable
            if _number(row.get("gpu_sample_interval_seconds"))), default=None),
        "pid": pid, "process_start_ticks": start, "process_image": image,
        "process_identity_consistent": identity_consistent, "sample_count": len(samples),
        "interval_sample_count": len(usable), "unknown_samples": len(incomplete),
        "engine_measurement": "wddm_pdh_per_process_per_engine_interval_percentage",
        "aggregate_measurement": "maximum_individual_engine_not_sum_not_task_manager_aggregate",
        "memory_measurement": "wddm_pdh_distinct_adapter_luid_physical_index_dedicated_shared",
        "counter_coverage": {name: {"available_samples": sum(row.get(name) == "AVAILABLE" for row in usable),
            "unknown_samples": sum(row.get(name) != "AVAILABLE" for row in usable)}
            for name in ("gpu_engine_status", "gpu_dedicated_status", "gpu_shared_status")},
        "fields": fields, "engines": {name: {"valid_samples": len(values),
            "peak": max(values), "mean": statistics.fmean(values)} for name, values in sorted(per_engine.items())},
        "adapters": sorted({(item["adapter_luid"], item["physical_adapter_index"])
            for row in samples for item in row.get("gpu_process_memory_instances", [])}),
        "limitations": ["No GPU queue, decode hardware attribution, or performance threshold acceptance.",
            "Busiest-engine percentage is not the Task Manager process GPU aggregation."]}
