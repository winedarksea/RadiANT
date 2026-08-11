#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/ant_identity.py and the page 81 privacy rule.

The interesting assertions here are the ones about what must NOT change:
Tier 0 provisioning is idempotent, and a re-roll moves the on-air number and
leaves the base number alone. Both are properties a plausible implementation
gets wrong in a way that looks fine until a household loses every pairing.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

try:
    from . import ant_identity as ai
    from . import ant_pages as ap
except ImportError:                       # run as a script or discovered flat
    import ant_identity as ai
    import ant_pages as ap


class TestRandomDeviceNumber(unittest.TestCase):

    def test_never_zero_and_never_out_of_range(self):
        # 0 is the wildcard on an ANT channel ID. A provisioning routine that
        # can emit it produces a node that matches every search and belongs to
        # nobody.
        for _ in range(2000):
            number = ai.random_device_number()
            self.assertGreaterEqual(number, 1)
            self.assertLessEqual(number, 0xFFFF)

    def test_is_not_obviously_constant(self):
        seen = {ai.random_device_number() for _ in range(200)}
        self.assertGreater(len(seen), 100)


class TestIdentity(unittest.TestCase):

    def test_new_sets_base_equal_to_the_on_air_number(self):
        identity = ai.Identity.new(tier=0)
        self.assertEqual(identity.device_number, identity.base_device_number)

    def test_reroll_moves_the_on_air_number_and_not_the_base(self):
        # The base number is bound into the key derivation, so re-rolling it
        # would silently invalidate every derived key and turn a Tier 1
        # re-roll into a rekey nobody asked for.
        identity = ai.Identity.new(tier=1)
        base = identity.base_device_number
        moved = False
        for _ in range(20):
            before = identity.device_number
            identity.reroll()
            moved = moved or identity.device_number != before
            self.assertEqual(identity.base_device_number, base)
        self.assertTrue(moved)

    def test_on_boot_only_rerolls_at_tier_2(self):
        for tier in (0, 1):
            identity = ai.Identity.new(tier=tier)
            before = identity.device_number
            identity.on_boot()
            self.assertEqual(identity.device_number, before,
                             "tier %d must not move at power-up; a receiver "
                             "that loses its sensor every session is exactly "
                             "the experience the tiering exists to avoid"
                             % tier)

        identity = ai.Identity.new(tier=2)
        moved = False
        for _ in range(20):
            before = identity.device_number
            identity.on_boot()
            moved = moved or identity.device_number != before
        self.assertTrue(moved)

    def test_zero_is_refused(self):
        with self.assertRaises(ValueError):
            ai.Identity(0, 1, 0, "")
        with self.assertRaises(ValueError):
            ai.Identity(1, 0, 0, "")

    def test_unknown_tier_is_refused(self):
        with self.assertRaises(ValueError):
            ai.Identity(1, 1, 3, "")

    def test_round_trips_through_json(self):
        identity = ai.Identity.new(tier=2, privacy_pages=False)
        back = ai.Identity.from_dict(json.loads(json.dumps(identity.to_dict())))
        self.assertEqual(back.to_dict(), identity.to_dict())

    def test_an_old_record_without_a_base_number_still_loads(self):
        back = ai.Identity.from_dict({"device_number": 1234})
        self.assertEqual(back.base_device_number, 1234)
        self.assertEqual(back.tier, 0)


class TestProvisioning(unittest.TestCase):

    def test_provision_is_idempotent(self):
        # Tier 0 is "random at FIRST provisioning, stable thereafter". A
        # provisioning step that rolls a new number every run is Tier 2 wearing
        # Tier 0's name, and the difference is whether every receiver in the
        # house keeps working.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "identity.json"
            first = ai.provision(path)
            for _ in range(5):
                again = ai.provision(path)
                self.assertEqual(again.device_number, first.device_number)

    def test_load_of_a_missing_file_is_none_rather_than_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(ai.load(Path(tmp) / "nothing.json"))

    def test_cli_provision_show_reroll(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "identity.json"
            self.assertEqual(ai.main(["--file", str(path), "provision"]), 0)
            before = ai.load(path)

            self.assertEqual(ai.main(["--file", str(path), "show"]), 0)
            self.assertEqual(ai.load(path).device_number, before.device_number)

            self.assertEqual(ai.main(["--file", str(path), "reroll"]), 0)
            after = ai.load(path)
            self.assertEqual(after.base_device_number,
                             before.base_device_number)

    def test_cli_show_without_a_record_fails_rather_than_inventing_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "identity.json"
            self.assertEqual(ai.main(["--file", str(path), "show"]), 1)
            self.assertEqual(ai.main(["--file", str(path), "reroll"]), 1)


class TestPage81PrivacyRule(unittest.TestCase):
    """docs/radiant-security.md 5.4 - the cheapest privacy rule in the design."""

    def test_none_encodes_the_not_supplied_sentinel(self):
        raw = ap.encode_common_81(sw_revision_main=3, serial_number=None)
        self.assertEqual(raw[4:8], b"\xff\xff\xff\xff")
        self.assertEqual(raw[4:8],
                         ai.PRIVACY_SERIAL_NOT_SUPPLIED.to_bytes(4, "little"))

    def test_the_decoder_maps_the_sentinel_back_to_none(self):
        # The bug D11 exposed: the encoder documented this sentinel from the
        # day it was written and the decoder returned 4294967295 for it, so a
        # tool asking "which sensor is this" got an answer that looked like a
        # serial number and was the same for every privacy-preserving node.
        raw = ap.encode_common_81(sw_revision_main=3, serial_number=None)
        self.assertIsNone(ap.decode_common_81(raw)["serial_number"])

    def test_a_real_serial_still_round_trips(self):
        raw = ap.encode_common_81(sw_revision_main=3, serial_number=0x12345678)
        self.assertEqual(ap.decode_common_81(raw)["serial_number"], 0x12345678)

    def test_a_serial_defeats_a_reroll(self):
        # Stated as a test because it is the whole argument for the rule: two
        # sessions with different device numbers and the same serial are two
        # sessions of the same node, and no amount of device-number rotation
        # changes that.
        identity = ai.Identity.new(tier=2)
        first = ap.encode_common_81(1, 0xDEADBEEF)
        identity.on_boot()
        second = ap.encode_common_81(1, 0xDEADBEEF)
        self.assertEqual(first, second)

        private = ap.encode_common_81(1, None)
        self.assertNotEqual(private, first)


if __name__ == "__main__":
    unittest.main()
