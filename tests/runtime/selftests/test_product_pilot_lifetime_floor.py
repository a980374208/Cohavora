"""Offline contract for the identity-bound product lifetime verifier.

Inputs are synthetic UIA requested frames and identity-bound lifetime proofs.
These cases do not review a Formal run or prove driver waiting/process liveness.
"""
from __future__ import annotations

from datetime import datetime
import importlib.util
from pathlib import Path
import sys
import unittest


RUN = "0123456789abcdef0123456789abcdef"
PID = 1234
TICKS = 639028224000000000
EXECUTABLE = str(Path(__file__).resolve().with_name("synthetic-owned-product.exe"))
CANDIDATE = Path(__file__).resolve().parents[3] / "tests/runtime/tools/product_acceptance/verify_product_external.py"
TOOLS = Path(__file__).resolve().parents[3] / "tests/runtime/tools/product_acceptance"


class ProductLifetimeFloorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Import the real candidate, including its normal local dependencies.
        # Do not extract/reimplement its predicate or call its review/main entry.
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location("focused_lifetime_candidate", CANDIDATE)
        if spec is None or spec.loader is None:
            raise RuntimeError("Candidate verifier cannot be loaded")
        cls.verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.verifier)

    def case(self):
        identity = dict(run_id=RUN, pid=PID, start_ticks=TICKS, executable=EXECUTABLE)
        first = dict(schema=1, run_id=RUN, pid=PID, cycle=1, cycle_id="1" * 32,
                     operation_id="a" * 32, action="login", phase="requested",
                     utc="2026-01-01T00:01:00Z", elapsed_seconds=60.0)
        exit_request = dict(schema=1, run_id=RUN, pid=PID, cycle=100, cycle_id="2" * 32,
                            operation_id="b" * 32, action="process_exit", phase="requested",
                            utc="2026-01-01T08:01:00Z", elapsed_seconds=28860.0)
        origin = dict(pid=PID, start_ticks=TICKS, executable=EXECUTABLE,
                      utc=first["utc"], run_clock_elapsed_origin_seconds=60.0,
                      clock_source="System.Diagnostics.Stopwatch", origin_kind="first_live_requested")
        result = dict(run_id=RUN, verdict="UIA_COMPLETE", cycles_requested=100, cycles_completed=100,
                      started_utc="2026-01-01T00:00:00Z", finished_utc="2026-01-01T08:01:01Z",
                      minimum_seconds=28800, product_first_live=origin,
                      product_live_elapsed_seconds_before_close=28800.0,
                      product_lifetime_floor_required_seconds=28800)
        return result, identity, [first, exit_request]

    def check(self, payload, expected):
        result, identity, actions = payload
        proof = self.verifier.product_lifetime_floor(result, identity, RUN, actions)
        self.assertEqual(proof["status"], expected)
        self.assertIs(proof["passed"], expected == "PASS")
        return proof

    def test_exact_28800_boundary_passes(self):
        proof = self.check(self.case(), "PASS")
        self.assertEqual(proof["first_requested_to_exit_requested_elapsed_seconds"], 28800.0)

    def test_driver_utc_eight_hours_cannot_cover_short_product_lifetime(self):
        result, identity, actions = self.case()
        result["finished_utc"] = "2026-01-01T08:00:00Z"
        duration = (datetime.fromisoformat(result["finished_utc"].replace("Z", "+00:00")) -
                    datetime.fromisoformat(result["started_utc"].replace("Z", "+00:00"))).total_seconds()
        self.assertEqual(duration, 28800)
        actions[-1].update(utc=result["finished_utc"], elapsed_seconds=28800.0)
        result["product_live_elapsed_seconds_before_close"] = 28740.0
        self.check((result, identity, actions), "FAIL")

    def test_both_requested_span_and_pre_close_duration_are_required(self):
        for short_clock in ("requested", "pre_close"):
            with self.subTest(short_clock=short_clock):
                result, identity, actions = self.case()
                if short_clock == "requested":
                    actions[-1]["elapsed_seconds"] = 28859.999
                else:
                    result["product_live_elapsed_seconds_before_close"] = 28799.999
                self.check((result, identity, actions), "FAIL")

    def test_utc_jump_does_not_replace_valid_monotonic_span(self):
        for wall_finish in ("2026-01-01T07:30:00Z", "2026-01-02T08:00:00Z"):
            with self.subTest(wall_finish=wall_finish):
                result, identity, actions = self.case()
                result["finished_utc"] = wall_finish
                actions[-1]["utc"] = wall_finish
                self.check((result, identity, actions), "PASS")

    def test_wrong_identity_or_diagnostic_origin_cannot_qualify(self):
        mutations = (("pid", PID + 1), ("start_ticks", TICKS + 1),
                     ("executable", EXECUTABLE + ".other"), ("start_ticks", 0),
                     ("start_ticks", 1 << 63), ("origin_kind", "supervisor_owned_live_diagnostic"))
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                result, identity, actions = self.case()
                result["product_first_live"][field] = value
                self.check((result, identity, actions), "FAIL")
        result, identity, actions = self.case()
        identity["run_id"] = "f" * 32
        self.check((result, identity, actions), "FAIL")

    def test_early_or_unobserved_exit_is_rejected(self):
        result, identity, actions = self.case()
        actions[-1]["elapsed_seconds"] = 200.0
        result["product_live_elapsed_seconds_before_close"] = 140.0
        self.check((result, identity, actions), "FAIL")
        result, identity, actions = self.case()
        result["product_live_elapsed_seconds_before_close"] = None
        self.check((result, identity, actions), "FAIL")
        result, identity, actions = self.case()
        actions.pop()
        result["product_live_elapsed_seconds_before_close"] = None
        self.check((result, identity, actions), "UNKNOWN")

    def test_nonfinite_or_non_numeric_clocks_are_rejected(self):
        for field in ("origin", "first_requested", "exit_requested", "pre_close"):
            for value in (float("nan"), float("inf"), -1.0, True, "28800"):
                with self.subTest(field=field, value=value):
                    result, identity, actions = self.case()
                    if field == "origin":
                        result["product_first_live"]["run_clock_elapsed_origin_seconds"] = value
                    elif field == "first_requested":
                        actions[0]["elapsed_seconds"] = value
                    elif field == "exit_requested":
                        actions[-1]["elapsed_seconds"] = value
                    else:
                        result["product_live_elapsed_seconds_before_close"] = value
                    self.check((result, identity, actions), "FAIL")
        result, identity, actions = self.case()
        result["product_lifetime_floor_required_seconds"] = 28800.0
        self.check((result, identity, actions), "FAIL")

    def test_missing_proof_frames_and_legacy_schema_stay_unknown(self):
        for field in ("product_first_live", "product_live_elapsed_seconds_before_close",
                      "product_lifetime_floor_required_seconds"):
            with self.subTest(missing=field):
                result, identity, actions = self.case()
                del result[field]
                self.check((result, identity, actions), "UNKNOWN")
        result, identity, actions = self.case()
        for field in ("product_first_live", "product_live_elapsed_seconds_before_close",
                      "product_lifetime_floor_required_seconds"):
            del result[field]
        self.check((result, identity, actions), "UNKNOWN")
        result, identity, actions = self.case()
        del result["product_first_live"]["origin_kind"]
        self.check((result, identity, actions), "UNKNOWN")
        result, identity, actions = self.case()
        del actions[0]["elapsed_seconds"]
        self.check((result, identity, actions), "UNKNOWN")

    def test_origin_must_match_the_first_requested_frame_without_padding(self):
        for field, value in (("run_clock_elapsed_origin_seconds", 59.999),
                             ("utc", "2026-01-01T00:00:59Z")):
            with self.subTest(field=field):
                result, identity, actions = self.case()
                result["product_first_live"][field] = value
                self.check((result, identity, actions), "FAIL")


if __name__ == "__main__":
    unittest.main(verbosity=2)
