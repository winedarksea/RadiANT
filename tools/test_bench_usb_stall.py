#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Off-target tests for tools/bench_usb_stall.py's --suspend-cycles plumbing.

    python -m unittest discover -s tools -p "test_*.py"

The suspend test itself needs a Linux host and a board. What can go wrong
without one is the parsing: the argument, the kernel's device name, and the
sysfs power attributes, any of which would otherwise surface as a crash
halfway through a bench run with runtime PM left switched on.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import bench_usb_stall as b  # noqa: E402
except SystemExit:  # pyusb missing: the module exits rather than raising
    raise unittest.SkipTest("pyusb is not installed") from None


class SuspendCyclesArg(unittest.TestCase):
    def test_accepts_whole_numbers(self):
        self.assertEqual(b.suspend_cycles_arg("40"), 40)
        self.assertEqual(b.suspend_cycles_arg("1"), 1)

    def test_refuses_zero_and_negative(self):
        for text in ("0", "-3"):
            with self.assertRaises(argparse.ArgumentTypeError):
                b.suspend_cycles_arg(text)

    def test_refuses_non_integers(self):
        for text in ("", "4.5", "forty", "0x10"):
            with self.assertRaises(argparse.ArgumentTypeError):
                b.suspend_cycles_arg(text)


class SysfsName(unittest.TestCase):
    def test_root_port(self):
        self.assertEqual(b.sysfs_usb_name(5, (1,)), "5-1")

    def test_behind_hubs(self):
        self.assertEqual(b.sysfs_usb_name(1, (2, 3, 4)), "1-2.3.4")

    def test_no_port_path_is_refused(self):
        for ports in (None, ()):
            with self.assertRaises(ValueError):
                b.sysfs_usb_name(5, ports)


class PowerAttributes(unittest.TestCase):
    def test_runtime_status_strips_the_newline(self):
        self.assertEqual(b.parse_runtime_status("suspended\n"), "suspended")
        self.assertEqual(b.parse_runtime_status("active\n"), "active")

    def test_runtime_status_unknown_is_refused(self):
        for text in ("", "asleep\n", "Suspended\n"):
            with self.assertRaises(ValueError):
                b.parse_runtime_status(text)

    def test_power_control(self):
        self.assertEqual(b.parse_power_control("on\n"), "on")
        self.assertEqual(b.parse_power_control("auto\n"), "auto")
        with self.assertRaises(ValueError):
            b.parse_power_control("off\n")

    def test_integers(self):
        self.assertEqual(b.parse_sysfs_int("2000\n"), 2000)
        self.assertEqual(b.parse_sysfs_int("-1\n"), -1)
        with self.assertRaises(ValueError):
            b.parse_sysfs_int("\n")


class Supported(unittest.TestCase):
    def test_skips_off_linux(self):
        reason = b.suspend_supported("win32")
        self.assertIsNotNone(reason)
        self.assertIn("Linux", reason)
        self.assertIsNotNone(b.suspend_supported("darwin"))

    def test_linux_needs_the_sysfs_tree(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(b.suspend_supported("linux", root))
            self.assertIsNotNone(
                b.suspend_supported("linux", os.path.join(root, "absent")))


class CountResets(unittest.TestCase):
    LOG = (
        "[   1.0] usb 5-1: new full-speed USB device number 24 using xhci_hcd\n"
        "[ 200.1] usb 5-1: reset full-speed USB device number 24 using xhci_hcd\n"
        "[ 210.4] usb 5-1.2: reset full-speed USB device number 7 using xhci_hcd\n"
        "[ 220.9] usb 15-1: reset high-speed USB device number 3 using xhci_hcd\n"
        "[ 230.2] usb 5-1: reset full-speed USB device number 24 using xhci_hcd\n"
        "[ 240.0] usb 5-1: USB disconnect, device number 24\n"
    )

    def test_counts_only_this_device(self):
        self.assertEqual(b.count_resets(self.LOG, "5-1"), 2)
        self.assertEqual(b.count_resets(self.LOG, "5-1.2"), 1)
        self.assertEqual(b.count_resets(self.LOG, "15-1"), 1)

    def test_enumeration_and_disconnect_are_not_resets(self):
        self.assertEqual(b.count_resets(self.LOG, "1-1"), 0)
        self.assertEqual(b.count_resets("", "5-1"), 0)


class WaitRuntimeStatus(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "runtime_status")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, text):
        with open(self.path, "w", encoding="ascii") as handle:
            handle.write(text)

    def test_reached(self):
        self.write("suspended\n")
        self.assertEqual(b.wait_runtime_status(self.path, "suspended", 0.1),
                         (True, "suspended"))

    def test_times_out_with_the_last_state(self):
        self.write("active\n")
        self.assertEqual(b.wait_runtime_status(self.path, "suspended", 0.05),
                         (False, "active"))

    def test_garbage_raises_rather_than_waiting(self):
        self.write("nonsense\n")
        with self.assertRaises(ValueError):
            b.wait_runtime_status(self.path, "suspended", 0.05)


if __name__ == "__main__":
    unittest.main()
