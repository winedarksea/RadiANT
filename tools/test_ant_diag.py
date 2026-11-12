"""Tests for tools/ant_diag.py against the generated wire constants and the
C header's page table."""
from __future__ import annotations

import os
import re
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ant_diag as d  # noqa: E402
import ant_wire  # noqa: E402

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                      "radiant", "include", "radiant", "radiant_diag.h")


def header_text() -> str:
    with open(HEADER, encoding="utf-8") as fh:
        return fh.read()


def body(page: int, values: list[int], flags: int = 0, backend: int = 2) -> bytes:
    return bytes([page, 1, flags, backend]) + struct.pack("<8I", *values)


class Wire(unittest.TestCase):
    def test_message_id_is_the_generated_one(self):
        self.assertEqual(ant_wire.MESG_RADIANT_DIAG_ID, 0xF7)
        self.assertEqual(d.MESG_RADIANT_DIAG_ID, 0xF7)

    def test_pages_and_body_match_the_c_header(self):
        text = header_text()
        nums = re.findall(r"#define RADIANT_DIAG_PAGE_\w+\s+(\d+)u", text)
        count = re.search(r"#define RADIANT_DIAG_PAGES\s+(\d+)u", text)
        self.assertEqual(int(count.group(1)), len(d.PAGES))
        self.assertEqual(sorted(int(v) for v in nums), sorted(d.PAGES))
        self.assertIn("#define RADIANT_DIAG_FIELDS          8u", text)
        for _name, fields in d.PAGES.values():
            self.assertEqual(len(fields), 8)

    def test_backend_table_order_matches_the_header_enum(self):
        text = header_text()
        enum = re.findall(r"RADIANT_RADIO_DIAG_(\w+)(?: = 0)?,", text)
        enum = [e.lower() for e in enum if e != "COUNT"]
        ours = d.PAGES[3][1] + d.PAGES[4][1]
        self.assertEqual(len(enum), 16)
        # The tool's names are the enum's, minus the prefix.
        self.assertEqual(ours, enum)


class Decode(unittest.TestCase):
    def test_round_trip_and_not_measured(self):
        got = d.decode(body(4, [1, 2, 3, 4, 5, 6, 7, d.NOT_MEASURED], flags=1))
        self.assertEqual(got["name"], "backend_b")
        self.assertEqual(got["backend"], "cc26xx")
        self.assertTrue(got["unmeasured_flag"])
        self.assertEqual(got["fields"]["sw_addr_mismatch"], 1)
        self.assertIsNone(got["fields"]["min_lead_us"])

    def test_margin_is_signed(self):
        got = d.decode(body(2, [0, 0, 0, 0, 0, (-37) & 0xFFFFFFFF, 0, 0]))
        self.assertEqual(got["fields"]["tx_margin_min_us"], -37)

    def test_refuses_short_unknown_and_wrong_version(self):
        with self.assertRaises(ValueError):
            d.decode(body(0, [0] * 8)[:20])
        with self.assertRaises(ValueError):
            d.decode(body(9, [0] * 8))
        bad = bytearray(body(0, [0] * 8))
        bad[1] = 2
        with self.assertRaises(ValueError):
            d.decode(bytes(bad))


class Diff(unittest.TestCase):
    def test_counters_subtract_mod_2_32_and_levels_do_not(self):
        a = {"2": d.decode(body(2, [0xFFFFFFF0, 0, 10, 900, 0, 5, 0, 0]))}
        b = {"2": d.decode(body(2, [0x10, 1, 8, 1200, 0, 3, 0, 0]))}
        out = d.diff(a, b)["2"]["fields"]
        self.assertEqual(out["cont_submits"], 0x20)
        self.assertEqual(out["cont_late"], 1)
        self.assertEqual(out["turn_min_us"], 8)      # level: later value
        self.assertEqual(out["turn_max_us"], 1200)
        self.assertEqual(out["tx_margin_min_us"], 3)


if __name__ == "__main__":
    unittest.main()
