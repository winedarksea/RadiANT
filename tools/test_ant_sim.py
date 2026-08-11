#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

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
class TestTelemetryEnvelope(unittest.TestCase):
    """Device type 0x60, end to end on the host with no radio.

    THIS IS THE PYTHON HALF OF PHASE E'S GATE. The C half
    (radiant_core/tests/src/test_profiles.c) puts frames across a mock radio
    and decodes a schema from them; this half drives a real page rotation for
    a simulated quarter of an hour and requires ant_verify.py's accumulator
    continuity check to pass over the whole of it.

    What makes it a gate rather than a round trip is that ant_verify.py is
    told nothing about ant_sim.py. It does not know this node's fields, their
    widths, their offsets or which pages carry them. It assembles the
    descriptor set off the stream, decodes against what it assembled, looks up
    the accumulating field's instantaneous partner in the section 7
    vocabulary, and checks one against the other. Every one of those steps is
    a step a receiver in the field takes.
    """

    def _run(self, profile: str, seconds: float = RUN_SECONDS):
        sensor = build(profile)
        records = ant_sim.dry_run([sensor], seconds)
        return sensor, records, analyse(sensor, records)

    def test_the_telemetry_node_passes_every_check(self):
        sensor, records, summary = self._run("telemetry")
        failed = [c for c in summary["checks"] if not c["pass"]]
        self.assertEqual(
            failed, [],
            "; ".join(f"{c['name']}: {c['detail']}" for c in failed))
        self.assertEqual(summary["violations"]["count"], 0,
                         summary["violations"]["first"])

    def test_the_schema_is_recovered_from_the_descriptor_alone(self):
        sensor, records, summary = self._run("telemetry")
        heard = summary["telemetry"]
        self.assertIsNotNone(heard, "no descriptor set was ever assembled")
        self.assertEqual(heard["schema_id"], ant_sim.TelemetrySensor.SCHEMA_ID)
        self.assertEqual(heard["period_ticks"], ap.RADIANT_TLM_PERIOD_DEFAULT)
        self.assertEqual(heard["rf_index"], 57)
        self.assertFalse(heard["sparse"])
        self.assertEqual(heard["schema_changes"], 0)

        # The vocabulary lookup is the part that makes a bridge mechanical
        # rather than bespoke: the receiver got a quantity and a unit for each
        # field without a line of per-node code.
        by_id = {f["id"]: f for f in heard["fields"]}
        self.assertEqual(by_id[1]["quantity"], "heart rate")
        self.assertEqual(by_id[1]["unit"], "bpm")
        self.assertEqual(by_id[2]["quantity"], "temperature")
        self.assertEqual(by_id[2]["unit"], "K")
        self.assertEqual(by_id[2]["exponent"], -2)
        self.assertEqual(by_id[3]["quantity"], "energy")
        self.assertTrue(by_id[3]["accumulate"])
        self.assertEqual(by_id[4]["quantity"], "active power")
        self.assertTrue(by_id[4]["signed"])
        self.assertEqual(by_id[4]["bit_offset"], 32)

        # And it decoded most of the stream against it. Everything before the
        # first complete set is counted rather than guessed at.
        self.assertGreater(heard["data_pages_decoded"], 3000)
        self.assertLessEqual(heard["data_pages_before_schema"], 6)

    def test_the_interleave_survives_the_round_trip(self):
        sensor, records, summary = self._run("telemetry")
        pages = summary["pages"]
        self.assertEqual(set(pages), {"0x00", "0x01", "0x02", "0x50", "0x51"})
        # Six descriptor frames, two common pages and 113 data pages per cycle
        # of 121 - and the descriptor set is CONSECUTIVE, so a receiver joining
        # mid-stream waits one cycle rather than six.
        cycles = len(records) / ap.RADIANT_TLM_CYCLE
        self.assertAlmostEqual(pages["0x00"] / cycles, 6.0, delta=0.2)
        self.assertAlmostEqual(pages["0x50"] / cycles, 1.0, delta=0.1)
        self.assertAlmostEqual(pages["0x51"] / cycles, 1.0, delta=0.1)

    def test_the_event_counter_wraps_and_is_still_continuous(self):
        sensor, records, summary = self._run("telemetry")
        # 3600 messages at 4 Hz, so the 8-bit counter goes round many times.
        # A run that never reached the wrap has not tested the delta.
        for page in ("0x01", "0x02"):
            self.assertGreater(
                summary["accumulator_wraps"].get(f"counter({page})", 0), 5,
                f"the counter on page {page} never wrapped")
        self.assertEqual(summary["violations"]["count"], 0)

    def test_a_corrupted_accumulator_is_caught(self):
        # The instrument has to fail things. Jump the energy accumulator on one
        # page-2 packet and the integral check must notice it disagreeing with
        # the instantaneous power beside it.
        sensor = build("telemetry")
        records = ant_sim.dry_run([sensor], 120.0)
        page2 = [i for i, r in enumerate(records) if r[3][0] == 0x02]
        index = page2[len(page2) // 2]
        t, dt_, dn, payload = records[index]
        broken = bytearray(payload)
        broken[2] = (broken[2] + 0x40) & 0xFF     # +2^30 J out of nowhere
        records[index] = (t, dt_, dn, bytes(broken))

        summary = analyse(sensor, records)
        continuity = [c for c in summary["checks"]
                      if c["name"] == "accumulator continuity"][0]
        self.assertFalse(continuity["pass"], continuity["detail"])
        self.assertGreater(summary["violations"]["count"], 0)

    def test_a_corrupted_descriptor_frame_is_refused_not_decoded(self):
        # A receiver that guesses a layout reports confident nonsense, so a
        # descriptor frame that cannot be parsed must leave the schema unknown
        # rather than half-applied.
        sensor = build("telemetry")
        records = ant_sim.dry_run([sensor], 60.0)
        broken = []
        for t, dt_, dn, payload in records:
            if payload[0] == ap.PAGE_TLM_DESCRIPTOR and payload[1] >> 4 == 0:
                payload = bytes([payload[0], payload[1], 0x24]) + payload[3:]
            broken.append((t, dt_, dn, payload))

        summary = analyse(sensor, broken)
        self.assertIsNone(summary["telemetry"],
                          "a version-2 descriptor must be rejected, not "
                          "decoded under v1 rules")
        decoded = [c for c in summary["checks"]
                   if c["name"] == "descriptor decoded"][0]
        self.assertFalse(decoded["pass"])


@unittest.skipIf(ant_sim is None, "pyusb is not installed")
class TestSparseAssetTag(unittest.TestCase):
    """The envelope with everything turned off.

    Free to run - there is nothing to encode - and the cheapest exercise of the
    sparse path there is.
    """

    def _run(self, privacy_pages: bool, seconds: float = RUN_SECONDS):
        sensor = build("asset-tag")
        if privacy_pages:
            sensor.serial_number = None
        records = ant_sim.dry_run([sensor], seconds)
        return sensor, records, analyse(sensor, records)

    def test_a_tag_with_a_privacy_posture_emits_nothing_but_a_heartbeat(self):
        sensor, records, summary = self._run(privacy_pages=True)
        # 900 s, a 30 s heartbeat, two frames per set. Nothing else at all:
        # page 82's operating-time counter is monotone, so it survives an
        # identity change and fingerprints a battery swap - which for a node
        # whose whole payload IS an identity is the leak that matters.
        self.assertEqual(set(summary["pages"]), {"0x00"})
        self.assertEqual(summary["pages"]["0x00"], 60)
        self.assertEqual(summary["violations"]["count"], 0)

        heard = summary["telemetry"]
        self.assertIsNotNone(heard)
        self.assertTrue(heard["sparse"])
        self.assertEqual(heard["heartbeat_s"], 30)
        self.assertEqual(heard["fields"], [])

    def test_page_82_appears_only_without_the_privacy_posture(self):
        _, _, summary = self._run(privacy_pages=False)
        self.assertEqual(set(summary["pages"]), {"0x00", "0x52"})
        self.assertEqual(summary["pages"]["0x52"], 30, "one per heartbeat")

    def test_a_sparse_node_is_not_judged_against_a_period_it_never_claimed(self):
        # Section 8's failure mode, and the reason the flag is in the
        # descriptor: a receiver that measured this node's 60 transmissions
        # against a 4 Hz period would report 98 % loss and enormous jitter for
        # a node that is working exactly as specified, "as terrible link
        # quality rather than as a configuration mismatch."
        _, _, summary = self._run(privacy_pages=True)
        names = {c["name"] for c in summary["checks"]}
        self.assertIn("sparse cadence", names)
        self.assertNotIn("loss", names)
        self.assertNotIn("jitter", names)
        self.assertNotIn("common pages", names)

        failed = [c for c in summary["checks"] if not c["pass"]]
        self.assertEqual(
            failed, [],
            "; ".join(f"{c['name']}: {c['detail']}" for c in failed))


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


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class TestLossAccounting(unittest.TestCase):
    """Loss the radio never reported did not happen on the radio.

    This is the check that would have caught the libusb read timeout that once
    sat at 250 ms against a 249.7 ms channel period, cancelling transfers on top
    of the packets they were waiting for. The invented losses were
    indistinguishable from on-air ones in every figure the tool printed; the only
    signal was that the radio raised no RX_FAIL for them.
    """

    @staticmethod
    def run_of(expected, received, events):
        return ant_verify.loss_accounting({
            "channels": [{"expected_packets": expected, "packets": received}],
            "events": events,
        })

    def test_silent_loss_fails(self):
        # Eight missing, three RX_FAIL: the shape of a real 300 s run before the
        # timeout was fixed.
        result = self.run_of(1201, 1193, {"RX_FAIL": 3})
        self.assertEqual(result["missing"], 8)
        self.assertEqual(result["unaccounted"], 5)
        self.assertFalse(result["pass"])
        self.assertIn("without the radio noticing", result["verdict"])

    def test_loss_the_radio_owns_passes(self):
        # Same run after the fix: fewer missing, and every one of them announced.
        result = self.run_of(1200, 1195, {"RX_FAIL": 5})
        self.assertEqual(result["unaccounted"], 0)
        self.assertTrue(result["pass"])
        self.assertIn("on the air", result["verdict"])

    def test_a_packet_either_way_is_not_a_bug(self):
        # `missing` is a wall-clock estimate and an event can land after the
        # window closes, so the check has to survive being off by one or two.
        self.assertTrue(self.run_of(1200, 1194, {"RX_FAIL": 4})["pass"])

    def test_no_events_at_all_is_the_loudest_case(self):
        result = self.run_of(1200, 1180, {})
        self.assertEqual(result["radio_fails"], 0)
        self.assertFalse(result["pass"])
        self.assertIn("USB path", result["verdict"])

    def test_replay_declines_to_judge(self):
        # A capture stores payloads, not channel events. Reading the absent
        # events as "the radio reported nothing" would blame the host for every
        # packet the air ate.
        self.assertIsNone(self.run_of(1200, 1180, None))

    def test_a_clean_run_has_nothing_to_account_for(self):
        self.assertIsNone(self.run_of(1200, 1200, {"RX_FAIL": 0}))


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class TestExtendedFields(unittest.TestCase):
    """The appended fields are positional, and the flag byte is the position.

    Every offset here depends on which *earlier* fields turned up, so reading
    RSSI at a fixed byte works perfectly until a run is made without the
    channel id and then reports half a device number as a signal strength. The
    tests that matter are the ones where a field is absent.
    """

    # A real body off the bench: channel 0, page 0x10, flags 0xE0, then
    # #14871 / type 0x0B / trans 5, then dBm-type RSSI of -27 with a -80
    # threshold, then a receive timestamp of 0x4CEE ticks.
    REAL = bytes.fromhex("001059004eca376300e0173a0b0520e5b0ee4c")

    def test_all_three_fields(self):
        fields = ant_verify.extended_fields(self.REAL)
        self.assertEqual(fields["device_number"], 14871)
        self.assertEqual(fields["device_type"], 0x0B)
        self.assertEqual(fields["rssi_dbm"], -27)
        self.assertEqual(fields["rx_ticks"], 0x4CEE)

    def test_channel_id_only_is_what_the_tools_used_to_ask_for(self):
        body = self.REAL[:10] + self.REAL[10:14]
        fields = ant_verify.extended_fields(body[:9] + b"\x80" + body[10:])
        self.assertEqual(fields["device_number"], 14871)
        self.assertNotIn("rssi_dbm", fields)
        self.assertNotIn("rx_ticks", fields)

    def test_rssi_without_channel_id_moves_up_four_bytes(self):
        # The whole reason for reading the flag byte. At a fixed offset of 14
        # this would return the -80 threshold, or nothing at all.
        body = self.REAL[:9] + b"\x40" + bytes([0x20, 0xE5, 0xB0])
        self.assertEqual(ant_verify.extended_fields(body)["rssi_dbm"], -27)

    def test_a_scale_we_cannot_read_is_not_reported(self):
        # Measurement types other than 0x20 are proprietary. A number on an
        # unknown scale printed as dBm is worse than no number.
        body = bytearray(self.REAL)
        body[14] = 0x00
        fields = ant_verify.extended_fields(bytes(body))
        self.assertNotIn("rssi_dbm", fields)
        self.assertEqual(fields["rx_ticks"], 0x4CEE)   # the rest still lines up

    def test_truncated_and_bare_bodies(self):
        self.assertEqual(ant_verify.extended_fields(self.REAL[:9]), {})
        self.assertEqual(ant_verify.extended_fields(self.REAL[:16]),
                         {"device_number": 14871, "device_type": 0x0B})
        self.assertEqual(ant_verify.extended_fields(b""), {})


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class TestRadioClock(unittest.TestCase):
    """Intervals measured on the dongle's clock instead of on Windows'."""

    PERIOD = 8182
    PAGE = bytes([0x10, 0x00, 0x00, 0x50, 0x00, 0x00, 0x64, 0x00])

    def analyzer(self):
        return ant_verify.ChannelAnalyzer("power", 0x0B, self.PERIOD,
                                          None, None, 2.105)

    def feed_run(self, ticks, host_step=8182 / 32768.0):
        a = self.analyzer()
        for i, tk in enumerate(ticks):
            page = bytes([0x10, i & 0xFF]) + self.PAGE[2:]
            a.feed(i * host_step, page,
                   {"rssi_dbm": -30 - (i % 3), "rx_ticks": tk & 0xFFFF})
        return a

    def test_a_wrap_is_subtracted_not_reconstructed(self):
        # The counter is 16 bits at 32768 Hz, so it rolls every 2 s - once
        # every eight packets at this period. A masked subtraction gets every
        # one of them right; anything that tried to track the high bits would
        # have to be right about them.
        a = self.feed_run([1000 + i * self.PERIOD for i in range(40)])
        self.assertEqual(len(a.radio_intervals), 39)
        self.assertTrue(all(abs(d - self.PERIOD / 32768.0) < 1e-9
                            for d in a.radio_intervals))

    def test_a_gap_too_long_to_be_unambiguous_is_dropped(self):
        # Beyond 2 s the masked subtraction cannot tell one wrap from two, and
        # a jitter sample invented from that is worse than a missing one. The
        # host clock is crude but it is easily good enough to notice.
        a = self.analyzer()
        a.feed(0.0, self.PAGE, {"rx_ticks": 0})
        a.feed(3.0, bytes([0x10, 1]) + self.PAGE[2:], {"rx_ticks": 500})
        self.assertEqual(a.radio_intervals, [])

    def test_signal_strength_is_summarised(self):
        summary = self.feed_run(
            [1000 + i * self.PERIOD for i in range(30)]).summary(
                ant_verify.DEFAULT_MAX_LOSS_PCT,
                ant_verify.DEFAULT_JITTER_FACTOR, None)
        self.assertEqual(summary["signal"]["n"], 30)
        self.assertEqual(summary["signal"]["max_dbm"], -30)
        self.assertEqual(summary["signal"]["min_dbm"], -32)
        self.assertAlmostEqual(summary["jitter"]["offgrid_radio_s"], 0.0)

    def test_a_lost_packet_moves_jitter_but_not_the_grid(self):
        """The distinction the whole measurement turns on.

        Skipping a slot doubles one interval. The plain stddev reads that as
        tens of ms of jitter even though every packet arrived exactly on time -
        which is how the jitter figure came to track the loss figure and say
        nothing on its own.
        """
        clean = self.feed_run([1000 + i * self.PERIOD for i in range(40)])
        holed = self.feed_run([1000 + i * self.PERIOD
                               for i in range(40) if i != 20])

        self.assertAlmostEqual(clean._offgrid(clean.radio_intervals), 0.0)
        self.assertAlmostEqual(holed._offgrid(holed.radio_intervals), 0.0)

        import statistics as st
        self.assertGreater(st.pstdev(holed.radio_intervals), 0.03)
        self.assertAlmostEqual(st.pstdev(clean.radio_intervals), 0.0)

    def test_a_dongle_that_reports_neither_still_works(self):
        # Replay, and any dongle that refuses the library config. Everything
        # that existed before these two fields has to keep working without
        # them, which is why they are reported and not judged.
        a = self.analyzer()
        for i in range(20):
            a.feed(i * 0.2497, bytes([0x10, i]) + self.PAGE[2:])
        summary = a.summary(ant_verify.DEFAULT_MAX_LOSS_PCT,
                            ant_verify.DEFAULT_JITTER_FACTOR, None)
        self.assertIsNone(summary["signal"])
        self.assertIsNone(summary["jitter"]["offgrid_radio_s"])
        self.assertIsNotNone(summary["jitter"]["offgrid_host_s"])
        self.assertTrue(any(c["name"] == "jitter" for c in summary["checks"]))


if __name__ == "__main__":
    unittest.main()
