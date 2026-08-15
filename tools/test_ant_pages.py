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

import dataclasses
import math
import sys
import unittest

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402
import radiant_crypto as rc  # noqa: E402


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


class TestHeartRatePages(unittest.TestCase):
    """Device type 0x78. The tightest page namespace here, and the one whose
    byte 0 is not simply a page number."""

    TAIL = dict(event_time=0x1234, beat_count=200, computed_hr=145)

    def test_round_trip_every_page(self):
        cases = [
            (ap.encode_hr_default(**self.TAIL), ap.PAGE_HR_DEFAULT, {}),
            (ap.encode_hr_cumulative_time(0x0A0B0C, **self.TAIL),
             ap.PAGE_HR_CUMULATIVE_TIME, {"operating_time": 0x0A0B0C}),
            (ap.encode_hr_manufacturer(0x2B, 0x4455, **self.TAIL),
             ap.PAGE_HR_MANUFACTURER,
             {"manufacturer_id": 0x2B, "serial_upper16": 0x4455}),
            (ap.encode_hr_product(3, 7, 9, **self.TAIL), ap.PAGE_HR_PRODUCT,
             {"hw_version": 3, "sw_version": 7, "model_number": 9}),
            (ap.encode_hr_previous_beat(0x1200, **self.TAIL),
             ap.PAGE_HR_PREVIOUS_BEAT, {"previous_event_time": 0x1200}),
        ]
        for raw, page, extra in cases:
            with self.subTest(page=page):
                self.assertEqual(len(raw), 8)
                got = ap.decode(raw, ap.HRM_DEVICE_TYPE)
                self.assertEqual(got["page"], page)
                self.assertFalse(got["toggle"])
                self.assertEqual(got["event_time"], 0x1234)
                self.assertEqual(got["beat_count"], 200)
                self.assertEqual(got["computed_hr"], 145)
                for key, value in extra.items():
                    self.assertEqual(got[key], value)

    def test_the_shared_tail_is_the_same_four_bytes_on_every_page(self):
        # A page that moved bytes [4..7] would not be a new page, it would be a
        # broken sensor. This is the assertion that says so.
        pages = [
            ap.encode_hr_default(**self.TAIL),
            ap.encode_hr_cumulative_time(1, **self.TAIL),
            ap.encode_hr_manufacturer(1, 2, **self.TAIL),
            ap.encode_hr_product(1, 2, 3, **self.TAIL),
            ap.encode_hr_previous_beat(1, **self.TAIL),
        ]
        tails = {raw[4:] for raw in pages}
        self.assertEqual(len(tails), 1, "bytes [4..7] differ between pages")

    def test_the_high_bit_of_byte_zero_is_the_toggle_not_the_page(self):
        plain = ap.encode_hr_default(**self.TAIL)
        toggled = ap.encode_hr_default(toggle=True, **self.TAIL)
        self.assertEqual(plain[0], 0x00)
        self.assertEqual(toggled[0], 0x80)
        # Same page, both times. A decoder that read the raw byte would report
        # page 0x80 for half the stream and find no heart rate in it.
        for raw in (plain, toggled):
            self.assertEqual(ap.decode(raw, ap.HRM_DEVICE_TYPE)["page"],
                             ap.PAGE_HR_DEFAULT)
        self.assertTrue(ap.decode(toggled, ap.HRM_DEVICE_TYPE)["toggle"])
        self.assertEqual(plain[1:], toggled[1:])

    def test_a_page_number_that_needs_bit_seven_is_refused(self):
        with self.assertRaises(ValueError):
            ap._hr_page(0x80, bytes(3), 0, 0, 0, False)

    def test_a_missing_reading_is_zero_not_the_u8_sentinel(self):
        raw = ap.encode_hr_default(event_time=0, beat_count=0,
                                   computed_hr=None)
        self.assertEqual(raw[7], 0)
        self.assertIsNone(ap.decode(raw, ap.HRM_DEVICE_TYPE)["computed_hr"])
        # 0xFF is a real 255 bpm here, not "not reported".
        self.assertEqual(
            ap.decode(ap.encode_hr_default(event_time=0, beat_count=0,
                                           computed_hr=255),
                      ap.HRM_DEVICE_TYPE)["computed_hr"], 255)

    def test_bpm_from_the_accumulators_matches_the_computed_byte(self):
        # 145 bpm is one beat every 1024 * 60 / 145 = 423.7 counts.
        before = ap.encode_hr_default(event_time=1000, beat_count=10,
                                      computed_hr=145)
        after = ap.encode_hr_default(event_time=1000 + 4237, beat_count=20,
                                     computed_hr=145)
        a = ap.decode(before, ap.HRM_DEVICE_TYPE)
        b = ap.decode(after, ap.HRM_DEVICE_TYPE)
        bpm = ap.heart_rate_bpm_from(
            ap.delta_u8(b["beat_count"], a["beat_count"]),
            ap.delta_u16(b["event_time"], a["event_time"]))
        self.assertAlmostEqual(bpm, 145.0, delta=0.1)

    def test_the_event_time_wrap_is_taken_in_sixteen_bits(self):
        a = ap.decode(ap.encode_hr_default(event_time=65500, beat_count=250,
                                           computed_hr=60),
                      ap.HRM_DEVICE_TYPE)
        b = ap.decode(ap.encode_hr_default(event_time=100, beat_count=2,
                                           computed_hr=60),
                      ap.HRM_DEVICE_TYPE)
        self.assertEqual(ap.delta_u16(b["event_time"], a["event_time"]), 136)
        self.assertEqual(ap.delta_u8(b["beat_count"], a["beat_count"]), 8)

    def test_a_common_page_on_this_device_type_still_carries_the_toggle(self):
        raw = bytearray(ap.encode_common_80(1, 2, 3))
        raw[0] |= ap.HR_PAGE_TOGGLE
        got = ap.decode(bytes(raw), ap.HRM_DEVICE_TYPE)
        self.assertEqual(got["page"], ap.PAGE_COMMON_MANUFACTURER)
        self.assertTrue(got["toggle"])
        self.assertEqual(got["manufacturer_id"], 2)

    def test_the_permitted_periods_are_a_closed_set(self):
        # A wrong period is the one setting in this whole layer a receiver
        # cannot skip past: the channel does not open at all.
        self.assertEqual(ap.HRM_PERIODS, (8070, 16140, 32280))
        self.assertEqual(ap.HRM_PERIOD, 8070)
        self.assertEqual(ap.HRM_PERIOD_HALF, 2 * ap.HRM_PERIOD)
        self.assertEqual(ap.HRM_PERIOD_QUARTER, 4 * ap.HRM_PERIOD)
        self.assertEqual(ap.BPWR_PERIODS, (8182,))


class TestRunningDynamicsPages(unittest.TestCase):
    """
    The vectors here are hand-placed bit patterns, not encoder output fed back
    to the decoder: a round trip agrees with itself whichever way the two split
    fields were read, and three of this profile's fields are split across a
    byte boundary in a direction that is easy to reverse.
    """

    def test_page_a_bit_placement(self):
        # Cadence 180.25 strides/min = 5768 in 1/32; VO 96.5 mm = 386 quarters;
        # GCT 261 ms; stance 35.75 % = 143 quarters; 100 steps.
        raw = ap.encode_rd_a(cadence_32=5768, vert_osc_quarter_mm=386,
                             gct_ms=261, stance_quarter=143, step_count=100)
        self.assertEqual(raw[0], 0x00)
        self.assertEqual(raw[1], 180)           # integer strides/min
        self.assertEqual(raw[2] & 0x1F, 8)      # 8/32 = 0.25
        self.assertEqual(raw[2] & 0x80, 0)      # bit 7 reserved, set to 0
        # Vertical oscillation: 386 quarters = 96 mm remainder 2.
        self.assertEqual(raw[3], 96)
        self.assertEqual(raw[4] & 0x07, 0)      # 96 needs no high bits
        self.assertEqual((raw[4] >> 3) & 0x03, 2)
        # Ground contact time keeps its THREE LOW bits in byte 4.
        self.assertEqual((raw[4] >> 5) & 0x07, 261 & 0x07)
        self.assertEqual(raw[5], 261 >> 3)
        # Stance 143 quarters = 35 % remainder 3 (binary 11), split low bit
        # first across the byte boundary.
        self.assertEqual(raw[6] & 0x7F, 35)
        self.assertEqual((raw[6] >> 7) & 0x01, 1)   # fraction bit 0
        self.assertEqual(raw[7] & 0x01, 1)          # fraction bit 1
        self.assertEqual((raw[7] >> 1) & 0x7F, 100)

    def test_page_a_round_trip(self):
        raw = ap.encode_rd_a(cadence_32=5768, vert_osc_quarter_mm=386,
                             gct_ms=261, stance_quarter=143, step_count=100,
                             walking=True, bidirectional=True)
        got = ap.decode_rd_a(raw)
        self.assertEqual(got["cadence_32"], 5768)
        self.assertEqual(got["vert_osc_quarter_mm"], 386)
        self.assertEqual(got["gct_ms"], 261)
        self.assertEqual(got["stance_quarter"], 143)
        self.assertEqual(got["step_count"], 100)
        self.assertTrue(got["walking"])
        self.assertTrue(got["bidirectional"])

    def test_page_a_gct_uses_the_full_eleven_bits(self):
        raw = ap.encode_rd_a(cadence_32=0, vert_osc_quarter_mm=0, gct_ms=2047,
                             stance_quarter=0, step_count=0)
        self.assertEqual(ap.decode_rd_a(raw)["gct_ms"], 2047)

    def test_page_a_invalid_is_zero_not_ff(self):
        """Every measurement on this profile spells invalid as ZERO."""
        raw = ap.encode_rd_a(cadence_32=0, vert_osc_quarter_mm=0, gct_ms=0,
                             stance_quarter=0, step_count=0)
        got = ap.decode_rd_a(raw)
        self.assertEqual(got["cadence_32"], ap.RD_INVALID)
        self.assertEqual(got["vert_osc_quarter_mm"], ap.RD_INVALID)
        self.assertEqual(got["stance_quarter"], ap.RD_INVALID)
        self.assertNotIn(ap.INVALID_U8, raw[1:])

    def test_page_b_bit_placement(self):
        # Balance 49.5 % = 1584 in 1/32; vertical ratio 7.75 % = 248; step
        # length 1200 mm.
        raw = ap.encode_rd_b(balance_32=1584, vert_ratio_32=248,
                             step_length_mm=1200, session_leader=0x1234)
        self.assertEqual(raw[0], 0x01)
        self.assertEqual(raw[1] & 0x7F, 49)
        # 1584 = 49*32 + 16, so the 5-bit fraction is 16 = 0b10000: bit 0 is 0
        # and the four high bits are 0b1000.
        self.assertEqual((raw[1] >> 7) & 0x01, 0)
        self.assertEqual(raw[2] & 0x0F, 8)
        self.assertEqual((raw[2] >> 4) & 0x0F, 7 & 0x0F)   # ratio low nibble
        self.assertEqual(raw[3] & 0x07, 7 >> 4)            # ratio high bits
        self.assertEqual((raw[3] >> 3) & 0x1F, 248 % 32)
        self.assertEqual(raw[4], 1200 & 0xFF)
        self.assertEqual(raw[5] & 0x1F, 1200 >> 8)
        # Page 0x01's two reserved bits are 0b11, where page 0x00's one
        # reserved bit is 0.
        self.assertEqual(raw[5] & 0xC0, 0xC0)
        self.assertEqual(raw[5] & 0x20, 0)                 # right side up
        self.assertEqual(raw[6], 0x34)
        self.assertEqual(raw[7], 0x12)

    def test_page_b_round_trip(self):
        raw = ap.encode_rd_b(balance_32=1584, vert_ratio_32=248,
                             step_length_mm=1200, session_leader=0x1234,
                             upside_down=True)
        got = ap.decode_rd_b(raw)
        self.assertEqual(got["balance_32"], 1584)
        self.assertEqual(got["vert_ratio_32"], 248)
        self.assertEqual(got["step_length_mm"], 1200)
        self.assertEqual(got["session_leader"], 0x1234)
        self.assertTrue(got["upside_down"])

    def test_balance_fraction_is_split_low_bit_first(self):
        """The bug this catches is wrong only when the low bit is set."""
        # 32 % + 1/32: fraction 1, so bit 0 is set and the high nibble is 0.
        raw = ap.encode_rd_b(balance_32=32 * 32 + 1, vert_ratio_32=0,
                             step_length_mm=0)
        self.assertEqual((raw[1] >> 7) & 0x01, 1)
        self.assertEqual(raw[2] & 0x0F, 0)
        self.assertEqual(ap.decode_rd_b(raw)["balance_32"], 32 * 32 + 1)

    def test_hr_rd_strap_reports_the_invalid_leader(self):
        raw = ap.encode_rd_b(balance_32=0, vert_ratio_32=0, step_length_mm=0,
                             session_leader=ap.RD_LEADER_HR_RD)
        self.assertEqual(ap.decode_rd_b(raw)["session_leader"], 0xFFFF)

    def test_percentage_reserved_range_is_refused(self):
        with self.assertRaises(ValueError):
            ap.encode_rd_a(cadence_32=0, vert_osc_quarter_mm=0, gct_ms=0,
                           stance_quarter=101 * 4, step_count=0)
        with self.assertRaises(ValueError):
            ap.encode_rd_b(balance_32=101 * 32, vert_ratio_32=0,
                           step_length_mm=0)

    def test_speed_page_and_its_two_sentinels(self):
        raw = ap.encode_rd_speed(3 * 256 + 128)        # 3.5 m/s
        self.assertEqual(ap.decode_rd_speed(raw)["speed_256"], 3 * 256 + 128)
        self.assertEqual(raw[3:], bytes([ap.INVALID_U8] * 5))

        invalid = ap.encode_rd_speed(None)
        self.assertEqual(invalid[1], 0xFF)
        self.assertEqual(invalid[2], 0xFF)
        self.assertIsNone(ap.decode_rd_speed(invalid)["speed_256"])

        # Either sentinel alone invalidates the reading, and they are different
        # values in different widths.
        half = bytearray(ap.encode_rd_speed(3 * 256 + 128))
        half[1] = (half[1] & 0xF0) | ap.RD_SPEED_INVALID_INT
        self.assertIsNone(ap.decode_rd_speed(bytes(half))["speed_256"])

    def test_speed_stops_below_fifteen_metres_per_second(self):
        with self.assertRaises(ValueError):
            ap.encode_rd_speed(15 * 256)

    def test_leader_request_refuses_the_invalid_id(self):
        raw = ap.encode_rd_leader_request(0x2A2B)
        self.assertEqual(ap.decode_rd_leader_request(raw)["leader_id"], 0x2A2B)
        with self.assertRaises(ValueError):
            ap.encode_rd_leader_request(ap.RD_LEADER_NONE)

    def test_open_channel_page(self):
        raw = ap.encode_rd_open_channel(0xAABBCC, rf_freq=61)
        got = ap.decode_rd_open_channel(raw)
        self.assertEqual(got["page"], 0x4A)
        self.assertEqual(got["leader_id_24"], 0xAABBCC)
        self.assertEqual(got["device_type"], ap.RD_DEVICE_TYPE)
        self.assertEqual(got["rf_freq"], 61)
        self.assertEqual(got["period"], ap.RD_PERIOD_HR_RD)
        with self.assertRaises(ValueError):
            ap.encode_rd_open_channel(0, rf_freq=57)   # the ANT+ frequency is
                                                       # not a legal RD channel
        with self.assertRaises(ValueError):
            ap.encode_rd_open_channel(0, rf_freq=61, period=4096)

    def test_rf_enumeration_is_not_sorted(self):
        """Code 3 is 2475 MHz and code 4 is 2461 MHz. Not a transcription slip."""
        self.assertEqual(ap.rd_rf_from_hr_page4(3), 75)
        self.assertEqual(ap.rd_rf_from_hr_page4(4), 61)
        self.assertEqual(ap.rd_rf_from_hr_page4(1), 3)
        self.assertEqual(ap.rd_rf_from_hr_page4(2), 39)

    def test_rf_enumeration_refuses_to_interpret_anything_else(self):
        # 0 is "not open"; 0xFF and everything else is manufacturer-specific
        # data on a strap without running dynamics, and the profile forbids
        # interpreting it. Both answer None so that is the default.
        self.assertIsNone(ap.rd_rf_from_hr_page4(0))
        self.assertIsNone(ap.rd_rf_from_hr_page4(ap.INVALID_U8))
        self.assertIsNone(ap.rd_rf_from_hr_page4(57))

    def test_page_numbers_are_eight_bit_here(self):
        """No page-change toggle on this device type, unlike 0x78."""
        raw = ap.encode_rd_open_channel(1, rf_freq=39)
        self.assertEqual(raw[0], ap.PAGE_RD_OPEN_CHANNEL)
        self.assertEqual(raw[0] & ap.HR_PAGE_TOGGLE, 0)

    def test_periods(self):
        self.assertEqual(ap.RD_PERIOD, 4096)
        self.assertEqual(ap.RD_PERIOD_HR_RD, ap.HRM_PERIOD)
        self.assertEqual(ap.RD_PERIODS, (4096, 8070))


class TestTrackerPages(unittest.TestCase):
    """ANT+ Tracker"""

    def test_location_1_bit_placement(self):
        raw = ap.encode_trk_location_1(asset_index=3, distance_m=1500,
                                       bearing_brad=128,
                                       status=ap.TRK_STATUS_LOW_BATTERY
                                       | ap.TRK_SITUATION_DOG_MOVING,
                                       latitude_semi=0x1A6CFB08)
        self.assertEqual(raw[0], 0x01)
        self.assertEqual(raw[1] & 0x1F, 3)
        self.assertEqual(raw[1] & 0xE0, 0xE0, "reserved top 3 bits are 0x7")
        self.assertEqual(raw[2] | (raw[3] << 8), 1500)
        self.assertEqual(raw[4], 128)
        self.assertEqual(raw[5], ap.TRK_STATUS_LOW_BATTERY | 1)
        # Latitude's LOW 16 bits only.
        self.assertEqual(raw[6] | (raw[7] << 8), 0xFB08)

    def test_location_split_across_two_pages(self):
        """Page 1 alone is a QUARTER of a position report, not half of one."""
        lat = 0x1A6CFB08
        lon = -1424586524 & 0xFFFFFFFF   # 0xAB1688E4, the spec's own example
        p1 = ap.encode_trk_location_1(0, 0, 0, 0, lat)
        p2 = ap.encode_trk_location_2(0, lat, -1424586524)

        loc1 = ap.decode_trk_location_1(p1)
        loc2 = ap.decode_trk_location_2(p2)
        self.assertEqual(ap.trk_combine_latitude(loc1, loc2), lat)
        self.assertEqual(loc2["longitude_semi"], -1424586524)
        self.assertEqual(p2[4] | (p2[5] << 8) | (p2[6] << 16) | (p2[7] << 24),
                         lon)

    def test_semicircle_conversion_round_trips(self):
        # Not pinned to the document's own worked-example decimals: the PDF
        # extraction of those particular figures does not reproduce exactly,
        # and a round trip through the formula is what the wire actually
        # needs to satisfy - encode then decode recovers the angle to the
        # field's own resolution (2^-31 of 180 degrees, well under 1e-6).
        for degrees in (-119.407462, 37.16148287, 0.0, 179.999999, -180.0):
            semi = ap.trk_degrees_to_semicircles(degrees)
            self.assertAlmostEqual(ap.trk_semicircles_to_degrees(semi),
                                   degrees, places=5)

    def test_bradian_conversion_matches_the_spec_example(self):
        # Equation 5's worked example: 60 degrees rounds to 0x2B bradians.
        self.assertEqual(ap.trk_degrees_to_bradians(60.0), 0x2B)
        # The inverse (equation 6) is only accurate to the field's own
        # resolution - 360/256 = 1.40625 degrees per count - which is why
        # 0x2B decodes to 60.46875 deg and not exactly 60.
        self.assertAlmostEqual(ap.trk_bradians_to_degrees(0x2B), 60.0,
                               delta=360.0 / ap.TRK_BRADIANS_PER_360DEG)

    def test_no_assets_page(self):
        raw = ap.encode_trk_no_assets()
        self.assertEqual(raw[0], 0x03)
        self.assertEqual(raw[1:], bytes([ap.INVALID_U8] * 7))

    def test_identity_pages_split_the_name(self):
        name = b"BuddyMax"    # 8 chars: first 5 on page 16, last 3 on page 17
        p1 = ap.encode_trk_ident_1(asset_index=5, colour=0b11100000, name=name)
        p2 = ap.encode_trk_ident_2(asset_index=5,
                                   asset_type=ap.TRK_ASSET_TYPE_DOG, name=name)
        self.assertEqual(p1[0], 0x10)
        self.assertEqual(p2[0], 0x11)
        got1 = ap.decode_trk_ident_1(p1)
        got2 = ap.decode_trk_ident_2(p2)
        self.assertEqual(got1["name_lo"], b"Buddy")
        self.assertEqual(got2["name_hi"], b"Max\x00\x00")
        self.assertEqual(got2["asset_type"], ap.TRK_ASSET_TYPE_DOG)

    def test_asset_index_out_of_range_is_refused(self):
        with self.assertRaises(ValueError):
            ap.encode_trk_location_1(32, 0, 0, 0, 0)

    def test_disconnect_page(self):
        raw = ap.encode_trk_disconnect()
        self.assertEqual(raw[0], 0x20)
        self.assertEqual(raw[1:], bytes([ap.INVALID_U8] * 7))

    def test_period_and_device_type(self):
        self.assertEqual(ap.TRK_DEVICE_TYPE, 0x29)
        self.assertEqual(ap.TRK_PERIOD, 2048)


class TestControlsPages(unittest.TestCase):
    """ANT+ Controls 0x10, against Rev 2.0 tables 9-1, 9-2, 9-8, 9-9."""

    def test_av_command_bit_placement(self):
        raw = ap.encode_ctrl_av(serial_number=0x1234, sequence=7, av=True,
                                command=ap.CTRL_AV_CMD_PLAY,
                                volume_percent=50)
        self.assertEqual(raw[0], 0x10)
        self.assertEqual(raw[1] | (raw[2] << 8), 0x1234)
        self.assertEqual(raw[3], 7)
        self.assertEqual(raw[4], ap.INVALID_U8)
        self.assertEqual(raw[5], ap.INVALID_U8)
        self.assertEqual(raw[6], 50)
        self.assertEqual(raw[7] & 0x80, 0x80, "video flag")
        self.assertEqual(raw[7] & 0x7F, ap.CTRL_AV_CMD_PLAY)

    def test_av_command_round_trip(self):
        raw = ap.encode_ctrl_av(0xFFFF, 0, av=False,
                                command=ap.CTRL_AV_CMD_VOLUME_UP,
                                volume_percent=ap.CTRL_AV_VOLUME_DEFAULT)
        got = ap.decode_ctrl_av(raw)
        self.assertFalse(got["av"])
        self.assertEqual(got["command"], ap.CTRL_AV_CMD_VOLUME_UP)
        self.assertEqual(got["serial_number"], ap.CTRL_SERIAL_UNKNOWN)
        self.assertEqual(got["volume_percent"], ap.CTRL_AV_VOLUME_DEFAULT)

    def test_generic_command_is_sixteen_bits(self):
        """Table 9-9's length column says 1 byte; table 9-8's range (0..65535)
        is what this project trusts, per the module's own note."""
        raw = ap.encode_ctrl_generic(1, 2, sequence=9,
                                     command=ap.CTRL_GENERIC_CUSTOM_MIN)
        self.assertEqual(raw[6] | (raw[7] << 8), ap.CTRL_GENERIC_CUSTOM_MIN)
        got = ap.decode_ctrl_generic(raw)
        self.assertEqual(got["command"], ap.CTRL_GENERIC_CUSTOM_MIN)
        self.assertEqual(got["slave_serial"], 1)
        self.assertEqual(got["slave_manufacturer_id"], 2)
        self.assertEqual(got["sequence"], 9)

    def test_generic_command_reserved_range_is_refused(self):
        with self.assertRaises(ValueError):
            ap.encode_ctrl_generic(0, 0, 0, command=5)      # 5..31 reserved
        with self.assertRaises(ValueError):
            ap.encode_ctrl_generic(0, 0, 0, command=1000)   # 37..32767 reserved

    def test_no_command_is_encodable_and_named(self):
        raw = ap.encode_ctrl_generic(1, 2, sequence=9,
                                     command=ap.CTRL_GENERIC_NO_COMMAND)
        self.assertEqual(ap.decode_ctrl_generic(raw)["command"], 0xFFFF)

    def test_av_command_number_is_seven_bits(self):
        with self.assertRaises(ValueError):
            ap.encode_ctrl_av(0, 0, av=False, command=0x80)

    def test_period_and_device_type(self):
        self.assertEqual(ap.CTRL_DEVICE_TYPE, 0x10)
        self.assertEqual(ap.CTRL_PERIOD, 8192)


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


class TestCommonPage84(unittest.TestCase):
    """Page 84, Subfield Data - decode only, so no round trip to lean on.

    Every other page in this file is tested by encoding and decoding it back,
    which proves the two halves agree and nothing else. There is no
    encode_common_84, so what stands in for it is the primary document's own
    worked example: D00001198 Rev 3.1 section 6.13.1, Figure 6-8. That is a
    stronger test than a round trip, not a weaker one - a round trip through
    two wrong halves passes.
    """

    # ANT Message Payload = [54][FF][01][03][6B][0A][EA][19]
    GOLDEN = bytes.fromhex("54FF01036B0AEA19")

    def test_the_spec_worked_example(self):
        got = ap.decode_common_84(self.GOLDEN)
        self.assertEqual(got["page"], ap.PAGE_COMMON_SUBFIELD)

        first, second = got["slots"]

        # "The first subfield of this data page communicates temperature and
        # the value given in this message is 26.67 C."
        self.assertEqual(first["subpage"], ap.SUBPAGE_TEMPERATURE)
        self.assertEqual(first["name"], "temperature")
        self.assertEqual(first["raw"], 0x0A6B)   # 2667 centi-degC
        self.assertAlmostEqual(first["value"], 26.67)

        # "The second subfield ... communicates percent humidity. The value of
        # the humidity field is 66.34%."
        self.assertEqual(second["subpage"], ap.SUBPAGE_HUMIDITY)
        self.assertEqual(second["name"], "humidity")
        self.assertEqual(second["raw"], 0x19EA)  # 6634 centi-percent
        self.assertAlmostEqual(second["value"], 66.34)

    def test_data_fields_are_little_endian(self):
        # The whole page hangs off this: 6B 0A read big-endian is 27402
        # centi-degC, i.e. 274 C, which is a plausible-looking number for an
        # oven and not for weather.
        got = ap.decode_common_84(self.GOLDEN)
        self.assertEqual(got["slots"][0]["raw"], 0x0A6B)
        self.assertNotEqual(got["slots"][0]["raw"], 0x6B0A)

    def test_invalid_sentinel_on_either_slot(self):
        # Table 6-16: a subpage byte of 0xFF means the slot carries nothing.
        # Declining it is a normal answer, so the OTHER slot still decodes.
        payload = bytes([0x54, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0xEA, 0x19])
        first, second = ap.decode_common_84(payload)["slots"]
        self.assertEqual(first["subpage"], ap.SUBPAGE_INVALID)
        self.assertIsNone(first["name"])
        self.assertIsNone(first["value"])
        self.assertAlmostEqual(second["value"], 66.34)

        payload = bytes([0x54, 0xFF, 0x01, 0xFF, 0x6B, 0x0A, 0x00, 0x00])
        first, second = ap.decode_common_84(payload)["slots"]
        self.assertAlmostEqual(first["value"], 26.67)
        self.assertEqual(second["subpage"], ap.SUBPAGE_INVALID)
        self.assertIsNone(second["value"])

    def test_reserved_subpage_is_declined_not_an_error(self):
        # 9-254 are "Reserved for future use" (Table 6-17). A sensor sending
        # one is behaving; the decoder hands the number back untouched.
        payload = bytes([0x54, 0xFF, 0x09, 0x03, 0x34, 0x12, 0xEA, 0x19])
        first, second = ap.decode_common_84(payload)["slots"]
        self.assertEqual(first["subpage"], 9)
        self.assertIsNone(first["name"])
        self.assertIsNone(first["value"])
        self.assertEqual(first["raw"], 0x1234)
        self.assertAlmostEqual(second["value"], 66.34)

    def test_temperature_subpages_are_twos_complement(self):
        # Subpages 1, 7 and 8 are signed; everything else in Table 6-17 is
        # not. Unsigned here would publish -5.00 C as +650.31 C.
        for subpage in (ap.SUBPAGE_TEMPERATURE, ap.SUBPAGE_TEMP_MIN,
                        ap.SUBPAGE_TEMP_MAX):
            with self.subTest(subpage=subpage):
                payload = bytes([0x54, 0xFF, subpage, 0xFF,
                                 0x0C, 0xFE, 0x00, 0x00])
                got = ap.decode_common_84(payload)["slots"][0]
                self.assertEqual(got["raw"], 0xFE0C)
                self.assertAlmostEqual(got["value"], -5.00)

    def test_subpage_7_and_8_are_named_not_described(self):
        # Table 6-17 transposes the two descriptions: 7 is NAMED "Minimum
        # Operating Temperature" and DESCRIBED as the maximum recorded
        # temperature, 8 the mirror image. The names are self-consistent and
        # are what this decoder follows. Pinned so a future reader who finds
        # the descriptions first cannot quietly swap them back.
        self.assertEqual(ap._SUBPAGE_NAMES[ap.SUBPAGE_TEMP_MIN], "temp_min")
        self.assertEqual(ap._SUBPAGE_NAMES[ap.SUBPAGE_TEMP_MAX], "temp_max")

    def test_unsigned_subpages_use_the_full_u16_range(self):
        # Table 6-17's stated maxima: 655.35 kPa, 655.35 km/h, 65535 cycles.
        for subpage, expect in ((ap.SUBPAGE_PRESSURE, 655.35),
                                (ap.SUBPAGE_WIND_SPEED, 655.35),
                                (ap.SUBPAGE_CHARGE_CYCLES, 65535)):
            with self.subTest(subpage=subpage):
                payload = bytes([0x54, 0xFF, subpage, 0xFF,
                                 0xFF, 0xFF, 0x00, 0x00])
                got = ap.decode_common_84(payload)["slots"][0]
                self.assertAlmostEqual(got["value"], expect)

    def test_wind_direction_scale_is_0_05_degrees(self):
        # Table 6-17's own maximum: 7199 steps = 359.95 degrees. The row that
        # prints "Wind Direction LSB" twice, where the second is plainly the
        # MSB - read as an ordinary little-endian u16, like every other
        # subpage in the table.
        payload = bytes([0x54, 0xFF, ap.SUBPAGE_WIND_DIRECTION, 0xFF,
                         0x1F, 0x1C, 0x00, 0x00])
        got = ap.decode_common_84(payload)["slots"][0]
        self.assertEqual(got["raw"], 7199)
        self.assertAlmostEqual(got["value"], 359.95)


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
            # Page 84 has no encoder (see decode_common_84), so it enters the
            # dispatcher as the section 6.13.1 payload itself. It is here on a
            # BICYCLE POWER device type on purpose: Table 4-1 keys the common
            # range on transmission type, not device type, so a power meter
            # carrying weather data is the specified case, not an oddity.
            (TestCommonPage84.GOLDEN, ap.PAGE_COMMON_SUBFIELD),
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
        self.assertEqual(ap.PROFILES["heart-rate"]["period"], 8070)
        self.assertEqual(ap.PROFILES["heart-rate"]["device_type"], 0x78)


# ---------------------------------------------------------------------------
# The compat layer: RadiANT pages added to an ANT+ device type
# ---------------------------------------------------------------------------


class TestCompatAllocation(unittest.TestCase):
    def test_two_page_numbers_and_the_nibble_that_makes_them_three_bytes(self):
        # Neither tier's layout has a spare bit anywhere (Tier I: [1..2]
        # counter, [3..7] tag; Tier II: [1] window index, [2..7] tag), so the
        # subtype can only live in byte [0], the page number.
        self.assertEqual(ap.COMPAT_PAGE_BEACON, 0x70)
        self.assertEqual(ap.COMPAT_PAGE_ATTEST_TIER_I, 0x71)
        self.assertEqual(ap.COMPAT_PAGE_ATTEST_TIER_II, 0x72)
        self.assertEqual(
            ap.compat_attest_page(ap.COMPAT_SUBTYPE_TIER_I),
            ap.COMPAT_PAGE_ATTEST_TIER_I)
        self.assertEqual(
            ap.compat_attest_page(ap.COMPAT_SUBTYPE_TIER_II),
            ap.COMPAT_PAGE_ATTEST_TIER_II)

    def test_every_compat_page_is_seven_bit(self):
        # Not a style rule. Heart rate cannot express 0x80 or above at all, and
        # the numbers must be the same in every compat profile so a receiver has
        # one rule.
        for page in (ap.COMPAT_PAGE_BEACON, ap.COMPAT_PAGE_ATTEST_TIER_I,
                     ap.COMPAT_PAGE_ATTEST_TIER_II):
            self.assertLessEqual(page, ap.COMPAT_PAGE_MAX)

    def test_the_announcement_never_takes_a_page_number(self):
        # Subtype 0x03 would be page 0x73 under the same rule; it rides frames 2
        # and 3 of the beacon page's set instead. A third page number was
        # rejected on the 7-bit namespace alone.
        self.assertNotIn(ap.compat_attest_page(ap.COMPAT_SUBTYPE_ANNOUNCE),
                         ap._COMPAT_DECODERS)

    def test_speed_and_cadence_is_not_a_compat_device_type(self):
        self.assertNotIn(ap.BSC_COMBINED_DEVICE_TYPE, ap.COMPAT_DEVICE_TYPES)
        self.assertEqual(set(ap.COMPAT_DEVICE_TYPES),
                         {ap.BPWR_DEVICE_TYPE, ap.HRM_DEVICE_TYPE})

    def test_the_constants_mirror_the_crypto_module(self):
        for name in ("COMPAT_DOM", "COMPAT_SUBTYPE_TIER_I",
                     "COMPAT_SUBTYPE_TIER_II", "COMPAT_SUBTYPE_ANNOUNCE",
                     "COMPAT_TIER_I_TAG_BITS", "COMPAT_TIER_II_TAG_BITS",
                     "COMPAT_WINDOW_SIZES", "COMPAT_COUNTDOWN_LENGTHS"):
            with self.subTest(constant=name):
                self.assertEqual(getattr(ap, name), getattr(rc, name))


class TestCompatBeacon(unittest.TestCase):
    def beacon(self, **kwargs) -> ap.CompatBeacon:
        kwargs.setdefault("key_group_hint", bytes([0xAA, 0xBB, 0xCC]))
        return ap.CompatBeacon(**kwargs)

    def test_round_trip_of_a_default_never_node(self):
        b = self.beacon()
        frames = ap.encode_compat_beacon(b)
        self.assertEqual([len(f) for f in frames], [8, 8])
        self.assertEqual(ap.decode_compat_beacon(frames), b)

    def test_round_trip_of_a_node_that_will_go_private(self):
        b = self.beacon(policy=ap.COMPAT_POLICY_COMMAND,
                        private_available=True, pairing_available=True,
                        pairing_open=True, pending_switch=True, window=32,
                        target_device_type=ap.RADIANT_TLM_DEVICE_TYPE,
                        target_device_number=0x9C41, target_period=8182)
        self.assertEqual(ap.decode_compat_beacon(ap.encode_compat_beacon(b)), b)

    def test_the_frame_head_states_the_whole_set(self):
        b = self.beacon()
        steady = ap.encode_compat_beacon(b)
        self.assertEqual([f[1] for f in steady], [0x01, 0x11])
        # ADR 0008 writes frames 2/3 as 0x23/0x33 under (index << 4) |
        # (count - 1); frames 0/1 must restate the new count while the set
        # is four frames long, becoming 0x03/0x13 for the duration.
        announcing = ap.encode_compat_beacon(
            b, frame_count=ap.COMPAT_BEACON_FRAMES_ANNOUNCING)
        self.assertEqual([f[1] for f in announcing], [0x03, 0x13])
        for frame in steady + announcing:
            self.assertEqual(ap.compat_frame_index(frame)[0] < 2, True)

    def test_there_is_no_epoch_anywhere_in_the_beacon(self):
        # The field a joining receiver would most obviously want, refused: for a
        # hostless node the epoch IS the boot counter, so broadcasting it every
        # 30 s fingerprints the device across sessions.
        epoch = 0x0A0B0C0D
        b = self.beacon(key_group_hint=rc.compat_key_group_hint(
            bytes(range(16)), epoch))
        blob = b"".join(ap.encode_compat_beacon(b))
        for order in ("little", "big"):
            self.assertNotIn(epoch.to_bytes(4, order), blob)

    def test_private_available_and_the_policy_must_agree(self):
        with self.assertRaises(ValueError):
            ap.encode_compat_beacon(self.beacon(private_available=True))
        with self.assertRaises(ValueError):
            ap.encode_compat_beacon(
                self.beacon(policy=ap.COMPAT_POLICY_ALWAYS,
                            private_available=False))

    def test_a_never_node_carries_no_locator(self):
        with self.assertRaises(ValueError):
            ap.encode_compat_beacon(self.beacon(target_device_number=0x1234))

    def test_reserved_bits_are_zero_and_a_receiver_says_so(self):
        frames = ap.encode_compat_beacon(self.beacon())
        self.assertEqual(frames[0][7], 0)
        self.assertEqual(frames[1][7], 0)
        for index, byte, bit in ((0, 3, 0x40), (0, 7, 0x01), (1, 7, 0x01)):
            with self.subTest(frame=index, byte=byte):
                broken = [bytearray(f) for f in frames]
                broken[index][byte] |= bit
                with self.assertRaises(ValueError):
                    ap.decode_compat_beacon([bytes(f) for f in broken])

    def test_one_frame_decodes_without_waiting_for_the_other(self):
        # A frame set arrives one frame per 121 messages, so a receiver that
        # could only report a complete pair would say nothing for a minute.
        frames = ap.encode_compat_beacon(
            self.beacon(policy=ap.COMPAT_POLICY_PHYSICAL,
                        private_available=True,
                        target_device_type=0x60, target_device_number=7,
                        target_period=8182))
        first = ap.decode(frames[0], ap.HRM_DEVICE_TYPE)
        self.assertEqual(first["kind"], "beacon")
        self.assertEqual(first["frame_index"], 0)
        self.assertEqual(first["policy"], ap.COMPAT_POLICY_PHYSICAL)
        self.assertEqual(first["key_group_hint"], bytes([0xAA, 0xBB, 0xCC]))
        second = ap.decode(frames[1], ap.BPWR_DEVICE_TYPE)
        self.assertEqual(second["target_device_type"], 0x60)
        self.assertEqual(second["target_period"], 8182)

    def test_the_window_code_is_the_documented_mapping(self):
        for code, window in enumerate((4, 8, 16, 32)):
            self.assertEqual(ap.compat_window_for_code(code), window)
            self.assertEqual(ap.compat_window_code(window), code)
        with self.assertRaises(ValueError):
            ap.compat_window_code(6)

    def test_v1_refuses_to_announce_opportunistic_attestation(self):
        # The field is specified so that turning it on later is a configuration
        # change rather than a format break. Emitting it now would be a claim
        # this build cannot honour.
        with self.assertRaises(ValueError):
            ap.encode_compat_beacon(
                self.beacon(mode=ap.COMPAT_MODE_OPPORTUNISTIC))


class TestCompatAnnounce(unittest.TestCase):
    KEY = bytes(range(16))

    def frame_a(self, **kwargs) -> bytes:
        kwargs.setdefault("target_device_type", ap.RADIANT_TLM_DEVICE_TYPE)
        kwargs.setdefault("target_device_number", 0x9C41)
        kwargs.setdefault("target_period", 8182)
        return ap.encode_compat_announce_a(ap.CompatAnnounce(**kwargs))

    def test_round_trip(self):
        raw = self.frame_a(reason=ap.COMPAT_REASON_PHYSICAL, countdown=8)
        self.assertEqual(raw[0], ap.COMPAT_PAGE_BEACON)
        self.assertEqual(raw[1], 0x23)
        got = ap.decode(raw, ap.HRM_DEVICE_TYPE)
        self.assertEqual(got["kind"], "announce")
        self.assertEqual(got["announce"].target_device_number, 0x9C41)
        self.assertEqual(got["announce"].reason, ap.COMPAT_REASON_PHYSICAL)
        self.assertEqual(got["announce"].countdown, 8)

    def test_the_countdown_counts_promoted_intervals_not_messages(self):
        # Six bits of messages would top out at 63 and the longest legal
        # countdown is K = 128. Six bits of eight-message intervals reaches 504.
        raw = self.frame_a(countdown=ap.COMPAT_COUNTDOWN_MAX)
        got = ap.decode(raw, ap.BPWR_DEVICE_TYPE)
        self.assertEqual(got["countdown_messages"], 63 * 8)
        longest = max(ap.COMPAT_COUNTDOWN_LENGTHS)
        self.assertGreaterEqual(got["countdown_messages"], longest)
        self.assertEqual(ap.COMPAT_BEACON_PROMOTED_INTERVAL, 8)

    def test_frame_b_tags_frame_a_and_a_replay_fails_on_the_counter(self):
        frame_a = self.frame_a(countdown=4)
        tag = rc.compat_announce_tag(self.KEY, 42, 0x3A17, 7, frame_a)
        frame_b = ap.encode_compat_announce_b(tag)
        self.assertEqual(frame_b[1], 0x33)
        self.assertEqual(ap.decode(frame_b, ap.HRM_DEVICE_TYPE)["tag"], tag)
        # The same frame A under a later attestation counter is a different tag,
        # which is what makes a captured announcement useless afterwards.
        self.assertNotEqual(
            rc.compat_announce_tag(self.KEY, 42, 0x3A17, 8, frame_a), tag)
        # And a flipped countdown byte does not verify against the old tag.
        forged = bytearray(frame_a)
        forged[7] ^= 0x01
        self.assertNotEqual(
            rc.compat_announce_tag(self.KEY, 42, 0x3A17, 7, bytes(forged)), tag)

    def test_the_announcement_subtype_is_neither_tier(self):
        frame_a = self.frame_a()
        announce = rc.compat_announce_tag(self.KEY, 1, 2, 3, frame_a)
        tier2 = rc.compat_tier2_tag(self.KEY, 1, 2, 3, [frame_a] * 3)
        self.assertNotEqual(announce, tier2)
        self.assertNotEqual(announce[:5],
                            rc.compat_tier1_tag(self.KEY, 1, 2, 3))

    def test_a_reserved_reason_and_an_eight_bit_device_type_are_refused(self):
        with self.assertRaises(ValueError):
            self.frame_a(reason=ap.COMPAT_REASON_RESERVED)
        with self.assertRaises(ValueError):
            self.frame_a(target_device_type=0x80)
        with self.assertRaises(ValueError):
            self.frame_a(countdown=64)


class TestCompatAttestation(unittest.TestCase):
    KEY = bytes(range(16))

    def test_tier_one_round_trip(self):
        tag = rc.compat_tier1_tag(self.KEY, 42, 0x3A17, 0x0102)
        raw = ap.encode_compat_attest_tier1(0x0102, tag)
        self.assertEqual(len(raw), 8)
        self.assertEqual(raw[0], ap.COMPAT_PAGE_ATTEST_TIER_I)
        got = ap.decode(raw, ap.HRM_DEVICE_TYPE)
        self.assertEqual(got["kind"], "attestation")
        self.assertEqual(got["subtype"], ap.COMPAT_SUBTYPE_TIER_I)
        self.assertEqual(got["att_counter"], 0x0102)
        self.assertEqual(got["tag"], tag)

    def test_tier_two_round_trip(self):
        window = [bytes([i] * 8) for i in range(7)]
        tag = rc.compat_tier2_tag(self.KEY, 42, 0x3A17, 5, window)
        raw = ap.encode_compat_attest_tier2(5, tag)
        self.assertEqual(raw[0], ap.COMPAT_PAGE_ATTEST_TIER_II)
        got = ap.decode(raw, ap.BPWR_DEVICE_TYPE)
        self.assertEqual(got["subtype"], ap.COMPAT_SUBTYPE_TIER_II)
        self.assertEqual(got["window_index"], 5)
        self.assertEqual(got["tag"], tag)

    def test_the_page_byte_carries_the_same_nibble_as_the_nonce(self):
        # The subtype is inside the MAC'd block at position 9; the page byte is
        # derived from it rather than being a second, independent statement of
        # it. That is what stops a Tier I and a Tier II tag over the same
        # counter from being separated only by a byte an attacker chooses.
        tier1 = ap.encode_compat_attest_tier1(
            9, rc.compat_tier1_tag(self.KEY, 1, 2, 9))
        tier2 = ap.encode_compat_attest_tier2(
            9, rc.compat_tier2_tag(self.KEY, 1, 2, 9, [bytes(8)] * 3))
        self.assertEqual(tier1[0] & 0x0F, ap.COMPAT_SUBTYPE_TIER_I)
        self.assertEqual(tier2[0] & 0x0F, ap.COMPAT_SUBTYPE_TIER_II)
        self.assertEqual(rc.compat_nonce_block(1, 2, 9, tier1[0] & 0x0F)[9],
                         ap.COMPAT_SUBTYPE_TIER_I)
        self.assertNotEqual(rc.compat_tier1_tag(self.KEY, 1, 2, 9),
                            rc.compat_tier2_tag(self.KEY, 1, 2, 9,
                                                [bytes(8)] * 3)[:5])

    def test_a_tag_of_the_wrong_length_is_refused(self):
        with self.assertRaises(ValueError):
            ap.encode_compat_attest_tier1(0, bytes(6))
        with self.assertRaises(ValueError):
            ap.encode_compat_attest_tier2(0, bytes(5))

    def test_the_toggle_bit_rides_a_compat_page_too(self):
        # On heart rate every page carries it, including the ones this project
        # added. A decoder that forgot would see page 0xF1 and report nothing.
        tag = rc.compat_tier1_tag(self.KEY, 1, 2, 3)
        raw = ap.encode_compat_attest_tier1(3, tag, toggle=True)
        self.assertEqual(raw[0], 0xF1)
        got = ap.decode(raw, ap.HRM_DEVICE_TYPE)
        self.assertEqual(got["page"], ap.COMPAT_PAGE_ATTEST_TIER_I)
        self.assertTrue(got["toggle"])
        self.assertEqual(got["tag"], tag)

    def test_a_compat_page_number_is_not_claimed_on_other_device_types(self):
        # 0x70 sits inside device type 0x60's reserved range. Decoding it as a
        # beacon there would be the "device type is a proof rather than a hint"
        # mistake the registry warns about.
        raw = ap.encode_compat_beacon(
            ap.CompatBeacon(key_group_hint=bytes(3)))[0]
        got = ap.decode(raw, ap.RADIANT_TLM_DEVICE_TYPE)
        self.assertEqual(got["page"], ap.COMPAT_PAGE_BEACON)
        self.assertIn("raw", got)
        self.assertNotIn("kind", got)


def sample_descriptor(**kwargs) -> ap.TlmDescriptor:
    """The node the C suite uses too, field for field.

    Four fields over two data pages covering every packing case: byte-aligned
    and not, signed and unsigned, accumulating and instantaneous. Field 4 (12
    bits at bit 32) is the one where a little-endian packer would silently
    produce a plausible wrong number rather than an error.
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

    These vectors are shared, byte for byte, with
    radiant/tests/src/test_profiles.c - written as literals rather than
    round trips so two implementations aren't just checked against each
    other.
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


class TestTelemetryScheduleBlock(unittest.TestCase):
    """Frame 1 byte [3] bits 3..0, plus the schedule frame - section 6.

    Frames pinned as hex, shared byte for byte with
    radiant/tests/src/test_schedule.c: a receiver sizes its receive
    window from the clock accuracy here, so a self-consistently wrong decoder
    would lose packets with no error anywhere.
    """

    # 500 us of listening every 2 s - the plan's example duty - announced by a
    # node with a 32 kHz crystal, at +4 dBm, on the 1 M PHY. The phase is not
    # round on purpose: a phase that happened to be a whole number of anything
    # would hide a packer that had quietly dropped its low bits.
    CANON_SCHED = ap.TlmSchedule(dl_interval=64, dl_phase=12000, dl_dwell=1,
                                 tx_power_dbm=4)
    CANON_SCHED_HEX = "20805dc01a00"

    FIELD = ap.TlmField(id=0x01, type=0x10, page=0x01, bit_offset=0,
                        width_code=7, signed=True, exponent=-2)

    CANON = ap.TlmDescriptor(schema_id=0x2B, period=8182, fields=[FIELD],
                             clock_stated=True,
                             clock_accuracy=ap.TLM_CLK_30PPM,
                             schedule=CANON_SCHED)
    CANON_HEX = ["0003112bf61f3900", "0013000e00000000",
                 "002320805dc01a00", "003301105cfe0100"]

    # The same node with nothing to announce: three frames, and byte for byte
    # what this encoder emitted before the block existed.
    QUIET = ap.TlmDescriptor(schema_id=0x2B, period=8182, fields=[FIELD])
    QUIET_HEX = ["0002112bf61f3900", "0012000000000000", "002201105cfe0100"]

    def test_the_block_is_byte_for_byte_what_the_c_encoder_emits(self):
        body = ap.encode_tlm_schedule(self.CANON_SCHED, False, 30)
        self.assertEqual(body.hex(), self.CANON_SCHED_HEX)

    def test_the_descriptor_set_is_byte_for_byte_what_the_c_encoder_emits(self):
        frames = ap.encode_tlm_descriptor(self.CANON)
        self.assertEqual([f.hex() for f in frames], self.CANON_HEX)
        # The schedule frame is index 2 - between the headers and the fields,
        # so the authentication frame keeps the last slot section 6 promises it.
        self.assertEqual(frames[2][1] >> 4, 2)

    def test_a_node_that_announces_nothing_is_the_node_it_was_before(self):
        """The idle-cost A/B: a node that fills nothing must emit the exact
        frames it did before this feature existed."""
        frames = ap.encode_tlm_descriptor(self.QUIET)
        self.assertEqual([f.hex() for f in frames], self.QUIET_HEX)
        self.assertEqual(frames[1][3] & 0x0F, 0)
        self.assertEqual(self.QUIET.frame_count, 3)
        self.assertEqual(self.QUIET.clock_ppm, 0)

        # Announcing only a clock accuracy costs a nibble and NOT a frame,
        # which is why it lives in frame 1: a sparse asset tag on an RC
        # oscillator announces it without lengthening its descriptor at all.
        tagged = dataclasses.replace(self.QUIET, clock_stated=True,
                                     clock_accuracy=ap.TLM_CLK_UNKNOWN)
        got = ap.encode_tlm_descriptor(tagged)
        self.assertEqual(len(got), len(frames))
        self.assertEqual(tagged.clock_ppm, 500)
        for index, (a, b) in enumerate(zip(got, frames, strict=True)):
            if index == 1:
                a = bytes(a[:3]) + bytes([a[3] & 0xF0]) + bytes(a[4:])
            self.assertEqual(a, b)

    def test_an_all_defaulted_block_is_forty_eight_zero_bits(self):
        # Which is what makes the gate a statement about bytes rather than
        # about intentions - and why the TX power is biased on the wire, since
        # 0 dBm is a real power and cannot double as silence.
        body = ap.encode_tlm_schedule(ap.TlmSchedule())
        self.assertEqual(body, bytes(6))
        got = ap.decode_tlm_schedule(body)
        self.assertEqual(got, ap.TlmSchedule())
        self.assertEqual(got.tx_power_dbm, ap.TLM_TX_POWER_UNSTATED)

    def test_round_trip(self):
        got = ap.decode_tlm_descriptor(ap.encode_tlm_descriptor(self.CANON))
        self.assertEqual(got, self.CANON)
        self.assertEqual(got.clock_ppm, 30)
        got = ap.decode_tlm_descriptor(ap.encode_tlm_descriptor(self.QUIET))
        self.assertEqual(got, self.QUIET)

    def test_the_ladder_is_the_handoff_pages_and_not_a_second_one(self):
        for code, ppm in ap.TLM_CLK_PPM_CEILING.items():
            with self.subTest(code=code):
                nibble = ap.tlm_sched_clk_nibble(code, True)
                self.assertEqual(nibble, ap.TLM_SCHED_CLK_STATED | code)
                self.assertEqual(ap.tlm_sched_clk_ppm(nibble), ppm)
                self.assertEqual(ap.tlm_clock_ppm(code), ppm)

        # THE ONE DISTINCTION THIS BLOCK ADDS. The handoff page's code 0 is the
        # worst case, because only a receiver that already knows something
        # sends that page. A descriptor's zeros are what every pre-block node
        # transmits, so with the stated bit clear they are silence.
        self.assertEqual(ap.tlm_clock_ppm(ap.TLM_CLK_UNKNOWN), 500)
        self.assertEqual(ap.tlm_sched_clk_ppm(0x00), 0)
        self.assertEqual(ap.tlm_sched_clk_ppm(ap.TLM_SCHED_CLK_STATED), 500)
        self.assertEqual(ap.tlm_sched_clk_nibble(ap.TLM_CLK_20PPM, False), 0)

    def test_a_ladder_code_with_nothing_stating_it_is_refused(self):
        # Otherwise the caller and the wire disagree about what was announced.
        with self.assertRaises(ValueError):
            ap.encode_tlm_descriptor(
                dataclasses.replace(self.QUIET,
                                    clock_accuracy=ap.TLM_CLK_50PPM))

    def test_the_shape_of_the_set_is_derived_and_not_announced(self):
        # No presence bit was spent: the count byte and the field count already
        # say whether the schedule frame is there, and a bit that could
        # disagree with the arithmetic would let a node describe a set it did
        # not send.
        frames = [bytearray(f) for f in ap.encode_tlm_descriptor(self.CANON)]
        for f in frames:
            f[1] = (f[1] & 0xF0) | 4      # claim five frames
        with self.assertRaises(ValueError):
            ap.decode_tlm_descriptor([bytes(f) for f in frames])

    def test_the_coding_vocabulary_the_long_range_phase_inherits(self):
        # One rate implemented, one defined and refused, the rest reserved.
        # The kbps figures are what lets a consumer budget a window, which is
        # why "yes" alone was never enough in the registry's LR PHY column.
        self.assertEqual(ap.tlm_sched_coding_kbps(ap.TLM_CODING_NONE), 1000)
        self.assertEqual(ap.tlm_sched_coding_kbps(ap.TLM_CODING_S8), 125)
        self.assertEqual(ap.tlm_sched_coding_kbps(ap.TLM_CODING_S2), 500)
        self.assertEqual(ap.tlm_sched_coding_kbps(7), 0)

        lr = dataclasses.replace(self.CANON_SCHED, coding=ap.TLM_CODING_S8)
        # The long-range flag and the coding rate are two statements about one
        # PHY, so they agree or the block is refused, both ways round.
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(lr, False, 30)
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(self.CANON_SCHED, True, 30)
        self.assertEqual(len(ap.encode_tlm_schedule(lr, True, 30)), 6)

        # S=2 is defined so a later second rate is a value and not a format
        # break, and refused so nothing announces a rate no code transmits.
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(
                dataclasses.replace(lr, coding=ap.TLM_CODING_S2), True, 30)

    def test_a_rate_this_build_cannot_use_still_decodes(self):
        # The forward-compatibility rule applied to a vocabulary field: the
        # rate describes the node, not the layout of these bytes, so a receiver
        # decodes it and then declines the channel rather than rejecting the
        # node.
        s = dataclasses.replace(self.CANON_SCHED, coding=ap.TLM_CODING_S8)
        body = bytearray(ap.encode_tlm_schedule(s, True, 30))
        ap.pack_bits(body, ap.TLM_SCHED_AREA_BITS, ap.TLM_SCHED_CODING_OFF,
                     ap.TLM_SCHED_CODING_W, ap.TLM_CODING_S2)
        got = ap.decode_tlm_schedule(bytes(body), True, 30)
        self.assertEqual(got.coding, ap.TLM_CODING_S2)
        self.assertNotIn(got.coding, ap.TLM_CODING_IMPLEMENTED)

    def test_the_dwell_must_cover_the_clock_error_it_announces(self):
        """The two announcements are not independent.

        A 500 ppm node pointing 1.83 s ahead has drifted a millisecond by the
        time its window opens, so the plan's 500 us dwell is a window only a
        crystal node can offer - and a downlink that silently never works is
        worse than one refused at the encoder.
        """
        s = dataclasses.replace(self.CANON_SCHED, dl_phase=60000)
        self.assertEqual(len(ap.encode_tlm_schedule(s, False, 30)), 6)
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(s, False, 500)
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(dataclasses.replace(s, dl_dwell=3),
                                   False, 500)
        self.assertEqual(
            len(ap.encode_tlm_schedule(dataclasses.replace(s, dl_dwell=4),
                                       False, 500)), 6)

    def test_tx_power_is_biased_so_that_zero_can_mean_silence(self):
        for dbm in (0, -40, 20, ap.TLM_TX_POWER_UNSTATED):
            with self.subTest(dbm=dbm):
                s = dataclasses.replace(self.CANON_SCHED, tx_power_dbm=dbm)
                got = ap.decode_tlm_schedule(
                    ap.encode_tlm_schedule(s, False, 30), False, 30)
                self.assertEqual(got.tx_power_dbm, dbm)
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(
                dataclasses.replace(self.CANON_SCHED, tx_power_dbm=40),
                False, 30)

        # Biased byte 228 is 128 once unbiased, which lands on the sentinel if
        # a receiver narrows before it checks.
        body = bytearray(ap.encode_tlm_schedule(self.CANON_SCHED, False, 30))
        ap.pack_bits(body, ap.TLM_SCHED_AREA_BITS, ap.TLM_SCHED_POWER_OFF,
                     ap.TLM_SCHED_POWER_W, 228)
        with self.assertRaises(ValueError):
            ap.decode_tlm_schedule(bytes(body), False, 30)

    def test_the_reservation_is_refused_rather_than_ignored(self):
        body = bytearray(ap.encode_tlm_schedule(self.CANON_SCHED, False, 30))
        body[5] |= 0x01
        with self.assertRaises(ValueError):
            ap.decode_tlm_schedule(bytes(body), False, 30)

    def test_a_window_the_node_does_not_open_carries_no_phase(self):
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(ap.TlmSchedule(dl_phase=1))
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(ap.TlmSchedule(dl_dwell=2))
        # A phase at or past the interval is a window in the next cycle
        # described as one in this one.
        with self.assertRaises(ValueError):
            ap.encode_tlm_schedule(
                ap.TlmSchedule(dl_interval=32, dl_phase=32768, dl_dwell=1))

    def test_the_listen_window_is_relative_to_the_frame_that_carried_it(self):
        # Two receivers share no time base, so nothing absolute may cross the
        # air. The same rule the sync-handoff page's slot phase follows, and
        # the same arithmetic profile_sched_listen_at() does in C.
        s = self.CANON_SCHED
        for t_carrier in (0, 1000000, 2 ** 31):
            with self.subTest(t_carrier=t_carrier):
                self.assertEqual(s.listen_at(t_carrier, 0),
                                 t_carrier + 366210)
                self.assertEqual(s.listen_at(t_carrier, 1),
                                 t_carrier + 366210 + 2000000)
        self.assertIsNone(ap.TlmSchedule().listen_at(1000))


class TestTelemetrySyncHandoff(unittest.TestCase):
    """Page 0x12, section 12. The page a RECEIVER sends about a node.

    Frames pinned as hex, shared byte for byte with
    radiant/tests/src/test_handoff.c: this page lets a receiver act on
    parameters it never measured, where a self-consistently wrong decoder
    costs the most.
    """

    # Next slot 5000 counts after frame 1's t_sync, at the 4 Hz ANT+ period.
    CANON = ap.TlmHandoff(device_number=0xB1CE, device_type=0x60,
                          trans_type=0x05, period=8182, phase=5006,
                          clock_accuracy=ap.TLM_CLK_30PPM, counter=0x2A)
    CANON_HEX = ["122a01b1cec00a00", "122a111ff69c7600"]

    def test_the_frames_are_byte_for_byte_what_the_c_encoder_emits(self):
        frames = ap.encode_tlm_handoff(self.CANON)
        self.assertEqual([f.hex() for f in frames], self.CANON_HEX)

    def test_page_number_counter_and_frame_index(self):
        frames = ap.encode_tlm_handoff(self.CANON)
        for index, frame in enumerate(frames):
            self.assertEqual(frame[0], ap.PAGE_TLM_SYNC_HANDOFF)
            # Byte [1] is the counter, NOT the frame index. The index moved to
            # byte [2] so that this page needs no third exception to the
            # section 4 counter invariant.
            self.assertEqual(frame[1], 0x2A)
            self.assertEqual(ap.tlm_handoff_frame_index(frame), (index, 2))

    def test_round_trip(self):
        got = ap.decode_tlm_handoff(ap.encode_tlm_handoff(self.CANON))
        self.assertEqual(got, self.CANON)

    def test_the_two_frames_are_order_insensitive(self):
        # Frame 0 is timeless and frame 1 is self-dating, so a receiver that
        # hears them backwards or a rotation apart has lost nothing but time.
        frames = ap.encode_tlm_handoff(self.CANON)
        self.assertEqual(ap.decode_tlm_handoff(list(reversed(frames))),
                         self.CANON)

    def test_two_frames_of_different_handoffs_are_refused(self):
        a = ap.encode_tlm_handoff(self.CANON)
        b = ap.encode_tlm_handoff(
            dataclasses.replace(self.CANON, counter=0x2B, device_number=1))
        with self.assertRaises(ValueError):
            ap.decode_tlm_handoff([a[0], b[1]])

    def test_an_incomplete_set_is_refused_rather_than_half_applied(self):
        frames = ap.encode_tlm_handoff(self.CANON)
        with self.assertRaises(ValueError):
            ap.decode_tlm_handoff([frames[0]])
        with self.assertRaises(ValueError):
            ap.decode_tlm_handoff([frames[1]])

    def test_every_reserved_field_is_zero(self):
        for frame in ap.encode_tlm_handoff(self.CANON):
            # Byte [7] is tag space, not padding: no RadiANT page may put a
            # field where an authentication tag has to go.
            self.assertEqual(frame[7], 0)
        # Frame 0 bit 31.
        self.assertEqual(
            ap.unpack_bits(ap.encode_tlm_handoff(self.CANON)[0][3:7],
                           ap.TLM_HANDOFF_AREA_BITS, 31, 1), 0)

    def test_a_set_reserved_bit_is_refused_not_ignored(self):
        frames = ap.encode_tlm_handoff(self.CANON)
        broken = bytearray(frames[0])
        broken[6] |= 0x01
        with self.assertRaises(ValueError):
            ap.decode_tlm_handoff([bytes(broken), frames[1]])
        tagged = bytearray(frames[1])
        tagged[7] = 0xFF
        with self.assertRaises(ValueError):
            ap.decode_tlm_handoff([frames[0], bytes(tagged)])

    def test_the_page_has_no_room_for_an_epoch(self):
        """For a hostless node the epoch IS the boot counter, so broadcasting
        it in the clear would fingerprint the device across sessions; all 64
        field bits are already assigned, so there's no space for one to
        quietly occupy."""
        assigned = 16 + 7 + 8 + 1 + 16 + 13 + 3   # who, then when
        self.assertEqual(assigned, 2 * ap.TLM_HANDOFF_AREA_BITS)
        # And the field area genuinely ends where the tag space begins.
        with self.assertRaises(ValueError):
            ap.pack_bits(bytearray(4), ap.TLM_HANDOFF_AREA_BITS, 32, 32, 0)
        # No decoded handoff has anywhere to put one.
        got = ap.decode_tlm_handoff(ap.encode_tlm_handoff(self.CANON))
        self.assertNotIn("epoch",
                         [f.name for f in dataclasses.fields(got)])

    def test_slot_phase_is_relative_to_the_carrying_frame(self):
        # The recipient adds the phase to the t_sync of the frame it heard.
        # Nothing absolute crosses the air, because two receivers share no
        # clock and an absolute instant would be a number from somebody else's.
        h = self.CANON
        for t_carrier in (0, 1, 12345, 2 ** 31):
            with self.subTest(t_carrier=t_carrier):
                self.assertEqual(ap.tlm_handoff_next_slot(h, t_carrier),
                                 t_carrier + 5000)

    def test_phase_resolution_stays_inside_the_guard_window_at_every_period(self):
        # The whole reason phase is a period/8192 FRACTION rather than a count.
        # RADIANT_CHANNEL_GUARD_MAX_US is 400; a round trip must not cost more
        # than that at any period the field can express, or a handed-off
        # channel would open a window the node's slot has already left.
        guard_max_us = 400
        for period in (273, 1024, 8182, 16384, 32768, 65535):
            for phase_counts in (0, 1, period // 3, period - 1):
                with self.subTest(period=period, phase=phase_counts):
                    code = ap.tlm_handoff_phase_encode(phase_counts, period)
                    back = ap.tlm_handoff_phase_counts(code, period)
                    error_us = abs(back - phase_counts) * 1e6 / 32768.0
                    self.assertLess(error_us, guard_max_us)

    def test_a_phase_measured_across_a_slot_boundary_is_reduced_not_refused(self):
        # It is a phase. A caller one period out is not wrong, it is one period
        # out, and refusing would push modular arithmetic to every caller.
        self.assertEqual(ap.tlm_handoff_phase_encode(8182 + 5000, 8182),
                         ap.tlm_handoff_phase_encode(5000, 8182))

    def test_an_asynchronous_node_has_no_slot_to_hand_over(self):
        with self.assertRaises(ValueError):
            ap.encode_tlm_handoff(dataclasses.replace(self.CANON, period=0))
        with self.assertRaises(ValueError):
            ap.tlm_handoff_phase_encode(0, 0)

    def test_the_pairing_bit_is_not_handed_over(self):
        # Device type is 7 bits here because the MSB of the on-air device type
        # field is the pairing bit, which is a property of a search and not of
        # the node.
        with self.assertRaises(ValueError):
            ap.encode_tlm_handoff(dataclasses.replace(self.CANON,
                                                      device_type=0xE0))

    def test_the_clock_accuracy_ladder_is_the_documented_one(self):
        self.assertEqual(
            [ap.tlm_clock_ppm(code) for code in range(8)],
            [500, 250, 150, 100, 75, 50, 30, 20])
        # Code 0 is "not stated", and on no evidence the worst case is the only
        # safe answer.
        self.assertEqual(ap.tlm_clock_ppm(ap.TLM_CLK_UNKNOWN), 500)
        for code in range(8):
            got = ap.decode_tlm_handoff(ap.encode_tlm_handoff(
                dataclasses.replace(self.CANON, clock_accuracy=code)))
            self.assertEqual(got.clock_accuracy, code)

    def test_the_handoff_reproduces_the_parameters_a_sweep_would_have_found(self):
        """The gate, in Python: what crosses the air is what a search result
        carries, minus anything a second receiver could not interpret."""
        got = ap.decode_tlm_handoff(ap.encode_tlm_handoff(self.CANON))
        self.assertEqual(got.device_number, 0xB1CE)
        self.assertEqual(got.device_type, ap.RADIANT_TLM_DEVICE_TYPE)
        self.assertEqual(got.trans_type, ap.RADIANT_TLM_TRANS_TYPE)
        self.assertEqual(got.period, ap.RADIANT_TLM_PERIOD_DEFAULT)
        self.assertEqual(ap.tlm_handoff_next_slot(got, 1_000_000), 1_005_000)


class TestFrequencyMove(unittest.TestCase):
    """Page 0x13, adaptive frequency (RF-7).

    CANON_HEX is shared byte for byte with radiant/tests/src/test_freq.c;
    this page tells a receiver where to point its radio.
    """

    CANON_HEX = "13001a08000000 00".replace(" ", "")

    def test_the_announcement_matches_the_c_suites_canon_bytes(self):
        body = ap.encode_tlm_freq_move(ap.TLM_FREQ_RF_MID, 8)
        self.assertEqual(body.hex(), self.CANON_HEX)

    def test_round_trip(self):
        got = ap.decode_tlm_freq_move(bytes.fromhex(self.CANON_HEX))
        self.assertEqual(got["target"], ap.TLM_FREQ_RF_MID)
        self.assertEqual(got["countdown"], 8)
        # Eight units is K = 64 messages, and the two constants have to keep
        # agreeing or a receiver retunes eight times too early.
        self.assertEqual(got["messages"], ap.TLM_FREQ_K_DEFAULT)

    def test_the_candidate_set_is_bles_advertising_placement(self):
        # The megahertz rather than the indices: 2/26/80 is only meaningful as
        # 2402, 2426 and 2480 MHz, which is where they sit relative to the three
        # non-overlapping Wi-Fi channels. An index typo would still be three
        # plausible small numbers.
        self.assertEqual([2400 + rf for rf in ap.TLM_FREQ_DEFAULTS],
                         [2402, 2426, 2480])
        # And 2457 MHz is what the phase exists to leave.
        self.assertEqual(2400 + ap.TLM_FREQ_RF_HOME, 2457)
        self.assertNotIn(ap.TLM_FREQ_RF_HOME, ap.TLM_FREQ_DEFAULTS)

    def test_a_reserved_byte_that_is_not_zero_is_refused(self):
        # Fail CLOSED, unlike a descriptor information flag: an unknown field
        # here is not something to ignore.
        for i in range(4, 8):
            body = bytearray(bytes.fromhex(self.CANON_HEX))
            body[i] = 1
            with self.assertRaises(ValueError):
                ap.decode_tlm_freq_move(bytes(body))

    def test_a_countdown_of_zero_and_an_index_out_of_range_are_refused(self):
        with self.assertRaises(ValueError):
            ap.encode_tlm_freq_move(ap.TLM_FREQ_RF_MID, 0)
        with self.assertRaises(ValueError):
            ap.encode_tlm_freq_move(ap.RADIANT_TLM_RF_INDEX_MAX + 1, 8)

        body = bytearray(bytes.fromhex(self.CANON_HEX))
        body[3] = 0
        with self.assertRaises(ValueError):
            ap.decode_tlm_freq_move(bytes(body))
        body = bytearray(bytes.fromhex(self.CANON_HEX))
        body[2] = ap.RADIANT_TLM_RF_INDEX_MAX + 1
        with self.assertRaises(ValueError):
            ap.decode_tlm_freq_move(bytes(body))

    def test_the_countdown_terminates_rather_than_walking_away(self):
        """The regression the C suite pins, in the mirror: a re-anchor applied
        one announcement too many pushes the target past the move itself
        forever, reading on a bench as a silent "adaptive frequency does not
        work"."""
        move_at = ap.TLM_FREQ_K_DEFAULT
        msgs = 0
        moved_at = None
        while msgs < 4 * ap.TLM_FREQ_K_DEFAULT:
            after = msgs + 1
            # The last slot before the move is not announced - see
            # profile_freq.c's claim().
            if msgs % ap.TLM_FREQ_ANNOUNCE_EVERY == 0 and move_at > after:
                _, move_at = ap.tlm_freq_countdown(move_at, after)
            msgs = after
            if msgs >= move_at:
                moved_at = msgs
                break

        self.assertIsNotNone(moved_at, "the node never moved")
        self.assertGreaterEqual(moved_at, ap.TLM_FREQ_K_DEFAULT)
        self.assertLessEqual(moved_at,
                             ap.TLM_FREQ_K_DEFAULT + ap.TLM_FREQ_UNIT)

    def test_a_receiver_holding_an_older_copy_retunes_early_never_late(self):
        # Early costs the tail of the old channel; late costs the head of the
        # new one, and the head is where the descriptor is.
        move_at = ap.TLM_FREQ_K_DEFAULT
        heard = []
        msgs = 0
        while msgs < ap.TLM_FREQ_K_DEFAULT:
            after = msgs + 1
            if msgs % ap.TLM_FREQ_ANNOUNCE_EVERY == 0 and move_at > after:
                c, move_at = ap.tlm_freq_countdown(move_at, after)
                heard.append(after + c * ap.TLM_FREQ_UNIT)
            msgs = after

        for computed in heard:
            self.assertLessEqual(computed, move_at)
            self.assertGreaterEqual(computed,
                                    move_at - (ap.TLM_FREQ_UNIT - 1))

    def test_selection_needs_evidence_for_where_we_already_are(self):
        # A quiet candidate is not evidence that here is loud.
        self.assertIsNone(ap.tlm_freq_select(
            ap.TLM_FREQ_RF_HOME, {ap.TLM_FREQ_RF_MID: (-100, -104)}))

    def test_selection_picks_the_quietest_candidate_past_the_margin(self):
        ev = {
            ap.TLM_FREQ_RF_HOME: (-70, -100),
            ap.TLM_FREQ_RF_LOW: (-84, -102),
            ap.TLM_FREQ_RF_MID: (-96, -104),
            ap.TLM_FREQ_RF_HIGH: (-88, -103),
        }
        self.assertEqual(ap.tlm_freq_select(ap.TLM_FREQ_RF_HOME, ev),
                         ap.TLM_FREQ_RF_MID)

    def test_a_candidate_inside_the_margin_is_not_worth_moving_to(self):
        ev = {ap.TLM_FREQ_RF_HOME: (-90, -100), ap.TLM_FREQ_RF_MID: (-95, -104)}
        self.assertIsNone(ap.tlm_freq_select(ap.TLM_FREQ_RF_HOME, ev))
        # Exactly the margin qualifies: the rule is "at least this much
        # quieter", and a boundary meaning one thing in the comment and another
        # in the code is why this is a test rather than an inspection.
        ev[ap.TLM_FREQ_RF_MID] = (-90 - ap.TLM_FREQ_MARGIN_DB, -104)
        self.assertEqual(ap.tlm_freq_select(ap.TLM_FREQ_RF_HOME, ev),
                         ap.TLM_FREQ_RF_MID)

    def test_selection_is_deterministic_under_a_tie(self):
        # Two receivers with the same evidence must recommend the same index or
        # a node takes whichever asked last.
        ev = {
            ap.TLM_FREQ_RF_HOME: (-70, -100),
            ap.TLM_FREQ_RF_HIGH: (-96, -104),
            ap.TLM_FREQ_RF_MID: (-96, -104),
        }
        self.assertEqual(ap.tlm_freq_select(ap.TLM_FREQ_RF_HOME, ev),
                         ap.TLM_FREQ_RF_MID)

    def test_the_retry_list_is_bounded_and_ends_at_home(self):
        plain = ap.tlm_freq_reacquire_order()
        self.assertEqual(plain[-1], ap.TLM_FREQ_RF_HOME)
        self.assertEqual(len(plain), len(ap.TLM_FREQ_DEFAULTS) + 1)

        told = ap.tlm_freq_reacquire_order(ap.TLM_FREQ_RF_MID)
        self.assertEqual(told[0], ap.TLM_FREQ_RF_MID)
        self.assertEqual(told[-1], ap.TLM_FREQ_RF_HOME)
        # Deduplicated: an entry listed twice is a window spent twice.
        self.assertEqual(len(told), len(set(told)))
        self.assertLessEqual(len(told), len(ap.TLM_FREQ_DEFAULTS) + 2)


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
