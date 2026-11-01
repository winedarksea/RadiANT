# SPDX-License-Identifier: Apache-2.0

"""The regression test for scripts/check_test_counts.py's two blind spots.

Both were found the same way: CI went red, and the check reported the wrong
thing about it.

  1. Seven of nine twister scenarios failed to COMPILE. Twister marks their
     statically-discovered cases `blocked`, the check counted `blocked` as
     `failed`, and the run page said "2464 test case(s) failed" - twenty
     `[blocked]` names and not one word of the compiler error, which twister had
     put in the suite-level `reason` the check never read.

  2. The same run reported `2480 >= 843` for the coverage floor and passed it.
     The floor was compared against every case twister DISCOVERED, and discovery
     happens before the build - so a run where nothing executed at all cleared a
     floor whose entire purpose is to notice coverage going down.

These tests are here rather than beside the script because host-tests runs
`python -m unittest discover -s tools -p "test_*.py"`, and this check has to
keep working on a fork with no secret - the same reason that job exists.
"""

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import check_test_counts as ctc  # noqa: E402


def _case(name, status):
    return {"identifier": name, "status": status}


def _report(suites):
    handle = tempfile.NamedTemporaryFile(
        mode="w", suffix=".json", delete=False, encoding="utf-8")
    with handle:
        json.dump({"testsuites": suites}, handle)
    return Path(handle.name)


def _run(path):
    """main() over one report. Returns (rc, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    argv = sys.argv
    sys.argv = ["check_test_counts.py", str(path)]
    try:
        with redirect_stdout(out), redirect_stderr(err):
            rc = ctc.main()
    finally:
        sys.argv = argv
    return rc, out.getvalue(), err.getvalue()


def _green_plan(floors):
    """(scenario name, cases) for a run that clears every floor: one suite per
    min_cases.* key, named as twister names it and holding max(per, floor)
    cases, topped up with plain scenario.N suites until min_scenarios is met.
    """
    per = floors["min_testcases"] // floors["min_scenarios"] + 1
    plan = [(k[len("min_cases."):], max(per, v))
            for k, v in sorted(floors.items()) if k.startswith("min_cases.")]
    i = 0
    while len(plan) < floors["min_scenarios"]:
        plan.append((f"scenario.{i}", per))
        i += 1
    return plan


def _suites_from(plan, extra_cases=()):
    return [
        {
            "name": f"app/some/tests/{name}",
            "status": "passed",
            "testcases": ([_case(f"{name}.t{j}", "passed") for j in range(n)] +
                          [_case(f"{name}.{c}", c) for c in extra_cases]),
        }
        for name, n in plan
    ]


def _green_report():
    """A run that would pass: enough scenarios, enough executed cases, every
    per-scenario floor met."""
    return _report(_suites_from(_green_plan(ctc.read_expected())))


class BuildFailureIsReportedAsOne(unittest.TestCase):
    """The shape of the red run: seven scenarios errored, two ran."""

    def setUp(self):
        floors = ctc.read_expected()
        broken = [
            {
                "name": f"radiant.unit.{i}",
                "status": "error",
                "reason": "CMake build failure",
                "testcases": [_case(f"radiant.unit.{i}.case{j}", "blocked")
                              for j in range(300)],
            }
            for i in range(floors["min_scenarios"] - 2)
        ]
        ran = [
            {
                "name": f"ok.{i}",
                "status": "passed",
                "testcases": [_case(f"ok.{i}.case{j}", "passed")
                              for j in range(8)],
            }
            for i in range(2)
        ]
        self.path = _report(broken + ran)

    def tearDown(self):
        self.path.unlink(missing_ok=True)

    def test_it_fails(self):
        rc, _, _ = _run(self.path)
        self.assertEqual(rc, 1)

    def test_it_leads_with_the_build_failure_and_its_reason(self):
        _, out, err = _run(self.path)
        self.assertIn("BUILD FAILURE", out)
        self.assertIn("CMake build failure", out)
        # Above the counts table, not buried under it.
        self.assertLess(out.index("BUILD FAILURE"), out.index("ztest counts"))
        self.assertIn("BUILD FAILURE", err)

    def test_blocked_is_not_reported_as_a_failed_assertion(self):
        _, out, err = _run(self.path)
        # 2100 blocked cases must not be summarised as 2100 failures.
        self.assertNotIn("2100 test case(s) failed", err)
        self.assertIn("blocked and never ran", err)
        self.assertIn("| failed | 0 |", out)

    def test_the_floor_is_not_satisfied_by_cases_that_never_ran(self):
        """The blind spot itself: 2116 discovered, 16 executed, floor 874."""
        _, _, err = _run(self.path)
        self.assertIn("actually executed", err)


class SuiteLevelStatusBlocked(unittest.TestCase):
    """Twister marks a suite `blocked` when its build's dependency failed."""

    def test_it_is_treated_as_a_build_failure(self):
        path = _report([{
            "name": "radiant.unit",
            "status": "blocked",
            "reason": "dependency build failed",
            "testcases": [_case("radiant.unit.a", "blocked")],
        }])
        try:
            rc, out, _ = _run(path)
        finally:
            path.unlink(missing_ok=True)
        self.assertEqual(rc, 1)
        self.assertIn("dependency build failed", out)


class AGenuinelyGreenRunStillPasses(unittest.TestCase):
    def test_it(self):
        path = _green_report()
        try:
            rc, out, err = _run(path)
        finally:
            path.unlink(missing_ok=True)
        self.assertEqual(rc, 0, err)
        self.assertNotIn("BUILD FAILURE", out)
        self.assertIn("all passed", out)


class SkippedAndFilteredCountTowardNeither(unittest.TestCase):
    """Unchanged behaviour, pinned so the rewrite cannot have moved it."""

    def test_it(self):
        plan = _green_plan(ctc.read_expected())
        path = _report(_suites_from(plan, extra_cases=("skipped", "filtered")))
        try:
            rc, out, err = _run(path)
        finally:
            path.unlink(missing_ok=True)
        self.assertEqual(rc, 0, err)
        expected = sum(n for _, n in plan)
        self.assertIn(f"| test cases executed | {expected} |", out)


class APerScenarioFloorIsEnforced(unittest.TestCase):
    """E3-1 (review 2026-10-31): the global floor had ~1.7k cases of slack,
    so a whole suite in one scenario could vanish under it. One case short of
    a single scenario's floor must fail, with the scenario named."""

    def test_one_short_fails(self):
        floors = ctc.read_expected()
        plan = _green_plan(floors)
        keyed = [i for i, (name, _) in enumerate(plan)
                 if f"min_cases.{name}" in floors]
        self.assertTrue(keyed, "no min_cases.* floors to test against")
        i = keyed[0]
        name, _ = plan[i]
        plan[i] = (name, floors[f"min_cases.{name}"] - 1)
        # Keep the global floor met, so only the per-scenario one can trip.
        plan.append(("scenario.extra", floors["min_testcases"]))
        path = _report(_suites_from(plan))
        try:
            rc, _, err = _run(path)
        finally:
            path.unlink(missing_ok=True)
        self.assertEqual(rc, 1)
        self.assertIn(f"scenario {name} executed", err)


class AFilteredScenarioIsNotCoverage(unittest.TestCase):
    """A scenario whose every case was filtered did not run; it must not count
    toward min_scenarios (it used to: the name was counted before any
    status was read)."""

    def test_it(self):
        floors = ctc.read_expected()
        plan = _green_plan(floors)
        suites = _suites_from(plan)
        suites.append({
            "name": "app/some/tests/only.filtered",
            "status": "filtered",
            "testcases": [_case("only.filtered.t0", "filtered")],
        })
        path = _report(suites)
        try:
            rc, out, err = _run(path)
        finally:
            path.unlink(missing_ok=True)
        self.assertEqual(rc, 0, err)
        self.assertIn(f"| scenarios | {len(plan)} |", out)


class ARealFailedAssertionStillFails(unittest.TestCase):
    def test_it(self):
        floors = ctc.read_expected()
        per = floors["min_testcases"] // floors["min_scenarios"] + 1
        suites = [
            {
                "name": f"scenario.{i}",
                "status": "passed",
                "testcases": [_case(f"s{i}.t{j}", "passed")
                              for j in range(per)],
            }
            for i in range(floors["min_scenarios"])
        ]
        suites[0]["testcases"].append(_case("scenario.0.bad", "failed"))
        path = _report(suites)
        try:
            rc, _, err = _run(path)
        finally:
            path.unlink(missing_ok=True)
        self.assertEqual(rc, 1)
        self.assertIn("1 test case(s) failed", err)
        self.assertIn("scenario.0.bad [failed]", err)


if __name__ == "__main__":
    unittest.main()
