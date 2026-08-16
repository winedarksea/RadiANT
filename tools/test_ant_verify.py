#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Tests for tools/ant_verify.py's multi-channel accounting. No hardware.

Everything here runs through the `--replay` path against
`vectors/handmade-multichannel.antcap`, so the code under test is the same
code a bench run executes - the analyser, the pairing of the per-channel
arguments, the derived block - with only the radio replaced by a file.

Three properties are worth more than the rest, and each has a way of quietly
going wrong:

  * PER-CHANNEL FIGURES MUST NOT POOL. The defect this measurement exists for
    costs whole slots on one channel of a pair and leaves the other untouched,
    so an aggregate halves it. The vector's two streams therefore have
    deliberately different losses, and the test asserts the two numbers
    separately rather than asserting their average.
  * THE AGGREGATE MUST KEEP ITS OLD MEANING. `ant_ab.py`'s loss gate reads
    `derived.loss_exact_pct`, and every baseline in archive/benchmarks/ is a
    single-channel run. On one channel the aggregate has to be that channel's
    own figure, to the bit.
  * A MISSING FIGURE MUST NOT READ AS ZERO. `loss_exact_pct` is None when the
    transmitter's counter cannot be trusted, because a gate that reads a
    zero passes and a gate that reads a missing field fails.
"""

from __future__ import annotations

import contextlib
import io
import json
import math
import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_ab  # noqa: E402
import ant_pages as ap  # noqa: E402

try:
    import ant_verify
except SystemExit:  # pragma: no cover - pyusb missing
    # ant_verify exits rather than raises when pyusb is absent; that would
    # otherwise take the whole test run down (test_ant_sim.py does the same).
    ant_verify = None

HERE = os.path.dirname(os.path.abspath(__file__))
VECTOR = os.path.join(HERE, "vectors", "handmade-multichannel.antcap")
SCHEMA_PATH = os.path.join(HERE, "..", "archive", "benchmarks",
                           "baseline.schema.json")

# What the vector was cut to contain, restated here rather than read out of
# the file. The comment block at the top of the .antcap claims 1 deleted page
# 0x10 on #4660 and 6 on #4661; if a regenerated vector ever disagrees with
# that claim, these numbers are what says so.
DEVICE_A, DEVICE_B = 4660, 4661
MISSED_A, MISSED_B = 1, 6
SENT = 159


def replay(*extra: str, path: str = VECTOR) -> dict:
    """Run ant_verify.py's --replay path and return the result document.

    Through main() and argparse rather than by calling the analyser directly:
    the argument pairing is half of what this change adds, and a test that
    constructs the analysers itself would not exercise it.

    Narration goes to stderr under `--json -` and is swallowed here; the
    return value is deliberately not checked, because a vector with holes cut
    into it is supposed to fail the loss check.
    """
    argv = ["ant_verify.py", "--replay", path, "--json", "-", *extra]
    out, err = io.StringIO(), io.StringIO()
    with (contextlib.redirect_stdout(out), contextlib.redirect_stderr(err),
            unittest.mock.patch.object(sys, "argv", argv)):
        ant_verify.main()
    return json.loads(out.getvalue())


def one_stream_capture(device_number: int, directory: str) -> str:
    """The vector with one sensor's packets kept, for the legacy shape.

    Cut from the same file rather than written separately so that the
    single-channel and multi-channel assertions are about the same packets.
    """
    records = [record for record in ap.read_capture(VECTOR)
               if record[2] == device_number]
    path = os.path.join(directory, f"one-{device_number}.antcap")
    ap.write_capture(path, records)
    return path


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class PerChannelArguments(unittest.TestCase):
    """`--channel` and friends, expanded to one value per channel."""

    def expand(self, values, count, **kwargs):
        return ant_verify.per_channel_args(values, count, "--x", **kwargs)

    def test_absent_is_the_default_everywhere(self):
        self.assertEqual(self.expand(None, 3, default=57), [57, 57, 57])

    def test_given_once_it_applies_to_every_channel(self):
        self.assertEqual(self.expand([78], 3, default=57), [78, 78, 78])

    def test_given_once_a_ramping_argument_counts_up(self):
        # The behaviour --channel has always had: one --channel with several
        # --profile flags opens consecutive channels. Bench scripts rely on
        # it, so it is pinned here rather than left to be rediscovered.
        self.assertEqual(self.expand([4], 3, default=0, ramp=True), [4, 5, 6])

    def test_a_wildcard_is_never_ramped(self):
        # 0 means "any device". Ramping it to 1 would pin channel 1 to a real
        # device number nobody asked for, and the run would then report the
        # resulting silence as loss.
        self.assertEqual(
            self.expand([0], 3, default=0, ramp=True, wildcard=0), [0, 0, 0])

    def test_repeated_it_pairs_positionally(self):
        self.assertEqual(self.expand([2, 9, 4], 3, default=0, ramp=True),
                         [2, 9, 4])

    def test_a_partial_pairing_is_refused_rather_than_zipped_short(self):
        # Silently truncating would attribute channel 2's setting to channel
        # 3 and report the result as a measurement.
        with self.assertRaises(SystemExit) as caught:
            self.expand([2, 9], 3, default=0)
        self.assertIn("--x", str(caught.exception))


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class MultiChannelReplay(unittest.TestCase):
    """Two sensors in one capture, counted apart."""

    @classmethod
    def setUpClass(cls):
        cls.result = replay("--channel", "0", "--device-number", str(DEVICE_A),
                            "--channel", "1", "--device-number", str(DEVICE_B))
        cls.per_channel = cls.result["derived"]["per_channel"]

    def test_each_channel_gets_its_own_entry_keyed_by_channel(self):
        self.assertEqual(sorted(self.per_channel), ["0", "1"])

    def test_the_two_channels_report_their_own_exact_loss(self):
        self.assertAlmostEqual(self.per_channel["0"]["loss_exact_pct"],
                               100.0 * MISSED_A / SENT)
        self.assertAlmostEqual(self.per_channel["1"]["loss_exact_pct"],
                               100.0 * MISSED_B / SENT)

    def test_the_missing_packets_are_counted_not_estimated(self):
        # From the transmitter's own event counter, so these are integers and
        # not a division by a nominal period.
        self.assertEqual(self.per_channel["0"]["exact_missed"], MISSED_A)
        self.assertEqual(self.per_channel["1"]["exact_missed"], MISSED_B)
        self.assertEqual(self.per_channel["0"]["exact_sent"], SENT)
        self.assertEqual(self.per_channel["1"]["exact_sent"], SENT)

    def test_a_channel_is_attributed_to_the_device_it_was_paired_with(self):
        self.assertEqual(self.per_channel["0"]["device_number"], DEVICE_A)
        self.assertEqual(self.per_channel["1"]["device_number"], DEVICE_B)

    def test_the_aggregate_pools_the_counters(self):
        self.assertAlmostEqual(self.result["derived"]["loss_exact_pct"],
                               100.0 * (MISSED_A + MISSED_B) / (2 * SENT))

    def test_one_channel_can_fail_while_the_other_passes(self):
        # The property the whole change is for. If per-channel state had
        # leaked between analysers, a stream with six holes in it would drag
        # the clean one down with it and the two verdicts would agree.
        verdicts = {c["profile"]: c["pass"] for c in self.result["channels"]}
        self.assertEqual(verdicts, {f"power #{DEVICE_A}": True,
                                    f"power #{DEVICE_B}": False})

    def test_the_streams_are_not_merged_into_one(self):
        self.assertEqual(len(self.result["channels"]), 2)

    def test_replay_reports_no_unexplained_loss_figure_at_all(self):
        # A capture records payloads, not channel events, so there are no
        # RX_FAILs to account the holes with. None, never 0: "nobody asked the
        # radio" and "the radio reported nothing" are different answers, and
        # ant_ab.py fails a missing field where it would pass a zero.
        self.assertIsNone(self.result["derived"]["unexplained_loss"])


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class ChannelsAreNotInventedOnReplay(unittest.TestCase):
    """A capture carries no channel numbers, and none are guessed."""

    def test_without_a_pairing_the_channel_is_null_and_the_key_is_the_stream(self):
        result = replay()
        per_channel = result["derived"]["per_channel"]
        self.assertEqual(sorted(per_channel),
                         [f"power #{DEVICE_A}", f"power #{DEVICE_B}"])
        for entry in per_channel.values():
            self.assertIsNone(entry["channel"])

    def test_the_figures_are_the_same_either_way(self):
        # Labelling is the only thing the pairing changes. If it changed a
        # number, the operator's command line would be an input to the
        # measurement.
        without = replay()["derived"]["loss_exact_pct"]
        with_pairing = replay("--channel", "0",
                              "--device-number", str(DEVICE_A),
                              "--channel", "1",
                              "--device-number", str(DEVICE_B))
        self.assertAlmostEqual(without,
                               with_pairing["derived"]["loss_exact_pct"])

    def test_a_ramped_device_number_reproduces_a_live_runs_pairing(self):
        # #4660 on channel 0 and #4661 on channel 1 is what a live run with a
        # single ramped --device-number would have opened, so a capture it
        # recorded replays with its channel labels intact.
        result = replay("--channel", "0", "--device-number", str(DEVICE_A),
                        "--profile", "power", "--profile", "power")
        self.assertEqual(sorted(result["derived"]["per_channel"]), ["0", "1"])


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class SingleChannelIsUnchanged(unittest.TestCase):
    """The aggregate must still mean what every baseline recorded it to mean."""

    @classmethod
    def setUpClass(cls):
        cls._directory = tempfile.TemporaryDirectory()
        path = one_stream_capture(DEVICE_B, cls._directory.name)
        cls.result = replay("--channel", "0", "--device-number", str(DEVICE_B),
                            path=path)

    @classmethod
    def tearDownClass(cls):
        cls._directory.cleanup()

    def test_one_channel_makes_the_aggregate_that_channels_own_figure(self):
        # Not almost-equal: on one channel the pooled expression must reduce
        # to the same arithmetic, or the loss gate is reading a different
        # number from the one archive/benchmarks/ was filled with.
        self.assertEqual(len(self.result["channels"]), 1)
        self.assertEqual(self.result["derived"]["loss_exact_pct"],
                         self.result["channels"][0]["exact_loss"]["loss_pct"])

    def test_the_other_aggregates_reduce_to_the_one_channel_too(self):
        channel = self.result["channels"][0]
        derived = self.result["derived"]
        self.assertEqual(derived["loss_pct"], channel["loss_pct"])
        self.assertEqual(derived["accumulator_violations"],
                         channel["violations"]["count"])
        self.assertAlmostEqual(derived["timing_offgrid_host_ms"],
                               channel["jitter"]["offgrid_host_s"] * 1000.0)

    def test_one_channel_still_produces_a_per_channel_block(self):
        # Cheap, and it is what lets a future baseline carry both without a
        # special case for the single-channel runs.
        self.assertIn("0", self.result["derived"]["per_channel"])

    def test_the_single_channel_figure_matches_the_multi_channel_one(self):
        # The same packets, analysed alone and analysed beside another
        # sensor, must give the same number. If they differ, per-channel
        # state is leaking between analysers - which is the failure mode the
        # whole per-channel accounting exists to rule out.
        beside = replay("--channel", "0", "--device-number", str(DEVICE_A),
                        "--channel", "1", "--device-number", str(DEVICE_B))
        self.assertEqual(
            self.result["derived"]["per_channel"]["0"]["loss_exact_pct"],
            beside["derived"]["per_channel"]["1"]["loss_exact_pct"])


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class DerivedBlock(unittest.TestCase):
    """derive(), on documents assembled here so the edges can be reached."""

    def channel(self, **overrides) -> dict:
        one = {
            "profile": "power #1",
            "channel": 0,
            "device_number": 1,
            "packets": 100,
            "expected_packets": 100.0,
            "loss_pct": 0.0,
            "jitter": {"offgrid_host_s": 0.0026, "offgrid_radio_s": 9e-06},
            "signal": {"n": 100, "mean_dbm": -35.0},
            "exact_loss": {"scope": "message", "missed": 1, "sent": 100,
                           "loss_pct": 1.0},
            "violations": {"count": 0, "first": []},
        }
        one.update(overrides)
        return one

    def test_a_channel_with_no_trustworthy_counter_reports_none(self):
        # A real power meter's event counter stops when the rider coasts, so
        # it cannot be used to count messages. That must read as "no figure",
        # never as "no loss".
        derived = ant_verify.derive(
            {"channels": [self.channel(exact_loss=None)]})
        self.assertIsNone(derived["loss_exact_pct"])
        self.assertIsNone(derived["per_channel"]["0"]["loss_exact_pct"])

    def test_the_aggregate_is_a_ratio_of_sums_not_a_mean_of_ratios(self):
        # 1 of 100 and 30 of 900: pooled is 31/1000 = 3.1 %, while the mean of
        # the two ratios is 2.17 %. A mean would give a channel that heard a
        # hundred packets the same weight as one that heard nine hundred.
        derived = ant_verify.derive({"channels": [
            self.channel(),
            self.channel(channel=1, exact_loss={"scope": "message",
                                                "missed": 30, "sent": 900,
                                                "loss_pct": 100.0 * 30 / 900}),
        ]})
        self.assertAlmostEqual(derived["loss_exact_pct"], 3.1)

    def test_a_nan_loss_becomes_null_rather_than_invalid_json(self):
        # An unknown device type has no period, so there is no denominator and
        # loss_pct is a genuine NaN. NaN is not JSON.
        derived = ant_verify.derive({"channels": [
            self.channel(loss_pct=float("nan"), expected_packets=0.0),
        ]})
        self.assertIsNone(derived["per_channel"]["0"]["loss_pct"])
        self.assertFalse(
            any(isinstance(v, float) and math.isnan(v)
                for v in derived.values() if isinstance(v, float)))

    def test_timing_takes_the_worst_channel_and_not_the_mean(self):
        derived = ant_verify.derive({"channels": [
            self.channel(),
            self.channel(channel=1,
                         jitter={"offgrid_host_s": 0.004,
                                 "offgrid_radio_s": None}),
        ]})
        self.assertAlmostEqual(derived["timing_offgrid_host_ms"], 4.0)

    def test_violations_are_summed_because_any_of_them_invalidates(self):
        derived = ant_verify.derive({"channels": [
            self.channel(violations={"count": 2, "first": []}),
            self.channel(channel=1, violations={"count": 3, "first": []}),
        ]})
        self.assertEqual(derived["accumulator_violations"], 5)

    def test_unexplained_loss_comes_from_the_accounting_block(self):
        derived = ant_verify.derive({
            "channels": [self.channel()],
            "accounting": {"missing": 4, "radio_fails": 1, "unaccounted": 3,
                           "allowed_unaccounted": 2, "verdict": "",
                           "pass": False},
        })
        self.assertEqual(derived["unexplained_loss"], 3)

    def test_a_wildcard_channel_is_labelled_with_the_device_it_heard(self):
        # The configured device number is 0 (any); the one that turned up is
        # the answer to "which sensor is this figure about".
        derived = ant_verify.derive({
            "channels": [self.channel(device_number=None)],
            "identities": {"0": {"device_number": 4660, "device_type": 11}},
        })
        self.assertEqual(derived["per_channel"]["0"]["device_number"], 4660)


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class ReacquireFromIntervals(unittest.TestCase):
    """T2: reacquire_s = max(gap) - D, on synthetic interval lists only - no
    hardware and no capture needed to exercise this arithmetic."""

    def test_the_worst_gap_minus_the_dropout_is_the_reacquire_time(self):
        # A 4 Hz stream (0.25 s period) with a 10 s staged dropout: the
        # dropout itself plus 0.6 s before the receiver saw the next packet.
        intervals = [0.25, 0.26, 0.24, 10.6, 0.25, 0.25]
        self.assertAlmostEqual(
            ant_verify.reacquire_from_intervals(intervals, 10.0), 0.6)

    def test_no_intervals_is_none_not_a_negative_number(self):
        # A channel that heard at most one packet has no gap to measure.
        self.assertIsNone(ant_verify.reacquire_from_intervals([], 10.0))

    def test_it_is_the_max_gap_not_the_first_or_last(self):
        # No search for "the" dropout gap by position - the widest gap in the
        # whole run is taken to be the staged one, wherever it lands.
        intervals = [0.25, 12.0, 0.25, 0.25, 0.25]
        self.assertAlmostEqual(
            ant_verify.reacquire_from_intervals(intervals, 10.0), 2.0)

    def test_a_negative_result_is_not_clamped(self):
        # If the worst gap is smaller than the staged dropout - a wrong -D
        # given on the command line, or a channel that never actually
        # dropped - the arithmetic says so rather than hiding it behind a
        # floor of zero, which would silently mask the mismatch.
        intervals = [0.25, 0.26, 0.24]
        self.assertAlmostEqual(
            ant_verify.reacquire_from_intervals(intervals, 10.0), -9.74)


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class DropoutMode(unittest.TestCase):
    """--expect-dropout SECONDS, exercised through ChannelAnalyzer directly:
    expected_packets is reduced by the dropout, TTFP is never set by the
    analyser itself, and a reopen retransmit is excluded from the exact-loss
    disqualifier."""

    def make(self, dropout_s=10.0):
        spec = ap.PROFILES["power"]
        return ant_verify.ChannelAnalyzer(
            "power", spec["device_type"], spec["period"], None, None, 2.105,
            dropout_s=dropout_s)

    def test_dropout_seconds_reduces_expected_packets(self):
        analyzer = self.make(dropout_s=10.0)
        period_s = analyzer.period_s
        # 41 packets one period apart, i.e. ~10 s of real elapsed time with
        # nothing missing, plus the dropout is charged separately below via
        # first_t/last_t bookkeeping - simplest is to feed evenly spaced
        # packets and confirm expected_packets is smaller than the naive
        # elapsed/period+1 would give.
        t = 0.0
        payload = bytes([0x10, 0, 0, 0, 0, 0, 0, 0])
        for _ in range(41):
            analyzer.feed(t, payload)
            t += period_s
        summary = analyzer.summary(100.0, 10.0, None)
        elapsed = summary["elapsed_s"]
        naive_expected = (elapsed / period_s) + 1.0
        self.assertLess(summary["expected_packets"], naive_expected)
        self.assertAlmostEqual(
            naive_expected - summary["expected_packets"],
            10.0 / period_s, places=6)

    def test_time_to_first_packet_defaults_to_none(self):
        # Only main() sets this, from t_open/t_listen it alone has. An
        # analyser built directly - as every test here and ant_sens.py's
        # measure_rung() do - never sets it, dropout mode or not.
        analyzer = self.make(dropout_s=10.0)
        self.assertIsNone(analyzer.time_to_first_packet_s)
        summary = analyzer.summary(100.0, 10.0, None)
        self.assertIsNone(summary["time_to_first_packet_s"])


@unittest.skipIf(ant_verify is None, "pyusb is not installed")
class TheDerivedBlockFitsTheBaselineSchema(unittest.TestCase):
    """What this tool emits is what archive/benchmarks/ has to accept.

    The `derived` block beside a recorded run used to be typed out of the
    report by hand. Now that the tool emits it, the two shapes have to agree
    or the convenience becomes a second transcription step in the other
    direction.
    """

    @classmethod
    def setUpClass(cls):
        with open(SCHEMA_PATH, encoding="utf-8") as handle:
            schema = json.load(handle)
        cls.schema = schema
        cls.derived_schema = (
            schema["$defs"]["radio_run"]["properties"]["derived"])

    def live_derived(self) -> dict:
        """A derived block as a LIVE run produces one.

        Replay cannot stand in here: it has no channel events, so
        `unexplained_loss` is null, and the schema requires an integer -
        correctly, since a baseline whose holes were never accounted for is
        not a baseline. So the accounting block is supplied the way listen()
        would have supplied it.
        """
        result = replay("--channel", "0", "--device-number", str(DEVICE_A))
        result["accounting"] = {"missing": 1, "radio_fails": 1,
                                "unaccounted": 0, "allowed_unaccounted": 2,
                                "verdict": "", "pass": True}
        return ant_verify.derive(result)

    def test_it_validates_against_the_baseline_schema(self):
        errors = ant_ab.validate(self.live_derived(), self.derived_schema,
                                 root=self.schema)
        self.assertEqual(errors, [])

    def test_the_schema_admits_the_per_channel_block(self):
        # additionalProperties is false on `derived`, so this is the check
        # that says a multi-channel run can be recorded at all.
        self.assertIn("per_channel", self.derived_schema["properties"])

    def test_a_gate_reads_the_emitted_block_without_transcription(self):
        # The end of the chain: ant_verify.py's own output, dropped into a
        # radio_runs entry, read by ant_ab.py's loss gate.
        derived = self.live_derived()
        base = ant_ab.Baseline("x.json", {"radio_runs": [
            {"profile": "power", "seconds": 300, "channels": 1,
             "verify": {}, "derived": derived}]})
        value, _run = base.worst("loss_exact_pct", min_seconds=300)
        self.assertAlmostEqual(value, derived["loss_exact_pct"])


if __name__ == "__main__":
    unittest.main()
