#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Published vectors for tools/radiant_crypto.py.

    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe -m unittest discover -s tools -p "test_*.py"

Four published sets and one cross-check:

  * FIPS-197 C.1 and appendix B          AES-128 block encryption
  * RFC 4493 section 4                   AES-CMAC, including the subkeys and
                                         the empty message
  * SP 800-38A F.5.1                     AES-128-CTR
  * self-consistency                     the SP 800-108 KDF against a
                                         hand-built block, and the two blocks
                                         docs/radiant-security.md pins

The same vectors run in C in radiant_core/tests/src/test_sec_aes.c, and the KDF
and nonce values below appear there as literals, so the two implementations are
checked against each other and not only against themselves. That matters more
than it sounds: both were written from the same identity for MixColumns, and
two implementations of one idea agreeing proves less than either agreeing with
NIST.

No board, no toolchain, no `native_sim` - which is the point. Every C assertion
in this project runs only in CI on Linux, so this is the layer a developer
iterates against on Windows.
"""

from __future__ import annotations

import unittest

try:
    from . import radiant_crypto as rc
except ImportError:                       # run as a script or discovered flat
    import radiant_crypto as rc


# RFC 4493's key, and the SP 800-38A plaintext, which are the same four blocks.
RFC4493_KEY = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
NIST_MESSAGE = bytes.fromhex(
    "6bc1bee22e409f96e93d7e117393172a"
    "ae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52ef"
    "f69f2445df4f9b17ad2b417be66c3710"
)


class TestAes128(unittest.TestCase):
    """FIPS-197."""

    def test_appendix_c1(self):
        got = rc.encrypt_block(
            bytes.fromhex("000102030405060708090a0b0c0d0e0f"),
            bytes.fromhex("00112233445566778899aabbccddeeff"))
        self.assertEqual(got.hex(), "69c4e0d86a7b0430d8cdb78070b4c55a")

    def test_appendix_b(self):
        got = rc.encrypt_block(
            RFC4493_KEY, bytes.fromhex("3243f6a8885a308d313198a2e0370734"))
        self.assertEqual(got.hex(), "3925841d02dc09fbdc118597196a0b32")

    def test_key_length_is_refused_loudly(self):
        # A truncated or padded key is a wrong answer that looks like a right
        # one, so it raises rather than being coerced.
        with self.assertRaises(ValueError):
            rc.encrypt_block(bytes(15), bytes(16))
        with self.assertRaises(ValueError):
            rc.encrypt_block(bytes(16), bytes(15))


class TestCmac(unittest.TestCase):
    """RFC 4493 section 4."""

    def test_subkeys(self):
        k1, k2 = rc.cmac_subkeys(RFC4493_KEY)
        self.assertEqual(k1.hex(), "fbeed618357133667c85e08f7236a8de")
        self.assertEqual(k2.hex(), "f7ddac306ae266ccf90bc11ee46d513b")

    def test_vectors(self):
        for length, want in (
                (0, "bb1d6929e95937287fa37d129b756746"),
                (16, "070a16b46b4d4144f79bdd9dd04a287c"),
                (40, "dfa66747de9ae63030ca32611497c827"),
                (64, "51f0bebf7e3b9d92fc49741779363cfe"),
        ):
            with self.subTest(length=length):
                got = rc.cmac(RFC4493_KEY, NIST_MESSAGE[:length])
                self.assertEqual(got.hex(), want)

    def test_multiple_of_sixteen_uses_the_other_subkey(self):
        # The case an incremental implementation gets wrong: a message whose
        # length is a multiple of 16 takes K1 and no padding, and one byte
        # shorter takes K2 and a 0x80 pad. If the two agreed, one of them would
        # be wrong.
        a = rc.cmac(RFC4493_KEY, NIST_MESSAGE[:16])
        b = rc.cmac(RFC4493_KEY, NIST_MESSAGE[:15])
        self.assertNotEqual(a, b)


class TestCtr(unittest.TestCase):
    """SP 800-38A F.5.1, CTR-AES128.Encrypt."""

    IV = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
    CIPHERTEXT = bytes.fromhex(
        "874d6191b620e3261bef6864990db6ce"
        "9806f66b7970fdff8617187bb9fffdff"
        "5ae4df3edbd5d35e5b4f09020db03eab"
        "1e031dda2fbe03d1792170a0f3009cee"
    )

    def test_encrypt(self):
        got = rc.ctr_xor(RFC4493_KEY, self.IV, NIST_MESSAGE)
        self.assertEqual(got, self.CIPHERTEXT)

    def test_is_its_own_inverse(self):
        back = rc.ctr_xor(RFC4493_KEY, self.IV, self.CIPHERTEXT)
        self.assertEqual(back, NIST_MESSAGE)

    def test_partial_block(self):
        # Five bytes is the length X_CONF actually uses - bytes [2..6] of a
        # page - so it is worth a vector of its own rather than trusting that
        # truncating a 16-byte result is the same thing.
        got = rc.ctr_xor(RFC4493_KEY, self.IV, NIST_MESSAGE[:5])
        self.assertEqual(got, self.CIPHERTEXT[:5])

    def test_counter_increments_across_the_whole_block(self):
        # A counter block of all-ones must carry all the way into byte 0, not
        # wrap the last byte in place. RadiANT never reaches this, but two
        # implementations disagreeing here would diverge silently on the first
        # long message.
        iv = b"\xff" * 16
        stream = rc.ctr_xor(RFC4493_KEY, iv, bytes(32))
        self.assertEqual(stream[:16], rc.encrypt_block(RFC4493_KEY, iv))
        self.assertEqual(stream[16:], rc.encrypt_block(RFC4493_KEY, bytes(16)))


class TestPinnedBlocks(unittest.TestCase):
    """docs/radiant-security.md section 3.3, byte for byte."""

    def test_nonce_block(self):
        got = rc.nonce_block(0x11223344, 0xBEEF, 0x0102, rc.DOM_CTR)
        self.assertEqual(got.hex(), "44332211efbe02010100000000000000")
        self.assertEqual(len(got), 16)

    def test_domain_byte_separates_keystream_from_mac(self):
        # The field the first draft omitted. Without it a keystream block and a
        # MAC block can coincide under one (epoch, devnum, counter), which is a
        # scheme that passes every test and leaks the tag key.
        args = (7, 0x1234, 9)
        ctr = rc.nonce_block(*args, rc.DOM_CTR)
        mac = rc.nonce_block(*args, rc.DOM_SPREAD_MAC)
        desc = rc.nonce_block(*args, rc.DOM_DESC_MAC)
        self.assertEqual(len({ctr, mac, desc}), 3)
        self.assertEqual(ctr[8], 0x01)
        self.assertEqual(mac[8], 0x02)
        self.assertEqual(desc[8], 0x03)

    def test_kdf_block(self):
        got = rc.kdf_block("enc", 0x11223344, 0xBEEF)
        self.assertEqual(got.hex(), "01656e6300443322 11efbe0080".replace(" ", ""))

    def test_kdf_is_cmac_over_that_block(self):
        root = bytes(range(16))
        for label in ("enc", "auth", "cmd"):
            with self.subTest(label=label):
                self.assertEqual(
                    rc.kdf(root, label, 0x11223344, 0xBEEF),
                    rc.cmac(root, rc.kdf_block(label, 0x11223344, 0xBEEF)))

    def test_kdf_vectors_shared_with_the_c_implementation(self):
        # These literals also appear in radiant_core/tests/src/test_sec_aes.c.
        # Neither implementation is authoritative; agreeing is the assertion.
        root = bytes(range(16))
        self.assertEqual(rc.kdf(root, "enc", 0x11223344, 0xBEEF).hex(),
                         "58d0157f06ca49c14144c2db93e91b40")
        self.assertEqual(rc.kdf(root, "auth", 0x11223344, 0xBEEF).hex(),
                         "6070645b3a9f3eac4a9eafa05b777112")
        self.assertEqual(rc.kdf(root, "id", 0, 0xBEEF).hex(),
                         "5a6a14a0289735712bf7d19a0230887f")
        self.assertEqual(rc.kdf(root, "cmd", 0x11223344, 0xBEEF).hex(),
                         "4ba0c7a190d3dd0e45243a1dee283b29")

    def test_id_label_is_epoch_less(self):
        # Pinned so two implementations cannot differ: "id" uses epoch = 0, and
        # passing anything else is a mistake rather than a variant.
        with self.assertRaises(ValueError):
            rc.kdf(bytes(16), "id", 1, 0)

    def test_base_devnum_separates_two_sensors_under_one_root(self):
        # A household sharing one root across two same-type sensors: binding
        # base_devnum into the KDF is what gives each its own keys.
        root = bytes(range(16))
        self.assertNotEqual(rc.kdf(root, "enc", 5, 1),
                            rc.kdf(root, "enc", 5, 2))

    def test_every_derived_key_differs(self):
        keys = rc.derive_keys(bytes(range(16)), 42, 0x0999)
        self.assertEqual(len(set(keys.values())), 4)


class TestTransforms(unittest.TestCase):
    """The shapes the transforms have, rather than their values."""

    ROOT = bytes(range(16))
    EPOCH = 0x00010000
    DEVNUM = 0x1234

    def keys(self):
        return rc.derive_keys(self.ROOT, self.EPOCH, self.DEVNUM)

    def test_conf_leaves_the_clear_bytes_alone(self):
        page = bytes([0x01, 0x07, 1, 2, 3, 4, 5, 0xAA])
        k_enc = self.keys()[rc.LABEL_ENC]
        ct = rc.conf_transform(k_enc, self.EPOCH, self.DEVNUM, 7, page)
        self.assertEqual(ct[0:2], page[0:2])   # page number and counter
        self.assertEqual(ct[7], page[7])       # tag space
        self.assertNotEqual(ct[2:7], page[2:7])

    def test_conf_round_trips(self):
        page = bytes([0x03, 0x40, 9, 8, 7, 6, 5, 0x00])
        k_enc = self.keys()[rc.LABEL_ENC]
        ct = rc.conf_transform(k_enc, self.EPOCH, self.DEVNUM, 0x40, page)
        back = rc.conf_transform(k_enc, self.EPOCH, self.DEVNUM, 0x40, ct)
        self.assertEqual(back, page)

    def test_tag_length_is_eight_times_w_bits(self):
        k_auth = self.keys()[rc.LABEL_AUTH]
        for window in rc.LEGAL_W:
            with self.subTest(w=window):
                packets = [bytes([0x01, i, 0, 0, 0, 0, 0, 0])
                           for i in range(window)]
                tag = rc.spread_tag(k_auth, self.EPOCH, self.DEVNUM, 0, packets)
                self.assertEqual(len(tag), window)

    def test_w_of_one_is_refused(self):
        # An inline 16-bit tag needs bytes [6..7], no text defines that layout,
        # and a descriptor declaring a field at bit offsets 32..39 would
        # silently overlap it. W=1 is reserved for the command page.
        with self.assertRaises(ValueError):
            rc.spread_tag(bytes(16), 0, 0, 0, [bytes(8)])

    def test_page_number_is_inside_the_message(self):
        # D1: leave byte [0] out and flipping the page number reinterprets the
        # same authenticated bits against a different schema.
        k_auth = self.keys()[rc.LABEL_AUTH]
        a = [bytes([0x01, 0, 1, 2, 3, 4, 5, 0]), bytes([0x01, 1, 1, 2, 3, 4, 5, 0])]
        b = [bytes([0x02, 0, 1, 2, 3, 4, 5, 0]), bytes([0x01, 1, 1, 2, 3, 4, 5, 0])]
        self.assertNotEqual(rc.spread_tag(k_auth, self.EPOCH, self.DEVNUM, 0, a),
                            rc.spread_tag(k_auth, self.EPOCH, self.DEVNUM, 0, b))

    def test_tag_byte_position_is_excluded_from_its_own_message(self):
        k_auth = self.keys()[rc.LABEL_AUTH]
        a = [bytes([0x01, 0, 1, 2, 3, 4, 5, 0x00]),
             bytes([0x01, 1, 1, 2, 3, 4, 5, 0x00])]
        b = [bytes([0x01, 0, 1, 2, 3, 4, 5, 0xFF]),
             bytes([0x01, 1, 1, 2, 3, 4, 5, 0xFF])]
        self.assertEqual(rc.spread_tag(k_auth, self.EPOCH, self.DEVNUM, 0, a),
                         rc.spread_tag(k_auth, self.EPOCH, self.DEVNUM, 0, b))


class TestWindowArithmetic(unittest.TestCase):
    """D2: the window is derived from the counter, never from arrival order."""

    def test_window_start_and_tag_index(self):
        for window in rc.LEGAL_W:
            for counter in range(0, 300):
                start = rc.window_start(counter, window)
                self.assertEqual(start % window, 0)
                self.assertTrue(start <= counter < start + window)
                self.assertEqual(rc.tag_byte_index(counter, window),
                                 counter - start)

    def test_a_lost_packet_does_not_desynchronise_the_next_window(self):
        # The whole reason the index is counter-derived. Drop counter 5 and
        # counter 6 still lands where it belongs.
        window = 4
        self.assertEqual(rc.window_start(6, window), 4)
        self.assertEqual(rc.tag_byte_index(6, window), 2)

    def test_every_legal_w_divides_both_wrap_points(self):
        # The property that makes the scheme self-synchronising through a
        # byte-counter wrap and a 16-bit counter wrap alike. A future W of 3,
        # 5 or 6 would break resynchronisation silently.
        for window in rc.LEGAL_W:
            self.assertEqual(256 % window, 0)
            self.assertEqual(65536 % window, 0)


class TestCounterReconstruction(unittest.TestCase):
    """D4 and D14: the counter comes from time, and a wrap moves the epoch."""

    def test_expected_index_at_four_hertz(self):
        # 8182 counts of 1/32768 s is the ANT+ 4 Hz period.
        period = 8182
        self.assertEqual(rc.expected_index(0, period), 0)
        one_second = 1000000
        self.assertAlmostEqual(rc.expected_index(10 * one_second, period), 40,
                               delta=1)

    def test_nearest_rollover_is_picked(self):
        # The receiver sees byte 0x02 with an expected index of 0x00FF: the
        # right answer is 0x0102, not 0x0002.
        counter, delta = rc.resolve_counter(0x00FF, 0x02)
        self.assertEqual(counter, 0x0102)
        self.assertEqual(delta, 0)

    def test_a_wrap_reports_an_epoch_advance(self):
        counter, delta = rc.resolve_counter(0x10000, 0x00)
        self.assertEqual(counter, 0x0000)
        self.assertEqual(delta, 1)

    def test_mid_epoch_join_needs_no_arrival_history(self):
        # The normal case, not an edge case: a receiver that has heard nothing
        # still resolves the counter from the epoch phase alone.
        counter, _ = rc.resolve_counter(5000, 5000 & 0xFF)
        self.assertEqual(counter, 5000)


if __name__ == "__main__":
    unittest.main()
