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
