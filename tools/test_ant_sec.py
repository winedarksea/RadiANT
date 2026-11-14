# SPDX-License-Identifier: Apache-2.0

"""Round-trip the RadiANT security messages, and pin what they refuse.

The encode/decode pairs are the easy half. The half worth writing is the
refusals: every ValueError below corresponds to a check the firmware also makes,
and a host library that let one through would produce a message the device
rejects with a code the user then has to interpret. Refusing locally, with a
sentence explaining why, is the difference between a tool and a byte-packer.

Where a check exists on both sides, the test names the C function that makes it
too, so a future change to one has a chance of finding the other.
"""

import struct
import unittest

import ant_sec
from ant_sec import (
    LEGAL_W,
    PAGE_HI_MAX,
    PAGE_LO_MIN,
    STATUS_LEN,
    STATUS_VERSION,
    SW_AUTH,
    SW_CONF,
    SW_DESC_CONF,
    SW_DROP_UNVER,
    SW_VERDICT_EVENT,
    VERDICT_UNVERIFIED,
    VERDICT_VERIFIED,
    SecStatus,
    decode_config,
    decode_epoch,
    decode_set_key,
    decode_status,
    decode_verdict,
    encode_config,
    encode_epoch,
    encode_set_key,
    encode_status,
    switches_str,
    verdict_str,
)

KEY = bytes(range(16))


class TestConfig(unittest.TestCase):
    """0xF1. Mirrors radiant_sec_configure() and antr_sec_config()."""

    def test_round_trip(self):
        body = encode_config(3, SW_AUTH | SW_CONF, 4, 0x01, 0x0F)
        self.assertEqual(5, len(body))
        got = decode_config(body)
        self.assertEqual(3, got["channel"])
        self.assertEqual(SW_AUTH | SW_CONF, got["switches"])
        self.assertEqual(4, got["w"])
        self.assertEqual(0x01, got["page_lo"])
        self.assertEqual(0x0F, got["page_hi"])

    def test_the_switches_are_the_bits_the_firmware_uses(self):
        # radiant/src/radiant_sec_host.c BUILD_ASSERTs these four, so this
        # is the Python end of a pin that already exists in C.
        self.assertEqual(0x01, SW_CONF)
        self.assertEqual(0x02, SW_AUTH)
        self.assertEqual(0x04, SW_DROP_UNVER)
        self.assertEqual(0x08, SW_DESC_CONF)

    def test_the_two_transforms_are_independent(self):
        # Not a ladder. X_AUTH alone is the most useful setting in the table
        # and must not require X_CONF, and vice versa.
        for switches in (SW_AUTH, SW_CONF, SW_AUTH | SW_CONF, 0):
            with self.subTest(switches=switches):
                decoded = decode_config(encode_config(0, switches, 2))
                self.assertEqual(switches, decoded["switches"])

    def test_a_reserved_switch_bit_is_refused(self):
        # Refused rather than masked: masking would enable the switches this
        # version understands and silently drop the one it does not, which is
        # the worst of both answers.
        for bit in (0x20, 0x40, 0x80):
            with self.subTest(bit=bit), self.assertRaises(ValueError):
                encode_config(0, SW_AUTH | bit, 2)

    def test_the_verdict_event_bit_needs_x_auth(self):
        # Bit 4 asks for 0xF8, and only X_AUTH has a verdict to send.
        self.assertEqual(0x10, SW_VERDICT_EVENT)
        body = encode_config(3, SW_AUTH | SW_VERDICT_EVENT, 2)
        self.assertEqual(SW_AUTH | SW_VERDICT_EVENT,
                         decode_config(body)["switches"])
        encode_config(0, SW_AUTH | SW_DROP_UNVER | SW_VERDICT_EVENT, 4)
        for switches in (SW_VERDICT_EVENT, SW_CONF | SW_VERDICT_EVENT,
                         SW_DROP_UNVER | SW_VERDICT_EVENT):
            with self.subTest(switches=switches), \
                    self.assertRaises(ValueError):
                encode_config(0, switches, 2)

    def test_only_w_two_four_and_eight(self):
        for w in range(0, 17):
            with self.subTest(w=w):
                if w in LEGAL_W:
                    encode_config(0, SW_AUTH, w)
                else:
                    with self.assertRaises(ValueError):
                        encode_config(0, SW_AUTH, w)

    def test_page_zero_is_refused(self):
        # 0x00 is the descriptor. Securing it would make the node's own
        # descriptor unreadable and the node undiscoverable, from one typo.
        with self.assertRaises(ValueError):
            encode_config(0, SW_AUTH, 2, page_lo=0x00, page_hi=0x0F)

    def test_the_common_pages_are_out_of_reach(self):
        # 0x50 is common page 80, which every ANT+ receiver already understands.
        with self.assertRaises(ValueError):
            encode_config(0, SW_AUTH, 2, page_lo=0x01, page_hi=0x50)

    def test_an_inverted_range_is_refused(self):
        with self.assertRaises(ValueError):
            encode_config(0, SW_AUTH, 2, page_lo=0x0F, page_hi=0x01)

    def test_the_range_bounds_themselves_are_accepted(self):
        decoded = decode_config(
            encode_config(0, SW_AUTH, 2, PAGE_LO_MIN, PAGE_HI_MAX))
        self.assertEqual(PAGE_LO_MIN, decoded["page_lo"])
        self.assertEqual(PAGE_HI_MAX, decoded["page_hi"])


class TestSetKey(unittest.TestCase):
    """0xF2. Mirrors radiant_sec_set_key() and antr_sec_key_set()."""

    def test_round_trip(self):
        body = encode_set_key(1, KEY)
        self.assertEqual(18, len(body))
        got = decode_set_key(body)
        self.assertEqual(1, got["channel"])
        self.assertEqual(128, got["bits"])
        self.assertEqual(KEY, got["key"])

    def test_a_pairing_moves_exactly_sixteen_bytes(self):
        # Everything else - K_enc, K_auth, K_id, K_cmd - is derived from these,
        # which is what makes an out-of-band pairing (a QR code, a typed key)
        # practical at all.
        self.assertEqual(16, len(decode_set_key(encode_set_key(0, KEY))["key"]))

    def test_a_short_key_is_refused(self):
        with self.assertRaises(ValueError):
            encode_set_key(0, KEY[:15])

    def test_a_long_key_is_refused(self):
        with self.assertRaises(ValueError):
            encode_set_key(0, KEY + b"\x10")

    def test_only_128_bits_in_v1(self):
        for bits in (0, 64, 192, 256):
            with self.subTest(bits=bits), self.assertRaises(ValueError):
                encode_set_key(0, KEY, bits=bits)


class TestEpoch(unittest.TestCase):
    """0xF3. Mirrors radiant_sec_set_epoch() and antr_sec_epoch_set()."""

    def test_round_trip(self):
        body = encode_epoch(2, 0x0001BEEF, us_into_epoch=1234567890)
        self.assertEqual(14, len(body))
        got = decode_epoch(body)
        self.assertEqual(2, got["channel"])
        self.assertEqual(0x0001BEEF, got["epoch"])
        self.assertEqual(1234567890, got["us_into_epoch"])
        self.assertFalse(got["real_time"])

    def test_the_real_time_flag_round_trips(self):
        got = decode_epoch(encode_epoch(0, 5, real_time=True))
        self.assertTrue(got["real_time"])
        self.assertEqual(ant_sec.EPOCH_FLAG_REAL_TIME, got["flags"])

    def test_the_epoch_is_little_endian(self):
        # Asserted against bytes laid out by hand rather than by the encoder.
        # A decoder checked only against its own encoder agrees with itself
        # about a byte order it has got backwards.
        body = bytes([0, 0]) + bytes([0x78, 0x56, 0x34, 0x12]) + bytes(8)
        self.assertEqual(0x12345678, decode_epoch(body)["epoch"])

    def test_the_phase_is_little_endian_and_64_bit(self):
        body = bytes([0, 0]) + bytes(4) + struct.pack("<Q", 2**40 + 7)
        self.assertEqual(2**40 + 7, decode_epoch(body)["us_into_epoch"])

    def test_the_headroom_at_the_top_is_reserved(self):
        # A counter wrap advances the epoch by 1, so an epoch with no room
        # above it has nowhere to wrap into.
        encode_epoch(0, ant_sec.EPOCH_MAX)
        with self.assertRaises(ValueError):
            encode_epoch(0, ant_sec.EPOCH_MAX + 1)
        with self.assertRaises(ValueError):
            encode_epoch(0, 0xFFFFFFFF)


class TestStatus(unittest.TestCase):
    """0xF4. Mirrors antr_sec_status_get()."""

    def sample(self, **kw) -> SecStatus:
        base = dict(
            channel=1, version=STATUS_VERSION, switches=SW_AUTH, w=4,
            page_lo=0x01, page_hi=0x0F,
            epoch=0x00ABCDEF, expected_index=0x1234,
            windows_verified=100, windows_unverified=3,
            dropped_non_broadcast=1, dropped_replay=2, dropped_policy=0,
            epoch_advances=7, verdict=VERDICT_VERIFIED, enrolments=9,
            dropped_direction=4)
        base.update(kw)
        return SecStatus(**base)

    def test_round_trip(self):
        st = self.sample()
        body = encode_status(st)
        self.assertEqual(STATUS_LEN, len(body))
        self.assertEqual(st, decode_status(body))

    def test_the_field_offsets_are_the_documented_ones(self):
        # Against bytes laid out by hand, from the 0xF4 description in
        # protocol/ant_wire.yaml. This is the test that would catch a field
        # inserted in the middle by a well-meaning edit.
        body = bytearray(28)
        body[0] = 5           # channel
        body[1] = 1           # structure version
        body[2] = SW_CONF     # switches
        body[3] = 8           # W
        body[4] = 0x02        # page lo
        body[5] = 0x1F        # page hi
        body[6:10] = struct.pack("<I", 0xDEADBEEF & 0x0FFFFFFF)
        body[10:12] = struct.pack("<H", 0xBEEF)
        body[12:14] = struct.pack("<H", 11)
        body[14:16] = struct.pack("<H", 22)
        body[16:18] = struct.pack("<H", 33)
        body[18:20] = struct.pack("<H", 44)
        body[20:22] = struct.pack("<H", 55)
        body[22] = 66
        body[23] = VERDICT_UNVERIFIED
        body[24:26] = struct.pack("<H", 0x0201)  # enrolments
        body[26:28] = struct.pack("<H", 0x0403)  # dropped_direction

        st = decode_status(bytes(body))
        self.assertEqual(5, st.channel)
        self.assertEqual(1, st.version)
        self.assertEqual(SW_CONF, st.switches)
        self.assertEqual(8, st.w)
        self.assertEqual(0x02, st.page_lo)
        self.assertEqual(0x1F, st.page_hi)
        self.assertEqual(0xDEADBEEF & 0x0FFFFFFF, st.epoch)
        self.assertEqual(0xBEEF, st.expected_index)
        self.assertEqual(11, st.windows_verified)
        self.assertEqual(22, st.windows_unverified)
        self.assertEqual(33, st.dropped_non_broadcast)
        self.assertEqual(44, st.dropped_replay)
        self.assertEqual(55, st.dropped_policy)
        self.assertEqual(66, st.epoch_advances)
        self.assertEqual(VERDICT_UNVERIFIED, st.verdict)
        self.assertEqual(0x0201, st.enrolments,
                         "enrolments is not little-endian at [24..25]")
        self.assertEqual(0x0403, st.dropped_direction,
                         "dropped_direction is not little-endian at [26..27]")

    def test_the_layout_is_28_bytes_at_version_1(self):
        # Pinned literally, so a change to either constant has to be made on
        # purpose here as well as in ant_radio.h's ANTR_SEC_STATUS_LEN/_VERSION.
        # The version stays 1 through the beta; the length tells the fields.
        self.assertEqual(28, STATUS_LEN)
        self.assertEqual(1, STATUS_VERSION)

    def test_a_26_byte_body_decodes_without_dropped_direction(self):
        # The layout before [26..27] was appended: same version, shorter.
        body = encode_status(self.sample())[:26]
        st = decode_status(body)
        self.assertIsNone(st.dropped_direction)
        self.assertEqual(9, st.enrolments)

    def test_dropped_direction_saturates_rather_than_wrapping(self):
        st = self.sample(dropped_direction=70000)
        self.assertEqual(0xFFFF,
                         decode_status(encode_status(st)).dropped_direction)

    def test_counters_saturate_rather_than_wrapping(self):
        # A wrapped counter would let a long noisy run look quiet, and "quiet"
        # is exactly the reading these counters exist to disprove.
        st = self.sample(windows_unverified=70000)
        self.assertEqual(0xFFFF, decode_status(encode_status(st)).windows_unverified)

    def test_enrolments_saturates_rather_than_wrapping(self):
        st = self.sample(enrolments=70000)
        self.assertEqual(0xFFFF, decode_status(encode_status(st)).enrolments)

    def test_version_zero_is_refused(self):
        # Firmware writes the version even for an unkeyed channel, so a zero
        # at [1] means this layout is being applied to the wrong bytes - for
        # instance the old 23-byte reply padded out, whose [1] is the switch
        # byte. Decoding it would shift every field by one, silently.
        body = bytearray(encode_status(self.sample()))
        body[1] = 0
        with self.assertRaises(ValueError):
            decode_status(bytes(body))

    def test_a_longer_higher_version_body_still_decodes_the_known_fields(self):
        # Fields are only ever appended, so a reader built for version 1 reads
        # version 1's fields from a later, longer reply and ignores the rest.
        st = self.sample()
        body = bytearray(encode_status(st))
        body[1] = STATUS_VERSION + 1
        body += bytes([0xAA, 0xBB, 0xCC])
        got = decode_status(bytes(body))
        self.assertEqual(STATUS_VERSION + 1, got.version)
        self.assertEqual(st.windows_verified, got.windows_verified)
        self.assertEqual(st.verdict, got.verdict)
        self.assertEqual(st.enrolments, got.enrolments)

    def test_epoch_advances_saturates_in_its_single_byte(self):
        st = self.sample(epoch_advances=1000)
        self.assertEqual(0xFF, decode_status(encode_status(st)).epoch_advances)

    def test_a_short_body_is_refused_rather_than_padded(self):
        with self.assertRaises(ValueError):
            decode_status(bytes(25))

    def test_an_unkeyed_channel_reads_as_clear_not_as_an_error(self):
        # antr_sec_status_get() answers with zeros for a channel nobody keyed,
        # apart from the version byte, which describes the layout rather than
        # the channel. "No security here" is a state a host must be able to
        # read without treating an ordinary channel as a fault.
        body = bytearray(STATUS_LEN)
        body[1] = STATUS_VERSION
        st = decode_status(bytes(body))
        self.assertFalse(st.secured)
        self.assertEqual(ant_sec.VERDICT_CLEAR, st.verdict)
        self.assertEqual(0, st.enrolments)

    def test_the_deliver_policy_alone_is_not_security(self):
        # DROP_UNVER with neither transform on is a delivery policy for a
        # verdict nothing is producing.
        self.assertFalse(self.sample(switches=SW_DROP_UNVER).secured)
        self.assertTrue(self.sample(switches=SW_AUTH).secured)
        self.assertTrue(self.sample(switches=SW_CONF).secured)


class TestHumanReadable(unittest.TestCase):
    def test_switches(self):
        self.assertEqual("none", switches_str(0))
        self.assertEqual("X_AUTH", switches_str(SW_AUTH))
        self.assertEqual("X_CONF|X_AUTH", switches_str(SW_CONF | SW_AUTH))
        self.assertIn("reserved", switches_str(0x80))
        self.assertEqual("X_AUTH|verdict-event",
                         switches_str(SW_AUTH | SW_VERDICT_EVENT))

    def test_verdicts(self):
        self.assertEqual("clear", verdict_str(ant_sec.VERDICT_CLEAR))
        self.assertEqual("verified", verdict_str(VERDICT_VERIFIED))
        self.assertEqual("unverified", verdict_str(VERDICT_UNVERIFIED))
        self.assertIn("unknown", verdict_str(9))


class TestVerdict(unittest.TestCase):
    """0xF8. Mirrors radiant_sec_window_verdict() in ant_radio_radiant.c and
    the bytes test_api_sec_host.c checks on the firmware side."""

    def test_the_bytes_are_ch_verdict_start_le_w_seq_le(self):
        body = bytes([2, VERDICT_UNVERIFIED, 0x34, 0x12, 4, 0x07, 0x01])
        self.assertEqual({"channel": 2, "verdict": VERDICT_UNVERIFIED,
                          "window_start": 0x1234, "w": 4, "seq": 0x0107},
                         decode_verdict(body))

    def test_round_trip(self):
        for w in LEGAL_W:
            for start in (0, w, 0x10000 - w):
                with self.subTest(w=w, start=start):
                    body = ant_sec.encode_verdict(1, VERDICT_VERIFIED,
                                                  start, w, 0xFFFF)
                    self.assertEqual(ant_sec.VERDICT_LEN, len(body))
                    v = decode_verdict(body)
                    self.assertEqual((1, VERDICT_VERIFIED, start, w, 0xFFFF),
                                     (v["channel"], v["verdict"],
                                      v["window_start"], v["w"], v["seq"]))

    def test_a_short_body_is_refused_and_a_long_one_decodes(self):
        with self.assertRaises(ValueError):
            decode_verdict(bytes([0, 1, 0, 0, 2, 0]))
        self.assertEqual(0x0102, decode_verdict(
            bytes([0, 1, 0x02, 0x01, 2, 0, 0, 0xEE]))["window_start"])

    def test_the_decoder_refuses_what_firmware_never_sends(self):
        for verdict in (0, 3, 0xFF):
            with self.subTest(verdict=verdict), \
                    self.assertRaises(ValueError):
                decode_verdict(bytes([0, verdict, 0, 0, 2, 0, 0]))
        for w in (0, 1, 3, 16):
            with self.subTest(w=w), self.assertRaises(ValueError):
                decode_verdict(bytes([0, 1, 0, 0, w, 0, 0]))

    def test_the_encoder_refuses_what_firmware_never_sends(self):
        with self.assertRaises(ValueError):
            ant_sec.encode_verdict(0, ant_sec.VERDICT_CLEAR, 0, 2)
        with self.assertRaises(ValueError):
            ant_sec.encode_verdict(0, VERDICT_VERIFIED, 1, 2)
        with self.assertRaises(ValueError):
            ant_sec.encode_verdict(0, VERDICT_VERIFIED, 0, 3)
        with self.assertRaises(ValueError):
            ant_sec.encode_verdict(0, VERDICT_VERIFIED, 0, 2, 0x10000)

    def test_a_packet_maps_to_its_window_by_byte_one(self):
        # Window 0x01FC at W=4 covers counters 0x01FC..0x01FF. W divides 256,
        # so a window never straddles a wrap of byte [1].
        v = decode_verdict(ant_sec.encode_verdict(0, VERDICT_VERIFIED,
                                                  0x01FC, 4))
        for low in (0xFC, 0xFD, 0xFE, 0xFF):
            with self.subTest(low=low):
                self.assertTrue(ant_sec.verdict_covers(
                    v, 0, bytes([0x05, low, 0, 0, 0, 0, 0, 0]), 0x01, 0x0F))
        for low in (0xFB, 0x00, 0x7C):
            with self.subTest(low=low):
                self.assertFalse(ant_sec.verdict_covers(
                    v, 0, bytes([0x05, low, 0, 0, 0, 0, 0, 0]), 0x01, 0x0F))

    def test_only_a_secured_range_page_on_the_same_channel_is_covered(self):
        # Review item 2: a cleartext page whose byte [1] happens to match a
        # verified window must not inherit its verdict.
        v = decode_verdict(ant_sec.encode_verdict(3, VERDICT_VERIFIED, 0, 2))
        page = bytes([0x05, 0x01, 0, 0, 0, 0, 0, 0])
        self.assertTrue(ant_sec.verdict_covers(v, 3, page, 0x01, 0x0F))
        self.assertFalse(ant_sec.verdict_covers(v, 4, page, 0x01, 0x0F))
        for p0 in (0x00, 0x10, 0x50, 0x52):
            with self.subTest(page=p0):
                self.assertFalse(ant_sec.verdict_covers(
                    v, 3, bytes([p0]) + page[1:], 0x01, 0x0F))


def _page(ctr: int, tag: int = 0) -> bytes:
    return bytes([0x05, ctr & 0xFF, tag, 0, 0, 0, 0, 0])


def _f8(verdict: int, start: int, seq: int, w: int = 2) -> bytes:
    return ant_sec.encode_verdict(0, verdict, start, w, seq)


class TestVerdictResolver(unittest.TestCase):
    """The host rule. Each case is one of the review's attacks on a host that
    credited a packet with any matching VERIFIED."""

    V, U = VERDICT_VERIFIED, VERDICT_UNVERIFIED

    def resolver(self, baseline_seq=None):
        """A resolver at 4 Hz, W=2 (deadline 0.75 s + 0.25 s slack). With
        `baseline_seq`, it has already taken one verdict with that seq, as
        a host that has been listening has."""
        r = ant_sec.VerdictResolver(0, w=2, period_s=0.25, page_lo=0x01,
                                    page_hi=0x0F)
        if baseline_seq is not None:
            r.verdict(_f8(self.V, 0x00FE, baseline_seq), 0.0)
        return r

    def test_the_page_range_is_not_optional(self):
        with self.assertRaises(TypeError):
            ant_sec.VerdictResolver(0, w=2, period_s=0.25)  # noqa

    def test_a_genuine_stream_verifies(self):
        r = self.resolver(baseline_seq=9)
        for c in range(4):
            r.data(_page(c), c * 0.25)
        r.verdict(_f8(self.V, 0, 10), 0.75)
        self.assertEqual([(_page(0), self.V), (_page(1), self.V)], r.settled)
        self.assertEqual(2, len(r.pending))

    def test_the_first_verdict_after_a_start_binds_nothing(self):
        r = self.resolver()
        for c in range(4):
            r.data(_page(c), c * 0.25)
        r.verdict(_f8(self.V, 0, 3), 0.75)
        self.assertEqual([self.U] * 4, [v for _, v in r.settled])
        r.data(_page(4), 1.0)
        r.data(_page(5), 1.25)
        r.verdict(_f8(self.V, 4, 4), 1.25)     # the baseline is set now
        self.assertEqual([self.V, self.V], [v for _, v in r.settled[4:]])

    def test_the_first_verdict_after_a_flush_binds_nothing(self):
        # Review finding 2: a forged counter-4 page was pumped just before the
        # host's 0xF3, which threw its window away, so no UNVERIFIED for it
        # will ever come. It reaches the host after the flush; the first
        # genuine verdict after the flush must not bind it - nor may the
        # later VERIFIED(4) the genuine window earns.
        r = self.resolver(baseline_seq=9)
        r.flush()
        forged = _page(4, tag=0xEE)
        r.data(forged, 0.5)
        for c in range(2, 6):
            r.data(_page(c), c * 0.25)
        r.verdict(_f8(self.V, 2, 12), 1.25)    # seq continuous: no gap
        r.data(_page(6), 1.5)
        r.data(_page(7), 1.75)
        r.verdict(_f8(self.V, 4, 13), 1.75)
        self.assertEqual([self.U], [v for p, v in r.settled if p == forged])

    def test_the_deadline_applies_before_a_verdict(self):
        # A packet older than 2W-1 periods plus slack can't be this verdict's:
        # a host that stalled its reads settles it before applying anything.
        r = self.resolver(baseline_seq=9)
        stale = _page(0)
        r.data(stale, 0.0)
        r.verdict(_f8(self.V, 0, 10), 5.0)
        self.assertEqual([(stale, self.U)], r.settled)

    def test_a_future_forgery_is_bound_to_the_first_verdict(self):
        # The firmware sequence for a forged counter-10 page among genuine
        # ones (test_api_sec_host.c): UNV(10) settles the forgery, and the
        # later VERIFIED(10) only reaches the genuine 10 and 11.
        r = self.resolver(baseline_seq=9)
        forged = _page(10, tag=0xEE)
        r.data(_page(0), 0)
        r.data(_page(1), 0.25)
        r.data(forged, 0.5)
        r.verdict(_f8(self.U, 0, 10), 0.5)
        r.data(_page(2), 0.5)
        r.data(_page(3), 0.75)
        r.verdict(_f8(self.U, 10, 11), 0.75)
        self.assertIn((forged, self.U), r.settled)
        for c in range(4, 12):
            r.data(_page(c), c * 0.25)
            if c % 2:
                r.verdict(_f8(self.V, c - 3, 12 + (c - 5) // 2), c * 0.25)
        r.data(_page(12), 3.0)
        r.data(_page(13), 3.25)
        r.verdict(_f8(self.V, 10, 16), 3.25)
        self.assertEqual([self.U], [v for p, v in r.settled if p == forged])
        self.assertIn((_page(10), self.V), r.settled)

    def test_a_lost_unverified_is_not_replaced_by_a_later_verified(self):
        # The forgery's UNV(10) (seq 11) is lost on the way; the next verdict
        # reveals the gap, and everything pending - the forgery included -
        # settles unverified before VERIFIED(10) is applied.
        r = self.resolver(baseline_seq=9)
        forged = _page(10, tag=0xEE)
        r.data(forged, 0.5)
        r.verdict(_f8(self.U, 0, 10), 0.5)
        r.data(_page(10), 2.5)
        r.data(_page(11), 2.75)
        r.verdict(_f8(self.V, 10, 12), 2.75)   # seq 11 never arrived
        self.assertEqual([self.U], [v for p, v in r.settled if p == forged])
        self.assertEqual(0, len(r.pending))

    def test_an_overflow_settles_every_channel(self):
        r0 = self.resolver(baseline_seq=9)
        r1 = ant_sec.VerdictResolver(1, w=2, period_s=0.25, page_lo=0x01,
                                     page_hi=0x0F)
        r0.data(_page(10, tag=0xEE), 0.5)
        r1.data(_page(3), 0.5)
        ant_sec.overflow_all([r0, r1])
        r0.verdict(_f8(self.V, 10, 10), 0.75)
        self.assertEqual([self.U], [v for _, v in r0.settled])
        self.assertEqual([self.U], [v for _, v in r1.settled])

    def test_a_late_reopen_gets_the_first_unverified(self):
        # A forged counter-0 page after window 0 closed reopens it: the
        # firmware sends UNV(0) twice. The first after each packet binds.
        r = self.resolver(baseline_seq=9)
        r.data(_page(0), 0)
        r.data(_page(1), 0.25)
        forged = _page(0, tag=0xEE)
        r.data(forged, 0.5)
        r.verdict(_f8(self.U, 0, 10), 0.5)
        r.data(_page(2), 0.5)
        r.data(_page(3), 0.75)
        r.verdict(_f8(self.U, 0, 11), 0.75)
        self.assertEqual(3, len(r.settled) - 0)
        self.assertTrue(all(v == self.U for _, v in r.settled))
        self.assertEqual(2, len(r.pending))

    def test_a_verdict_that_never_comes_expires(self):
        r = self.resolver(baseline_seq=9)
        r.data(_page(4), 1.0)
        r.expire(1.0 + 0.75 + 0.2)          # inside 2W-1 periods + slack
        self.assertEqual(1, len(r.pending))
        r.expire(1.0 + 0.75 + 0.3)
        self.assertEqual([(_page(4), self.U)], r.settled)

    def test_a_cleartext_page_is_clear_not_pending(self):
        r = self.resolver(baseline_seq=9)
        self.assertEqual(ant_sec.VERDICT_CLEAR,
                         r.data(bytes([0x50, 0x00, 0, 0, 0, 0, 0, 0]), 0))
        r.verdict(_f8(self.V, 0, 10), 0.25)
        self.assertEqual([ant_sec.VERDICT_CLEAR], [v for _, v in r.settled])


class TestPairing(unittest.TestCase):
    """0xF5. Mirrors radiant_sec_pair.c and antr_sec_pair()."""

    def test_enter_carries_a_timeout(self):
        body = ant_sec.encode_pair_enter(1, 30)
        self.assertEqual(bytes([1, 0x01, 30]), body)

    def test_a_zero_timeout_is_the_default_not_forever(self):
        # A node in pairing mode accepts a key from whoever asks. An interface
        # where 0 meant no bound would turn one forgotten command into a
        # permanently open node, so the device reads 0 as 60 seconds.
        body = ant_sec.encode_pair_enter(0)
        self.assertEqual(0, body[2])
        self.assertEqual(60, ant_sec.PAIR_TIMEOUT_DEFAULT_S)

    def test_leave_is_two_bytes(self):
        self.assertEqual(bytes([2, 0x00]), ant_sec.encode_pair_leave(2))

    def test_a_scalar_must_be_thirty_two_bytes(self):
        ant_sec.encode_pair_scalar(0, bytes(32))
        with self.assertRaises(ValueError):
            ant_sec.encode_pair_scalar(0, bytes(31))
        with self.assertRaises(ValueError):
            ant_sec.encode_pair_scalar(0, bytes(33))

    def test_a_scalar_reply_carries_the_public_key(self):
        pub = bytes(range(32))
        reply = bytes([3, 0x02]) + pub
        got = ant_sec.decode_pair_reply(reply)
        self.assertEqual(3, got["channel"])
        self.assertEqual(0x02, got["subcmd"])
        self.assertEqual(pub, got["public_key"])

    def test_an_exchange_carries_the_peer_key_and_answers_nothing(self):
        # DH only: the exchange installs nothing and has no fingerprint yet,
        # so its reply is the bare echo (review 2026-11-13, item 4).
        pub = bytes(range(32))
        self.assertEqual(bytes([1, 0x03]) + pub,
                         ant_sec.encode_pair_exchange(1, pub))
        got = ant_sec.decode_pair_reply(bytes([1, 0x03]))
        self.assertNotIn("fingerprint", got)

    def test_a_nonce_carries_the_role_then_sixteen_bytes(self):
        n = bytes(range(16))
        self.assertEqual(bytes([2, 0x04, 0x01]) + n,
                         ant_sec.encode_pair_nonce(
                             2, ant_sec.PAIR_ROLE_RESPONDER, n))
        self.assertEqual(bytes([2, 0x04, 0x00]) + n,
                         ant_sec.encode_pair_nonce(
                             2, ant_sec.PAIR_ROLE_INITIATOR, n))
        with self.assertRaises(ValueError):
            ant_sec.encode_pair_nonce(2, 2, n)
        with self.assertRaises(ValueError):
            ant_sec.encode_pair_nonce(2, 0, bytes(15))

    def test_the_commit_and_peer_nonce_carry_sixteen_bytes(self):
        c = bytes(range(16, 32))
        self.assertEqual(bytes([0, 0x05]) + c,
                         ant_sec.encode_pair_peer_commit(0, c))
        self.assertEqual(bytes([0, 0x06]) + c,
                         ant_sec.encode_pair_peer_nonce(0, c))
        for enc in (ant_sec.encode_pair_peer_commit,
                    ant_sec.encode_pair_peer_nonce):
            with self.subTest(enc=enc.__name__):
                with self.assertRaises(ValueError):
                    enc(0, bytes(17))

    def test_a_responders_nonce_reply_carries_the_commitment(self):
        c = bytes(range(16))
        got = ant_sec.decode_pair_reply(bytes([1, 0x04]) + c)
        self.assertEqual(c, got["commit"])
        # An initiator's is the bare echo.
        self.assertNotIn("commit", ant_sec.decode_pair_reply(bytes([1, 0x04])))

    def test_a_peer_nonce_reply_carries_a_u24_fingerprint(self):
        # 999999 = 0x0F423F, little-endian 3F 42 0F.
        reply = bytes([0, 0x06, 0x3F, 0x42, 0x0F])
        got = ant_sec.decode_pair_reply(reply)
        self.assertEqual(999999, got["fingerprint"])

    def test_every_reply_says_which_sub_command_answered(self):
        # A public key, a commitment and a fingerprint are all "bytes after a
        # channel byte", so a host reading a stream needs the echo to tell
        # them apart.
        for sub in (0x00, 0x01, 0x03, 0x04, 0x05):
            with self.subTest(sub=sub):
                got = ant_sec.decode_pair_reply(bytes([1, sub]))
                self.assertEqual(sub, got["subcmd"])
                self.assertNotIn("public_key", got)
                self.assertNotIn("commit", got)
                self.assertNotIn("fingerprint", got)

    def test_a_truncated_reply_is_refused(self):
        with self.assertRaises(ValueError):
            ant_sec.decode_pair_reply(bytes([0]))
        with self.assertRaises(ValueError):
            ant_sec.decode_pair_reply(bytes([0, 0x02]) + bytes(31))
        with self.assertRaises(ValueError):
            ant_sec.decode_pair_reply(bytes([0, 0x04]) + bytes(15))
        with self.assertRaises(ValueError):
            ant_sec.decode_pair_reply(bytes([0, 0x06, 0x11, 0x22]))

    def test_the_sub_command_numbers_are_the_generated_ones(self):
        import ant_wire
        self.assertEqual((0x03, 0x04, 0x05, 0x06),
                         (ant_wire.RADIANT_PAIR_EXCHANGE,
                          ant_wire.RADIANT_PAIR_NONCE,
                          ant_wire.RADIANT_PAIR_PEER_COMMIT,
                          ant_wire.RADIANT_PAIR_PEER_NONCE))

    def test_an_all_zero_nonce_is_refused(self):
        # As the device refuses it: zeros are enrolment's constant.
        with self.assertRaises(ValueError):
            ant_sec.encode_pair_nonce(0, ant_sec.PAIR_ROLE_INITIATOR,
                                      bytes(16))
        with self.assertRaises(ValueError):
            ant_sec.encode_pair_peer_nonce(0, bytes(16))

    def test_a_commitment_mismatch_has_its_own_response_code(self):
        # The MITM signature must not read as an out-of-order call (0x33).
        self.assertEqual(0xF5, ant_sec.PAIR_COMMIT_MISMATCH)
        self.assertNotEqual(0x33, ant_sec.PAIR_COMMIT_MISMATCH)

    def test_the_fingerprint_is_zero_padded(self):
        # "042317" and "42317" are the same number, and a user comparing two
        # screens is comparing strings. An unpadded fingerprint starting with a
        # zero would look like a mismatch and teach the user to ignore the
        # check - which voids the only defence against a man in the middle.
        self.assertEqual("042317", ant_sec.fingerprint_str(42317))
        self.assertEqual("000000", ant_sec.fingerprint_str(0))
        self.assertEqual("999999", ant_sec.fingerprint_str(999999))
        with self.assertRaises(ValueError):
            ant_sec.fingerprint_str(1000000)


class TestMessageIds(unittest.TestCase):
    def test_the_ids_are_the_generated_ones(self):
        # Imported from tools/ant_wire.py, which is generated from
        # protocol/ant_wire.yaml. Asserted here so a renumbering shows up as a
        # test failure rather than as a device that answers INVALID_MESSAGE.
        self.assertEqual(0xF1, ant_sec.MESG_CONFIG)
        self.assertEqual(0xF2, ant_sec.MESG_SET_KEY)
        self.assertEqual(0xF3, ant_sec.MESG_EPOCH)
        self.assertEqual(0xF4, ant_sec.MESG_STATUS)
        self.assertEqual(0xF5, ant_sec.MESG_PAIRING)
        self.assertEqual(0xF8, ant_sec.MESG_VERDICT)


if __name__ == "__main__":
    unittest.main()
