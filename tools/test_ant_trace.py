#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Tests for tools/ant_trace.py. No hardware, no device, standard library only.

The forward parser cannot be tested against a real `Device0.txt` because no
sample exists yet - that limitation is stated in `ant_trace`'s docstring and it
is not something a test can fix. What *can* be tested, and is, is everything
around it: that each dialect reads what it claims to read, that an unrecognised
line fails loudly and names itself rather than being skipped, that detection
refuses an ambiguous file, and that the reverse direction round-trips a
transcript through annotated text and back to the same bytes.
"""

from __future__ import annotations

import unittest

import ant_trace
import ant_wire as wire
from ant_trace import DONGLE_TO_HOST, HOST_TO_DONGLE, Record, TraceError

RESET = wire.frame(wire.MESG_SYSTEM_RESET_ID, b"\x00")
STARTUP = wire.frame(wire.MESG_STARTUP_MESG_ID,
                     bytes([wire.STARTUP_REASONS["STARTUP_COMMAND_RESET"]]))
REQUEST_CAPS = wire.frame(wire.MESG_REQUEST_ID,
                          bytes([0, wire.MESG_CAPABILITIES_ID]))


class FrameSplitting(unittest.TestCase):
    def test_one_frame(self):
        self.assertEqual(ant_trace.frames_from_bytes(RESET), [RESET])

    def test_several_frames_in_one_run(self):
        run = RESET + STARTUP + REQUEST_CAPS
        self.assertEqual(ant_trace.frames_from_bytes(run),
                         [RESET, STARTUP, REQUEST_CAPS])

    def test_bad_checksum_is_refused_loudly(self):
        broken = bytearray(RESET)
        broken[-1] ^= 0x01
        with self.assertRaises(TraceError) as caught:
            ant_trace.frames_from_bytes(bytes(broken))
        # The message has to say *what* is wrong, because the likeliest cause
        # is that this tool read the wrong columns, not that Garmin emitted a
        # bad frame.
        self.assertIn("checksum", str(caught.exception))

    def test_wrong_sync_names_the_bidirectional_variant(self):
        with self.assertRaises(TraceError) as caught:
            ant_trace.frames_from_bytes(bytes([wire.SYNC_RX]) + RESET[1:])
        self.assertIn("0xA5", str(caught.exception))

    def test_truncated_tail_is_refused(self):
        with self.assertRaises(TraceError):
            ant_trace.frames_from_bytes(RESET[:-1])

    def test_non_strict_stops_rather_than_raising(self):
        self.assertEqual(ant_trace.frames_from_bytes(RESET[:-1], strict=False),
                         [])


class AntserRoundTrip(unittest.TestCase):
    def records(self):
        return [
            Record(None, HOST_TO_DONGLE, RESET, "4a-system-reset/valid"),
            Record(None, DONGLE_TO_HOST, STARTUP, "4a-system-reset/valid"),
            Record(0.0142, HOST_TO_DONGLE, REQUEST_CAPS, "other"),
        ]

    def test_write_then_read_is_lossless(self):
        text = ant_trace.format_antser(self.records())
        back = ant_trace.read_antser(text)
        self.assertEqual([(r.seconds, r.direction, r.data, r.case)
                          for r in back],
                         [(r.seconds, r.direction, r.data, r.case)
                          for r in self.records()])

    def test_suppressed_timestamp_is_a_bare_dash(self):
        text = ant_trace.format_antser(self.records()[:1])
        self.assertIn(f"- {HOST_TO_DONGLE} {RESET.hex()}", text)

    def test_case_markers_are_emitted_once_per_run(self):
        text = ant_trace.format_antser(self.records())
        self.assertEqual(text.count("# case 4a-system-reset/valid"), 1)

    def test_formatting_is_deterministic(self):
        # The whole Tier 1 gate rests on this one property.
        self.assertEqual(ant_trace.format_antser(self.records()),
                         ant_trace.format_antser(self.records()))

    def test_annotated_text_reads_back_to_the_same_bytes(self):
        original = ant_trace.format_antser(self.records())
        annotated = ant_trace.annotate(self.records())
        self.assertNotEqual(annotated, original)   # it really is annotated
        round_tripped = ant_trace.format_antser(
            ant_trace.read_antser(annotated))
        self.assertEqual(round_tripped, original)

    def test_annotation_names_the_message(self):
        annotated = ant_trace.annotate(self.records())
        self.assertIn("MESG_STARTUP_MESG", annotated)
        self.assertIn("STARTUP_COMMAND_RESET", annotated)

    def test_a_malformed_line_names_the_line_number(self):
        text = (ant_trace.ANTSER_HEADER + "\n"
                + "- > " + RESET.hex() + "\n"
                + "this is not a record\n")
        with self.assertRaises(TraceError) as caught:
            ant_trace.read_antser(text, source="x.antser")
        self.assertIn("x.antser:3", str(caught.exception))

    def test_two_frames_on_one_line_is_refused(self):
        text = f"- > {(RESET + STARTUP).hex()}\n"
        with self.assertRaises(TraceError) as caught:
            ant_trace.read_antser(text)
        self.assertIn("one frame", str(caught.exception))

    def test_a_deliberately_broken_frame_is_still_recordable(self):
        # A bad-checksum conformance case has to survive a write/read cycle;
        # the transcript records what was sent, not only what was well formed.
        broken = bytearray(RESET)
        broken[-1] ^= 0x01
        text = ant_trace.format_antser(
            [Record(None, HOST_TO_DONGLE, bytes(broken), "bad")])
        back = ant_trace.read_antser(text)
        self.assertEqual(back[0].data, bytes(broken))
        self.assertIsNone(back[0].unframed())

    def test_direction_must_be_one_of_the_two_markers(self):
        with self.assertRaises(TraceError):
            Record(None, "?", RESET)


class Describe(unittest.TestCase):
    def test_response_event_decodes_the_code_by_name(self):
        frame = wire.frame(wire.MESG_RESPONSE_EVENT_ID,
                           bytes([0, wire.MESG_ASSIGN_CHANNEL_ID,
                                  wire.RESPONSE_CODES["INVALID_MESSAGE"]]))
        text = ant_trace.describe_frame(frame)
        self.assertIn("MESG_ASSIGN_CHANNEL", text)
        self.assertIn("INVALID_MESSAGE", text)

    def test_channel_event_is_not_read_as_a_reply(self):
        frame = wire.frame(wire.MESG_RESPONSE_EVENT_ID,
                           bytes([3, wire.MESG_EVENT_ID,
                                  wire.EVENT_CODES["EVENT_RX_FAIL"]]))
        text = ant_trace.describe_frame(frame)
        self.assertIn("channel 3 event EVENT_RX_FAIL", text)

    def test_capabilities_decodes_the_observed_reply(self):
        frame = wire.frame(wire.MESG_CAPABILITIES_ID,
                           wire.OBSERVED_CAPABILITIES)
        text = ant_trace.describe_frame(frame)
        self.assertIn("max_channels=8", text)
        self.assertIn("CAPABILITIES_ADVANCED_BURST_ENABLED", text)

    def test_a_non_frame_says_so_rather_than_guessing(self):
        self.assertIn("not a frame", ant_trace.describe_frame(b"\xa4\x01"))


class Dialects(unittest.TestCase):
    """Each candidate Device0.txt shape, and the loud failure when none fits."""

    def parse(self, lines, name):
        dialect = next(d for d in ant_trace.DIALECTS if d.name == name)
        return ant_trace.parse_lines(lines, dialect)

    def test_clock_and_bracketed_bytes(self):
        lines = [
            "10:32:05.100 Tx [A4][01][4A][00][EF]",
            "10:32:05.150 Rx [A4][01][6F][20][EA]",
        ]
        result = self.parse(lines, "clock-dir-bracketed")
        self.assertEqual([r.direction for r in result.records],
                         [HOST_TO_DONGLE, DONGLE_TO_HOST])
        self.assertEqual(result.records[0].data, RESET)
        self.assertEqual(result.records[0].seconds, 0.0)
        # Timestamps are relative to the first record, per the format contract.
        self.assertAlmostEqual(result.records[1].seconds, 0.05, places=6)

    def test_clock_and_spaced_bytes(self):
        result = self.parse(["10:32:05, Tx, A4 01 4A 00 EF"],
                            "clock-dir-spaced")
        self.assertEqual(result.records[0].data, RESET)

    def test_millisecond_counter(self):
        result = self.parse(["1200 Tx A4 01 4A 00 EF",
                             "1450 Rx A4 01 6F 20 EA"], "millis-dir-spaced")
        self.assertAlmostEqual(result.records[1].seconds, 0.25, places=6)

    def test_a_line_carrying_two_frames_becomes_two_records(self):
        run = " ".join(f"{b:02X}" for b in RESET + REQUEST_CAPS)
        result = self.parse([f"10:00:00.000 Tx {run}"], "clock-dir-spaced")
        self.assertEqual(len(result.records), 2)
        self.assertEqual(result.frames, 2)

    def test_an_unreadable_line_raises_and_names_it(self):
        with self.assertRaises(TraceError) as caught:
            ant_trace.parse_lines(["10:32:05.100 Tx [A4][01][4A][00][EF]",
                                   "Session opened, 2 devices"],
                                  next(d for d in ant_trace.DIALECTS
                                       if d.name == "clock-dir-bracketed"),
                                  source="Device0.txt")
        message = str(caught.exception)
        self.assertIn("Device0.txt:2", message)
        self.assertIn("Session opened", message)
        # And it must say what to do about it, because guessing is the failure
        # mode this whole design is arranged against.
        self.assertIn("--dump-unparsed", message)

    def test_dump_unparsed_collects_instead_of_stopping(self):
        result = ant_trace.parse_lines(
            ["10:32:05.100 Tx [A4][01][4A][00][EF]",
             "Session opened, 2 devices",
             "10:32:05.150 Rx [A4][01][6F][20][EA]"],
            next(d for d in ant_trace.DIALECTS
                 if d.name == "clock-dir-bracketed"),
            collect_unparsed=True)
        self.assertEqual(len(result.records), 2)
        self.assertEqual(result.unparsed, [(2, "Session opened, 2 devices")])

    def test_blank_lines_and_rules_are_not_records_and_not_errors(self):
        result = self.parse(["", "-------", "10:32:05.100 Tx [A4][01][4A][00][EF]"],
                            "clock-dir-bracketed")
        self.assertEqual(len(result.records), 1)

    def test_detection_picks_the_dialect_that_reads_the_file(self):
        lines = ["10:32:05.100 Tx [A4][01][4A][00][EF]"] * 4
        self.assertEqual(ant_trace.detect_dialect(lines).name,
                         "clock-dir-bracketed")

    def test_detection_recognises_our_own_format(self):
        lines = [f"- > {RESET.hex()}", f"- < {STARTUP.hex()}"]
        self.assertEqual(ant_trace.detect_dialect(lines).name, "antser")

    def test_detection_refuses_a_file_it_does_not_understand(self):
        with self.assertRaises(TraceError) as caught:
            ant_trace.detect_dialect(["ANT log v3", "started", "closed"])
        message = str(caught.exception)
        self.assertIn("no dialect reads this file", message)
        # It must name what it tried, or the next person has to read the source
        # to find out which guesses were already made.
        self.assertIn("clock-dir-bracketed", message)

    def test_detection_refuses_a_mostly_unreadable_file(self):
        lines = ["10:32:05.100 Tx [A4][01][4A][00][EF]"] + ["noise"] * 9
        with self.assertRaises(TraceError):
            ant_trace.detect_dialect(lines)

    def test_a_line_whose_hex_is_not_a_frame_names_the_line(self):
        with self.assertRaises(TraceError) as caught:
            ant_trace.parse_lines(["10:32:05.100 Tx [01][02][03][04]"],
                                  next(d for d in ant_trace.DIALECTS
                                       if d.name == "clock-dir-bracketed"),
                                  source="Device0.txt")
        self.assertIn("Device0.txt:1", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
