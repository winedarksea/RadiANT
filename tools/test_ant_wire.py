#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Golden frame vectors for the ANT serial framing rules.

    python -m unittest discover -s tools -p "test_*.py"

Runs in the CI `host-tests` job: standard library only, no board, no driver,
no `pyusb`.

## Why this file exists, and the one rule for adding to it

`protocol/ant_wire.yaml` generates both `src/ant_wire.h` and
`tools/ant_wire.py`, so the *constants* cannot drift apart. What generation
cannot check is a **rule** - and a rule is exactly what firmware and tools
both got wrong once: the parser seeded its running XOR at `0` instead of at
the `0xA4` SYNC byte it had just consumed, so every checksum was off by
exactly `0xA4` on both sides, they agreed perfectly with each other and with
nothing else, and the whole suite passed while Zwift never executed a
command. A constant-for-constant cross-check would have passed too - both
sides had `SYNC = 0xA4`.

What catches a rule is a third implementation, which is what the vectors
below are. One absolute rule for extending them: **never obtain an expected
byte string by running frame() and freezing its output** - a vector derived
from the implementation under test agrees with it by construction and looks
like coverage while proving nothing.

Every vector carries a `source`:

* `observed`  - read off real hardware, recorded in `docs/` or `archive/`.
* `doc`       - a decoded example in a document, XOR re-worked here.
* `hand`      - assembled here by hand, XOR shown term by term.
* `negative`  - bytes that must be **rejected**; provenance still matters.

## What is missing

No `Device0.txt` (Garmin's own debug log, the one transcript here produced by
software nobody here wrote) exists in this repository. Everything below is
therefore an observation already in the repo or a hand-derivation; there is
no acknowledged data, no burst run, no extended receive message with the flag
byte and appended fields, and nothing from a genuine ANTUSB-m. Completing the
set needs a human, a Windows box with the libusb-win32 driver, and a real
session logged via `ANT_SetDebugLogDirectory` before `ANT_Init`, normalised
with `tools/ant_trace.py` into `archive/captures/serial/`. It also needs a
genuine ANTUSB-m's capabilities reply recorded alongside ours
(`080800b23200fd8d0f`) to settle any future argument about a capability bit.
"""

from __future__ import annotations

import os
import sys
import unittest
from collections import namedtuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ant_wire as w  # noqa: E402

# `direction` is the `.antser` column: '>' host to dongle, '<' dongle to host.
Vector = namedtuple("Vector", "name direction hex msg_id payload source")


def _v(name, direction, text, msg_id, payload, source):
    return Vector(name, direction, text, msg_id, bytes(payload), source)


# ---------------------------------------------------------------------------
# The vectors. Each `hex` string was typed from its source, not produced by
# frame(); the XOR is worked out term by term so it can be checked by hand.
# ---------------------------------------------------------------------------

GOLDEN = [

    # -- request capabilities -----------------------------------------------
    # docs/ant-serial-protocol.md, "The checksum rule, worked":
    #   A4 ^ 02 = A6   A6 ^ 4D = EB   EB ^ 00 = EB   EB ^ 54 = BF
    # The shortest message with a guaranteed reply, hence the first thing
    # every host-side tool here sends.
    _v("request capabilities", ">", "a4024d0054bf",
       0x4D, [0x00, 0x54],
       "doc: docs/ant-serial-protocol.md 'The checksum rule, worked'"),

    # -- capabilities reply -------------------------------------------------
    # docs/ant-serial-protocol.md, "Capabilities reply, decoded". The nine
    # payload bytes are what this firmware actually answers, identically on
    # nRF52840/USB and nRF54L15/UART (also in ant_wire.OBSERVED_CAPABILITIES,
    # protocol/ant_wire.yaml, docs/backends.md). Worked:
    #   A4^09=AD  AD^54=F9  F9^08=F1  F1^08=F9  F9^00=F9  F9^B2=4B
    #   4B^32=79  79^00=79  79^FD=84  84^8D=09  09^0F=06
    _v("capabilities reply", "<", "a40954080800b23200fd8d0f06",
       0x54, [0x08, 0x08, 0x00, 0xB2, 0x32, 0x00, 0xFD, 0x8D, 0x0F],
       "observed: real reply from this firmware; "
       "docs/ant-serial-protocol.md and ant_wire.OBSERVED_CAPABILITIES"),

    # -- system reset -------------------------------------------------------
    # archive/captures/serial/README.md's worked `.antser` example:
    #   A4 ^ 01 = A5   A5 ^ 4A = EF
    # Every host library opens a session with 0x4A.
    _v("system reset", ">", "a4014a00ef",
       0x4A, [0x00],
       "doc: archive/captures/serial/README.md example transcript"),

    # -- startup message ----------------------------------------------------
    # The reply to 0x4A, carrying STARTUP_COMMAND_RESET (0x20):
    #   A4 ^ 01 = A5   A5 ^ 6F = CA   CA ^ 20 = EA
    # See NEGATIVE below: this frame is printed with checksum FA, not EA, in
    # two places in the repository.
    _v("startup message, command reset", "<", "a4016f20ea",
       0x6F, [0x20],
       "hand: MESG_STARTUP_MESG_ID + STARTUP_COMMAND_RESET, XOR shown"),

    # -- assign channel, whose checksum IS the SYNC byte --------------------
    # Chosen deliberately - see TestSyncInChecksum. [channel 1,
    # CHANNEL_TYPE_SLAVE_RX_ONLY (0x40), network 0]:
    #   A4^03=A7  A7^42=E5  E5^01=E4  E4^40=A4  A4^00=A4
    # Two properties wanted: omitting SYNC gives checksum 0x00 (unremarkable,
    # already present elsewhere); the correct checksum 0xA4 makes the frame
    # end in what looks like the start of the next frame, tripping a reader
    # that resyncs by hunting for 0xA4 instead of trusting LEN.
    _v("assign channel (checksum == SYNC)", ">", "a40342014000a4",
       0x42, [0x01, 0x40, 0x00],
       "hand: MESG_ASSIGN_CHANNEL [channel, type, network], XOR shown"),

    # -- broadcast data whose payload is all SYNC bytes ---------------------
    # Eight 0xA4 data bytes XOR to zero:
    #   A4^09=AD  AD^4E=E3  E3^00=E3  E3^(A4 x8)=E3
    # A payload byte equal to SYNC is legal and unavoidable (no escaping in
    # this protocol), so a reader must be driven by LEN.
    _v("broadcast data, payload is eight SYNC bytes", "<",
       "a4094e00a4a4a4a4a4a4a4a4e3",
       0x4E, [0x00] + [0xA4] * 8,
       "hand: MESG_BROADCAST_DATA [channel, data*8], XOR shown"),

    # -- response event, no error ------------------------------------------
    # [channel 0, the command's own id 0x4B, RESPONSE_NO_ERROR]:
    #   A4^03=A7  A7^40=E7  E7^00=E7  E7^4B=AC  AC^00=AC
    _v("response event: open channel, no error", "<", "a40340004b00ac",
       0x40, [0x00, 0x4B, 0x00],
       "hand: MESG_RESPONSE_EVENT [channel, id, code], XOR shown"),

    # -- unsolicited channel event -----------------------------------------
    # Same 0x40 frame with MESG_EVENT_ID (0x01) in the middle byte, so byte 2
    # is an event code (EVENT_TX), not a response code - a reader skipping
    # the middle-byte check would report EVENT_TX as a reply to message 0x03.
    #   A4^03=A7  A7^40=E7  E7^00=E7  E7^01=E6  E6^03=E5
    _v("channel event: EVENT_TX", "<", "a40340000103e5",
       0x40, [0x00, 0x01, 0x03],
       "hand: MESG_RESPONSE_EVENT with MESG_EVENT_ID marker, XOR shown"),

    # -- advanced burst at its largest block --------------------------------
    # One burst header byte + ADV_BURST_BLOCK_MAX (24) data bytes, LEN=25.
    # Header 0x20 is sequence 1, channel 0. Data 00..17 XORs to zero:
    #   A4^19=BD  BD^72=CF  CF^20=EF  EF^00=EF
    # Proves MAX_SIZE_VALUE cannot be 20: a msg_body[20] would make
    # handle_burst() compute size=19, fail its size%8 check, and silently
    # drop every 24-byte block.
    _v("advanced burst, 24-byte block", ">",
       "a4197220000102030405060708090a0b0c0d0e0f1011121314151617ef",
       0x72, [0x20] + list(range(24)),
       "hand: MESG_ADV_BURST_DATA at ADV_BURST_BLOCK_MAX, XOR shown"),

    # -- the largest frame the LEN byte can express -------------------------
    # LEN = MAX_SIZE_VALUE = 38, frame = MAX_FRAME_SIZE = 42 bytes. No
    # defined message declares a payload this long (longest is 25 above) -
    # the buffer ceiling is set by what LEN can *say*, not by the longest
    # message anyone has thought of. 0x7C is the one id whose declared
    # length is 'var'. 38 zero bytes XOR to zero:
    #   A4^26=82  82^7C=FE  FE^(38 zeros)=FE
    _v("maximum-size frame (LEN = 38)", ">",
       "a4267c" + "00" * 38 + "fe",
       0x7C, [0x00] * 38,
       "hand: LEN at MAX_SIZE_VALUE, XOR shown"),
]


# ---------------------------------------------------------------------------
# Vectors that must be REJECTED. Provenance matters here too.
# ---------------------------------------------------------------------------

Negative = namedtuple("Negative", "name hex why source")

NEGATIVE = [
    # Found printed as a valid example in archive/captures/serial/README.md
    # and in tools/ant_trace.py's error text. Checksum is wrong:
    # A4 ^ 01 ^ 6F ^ 20 = EA, not FA. Kept as a negative rather than asserted
    # against the documents' text so this stays correct even if those files
    # are fixed later.
    Negative("startup message with a typo'd checksum", "a4016f20fa",
             "A4 ^ 01 ^ 6F ^ 20 = 0xEA; this says 0xFA",
             "negative: printed as valid in archive/captures/serial/"
             "README.md and tools/ant_trace.py's error text"),

    # Same README's third example line. LEN says 2, so the checksum byte is
    # the 0x00 at index 5, but the XOR through index 4 is 0xBF - a stray 0x00
    # spliced into `a4024d0054bf` before the checksum.
    Negative("request capabilities with a stray byte", "a4024d005400bf",
             "LEN = 2 makes index 5 the checksum, and 0x00 != 0xBF",
             "negative: printed as valid in archive/captures/serial/"
             "README.md"),

    # The checksum bug itself: 0x1B is what you get seeding the XOR at 0 and
    # starting after SYNC. The single most important line in the file.
    Negative("request capabilities, SYNC omitted from the checksum",
             "a4024d00541b",
             "0x02 ^ 0x4D ^ 0x00 ^ 0x54 = 0x1B; the correct value is 0xBF",
             "negative: the historical bug, reproduced"),

    # 0xA5 is SYNC for the bidirectional/asynchronous serial variants; this
    # dongle never emits or accepts it.
    Negative("valid frame under the wrong SYNC", "a5024d0054be",
             "SYNC_RX (0xA5) is not SYNC_TX (0xA4)",
             "hand: SYNC_RX substituted, checksum recomputed for it"),
]


def _naive_checksum(frame_bytes):
    """The wrong checksum: XOR from LEN onwards, SYNC left out.

    Written out here rather than imported, so the test compares the real
    implementation against a rule stated independently of it.
    """
    total = 0
    for byte in frame_bytes[1:-1]:
        total ^= byte
    return total


class TestGoldenVectors(unittest.TestCase):
    """frame() must reproduce bytes it never produced."""

    def test_frame_reproduces_every_vector(self):
        for vec in GOLDEN:
            with self.subTest(vec.name):
                self.assertEqual(w.frame(vec.msg_id, vec.payload).hex(),
                                 vec.hex,
                                 f"{vec.name} ({vec.source})")

    def test_unframe_recovers_every_vector(self):
        for vec in GOLDEN:
            with self.subTest(vec.name):
                got = w.unframe(bytes.fromhex(vec.hex))
                self.assertIsNotNone(got, f"{vec.name} was rejected")
                msg_id, payload = got
                self.assertEqual(msg_id, vec.msg_id)
                self.assertEqual(payload, vec.payload)

    def test_checksum_matches_the_last_byte_of_every_vector(self):
        for vec in GOLDEN:
            with self.subTest(vec.name):
                raw = bytes.fromhex(vec.hex)
                self.assertEqual(w.checksum(raw[:-1]), raw[-1])

    def test_len_byte_counts_the_payload_only(self):
        for vec in GOLDEN:
            with self.subTest(vec.name):
                raw = bytes.fromhex(vec.hex)
                self.assertEqual(raw[1], len(vec.payload))
                self.assertEqual(len(raw), len(vec.payload) + w.MSG_OVERHEAD)

    def test_every_vector_starts_with_sync(self):
        for vec in GOLDEN:
            with self.subTest(vec.name):
                self.assertEqual(bytes.fromhex(vec.hex)[0], w.SYNC_TX)

    def test_vector_message_ids_are_known(self):
        for vec in GOLDEN:
            with self.subTest(vec.name):
                self.assertIn(vec.msg_id, w.MESSAGES,
                              f"{vec.name} uses id 0x{vec.msg_id:02X}")


class TestNegativeVectors(unittest.TestCase):
    def test_every_negative_vector_is_rejected(self):
        for neg in NEGATIVE:
            with self.subTest(neg.name):
                self.assertIsNone(w.unframe(bytes.fromhex(neg.hex)),
                                  f"{neg.name} was accepted: {neg.why} "
                                  f"[{neg.source}]")


class TestSyncInChecksum(unittest.TestCase):
    """The rule that cost a week, checked from three directions."""

    def test_error_is_exactly_the_sync_byte_on_every_vector(self):
        # Leaving SYNC out shifts the checksum by exactly 0xA4 in every frame,
        # both directions (docs/gotchas.md, docs/ant-serial-protocol.md).
        for vec in GOLDEN:
            with self.subTest(vec.name):
                raw = bytes.fromhex(vec.hex)
                self.assertEqual(raw[-1] ^ _naive_checksum(raw), w.SYNC_TX)

    def test_the_documented_tell_on_the_worked_example(self):
        # docs/ant-serial-protocol.md: "Note that 0xBF ^ 0x1B = 0xA4".
        self.assertEqual(0xBF ^ 0x1B, 0xA4)
        self.assertEqual(_naive_checksum(bytes.fromhex("a4024d0054bf")), 0x1B)

    def test_omitting_sync_is_rejected_on_every_vector(self):
        for vec in GOLDEN:
            with self.subTest(vec.name):
                raw = bytearray(bytes.fromhex(vec.hex))
                raw[-1] = _naive_checksum(raw)
                self.assertIsNone(w.unframe(bytes(raw)),
                                  f"{vec.name} accepted a SYNC-omitted "
                                  f"checksum")

    def test_the_wrong_answer_can_look_entirely_plausible(self):
        # On the assign-channel vector the SYNC-omitted checksum is 0x00 (an
        # unremarkable byte already present twice) while the correct one is
        # 0xA4 - a hex dump alone cannot tell them apart, only the rule can.
        raw = bytes.fromhex("a40342014000a4")
        self.assertEqual(_naive_checksum(raw), 0x00)
        self.assertEqual(raw[-1], 0xA4)
        self.assertEqual(w.checksum(raw[:-1]), 0xA4)
        self.assertIsNone(w.unframe(raw[:-1] + b"\x00"))

    def test_checksum_covers_the_sync_byte_by_construction(self):
        # A property rather than a vector: an implementation skipping byte 0
        # would pass every round-trip test in this file except this one.
        head = bytes([w.SYNC_TX, 0x02, 0x4D, 0x00, 0x54])
        other = bytes([0x00]) + head[1:]
        self.assertNotEqual(w.checksum(head), w.checksum(other))
        self.assertEqual(w.checksum(head) ^ w.checksum(other), w.SYNC_TX)


class TestObservedCapabilities(unittest.TestCase):
    """The one payload in the tables that came off real hardware."""

    def test_the_reply_frame_carries_the_observed_bytes(self):
        raw = bytes.fromhex("a40954080800b23200fd8d0f06")
        self.assertEqual(raw[3:-1], w.OBSERVED_CAPABILITIES)

    def test_the_declared_size_matches_the_frame(self):
        raw = bytes.fromhex("a40954080800b23200fd8d0f06")
        self.assertEqual(raw[1], w.MESG_CAPABILITIES_SIZE)
        self.assertEqual(len(w.OBSERVED_CAPABILITIES),
                         w.MESG_CAPABILITIES_SIZE)

    def test_the_two_load_bearing_zeros(self):
        # Scan mode and LED both report OFF, which is why a host holding
        # ANT_OpenRxScanMode/ANT_EnableLED never sends 0x5B or 0x68
        # (docs/ant-serial-protocol.md).
        byte4 = w.OBSERVED_CAPABILITIES[
            w.CAPABILITIES_OFFSET_ADVANCED_OPTIONS_2]
        self.assertEqual(byte4 & w.CAPABILITIES_SCAN_MODE_ENABLED, 0)
        self.assertEqual(byte4 & w.CAPABILITIES_LED_ENABLED, 0)


class TestRoundTrip(unittest.TestCase):
    def test_every_payload_length_up_to_the_ceiling(self):
        for size in range(0, w.MAX_SIZE_VALUE + 1):
            with self.subTest(size=size):
                payload = bytes((i * 7 + 3) & 0xFF for i in range(size))
                raw = w.frame(w.MESG_BROADCAST_DATA_ID, payload)
                self.assertEqual(len(raw), size + w.MSG_OVERHEAD)
                self.assertEqual(raw[1], size)
                self.assertEqual(w.unframe(raw),
                                 (w.MESG_BROADCAST_DATA_ID, payload))

    def test_every_defined_message_id(self):
        for msg_id in w.MESSAGES:
            with self.subTest(msg_id=f"0x{msg_id:02X}"):
                raw = w.frame(msg_id, b"\x00\x01\x02")
                self.assertEqual(w.unframe(raw), (msg_id, b"\x00\x01\x02"))

    def test_a_payload_of_nothing_but_sync_bytes(self):
        payload = bytes([w.SYNC_TX]) * 8
        raw = w.frame(w.MESG_BROADCAST_DATA_ID, payload)
        self.assertEqual(w.unframe(raw),
                         (w.MESG_BROADCAST_DATA_ID, payload))

    def test_single_bit_flips_anywhere_in_a_frame_are_caught(self):
        # An XOR checksum catches every single-bit error but not every
        # double-bit error - not a defect, since USB bulk has its own CRC and
        # this checksum is here to reject mis-framing, not line noise.
        raw = bytes.fromhex("a4024d0054bf")
        for index in range(len(raw)):
            for bit in range(8):
                with self.subTest(index=index, bit=bit):
                    broken = bytearray(raw)
                    broken[index] ^= 1 << bit
                    self.assertIsNone(w.unframe(bytes(broken)))


class TestMalformedInput(unittest.TestCase):
    def test_empty(self):
        self.assertIsNone(w.unframe(b""))

    def test_shorter_than_the_overhead(self):
        for size in range(0, w.MSG_OVERHEAD):
            with self.subTest(size=size):
                self.assertIsNone(w.unframe(b"\xa4\x02\x4d\x00"[:size]))

    def test_wrong_sync(self):
        # Recomputed for 0xA5 so only the SYNC value is wrong; otherwise the
        # checksum would reject it first and the SYNC check would go untested.
        raw = bytearray(bytes.fromhex("a4024d0054bf"))
        raw[0] = w.SYNC_RX
        raw[-1] = w.checksum(bytes(raw[:-1]))
        self.assertIsNone(w.unframe(bytes(raw)))

    def test_bad_checksum(self):
        raw = bytearray(bytes.fromhex("a4024d0054bf"))
        raw[-1] ^= 0x01
        self.assertIsNone(w.unframe(bytes(raw)))

    def test_len_larger_than_the_bytes_present(self):
        # LEN claims 5 payload bytes; only 2 are here - the ordinary
        # partial-frame case on USB (a frame may span two bulk transfers), so
        # this must be a clean rejection, never a read past the end.
        self.assertIsNone(w.unframe(bytes.fromhex("a4054d0054bf")))

    def test_truncated_by_one_byte(self):
        raw = bytes.fromhex("a4024d0054bf")
        self.assertIsNone(w.unframe(raw[:-1]))

    def test_len_smaller_than_the_bytes_present(self):
        # Trailing bytes are the *next* frame, not an error: unframe() reads
        # one frame and leaves the rest to the caller's ring buffer. Asserted
        # so a future "reject trailing bytes" change cannot pass silently -
        # it would break every reassembling reader in tools/.
        two = bytes.fromhex("a4024d0054bf") + bytes.fromhex("a4014a00ef")
        self.assertEqual(w.unframe(two), (0x4D, b"\x00\x54"))

    def test_a_frame_whose_declared_length_is_absurd(self):
        # LEN = 0xFF: 259 bytes claimed, 4 exist.
        self.assertIsNone(w.unframe(b"\xa4\xff\x4d\x00"))


class TestFrameSizeCeiling(unittest.TestCase):
    """The 38/42 pair, and the arithmetic that makes 20/24 wrong.

    Relationships between constants, not the constants themselves - a
    generated header and module can't disagree about 38, but both could be
    generated from a YAML that says 20 (an earlier draft's value), which a
    24-byte advanced-burst block plus its header byte does not fit inside.
    """

    def test_the_arithmetic_that_defines_the_ceiling(self):
        self.assertEqual(w.MAX_FRAME_SIZE, w.MAX_SIZE_VALUE + w.MSG_OVERHEAD)
        self.assertEqual(w.MAX_DATA_SIZE,
                         w.MAX_SIZE_VALUE - w.CHANNEL_NUM_SIZE)
        self.assertEqual(w.MESG_MAX_SIZE, w.MAX_DATA_SIZE + w.MSG_OVERHEAD)

    def test_mesg_max_size_is_one_short_and_must_not_be_used_for_buffers(self):
        # MESG_MAX_SIZE is exactly one byte less than the largest legal frame,
        # since LEN counts the channel byte that MAX_DATA_SIZE does not.
        # Sizing a buffer from it writes the checksum one past the end.
        self.assertEqual(w.MAX_FRAME_SIZE - w.MESG_MAX_SIZE, 1)

    def test_an_advanced_burst_block_fits(self):
        burst_len = w.CHANNEL_NUM_SIZE + w.ADV_BURST_BLOCK_MAX
        self.assertLessEqual(burst_len, w.MAX_SIZE_VALUE)
        # handle_burst()'s own check on what is left after the header.
        self.assertEqual((burst_len - w.CHANNEL_NUM_SIZE)
                         % w.ANT_MAX_PAYLOAD_SIZE, 0)

    def test_every_declared_message_length_fits(self):
        for msg_id, info in w.MESSAGES.items():
            declared = info["payload_len"]
            if declared == "var":
                continue
            longest = int(str(declared).split("..")[-1])
            with self.subTest(msg_id=f"0x{msg_id:02X}", name=info["name"]):
                self.assertLessEqual(longest, w.MAX_SIZE_VALUE)

    def test_a_frame_at_exactly_the_maximum(self):
        raw = bytes.fromhex("a4267c" + "00" * 38 + "fe")
        self.assertEqual(len(raw), w.MAX_FRAME_SIZE)
        self.assertEqual(raw[1], w.MAX_SIZE_VALUE)
        self.assertEqual(w.unframe(raw), (0x7C, b"\x00" * 38))

    def test_one_byte_over_the_maximum(self):
        # LEN = 39: frame is 43 bytes, one more than any firmware buffer, so
        # it cannot be received whole.
        #   A4^27=83  83^7C=FF  FF^(39 zeros)=FF
        raw = bytes.fromhex("a4277c" + "00" * 39 + "ff")
        self.assertEqual(len(raw), w.MAX_FRAME_SIZE + 1)
        self.assertGreater(raw[1], w.MAX_SIZE_VALUE)
        self.assertEqual(w.checksum(raw[:-1]), raw[-1])
        # unframe() does NOT enforce the ceiling itself (pure framing, LEN
        # can express up to 255) - enforcement is the receive buffer's, so
        # asserting current behaviour here makes a future change to reject
        # in unframe() a deliberate diff rather than a surprise.
        self.assertEqual(w.unframe(raw), (0x7C, b"\x00" * 39))
        self.assertGreater(len(raw), w.MAX_FRAME_SIZE)


class TestFalseFrameHazard(unittest.TestCase):
    """A payload byte equal to SYNC can start a false frame."""

    def test_a_length_driven_reader_splits_a_run_correctly(self):
        # The assign-channel vector ends in 0xA4 by construction, so
        # concatenated with the next frame, the last byte of the first frame
        # looks like the start of the second.
        run = bytes.fromhex("a40342014000a4") + bytes.fromhex("a4024d0054bf")
        frames = []
        offset = 0
        while offset < len(run):
            total = run[offset + 1] + w.MSG_OVERHEAD
            chunk = run[offset:offset + total]
            self.assertIsNotNone(w.unframe(chunk))
            frames.append(chunk)
            offset += total
        self.assertEqual([f.hex() for f in frames],
                         ["a40342014000a4", "a4024d0054bf"])

    def test_hunting_for_the_next_sync_byte_gets_it_wrong(self):
        # A reader resynchronising by searching for 0xA4 finds the checksum
        # of the first frame and starts a frame there; only the checksum
        # throws it away again, which is why the checksum can't be optional.
        run = bytes.fromhex("a40342014000a4") + bytes.fromhex("a4024d0054bf")
        false_start = run.index(w.SYNC_TX, 1)
        self.assertEqual(false_start, 6)          # the checksum, not a frame
        self.assertIsNone(w.unframe(run[false_start:]))


if __name__ == "__main__":
    unittest.main()
