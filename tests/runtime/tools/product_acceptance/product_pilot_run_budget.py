"""Bind Windows collectors to the controller's one frozen QPC run budget."""
import ctypes
import json
from pathlib import Path
import re
import sys
import time


def query_qpc():
    if sys.platform != "win32":
        raise ValueError("run_budget_requires_windows_qpc")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    counter, frequency = ctypes.c_longlong(), ctypes.c_longlong()
    for name, value in (("QueryPerformanceFrequency", frequency), ("QueryPerformanceCounter", counter)):
        function = getattr(kernel, name)
        function.argtypes = [ctypes.POINTER(ctypes.c_longlong)]
        function.restype = ctypes.c_int
        if not function(ctypes.byref(value)):
            raise OSError(ctypes.get_last_error(), name)
    if counter.value < 0 or frequency.value <= 0:
        raise ValueError("run_budget_qpc_invalid")
    return counter.value, frequency.value


def validate_perf_counter_mapping():
    """Verify the host's perf_counter mapping without using it as our epoch."""
    before, frequency = query_qpc()
    perf_ns = time.perf_counter_ns()
    after, after_frequency = query_qpc()
    if frequency != after_frequency or after < before:
        raise ValueError("run_budget_qpc_frequency_or_clock_changed")
    tolerance_ns = 1000000  # API conversion/rounding only; never adds run time.
    lower, upper = before * 1000000000 // frequency, after * 1000000000 // frequency
    if not lower - tolerance_ns <= perf_ns <= upper + tolerance_ns:
        raise ValueError("run_budget_perf_counter_mapping_unknown")
    return dict(qpc_frequency_hz=frequency, qpc_before_ticks=before, qpc_after_ticks=after,
        perf_counter_ns=perf_ns, mapping="same QPC epoch", tolerance_ns=tolerance_ns)


class RunBudget:
    def __init__(self, marker, run_id, seconds):
        required = {"schema", "run_id", "maximum_seconds", "start_qpc_ticks", "frequency_hz", "clock_source"}
        if (not isinstance(marker, dict) or set(marker) != required
                or type(marker["schema"]) is not int or marker["schema"] != 1
                or not isinstance(run_id, str) or not re.fullmatch("[0-9a-f]{32}", run_id)
                or marker["run_id"] != run_id or type(seconds) is not int or seconds <= 0
                or type(marker["maximum_seconds"]) is not int or marker["maximum_seconds"] != seconds
                or type(marker["start_qpc_ticks"]) is not int or marker["start_qpc_ticks"] <= 0
                or type(marker["frequency_hz"]) is not int or marker["frequency_hz"] <= 0
                or marker["clock_source"] != "QueryPerformanceCounter"):
            raise ValueError("run_budget_marker_identity_or_contract")
        self.marker = marker
        self.start, self.frequency, self.seconds = (marker[k] for k in
            ("start_qpc_ticks", "frequency_hz", "maximum_seconds"))
        self.remaining_seconds()

    def remaining_seconds(self):
        now, frequency = query_qpc()
        if frequency != self.frequency or now < self.start:
            raise ValueError("run_budget_qpc_frequency_or_start_mismatch")
        return self.seconds - (now - self.start) / frequency

    def expired(self):
        return self.remaining_seconds() <= 0


class LocalBudget:
    """Compatibility for isolated offline fixtures without a production marker."""
    def __init__(self, seconds):
        if type(seconds) is not int or seconds <= 0:
            raise ValueError("run_budget_duration_invalid")
        self.deadline = time.monotonic() + seconds

    def expired(self):
        return time.monotonic() >= self.deadline


def load_run_budget(path, run_id, seconds):
    if path is None:
        return LocalBudget(seconds)
    path = Path(path)
    if path.is_symlink() or path.stat().st_size > 4096:
        raise ValueError("run_budget_marker_path_or_size")
    return RunBudget(json.loads(path.read_text(encoding="utf-8-sig")), run_id, seconds)
