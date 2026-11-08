# SPDX-License-Identifier: Apache-2.0
"""ant_probe.py --reboot-to-bootloader: the exact request the firmware matches.

CONFIG_ANT_DONGLE_UF2_REBOOT_REQUEST answers only bmRequestType 0x40,
bRequest 0x57, wValue 0x5546, wIndex 0, wLength 0. A drift in any one field
here is a request the firmware STALLs, which looks exactly like an image built
without the option - so the bytes are pinned.
"""
from __future__ import annotations

import contextlib
import io
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import ant_probe
    import usb.core
except SystemExit:  # pyusb missing: ant_probe exits rather than raising
    ant_probe = None
except ImportError:
    ant_probe = None


class FakeDev:
    def __init__(self, fail: bool = False):
        self.calls = []
        self.fail = fail

    def ctrl_transfer(self, *args, **kwargs):
        self.calls.append((args, kwargs))
        if self.fail:
            raise usb.core.USBError("Pipe error")


@unittest.skipIf(ant_probe is None, "pyusb is not installed")
class RebootToBootloader(unittest.TestCase):
    def run_quietly(self, dev):
        with contextlib.redirect_stdout(io.StringIO()) as out, \
                unittest.mock.patch.object(ant_probe, "close_device"):
            rc = ant_probe.reboot_to_bootloader(dev)
        return rc, out.getvalue()

    def test_the_request_is_exactly_what_the_firmware_matches(self):
        dev = FakeDev()
        rc, _ = self.run_quietly(dev)
        self.assertEqual(rc, 0)
        (args, kwargs), = dev.calls
        self.assertEqual(args, (0x40, 0x57, 0x5546, 0, None))

    def test_a_stall_names_the_missing_build_option(self):
        rc, out = self.run_quietly(FakeDev(fail=True))
        self.assertEqual(rc, 1)
        self.assertIn("CONFIG_ANT_DONGLE_UF2_REBOOT_REQUEST", out)

    def test_the_uart_transport_is_refused_rather_than_attempted(self):
        dev = ant_probe.SerialDevice.__new__(ant_probe.SerialDevice)
        rc, _ = self.run_quietly(dev)
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
