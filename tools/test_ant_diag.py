# SPDX-License-Identifier: Apache-2.0

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
        no_sample = re.search(r"#define RADIANT_DIAG_NO_SAMPLE\s+(0x[0-9A-Fa-f]+)u", text)
        self.assertEqual(int(no_sample.group(1), 16), d.NO_SAMPLE)
        for _name, fields in d.PAGES.values():
            self.assertEqual(len(fields), 8)

    def test_backend_table_order_matches_the_header_enum(self):
        text = header_text()
        enum = re.findall(r"RADIANT_RADIO_DIAG_(\w+)(?: = 0)?,", text)
        enum = [e.lower() for e in enum if e != "COUNT"]
        timing = d.PAGES[8][1]
        ours = (d.PAGES[3][1] + d.PAGES[4][1] + d.PAGES[7][1][:3]
                + timing[:4] + timing[7:] + d.PAGES[7][1][6:]
                + d.PAGES[9][1][5:] + d.PAGES[10][1][4:])
        self.assertEqual(len(enum), 33)
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
            d.decode(body(12, [0] * 8))
        bad = bytearray(body(0, [0] * 8))
        bad[1] = 2
        with self.assertRaises(ValueError):
            d.decode(bytes(bad))


class Timing(unittest.TestCase):
    def test_page_8_decodes_and_its_worst_cases_are_levels(self):
        got = d.decode(body(8, [3, 120, 18, d.NOT_MEASURED, 90, 5, 2, 1],
                            flags=1, backend=1))
        self.assertEqual(got["name"], "timing")
        self.assertIsNone(got["fields"]["rx_cb_max_us"])
        a = {"8": d.decode(body(8, [3, 120, 18, 200, 90, 5, 2, 1]))}
        b = {"8": d.decode(body(8, [4, 110, 18, 250, 95, 6, 7, 4]))}
        out = d.diff(a, b)["8"]["fields"]
        for level in ("drain_max_frames", "frame_max_us", "crc_max_us",
                      "rx_cb_max_us", "pass_max_us", "pass_steps_max"):
            self.assertEqual(out[level], b["8"]["fields"][level], level)
        self.assertEqual(out["pass_over_100us"], 5)   # counts subtract
        self.assertEqual(out["drain_over_250us"], 3)


class PassBreakdown(unittest.TestCase):
    def test_page_9_slack_is_signed_with_its_own_no_sample_sentinel(self):
        got = d.decode(body(9, [12, 30, 25, 8, d.NOT_MEASURED, 180, 37, 0],
                            flags=1, backend=1))
        self.assertEqual(got["name"], "pass")
        self.assertIsNone(got["fields"]["book_max_us"])   # not measured
        self.assertEqual(got["fields"]["arm_slack_min_us"], 37)
        fresh = d.decode(body(9, [0, 0, 0, 0, 0, 0, 0x7FFFFFFF, 0]))
        self.assertEqual(fresh["fields"]["arm_slack_min_us"], "no_sample")
        # 0x7FFFFFFF is a sentinel on the slack field only.
        other = d.decode(body(9, [0x7FFFFFFF, 0, 0, 0, 0, 0, 1, 0]))
        self.assertEqual(other["fields"]["sweep_max_us"], 0x7FFFFFFF)
        # Read as an i32, like page 2's margin.
        neg = d.decode(body(9, [0, 0, 0, 0, 0, 0, (-5) & 0xFFFFFFFF, 0]))
        self.assertEqual(neg["fields"]["arm_slack_min_us"], -5)

    def test_page_9_levels_report_the_later_value_and_etime_subtracts(self):
        a = {"9": d.decode(body(9, [12, 30, 25, 8, 4, 180, 0x7FFFFFFF, 2]))}
        b = {"9": d.decode(body(9, [11, 35, 20, 9, 4, 190, 21, 5]))}
        out = d.diff(a, b)["9"]["fields"]
        for level in ("sweep_max_us", "decide_max_us", "arm_call_max_us",
                      "done_cb_max_us", "book_max_us", "pass_to_arm_max_us",
                      "arm_slack_min_us"):
            self.assertEqual(out[level], b["9"]["fields"][level], level)
        self.assertEqual(out["early_etime"], 3)

    def test_page_10_counts_subtract_and_its_timings_do_not(self):
        a = {"10": d.decode(body(10, [5, 50, 1, 0, 0, 40, 9, 14]))}
        b = {"10": d.decode(body(10, [28, 90, 2, 1, 0, 35, 12, 13]))}
        got = d.diff(a, b)["10"]
        self.assertEqual(got["name"], "rx_integrity")
        out = got["fields"]
        self.assertEqual(out["crc_repaired"], 23)
        self.assertEqual(out["crc_unrepairable"], 40)
        self.assertEqual(out["crc_repair_refuted"], 1)
        self.assertEqual(out["crc_repair_implausible"], 1)
        self.assertEqual(out["late_isr_drops"], 0)
        for level in ("end_late_max_us", "t_sync_max_us", "match_max_us"):
            self.assertEqual(out[level], b["10"]["fields"][level], level)


class FakeDev:
    def __init__(self):
        self.writes = []

    def write(self, ep, data):
        self.writes.append(bytes(data))


class FakeReader:
    """Hands out scripted frames, one per next_frame() call. A None item, or
    running out, is a timeout: the request waiting on it gets no reply."""

    def __init__(self, script):
        self.script = list(script)

    def next_frame(self, deadline):
        return self.script.pop(0) if self.script else None


def page_reply(page):
    return (d.MESG_RADIANT_DIAG_ID, body(page, [page] * 8))


def refusal(page, code=0x33):
    return (ant_wire.MESG_RESPONSE_EVENT_ID,
            bytes([page, ant_wire.MESG_REQUEST_ID, code]))


class ReadPages(unittest.TestCase):
    """A refused page may be skipped; a lost reply may not."""

    def run_script(self, script, wanted):
        import io
        dev, err = FakeDev(), io.StringIO()
        pages = d.read_pages(dev, FakeReader(script), wanted, err=err)
        return pages, dev, err

    def test_older_build_refusing_page_8_is_skipped(self):
        script = [page_reply(p) for p in range(8)] + [refusal(8)]
        pages, dev, err = self.run_script(script, list(range(9)))
        self.assertEqual(sorted(pages, key=int), [str(p) for p in range(8)])
        self.assertIn("page 8: refused", err.getvalue())

    def test_refusal_of_the_first_page_fails(self):
        with self.assertRaises(RuntimeError):
            self.run_script([refusal(0)], [0, 1])

    def test_a_lost_reply_is_retried_not_skipped(self):
        script = [page_reply(0), None, page_reply(1), page_reply(2)]
        pages, dev, _ = self.run_script(script, [0, 1, 2])
        self.assertEqual(sorted(pages, key=int), ["0", "1", "2"])
        self.assertEqual(len(dev.writes), 4)   # page 1 asked twice

    def test_a_page_that_never_answers_fails_the_read(self):
        script = [page_reply(0), None, None]
        with self.assertRaises(RuntimeError):
            self.run_script(script, [0, 1])

    def test_a_reply_for_another_page_or_message_is_ignored(self):
        # A stale reply to page 0 and an unrelated response event arrive
        # before page 1's own reply.
        stale = page_reply(0)
        other = (ant_wire.MESG_RESPONSE_EVENT_ID, bytes([1, 0x42, 0x15]))
        got = d.request(FakeDev(), FakeReader([stale, other, page_reply(1)]), 1)
        self.assertEqual(got[0], 1)


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
