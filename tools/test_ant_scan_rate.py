# SPDX-License-Identifier: Apache-2.0

"""tools/ant_scan_rate.py's arithmetic, with no device.

The numbers that get compared across an A/B are sightings/s and the median
time to first sighting after an open. Both are easy to get subtly wrong in a
way that flatters the dongle: dividing by wall time instead of listening time
(the reopen gaps are deaf), or taking the median over only the windows a
device was heard in. These pin both.
"""

from __future__ import annotations

import argparse
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ant_scan_rate as sr  # noqa: E402


class ParseTrack(unittest.TestCase):
    def test_default_period_from_the_device_type(self):
        t = sr.parse_track("20329:0x0B")
        self.assertEqual((t.device_number, t.device_type, t.period),
                         (20329, 0x0B, 8182))
        self.assertEqual(sr.parse_track("1458:120").period, 8070)

    def test_explicit_period_wins(self):
        self.assertEqual(sr.parse_track("52233:0x11:4096").period, 4096)

    def test_key_is_stable(self):
        self.assertEqual(sr.parse_track("1458:0x78").key, "1458:0x78")

    def test_rejects(self):
        for bad in ("0:0x78", "1458", "1458:0x78:1:2", "x:0x78",
                    "1458:0x99", "70000:0x78", "1:0x05"):
            with self.subTest(bad=bad):
                with self.assertRaises(argparse.ArgumentTypeError):
                    sr.parse_track(bad)


class Windows(unittest.TestCase):
    def test_windows_pair_each_open_with_the_next_close(self):
        log = sr.ScanLog(end=20.0, opens=[0.1, 5.2, 10.3],
                         closes=[5.0, 10.0])
        self.assertEqual(log.windows(), [(0.1, 5.0), (5.2, 10.0),
                                         (10.3, 20.0)])
        self.assertAlmostEqual(log.listening_seconds(), 4.9 + 4.8 + 9.7)

    def test_first_after_open_keeps_misses(self):
        windows = [(0.0, 5.0), (5.5, 10.0), (10.5, 15.0)]
        times = [0.3, 2.0, 11.0]
        self.assertEqual(sr.first_after_open(times, windows),
                         [0.3, None, 0.5])

    def test_a_sighting_in_the_reopen_gap_belongs_to_no_window(self):
        self.assertEqual(sr.first_after_open([5.2], [(0.0, 5.0),
                                                     (5.5, 10.0)]),
                         [None, None])


class Median(unittest.TestCase):
    def test_plain(self):
        self.assertEqual(sr.median_with_misses([0.3, 0.1, 0.2]), 0.2)
        self.assertEqual(sr.median_with_misses([0.1, 0.3]), 0.2)

    def test_misses_count_as_slowest(self):
        # Heard in 2 of 3: the median is the slower hit, not the faster.
        self.assertEqual(sr.median_with_misses([0.1, None, 0.4]), 0.4)

    def test_missed_in_most_windows_has_no_median(self):
        self.assertIsNone(sr.median_with_misses([0.1, None, None]))
        self.assertIsNone(sr.median_with_misses([0.1, None]))
        self.assertIsNone(sr.median_with_misses([]))


class PlanChildren(unittest.TestCase):
    def test_port_joins_the_serial_children_last(self):
        self.assertEqual(
            sr.plan_children(["006", "EE69"], "COM11", 57600),
            [("006", ["--serial", "006"]),
             ("EE69", ["--serial", "EE69"]),
             ("COM11", ["--port", "COM11", "--baud", "57600"])])

    def test_one_device_is_not_a_multi_run(self):
        self.assertEqual(len(sr.plan_children([], "COM11", 115200)), 1)
        self.assertEqual(len(sr.plan_children(["006"], None, 115200)), 1)
        self.assertEqual(len(sr.plan_children([], None, 115200)), 0)

    def test_baud_reaches_only_the_port_child(self):
        for label, dev in sr.plan_children(["006"], "COM11", 57600):
            self.assertEqual("--baud" in dev, label == "COM11")

    def test_child_command_carries_the_shared_start_and_options(self):
        args = argparse.Namespace(seconds=60.0, reopen=4.5,
                                  track=[sr.parse_track("52233:0x11")],
                                  scan_type=0x40)
        cmd = sr._child_args(args, ["--port", "COM11", "--baud", "57600"],
                             "out.json", 123.5)
        tail = cmd[2:]
        self.assertEqual(tail[:4], ["--port", "COM11", "--baud", "57600"])
        self.assertIn("--start-at", tail)
        self.assertEqual(tail[tail.index("--start-at") + 1], "123.5")
        self.assertEqual(tail[tail.index("--track") + 1], "52233:17:8192")
        self.assertEqual(tail[tail.index("--reopen") + 1], "4.5")
        self.assertNotIn("--serial", tail)
        # A child must assign the same scan type as its siblings, or a
        # same-window A/B compares two different channel configurations.
        self.assertEqual(tail[tail.index("--scan-type") + 1], "0x40")


class Summarise(unittest.TestCase):
    def test_rate_is_per_listening_second_not_wall_second(self):
        log = sr.ScanLog(end=10.0, opens=[0.0, 6.0], closes=[4.0])
        log.sightings = {"1458:0x78": [1.0, 2.0, 3.0, 7.0, 8.0, 9.0]}
        s = sr.summarise(log)
        # 8 s listening (0-4, 6-10), not 10.
        self.assertAlmostEqual(s["listening_s"], 8.0)
        self.assertAlmostEqual(s["devices"]["1458:0x78"]["per_s"], 6 / 8)
        self.assertAlmostEqual(s["sightings_per_s"], 6 / 8)
        self.assertEqual(s["devices"]["1458:0x78"]["opens_heard"], 2)
        self.assertAlmostEqual(
            s["devices"]["1458:0x78"]["first_after_open_median_s"], 1.0)

    def test_tracked_loss(self):
        log = sr.ScanLog(end=1.0, opens=[0.0])
        log.tracked = {"20329:0x0B": {"channel": 1, "received": 90,
                                      "rx_fail": 10}}
        s = sr.summarise(log)
        self.assertAlmostEqual(s["tracked"]["20329:0x0B"]["loss_pct"], 10.0)
        self.assertIsNone(sr.loss(0, 0))

    def test_render_does_not_raise_on_a_failed_dongle(self):
        log = sr.ScanLog(end=5.0, opens=[0.0])
        log.sightings = {"1:0x78": [1.0]}
        text = sr.render({"A": {"version": "X", "summary": sr.summarise(log)},
                          "B": {"error": "no device"}})
        self.assertIn("B: FAILED - no device", text)
        self.assertIn("1:0x78", text)


if __name__ == "__main__":
    unittest.main()
