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


class TestCompatAttestation(unittest.TestCase):
    """docs/radiant-security.md section 11.4 and ADR 0008, byte for byte.

    Every literal in this class also appears in
    radiant_core/tests/src/test_sec_compat.c. Neither implementation computes
    the other's expected value at test time and neither is authoritative:
    agreeing is the assertion, and pinning both is what stops two
    implementations that share a mistake from reporting it as a pass.

    There is no page here and no profile. The subtype's separation of the two
    tiers is asserted by building the nonce inputs directly, because a test that
    checked only a page byte would pass on an implementation that never put the
    subtype anywhere near the MAC.
    """

    K_AUTH = bytes(range(16))
    EPOCH = 0x11223344
    DEVNUM = 0xBEEF
    COUNTER = 0x0501

    #: Seven synthetic transmitted messages - N = 8 means p_1..p_7 - with no
    #: profile meaning of any kind. Byte [0] differs from the rest of each so
    #: that an implementation skipping it, as spread_tag() legitimately does,
    #: is caught.
    MSGS = [bytes([0x10 + i] + [i] * 7) for i in range(7)]

    def test_nonce_block_is_section_3_3_extended_at_position_9(self):
        got = rc.compat_nonce_block(self.EPOCH, self.DEVNUM, self.COUNTER,
                                    rc.COMPAT_SUBTYPE_TIER_I)
        self.assertEqual(got.hex(), "44332211efbe01050401000000000000")
        self.assertEqual(got[8], rc.COMPAT_DOM)
        self.assertEqual(got[9], rc.COMPAT_SUBTYPE_TIER_I)
        self.assertEqual(got[10:], bytes(6))

        got = rc.compat_nonce_block(self.EPOCH, self.DEVNUM, self.COUNTER,
                                    rc.COMPAT_SUBTYPE_TIER_II)
        self.assertEqual(got.hex(), "44332211efbe01050402000000000000")

    def test_the_compat_domain_byte_is_its_own(self):
        # Without it a compat tag and a spread tag over the same (epoch,
        # devnum, counter) are the same block.
        args = (self.EPOCH, self.DEVNUM, self.COUNTER)
        self.assertEqual(rc.COMPAT_DOM, 0x04)
        self.assertNotEqual(rc.compat_nonce_block(*args,
                                                  rc.COMPAT_SUBTYPE_TIER_I),
                            rc.nonce_block(*args, rc.DOM_SPREAD_MAC))

    def test_the_subtype_reaches_the_mac_and_not_only_the_page(self):
        # The assertion the whole subtype pin exists for. A subtype written
        # only into a page byte is chosen by whoever sends the page, so the two
        # tiers' tags over one counter value would be interchangeable.
        args = (self.EPOCH, self.DEVNUM, self.COUNTER)
        one = rc.cmac(self.K_AUTH,
                      rc.compat_nonce_block(*args, rc.COMPAT_SUBTYPE_TIER_I))
        two = rc.cmac(self.K_AUTH,
                      rc.compat_nonce_block(*args, rc.COMPAT_SUBTYPE_TIER_II))
        self.assertEqual(one.hex(), "86a63d51149983aa9294ad0d7d007ad6")
        self.assertEqual(two.hex(), "18d0a91fa67558479b5e3415fe72854e")
        self.assertNotEqual(one, two)

        # The third subtype, which nothing computes yet: pinned now so the
        # value cannot be spent twice before C8 exists.
        three = rc.compat_nonce_block(*args, rc.COMPAT_SUBTYPE_ANNOUNCE)
        self.assertEqual(three[9], 0x03)
        self.assertEqual(len({rc.COMPAT_SUBTYPE_TIER_I,
                              rc.COMPAT_SUBTYPE_TIER_II,
                              rc.COMPAT_SUBTYPE_ANNOUNCE}), 3)

    def test_an_illegal_subtype_is_refused(self):
        # The subtype is a nibble because it also has to fit in a page byte
        # beside something else.
        for sub in (0, 0x10, 0xFF, -1):
            with self.subTest(sub=sub), self.assertRaises(ValueError):
                rc.compat_nonce_block(self.EPOCH, self.DEVNUM, self.COUNTER,
                                      sub)

    def test_tier1_vector_shared_with_the_c_implementation(self):
        got = rc.compat_tier1_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                  self.COUNTER)
        self.assertEqual(got.hex(), "86a63d5114")
        # trunc40 takes the FIRST five bytes; which end a truncation takes is
        # exactly what two implementations settle differently.
        self.assertEqual(
            got,
            rc.cmac(self.K_AUTH,
                    rc.compat_nonce_block(self.EPOCH, self.DEVNUM,
                                          self.COUNTER,
                                          rc.COMPAT_SUBTYPE_TIER_I))[:5])
        self.assertEqual(len(got) * 8, rc.COMPAT_TIER_I_TAG_BITS)
        self.assertEqual(rc.COMPAT_TIER_I_TAG_BITS, 40)

    def test_tier1_covers_the_counter_the_devnum_and_the_epoch(self):
        # It covers no payload, so these three are the whole of what it says.
        base = rc.compat_tier1_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                   self.COUNTER)
        for label, args in (
                ("counter", (self.EPOCH, self.DEVNUM, self.COUNTER + 1)),
                ("devnum", (self.EPOCH, self.DEVNUM ^ 1, self.COUNTER)),
                ("epoch", (self.EPOCH + 1, self.DEVNUM, self.COUNTER))):
            with self.subTest(field=label):
                self.assertNotEqual(base,
                                    rc.compat_tier1_tag(self.K_AUTH, *args))

    def test_tier1_is_key_bound(self):
        other = bytes.fromhex("ffeeddccbbaa99887766554433221100")
        self.assertNotEqual(
            rc.compat_tier1_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                self.COUNTER),
            rc.compat_tier1_tag(other, self.EPOCH, self.DEVNUM, self.COUNTER))

    def test_tier2_vectors_shared_with_the_c_implementation(self):
        self.assertEqual(
            rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                self.COUNTER, self.MSGS).hex(),
            "b61aa9878f10")
        # A second window size, because an implementation that hard-coded 8
        # would pass everything above.
        self.assertEqual(
            rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM, 0x0007,
                                self.MSGS[:3]).hex(),
            "dc941a16d23e")
        self.assertEqual(rc.COMPAT_TIER_II_TAG_BITS, 48)

    def test_tier2_covers_every_byte_of_every_covered_message(self):
        flipped = list(self.MSGS)
        flipped[-1] = bytes([flipped[-1][0], flipped[-1][1] ^ 0x01]) \
            + flipped[-1][2:]
        self.assertEqual(
            rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                self.COUNTER, flipped).hex(),
            "221f4a94dc76")

        # Byte [0] and byte [7] specifically. The spread tag excludes [7]
        # because that is where it rides; a compat tag rides on a page of its
        # own and covers all eight - and leaving [0] out would let an attacker
        # who flips it reinterpret the same authenticated bits against a
        # different schema.
        base = rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                   self.COUNTER, self.MSGS)
        for index in (0, 7):
            with self.subTest(byte=index):
                edited = list(self.MSGS)
                message = bytearray(edited[3])
                message[index] ^= 0x80
                edited[3] = bytes(message)
                self.assertNotEqual(
                    base,
                    rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                        self.COUNTER, edited))

    def test_tier2_message_order_is_transmission_order(self):
        # A MAC over a set rather than a sequence would let an attacker
        # reorder a window's history and keep it verifiable.
        swapped = list(self.MSGS)
        swapped[0], swapped[-1] = swapped[-1], swapped[0]
        self.assertNotEqual(
            rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                self.COUNTER, self.MSGS),
            rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                self.COUNTER, swapped))

    def test_tier2_window_index_is_inside_the_tag(self):
        self.assertNotEqual(
            rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                self.COUNTER, self.MSGS),
            rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                self.COUNTER + 1, self.MSGS))

    def test_tier2_refuses_an_unenumerated_window(self):
        # N is simultaneously the airtime cost, the verification latency and
        # the DoS amplification factor, so an unenumerated N is three
        # surprises. `messages` is N-1 long, so these counts are the illegal
        # ones.
        for count in (0, 1, 2, 4, 5, 6, 8, 11, 23, 30, 32, 63):
            with self.subTest(messages=count), self.assertRaises(ValueError):
                rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                    self.COUNTER, [bytes(8)] * count)

        # And every legal one is accepted, so the check above cannot pass by
        # refusing everything.
        for window in rc.COMPAT_WINDOW_SIZES:
            with self.subTest(n=window):
                tag = rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                          self.COUNTER,
                                          [bytes(8)] * (window - 1))
                self.assertEqual(len(tag) * 8, rc.COMPAT_TIER_II_TAG_BITS)

    def test_tier2_refuses_a_message_that_is_not_eight_bytes(self):
        for bad in (b"", bytes(7), bytes(9)):
            with self.subTest(length=len(bad)), self.assertRaises(ValueError):
                rc.compat_tier2_tag(self.K_AUTH, self.EPOCH, self.DEVNUM,
                                    self.COUNTER, [bad] * 7)

    def test_the_enumerated_sets_are_what_adr_0008_pins(self):
        # scripts/check_profile_registry.py asserts the same values against
        # this module. Stated here too so a change fails a fast test rather
        # than only the registry checker.
        self.assertEqual(rc.COMPAT_WINDOW_SIZES, (4, 8, 16, 32))
        self.assertEqual(rc.COMPAT_COUNTDOWN_LENGTHS, (16, 32, 64, 128))
        self.assertEqual(rc.COMPAT_DEFAULT_WINDOW, 8)
        self.assertIn(rc.COMPAT_DEFAULT_WINDOW, rc.COMPAT_WINDOW_SIZES)


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


class TestNodeIdentity(unittest.TestCase):
    """ADR 0009: what a node with no host derives from K_dev and a counter.

    These are the vectors radiant_core/tests/src/test_node_ident.c asserts, and
    the two implementations share no code, so agreement here is evidence rather
    than tautology.
    """

    K_DEV = bytes(range(16))
    TIER0_DEVNUM = 51235

    def test_tier0_device_number_is_derived_and_in_range(self):
        self.assertEqual(rc.node_tier0_devnum(self.K_DEV), self.TIER0_DEVNUM)
        # 0 is the ANT wildcard and must never come out of this.
        for byte in range(256):
            key = bytes([byte]) * 16
            devnum = rc.node_tier0_devnum(key)
            self.assertTrue(1 <= devnum <= 0xFFFF)

    def test_pair_scalar_vectors(self):
        self.assertEqual(
            rc.node_pair_scalar(self.K_DEV, 1, self.TIER0_DEVNUM).hex(),
            "7f80f817776ac5264d9f45da009ce2fc"
            "8165d1dce83147783225e05edca6ef10")
        self.assertEqual(
            rc.node_pair_scalar(self.K_DEV, 2, self.TIER0_DEVNUM).hex(),
            "d4bd67bcfeb0441e4785983d30883619"
            "364122a1a942477b8f52a90eac9a0642")
        self.assertEqual(
            rc.node_pair_scalar(self.K_DEV, 7, self.TIER0_DEVNUM).hex(),
            "dc7b9e17f334145f4287aa805a4be02b"
            "49a819493dfa29f1f4085a66d1b5c438")

    def test_the_scalar_is_deterministic_in_the_counter(self):
        # The whole point: the same counter is the same private key, which is
        # why the counter may never repeat.
        first = rc.node_pair_scalar(self.K_DEV, 4, 1)
        self.assertEqual(first, rc.node_pair_scalar(self.K_DEV, 4, 1))
        self.assertNotEqual(first, rc.node_pair_scalar(self.K_DEV, 5, 1))
        self.assertNotEqual(first, rc.node_pair_scalar(b"\xff" * 16, 4, 1))

    def test_the_two_halves_differ(self):
        # A 32-byte output built by calling a 128-bit KDF twice with an
        # unchanged block would be the same 16 bytes repeated, which is the
        # mistake this asserts against.
        scalar = rc.node_pair_scalar(self.K_DEV, 3, 9)
        self.assertEqual(len(scalar), rc.NODE_PAIR_SCALAR_BYTES)
        self.assertNotEqual(scalar[:16], scalar[16:])

    def test_the_length_field_is_bound(self):
        # [L]_2 = 256 here and 128 in kdf_block(), so the first block of the
        # pairing derivation is NOT the 128-bit "pair" key. If it were, a caller
        # that reached for the ordinary KDF would silently get half a scalar
        # that happened to match.
        self.assertNotEqual(
            rc.node_pair_scalar(self.K_DEV, 1, self.TIER0_DEVNUM)[:16],
            rc.kdf(self.K_DEV, rc.LABEL_PAIR, 1, self.TIER0_DEVNUM))

    def test_the_sp800_108_counter_is_rejected_out_of_range(self):
        with self.assertRaises(ValueError):
            rc.node_pair_block(0, 1, 1)
        with self.assertRaises(ValueError):
            rc.node_pair_block(3, 1, 1)


if __name__ == "__main__":
    unittest.main()
