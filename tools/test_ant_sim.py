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

    def test_heart_rate(self):
        summary = self._run("heart-rate")
        # A main page and a four-page background rotation, plus the common
        # pages. All five heart-rate encoders are on the air in one run.
        self.assertEqual(set(summary["pages"]),
                         {"0x00", "0x01", "0x02", "0x03", "0x04",
                          "0x50", "0x51"})
        self.assertGreater(summary["heart_rate_bpm"]["n"], 0)
        self.assertAlmostEqual(summary["heart_rate_bpm"]["mean"],
                               ant_sim.DEFAULT_BPM, delta=5.0)
        # The event time wraps every ~64 s at 1/1024 s, so a 15-minute run that
        # did not reach it would be hiding the wrap rather than testing it.
        self.assertGreater(summary["accumulator_wraps"].get("hr_event_time", 0),
                           0)
        self.assertGreater(summary["accumulator_wraps"].get("beat_count", 0), 0)

    def test_the_toggle_bit_is_not_part_of_the_heart_rate_page_number(self):
        sensor = build("heart-rate")
        records = ant_sim.dry_run([sensor], 60.0)
        raw = {payload[0] for _, _, _, payload in records}
        # Both states of the toggle appear on the air...
        self.assertTrue(any(byte & 0x80 for byte in raw))
        self.assertTrue(any(not byte & 0x80 for byte in raw))
        # ...and neither is a page number.
        summary = analyse(sensor, records)
        for page in summary["pages"]:
            self.assertLessEqual(int(page, 16), 0x7F)

    def test_a_zero_noise_run_is_exact(self):
        sensor = build("power", noise=0.0)
        records = ant_sim.dry_run([sensor], 120.0)
        summary = analyse(sensor, records)
        self.assertLess(summary["power"]["mean_abs_error"], 0.51)
        self.assertLess(summary["cadence"]["mean_abs_error"], 0.51)


ROOT = bytes(range(16))
EPOCH = 7
DEVNUM = 0x3A17


def build_attested(profile: str, window=None, interval_s: float = 20.0,
                   policy: int = ap.COMPAT_POLICY_NEVER, seed: int = 7):
    sensor = build(profile, seed=seed)
    sensor.compat = ant_sim.CompatAttestation(
        ROOT, EPOCH, sensor.device_number, interval_s=interval_s,
        window=window, policy=policy,
        target_device_number=sensor.device_number ^ 0xA5A5)
    return sensor


def analyse_attested(sensor, records, window=None, key: bytes = ROOT,
                     epoch: int = EPOCH) -> dict:
    profile = ant_verify.profile_for(sensor.device_type, __import__(
        "collections").Counter(payload[0] & (0x7F if sensor.device_type ==
                                             ap.HRM_DEVICE_TYPE else 0xFF)
                               for _, _, _, payload in records))
    verifier = (None if key is None else
                ant_verify.CompatVerifier(key, epoch, sensor.device_number,
                                          window=window))
    analyzer = ant_verify.ChannelAnalyzer(
        profile, sensor.device_type,
        ant_verify.period_for(sensor.device_type, profile),
        WATTS, RPM, 2.105, compat=verifier, expect_bpm=ant_sim.DEFAULT_BPM)
    for t, _device_type, _device_number, payload in records:
        analyzer.feed(t, payload)
    return analyzer.summary(ant_verify.DEFAULT_MAX_LOSS_PCT,
                            ant_verify.DEFAULT_JITTER_FACTOR, None)


@unittest.skipIf(ant_sim is None, "pyusb is not installed")
class TestAttestedStream(unittest.TestCase):
    """The compat layer end to end: sim emits, verifier judges, no radio.

    docs/radiant-security.md section 11. The verifier is told the key, the
    epoch and the device number and nothing else - not the interval, not the
    schedule, not which slots the sim chose - so a pass here is the two halves
    agreeing about bytes rather than about intentions.
    """

    def test_a_default_attested_stream_verifies(self):
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], RUN_SECONDS)
        summary = analyse_attested(sensor, records)
        failed = [c for c in summary["checks"] if not c["pass"]]
        self.assertEqual(failed, [], "; ".join(
            f"{c['name']}: {c['detail']}" for c in failed))

        attestation = summary["attestation"]
        self.assertEqual(attestation["verdict"], ant_verify.ATTEST_VERIFIED)
        self.assertGreater(attestation["tier_i"]["verified"], 0)
        self.assertEqual(attestation["tier_i"]["unverified"], 0)
        self.assertEqual(attestation["tier_i"]["lost"], 0)
        self.assertEqual(attestation["tier_i"]["verified_of_delivered"], 1.0)
        # Tier II is off by default and must stay off.
        self.assertEqual(attestation["tier_ii"]["verdict"],
                         ant_verify.ATTEST_CLEAR)
        self.assertEqual(attestation["tier_ii"]["verified"], 0)

    def test_the_stream_is_the_same_ant_plus_profile_plus_two_page_numbers(self):
        # The whole claim, and the reason it is stated as a page-number set
        # rather than a byte-for-byte diff against a plain run: the compat pages
        # DISPLACE data pages, so the two streams carry different samples at the
        # same instants and always will. What must hold is that nothing existing
        # changed - the same profile, the same layouts, the same accumulators,
        # plus exactly two numbers a legacy receiver skips.
        added = {ap.COMPAT_PAGE_BEACON, ap.COMPAT_PAGE_ATTEST_TIER_I}
        plain = ant_sim.dry_run([build("power")], 300.0)
        attested = ant_sim.dry_run([build_attested("power")], 300.0)

        plain_numbers = {p[0] for _, _, _, p in plain}
        attested_numbers = {p[0] for _, _, _, p in attested}
        self.assertEqual(attested_numbers, plain_numbers | added)
        self.assertEqual(plain_numbers & added, set())

        # And a legacy receiver's view: drop what it does not know and every
        # remaining page decodes as a page of the profile it does.
        for _, _, _, payload in attested:
            if payload[0] in added:
                continue
            got = ap.decode(payload, ap.BPWR_DEVICE_TYPE)
            self.assertIn(got["page"], plain_numbers)
            self.assertNotIn("raw", got)

    def test_the_default_configuration_spends_about_two_percent_of_slots(self):
        # 0.8 % beacon plus 1.2 % Tier I, against the 1.65 % ANT+ itself spends
        # on common pages 80 and 81. That comparison is the compatibility
        # argument; the bare number on its own is just a number.
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], RUN_SECONDS)
        pages = __import__("collections").Counter(
            payload[0] for _, _, _, payload in records)
        total = sum(pages.values())
        beacon = pages[ap.COMPAT_PAGE_BEACON] / total
        tier1 = pages[ap.COMPAT_PAGE_ATTEST_TIER_I] / total
        self.assertAlmostEqual(beacon, 0.008, delta=0.002)
        self.assertAlmostEqual(tier1, 0.012, delta=0.003)
        self.assertLess(beacon + tier1, 0.025)
        common = (pages[ap.PAGE_COMMON_MANUFACTURER]
                  + pages[ap.PAGE_COMMON_PRODUCT]) / total
        self.assertAlmostEqual(common, 0.0165, delta=0.003)

    def test_tier_one_verifies_at_the_delivery_rate_under_heavy_loss(self):
        # THE CLAIM THE TIER EXISTS FOR, as an assertion rather than a
        # paragraph: its tag covers no payload, so a packet lost anywhere else
        # costs it nothing. Twenty percent loss is fifty times the characterised
        # bench floor and every DELIVERED Tier I page still verifies.
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], RUN_SECONDS)
        rng = random.Random(11)
        lossy = [r for r in records if rng.random() > 0.20]
        self.assertLess(len(lossy), 0.85 * len(records))

        summary = analyse_attested(sensor, lossy)
        tier1 = summary["attestation"]["tier_i"]
        self.assertGreater(tier1["verified"], 0)
        self.assertEqual(tier1["unverified"], 0)
        self.assertEqual(tier1["verified_of_delivered"], 1.0)
        # And loss is reported as loss, in its own column, rather than as a
        # failed tag. Conflating the two is how a bench floor becomes an attack.
        self.assertGreater(tier1["lost"], 0)

    def test_a_wrong_key_is_unverified_rather_than_clear(self):
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], 300.0)
        summary = analyse_attested(sensor, records, key=bytes(16))
        attestation = summary["attestation"]
        self.assertEqual(attestation["verdict"], ant_verify.ATTEST_UNVERIFIED)
        self.assertGreater(attestation["tier_i"]["unverified"], 0)
        self.assertEqual(attestation["tier_i"]["verified"], 0)
        self.assertFalse(summary["pass"])

    def test_no_key_is_clear_and_clear_is_not_a_judgement(self):
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], 300.0)
        summary = analyse_attested(sensor, records, key=None)
        self.assertEqual(summary["attestation"]["verdict"],
                         ant_verify.ATTEST_CLEAR)
        # A receiver with no key has no opinion, so the stream still passes.
        self.assertTrue(summary["pass"])

    def test_stripping_the_attestation_is_unverified_not_clear(self):
        # Receiver-side downgrade protection. Strip the added pages and a naive
        # receiver falls back to clear; a pinned one must not.
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], 300.0)
        stripped = [r for r in records
                    if r[3][0] not in (ap.COMPAT_PAGE_BEACON,
                                       ap.COMPAT_PAGE_ATTEST_TIER_I)]
        summary = analyse_attested(sensor, stripped)
        self.assertEqual(summary["attestation"]["verdict"],
                         ant_verify.ATTEST_UNVERIFIED)
        self.assertEqual(summary["attestation"]["tier_i"]["verified"], 0)
        self.assertEqual(summary["attestation"]["beacon_frames"], 0)

    def test_a_replayed_tier_one_page_is_rejected_on_the_counter(self):
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], 300.0)
        first = next(r for r in records
                     if r[3][0] == ap.COMPAT_PAGE_ATTEST_TIER_I)
        replayed = list(records) + [(records[-1][0] + 0.25, first[1], first[2],
                                     first[3])]
        summary = analyse_attested(sensor, replayed)
        tier1 = summary["attestation"]["tier_i"]
        self.assertEqual(tier1["replays"], 1)
        self.assertEqual(tier1["unverified"], 1)
        self.assertEqual(summary["attestation"]["verdict"],
                         ant_verify.ATTEST_UNVERIFIED)

    def test_the_beacon_says_what_the_node_will_do(self):
        sensor = build_attested("power", policy=ap.COMPAT_POLICY_COMMAND)
        records = ant_sim.dry_run([sensor], RUN_SECONDS)
        beacon = analyse_attested(sensor, records)["attestation"]["beacon"]
        self.assertIsNotNone(beacon, "no complete beacon set in 15 minutes")
        self.assertEqual(beacon["policy"], ap.COMPAT_POLICY_COMMAND)
        self.assertTrue(beacon["private_available"])
        self.assertEqual(beacon["target_device_type"],
                         ap.RADIANT_TLM_DEVICE_TYPE)
        self.assertEqual(beacon["target_device_number"], DEVNUM ^ 0xA5A5)

    def test_a_never_node_advertises_nowhere_to_go(self):
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], RUN_SECONDS)
        summary = analyse_attested(sensor, records)
        beacon = summary["attestation"]["beacon"]
        self.assertEqual(beacon["policy"], ap.COMPAT_POLICY_NEVER)
        self.assertFalse(beacon["private_available"])
        self.assertEqual(beacon["target_device_number"], 0)
        self.assertEqual(beacon["target_period"], 0)
        self.assertEqual(summary["attestation"]["beacon_malformed"], 0)
        self.assertGreater(summary["attestation"]["key_group_hint_matches"], 0)

    def test_the_key_group_hint_is_epoch_derived(self):
        sensor = build_attested("power")
        records = ant_sim.dry_run([sensor], RUN_SECONDS)
        # The same key one epoch later produces a different hint, which is what
        # stops the field being a stable tracking identifier.
        matched = analyse_attested(sensor, records)["attestation"]
        stale = analyse_attested(sensor, records, epoch=EPOCH + 1)["attestation"]
        self.assertGreater(matched["key_group_hint_matches"], 0)
        self.assertEqual(stale["key_group_hint_matches"], 0)

    def test_a_faster_interval_costs_proportionally_more_slots(self):
        # T is in seconds and decoupled from the data rate, so halving it
        # doubles the page count and nothing else moves.
        counts = []
        for interval in (20.0, 10.0):
            sensor = build_attested("power", interval_s=interval)
            records = ant_sim.dry_run([sensor], 600.0)
            counts.append(sum(1 for _, _, _, p in records
                              if p[0] == ap.COMPAT_PAGE_ATTEST_TIER_I))
        self.assertAlmostEqual(counts[0], 600 / 20, delta=2)
        self.assertAlmostEqual(counts[1], 600 / 10, delta=2)

    def test_heart_rate_carries_the_same_two_page_numbers(self):
        # The same numbers in every compat profile, so a receiver has one rule.
        sensor = build_attested("heart-rate")
        records = ant_sim.dry_run([sensor], RUN_SECONDS)
        summary = analyse_attested(sensor, records)
        failed = [c for c in summary["checks"] if not c["pass"]]
        self.assertEqual(failed, [], "; ".join(
            f"{c['name']}: {c['detail']}" for c in failed))
        self.assertEqual(summary["attestation"]["verdict"],
                         ant_verify.ATTEST_VERIFIED)
        self.assertIn(f"0x{ap.COMPAT_PAGE_BEACON:02X}", summary["pages"])
        self.assertIn(f"0x{ap.COMPAT_PAGE_ATTEST_TIER_I:02X}",
                      summary["pages"])
        # Every compat page number is 7-bit, so the toggle bit never lifts one
        # out of the namespace a heart-rate receiver can express.
        for page in summary["pages"]:
            self.assertLessEqual(int(page, 16), ap.COMPAT_PAGE_MAX)

    def test_speed_and_cadence_can_never_carry_these_pages(self):
        self.assertNotIn("csc", ant_sim.ATTESTABLE)
        self.assertNotIn(ap.BSC_COMBINED_DEVICE_TYPE, ap.COMPAT_DEVICE_TYPES)
        # And structurally: its profile overrides the rotation outright, so
        # there is no slot for a compat page even if one were configured.
        sensor = build("csc")
        sensor.compat = ant_sim.CompatAttestation(ROOT, EPOCH, DEVNUM)
        records = ant_sim.dry_run([sensor], 300.0)
        self.assertEqual(len(records), 300.0 // sensor.period_s)
        self.assertEqual(sensor.compat.beacons_sent, 0)
        self.assertEqual(sensor.compat.tier1_sent, 0)

    def test_an_announcement_is_acted_on_only_after_its_tag_verifies(self):
        # The SWITCH/RETURN path, which is otherwise written in C8 and never
        # exercised until then. Frame B tags frame A's full eight bytes under
        # subtype 0x03 and the Tier I counter.
        verifier = ant_verify.CompatVerifier(ROOT, EPOCH, DEVNUM)
        tier1 = ap.encode_compat_attest_tier1(
            5, __import__("radiant_crypto").compat_tier1_tag(
                verifier.k_auth, EPOCH, DEVNUM, 5))
        self.assertEqual(verifier.feed(tier1), ant_verify.ATTEST_VERIFIED)

        frame_a = ap.encode_compat_announce_a(ap.CompatAnnounce(
            target_device_type=ap.RADIANT_TLM_DEVICE_TYPE,
            target_device_number=0x9C41, target_period=8182,
            reason=ap.COMPAT_REASON_COMMAND, countdown=8))
        tag = __import__("radiant_crypto").compat_announce_tag(
            verifier.k_auth, EPOCH, DEVNUM, 5, frame_a)
        self.assertEqual(verifier.feed(frame_a), None)
        self.assertEqual(verifier.feed(ap.encode_compat_announce_b(tag)),
                         ant_verify.ATTEST_VERIFIED)

        # A forged locator under a captured tag is ignored, and counted.
        forged = bytearray(frame_a)
        forged[3] ^= 0xFF
        self.assertEqual(verifier.feed(bytes(forged)), None)
        self.assertEqual(verifier.feed(ap.encode_compat_announce_b(tag)),
                         ant_verify.ATTEST_UNVERIFIED)
        self.assertEqual(verifier.summary()["announce"],
                         {"verified": 1, "unverified": 1})


@unittest.skipIf(ant_sim is None, "pyusb is not installed")
class TestTierTwoDataAttestation(unittest.TestCase):
    """The expensive tier, and the reason it is off by default."""

    WINDOW = 8

    def stream(self, seconds: float = 600.0):
        sensor = build_attested("power", window=self.WINDOW)
        return sensor, ant_sim.dry_run([sensor], seconds)

    def test_the_windows_verify(self):
        sensor, records = self.stream()
        summary = analyse_attested(sensor, records, window=self.WINDOW)
        tier2 = summary["attestation"]["tier_ii"]
        self.assertGreater(tier2["verified"], 100)
        self.assertEqual(tier2["unverified"], 0)
        self.assertEqual(tier2["lost"], 0)
        self.assertEqual(tier2["verdict"], ant_verify.ATTEST_VERIFIED)
        self.assertEqual(summary["attestation"]["verdict"],
                         ant_verify.ATTEST_VERIFIED)

    def test_it_costs_one_slot_in_n(self):
        sensor, records = self.stream()
        tags = sum(1 for _, _, _, p in records
                   if p[0] == ap.COMPAT_PAGE_ATTEST_TIER_II)
        self.assertAlmostEqual(tags / len(records), 1.0 / self.WINDOW,
                               delta=0.005)

    def test_a_lost_packet_unverifies_a_window_but_not_the_next_one(self):
        # The honest regression: a window CMAC is not self-synchronising, so a
        # dropped packet leaves nothing to check the tag against. That is LOST,
        # not UNVERIFIED - the tag never failed, its evidence never arrived.
        sensor, records = self.stream(300.0)
        victim = next(i for i, r in enumerate(records)
                      if i > 40 and r[3][0] == ap.PAGE_POWER_STANDARD)
        lossy = records[:victim] + records[victim + 1:]
        tier2 = analyse_attested(sensor, lossy,
                                 window=self.WINDOW)["attestation"]["tier_ii"]
        self.assertEqual(tier2["lost"], 1)
        self.assertEqual(tier2["unverified"], 0)
        self.assertGreater(tier2["verified"], 20)

    def test_a_flipped_payload_bit_is_unverified_not_lost(self):
        # The other half of the same distinction: everything arrived and the
        # tag says the bytes are not the ones that were sent.
        sensor, records = self.stream(300.0)
        victim = next(i for i, r in enumerate(records)
                      if i > 40 and r[3][0] == ap.PAGE_POWER_STANDARD)
        t, device_type, device_number, payload = records[victim]
        corrupt = bytearray(payload)
        corrupt[6] ^= 0x01
        tampered = list(records)
        tampered[victim] = (t, device_type, device_number, bytes(corrupt))
        tier2 = analyse_attested(sensor, tampered,
                                 window=self.WINDOW)["attestation"]["tier_ii"]
        self.assertEqual(tier2["unverified"], 1)
        self.assertEqual(tier2["lost"], 0)

    def test_tier_one_is_unmoved_by_what_breaks_tier_two(self):
        # One lost packet unverifies a whole Tier II window and costs Tier I
        # nothing at all, because Tier I covers no payload. That difference is
        # the entire reason the two tiers exist separately.
        sensor, records = self.stream(600.0)
        rng = random.Random(3)
        lossy = [r for r in records if rng.random() > 0.10]
        attestation = analyse_attested(sensor, lossy,
                                       window=self.WINDOW)["attestation"]
        self.assertEqual(attestation["tier_i"]["verified_of_delivered"], 1.0)
        self.assertEqual(attestation["tier_i"]["unverified"], 0)
        self.assertGreater(attestation["tier_ii"]["lost"], 10)


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
