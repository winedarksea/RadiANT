#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Decode tests for tools/ant_health.py's 0xF6 reader.

    python -m unittest discover -s tools -p "test_*.py"

Runs in the CI `host-tests` job alongside the other tools/test_*.py suites.

0xF6 follows the append-only rule: fields are only ever added at the end and an
offset is never reused, so decode() refuses a body shorter than the current
layout and reads every field it knows from a longer body or a higher version.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import ant_health as h  # noqa: E402
except SystemExit:  # pyusb missing: ant_probe exits rather than raising
    # Same reason as test_ant_features.py; otherwise this one module takes
    # the whole discover run down with an ERROR instead of a skip.
    raise unittest.SkipTest("pyusb is not installed") from None


def _body(version: int, length: int, **fields: int) -> bytes:
    """Build a synthetic 0xF6 body: `version` at [0], zero-filled to
    `length`, with named FIELDS entries patched in as little-endian."""
    out = bytearray(length)
    out[0] = version
    field_map = {name: (off, width) for name, off, width, _d in h.FIELDS}
    for name, value in fields.items():
        off, width = field_map[name]
        out[off:off + width] = int(value).to_bytes(width, "little")
    return bytes(out)


class DecodeTest(unittest.TestCase):

    def test_a_30_byte_v1_body_decodes_every_field(self):
        # A distinct value per field, so a wrong offset or width shows up as
        # the wrong number rather than as a coincidental match.
        values = {name: (i + 1) * 3 for i, (name, _o, width, _d)
                  in enumerate(h.FIELDS)}
        body = _body(1, h.PAYLOAD_LEN, **values)
        self.assertEqual(30, h.PAYLOAD_LEN)
        got = h.decode(body)
        self.assertEqual(got["version"], 1)
        self.assertFalse(got["saturated"])
        for name, value in values.items():
            self.assertEqual(got[name], value, name)

    def test_a_29_byte_body_is_refused(self):
        with self.assertRaises(ValueError):
            h.decode(_body(1, h.PAYLOAD_LEN - 1))

    def test_a_longer_higher_version_body_decodes_the_known_fields(self):
        # A later version with extra bytes this tool has never seen: decode()
        # must not choke on the extra length, must report the version it was
        # sent, and must still read the fields it recognises.
        body = _body(2, h.PAYLOAD_LEN + 4, tx_drops=5, tx_timeout=42)
        got = h.decode(body)
        self.assertEqual(got["version"], 2)
        self.assertEqual(got["tx_drops"], 5)
        self.assertEqual(got["tx_timeout"], 42)

    def test_saturated_flag(self):
        body = bytearray(_body(1, h.PAYLOAD_LEN))
        body[1] = 0x01
        got = h.decode(bytes(body))
        self.assertTrue(got["saturated"])


if __name__ == "__main__":
    unittest.main()
