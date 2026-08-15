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


def summarise(report: Path) -> tuple[int, int, int, int, list[str]]:
    # utf-8-sig, not utf-8: a BOM is invalid JSON to json.loads, and anything
    # that rewrites this file on Windows is liable to add one.
    data = json.loads(report.read_text(encoding="utf-8-sig"))
    suites = data.get("testsuites", [])

    scenarios: set[str] = set()
    total = passed = failed = 0
    failing: list[str] = []

    for suite in suites:
        name = suite.get("name", "?")
        scenarios.add(name)
        for case in suite.get("testcases", []):
            status = (case.get("status") or "").lower()
            # Twister reports skipped/filtered cases too; they are not failures
            # and not evidence of coverage, so they count toward neither.
            if status in ("skipped", "filtered", "none", ""):
                continue
            total += 1
            if status == "passed":
                passed += 1
            else:
                failed += 1
                failing.append(f"{name}: {case.get('identifier', '?')} [{status}]")

    return len(scenarios), total, passed, failed, failing


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
    n_scen, total, passed, failed, failing = summarise(report)

    lines = [
        "### ztest counts",
        "",
        "| | observed | floor |",
        "|---|---|---|",
        f"| scenarios | {n_scen} | {expected['min_scenarios']} |",
        f"| test cases | {total} | {expected['min_testcases']} |",
        f"| passed | {passed} | |",
        f"| failed | {failed} | 0 |",
    ]
    table = "\n".join(lines)
    print(table)

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as handle:
            handle.write(table + "\n")

    problems: list[str] = []
    if n_scen < expected["min_scenarios"]:
        problems.append(
            f"only {n_scen} scenarios ran, expected at least "
            f"{expected['min_scenarios']}. A whole test application has "
            f"probably stopped being discovered - check that every "
            f"testcase.yaml under radiant/tests is still valid."
        )
    if total < expected["min_testcases"]:
        problems.append(
            f"only {total} test cases ran, expected at least "
            f"{expected['min_testcases']}. Coverage went down; if that is "
            f"deliberate, lower the floor in tests/expected_counts.yaml in the "
            f"same commit and say why."
        )
    if failed:
        problems.append(f"{failed} test case(s) failed")
        problems.extend(f"  {f}" for f in failing[:20])

    if problems:
        print("\ncheck_test_counts: FAILED", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1

    print(f"\ncheck_test_counts: OK - {total} cases in {n_scen} scenarios, "
          f"all passed")
    if total > expected["min_testcases"]:
        print(f"  (floor is {expected['min_testcases']}; it can be raised to "
              f"{total} on this evidence)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
