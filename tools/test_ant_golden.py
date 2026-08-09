#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Replay `.antser` transcripts and check them against the framing rules.

    python -m unittest discover -s tools -p "test_*.py"

Runs in the CI `host-tests` job: standard library only, no board, no driver,
no `pyusb`.

## What this does

An `.antser` file is a byte-level host<->dongle transcript, one framed message
per line, direction in a column - the format specified in
`archive/captures/serial/README.md`:

    # ant serial capture v1: <seconds|-> <dir> <framed message, hex>
    0.000000 > a4014a00ef
    0.014200 < a4016f20ea

For every **host to dongle** record, this re-derives the frame from
`tools/ant_wire.py` - `frame(msg_id, payload)` against the payload read out of
the record - and asserts the bytes match exactly. For every **dongle to host**
record it parses the frame and asserts the decode is self-consistent: the id
and payload `unframe()` returns are the bytes that are actually there, the id
is one the tables know, its declared length admits the payload it carries, its
declared direction admits the column it arrived in, and a response event names
either a real message id or the event marker.

That is worth doing only because the transcript is foreign. A vector obtained
by running `frame()` and freezing the result agrees with `frame()` by
construction; a transcript recorded by `ANT_DLL.dll`, or typed by hand from a
table, does not. This is the same argument that makes
`scripts/gen_ant_wire.py` generate constants instead of cross-checking them:
generation makes one number appear in three places, and only a foreign
transcript checks the *rules*.

## Two sources, and why one of them is skipped

`archive/captures/serial/` is where real captures go. **It is empty**, and it
will stay empty until somebody with a Windows host, the libusb-win32 driver,
`ANT_DLL.dll` and an application that drives a real session records one - see
that directory's README for the procedure and the shopping list. When there is
nothing there, `TestRealCaptures` skips, loudly, naming what is missing. It
does not pass.

`tools/vectors/` holds hand-assembled fixtures with their XOR worked out in
comments, and they are replayed on every run. They exist so the harness itself
is exercised from day one: a first real capture should add coverage, not be the
thing that discovers the harness was broken. A test that skips on every run and
a test that vacuously passes are the same test.

## The `malformed` case convention

A `# case` name containing the word `malformed` declares that the records under
it must be **rejected**. Without it a transcript cannot carry a deliberately
bad frame - which `tools/ant_conformance.py` needs to, since refusing garbage
correctly is part of what Tier 1 compares - without either failing this suite
or being waved through unchecked. The convention is introduced here because the
format had none; if `ant_conformance.py` settles on different wording, this
file is what changes.

## Deliberately not importing tools/ant_trace.py

`ant_trace.py` owns the `.antser` reader for the rest of the tools directory,
and this file reimplements a small strict parser instead. That is not
duplication for its own sake: a fixture read back by the same code that wrote
it proves nothing about the format, and this test's whole value is being an
implementation that the thing under test did not produce.
"""

from __future__ import annotations

import os
import re
import sys
import unittest
from collections import namedtuple

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import ant_wire as w  # noqa: E402

_REPO = os.path.dirname(_HERE)

VECTORS_DIR = os.path.join(_HERE, "vectors")
CAPTURES_DIR = os.path.join(_REPO, "archive", "captures", "serial")

HOST_TO_DONGLE = ">"
DONGLE_TO_HOST = "<"

HEADER_PREFIX = "# ant serial capture v1:"
CASE_MARKER = "# case "

# `<seconds|-> <dir> <hex>`, with an optional trailing comment so that the
# annotated form `ant_trace.py --to-text` writes still reads back.
_LINE = re.compile(r"^(?P<ts>-|\d+(?:\.\d+)?)\s+(?P<dir>[<>])\s+"
                   r"(?P<hex>[0-9a-fA-F]+)\s*(?:#.*)?$")

Record = namedtuple("Record", "lineno seconds direction data case")


class AntserError(Exception):
    """A line this parser refuses to interpret. Always names the line."""


def read_antser(text, source="<string>"):
    """Parse an `.antser` transcript strictly. No lenient mode.

    A fixture that half-parses is worse than one that does not parse at all:
    the half that was dropped is exactly the half nothing then checks.
    """
    records = []
    case = None
    seen_header = False

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            if line.startswith(HEADER_PREFIX):
                seen_header = True
            elif line.startswith(CASE_MARKER):
                case = line[len(CASE_MARKER):].strip()
            continue

        match = _LINE.match(line)
        if match is None:
            raise AntserError(
                f"{source}:{lineno}: not an .antser record: {raw!r}\n"
                f"  expected '<seconds|-> <dir> <hex>', e.g. "
                f"'- > a4014a00ef' or '0.014200 < a4016f20ea'")

        hex_text = match.group("hex")
        if len(hex_text) % 2:
            raise AntserError(f"{source}:{lineno}: odd number of hex digits "
                              f"in {hex_text!r}")
        seconds = None if match.group("ts") == "-" else float(
            match.group("ts"))
        records.append(Record(lineno, seconds, match.group("dir"),
                              bytes.fromhex(hex_text), case))

    if not seen_header:
        raise AntserError(
            f"{source}: no '{HEADER_PREFIX}' header line. A reader that does "
            f"not recognise the header should refuse rather than guess at the "
            f"columns.")
    return records


def read_antser_file(path):
    with open(path, "r", encoding="utf-8") as handle:
        return read_antser(handle.read(), source=os.path.basename(path))


def discover(directory):
    if not os.path.isdir(directory):
        return []
    return sorted(os.path.join(directory, name)
                  for name in os.listdir(directory)
                  if name.endswith(".antser"))


def _declared_range(declared):
    """(low, high) payload lengths a message id admits, or None for 'var'."""
    if declared == "var":
        return None
    text = str(declared)
    if ".." in text:
        low, high = text.split("..")
        return int(low), int(high)
    return int(text), int(text)


def _expects_rejection(record):
    return "malformed" in (record.case or "")


class ReplayMixin:
    """The checks. Shared by the fixture and real-capture test cases."""

    def replay_file(self, path):
        records = read_antser_file(path)
        name = os.path.basename(path)
        self.assertTrue(records, f"{name}: no records")

        for record in records:
            with self.subTest(file=name, line=record.lineno,
                              case=record.case):
                if _expects_rejection(record):
                    self.check_rejected(record, name)
                else:
                    self.check_frame(record, name)

        self.check_timestamps(records, name)
        self.check_request_replies(records, name)
        return records

    # -- per record ---------------------------------------------------------

    def check_rejected(self, record, name):
        where = f"{name}:{record.lineno}"
        self.assertIsNone(
            w.unframe(record.data),
            f"{where}: case {record.case!r} declares this frame malformed, "
            f"but unframe() accepted {record.data.hex()}")

    def check_frame(self, record, name):
        data = record.data
        where = f"{name}:{record.lineno} {data.hex()}"

        self.assertGreaterEqual(len(data), w.MSG_OVERHEAD,
                                f"{where}: shorter than MSG_OVERHEAD")
        self.assertEqual(data[0], w.SYNC_TX, f"{where}: not SYNC_TX")
        self.assertEqual(data[1] + w.MSG_OVERHEAD, len(data),
                         f"{where}: LEN says {data[1]} payload bytes, "
                         f"{len(data) - w.MSG_OVERHEAD} are present")
        self.assertLessEqual(len(data), w.MAX_FRAME_SIZE,
                             f"{where}: longer than MAX_FRAME_SIZE "
                             f"({w.MAX_FRAME_SIZE}); no receive buffer in the "
                             f"firmware can hold it")

        parsed = w.unframe(data)
        self.assertIsNotNone(
            parsed,
            f"{where}: rejected. If the checksum is out by exactly 0xA4 the "
            f"writer left the SYNC byte out of it "
            f"(computed 0x{w.checksum(data[:-1]):02X}, frame says "
            f"0x{data[-1]:02X}).")
        msg_id, payload = parsed

        # The decode must be the bytes that are there, not a reinterpretation.
        self.assertEqual(msg_id, data[2], f"{where}: id disagrees with byte 2")
        self.assertEqual(payload, data[3:-1],
                         f"{where}: payload disagrees with the frame body")
        self.assertEqual(len(payload), data[1],
                         f"{where}: payload length disagrees with LEN")

        # Re-derive the frame from ant_wire and require byte equality. This is
        # the assertion the whole file exists for; everything else is context.
        self.assertEqual(
            w.frame(msg_id, payload), data,
            f"{where}: ant_wire.frame() does not reproduce this record")

        self.check_message_id(record, msg_id, payload, where)

    def check_message_id(self, record, msg_id, payload, where):
        info = w.MESSAGES.get(msg_id) or w.RADIANT_MESSAGES.get(msg_id)
        self.assertIsNotNone(
            info,
            f"{where}: message id 0x{msg_id:02X} is in no table. If a real "
            f"capture contains it, that is information to absorb into "
            f"protocol/ant_wire.yaml, not to ignore.")

        self.assertNotEqual(
            info["direction"], "marker",
            f"{where}: 0x{msg_id:02X} ({info['name']}) is a marker inside "
            f"another message, never a frame of its own")

        allowed = {HOST_TO_DONGLE: ("h2d", "both"),
                   DONGLE_TO_HOST: ("d2h", "both")}[record.direction]
        self.assertIn(
            info["direction"], allowed,
            f"{where}: {info['name']} is {info['direction']}, but the record "
            f"is in the {record.direction!r} column")

        bounds = _declared_range(info["payload_len"])
        if bounds is not None:
            low, high = bounds
            self.assertTrue(
                low <= len(payload) <= high,
                f"{where}: {info['name']} declares a payload of "
                f"{info['payload_len']}, this carries {len(payload)}")

        if msg_id == w.MESG_RESPONSE_EVENT_ID:
            self.check_response_event(payload, where)

    def check_response_event(self, payload, where):
        """[channel, message id or MESG_EVENT_ID, code].

        The middle byte decides which number space byte 2 lives in, and the
        two spaces overlap. A reader that skips the check reports EVENT_TX
        (0x03) as a reply to message 0x03.
        """
        channel, middle, code = payload
        self.assertLess(channel, 32,
                        f"{where}: channel {channel} is above the 5-bit "
                        f"ceiling the burst header imposes")
        if middle == w.MESG_EVENT_ID:
            self.assertIn(code, w.EVENT_CODES_BY_VALUE,
                          f"{where}: 0x{code:02X} is not a known event code")
        else:
            self.assertIn(middle, w.MESSAGES,
                          f"{where}: response names unknown message "
                          f"0x{middle:02X}")
            self.assertIn(code, w.RESPONSE_CODES_BY_VALUE,
                          f"{where}: 0x{code:02X} is not a known response "
                          f"code")

    # -- per file -----------------------------------------------------------

    def check_timestamps(self, records, name):
        stamps = [r.seconds for r in records]
        timed = [s for s in stamps if s is not None]
        self.assertIn(len(timed), (0, len(stamps)),
                      f"{name}: mixes real timestamps with '-'. A transcript "
                      f"is either a statement about timing or it is not.")
        for earlier, later in zip(timed, timed[1:]):
            self.assertLessEqual(earlier, later,
                                 f"{name}: timestamps go backwards")

    def check_request_replies(self, records, name):
        """Every MESG_REQUEST is answered somewhere later in the file.

        Ordering across directions is the property the format's
        direction-as-a-column decision exists to preserve, so it is worth one
        check. A refusal comes back as a 0x40 naming 0x4D, not the wanted id.
        """
        for index, record in enumerate(records):
            if (_expects_rejection(record)
                    or record.direction != HOST_TO_DONGLE
                    or len(record.data) < 5
                    or record.data[2] != w.MESG_REQUEST_ID):
                continue
            wanted = record.data[4]
            answered = False
            for later in records[index + 1:]:
                if later.direction != DONGLE_TO_HOST or len(later.data) < 4:
                    continue
                if later.data[2] == wanted:
                    answered = True
                    break
                if (later.data[2] == w.MESG_RESPONSE_EVENT_ID
                        and len(later.data) >= 6
                        and later.data[4] == w.MESG_REQUEST_ID):
                    answered = True
                    break
            self.assertTrue(
                answered,
                f"{name}:{record.lineno}: nothing later in the transcript "
                f"answers the request for 0x{wanted:02X} - neither the "
                f"message itself nor a 0x40 refusing 0x4D")


class TestHandmadeFixtures(ReplayMixin, unittest.TestCase):
    """The fixtures in tools/vectors/, which are committed and always run."""

    def test_the_fixtures_are_still_there(self):
        # If these vanish, every other test in this file silently stops
        # checking anything. Say so instead.
        self.assertTrue(discover(VECTORS_DIR),
                        f"no .antser fixtures in {VECTORS_DIR} - the harness "
                        f"would run against nothing")

    def test_replay(self):
        for path in discover(VECTORS_DIR):
            with self.subTest(os.path.basename(path)):
                self.replay_file(path)

    def test_the_malformed_case_is_actually_malformed(self):
        # The convention has to be load-bearing in both directions: a record
        # under a 'malformed' case that happens to be a perfectly good frame
        # would let the fixture claim coverage it does not have. At least one
        # such record must exist, and it must genuinely be rejected.
        found = 0
        for path in discover(VECTORS_DIR):
            for record in read_antser_file(path):
                if _expects_rejection(record):
                    found += 1
                    self.assertIsNone(w.unframe(record.data))
        self.assertGreater(found, 0,
                           "no fixture exercises the malformed-case path")

    def test_both_timestamp_forms_are_exercised(self):
        forms = set()
        for path in discover(VECTORS_DIR):
            for record in read_antser_file(path):
                forms.add(record.seconds is None)
        self.assertEqual(forms, {True, False},
                         "the fixtures should cover both the '-' column and "
                         "real timestamps; ant_conformance.py writes one and "
                         "ant_trace.py the other")


class TestRealCaptures(ReplayMixin, unittest.TestCase):
    """archive/captures/serial/*.antser, when any exist."""

    @classmethod
    def setUpClass(cls):
        cls.paths = discover(CAPTURES_DIR)
        if not cls.paths:
            raise unittest.SkipTest(
                f"no .antser captures in {CAPTURES_DIR}. This is not a pass: "
                f"nothing recorded from a real host has been replayed. One is "
                f"produced by calling ANT_DLL.dll's ANT_SetDebugLogDirectory "
                f"before ANT_Init, running a session, and normalising the "
                f"resulting Device0.txt with tools/ant_trace.py - see that "
                f"directory's README for the shopping list. Until then the "
                f"hand-assembled fixtures in tools/vectors/ are the only "
                f"thing exercising this harness.")

    def test_replay(self):
        for path in self.paths:
            with self.subTest(os.path.basename(path)):
                self.replay_file(path)


class TestTheParserItself(unittest.TestCase):
    """A harness with a broken reader reports success on nothing."""

    HEADER = HEADER_PREFIX + " <seconds|-> <dir> <framed message, hex>\n"

    def test_minimal_file(self):
        records = read_antser(self.HEADER + "- > a4014a00ef\n")
        self.assertEqual(len(records), 1)
        self.assertIsNone(records[0].seconds)
        self.assertEqual(records[0].direction, ">")
        self.assertEqual(records[0].data.hex(), "a4014a00ef")

    def test_timestamps_and_trailing_comments(self):
        records = read_antser(
            self.HEADER + "0.014200 < a4016f20ea  # startup\n")
        self.assertEqual(records[0].seconds, 0.0142)
        self.assertEqual(records[0].data.hex(), "a4016f20ea")

    def test_case_markers_attach_to_following_records(self):
        records = read_antser(self.HEADER
                              + "# case one\n- > a4014a00ef\n"
                              + "# case two/malformed\n- > a4024d00541b\n")
        self.assertEqual([r.case for r in records], ["one", "two/malformed"])
        self.assertFalse(_expects_rejection(records[0]))
        self.assertTrue(_expects_rejection(records[1]))

    def test_a_file_with_no_header_is_refused(self):
        with self.assertRaises(AntserError):
            read_antser("- > a4014a00ef\n")

    def test_a_line_it_cannot_read_is_refused(self):
        for bad in ("a4014a00ef",            # no columns
                    "- ! a4014a00ef",        # not a direction
                    "- > zz014a00ef",        # not hex
                    "0.1 > a4014a00e"):      # odd digit count
            with self.subTest(bad):
                with self.assertRaises(AntserError):
                    read_antser(self.HEADER + bad + "\n")

    def test_blank_lines_and_comments_are_skipped(self):
        self.assertEqual(
            read_antser(self.HEADER + "\n# just a note\n\n"), [])


if __name__ == "__main__":
    unittest.main()
