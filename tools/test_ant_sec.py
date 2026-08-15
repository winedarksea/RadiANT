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
    SW_AUTH,
    SW_CONF,
    SW_DESC_CONF,
    SW_DROP_UNVER,
    VERDICT_UNVERIFIED,
    VERDICT_VERIFIED,
    SecStatus,
    decode_config,
    decode_epoch,
    decode_set_key,
    decode_status,
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
        for bit in (0x10, 0x20, 0x40, 0x80):
            with self.subTest(bit=bit), self.assertRaises(ValueError):
                encode_config(0, SW_AUTH | bit, 2)

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
            channel=1, switches=SW_AUTH, w=4, page_lo=0x01, page_hi=0x0F,
            epoch=0x00ABCDEF, expected_index=0x1234,
            windows_verified=100, windows_unverified=3,
            dropped_non_broadcast=1, dropped_replay=2, dropped_policy=0,
            epoch_advances=7, verdict=VERDICT_VERIFIED)
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
        body = bytearray(STATUS_LEN)
        body[0] = 5           # channel
        body[1] = SW_CONF     # switches
        body[2] = 8           # W
        body[3] = 0x02        # page lo
        body[4] = 0x1F        # page hi
        body[5:9] = struct.pack("<I", 0xDEADBEEF & 0x0FFFFFFF)
        body[9:11] = struct.pack("<H", 0xBEEF)
        body[11:13] = struct.pack("<H", 11)
        body[13:15] = struct.pack("<H", 22)
        body[15:17] = struct.pack("<H", 33)
        body[17:19] = struct.pack("<H", 44)
        body[19:21] = struct.pack("<H", 55)
        body[21] = 66
        body[22] = VERDICT_UNVERIFIED

        st = decode_status(bytes(body))
        self.assertEqual(5, st.channel)
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

    def test_counters_saturate_rather_than_wrapping(self):
        # A wrapped counter would let a long noisy run look quiet, and "quiet"
        # is exactly the reading these counters exist to disprove.
        st = self.sample(windows_unverified=70000)
        self.assertEqual(0xFFFF, decode_status(encode_status(st)).windows_unverified)

    def test_epoch_advances_saturates_in_its_single_byte(self):
        st = self.sample(epoch_advances=1000)
        self.assertEqual(0xFF, decode_status(encode_status(st)).epoch_advances)

    def test_a_short_body_is_refused_rather_than_padded(self):
        with self.assertRaises(ValueError):
            decode_status(bytes(STATUS_LEN - 1))

    def test_an_unkeyed_channel_reads_as_clear_not_as_an_error(self):
        # antr_sec_status_get() answers with zeros for a channel nobody keyed.
        # "No security here" is a state a host must be able to read without
        # treating an ordinary channel as a fault.
        st = decode_status(bytes(STATUS_LEN))
        self.assertFalse(st.secured)
        self.assertEqual(ant_sec.VERDICT_CLEAR, st.verdict)

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

    def test_verdicts(self):
        self.assertEqual("clear", verdict_str(ant_sec.VERDICT_CLEAR))
        self.assertEqual("verified", verdict_str(VERDICT_VERIFIED))
        self.assertEqual("unverified", verdict_str(VERDICT_UNVERIFIED))
        self.assertIn("unknown", verdict_str(9))


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

    def test_an_exchange_reply_carries_a_u24_fingerprint(self):
        # 999999 = 0x0F423F, little-endian 3F 42 0F.
        reply = bytes([0, 0x03, 0x3F, 0x42, 0x0F])
        got = ant_sec.decode_pair_reply(reply)
        self.assertEqual(999999, got["fingerprint"])

    def test_every_reply_says_which_sub_command_answered(self):
        # A public key and a fingerprint are both "bytes after a channel byte",
        # so a host reading a stream needs the echo to tell them apart.
        for sub in (0x00, 0x01):
            with self.subTest(sub=sub):
                got = ant_sec.decode_pair_reply(bytes([1, sub]))
                self.assertEqual(sub, got["subcmd"])
                self.assertNotIn("public_key", got)
                self.assertNotIn("fingerprint", got)

    def test_a_truncated_reply_is_refused(self):
        with self.assertRaises(ValueError):
            ant_sec.decode_pair_reply(bytes([0]))
        with self.assertRaises(ValueError):
            ant_sec.decode_pair_reply(bytes([0, 0x02]) + bytes(31))
        with self.assertRaises(ValueError):
            ant_sec.decode_pair_reply(bytes([0, 0x03, 0x11, 0x22]))

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


if __name__ == "__main__":
    unittest.main()
