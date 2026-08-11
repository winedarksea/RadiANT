#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Round-trip every ANT+ page encoder through its decoder, on the host.

Everything else in tools/ needs a board, a driver and usually a second board,
which is why none of it runs in CI. This does:

    python -m unittest discover -s tools -p "test_*.py"

The cases that matter are the wrap cases. An accumulator that wraps at 65536 is
correct for tens of minutes and then produces one absurd sample, and on the air
that is indistinguishable from interference - so it gets tested here, where it
is deterministic, rather than waited for on hardware.
"""

from __future__ import annotations

import math
import sys
import unittest

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402


class TestStandardPower(unittest.TestCase):
    def test_round_trip(self):
        raw = ap.encode_power_std(event_count=7, cadence=80, acc_power=12345,
                                  inst_power=100)
        self.assertEqual(len(raw), 8)
        got = ap.decode_power_std(raw)
        self.assertEqual(got["page"], ap.PAGE_POWER_STANDARD)
        self.assertEqual(got["event_count"], 7)
        self.assertEqual(got["cadence"], 80)
        self.assertEqual(got["acc_power"], 12345)
        self.assertEqual(got["inst_power"], 100)
        self.assertIsNone(got["pedal_power"])

    def test_byte_order_is_little_endian(self):
        raw = ap.encode_power_std(0, None, acc_power=0x1234,
                                  inst_power=0xABCD)
        self.assertEqual(raw[4:6], b"\x34\x12")
        self.assertEqual(raw[6:8], b"\xcd\xab")

    def test_invalid_cadence_survives_the_round_trip(self):
        raw = ap.encode_power_std(0, None, 0, 0)
        self.assertEqual(raw[3], ap.INVALID_U8)
        self.assertIsNone(ap.decode_power_std(raw)["cadence"])

    def test_accumulators_wrap_rather_than_overflow(self):
        raw = ap.encode_power_std(event_count=260, cadence=80,
                                  acc_power=65536 + 42, inst_power=100)
        got = ap.decode_power_std(raw)
        self.assertEqual(got["event_count"], 4)
        self.assertEqual(got["acc_power"], 42)

    def test_delta_across_the_wrap(self):
        # 65500 -> 100 is a delta of 136, not -65400. Getting this wrong is
        # write-back item 1's failure mode in miniature.
        self.assertEqual(ap.delta_u16(100, 65500), 136)
        self.assertEqual(ap.delta_u8(3, 253), 6)
        self.assertEqual(ap.delta_u16(0, 0), 0)


class TestTorquePages(unittest.TestCase):
    def test_round_trip_both_pages(self):
        for page in (ap.PAGE_POWER_WHEEL_TORQUE, ap.PAGE_POWER_CRANK_TORQUE):
            with self.subTest(page=page):
                raw = ap.encode_power_torque(page, event_count=9, ticks=200,
                                             cadence=80, acc_period=40000,
                                             acc_torque=50000)
                got = ap.decode_power_torque(raw)
                self.assertEqual(got["page"], page)
                self.assertEqual(got["event_count"], 9)
                self.assertEqual(got["ticks"], 200)
                self.assertEqual(got["cadence"], 80)
                self.assertEqual(got["acc_period"], 40000)
                self.assertEqual(got["acc_torque"], 50000)

    def test_rejects_a_page_that_is_not_a_torque_page(self):
        with self.assertRaises(ValueError):
            ap.encode_power_torque(ap.PAGE_POWER_STANDARD, 0, 0, None, 0, 0)

    def test_all_accumulators_wrap(self):
        raw = ap.encode_power_torque(ap.PAGE_POWER_CRANK_TORQUE,
                                     event_count=256 + 5, ticks=256 + 6,
                                     cadence=80, acc_period=65536 + 7,
                                     acc_torque=65536 + 8)
        got = ap.decode_power_torque(raw)
        self.assertEqual(got["event_count"], 5)
        self.assertEqual(got["ticks"], 6)
        self.assertEqual(got["acc_period"], 7)
        self.assertEqual(got["acc_torque"], 8)

    def test_power_matches_the_torque_that_produced_it(self):
        # One crank revolution at 80 rpm delivering 100 W: 0.75 s of period,
        # 11.94 Nm of torque. Encode it, decode it, and the reconstructed
        # power should come back within rounding of 100 W.
        watts, rpm = 100.0, 80.0
        torque = ap.torque_nm_for(watts, rpm)
        self.assertAlmostEqual(torque, 11.9366, places=3)

        dt_s = 60.0 / rpm
        d_period = round(2048 * dt_s)
        d_torque = round(32 * torque)

        got = ap.power_from_torque(d_torque, d_period)
        self.assertAlmostEqual(got, watts, delta=0.5)

    def test_power_across_an_accumulator_wrap(self):
        # Same revolution, but positioned so both accumulators wrap inside it.
        before_period, before_torque = 65500, 65520
        d_period, d_torque = 1536, 382
        after_period = (before_period + d_period) % ap.U16_WRAP
        after_torque = (before_torque + d_torque) % ap.U16_WRAP
        self.assertLess(after_period, before_period)
        self.assertLess(after_torque, before_torque)

        got = ap.power_from_torque(ap.delta_u16(after_torque, before_torque),
                                   ap.delta_u16(after_period, before_period))
        self.assertAlmostEqual(got, 100.0, delta=1.0)

    def test_no_new_event_is_an_error_not_a_zero(self):
        with self.assertRaises(ZeroDivisionError):
            ap.power_from_torque(0, 0)


class TestTorqueFrequencyPage(unittest.TestCase):
    def test_round_trip(self):
        raw = ap.encode_power_torque_freq(event_count=11,
                                          slope_tenth_nm_hz=100,
                                          time_stamp=30000,
                                          torque_ticks=40000)
        got = ap.decode_power_torque_freq(raw)
        self.assertEqual(got["page"], ap.PAGE_POWER_TORQUE_FREQ)
        self.assertEqual(got["event_count"], 11)
        self.assertEqual(got["slope_tenth_nm_hz"], 100)
        self.assertEqual(got["time_stamp"], 30000)
        self.assertEqual(got["torque_ticks"], 40000)

    def test_this_page_alone_is_big_endian(self):
        raw = ap.encode_power_torque_freq(0, 0x1234, 0x5678, 0x9ABC)
        self.assertEqual(raw[2:4], b"\x12\x34")
        self.assertEqual(raw[4:6], b"\x56\x78")
        self.assertEqual(raw[6:8], b"\x9a\xbc")

    def test_power_matches_the_torque_that_produced_it(self):
        # One revolution at 80 rpm, 100 W, with a 10.0 Nm/Hz slope.
        watts, rpm = 100.0, 80.0
        slope_tenth = 100                       # 10.0 Nm/Hz
        torque = ap.torque_nm_for(watts, rpm)
        elapsed_s = 60.0 / rpm
        d_time = round(2000 * elapsed_s)
        d_ticks = round(torque * (slope_tenth / 10.0) * elapsed_s)

        got = ap.power_from_torque_freq(1, d_time, d_ticks, slope_tenth)
        self.assertAlmostEqual(got, watts, delta=1.0)


class TestCombinedSpeedCadence(unittest.TestCase):
    def test_round_trip(self):
        raw = ap.encode_bsc_combined(cad_event_time=1000, cad_revs=20,
                                     spd_event_time=1100, spd_revs=30)
        self.assertEqual(len(raw), 8)
        got = ap.decode_bsc_combined(raw)
        self.assertEqual(got["cad_event_time"], 1000)
        self.assertEqual(got["cad_revs"], 20)
        self.assertEqual(got["spd_event_time"], 1100)
        self.assertEqual(got["spd_revs"], 30)

    def test_cadence_lives_in_the_first_four_bytes(self):
        # aerosense reads only [4..7] and calls it a speed sensor, so the
        # cadence half is dropped in silence. Pin the layout down here so the
        # replay test that proves that has something to point at.
        raw = ap.encode_bsc_combined(0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD)
        self.assertEqual(raw[0:2], b"\xaa\xaa")
        self.assertEqual(raw[2:4], b"\xbb\xbb")
        self.assertEqual(raw[4:6], b"\xcc\xcc")
        self.assertEqual(raw[6:8], b"\xdd\xdd")

    def test_there_is_no_page_number_byte(self):
        # A cadence event time whose low byte happens to be 0x10 would decode
        # as a standard power page under a byte-0 dispatch. decode() must not
        # do that when told the device type.
        raw = ap.encode_bsc_combined(cad_event_time=0x2010, cad_revs=1,
                                     spd_event_time=0x2010, spd_revs=1)
        self.assertEqual(raw[0], ap.PAGE_POWER_STANDARD)
        got = ap.decode(raw, ap.BSC_COMBINED_DEVICE_TYPE)
        self.assertIsNone(got["page"])
        self.assertEqual(got["cad_event_time"], 0x2010)

    def test_cadence_and_speed_from_deltas(self):
        # 80 rpm is one revolution every 0.75 s, which is 768 counts of 1024.
        self.assertAlmostEqual(ap.cadence_rpm_from(1, 768), 80.0, places=6)
        # Same wheel period with a 2.105 m circumference.
        self.assertAlmostEqual(ap.speed_mps_from(1, 768, 2.105),
                               2.105 / 0.75, places=6)

    def test_across_the_event_time_wrap(self):
        before, after = 65500, (65500 + 768) % ap.U16_WRAP
        self.assertLess(after, before)
        self.assertAlmostEqual(
            ap.cadence_rpm_from(1, ap.delta_u16(after, before)), 80.0,
            places=6)


class TestCommonPages(unittest.TestCase):
    def test_page_80_round_trip(self):
        raw = ap.encode_common_80(hw_revision=1, manufacturer_id=255,
                                  model_number=4660)
        got = ap.decode_common_80(raw)
        self.assertEqual(got["page"], ap.PAGE_COMMON_MANUFACTURER)
        self.assertEqual(got["hw_revision"], 1)
        self.assertEqual(got["manufacturer_id"], 255)
        self.assertEqual(got["model_number"], 4660)
        self.assertEqual(raw[1:3], b"\xff\xff")

    def test_page_81_round_trip(self):
        raw = ap.encode_common_81(sw_revision_main=3, serial_number=0x12345678)
        got = ap.decode_common_81(raw)
        self.assertEqual(got["page"], ap.PAGE_COMMON_PRODUCT)
        self.assertEqual(got["sw_revision_main"], 3)
        self.assertEqual(got["serial_number"], 0x12345678)
        self.assertIsNone(got["sw_revision_supplemental"])

    def test_page_81_serial_is_little_endian(self):
        raw = ap.encode_common_81(0, 0x12345678)
        self.assertEqual(raw[4:8], b"\x78\x56\x34\x12")

    def test_page_82_round_trip(self):
        raw = ap.encode_common_82(fractional_voltage=128, coarse_voltage=3,
                                  status=3, operating_time=0x0102FF)
        got = ap.decode_common_82(raw)
        self.assertEqual(got["page"], ap.PAGE_COMMON_BATTERY)
        self.assertEqual(got["operating_time"], 0x0102FF)
        self.assertEqual(got["coarse_voltage"], 3)
        self.assertEqual(got["status"], 3)
        self.assertAlmostEqual(got["voltage"], 3.5)


class TestDispatch(unittest.TestCase):
    def test_every_page_decodes_through_the_dispatcher(self):
        cases = [
            (ap.encode_power_std(1, 80, 100, 100), ap.PAGE_POWER_STANDARD),
            (ap.encode_power_torque(ap.PAGE_POWER_WHEEL_TORQUE, 1, 1, 80,
                                    100, 100), ap.PAGE_POWER_WHEEL_TORQUE),
            (ap.encode_power_torque(ap.PAGE_POWER_CRANK_TORQUE, 1, 1, 80,
                                    100, 100), ap.PAGE_POWER_CRANK_TORQUE),
            (ap.encode_power_torque_freq(1, 100, 1, 1),
             ap.PAGE_POWER_TORQUE_FREQ),
            (ap.encode_common_80(1, 2, 3), ap.PAGE_COMMON_MANUFACTURER),
            (ap.encode_common_81(1, 2), ap.PAGE_COMMON_PRODUCT),
            (ap.encode_common_82(0, 3, 3), ap.PAGE_COMMON_BATTERY),
        ]
        for raw, page in cases:
            with self.subTest(page=page):
                self.assertEqual(ap.decode(raw, ap.BPWR_DEVICE_TYPE)["page"],
                                 page)

    def test_unknown_page_is_returned_raw_rather_than_guessed_at(self):
        raw = bytes([0x77]) + bytes(7)
        got = ap.decode(raw, ap.BPWR_DEVICE_TYPE)
        self.assertEqual(got["page"], 0x77)
        self.assertEqual(got["raw"], raw)

    def test_short_payload_is_an_error(self):
        with self.assertRaises(ValueError):
            ap.decode(bytes(7), ap.BPWR_DEVICE_TYPE)

    def test_profiles_agree_with_the_page_constants(self):
        # ant_sim.py and ant_verify.py both read PROFILES. If a period drifts
        # between them the symptom is a loss percentage, not an exception, so
        # the table is pinned here instead.
        self.assertEqual(ap.PROFILES["power"]["period"], 8182)
        self.assertEqual(ap.PROFILES["power"]["device_type"], 0x0B)
        self.assertEqual(ap.PROFILES["csc"]["period"], 8086)
        self.assertEqual(ap.PROFILES["csc"]["device_type"], 0x79)
        self.assertEqual(ap.PROFILES["power-torque"]["pages"], (0x11, 0x12))


def sample_descriptor(**kwargs) -> "ap.TlmDescriptor":
    """The node the C suite uses too, field for field.

    Four fields over two data pages, chosen so every packing case the envelope
    allows appears at least once: byte-aligned and not, signed and unsigned,
    accumulating and instantaneous. Field 4 is the interesting one - 12 bits
    starting at bit 32 - because a receiver that packed little-endian gets a
    plausible wrong number out of it rather than an error.
    """
    fields = [
        ap.TlmField(id=1, type=0x26, page=1, bit_offset=0, width_code=4),
        ap.TlmField(id=2, type=0x10, page=1, bit_offset=8, width_code=7,
                    exponent=-2),
        ap.TlmField(id=3, type=0x30, page=2, bit_offset=0, width_code=10,
                    accumulate=True),
        ap.TlmField(id=4, type=0x1C, page=2, bit_offset=32, width_code=6,
                    signed=True),
    ]
    kwargs.setdefault("schema_id", 0x2B)
    kwargs.setdefault("period", 8182)
    kwargs.setdefault("fields", fields)
    return ap.TlmDescriptor(**kwargs)


class TestTelemetryBitPacker(unittest.TestCase):
    """The MSB-first field area of device type 0x60.

    THESE VECTORS ARE SHARED, BYTE FOR BYTE, WITH
    radiant_core/tests/src/test_profiles.c. That is the whole reason they are
    written as a table of literals rather than as round trips: two
    implementations checked only against each other are not checked at all, and
    docs/radiant-telemetry.md section 6 says MSB-first bit order exists
    precisely because little-endian bit order is "a reliable source of two
    implementations that each work alone."
    """

    VECTORS = [
        # (bit offset, width, value, six expected bytes)
        (0, 1, 0x1, "800000000000"),
        (47, 1, 0x1, "000000000001"),
        (8, 8, 0xA5, "00a50000000000"[:12]),
        (4, 12, 0xABC, "0abc0000000000"[:12]),
        (32, 12, 0xFFF, "00000000fff0"),
        (7, 6, 0x2B, "015800000000"),
        (0, 48, 0x0123456789AB, "0123456789ab"),
        (0, 32, 0xDEADBEEF, "deadbeef0000"),
        (3, 10, 0x155, "0aa800000000"),
    ]

    def test_vectors(self):
        for off, width, value, expect in self.VECTORS:
            with self.subTest(off=off, width=width):
                area = bytearray(6)
                ap.pack_bits(area, 48, off, width, value)
                self.assertEqual(bytes(area).hex(), expect)
                self.assertEqual(
                    ap.unpack_bits(bytes(area), 48, off, width), value)

    def test_offset_zero_is_the_most_significant_bit(self):
        # The single fact the whole convention rests on. Stated as its own
        # test so that a change to it fails with the right name.
        area = bytearray(6)
        ap.pack_bits(area, 48, 0, 1, 1)
        self.assertEqual(area[0], 0x80)

    def test_a_field_that_does_not_fit_is_an_error(self):
        area = bytearray(6)
        with self.assertRaises(ValueError):
            ap.pack_bits(area, 48, 41, 8, 0)
        # X_AUTH shrinks the field area to 40 bits, so the same field is legal
        # at one offset and illegal one bit later.
        ap.pack_bits(area, 40, 32, 8, 0)
        with self.assertRaises(ValueError):
            ap.pack_bits(area, 40, 33, 8, 0)

    def test_the_width_table_is_the_documented_one(self):
        self.assertEqual(ap.TLM_WIDTHS,
                         (1, 2, 4, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48))
        for code, bits in enumerate(ap.TLM_WIDTHS):
            self.assertEqual(ap.tlm_width_for_code(code), bits)
            self.assertEqual(ap.tlm_code_for_width(bits), code)
        # 13..15 are reserved and are rejected rather than clamped: a receiver
        # that guesses a width decodes every field after it at a wrong offset.
        with self.assertRaises(ValueError):
            ap.tlm_width_for_code(13)

    def test_sign_extension_is_in_the_fields_own_width(self):
        self.assertEqual(ap.tlm_sign_extend(0xFFF, 12), -1)
        self.assertEqual(ap.tlm_sign_extend(0x800, 12), -2048)
        self.assertEqual(ap.tlm_sign_extend(0x7FF, 12), 2047)
        self.assertEqual(ap.tlm_sign_extend(0xFF, 8), -1)
        self.assertEqual(ap.tlm_sign_extend(1, 1), -1)


class TestTelemetryDescriptor(unittest.TestCase):
    # The exact frames the C encoder produces for sample_descriptor(). Pinned
    # as hex rather than recomputed, so a change on either side of the mirror
    # shows up here as a diff rather than as agreement between two things that
    # moved together.
    GOLDEN = [
        "0005142bf61f3900",
        "0015000000000000",
        "0025012610000100",
        "003502101cfe0108",
        "00450330a8000200",
        "0055041c58000220",
    ]

    def test_the_frames_are_byte_for_byte_what_the_c_encoder_emits(self):
        frames = ap.encode_tlm_descriptor(sample_descriptor())
        self.assertEqual([f.hex() for f in frames], self.GOLDEN)

    def test_frame_index_byte_and_page_number(self):
        frames = ap.encode_tlm_descriptor(sample_descriptor())
        for index, frame in enumerate(frames):
            self.assertEqual(frame[0], ap.PAGE_TLM_DESCRIPTOR)
            # (index << 4) | (count - 1). A frame INDEX, not a counter - one
            # of the two documented exceptions to the counter invariant.
            self.assertEqual(frame[1], (index << 4) | (len(frames) - 1))

    def test_round_trip(self):
        d = sample_descriptor()
        back = ap.decode_tlm_descriptor(ap.encode_tlm_descriptor(d))
        self.assertEqual(back.schema_id, d.schema_id)
        self.assertEqual(back.period, d.period)
        self.assertEqual(back.rf_index, d.rf_index)
        self.assertEqual(back.flags, 0)
        self.assertEqual(back.epoch, 0)
        self.assertEqual(back.fields, d.fields)
        self.assertEqual(back.data_pages, [1, 2])

    def test_every_reserved_field_is_zero(self):
        # docs/radiant-telemetry.md section 11: "Every reservation in this
        # document is populated with zeros in v1." Asserted rather than
        # trusted, because a reserved bit that quietly carries something is a
        # format break the day the phase that owns it arrives.
        frames = ap.encode_tlm_descriptor(sample_descriptor())
        self.assertEqual(frames[0][7] & ap.TLM_FLAG_TRANSFORM_MASK, 0)
        self.assertEqual(frames[0][7] & ap.TLM_FLAG_RSVD_3, 0)
        self.assertEqual(frames[1][2], 0)          # heartbeat, off sparse
        self.assertEqual(frames[1][3], 0)          # W, k, and bits 3..0
        self.assertEqual(bytes(frames[1][4:8]), b"\x00\x00\x00\x00")  # epoch
        for frame in frames[2:]:
            self.assertEqual(frame[4] & 0x03, 0)   # encoding bits 1..0

    def test_a_malformed_schema_is_refused(self):
        cases = {
            "class 0x30 without the accumulate bit":
                dict(index=2, attr="accumulate", value=False),
            "two fields overlapping in one page":
                dict(index=1, attr="bit_offset", value=4),
            "a field past the end of the field area":
                dict(index=2, attr="bit_offset", value=20),
            "two fields sharing an id":
                dict(index=1, attr="id", value=1),
        }
        for name, change in cases.items():
            with self.subTest(name):
                d = sample_descriptor()
                setattr(d.fields[change["index"]], change["attr"],
                        change["value"])
                with self.assertRaises(ValueError):
                    ap.encode_tlm_descriptor(d)

    def test_the_node_may_not_disagree_with_itself_about_its_frequency(self):
        d = sample_descriptor(rf_index=80)
        with self.assertRaises(ValueError):
            ap.encode_tlm_descriptor(d)
        d.flags |= ap.TLM_FLAG_OFF_RF57
        self.assertEqual(len(ap.encode_tlm_descriptor(d)), 6)

    def test_a_sparse_node_without_a_heartbeat_is_refused(self):
        # Without a heartbeat a receiver cannot tell a quiet node from a dead
        # one, and "no data" is the one reading a telemetry system must never
        # produce silently.
        d = sample_descriptor(flags=ap.TLM_FLAG_SPARSE, heartbeat_s=0)
        with self.assertRaises(ValueError):
            ap.encode_tlm_descriptor(d)

    def test_info_bit_3_stays_reserved(self):
        # ADR 0006 keeps the withdrawn X_PRIV bit reserved rather than
        # reclaiming it: revisiting rotation stays a format-compatible change,
        # and announcing a privacy posture in the clear is itself a leak.
        d = sample_descriptor(flags=ap.TLM_FLAG_RSVD_3)
        with self.assertRaises(ValueError):
            ap.encode_tlm_descriptor(d)

    def test_v1_refuses_to_announce_a_transform(self):
        # There is no descriptor authentication frame to go with it, and a
        # transform announced without one gets an attacker wrong readings out
        # of correctly authenticated packets.
        for flag in (ap.TLM_FLAG_X_AUTH, ap.TLM_FLAG_X_CONF):
            with self.subTest(flag=flag):
                d = sample_descriptor(flags=flag, w_code=ap.TLM_W_CODE_4)
                with self.assertRaises(ValueError):
                    ap.encode_tlm_descriptor(d)

    def test_a_receiver_fails_closed_on_a_transform_it_cannot_do(self):
        d = sample_descriptor()
        self.assertTrue(d.may_decode_data)
        d.flags = ap.TLM_FLAG_X_CONF
        self.assertFalse(d.may_decode_data)
        with self.assertRaises(ValueError):
            ap.decode_tlm_data(d, bytes([1, 0, 0, 0, 0, 0, 0, 0]))

    def test_an_unimplemented_version_is_rejected_not_guessed_at(self):
        frames = ap.encode_tlm_descriptor(sample_descriptor())
        broken = list(frames)
        broken[0] = bytes([frames[0][0], frames[0][1], 0x24]) + frames[0][3:]
        with self.assertRaises(ValueError):
            ap.decode_tlm_descriptor(broken)
        # Field count 15 names an extended descriptor - a different frame set,
        # not fifteen fields.
        broken[0] = bytes([frames[0][0], frames[0][1], 0x1F]) + frames[0][3:]
        with self.assertRaises(ValueError):
            ap.decode_tlm_descriptor(broken)


class TestTelemetryDescriptorAssembly(unittest.TestCase):
    def test_the_set_assembles_backwards_and_through_holes(self):
        frames = ap.encode_tlm_descriptor(sample_descriptor())
        rx = ap.TlmDescriptorRx()
        for index in reversed(range(len(frames))):
            done = rx.feed(frames[index])
            self.assertEqual(done, index == 0,
                             "the set completes when the last hole fills")
        self.assertEqual(rx.take().schema_id, 0x2B)

    def test_other_pages_do_not_disturb_an_assembly(self):
        frames = ap.encode_tlm_descriptor(sample_descriptor())
        rx = ap.TlmDescriptorRx()
        rx.feed(frames[0])
        rx.feed(ap.encode_common_80(1, 0xFF, 1))
        rx.feed(bytes([0x01, 0x07, 0, 0, 0, 0, 0, 0]))
        self.assertFalse(rx.complete)

    def test_a_new_schema_id_abandons_the_partial_set(self):
        # What the schema id is for: a receiver notices a change after reading
        # a SINGLE frame instead of the whole set. Anything already assembled
        # describes the previous schema, and decoding this node's pages against
        # it would put every field at the wrong offset.
        old = ap.encode_tlm_descriptor(sample_descriptor())
        changed = sample_descriptor(schema_id=0x2C)
        changed.fields[0].bit_offset = 16
        changed.fields[1].bit_offset = 24
        new = ap.encode_tlm_descriptor(changed)

        rx = ap.TlmDescriptorRx()
        for frame in old[:-1]:
            rx.feed(frame)
        rx.feed(new[0])
        self.assertFalse(rx.complete)
        self.assertEqual(rx.resets, 1)
        for frame in new[1:]:
            rx.feed(frame)
        self.assertEqual(rx.take().fields[0].bit_offset, 16)


class TestTelemetryDataPages(unittest.TestCase):
    def test_round_trip_including_the_unaligned_signed_field(self):
        d = sample_descriptor()
        body = ap.encode_tlm_data(d, 2, 0x12,
                                  {3: 0xFFFFFFF0, 4: -250})
        self.assertEqual(body.hex(), "0212fffffff0f060")
        got = ap.decode_tlm_data(d, body)
        self.assertEqual(got["page"], 2)
        self.assertEqual(got["counter"], 0x12)
        self.assertEqual(got["values"][3], 0xFFFFFFF0)
        self.assertEqual(got["values"][4], -250)
        # Bits the schema does not claim are zero: page 2 uses 0..43, so the
        # low nibble of byte [7] is untouched and stays zero, because a later
        # schema may claim it and a receiver mid-change would read a stale
        # value as a number.
        self.assertEqual(body[7] & 0x0F, 0)

    def test_byte_1_is_the_event_counter_and_wraps(self):
        d = sample_descriptor()
        self.assertEqual(ap.encode_tlm_data(d, 1, 300, {})[1], 44)

    def test_an_accumulating_field_wraps_rather_than_saturating(self):
        # Section 5: the transmitted value is a running total in the field's
        # declared width and it is MEANT to wrap.
        d = sample_descriptor()
        body = ap.encode_tlm_data(d, 2, 0, {3: (1 << 32) + 255})
        self.assertEqual(ap.decode_tlm_data(d, body)["values"][3], 255)

    def test_the_scale_exponent_produces_the_worked_example(self):
        # docs/radiant-telemetry.md section 7: raw 29315 at exp -2 is 293.15 K.
        d = sample_descriptor()
        self.assertAlmostEqual(ap.tlm_value_si(d.fields[1], 29315), 293.15,
                               places=6)


class TestTelemetryPageScheduler(unittest.TestCase):
    def builders(self, d):
        return {
            "data_page": lambda page, counter: ap.encode_tlm_data(
                d, page, counter, {}),
            "common_80": lambda: ap.encode_common_80(1, 0x00FF, 1),
            # serial = 0xFFFFFFFF, the not-supplied sentinel and the whole of
            # the page 81 privacy rule.
            "common_81": lambda: ap.encode_common_81(1, None),
        }

    def test_the_interleave_is_119_120_over_121(self):
        d = sample_descriptor()
        sched = ap.TlmPageScheduler(d, self.builders(d))
        for _ in range(ap.RADIANT_TLM_CYCLE):    # burn the power-up burst
            sched.next()

        data = 0
        for cycle in range(3):
            for slot in range(ap.RADIANT_TLM_CYCLE):
                kind, body = sched.next()
                if slot <= 5:
                    self.assertEqual(kind, ap.SLOT_DESCRIPTOR,
                                     f"cycle {cycle} slot {slot}")
                    self.assertEqual(body[0], ap.PAGE_TLM_DESCRIPTOR)
                    self.assertEqual(body[1], (slot << 4) | 5)
                elif slot == ap.RADIANT_TLM_SLOT_PAGE_80:
                    self.assertEqual(kind, ap.SLOT_COMMON_80)
                elif slot == ap.RADIANT_TLM_SLOT_PAGE_81:
                    self.assertEqual(kind, ap.SLOT_COMMON_81)
                else:
                    self.assertEqual(kind, ap.SLOT_DATA)
                    data += 1
        # 121 - 6 descriptor - 2 common. The descriptor set is CONSECUTIVE:
        # spreading six frames one per cycle would make a mid-stream join take
        # six cycles instead of one.
        self.assertEqual(data, 3 * 113)

    def test_the_rotation_alternates_and_the_counter_counts_only_data(self):
        d = sample_descriptor()
        sched = ap.TlmPageScheduler(d, self.builders(d))
        expect_page, expect_counter, seen = 1, 0, 0
        for _ in range(3 * ap.RADIANT_TLM_CYCLE):
            kind, body = sched.next()
            if kind != ap.SLOT_DATA:
                continue
            self.assertEqual(body[0], expect_page)
            self.assertEqual(body[1], expect_counter)
            expect_page = 2 if expect_page == 1 else 1
            expect_counter = (expect_counter + 1) % 256
            seen += 1
        self.assertEqual(seen, 3 * 113)
        self.assertGreater(seen, 256, "the run must cross the counter's wrap")

    def test_the_client_seam_takes_slots_but_never_the_cadence(self):
        d = sample_descriptor()
        offers = []

        def claim(m):
            offers.append(m)
            if m % 3:
                return None
            return bytes([0xF0, m & 0xFF, 0, 0, 0, 0, 0, 0])

        sched = ap.TlmPageScheduler(d, self.builders(d), client=claim)
        claimed = 0
        for slot in range(ap.RADIANT_TLM_CYCLE):
            kind, body = sched.next()
            if slot <= 5:
                self.assertEqual(kind, ap.SLOT_DESCRIPTOR)
            elif slot in (ap.RADIANT_TLM_SLOT_PAGE_80,
                          ap.RADIANT_TLM_SLOT_PAGE_81):
                self.assertIn(kind, (ap.SLOT_COMMON_80, ap.SLOT_COMMON_81))
            elif kind == ap.SLOT_CLIENT:
                self.assertEqual(body[0], 0xF0)
                claimed += 1
            else:
                self.assertEqual(kind, ap.SLOT_DATA)

        # Offered exactly the 113 data slots and no others: a client that could
        # displace a common page or half a descriptor set would be forking the
        # rotation with extra steps.
        self.assertEqual(len(offers), 113)
        self.assertNotIn(0, offers)
        self.assertNotIn(119, offers)
        self.assertNotIn(120, offers)
        self.assertGreater(claimed, 30)

    def test_a_client_with_no_descriptor_still_gets_the_interleave(self):
        # An ANT+ compatibility device type is the actual first caller, and it
        # has no schema of its own. The interleave has to hold with the
        # descriptor slots simply absent, or that plan has to fork this one.
        offers = []

        def claim(m):
            offers.append(m)
            return bytes([0x10, m & 0xFF, 0, 0, 0, 0, 0, 0])

        sched = ap.TlmPageScheduler(None, {
            "common_80": lambda: ap.encode_common_80(1, 0x00FF, 1),
            "common_81": lambda: ap.encode_common_81(1, None),
        }, client=claim)

        kinds = [sched.next()[0] for _ in range(ap.RADIANT_TLM_CYCLE)]
        self.assertEqual(kinds[119], ap.SLOT_COMMON_80)
        self.assertEqual(kinds[120], ap.SLOT_COMMON_81)
        self.assertEqual(len(offers), 119)
        self.assertTrue(all(k == ap.SLOT_CLIENT
                            for i, k in enumerate(kinds) if i < 119))

    def test_a_sparse_node_is_silent_between_heartbeats(self):
        d = sample_descriptor(flags=ap.TLM_FLAG_SPARSE, heartbeat_s=30,
                              k_code=ap.TLM_K_CODE_3)
        sched = ap.TlmPageScheduler(d, {"data_page": self.builders(d)["data_page"]})
        self.assertEqual(sched.hb_slots, 120)

        kinds = [sched.next()[0] for _ in range(3 * 120)]
        self.assertEqual(kinds.count(ap.SLOT_DESCRIPTOR), 3 * 6,
                         "the whole set rides every heartbeat")
        self.assertEqual(kinds.count(ap.SLOT_IDLE), 3 * 120 - 18,
                         "and the node says nothing at all in between")

    def test_a_sparse_event_is_repeated_k_times_with_distinct_counters(self):
        d = sample_descriptor(flags=ap.TLM_FLAG_SPARSE, heartbeat_s=60,
                              k_code=ap.TLM_K_CODE_3)
        sched = ap.TlmPageScheduler(d, {"data_page": self.builders(d)["data_page"]})
        for _ in range(16):          # clear the power-up heartbeat
            sched.next()

        sched.post_event(1)
        counters = [body[1] for kind, body in
                    (sched.next() for _ in range(10))
                    if kind == ap.SLOT_DATA]
        # k = 3, one per slot, spaced by roughly one channel period - there is
        # no retransmission and a scanning receiver may be mid-dwell elsewhere.
        # The counter is what lets it deduplicate the repeats.
        self.assertEqual(counters, [0, 1, 2])

        # Command 0x06: how a receiver that just joined gets a schema without
        # waiting for the next heartbeat.
        sched.request_descriptor()
        self.assertEqual([sched.next()[0] for _ in range(6)],
                         [ap.SLOT_DESCRIPTOR] * 6)
        self.assertEqual(sched.next()[0], ap.SLOT_IDLE)


class TestTelemetryCommandPages(unittest.TestCase):
    """Layout only. The idempotency rule, the accept window, the tag
    derivation and the backoff on failed verifications all need a key and a
    state machine, and both belong to the phase that turns the command path
    on."""

    def test_command_round_trip(self):
        raw = ap.encode_tlm_command(seq=7, cmd=ap.TLM_CMD_SET_LEVEL,
                                    target=4, arg=0x1234, tag=0xBEEF)
        self.assertEqual(raw[0], ap.PAGE_TLM_COMMAND)
        self.assertEqual(raw[1], 7)          # the counter invariant
        self.assertEqual(raw[6:8], b"\xef\xbe")   # trailing bytes are tag space
        got = ap.decode_tlm_command(raw)
        self.assertEqual(got["cmd"], ap.TLM_CMD_SET_LEVEL)
        self.assertEqual(got["target"], 4)
        self.assertEqual(got["arg"], 0x1234)
        self.assertEqual(got["tag"], 0xBEEF)

    def test_ack_round_trip(self):
        raw = ap.encode_tlm_command_ack(seq=7, result=ap.TLM_RESULT_ALREADY,
                                        cmd=ap.TLM_CMD_SET_BOOL, value=1,
                                        tag=0x0102)
        got = ap.decode_tlm_command_ack(raw)
        self.assertEqual(got["page"], ap.PAGE_TLM_COMMAND_ACK)
        self.assertEqual(got["seq"], 7)
        # "accepted; already executed" is what makes a retried "AC on" safe.
        self.assertEqual(got["result"], ap.TLM_RESULT_ALREADY)
        self.assertEqual(got["value"], 1)


class TestAgainstTheAerosenseDecoders(unittest.TestCase):
    """Reference results the C code in zephyr_aerosense/src/ant must match.

    These are the numbers the replay tests over there are checked against, so
    a change to either side that moves them shows up as a failure here first.
    """

    def test_aerosense_torque_formula_is_this_one(self):
        # ant_power_rx.c computes delta_torque * 2*pi * 64 / delta_period.
        d_torque, d_period = 382, 1536
        theirs = d_torque * (2.0 * math.pi * 64.0) / d_period
        self.assertAlmostEqual(ap.power_from_torque(d_torque, d_period),
                               theirs, places=9)

    def test_pages_11_and_12_are_independent_series(self):
        # Write-back item 1: a sensor that emits both pages advances two
        # unrelated accumulator sets. Interleaving them into one baseline
        # produces a delta between series, which is what this shows.
        wheel = [(1000, 2000), (1500, 2100)]     # (acc_period, acc_torque)
        crank = [(50000, 60000), (50500, 60100)]

        shared_delta = ap.delta_u16(crank[0][1], wheel[0][1])
        correct_delta = ap.delta_u16(wheel[1][1], wheel[0][1])
        self.assertNotEqual(shared_delta, correct_delta)
        self.assertEqual(correct_delta, 100)


if __name__ == "__main__":
    unittest.main()
