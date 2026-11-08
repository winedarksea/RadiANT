#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""The tools added for TODO.md items 2 and 3 (2026-11-07 Garmin comparison):
a lost STARTUP after SYSTEM_RESET, and a receiver's port dying mid-window.

Each test pins one way a diagnosis used to come out wrong or not at all:
wall_s stamped from the slowest child, a misspelt rig key dropped in silence,
an unsolicited STARTUP read as nothing, a dead port leaving no JSON.
"""

from __future__ import annotations

import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_compare as ac  # noqa: E402
import ant_multi_verify as amv  # noqa: E402
import ant_reset_loop as arl  # noqa: E402
import console_capture as cc  # noqa: E402
from ant_wire import (  # noqa: E402
    MESG_BROADCAST_DATA_ID,
    MESG_STARTUP_MESG_ID,
    STARTUP_WATCH_DOG_RESET,
)

try:
    import ant_verify
except SystemExit:  # pragma: no cover - pyusb missing
    ant_verify = None


class FakeProc:
    """A Popen whose poll() turns non-None `after_s` after construction."""

    def __init__(self, after_s: float, rc: int = 0):
        self._due = time.monotonic() + after_s
        self._rc = rc

    def poll(self):
        return self._rc if time.monotonic() >= self._due else None


class TestPerChildWallClock(unittest.TestCase):
    def test_a_fast_child_listed_after_a_slow_one_keeps_its_own_finish(self):
        # The 2026-11-07 shape: the slow receiver first, the one whose port
        # died early second. Waiting in order stamped both with the slow one.
        t0 = time.monotonic()
        slow, fast = FakeProc(0.6), FakeProc(0.1, rc=1)
        procs = [(None, slow), (None, fast)]
        done = amv.wait_each(procs, poll_s=0.02)
        self.assertEqual(done[id(fast)][0], 1)
        fast_wall = done[id(fast)][1] - t0
        slow_wall = done[id(slow)][1] - t0
        self.assertLess(fast_wall, 0.35)
        self.assertGreaterEqual(slow_wall, 0.6)

    def test_the_short_child_then_trips_the_dropped_port_check(self):
        problems = ac.window_sanity({"identities": {"0": {"device_number": 7}}},
                                    [7], 300.0, 245.0)
        self.assertTrue(any("dropped port" in p for p in problems))


def rig_with(**device_extra) -> dict:
    raw = {
        "meta": {"name": "t"},
        "device": [
            {"name": "Ref", "address": "usb:006", "stack": "reference"},
            {"name": "L15", "address": "port:COM4", "part": "nrf54l15"},
            {"name": "TX", "address": "port:COM5", "part": "nrf5340net",
             "transmit": True, "device_number_base": 53000},
        ],
        "round": [{"name": "A", "transmitters": ["TX"], "phases": ["multi"]}],
    }
    raw["device"][1].update(device_extra)
    return raw


class TestRigLogPort(unittest.TestCase):
    def test_log_port_is_parsed(self):
        rig = ac.parse_rig(rig_with(log_port="COM3"))
        self.assertEqual(rig.devices["L15"].log_port, "COM3")
        self.assertIsNone(rig.devices["Ref"].log_port)
        self.assertEqual(rig.warnings, [])

    def test_an_unknown_key_is_warned_about_not_dropped_silently(self):
        rig = ac.parse_rig(rig_with(log_prot="COM3"))
        self.assertIsNone(rig.devices["L15"].log_port)
        self.assertEqual(len(rig.warnings), 1)
        self.assertIn("log_prot", rig.warnings[0])

    def test_a_log_port_that_is_also_an_ant_port_is_refused(self):
        with self.assertRaises(ac.RigError):
            ac.parse_rig(rig_with(log_port="com5"))

    def test_the_shipped_rig_has_no_unknown_keys(self):
        self.assertEqual(ac.load_rig(Path(ac.__file__).parent / "compare_rigs"
                                     / "2026-11-07-fixed.toml").warnings, [])


class TestWindowSanityReadsTheNewFields(unittest.TestCase):
    def test_a_reboot_and_a_port_death_are_both_named(self):
        data = {"identities": {"0": {"device_number": 7}},
                "receiver_reboots": [{"t_s": 244.9, "reason": 2,
                                      "reason_name": "STARTUP_WATCH_DOG_RESET"}],
                "port_death": {"t_s": 245.1, "error": "SerialException: x"}}
        problems = ac.window_sanity(data, [7], 300.0, 300.0)
        self.assertTrue(any("rebooted" in p and "WATCH_DOG" in p
                            for p in problems))
        self.assertTrue(any("ANT port died at 245" in p for p in problems))


class FakeReader:
    def __init__(self, script):
        self._script = list(script)

    def next_frame(self, deadline):
        if self._script:
            item = self._script.pop(0)
            if isinstance(item, BaseException):
                raise item
            return item
        return None


@unittest.skipIf(ant_verify is None, "pyusb not installed")
class TestListenFlagsReboots(unittest.TestCase):
    def test_an_unsolicited_startup_is_recorded_as_a_receiver_reboot(self):
        reader = FakeReader([
            (MESG_BROADCAST_DATA_ID, bytes(9)),
            (MESG_STARTUP_MESG_ID, bytes([STARTUP_WATCH_DOG_RESET])),
        ])
        out = ant_verify.listen(None, reader, {}, 0.2, False, [])
        self.assertEqual(len(out["receiver_reboots"]), 1)
        reboot = out["receiver_reboots"][0]
        self.assertEqual(reboot["reason"], STARTUP_WATCH_DOG_RESET)
        self.assertEqual(reboot["reason_name"], "STARTUP_WATCH_DOG_RESET")
        self.assertNotIn("port_death", out)
        faults = ant_verify.window_faults(out)
        self.assertEqual(len(faults), 1)
        self.assertIn("rebooted", faults[0])

    def test_a_dying_port_returns_partial_results_with_the_time_of_death(self):
        # pyserial's SerialException is an OSError; this is its Windows text.
        reader = FakeReader([
            (MESG_BROADCAST_DATA_ID, bytes(9)),
            OSError("ReadFile failed (PermissionError(13, 'A device attached "
                    "to the system is not functioning.', None, 31))"),
        ])
        start = time.monotonic()
        out = ant_verify.listen(None, reader, {}, 30.0, False, [])
        self.assertLess(time.monotonic() - start, 5.0,
                        "a dead port must end the window, not spin to 30 s")
        self.assertIn("PermissionError", out["port_death"]["error"])
        self.assertIn("port died", ant_verify.window_faults(out)[0])

    def test_a_clean_window_has_no_faults(self):
        out = ant_verify.listen(None, FakeReader([]), {}, 0.05, False, [])
        self.assertEqual(out["receiver_reboots"], [])
        self.assertEqual(ant_verify.window_faults(out), [])


class FakeSerial:
    def __init__(self, chunks, die_with=None):
        self._chunks = list(chunks)
        self._die = die_with
        self.dtr = self.rts = False
        self.closed = False

    def read(self, n):
        if self._chunks:
            return self._chunks.pop(0)
        if self._die is not None:
            raise self._die
        time.sleep(0.01)
        return b""

    def close(self):
        self.closed = True


class TestConsoleCapture(unittest.TestCase):
    def run_capture(self, fake):
        tick = iter(range(1_700_000_000, 1_700_001_000))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "c.log"
            cap = cc.ConsoleCapture("COM3", path, opener=lambda p, b: fake,
                                    clock=lambda: float(next(tick)))
            cap.run()
            return cap, path.read_text(encoding="utf-8")

    def test_lines_are_stamped_split_across_reads_and_dtr_is_asserted(self):
        fake = FakeSerial([b"boot\r\nhalf", b" line\n", b"tail"],
                          die_with=OSError("gone"))
        cap, text = self.run_capture(fake)
        self.assertTrue(fake.dtr and fake.rts)
        lines = text.splitlines()
        self.assertTrue(all(line.startswith("[20") for line in lines))
        self.assertTrue(any(line.endswith("] boot") for line in lines))
        self.assertTrue(any(line.endswith("] half line") for line in lines))
        self.assertTrue(any("tail  [partial line]" in line for line in lines))
        self.assertEqual(cap.lines, 3)

    def test_the_moment_the_port_dies_is_recorded(self):
        cap, text = self.run_capture(
            FakeSerial([b"x\n"], die_with=OSError("PermissionError 13")))
        summary = cap.summary()
        self.assertIsNotNone(summary["died_at_unix"])
        self.assertGreater(summary["died_after_s"], 0)
        self.assertIn("PermissionError 13", summary["error"])
        self.assertIn("PORT DIED", text)

    def test_an_open_failure_is_reported_not_raised(self):
        def refuse(port, baud):
            raise OSError("busy")
        with tempfile.TemporaryDirectory() as tmp:
            cap = cc.ConsoleCapture("COM3", Path(tmp) / "c.log", opener=refuse)
            cap.run()
        self.assertFalse(cap.opened)
        self.assertIn("open failed", cap.error)


class CountingPort:
    """pyserial-like: every timeout assignment would be a SetCommState."""

    def __init__(self):
        self._timeout = None
        self.sets = 0
        self.in_waiting = 0

    @property
    def timeout(self):
        return self._timeout

    @timeout.setter
    def timeout(self, value):
        self._timeout = value
        self.sets += 1

    def read(self, n):
        return b"\xa4"


class TestSerialDeviceDoesNotReconfigureEveryRead(unittest.TestCase):
    def test_repeated_reads_at_one_timeout_configure_the_port_once(self):
        try:
            from ant_probe import SerialDevice
        except SystemExit:  # pragma: no cover - pyusb missing
            self.skipTest("pyusb not installed")
        dev = SerialDevice.__new__(SerialDevice)
        dev._port = CountingPort()
        for _ in range(500):
            dev.read(0x81, 64, timeout=1000)
        self.assertEqual(dev._port.sets, 1)
        dev.read(0x81, 64, timeout=50)
        self.assertEqual(dev._port.sets, 2, "a real change still applies")


class TestResetLoop(unittest.TestCase):
    def test_startup_latency_and_the_frames_that_came_first(self):
        reader = FakeReader([(MESG_BROADCAST_DATA_ID, bytes(9)),
                             (MESG_STARTUP_MESG_ID, bytes([0x20]))])
        times = iter([10.0, 10.0512])
        row = arl.wait_startup(reader, 10.0, 3.0, clock=lambda: next(times))
        self.assertTrue(row["ok"])
        self.assertAlmostEqual(row["latency_ms"], 51.2, places=3)
        self.assertEqual(row["reason"], "STARTUP_COMMAND_RESET")
        self.assertEqual(row["frames_before"], ["0x4E"])

    def test_no_startup_is_a_failure_not_a_hang(self):
        row = arl.wait_startup(FakeReader([]), time.perf_counter(), 0.05)
        self.assertFalse(row["ok"])
        self.assertIsNone(row["latency_ms"])

    def test_summary_counts_failures_with_a_wilson_bound(self):
        rows = [{"i": i, "ok": True, "latency_ms": 50.0 + i % 3}
                for i in range(999)]
        rows.append({"i": 999, "ok": False, "latency_ms": None})
        s = arl.summarise(rows, 50)
        self.assertEqual(s["failures"], 1)
        self.assertEqual(s["failed_iterations"], [999])
        lo, hi = s["failure_ci95_pct"]
        self.assertLess(lo, 0.1)
        self.assertGreater(hi, 0.1)
        self.assertEqual(s["latency_ms"]["min"], 50.0)
        self.assertEqual(s["latency_ms"]["max"], 52.0)
        self.assertEqual(s["within_one_read_timeout"], 999)

    def test_zero_in_a_thousand_bounds_the_rate_below_half_a_percent(self):
        s = arl.summarise([{"i": i, "ok": True, "latency_ms": 50.0}
                           for i in range(1000)], 1000)
        self.assertEqual(s["failures"], 0)
        self.assertLess(s["failure_ci95_pct"][1], 0.4)


if __name__ == "__main__":
    unittest.main()
