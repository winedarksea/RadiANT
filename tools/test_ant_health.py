#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Decode tests for tools/ant_health.py's version-aware 0xF6 reader.

    python -m unittest discover -s tools -p "test_*.py"

Runs in the CI `host-tests` job alongside the other tools/test_*.py suites.

W1 bumped the 0xF6 structure from version 1 (24-byte body) to version 2
(30-byte body, adding host_desync/host_csum/rx_not_owner/tx_timeout). This
file's one job is making sure decode() still reads an old stick's shorter
reply correctly, as well as a current one's - a host in the field cannot be
made to reflash just because this tool grew new fields.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ant_health as h  # noqa: E402


def _body(version: int, length: int, **fields: int) -> bytes:
    """Build a synthetic 0xF6 body: `version` at [0], zero-filled to
    `length`, with named FIELDS entries patched in as little-endian."""
    out = bytearray(length)
    out[0] = version
    field_map = {name: (off, width) for name, off, width, _d, _mv in h.FIELDS}
    for name, value in fields.items():
        off, width = field_map[name]
        out[off:off + width] = int(value).to_bytes(width, "little")
    return bytes(out)


class DecodeV1Test(unittest.TestCase):
    """A 24-byte, version-1 body - what an older stick still sends."""

    def test_decodes_the_v1_fields(self):
        body = _body(1, h.PAYLOAD_LEN_V1, tx_drops=5, sched_denied=1234)
        got = h.decode(body)
        self.assertEqual(got["version"], 1)
        self.assertEqual(got["tx_drops"], 5)
        self.assertEqual(got["sched_denied"], 1234)

    def test_v1_body_carries_none_of_the_v2_fields(self):
        body = _body(1, h.PAYLOAD_LEN_V1)
        got = h.decode(body)
        for name in ("host_desync", "host_csum", "rx_not_owner",
                     "tx_timeout"):
            self.assertNotIn(
                name, got,
                "%s should be absent from a v1 body, not a fabricated 0"
                % name,
            )

    def test_shorter_than_v1_is_rejected(self):
        with self.assertRaises(ValueError):
            h.decode(_body(1, h.PAYLOAD_LEN_V1 - 1))


class DecodeV2Test(unittest.TestCase):
    """A 30-byte, version-2 body - what a build with W1 sends today."""

    def test_decodes_every_field_including_the_new_ones(self):
        body = _body(
            2, h.PAYLOAD_LEN,
            tx_drops=5,
            host_desync=7,
            host_csum=8,
            rx_not_owner=9,
            tx_timeout=10,
        )
        got = h.decode(body)
        self.assertEqual(got["version"], 2)
        self.assertEqual(got["tx_drops"], 5)
        self.assertEqual(got["host_desync"], 7)
        self.assertEqual(got["host_csum"], 8)
        self.assertEqual(got["rx_not_owner"], 9)
        self.assertEqual(got["tx_timeout"], 10)

    def test_saturated_flag(self):
        body = bytearray(_body(2, h.PAYLOAD_LEN))
        body[1] = 0x01
        got = h.decode(bytes(body))
        self.assertTrue(got["saturated"])

    def test_v2_body_from_a_future_version_still_decodes_known_fields(self):
        # A hypothetical version 3 with extra bytes this tool has never seen:
        # decode() must not choke on the extra length, and must still report
        # the fields it recognizes.
        body = _body(3, h.PAYLOAD_LEN + 4, tx_timeout=42)
        got = h.decode(body)
        self.assertEqual(got["version"], 3)
        self.assertEqual(got["tx_timeout"], 42)


if __name__ == "__main__":
    unittest.main()
