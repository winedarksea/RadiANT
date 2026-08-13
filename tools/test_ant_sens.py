#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Check the sensitivity instrument on the host, with no radio at all.

tools/ant_sens.py turns a loss curve into one number in dBm, graded against
curves whose answer is known by construction. The negative cases matter most:
a bottomed-out ladder, a stuck transmitter, or a link already broken at full
power must each be *refused*, not answered with a plausible-looking number.
"""

from __future__ import annotations

import sys
import unittest

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

try:
    import ant_sens
except SystemExit:
    # ant_sens imports ant_sim/ant_verify, which exit (not raise) when pyusb
    # is missing; that would otherwise take the whole test run down.
    ant_sens = None


def curve(points, rssi_at_top: float | None = -35.0, samples: int = 240):
    """A ladder from (tx_power_dbm, loss_pct) pairs, loudest first.

    RSSI tracks transmit power exactly by default; tests of a broken dial
    override it.
    """
    top = max(dbm for dbm, _ in points)
    steps = []
    for dbm, loss in points:
        steps.append({
            "tx_power_dbm": dbm,
            "loss_pct": loss,
            "packets": int(samples * (1.0 - loss / 100.0)),
            "rssi_dbm_mean": (None if rssi_at_top is None
                              else rssi_at_top - (top - dbm)),
            "rssi_samples": int(samples * (1.0 - loss / 100.0)),
        })
    return steps


# A knee at -14 dBm: loss flat at the bench floor, then climbs steeply once
# margin runs out, matching a real packet error rate shape.
KNEE_DBM = -14.0
SYNTHETIC = [
    (8, 0.4), (4, 0.4), (0, 0.5), (-4, 0.6), (-8, 1.0),
    (-12, 3.0), (-16, 11.0), (-20, 45.0), (-40, 100.0),
]


@unittest.skipIf(ant_sens is None, "pyusb is not installed")
class TestKneeInterpolation(unittest.TestCase):
    def test_a_synthetic_knee_is_found_where_it_was_put(self):
        knee = ant_sens.interpolate_knee(curve(SYNTHETIC), 5.0)
        # 3.0% at -12 and 11.0% at -16, so 5% lands a quarter of the way down
        # that 4 dB bracket: -13 dBm.
        self.assertAlmostEqual(knee["tx_power_dbm"], -13.0, places=6)
        self.assertEqual(knee["bracket"], [-12, -16])
        self.assertEqual(knee["bracket_db"], 4)

    def test_the_target_is_a_parameter_and_moves_the_answer(self):
        at_1 = ant_sens.interpolate_knee(curve(SYNTHETIC), 1.0)
        at_10 = ant_sens.interpolate_knee(curve(SYNTHETIC), 10.0)
        self.assertLess(at_10["tx_power_dbm"], at_1["tx_power_dbm"])

    def test_an_exact_hit_needs_no_interpolation(self):
        knee = ant_sens.interpolate_knee(
            curve([(0, 1.0), (-4, 5.0), (-8, 20.0)]), 5.0)
        self.assertAlmostEqual(knee["tx_power_dbm"], -4.0)

    def test_a_ladder_that_bottoms_out_reports_no_answer(self):
        # Extrapolating past the last rung would fabricate a number
        # indistinguishable from a measured one.
        knee = ant_sens.interpolate_knee(
            curve([(8, 0.3), (4, 0.4), (0, 0.3), (-4, 0.5), (-12, 0.4),
                   (-20, 0.6)]), 5.0)
        self.assertIsNone(knee["tx_power_dbm"])
        self.assertIn("below the bottom of the ladder", knee["reason"])
        self.assertIn("28 dB", knee["reason"])

    def test_a_link_already_broken_at_full_power_reports_no_answer(self):
        knee = ant_sens.interpolate_knee(
            curve([(8, 40.0), (4, 60.0), (0, 90.0)]), 5.0)
        self.assertIsNone(knee["tx_power_dbm"])
        self.assertIn("broken before the ladder starts", knee["reason"])

    def test_the_first_crossing_wins_not_the_last(self):
        # Taking the last crossing would report whichever deep, noisy rung
        # happened to bounce below 5%, 20 dB from the truth.
        knee = ant_sens.interpolate_knee(
            curve([(0, 1.0), (-4, 9.0), (-8, 3.0), (-12, 80.0)]), 5.0)
        self.assertEqual(knee["bracket"], [0, -4])

    def test_a_wide_bracket_says_so(self):
        # The coarse ladder's -12 to -20 gap: still the best number available,
        # but must not be quoted to 1 dB without flagging the 8 dB span.
        knee = ant_sens.interpolate_knee(
            curve([(0, 0.4), (-12, 1.0), (-20, 40.0)]), 5.0)
        self.assertIsNotNone(knee["tx_power_dbm"])
        self.assertEqual(knee["bracket_db"], 8)
        self.assertIn("--rungs fine", knee["reason"])

    def test_too_few_rungs_is_not_a_ladder(self):
        self.assertIsNone(
            ant_sens.interpolate_knee(curve([(0, 0.4)]), 5.0)["tx_power_dbm"])

    def test_rungs_out_of_order_are_sorted_not_believed(self):
        shuffled = curve(SYNTHETIC)
        shuffled.reverse()
        self.assertAlmostEqual(
            ant_sens.interpolate_knee(shuffled, 5.0)["tx_power_dbm"], -13.0)


@unittest.skipIf(ant_sens is None, "pyusb is not installed")
class TestTheDialIsChecked(unittest.TestCase):
    """Three ways the transmitter can ignore its commanded power, all of
    which produce a normal-looking loss curve - hence checking RSSI too."""

    def test_a_working_dial_reads_one_db_per_db(self):
        slope = ant_sens.rssi_slope(curve(SYNTHETIC))
        self.assertAlmostEqual(slope["slope"], 1.0, places=6)
        self.assertAlmostEqual(slope["worst_residual_db"], 0.0, places=6)
        ok, detail = ant_sens.slope_verdict(slope)
        self.assertTrue(ok, detail)

    def test_a_transmitter_stuck_at_one_power_is_caught(self):
        # Every rung asks for a different power but all transmit at 0 dBm; the
        # flat loss curve reads as a great receiver, not a stuck transmitter.
        steps = curve([(8, 0.4), (4, 0.4), (0, 0.4), (-4, 0.5), (-12, 0.4)])
        for step in steps:
            step["rssi_dbm_mean"] = -35.0
        ok, detail = ant_sens.slope_verdict(ant_sens.rssi_slope(steps))
        self.assertFalse(ok)
        self.assertIn("not transmitting at the powers it was told", detail)

    def test_tx_power_boost_folding_two_levels_together_is_caught(self):
        # CONFIG_ANT_DONGLE_TX_POWER_BOOST maps levels 3 and 4 both onto
        # +8 dBm. The best-fit slope through that is still ~1.12 (within any
        # sane tolerance), so it's the per-rung residual, not the slope, that
        # catches the three rungs sitting 3-6 dB off the line.
        steps = curve([(8, 0.4), (4, 0.4), (0, 0.4), (-4, 0.6), (-12, 1.2),
                       (-20, 4.0)])
        for step in steps:
            if step["tx_power_dbm"] in (8, 4, 0):
                step["rssi_dbm_mean"] = -35.0
        slope = ant_sens.rssi_slope(steps)
        self.assertLess(abs(slope["slope"] - 1.0), ant_sens.SLOPE_TOLERANCE,
                        "the point of this case is that the slope looks fine")
        self.assertGreater(slope["worst_residual_db"],
                           ant_sens.MAX_RESIDUAL_DB)
        ok, detail = ant_sens.slope_verdict(slope)
        self.assertFalse(ok)
        self.assertIn("CONFIG_ANT_DONGLE_TX_POWER_BOOST", detail)

    def test_a_dongle_that_reports_no_rssi_is_not_silently_trusted(self):
        ok, detail = ant_sens.slope_verdict(
            ant_sens.rssi_slope(curve(SYNTHETIC, rssi_at_top=None)))
        self.assertFalse(ok)
        self.assertIn("never checked", detail)

    def test_rungs_past_the_knee_do_not_flatten_the_slope(self):
        # Survivor bias: at 90% loss the packets that arrive are the ones
        # that faded up, so fitting through those rungs reads as a broken
        # dial on a working bench - hence fitting below the knee only.
        steps = curve(SYNTHETIC)
        for step in steps:
            if step["loss_pct"] > 20.0:
                step["rssi_dbm_mean"] = -88.0    # the noise floor, not the dial
        slope = ant_sens.rssi_slope(steps)
        self.assertAlmostEqual(slope["slope"], 1.0, places=6)
        self.assertTrue(ant_sens.slope_verdict(slope)[0])


@unittest.skipIf(ant_sens is None, "pyusb is not installed")
class TestTheLadderItself(unittest.TestCase):
    def test_the_coarse_ladder_is_the_six_ant_levels_loudest_first(self):
        self.assertEqual(ant_sens.ladder_dbm("levels", None),
                         [8, 4, 0, -4, -12, -20])

    def test_the_fine_ladder_spans_forty_eight_db(self):
        fine = ant_sens.ladder_dbm("fine", "nrf52840")
        self.assertEqual(fine[0], 8)
        self.assertEqual(fine[-1], -40)
        self.assertEqual(fine[0] - fine[-1], 48)
        self.assertEqual(fine, sorted(fine, reverse=True))

    def test_the_ladder_can_be_trimmed_at_both_ends(self):
        self.assertEqual(
            ant_sens.ladder_dbm("levels", None, top=0, bottom=-12),
            [0, -4, -12])

    def test_a_fine_ladder_without_a_known_part_refuses(self):
        with self.assertRaises(ValueError):
            ant_sens.ladder_dbm("fine", None)
        with self.assertRaises(ValueError):
            ant_sens.ladder_dbm("fine", "nrf54l15")

    def test_a_ladder_trimmed_to_one_rung_refuses(self):
        with self.assertRaises(ValueError):
            ant_sens.ladder_dbm("levels", None, top=-20)


@unittest.skipIf(ant_sens is None, "pyusb is not installed")
class TestPowerEncoding(unittest.TestCase):
    """A raw register value guessed for the wrong part is a silent wrong
    answer: nRF52840 encodes TXPOWER as signed dBm, nRF54L15 does not, so the
    same byte is two different powers on either part with no error raised."""

    def test_the_six_named_levels_need_no_part(self):
        for dbm, level in ((-20, 0), (-12, 1), (-4, 2), (0, 3), (4, 4), (8, 5)):
            self.assertEqual(ant_sens.encode_power(dbm), (level, 0))

    def test_a_fine_step_is_the_custom_escape_and_a_register_value(self):
        level, custom = ant_sens.encode_power(-8, "nrf52840")
        self.assertEqual(level, ant_sens.RADIO_TX_POWER_LVL_CUSTOM)
        self.assertEqual(custom, 0xF8)          # -8 in two's complement
        self.assertEqual(ant_sens.encode_power(-40, "nrf52840")[1], 0xD8)
        self.assertEqual(ant_sens.encode_power(7, "nrf52840")[1], 0x07)

    def test_a_fine_step_without_a_part_refuses_rather_than_guessing(self):
        with self.assertRaises(ValueError):
            ant_sens.encode_power(-8)

    def test_an_unknown_part_refuses(self):
        with self.assertRaises(ValueError) as raised:
            ant_sens.encode_power(-8, "nrf54l15")
        self.assertIn("radiant_radio_nrf.c", str(raised.exception))

    def test_a_power_the_part_does_not_have_refuses(self):
        with self.assertRaises(ValueError):
            ant_sens.encode_power(-1, "nrf52840")


@unittest.skipIf(ant_sens is None, "pyusb is not installed")
class TestRepeatability(unittest.TestCase):
    """The Phase 0 gate: two ladders on an unchanged rig must agree within
    1 dB, or the instrument cannot grade the 1 dB claims Phases 2 and 5 rest
    on."""

    def test_agreement_is_the_spread(self):
        self.assertAlmostEqual(ant_sens.agreement_db([-13.0, -13.4]), 0.4)
        self.assertAlmostEqual(
            ant_sens.agreement_db([-13.0, -13.4, -14.9]), 1.9)

    def test_one_ladder_has_nothing_to_agree_with(self):
        self.assertIsNone(ant_sens.agreement_db([-13.0]))

    def test_a_ladder_with_no_knee_does_not_count_as_agreement(self):
        self.assertIsNone(ant_sens.agreement_db([None, None]))
        self.assertIsNone(ant_sens.agreement_db([-13.0, None]))


@unittest.skipIf(ant_sens is None, "pyusb is not installed")
class TestDerivedBlock(unittest.TestCase):
    def test_the_receiver_side_figure_is_derived_not_read_off_the_knee(self):
        # The knee rung's own RSSI is biased (arrivals near the knee skew
        # toward faded-up), so the figure is walked down in dB from a
        # reference rung well above the knee instead: -35 dBm top, 21 dB down
        # to the knee, giving -56 regardless of what the knee rung reports.
        steps = curve(SYNTHETIC)
        for step in steps:
            if step["loss_pct"] > 2.0:
                step["rssi_dbm_mean"] = -50.0    # nonsense, and unused
        derived = ant_sens.derive(steps, 5.0)
        self.assertAlmostEqual(derived["loss5pct_tx_power_dbm"], -13.0)
        self.assertAlmostEqual(derived["loss5pct_rssi_dbm"], -35.0 - 21.0)
        self.assertEqual(derived["rssi_reference"]["tx_power_dbm"], 8)

    def test_no_knee_means_no_receiver_side_figure_either(self):
        derived = ant_sens.derive(
            curve([(8, 0.3), (0, 0.4), (-20, 0.5)]), 5.0)
        self.assertIsNone(derived["loss5pct_tx_power_dbm"])
        self.assertIsNone(derived["loss5pct_rssi_dbm"])

    def test_a_broken_dial_is_carried_into_the_block(self):
        steps = curve(SYNTHETIC)
        for step in steps:
            step["rssi_dbm_mean"] = -35.0
        derived = ant_sens.derive(steps, 5.0)
        # The knee arithmetic still runs and produces a number; only
        # `dial_trustworthy` says it's fiction.
        self.assertIsNotNone(derived["loss5pct_tx_power_dbm"])
        self.assertFalse(derived["dial_trustworthy"])


if __name__ == "__main__":
    unittest.main()
