#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Tests for tools/ant_features.py's capabilities gate. No hardware.

Until 2026-11-07 the tool required a 9-byte capabilities reply and stopped at
step 1 on anything shorter. A Garmin ANT USB-m answers with the 7 standard
bytes, so the tool could not produce a reference run on the one stick it most
needs to be compared against - although the only byte it ever reads is [6].
"""

from __future__ import annotations

import sys
import unittest

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

try:
    import ant_features
except SystemExit:  # pragma: no cover - pyusb missing
    # ant_features imports ant_probe, which exits rather than raises when
    # pyusb is absent; that would otherwise take the whole test run down.
    ant_features = None

# archive/benchmarks/2026-11-07-garmin-compare/identity_features_Garmin.log
GARMIN_REPLY = bytes.fromhex("080800ba3600df")
# ...and identity_features_Dongle.log, the RadiANT nRF52840 Dongle.
DONGLE_REPLY = bytes.fromhex("200300b22600d50001")


@unittest.skipIf(ant_features is None, "pyusb is not installed")
class CapabilitiesReply(unittest.TestCase):

    def test_the_garmins_seven_byte_reply_is_usable(self):
        self.assertTrue(ant_features.capabilities_usable(GARMIN_REPLY))
        # Advanced options 3, the one byte the tool goes on to read.
        self.assertEqual(GARMIN_REPLY[6], 0xDF)

    def test_the_dongles_nine_byte_reply_is_usable(self):
        self.assertTrue(ant_features.capabilities_usable(DONGLE_REPLY))

    def test_a_reply_short_of_advanced_options_3_is_rejected(self):
        self.assertFalse(ant_features.capabilities_usable(GARMIN_REPLY[:6]))

    def test_a_refusal_code_or_no_reply_is_rejected(self):
        # request() returns the response code on refusal and None on silence.
        self.assertFalse(ant_features.capabilities_usable(0x28))
        self.assertFalse(ant_features.capabilities_usable(None))

    def test_the_minimum_is_the_standard_length(self):
        self.assertEqual(ant_features.MIN_CAPABILITIES_LEN, 7)


if __name__ == "__main__":
    unittest.main()
