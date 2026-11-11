#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Host tests for tools/ant_disc_phase.py.

    python -m unittest discover -s tools -p "test_*.py"

The real run needs three radios. What the host can assert is the parsing, the
phase arithmetic, and that a DUT image without the diagnostic fails the run
instead of recording empty trials.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import ant_disc_phase as d  # noqa: E402
except SystemExit:  # pyusb missing: ant_probe exits rather than raising
    raise unittest.SkipTest("pyusb is not installed") from None

FRAME = ("[00:01:02.003,004] <inf> radiant_api: TDIAG frame dev=5005 type=0x78 "
         "tt=1 ph=38921 set=7 claim=1 nact=2 nmatch=1 ch0=2")
CHUNKS = ("<inf> radiant_api: TEMPDIAG chunks: ok=3 aborted=9 other=0 "
          "credited_ms=80 phase_bins=0x003fcffc sets=4 ok_frames=2 acq=1 "
          "idrej=0 lost=0")


class ArgsTest(unittest.TestCase):

    def test_a_master_needs_exactly_one_of_serial_or_port(self):
        base = ["out", "--bridge", "COM4", "--log", "COM3",
                "--b-serial", "X"]
        with self.assertRaises(SystemExit):
            d.parse_args(base)
        with self.assertRaises(SystemExit):
            d.parse_args(base + ["--a-serial", "Y", "--a-port", "COM5"])

    def test_no_default_ports(self):
        with self.assertRaises(SystemExit):
            d.parse_args(["out", "--a-serial", "Y", "--b-serial", "X"])

    def test_ch0_defaults_to_master_a(self):
        a = d.parse_args(["out", "--a-serial", "Y", "--b-port", "COM5",
                          "--bridge", "COM4", "--log", "COM3",
                          "--a-num", "2002"])
        self.assertEqual(2002, a.ch0)
        self.assertEqual("COM5", a.b_port)


class ParseTest(unittest.TestCase):

    def test_frames_by_dev(self):
        got = d.frames_by_dev([FRAME, FRAME, "unrelated"])
        self.assertEqual(2, got["5005"]["n"])
        self.assertEqual([38921, 38921], got["5005"]["ph_us"])
        self.assertEqual([1], got["5005"]["claims"])

    def test_phase_bin_masks(self):
        self.assertEqual(["0x003fcffc"],
                         d.phase_bin_masks([CHUNKS, "unrelated"]))


class SummaryTest(unittest.TestCase):

    def test_summarize_counts_found_on_channel_one(self):
        results = [
            {"trial": 0, "delay_ms": 0.0, "host_phase_ms": 2.0,
             "frames_by_dev": {"5005": {"n": 3}}, "masks": ["0x1"],
             "channels": [{"ch": 0, "packets": 200},
                          {"ch": 1, "packets": 180, "first_s": 8.8}]},
            {"trial": 1, "delay_ms": 15.0, "host_phase_ms": 30.0,
             "frames_by_dev": {}, "masks": ["0x2"],
             "channels": [{"ch": 0, "packets": 200},
                          {"ch": 1, "packets": 0, "first_s": None}]},
        ]
        got = d.summarize(results, target_dev=5005)
        self.assertEqual(1, got["found"])
        self.assertEqual(2, got["total"])
        self.assertEqual(["0x1", "0x2"], got["masks"])
        self.assertEqual(3, got["trials"][0]["tdiag_frames_of_b"])
        self.assertEqual(0, got["trials"][1]["tdiag_frames_of_b"])


class WrongDeviceTest(unittest.TestCase):

    def test_a_wildcard_bound_to_a_stranger_is_not_found(self):
        results = [{"trial": 0, "delay_ms": 0.0, "host_phase_ms": 2.0,
                    "frames_by_dev": {}, "masks": [],
                    "channels": [{"ch": 1, "packets": 180, "first_s": 5.0,
                                  "bound_dev": 8265}]}]
        got = d.summarize(results, target_dev=5005)
        self.assertEqual(0, got["found"])
        self.assertFalse(got["trials"][0]["found"])

    def test_a_wildcard_bound_to_b_is_found(self):
        results = [{"trial": 0, "delay_ms": 0.0, "host_phase_ms": 2.0,
                    "frames_by_dev": {}, "masks": [],
                    "channels": [{"ch": 1, "packets": 180, "first_s": 5.0,
                                  "bound_dev": 5005}]}]
        self.assertEqual(1, d.summarize(results, target_dev=5005)["found"])


class PhaseTest(unittest.TestCase):

    def test_phase_of_b_after_a_is_recovered(self):
        a = [i * d.PERIOD_S for i in range(50)]
        b = [t + 0.040 for t in a]
        ph, sd = d.phase_ms(a, b)
        self.assertAlmostEqual(40.0, ph, places=6)
        self.assertAlmostEqual(0.0, sd, places=6)

    def test_too_few_events_is_none(self):
        self.assertEqual((None, None), d.phase_ms([1.0], [1.2, 1.4]))


class DelaysTest(unittest.TestCase):

    def test_seed_is_deterministic_and_keeps_values(self):
        got = d.delays_for("0,15,30,45", 3)
        self.assertEqual(sorted(got), [0.0, 15.0, 30.0, 45.0])
        self.assertEqual(got, d.delays_for("0,15,30,45", 3))

    def test_random_form(self):
        self.assertEqual(4, len(d.delays_for("random:4", None)))


class FakeLog:
    """Just enough of Log for require_diag()."""

    def __init__(self, lines):
        self._lines = lines

    def since(self, _mark, pattern):
        return [line for line in self._lines if pattern.search(line)]


class RequireDiagTest(unittest.TestCase):

    def test_an_image_that_emits_neither_line_fails_the_run(self):
        notes = []
        with self.assertRaises(SystemExit) as cm:
            d.require_diag(FakeLog(["unrelated"]), notes.append)
        self.assertEqual(2, cm.exception.code)
        self.assertIn("TDIAG", notes[0])

    def test_either_line_passes(self):
        d.require_diag(FakeLog([FRAME]), lambda _m: None)
        d.require_diag(FakeLog([CHUNKS]), lambda _m: None)


BASE = ["out", "--a-serial", "Y", "--b-serial", "X", "--bridge", "COM4",
        "--log", "COM3"]


class R1WalkTest(unittest.TestCase):
    """--r1-walk: acquire B clear of A, then walk its phase by period."""

    def test_walk_and_plain_r1_are_exclusive(self):
        with self.assertRaises(SystemExit):
            d.parse_args(BASE + ["--r1", "--r1-walk"])

    def test_walk_step_is_bounded(self):
        with self.assertRaises(SystemExit):
            d.parse_args(BASE + ["--r1-walk", "--r1-step-counts", "0"])
        a = d.parse_args(BASE + ["--r1-walk"])
        self.assertEqual(2, a.r1_step_counts)
        self.assertEqual(40.0, a.r1_start_ms)
        self.assertEqual(12.0, a.r1_target_ms)

    def test_circular_difference_takes_the_short_way(self):
        self.assertAlmostEqual(-28.0, d.circ_diff_ms(12.0, 40.0))
        self.assertAlmostEqual(10.0, d.circ_diff_ms(5.0, d.PERIOD_MS - 5.0),
                               places=6)
        self.assertAlmostEqual(-10.0, d.circ_diff_ms(d.PERIOD_MS - 5.0, 5.0),
                               places=6)

    def test_walking_earlier_runs_a_shorter_period(self):
        period, n = d.walk_plan(40.0, 12.0, 2)
        self.assertEqual(8068, period)
        # 28 ms at 2 counts (61.04 us) a period
        self.assertAlmostEqual(28.0 / (2 / 32768.0 * 1000.0), n, places=3)
        period, _ = d.walk_plan(12.0, 40.0, 3)
        self.assertEqual(8073, period)

    def test_packets_are_counted_in_the_window_only(self):
        recs = [(0.5, 0x78, 2002, b""), (10.0, 0x78, 2002, b""),
                (10.1, 0x78, 5005, b""), (19.9, 0x78, 5005, b""),
                (20.0, 0x78, 5005, b"")]
        counts = d.count_in_window(recs, 1000.0, 1010.0, 1020.0)
        self.assertEqual({2002: 1, 5005: 2}, counts)

    def test_window_loss_against_the_hr_period(self):
        secs = 60.0
        full = round(secs / d.PERIOD_S)
        self.assertLess(d.window_loss_pct(full, secs), 0.5)
        self.assertAlmostEqual(100.0, d.window_loss_pct(0, secs))
        self.assertAlmostEqual(50.0, d.window_loss_pct(full / 2, secs), places=0)

    def test_the_capture_listen_start_is_read_from_its_comment(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "c.antser"
            p.write_text("# ant capture v1\n# listen_start_unix=1731200000.250000\n"
                         "0.1 120 5005 0000000000000000\n", encoding="utf-8")
            self.assertAlmostEqual(1731200000.25, d.capture_listen_start(p))

    def test_acquisition_needs_channel_1_to_claim_b(self):
        mine = FRAME  # dev=5005 claim=1
        other = FRAME.replace("claim=1", "claim=-1")
        self.assertTrue(d.acquired(FakeLog([mine]), 0, 5005))
        self.assertFalse(d.acquired(FakeLog([other]), 0, 5005))
        self.assertFalse(d.acquired(FakeLog([mine]), 0, 2002))

    def test_the_walk_steers_by_recent_events_only(self):
        a = [i * d.PERIOD_S for i in range(100)]
        b = [t + 0.040 for t in a[:50]] + [t + 0.012 for t in a[50:]]
        self.assertAlmostEqual(12.0, d.phase_now_ms(a, b), places=3)


if __name__ == "__main__":
    unittest.main()
