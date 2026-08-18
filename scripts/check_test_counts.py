# SPDX-License-Identifier: Apache-2.0

"""Assert the ztest job actually ran the tests it is supposed to run.

`twister -T app/radiant/tests` discovers test applications by walking for
testcase.yaml. That means a renamed, moved or malformed testcase.yaml silently
removes an entire scenario - and twister still exits 0, because from its point
of view there was simply less to do. A green check that quietly stopped running
a third of the suite is worse than a red one.

So this reads twister.json and compares against tests/expected_counts.yaml,
which holds floors rather than exact counts: tests get added far more often
than removed, and an exact count would fail on every honest addition until
somebody relaxed it permanently.

It reads twister.json at BOTH levels, and that is not incidental. A scenario
that failed to compile has a suite-level status of `error` with the compiler
output in `reason`, and all of its statically-discovered cases come back
`blocked` - never run, nothing asserted. Reading only the case level turned one
compiler error into "2464 test case(s) failed" and printed twenty `[blocked]`
names instead of the error. So: a build failure is reported as a build failure,
above everything else; `blocked` is counted apart from `failed`; and the
testcase floor is compared against cases that EXECUTED, since discovery happens
before the build and a wholly-broken run otherwise cleared the floor it exists
to hold.

It also prints a table for $GITHUB_STEP_SUMMARY, so the numbers are visible on
the run page rather than buried in twister-out/.

Standard library only - no PyYAML. The expectations file is flat `key: value`
and parsing it directly avoids depending on a package that is present only
because twister happens to pull it in.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EXPECTED = REPO / "tests" / "expected_counts.yaml"


def read_expected() -> dict[str, int]:
    if not EXPECTED.is_file():
        raise SystemExit(f"No expectations file at {EXPECTED}")
    out: dict[str, int] = {}
    for line in EXPECTED.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        m = re.match(r"^([a-z_]+)\s*:\s*(\d+)$", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    for key in ("min_scenarios", "min_testcases"):
        if key not in out:
            raise SystemExit(f"{EXPECTED} is missing {key}")
    return out


class Summary:
    """What one twister.json says, at both levels it says it at.

    The two levels are the whole point. A suite that failed to COMPILE has a
    suite-level status of `error` with the compiler output in `reason`, and its
    statically-discovered cases all come back `blocked` - never run, nothing
    asserted. Reading only the case level turned one compiler error into "2464
    test case(s) failed", which is a different problem with a different fix.
    """

    def __init__(self) -> None:
        self.scenarios: set[str] = set()
        self.executed = 0        # passed + failed: cases that actually ran
        self.passed = 0
        self.failed = 0
        self.blocked = 0
        self.failing: list[str] = []
        self.blocked_names: list[str] = []
        self.build_failures: list[tuple[str, str]] = []


# Suite-level statuses that mean "this scenario never got as far as running".
# `error` is twister's build failure; `blocked` appears when a dependency of
# the build failed.
SUITE_BROKEN = ("error", "blocked")


def summarise(report: Path) -> Summary:
    # utf-8-sig, not utf-8: a BOM is invalid JSON to json.loads, and anything
    # that rewrites this file on Windows is liable to add one.
    data = json.loads(report.read_text(encoding="utf-8-sig"))
    suites = data.get("testsuites", [])

    s = Summary()

    for suite in suites:
        name = suite.get("name", "?")
        s.scenarios.add(name)

        suite_status = (suite.get("status") or "").lower()
        if suite_status in SUITE_BROKEN:
            reason = (suite.get("reason") or "no reason given").strip()
            s.build_failures.append((name, reason))

        for case in suite.get("testcases", []):
            status = (case.get("status") or "").lower()
            # Twister reports skipped/filtered cases too; they are not failures
            # and not evidence of coverage, so they count toward neither.
            if status in ("skipped", "filtered", "none", ""):
                continue
            if status == "passed":
                s.executed += 1
                s.passed += 1
            elif status == "blocked":
                # Blocked is "never ran", not "ran and disagreed". Counting it
                # as failed is what made a build break read as a mass
                # regression, and counting it as executed is what let the
                # coverage floor pass on a run where nothing executed.
                s.blocked += 1
                s.blocked_names.append(
                    f"{name}: {case.get('identifier', '?')}")
            else:
                s.executed += 1
                s.failed += 1
                s.failing.append(
                    f"{name}: {case.get('identifier', '?')} [{status}]")

    return s


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", nargs="?",
                        default="twister-out/twister.json",
                        help="path to twister.json")
    args = parser.parse_args()

    report = Path(args.report)
    if not report.is_file():
        print(f"No twister report at {report}. The ztest job must run twister "
              f"before this check.", file=sys.stderr)
        return 1

    expected = read_expected()
    s = summarise(report)
    n_scen = len(s.scenarios)

    # The build verdict goes FIRST, above the table, in both the terminal and
    # the step summary. A compiler error buried under twenty [blocked] case
    # names is a compiler error nobody reads.
    head: list[str] = []
    if s.build_failures:
        head.append("### BUILD FAILURE")
        head.append("")
        head.append(f"{len(s.build_failures)} scenario(s) never ran because "
                    f"the build did not complete:")
        head.append("")
        for name, reason in s.build_failures:
            head.append(f"- **{name}**: {reason}")
        head.append("")

    lines = head + [
        "### ztest counts",
        "",
        "| | observed | floor |",
        "|---|---|---|",
        f"| scenarios | {n_scen} | {expected['min_scenarios']} |",
        f"| test cases executed | {s.executed} | {expected['min_testcases']} |",
        f"| passed | {s.passed} | |",
        f"| failed | {s.failed} | 0 |",
        f"| blocked (never ran) | {s.blocked} | 0 |",
    ]
    table = "\n".join(lines)
    print(table)

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as handle:
            handle.write(table + "\n")

    problems: list[str] = []
    for name, reason in s.build_failures:
        problems.append(f"BUILD FAILURE in {name}: {reason}")
    if n_scen < expected["min_scenarios"]:
        problems.append(
            f"only {n_scen} scenarios ran, expected at least "
            f"{expected['min_scenarios']}. A whole test application has "
            f"probably stopped being discovered - check that every "
            f"testcase.yaml under radiant/tests is still valid."
        )
    # Against EXECUTED, not against every case twister discovered. Discovery
    # happens before the build, so a run where nothing compiled still lists
    # thousands of cases - and the floor reported green on exactly that.
    if s.executed < expected["min_testcases"]:
        problems.append(
            f"only {s.executed} test cases actually executed, expected at "
            f"least {expected['min_testcases']}. Coverage went down; if that "
            f"is deliberate, lower the floor in tests/expected_counts.yaml in "
            f"the same commit and say why."
        )
    if s.failed:
        problems.append(f"{s.failed} test case(s) failed")
        problems.extend(f"  {f}" for f in s.failing[:20])
    if s.blocked:
        problems.append(f"{s.blocked} test case(s) were blocked and never ran")
        problems.extend(f"  {b}" for b in s.blocked_names[:20])

    if problems:
        print("\ncheck_test_counts: FAILED", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1

    print(f"\ncheck_test_counts: OK - {s.executed} cases in {n_scen} "
          f"scenarios, all passed")
    if s.executed > expected["min_testcases"]:
        print(f"  (floor is {expected['min_testcases']}; it can be raised to "
              f"{s.executed} on this evidence)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
