#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""H3: every archive/benchmarks/20??-*.json validates against
baseline.schema.json.

Lands with H1 (the cc26xx baseline repair) so hand-written drift can't
re-enter: without this test, a baseline that fails validation is only
discovered the next time somebody happens to run tools/ant_ab.py against it,
which is exactly how 2026-08-13-radiant-cc26xx.json went unnoticed.

Uses tools/ant_ab.py's own hand-written validator - the one the CI
`host-tests` job actually exercises against these files - rather than the
`jsonschema` package, for the same reason ant_ab.py's own docstring gives:
the fork-safe CI job installs pyusb and nothing else.
"""

from __future__ import annotations

import glob
import json
import os
import unittest

import ant_ab

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE_DIR = os.path.join(HERE, "..", "archive", "benchmarks")
SCHEMA_PATH = os.path.join(ARCHIVE_DIR, "baseline.schema.json")

with open(SCHEMA_PATH, encoding="utf-8") as _handle:
    SCHEMA = json.load(_handle)

# Every dated baseline. archive/benchmarks/ also holds baseline.schema.json
# and README.md, neither of which is a baseline - the 20??-* glob is what
# baseline.schema.json's own $id and this project's naming convention both
# use to mean "a recorded sitting".
BASELINE_PATHS = sorted(
    glob.glob(os.path.join(ARCHIVE_DIR, "20??-*.json")))


class EveryArchivedBaselineValidates(unittest.TestCase):
    """A parameterised-by-hand test per file, so one bad baseline names
    itself in the failure rather than hiding inside a loop's single
    assertion."""

    def test_at_least_one_baseline_was_found(self):
        # A glob that silently matched nothing would make every test below
        # vacuously pass, which is worse than no test at all.
        self.assertGreater(len(BASELINE_PATHS), 0,
                           f"no 20??-*.json files under {ARCHIVE_DIR!r} - "
                           f"the glob is broken or the archive is empty")


def _make_test(path: str):
    def test(self):
        with open(path, encoding="utf-8-sig") as handle:
            data = json.load(handle)
        errors = ant_ab.validate(data, SCHEMA, path=os.path.basename(path))
        self.assertEqual(errors, [],
                         f"{path} does not validate against the baseline "
                         f"schema:\n  " + "\n  ".join(errors))
    return test


for _path in BASELINE_PATHS:
    _name = "test_" + os.path.basename(_path).replace(".", "_").replace(
        "-", "_")
    setattr(EveryArchivedBaselineValidates, _name, _make_test(_path))


if __name__ == "__main__":
    unittest.main()
