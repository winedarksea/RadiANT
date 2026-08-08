#!/usr/bin/env python3
"""Run the simulator through the verifier on the host, with no radio at all.

tools/test_ant_pages.py proves a page survives its own round trip. This proves
the two halves of the closed loop agree: ant_sim.py's sensors are driven for a
simulated quarter of an hour and every packet is fed to ant_verify.py's
analyser, which must find no fault. When it later finds one on the bench, that
is a fact about the radio or the firmware rather than about these two scripts.

The negative cases matter as much as the positive one. An instrument that
passes everything is not an instrument, so a stream with packets removed has to
report loss, and one with a corrupted accumulator has to report a continuity
violation.
"""

from __future__ import annotations

import random
import sys
import unittest

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402

try:
    import ant_sim
    import ant_verify
except SystemExit:
    # Both exit rather than raise when pyusb is missing, so that a developer
    # running the tool sees advice instead of a traceback. Here that would take
    # the whole test run down.
    ant_sim = None
    ant_verify = None

# Long enough that the 16-bit accumulators wrap: the crank-torque period wraps
# every ~32 s and the torque-frequency tick count every ~9 minutes, and a run
# that stops short of those has not tested them.
RUN_SECONDS = 900.0
WATTS = 100.0
RPM = 80.0


def build(profile: str, noise: float = 3.0, seed: int = 7):
    rng = random.Random(seed)
    return ant_sim.SENSORS[profile](
        0, 0x3A17, 5,
        ant_sim.Signal(WATTS, noise, rng),
        ant_sim.Signal(RPM, noise, rng))


def analyse(sensor, records) -> dict:
    profile = ant_verify.profile_for(
        sensor.device_type,
        __import__("collections").Counter(
            payload[0] for _, _, _, payload in records))
    analyzer = ant_verify.ChannelAnalyzer(
        profile, sensor.device_type,
        ant_verify.period_for(sensor.device_type, profile),
        WATTS, RPM, 2.105)
    for t, _device_type, _device_number, payload in records:
        analyzer.feed(t, payload)
    return analyzer.summary(ant_verify.DEFAULT_MAX_LOSS_PCT,
                            ant_verify.DEFAULT_JITTER_FACTOR, None)


@unittest.skipIf(ant_sim is None, "pyusb is not installed")
class TestSimulatorPassesVerification(unittest.TestCase):
    def _run(self, profile: str) -> dict:
        sensor = build(profile)
        records = ant_sim.dry_run([sensor], RUN_SECONDS)
        summary = analyse(sensor, records)
        failed = [c for c in summary["checks"] if not c["pass"]]
        self.assertEqual(
            failed, [],
            f"{profile}: " + "; ".join(
                f"{c['name']}: {c['detail']}" for c in failed))
        return summary

    def test_standard_power(self):
        summary = self._run("power")
        # The accumulator has to wrap inside the run or the wrap is untested,
        # and a run that quietly did not reach it would look like a pass.
        self.assertGreater(summary["accumulator_wraps"].get("acc_power", 0), 0)
        self.assertGreater(summary["accumulator_wraps"].get("event_count", 0), 0)

    def test_torque_pages(self):
        summary = self._run("power-torque")
        self.assertEqual(set(summary["pages"]),
                         {"0x11", "0x12", "0x50", "0x51"})
        for field in ("acc_period(0x11)", "acc_period(0x12)",
                      "acc_torque(0x11)", "acc_torque(0x12)"):
            self.assertGreater(summary["accumulator_wraps"].get(field, 0), 0,
                               f"{field} never wrapped in {RUN_SECONDS:.0f} s")

    def test_torque_frequency_page(self):
        summary = self._run("power-torque-freq")
        self.assertGreater(summary["accumulator_wraps"].get("time_stamp", 0), 0)
        self.assertGreater(summary["accumulator_wraps"].get("torque_ticks", 0),
                           0)

    def test_combined_speed_and_cadence(self):
        summary = self._run("csc")
        # This page has no page numbers, so the histogram must be empty rather
        # than full of whatever byte 0 happened to hold.
        self.assertEqual(summary["pages"], {})
        self.assertIsNotNone(summary["cadence"])
        self.assertGreater(summary["cadence"]["n"], 0)

    def test_a_zero_noise_run_is_exact(self):
        sensor = build("power", noise=0.0)
        records = ant_sim.dry_run([sensor], 120.0)
        summary = analyse(sensor, records)
        self.assertLess(summary["power"]["mean_abs_error"], 0.51)
        self.assertLess(summary["cadence"]["mean_abs_error"], 0.51)


@unittest.skipIf(ant_sim is None, "pyusb is not installed")
class TestVerifierCatchesFaults(unittest.TestCase):
    """The instrument has to fail things, or a pass means nothing."""

    def _summary_of(self, records, sensor) -> dict:
        return analyse(sensor, records)

    def test_dropped_packets_are_reported_as_loss(self):
        sensor = build("power")
        records = ant_sim.dry_run([sensor], 120.0)
        thinned = [r for index, r in enumerate(records) if index % 20]
        summary = self._summary_of(thinned, sensor)

        loss = next(c for c in summary["checks"] if c["name"] == "loss")
        self.assertFalse(loss["pass"], loss["detail"])
        self.assertGreater(summary["loss_pct"], 4.0)

    def test_a_corrupted_accumulator_is_reported(self):
        sensor = build("power")
        records = ant_sim.dry_run([sensor], 120.0)

        # Jump the accumulated-power field by 5000 on one packet. Nothing else
        # changes, so only the accumulator-versus-instantaneous cross-check can
        # notice - which is the check that exists for exactly this.
        t, device_type, device_number, payload = records[100]
        bumped = bytearray(payload)
        acc = (ap._rd_le16(payload, 4) + 5000) % ap.U16_WRAP
        bumped[4:6] = acc.to_bytes(2, "little")
        records[100] = (t, device_type, device_number, bytes(bumped))

        summary = self._summary_of(records, sensor)
        continuity = next(c for c in summary["checks"]
                          if c["name"] == "accumulator continuity")
        self.assertFalse(continuity["pass"], continuity["detail"])
        self.assertGreater(summary["violations"]["count"], 0)

    def test_a_stalled_transmitter_is_reported(self):
        sensor = build("power")
        records = ant_sim.dry_run([sensor], 120.0)
        # Every packet repeats the first: the radio is alive, the sensor is
        # not. Loss is zero, jitter is zero, and the decoded power is a
        # perfectly correct 100 W forever - so every check except liveness
        # passes, which is why liveness exists.
        first = records[0]
        frozen = [(t, dt, dn, first[3]) for t, dt, dn, _ in records]
        summary = self._summary_of(frozen, sensor)

        liveness = next(c for c in summary["checks"]
                        if c["name"] == "sensor liveness")
        self.assertFalse(liveness["pass"], liveness["detail"])
        self.assertEqual(summary["event_advances"], 0)
        power = next(c for c in summary["checks"] if c["name"] == "power accuracy")
        self.assertTrue(power["pass"],
                        "the point of this case is that accuracy alone cannot "
                        "see a stalled sensor")

    def test_the_missing_common_pages_are_reported(self):
        sensor = build("power")
        records = ant_sim.dry_run([sensor], 120.0)
        stripped = [r for r in records
                    if r[3][0] not in (ap.PAGE_COMMON_MANUFACTURER,
                                       ap.PAGE_COMMON_PRODUCT)]
        summary = self._summary_of(stripped, sensor)
        common = next(c for c in summary["checks"] if c["name"] == "common pages")
        self.assertFalse(common["pass"], common["detail"])


@unittest.skipIf(ant_sim is None, "pyusb is not installed")
class TestSharedTorqueBaselineIsGarbage(unittest.TestCase):
    """Pin down what aerosense's write-back item 1 actually costs.

    ant_power_rx.c keeps one accumulator set for pages 0x11 and 0x12 and routes
    both into it. This is that mistake, made against a real simulated stream, so
    the replay test in zephyr_aerosense has a number to expect rather than an
    assertion that something is wrong.
    """

    def test_one_baseline_for_two_pages_produces_absurd_power(self):
        sensor = build("power-torque", noise=0.0)
        records = ant_sim.dry_run([sensor], 120.0)
        torque_pages = [ap.decode_power_torque(payload)
                        for _, _, _, payload in records
                        if payload[0] in (ap.PAGE_POWER_WHEEL_TORQUE,
                                          ap.PAGE_POWER_CRANK_TORQUE)]

        shared = []
        for before, now in zip(torque_pages, torque_pages[1:]):
            d_period = ap.delta_u16(now["acc_period"], before["acc_period"])
            d_torque = ap.delta_u16(now["acc_torque"], before["acc_torque"])
            if d_period:
                shared.append(ap.power_from_torque(d_torque, d_period))

        per_page = []
        for page in (ap.PAGE_POWER_WHEEL_TORQUE, ap.PAGE_POWER_CRANK_TORQUE):
            series = [p for p in torque_pages if p["page"] == page]
            for before, now in zip(series, series[1:]):
                d_period = ap.delta_u16(now["acc_period"], before["acc_period"])
                d_torque = ap.delta_u16(now["acc_torque"], before["acc_torque"])
                if d_period:
                    per_page.append(ap.power_from_torque(d_torque, d_period))

        self.assertTrue(per_page)
        self.assertTrue(shared)

        # Keeping the series apart recovers the transmitted power almost
        # exactly - there is no noise in this run, so anything left is rounding
        # in the accumulators.
        per_page_mae = sum(abs(w - WATTS) for w in per_page) / len(per_page)
        self.assertLess(per_page_mae, 2.0)

        # Merging them does not: mean absolute error runs to tens of kilowatts.
        shared_mae = sum(abs(w - WATTS) for w in shared) / len(shared)
        self.assertGreater(shared_mae, 1000.0)
        self.assertGreater(max(shared), 100000.0)

        # And yet only about one sample in six is individually absurd. That is
        # the reason this defect survives on real hardware: most samples land
        # somewhere a receiver's smoothing will accept, and the rest look like
        # dropouts. Anyone judging the fix by eye will conclude it was already
        # working, so the replay test asserts on the aggregate instead.
        absurd = [w for w in shared if w > 3000.0 or w < 20.0]
        self.assertLess(len(absurd) / len(shared), 0.30)


if __name__ == "__main__":
    unittest.main()
