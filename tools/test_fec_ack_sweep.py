# SPDX-License-Identifier: Apache-2.0
"""fec_ack_sweep.py's join and classifier, on synthetic records.

The hardware half needs two boards; the half that decides what a failure
MEANS does not, and it is the half whose mistake would silently move a
dongle fault onto the trainer or the reverse.
"""

import unittest

import fec_ack_sweep as s


def rec(seq, tag, act="ack", tx=1, cause="none", off=0):
    raw = tag
    payload = bytes([0x33, 0xFF, 0xFF, 0xFF, 0xFF, raw & 0xFF, raw >> 8, 0xFF])
    return s.SimRecord(seq, seq * 1000000, payload, act, cause, off, tx)


DUMP = """\
uart:~$ ackfault dump
AF-BEGIN n=3 lost=0 acks_sent=1 too_early=1
[00:00:03.123,000] <inf> trainer: something unrelated
AF seq=0 t=1000 p=33ffffffff304dff act=ack cause=none off=0 tx=1
AF seq=1 t=2000 p=33ffffffff314dff act=mute cause=mute off=0 tx=0
AF seq=2 t=3000 p=33ffffffff324dff act=ack cause=none off=-500 tx=0
AF-END
uart:~$ """


class Tags(unittest.TestCase):
    def test_tags_are_unique_over_the_span_and_stay_within_20_percent(self):
        tags = [s.tag_grade_raw(i) for i in range(s.TAG_SPAN)]
        self.assertEqual(len(set(tags)), s.TAG_SPAN)
        self.assertGreaterEqual(min(tags), s.GRADE_ZERO_RAW - 2000)
        self.assertLessEqual(max(tags), s.GRADE_ZERO_RAW + 2000)

    def test_payload_round_trips_its_tag(self):
        for i in (0, 1, 1999, 2000, 3999, 4000):
            self.assertEqual(s.tag_of_payload(s.page51_tagged(i)),
                             s.tag_grade_raw(i))

    def test_a_non_page_51_payload_has_no_tag(self):
        self.assertIsNone(s.tag_of_payload(bytes([0x30] + [0xFF] * 7)))
        self.assertIsNone(s.tag_of_payload(b"\x33\x00"))


class Dump(unittest.TestCase):
    def test_parses_through_echo_prompts_and_log_lines(self):
        d = s.parse_af_dump(DUMP)
        self.assertTrue(d.complete)
        self.assertEqual(d.n, 3)
        self.assertEqual(d.too_early, 1)
        self.assertEqual([r.action for r in d.records], ["ack", "mute", "ack"])
        self.assertEqual(d.records[2].offset_us, -500)
        self.assertEqual(s.tag_of_payload(d.records[0].payload), 0x4D30)

    def test_a_truncated_dump_says_so(self):
        d = s.parse_af_dump(DUMP.replace("AF-END", ""))
        self.assertFalse(d.complete)
        cut = "\n".join(l for l in DUMP.splitlines() if "seq=1" not in l)
        self.assertFalse(s.parse_af_dump(cut).complete,
                         "AF-END with a record missing is still incomplete")

    def test_records_before_the_begin_marker_are_ignored(self):
        stale = "AF seq=9 t=1 p=33ffffffff304dff act=ack cause=none off=0 tx=1\n"
        self.assertEqual(len(s.parse_af_dump(stale + DUMP).records), 3)


class Classify(unittest.TestCase):
    def test_the_table(self):
        c = s.classify
        self.assertEqual(c(s.COMPLETED, [rec(0, 1)]), s.OK)
        self.assertEqual(c(s.FAILED, [rec(0, 1)]), s.WINDOW_MISSED)
        self.assertEqual(c(s.FAILED, [rec(0, 1, tx=0)]), s.SIM_NO_TX)
        self.assertEqual(c(s.FAILED, [rec(0, 1, act="mute", tx=0)]), s.EXPECTED)
        self.assertEqual(c(s.FAILED, [rec(0, 1, act="deaf", tx=0)]), s.EXPECTED)
        self.assertEqual(c(s.FAILED, []), s.NOT_HEARD)

    def test_a_completion_without_an_ack_on_air_is_impossible(self):
        for r in ([rec(0, 1, act="mute", tx=0)], [rec(0, 1, act="deaf", tx=0)],
                  [rec(0, 1, tx=0)], []):
            self.assertEqual(s.classify(s.COMPLETED, r), s.IMPOSSIBLE, r)

    def test_originator_errors_are_their_own_verdict(self):
        self.assertEqual(s.classify("no terminal event", [rec(0, 1)]),
                         s.ORIGINATOR_ERROR)
        self.assertEqual(s.classify("REFUSED code 21", []), s.ORIGINATOR_ERROR)

    def test_the_best_of_duplicate_records_wins(self):
        dup = [rec(0, 1, act="mute", tx=0), rec(1, 1, tx=1)]
        self.assertEqual(s.classify(s.COMPLETED, dup), s.OK)
        self.assertEqual(s.classify(s.FAILED, dup), s.WINDOW_MISSED)


class Join(unittest.TestCase):
    def test_joins_by_tag_not_by_order(self):
        sent = [s.Sent(i, s.tag_grade_raw(i), o) for i, o in
                enumerate([s.COMPLETED, s.FAILED, s.FAILED, s.FAILED])]
        # The sim heard 2 before 0, never heard 3, and muted 1.
        dump = s.SimDump(records=[
            rec(0, s.tag_grade_raw(2), tx=1),
            rec(1, s.tag_grade_raw(0), tx=1),
            rec(2, s.tag_grade_raw(1), act="mute", tx=0, cause="mute"),
        ], n=3, complete=True)
        rows = s.join(sent, dump)
        self.assertEqual([r["verdict"] for r in rows],
                         [s.OK, s.EXPECTED, s.WINDOW_MISSED, s.NOT_HEARD])
        summary = s.summarize(rows)
        self.assertEqual(summary["n"], 4)
        self.assertEqual(summary[s.WINDOW_MISSED], 1)
        self.assertAlmostEqual(summary["window_miss_rate"], 0.5)

    def test_rate_is_none_with_nothing_acknowledged(self):
        rows = s.join([s.Sent(0, 1, s.FAILED)], s.SimDump())
        self.assertIsNone(s.summarize(rows)["window_miss_rate"])


class Page71(unittest.TestCase):
    def test_advance_sums_steps_and_survives_the_u8_wrap(self):
        self.assertEqual(s.page71_advance([]), 0)
        self.assertEqual(s.page71_advance([7]), 0)
        self.assertEqual(s.page71_advance([1, 2, 4]), 3)
        self.assertEqual(s.page71_advance([254, 255, 1]), 3)

    def test_watch_records_only_changes_of_page_71(self):
        seqs = []
        b71 = lambda seq: (0x4E, bytes([0, 0x47, 0x33, seq, 0, 0, 0, 0, 0]))
        for f in (b71(5), b71(5), (0x4E, bytes([0, 0x10] + [0] * 7)), b71(6),
                  None, (0x40, bytes([0, 0x47, 0, 9]))):
            s.watch(f, seqs)
        self.assertEqual(seqs, [5, 6])

    def test_deaf_is_not_applied(self):
        d = s.SimDump(records=[rec(0, 1), rec(1, 2, act="mute", tx=0),
                               rec(2, 3, act="deaf", tx=0)])
        self.assertEqual(s.applied_by_sim(d), 2)


class Sweep(unittest.TestCase):
    def test_parse_is_inclusive(self):
        self.assertEqual(s.parse_sweep("-400:600:50")[0], -400)
        self.assertEqual(s.parse_sweep("-400:600:50")[-1], 600)
        self.assertEqual(len(s.parse_sweep("-400:600:50")), 21)
        self.assertEqual(s.parse_sweep("0:0:10"), [0])

    def test_parse_refuses_nonsense(self):
        for bad in ("0:100", "100:0:10", "0:100:0", "a:b:c"):
            with self.assertRaises(ValueError):
                s.parse_sweep(bad)

    def test_curve_prints_one_line_per_point(self):
        rows = s.join([s.Sent(0, 1, s.FAILED)],
                      s.SimDump(records=[rec(0, 1)], n=1, complete=True))
        lines = s.curve_lines([{"offset_us": 300,
                                "summary": s.summarize(rows)}])
        self.assertEqual(len(lines), 2)
        self.assertIn("+300", lines[1])
        self.assertIn("100.0", lines[1])


if __name__ == "__main__":
    unittest.main()
