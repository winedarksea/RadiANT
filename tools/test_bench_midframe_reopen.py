#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Off-target tests for tools/bench_midframe_reopen.py. No device is opened.

    python -m unittest discover -s tools -p "test_*.py"

The bench itself needs a dongle. What can go wrong without one is the frame
the dying child writes, the argument handling, the child's command line and
the N/M arithmetic - any of which would otherwise surface as a crash or a
wrong verdict partway through a bench run.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_midframe_reopen as b  # noqa: E402
from ant_wire import unframe  # noqa: E402


class TheRequest(unittest.TestCase):
    def test_is_a_capabilities_request(self):
        # A4 02 4D 00 54 <checksum>: request message 0x54 on channel 0.
        self.assertEqual(b.CAPABILITIES_REQUEST[:5],
                         bytes([0xA4, 0x02, 0x4D, 0x00, 0x54]))
        self.assertEqual(unframe(b.CAPABILITIES_REQUEST),
                         (0x4D, bytes([0x00, 0x54])))


class PartialFrame(unittest.TestCase):
    def test_default_is_sync_length_id_first_byte(self):
        self.assertEqual(b.partial_frame(b.DEFAULT_PARTIAL_BYTES),
                         bytes([0xA4, 0x02, 0x4D, 0x00]))

    def test_is_a_prefix_and_never_a_whole_frame_below_full_length(self):
        full = b.CAPABILITIES_REQUEST
        for n in range(1, len(full)):
            part = b.partial_frame(n)
            self.assertEqual(part, full[:n])
            self.assertIsNone(unframe(part), f"{n} bytes parsed as a frame")

    def test_full_length_is_the_control_arm(self):
        self.assertEqual(b.partial_frame(len(b.CAPABILITIES_REQUEST)),
                         b.CAPABILITIES_REQUEST)

    def test_out_of_range_is_refused(self):
        for n in (0, -1, len(b.CAPABILITIES_REQUEST) + 1):
            with self.assertRaises(ValueError):
                b.partial_frame(n)


class Arguments(unittest.TestCase):
    def parse_error(self, argv):
        with contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as raised:
            b.parse_args(argv)
        self.assertEqual(raised.exception.code, 2)

    def test_defaults(self):
        args = b.parse_args([])
        self.assertEqual(args.partial_bytes, 4)
        self.assertEqual(args.reply_timeout, 1.0)
        self.assertFalse(args.child)
        self.assertIsNone(args.json)
        self.assertLess(args.delay_min, args.delay_max)

    def test_partial_bytes_type(self):
        self.assertEqual(b.partial_bytes_arg("1"), 1)
        self.assertEqual(b.partial_bytes_arg("6"), 6)
        for text in ("0", "7", "", "four", "4.0"):
            with self.assertRaises(argparse.ArgumentTypeError):
                b.partial_bytes_arg(text)

    def test_refusals(self):
        self.parse_error(["--partial-bytes", "0"])
        self.parse_error(["--serial", "ABC", "--port", "COM3"])
        self.parse_error(["--count", "0"])
        self.parse_error(["--reply-timeout", "0"])
        self.parse_error(["--delay-min", "1", "--delay-max", "0.5"])
        self.parse_error(["--delay-min", "-0.1"])

    def test_child_inherits_the_device_selection(self):
        args = b.parse_args(["--serial", "EE6986854FD72803",
                             "--partial-bytes", "3", "--count", "9",
                             "--json", "out.json"])
        argv = b.child_argv(args, script="s.py", python="py")
        self.assertEqual(argv[:3], ["py", "s.py", "--child"])
        self.assertIn("--serial", argv)
        self.assertEqual(argv[argv.index("--serial") + 1], "EE6986854FD72803")
        self.assertEqual(argv[argv.index("--partial-bytes") + 1], "3")
        self.assertNotIn("--port", argv)
        # The child writes no report and runs no loop.
        self.assertNotIn("--json", argv)
        self.assertNotIn("--count", argv)
        # And it parses back as a child for the same frame.
        child = b.parse_args(argv[2:])
        self.assertTrue(child.child)
        self.assertEqual(child.partial_bytes, 3)

    def test_child_on_a_uart_carries_the_baud(self):
        args = b.parse_args(["--port", "COM8", "--baud", "57600"])
        argv = b.child_argv(args, script="s.py", python="py")
        self.assertEqual(argv[argv.index("--port") + 1], "COM8")
        self.assertEqual(argv[argv.index("--baud") + 1], "57600")
        self.assertNotIn("--serial", argv)


class Delays(unittest.TestCase):
    def test_in_range_and_not_fixed(self):
        rng = random.Random(1)
        delays = [b.pick_delay(rng, 0.05, 0.75) for _ in range(200)]
        self.assertTrue(all(0.05 <= d <= 0.75 for d in delays))
        # A fixed delay phase-locks to one search phase; these must spread.
        self.assertGreater(len(set(delays)), 150)

    def test_seed_repeats(self):
        first = [b.pick_delay(random.Random(7), 0.0, 1.0) for _ in range(3)]
        again = [b.pick_delay(random.Random(7), 0.0, 1.0) for _ in range(3)]
        self.assertEqual(first, again)


def row(i, child_rc=b.CHILD_WROTE, rtt=5.0):
    return {"i": i, "delay_s": 0.1, "child_rc": child_rc,
            "ok": child_rc == b.CHILD_WROTE and rtt is not None,
            "round_trip_ms": rtt if child_rc == b.CHILD_WROTE else None}


class Summary(unittest.TestCase):
    def test_all_answered_passes(self):
        s = b.summarise([row(0, rtt=3.0), row(1, rtt=7.0)])
        self.assertEqual((s["answered"], s["valid"], s["failures"]), (2, 2, 0))
        self.assertEqual(s["round_trip_ms"]["min"], 3.0)
        self.assertEqual(s["round_trip_ms"]["max"], 7.0)
        self.assertTrue(b.passed(s))

    def test_a_missed_reply_fails(self):
        s = b.summarise([row(0), row(1, rtt=None), row(2)])
        self.assertEqual((s["answered"], s["valid"]), (2, 3))
        self.assertEqual(s["failed_iterations"], [1])
        self.assertAlmostEqual(s["failure_rate_pct"], 100.0 / 3)
        self.assertFalse(b.passed(s))

    def test_a_child_that_never_wrote_is_not_a_sample_but_fails_the_run(self):
        # The dongle was never put mid-frame, so the iteration says nothing
        # about the issue - it is excluded from N/M, and reported.
        s = b.summarise([row(0), row(1, child_rc=1), row(2, child_rc=None)])
        self.assertEqual((s["answered"], s["valid"]), (1, 1))
        self.assertEqual(s["child_failed_iterations"], [1, 2])
        self.assertEqual(s["failures"], 0)
        self.assertFalse(b.passed(s))

    def test_an_empty_run_does_not_pass(self):
        s = b.summarise([])
        self.assertIsNone(s["failure_rate_pct"])
        self.assertEqual(s["round_trip_ms"], {})
        self.assertFalse(b.passed(s))


if __name__ == "__main__":
    unittest.main()
