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


def _green_report(n_scenarios, cases_each):
    """A run that would pass: enough scenarios, enough executed cases."""
    return _report([
        {
            "name": f"scenario.{i}",
            "status": "passed",
            "testcases": [_case(f"s{i}.t{j}", "passed")
                          for j in range(cases_each)],
        }
        for i in range(n_scenarios)
    ])


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
        floors = ctc.read_expected()
        per = floors["min_testcases"] // floors["min_scenarios"] + 1
        path = _green_report(floors["min_scenarios"], per)
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
        floors = ctc.read_expected()
        per = floors["min_testcases"] // floors["min_scenarios"] + 1
        suites = [
            {
                "name": f"scenario.{i}",
                "status": "passed",
                "testcases": (
                    [_case(f"s{i}.t{j}", "passed") for j in range(per)] +
                    [_case(f"s{i}.skip", "skipped"),
                     _case(f"s{i}.filt", "filtered")]
                ),
            }
            for i in range(floors["min_scenarios"])
        ]
        path = _report(suites)
        try:
            rc, out, err = _run(path)
        finally:
            path.unlink(missing_ok=True)
        self.assertEqual(rc, 0, err)
        expected = per * floors["min_scenarios"]
        self.assertIn(f"| test cases executed | {expected} |", out)


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
