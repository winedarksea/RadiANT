#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Tests for tools/ant_ab.py. No hardware, standard library only.

Every gate is exercised on both sides of its threshold *and* exactly on it,
because "<=" and "<" is the kind of difference that only ever shows up as an
argument about a bench result six months later. The two rules the tool exists to
enforce get tests of their own: that the loss gate reads `loss_exact_pct` and
never `loss_pct`, and that the timing gate reads `timing_offgrid_host_ms` and
never a jitter figure. And a missing field is checked to FAIL rather than pass,
which is the failure mode that would quietly make this whole tier decorative.
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import os
import unittest

import ant_ab

SCHEMA_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "archive", "benchmarks",
                           "baseline.schema.json")

with open(SCHEMA_PATH, "r", encoding="utf-8") as _handle:
    SCHEMA = json.load(_handle)

GATES_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "ab_gates.toml")

SHA_A = "a" * 64
SHA_B = "b" * 64


def baseline(backend="sdk_ant", **overrides):
    """A complete, schema-valid sitting. Tests mutate copies of this."""
    data = {
        "schema_version": 1,
        "meta": {
            "recorded": "2026-08-15",
            "backend": backend,
            "git_describe": "v0.8.0-3-gdeadbee",
            "board": "adafruit_feather_nrf52840/nrf52840/uf2",
            "serial_suffix": "1234",
        },
        "rig": {
            "transmitter": {"kind": "sim_firmware",
                            "board": "nrf54l15dk/nrf54l15/cpuapp"},
            "receiver": {"board": "adafruit_feather_nrf52840/nrf52840/uf2"},
            "link": {"kind": "inline_attenuator", "attenuation_db": 30.0,
                     "rf_channel": 57},
        },
        "radio_runs": [{
            "profile": "power",
            "seconds": 300,
            "channels": 1,
            "verify": {},
            "derived": {
                "loss_pct": 0.50,
                "loss_exact_pct": 0.40,
                "unexplained_loss": 0,
                "accumulator_violations": 0,
                "timing_offgrid_host_ms": 2.60,
                "timing_offgrid_radio_ms": 0.009,
                "time_to_first_packet_s": 1.20,
                "reacquire_s": 0.90,
            },
        }],
        "usb_runs": [{
            "label": "legacy",
            "bench": {},
            "derived": {"latency_p50_ms": 0.361, "msgs_per_s": 3334},
        }],
        "conformance": {"antser_path": "archive/captures/serial/x.antser",
                        "sha256": SHA_A},
        "sensitivity": {
            "method": "inline_attenuator",
            "seconds_per_step": 60,
            "steps": [{"attenuation_db": 60, "loss_exact_pct": 0.4},
                      {"attenuation_db": 80, "loss_exact_pct": 12.0}],
            "loss5pct_attenuation_db": 72.0,
        },
        "ack_data": {"attempts": 200, "successes": 200, "success_pct": 100.0},
    }
    data.update(overrides)
    return data


def pair(mutate_b=None):
    a = ant_ab.Baseline("a.json", baseline("sdk_ant"))
    data_b = baseline("core")
    data_b["conformance"]["sha256"] = SHA_A   # identical transcript by default
    if mutate_b is not None:
        mutate_b(data_b)
    return a, ant_ab.Baseline("b.json", data_b)


def gates():
    import tomllib
    with open(GATES_PATH, "rb") as handle:
        return tomllib.load(handle)


def verdicts(results):
    return {result.name: result.verdict for result in results}


class SchemaValidator(unittest.TestCase):
    def test_a_complete_baseline_validates(self):
        self.assertEqual(ant_ab.validate(baseline(), SCHEMA), [])

    def test_a_missing_required_property_is_named(self):
        data = baseline()
        del data["meta"]["board"]
        errors = ant_ab.validate(data, SCHEMA)
        self.assertEqual(len(errors), 1)
        self.assertIn("board", errors[0])

    def test_an_unexpected_property_is_named(self):
        data = baseline()
        data["meta"]["notes"] = "should have gone in the top-level notes"
        errors = ant_ab.validate(data, SCHEMA)
        self.assertIn("unexpected property 'notes'", errors[0])

    def test_a_wrong_type_is_caught(self):
        data = baseline()
        data["radio_runs"][0]["derived"]["loss_exact_pct"] = "0.4"
        errors = ant_ab.validate(data, SCHEMA)
        self.assertIn("expected number", errors[0])

    def test_a_bad_enum_value_is_caught(self):
        data = baseline()
        data["meta"]["backend"] = "libant"
        self.assertTrue(ant_ab.validate(data, SCHEMA))

    def test_the_sha256_pattern_is_enforced(self):
        data = baseline()
        data["conformance"]["sha256"] = "not a hash"
        errors = ant_ab.validate(data, SCHEMA)
        self.assertIn("does not match", errors[0])

    def test_a_boolean_is_not_an_integer(self):
        # bool subclasses int in Python, and a validator that forgets it
        # accepts `"unexplained_loss": true` as zero holes.
        data = baseline()
        data["radio_runs"][0]["derived"]["unexplained_loss"] = True
        self.assertTrue(ant_ab.validate(data, SCHEMA))

    def test_a_negative_count_is_caught_by_the_minimum(self):
        data = baseline()
        data["radio_runs"][0]["derived"]["unexplained_loss"] = -1
        self.assertTrue(ant_ab.validate(data, SCHEMA))

    def test_a_sensitivity_sweep_needs_more_than_one_point(self):
        data = baseline()
        data["sensitivity"]["steps"] = data["sensitivity"]["steps"][:1]
        errors = ant_ab.validate(data, SCHEMA)
        self.assertIn("at least 2", errors[0])

    def test_an_unimplemented_keyword_raises_rather_than_passing(self):
        with self.assertRaises(ant_ab.SchemaError):
            ant_ab.validate({}, {"type": "object", "oneOf": []})


class Sitting(unittest.TestCase):
    def test_one_sitting_is_accepted(self):
        a, b = pair()
        self.assertEqual(ant_ab.check_sitting(gates()["sitting"], [a, b]), [])

    def test_two_dates_are_refused(self):
        def mutate(data):
            data["meta"]["recorded"] = "2026-08-16"
        a, b = pair(mutate)
        problems = ant_ab.check_sitting(gates()["sitting"], [a, b])
        self.assertEqual(len(problems), 1)
        self.assertIn("different sittings", problems[0])

    def test_a_datetime_on_the_same_day_is_still_one_sitting(self):
        def mutate(data):
            data["meta"]["recorded"] = "2026-08-15T16:40:00Z"
        a, b = pair(mutate)
        self.assertEqual(ant_ab.check_sitting(gates()["sitting"], [a, b]), [])

    def test_a_changed_rig_is_refused(self):
        def mutate(data):
            data["rig"]["link"]["attenuation_db"] = 20.0
        a, b = pair(mutate)
        problems = ant_ab.check_sitting(gates()["sitting"], [a, b])
        self.assertIn("different rigs", problems[0])

    def test_aba_with_agreeing_reference_runs_is_readable(self):
        a, _ = pair()
        a2 = ant_ab.Baseline("a2.json", copy.deepcopy(a.data))
        a2.data["radio_runs"][0]["derived"]["loss_exact_pct"] = 0.60
        self.assertIsNone(ant_ab.check_repeat_a(gates()["sitting"], a, a2))

    def test_aba_with_disagreeing_reference_runs_voids_the_sitting(self):
        a, _ = pair()
        a2 = ant_ab.Baseline("a2.json", copy.deepcopy(a.data))
        a2.data["radio_runs"][0]["derived"]["loss_exact_pct"] = 1.10
        void = ant_ab.check_repeat_a(gates()["sitting"], a, a2)
        self.assertIn("void", void)

    def test_aba_at_exactly_the_allowed_spread_still_passes(self):
        a, _ = pair()
        a2 = ant_ab.Baseline("a2.json", copy.deepcopy(a.data))
        limit = gates()["sitting"]["repeat_a_max_delta_pp"]
        a2.data["radio_runs"][0]["derived"]["loss_exact_pct"] = 0.40 + limit
        self.assertIsNone(ant_ab.check_repeat_a(gates()["sitting"], a, a2))

    def test_aba_needs_the_same_backend_first_and_last(self):
        a, b = pair()
        self.assertIn("same backend",
                      ant_ab.check_repeat_a(gates()["sitting"], a, b))


class Gates(unittest.TestCase):
    def evaluate(self, mutate_b=None, conformance=None):
        a, b = pair(mutate_b)
        return ant_ab.evaluate(gates(), a, b, conformance)

    def name(self, results, needle):
        return next(r for r in results if needle in r.name)

    # -- conformance --------------------------------------------------------

    def test_identical_transcript_hashes_pass(self):
        self.assertEqual(self.name(self.evaluate(), "conformance").verdict,
                         ant_ab.PASS)

    def test_different_transcript_hashes_fail(self):
        def mutate(data):
            data["conformance"]["sha256"] = SHA_B
        self.assertEqual(
            self.name(self.evaluate(mutate), "conformance").verdict,
            ant_ab.FAIL)

    def test_a_missing_conformance_block_fails_and_names_it(self):
        def mutate(data):
            del data["conformance"]
        result = self.name(self.evaluate(mutate), "conformance")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("conformance", result.detail)

    def test_a_conformance_comparison_overrides_the_hashes(self):
        override = {"byte_identical": False, "differing_records": 2,
                    "differing_cases": ["4d-request-version-0/valid"],
                    "a": {"sha256": SHA_A}, "b": {"sha256": SHA_B}}
        result = self.name(self.evaluate(conformance=override), "conformance")
        self.assertEqual(result.verdict, ant_ab.FAIL)

    # -- loss ---------------------------------------------------------------

    def test_loss_within_the_delta_passes(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["loss_exact_pct"] = 0.55
        self.assertEqual(self.name(self.evaluate(mutate), "loss").verdict,
                         ant_ab.PASS)

    def test_loss_exactly_at_the_delta_passes(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["loss_exact_pct"] = 0.60
        self.assertEqual(self.name(self.evaluate(mutate), "loss").verdict,
                         ant_ab.PASS)

    def test_loss_past_the_delta_fails(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["loss_exact_pct"] = 0.61
        self.assertEqual(self.name(self.evaluate(mutate), "loss").verdict,
                         ant_ab.FAIL)

    def test_loss_over_the_absolute_ceiling_fails_even_if_a_was_worse(self):
        # A backend can be within 0.2 pp of a reference run that was itself
        # having a bad afternoon; 1.5 % is the acceptance ceiling regardless.
        a, b = pair()
        a.data["radio_runs"][0]["derived"]["loss_exact_pct"] = 1.45
        b.data["radio_runs"][0]["derived"]["loss_exact_pct"] = 1.55
        result = self.name(ant_ab.evaluate(gates(), a, b), "loss")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("ceiling", result.detail)

    def test_the_wall_clock_loss_line_is_never_read(self):
        # loss_pct passing while loss_exact_pct fails must fail. If this test
        # ever goes green by accident, the gate is reading the wrong line - the
        # exact mistake docs/testing.md warns about.
        def mutate(data):
            data["radio_runs"][0]["derived"]["loss_pct"] = 0.01
            data["radio_runs"][0]["derived"]["loss_exact_pct"] = 3.0
        self.assertEqual(self.name(self.evaluate(mutate), "loss").verdict,
                         ant_ab.FAIL)

    def test_a_suspiciously_low_loss_warns_and_does_not_fail(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["loss_exact_pct"] = 0.01
        result = self.name(self.evaluate(mutate), "loss")
        self.assertEqual(result.verdict, ant_ab.PASS)
        self.assertTrue(any("suspicious" in w for w in result.warnings))

    def test_a_short_run_does_not_count_as_a_300_s_run(self):
        def mutate(data):
            data["radio_runs"][0]["seconds"] = 60
        result = self.name(self.evaluate(mutate), "loss")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("300", result.detail)

    def test_a_missing_loss_field_fails_naming_the_field(self):
        def mutate(data):
            del data["radio_runs"][0]["derived"]["loss_exact_pct"]
        result = self.name(self.evaluate(mutate), "loss")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("loss_exact_pct", result.detail)

    def test_the_worst_run_is_the_one_gated(self):
        def mutate(data):
            good = copy.deepcopy(data["radio_runs"][0])
            good["derived"]["loss_exact_pct"] = 0.10
            good["profile"] = "csc"
            data["radio_runs"][0]["derived"]["loss_exact_pct"] = 0.90
            data["radio_runs"].append(good)
        self.assertEqual(self.name(self.evaluate(mutate), "loss").verdict,
                         ant_ab.FAIL)

    # -- unexplained loss and accumulators ----------------------------------

    def test_one_unexplained_hole_fails(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["unexplained_loss"] = 1
        self.assertEqual(
            self.name(self.evaluate(mutate), "unexplained").verdict,
            ant_ab.FAIL)

    def test_an_unexplained_hole_in_the_reference_voids_the_gate_too(self):
        a, b = pair()
        a.data["radio_runs"][0]["derived"]["unexplained_loss"] = 2
        result = self.name(ant_ab.evaluate(gates(), a, b), "unexplained")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("invalidates the sitting", result.detail)

    def test_an_accumulator_violation_fails(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["accumulator_violations"] = 1
        self.assertEqual(
            self.name(self.evaluate(mutate), "accumulator").verdict,
            ant_ab.FAIL)

    # -- timing -------------------------------------------------------------

    def test_timing_at_exactly_the_ratio_passes(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["timing_offgrid_host_ms"] = 3.25
        self.assertEqual(self.name(self.evaluate(mutate), "timing").verdict,
                         ant_ab.PASS)

    def test_timing_past_the_ratio_fails(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["timing_offgrid_host_ms"] = 3.30
        self.assertEqual(self.name(self.evaluate(mutate), "timing").verdict,
                         ant_ab.FAIL)

    def test_a_tiny_absolute_change_passes_on_the_floor(self):
        a, b = pair()
        a.data["radio_runs"][0]["derived"]["timing_offgrid_host_ms"] = 0.009
        b.data["radio_runs"][0]["derived"]["timing_offgrid_host_ms"] = 0.30
        self.assertEqual(
            self.name(ant_ab.evaluate(gates(), a, b), "timing").verdict,
            ant_ab.PASS)

    def test_a_missing_radio_clock_figure_warns_about_lib_config(self):
        def mutate(data):
            del data["radio_runs"][0]["derived"]["timing_offgrid_radio_ms"]
        result = self.name(self.evaluate(mutate), "timing")
        self.assertEqual(result.verdict, ant_ab.PASS)
        self.assertTrue(any("0xE0" in w for w in result.warnings))

    def test_no_gate_reads_a_jitter_field(self):
        # The tool must not grow a jitter reader by accident: one lost packet
        # turns a 250 ms gap into 500 ms, so gating on it gates on loss twice.
        with open(ant_ab.__file__, "r", encoding="utf-8") as handle:
            source = handle.read()
        code = "\n".join(line for line in source.splitlines()
                         if not line.lstrip().startswith("#"))
        # The word appears only in prose explaining why it is not read.
        self.assertNotIn('"jitter"', code)
        self.assertNotIn("['jitter']", code)

    # -- acquisition --------------------------------------------------------

    def test_acquisition_inside_both_limits_passes(self):
        self.assertEqual(self.name(self.evaluate(), "first packet").verdict,
                         ant_ab.PASS)

    def test_acquisition_over_the_absolute_limit_fails_even_within_the_ratio(self):
        a, b = pair()
        a.data["radio_runs"][0]["derived"]["time_to_first_packet_s"] = 4.0
        b.data["radio_runs"][0]["derived"]["time_to_first_packet_s"] = 5.5
        self.assertEqual(
            self.name(ant_ab.evaluate(gates(), a, b), "first packet").verdict,
            ant_ab.FAIL)

    def test_acquisition_exactly_at_five_seconds_passes(self):
        a, b = pair()
        a.data["radio_runs"][0]["derived"]["time_to_first_packet_s"] = 4.0
        b.data["radio_runs"][0]["derived"]["time_to_first_packet_s"] = 5.0
        self.assertEqual(
            self.name(ant_ab.evaluate(gates(), a, b), "first packet").verdict,
            ant_ab.PASS)

    # -- sensitivity --------------------------------------------------------

    def test_sensitivity_within_one_db_passes(self):
        def mutate(data):
            data["sensitivity"]["loss5pct_attenuation_db"] = 71.0
        self.assertEqual(
            self.name(self.evaluate(mutate), "sensitivity").verdict,
            ant_ab.PASS)

    def test_sensitivity_more_than_one_db_worse_fails(self):
        def mutate(data):
            data["sensitivity"]["loss5pct_attenuation_db"] = 70.5
        self.assertEqual(
            self.name(self.evaluate(mutate), "sensitivity").verdict,
            ant_ab.FAIL)

    def test_better_sensitivity_is_not_a_failure(self):
        def mutate(data):
            data["sensitivity"]["loss5pct_attenuation_db"] = 80.0
        self.assertEqual(
            self.name(self.evaluate(mutate), "sensitivity").verdict,
            ant_ab.PASS)

    def test_two_different_methods_are_not_comparable(self):
        def mutate(data):
            data["sensitivity"]["method"] = "fixed_open_air"
        result = self.name(self.evaluate(mutate), "sensitivity")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("not comparable", result.detail)

    def test_a_missing_sensitivity_block_fails_because_it_is_required(self):
        def mutate(data):
            del data["sensitivity"]
        self.assertEqual(
            self.name(self.evaluate(mutate), "sensitivity").verdict,
            ant_ab.FAIL)

    # -- the transmit-power ladder, where the sign is the other way round ----

    def ladder(self, a_dbm: float, b_dbm: float, spread: float | None = 0.3):
        """A tx_ladder sitting: the master's power stepped down to the knee.

        Both baselines get the ladder, because a comparison across two methods
        is refused before any number is read.
        """
        def block(dbm):
            got = {
                "method": "tx_ladder",
                "seconds_per_step": 60,
                "steps": [{"tx_power_dbm": -12, "loss_exact_pct": 0.4,
                           "loss_basis": "event_counter"},
                          {"tx_power_dbm": -20, "loss_exact_pct": 12.0,
                           "loss_basis": "event_counter"}],
                "loss5pct_tx_power_dbm": dbm,
            }
            if spread is not None:
                got["repeat_spread_db"] = spread
            return got

        a, b = pair(lambda data: data.update({"sensitivity": block(b_dbm)}))
        a.data["sensitivity"] = block(a_dbm)
        return ant_ab.evaluate(gates(), a, b)

    def test_a_quieter_transmitter_at_the_knee_is_better_hearing(self):
        """The sign that is silent when it is wrong.

        More attenuation at the 5 % point means a better receiver, and so does
        LESS transmit power. B needing the master 3 dB quieter than A did is B
        hearing 3 dB further, and it must read as a pass with B in bold - not
        as a 3 dB regression, which is what a single lower-is-better rule
        applied to every sensitivity field would report.
        """
        result = self.name(self.ladder(-14.0, -17.0), "sensitivity")
        self.assertEqual(result.verdict, ant_ab.PASS)
        self.assertEqual(result.better, "b")

    def test_a_ladder_that_needs_a_louder_transmitter_fails(self):
        result = self.name(self.ladder(-17.0, -14.0), "sensitivity")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertEqual(result.better, "a")

    def test_a_ladder_within_one_db_passes(self):
        self.assertEqual(
            self.name(self.ladder(-17.0, -16.2), "sensitivity").verdict,
            ant_ab.PASS)

    def test_a_ladder_that_cannot_repeat_cannot_grade_the_gate(self):
        """Phase 0's own gate, applied at the point the number gets used.

        A ladder whose two passes disagreed by 2 dB has not measured a 1 dB
        difference; it has measured its own noise. The comparison is refused
        even though the two numbers are within the threshold, because the
        threshold is narrower than the instrument that read them.
        """
        result = self.name(self.ladder(-14.0, -14.2, spread=2.0),
                           "sensitivity")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("instrument cannot read this comparison", result.detail)

    def test_a_single_ladder_warns_that_its_repeat_is_unknown(self):
        result = self.name(self.ladder(-14.0, -14.2, spread=None),
                           "sensitivity")
        self.assertEqual(result.verdict, ant_ab.PASS)
        self.assertTrue(any("--repeat 2" in w for w in result.warnings))

    def test_a_ladder_against_an_attenuator_is_refused(self):
        # The whole point of recording the method: the two carry different
        # systematic offsets and neither converts into the other.
        def mutate(data):
            data["sensitivity"] = {
                "method": "tx_ladder",
                "steps": [{"tx_power_dbm": -12, "loss_exact_pct": 0.4},
                          {"tx_power_dbm": -20, "loss_exact_pct": 12.0}],
                "loss5pct_tx_power_dbm": -14.0,
            }
        result = self.name(self.evaluate(mutate), "sensitivity")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("not comparable", result.detail)

    def test_the_rig_refuses_a_ladder_against_a_distance_run_before_any_gate(self):
        """Why the method is recorded in the RIG and not only in the result.

        gate_sensitivity() already refuses two different methods, but it does so
        one gate deep, in a detail line, after nine other gates have printed
        verdicts about a sitting that should never have been compared at all.
        The rig is what `check_sitting()` reads, and a refusal there stops the
        whole comparison with the reason at the top.
        """
        a, b = pair(lambda data:
                    data["rig"]["link"].update({"sensitivity_method":
                                                "tx_ladder"}))
        a.data["rig"]["link"]["sensitivity_method"] = "fixed_open_air"

        problems = ant_ab.check_sitting(gates()["sitting"], [a, b])
        self.assertEqual(len(problems), 1)
        self.assertIn("different rigs", problems[0])

    def test_two_ladders_on_one_rig_are_compared_normally(self):
        # The other half: recording the method must not make every sitting
        # refuse itself.
        a, b = pair(lambda data:
                    data["rig"]["link"].update({"sensitivity_method":
                                                "tx_ladder"}))
        a.data["rig"]["link"]["sensitivity_method"] = "tx_ladder"
        self.assertEqual(ant_ab.check_sitting(gates()["sitting"], [a, b]), [])

    def test_a_recorded_method_validates_against_the_schema(self):
        data = baseline()
        data["rig"]["link"]["sensitivity_method"] = "tx_ladder"
        self.assertEqual(ant_ab.validate(data, SCHEMA), [])

        data["rig"]["link"]["sensitivity_method"] = "guesswork"
        errors = ant_ab.validate(data, SCHEMA)
        self.assertEqual(len(errors), 1)
        self.assertIn("must be one of", errors[0])

    def test_a_ladder_that_never_reached_the_knee_is_not_a_pass(self):
        # ant_sens.py writes null rather than extrapolating past its last rung.
        # That is a refusal to invent an answer, and it must not read as an
        # absent optional field.
        result = self.name(self.ladder(-14.0, None), "sensitivity")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("refusing to extrapolate", result.detail)

    # -- scale, which is absolute -------------------------------------------

    def test_a_missing_scale_block_skips_and_never_passes(self):
        result = self.name(self.evaluate(), "32-channel")
        self.assertEqual(result.verdict, ant_ab.SKIP)
        self.assertIn("libant.a", result.detail)

    def test_scale_within_half_a_point_passes(self):
        def mutate(data):
            data["scale"] = {"channels_tracked": 32,
                             "per_channel_loss_pct": 0.85,
                             "single_channel_loss_pct": 0.40}
        self.assertEqual(self.name(self.evaluate(mutate), "32-channel").verdict,
                         ant_ab.PASS)

    def test_scale_past_half_a_point_fails(self):
        def mutate(data):
            data["scale"] = {"channels_tracked": 32,
                             "per_channel_loss_pct": 0.95,
                             "single_channel_loss_pct": 0.40}
        self.assertEqual(self.name(self.evaluate(mutate), "32-channel").verdict,
                         ant_ab.FAIL)

    def test_scale_with_too_few_channels_fails(self):
        def mutate(data):
            data["scale"] = {"channels_tracked": 8,
                             "per_channel_loss_pct": 0.40,
                             "single_channel_loss_pct": 0.40}
        self.assertEqual(self.name(self.evaluate(mutate), "32-channel").verdict,
                         ant_ab.FAIL)

    # -- ack data -----------------------------------------------------------

    def test_ack_at_exactly_99_percent_passes(self):
        def mutate(data):
            data["ack_data"] = {"attempts": 100, "successes": 99}
        self.assertEqual(self.name(self.evaluate(mutate), "ack-data").verdict,
                         ant_ab.PASS)

    # -- coexistence --------------------------------------------------------
    #
    # Tested against gate_coexistence() directly rather than through
    # evaluate(), because the block is `enabled = false` in ab_gates.toml until
    # P3 measures its thresholds - so evaluate() correctly skips it, and a test
    # that went through evaluate() would be asserting nothing while looking
    # like it asserted something.

    COEX_CFG = {"required": False, "max_delta_pp": 0.5,
                "absolute_ceiling_pct": 1.5,
                "arbiter_only_max_delta_pp": 1.5,
                "min_sweep_rate_ratio": 0.5}

    def coex(self, block):
        a, b = pair()
        if block is not None:
            b.data["coexistence"] = block
        return {r.name: r for r in
                ant_ab.gate_coexistence(self.COEX_CFG, a, b)}

    def test_coexistence_absent_is_a_skip_not_a_pass(self):
        results = self.coex(None)
        self.assertEqual(results["coexistence (second stack on)"].verdict,
                         ant_ab.SKIP)

    def test_coexistence_within_half_a_point_passes(self):
        results = self.coex({"second_stack": "ble",
                             "loss_direct_pct": 0.40,
                             "loss_second_stack_off_pct": 0.45,
                             "loss_second_stack_on_pct": 0.85,
                             "sweep_sets_per_s_off": 4.0,
                             "sweep_sets_per_s_on": 3.0})
        for result in results.values():
            self.assertEqual(result.verdict, ant_ab.PASS, result.name)

    def test_coexistence_beyond_half_a_point_fails(self):
        results = self.coex({"second_stack": "thread_med",
                             "loss_direct_pct": 0.40,
                             "loss_second_stack_off_pct": 0.45,
                             "loss_second_stack_on_pct": 1.10,
                             "sweep_sets_per_s_off": 4.0,
                             "sweep_sets_per_s_on": 3.0})
        self.assertEqual(results["coexistence (second stack on)"].verdict,
                         ant_ab.FAIL)

    def test_coexistence_within_half_a_point_of_a_bad_afternoon_still_fails(self):
        # 1.60 - 1.20 is inside 0.5 pp and 1.60 % is over the 1.5 % ceiling.
        # The delta gate alone would pass this, which is the whole reason
        # loss_exact carries a ceiling too.
        results = self.coex({"second_stack": "ble",
                             "loss_direct_pct": 0.40,
                             "loss_second_stack_off_pct": 1.20,
                             "loss_second_stack_on_pct": 1.60,
                             "sweep_sets_per_s_off": 4.0,
                             "sweep_sets_per_s_on": 3.0})
        self.assertEqual(results["coexistence (second stack on)"].verdict,
                         ant_ab.FAIL)

    def test_the_arbiter_paying_for_itself_is_the_exit_criterion(self):
        # The second stack is off in both numbers, so this is the timeslot
        # machinery's own cost. Over the limit, the combined build is abandoned
        # for the two-box handoff rather than tuned.
        results = self.coex({"second_stack": "none",
                             "loss_direct_pct": 0.40,
                             "loss_second_stack_off_pct": 2.10,
                             "loss_second_stack_on_pct": 2.20,
                             "sweep_sets_per_s_off": 4.0,
                             "sweep_sets_per_s_on": 3.0})
        result = results["arbiter cost (no second stack)"]
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("7.3", result.detail)

    def test_a_sweep_that_stopped_is_not_a_sweep_that_gave_way(self):
        # ADR 0013 makes the sweep the elastic consumer, so it IS slower. The
        # bound is what separates "gave way" from "stopped", which no other
        # number in this file can see.
        results = self.coex({"second_stack": "thread_sed",
                             "loss_direct_pct": 0.40,
                             "loss_second_stack_off_pct": 0.45,
                             "loss_second_stack_on_pct": 0.85,
                             "sweep_sets_per_s_off": 4.0,
                             "sweep_sets_per_s_on": 0.4})
        self.assertEqual(results["sweep rate under contention"].verdict,
                         ant_ab.FAIL)

    def test_ack_below_99_percent_fails(self):
        def mutate(data):
            data["ack_data"] = {"attempts": 1000, "successes": 989}
        self.assertEqual(self.name(self.evaluate(mutate), "ack-data").verdict,
                         ant_ab.FAIL)

    def test_ack_more_than_a_point_below_the_reference_fails(self):
        a, b = pair()
        a.data["ack_data"] = {"attempts": 100, "successes": 100}
        b.data["ack_data"] = {"attempts": 1000, "successes": 985}
        self.assertEqual(
            self.name(ant_ab.evaluate(gates(), a, b), "ack-data").verdict,
            ant_ab.FAIL)

    def test_ack_success_is_derived_when_the_percentage_is_absent(self):
        def mutate(data):
            data["ack_data"] = {"attempts": 200, "successes": 199}
        result = self.name(self.evaluate(mutate), "ack-data")
        self.assertEqual(result.b, "99.50 %")

    # -- USB ----------------------------------------------------------------

    def test_usb_latency_within_ten_percent_passes(self):
        def mutate(data):
            data["usb_runs"][0]["derived"]["latency_p50_ms"] = 0.39
        self.assertEqual(self.name(self.evaluate(mutate), "USB").verdict,
                         ant_ab.PASS)

    def test_usb_latency_far_past_the_ratio_fails(self):
        def mutate(data):
            data["usb_runs"][0]["derived"]["latency_p50_ms"] = 0.90
        self.assertEqual(self.name(self.evaluate(mutate), "USB").verdict,
                         ant_ab.FAIL)

    def test_a_missing_usb_run_fails_naming_it(self):
        def mutate(data):
            data["usb_runs"] = []
        result = self.name(self.evaluate(mutate), "USB")
        self.assertEqual(result.verdict, ant_ab.FAIL)
        self.assertIn("usb_runs", result.detail)


class Reporting(unittest.TestCase):
    def test_the_table_has_the_backends_md_shape(self):
        a, b = pair()
        results = ant_ab.evaluate(gates(), a, b)
        table = ant_ab.render(results, a.label, b.label)
        lines = table.splitlines()
        self.assertTrue(lines[0].startswith("|"))
        self.assertIn("sdk-ant", lines[0])
        self.assertIn("core", lines[0])
        # Second line is the markdown rule, as in docs/backends.md.
        self.assertRegex(lines[1], r"^\|-+\|")
        self.assertEqual(len(lines), len(results) + 2)

    def test_the_better_column_is_emboldened(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["loss_exact_pct"] = 0.20
        a, b = pair(mutate)
        table = ant_ab.render(ant_ab.evaluate(gates(), a, b), "sdk-ant",
                              "core")
        self.assertIn("**0.200 %**", table)

    def report(self, a, b):
        # The report is checked, not printed: its terminal output would
        # otherwise interleave with the test runner's.
        with contextlib.redirect_stdout(io.StringIO()):
            return ant_ab.report(ant_ab.evaluate(gates(), a, b),
                                 a.label, b.label)

    def test_a_clean_sitting_reports_a_pass(self):
        a, b = pair()
        self.assertTrue(self.report(a, b))

    def test_one_failure_makes_the_whole_report_fail(self):
        def mutate(data):
            data["radio_runs"][0]["derived"]["accumulator_violations"] = 3
        a, b = pair(mutate)
        self.assertFalse(self.report(a, b))

    def test_a_skipped_gate_is_reported_in_its_own_paragraph(self):
        a, b = pair()
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            ant_ab.report(ant_ab.evaluate(gates(), a, b), a.label, b.label)
        text = buffer.getvalue()
        self.assertIn("COULD NOT BE EVALUATED", text)
        self.assertIn("SKIP 32-channel per-channel loss", text)


class GatesFile(unittest.TestCase):
    """The thresholds themselves, checked against docs/testing.md's table."""

    def test_the_shipped_thresholds_are_the_documented_ones(self):
        cfg = gates()["gates"]
        self.assertEqual(cfg["loss_exact"]["max_delta_pp"], 0.2)
        self.assertEqual(cfg["loss_exact"]["min_seconds"], 300)
        self.assertEqual(cfg["loss_exact"]["absolute_ceiling_pct"], 1.5)
        self.assertEqual(cfg["unexplained_loss"]["max"], 0)
        self.assertEqual(cfg["accumulator_violations"]["max"], 0)
        self.assertEqual(cfg["timing"]["max_ratio"], 1.25)
        self.assertEqual(cfg["acquisition"]["max_ratio"], 1.5)
        self.assertEqual(cfg["acquisition"]["max_absolute_s"], 5.0)
        self.assertEqual(cfg["sensitivity"]["max_delta_db"], 1.0)
        self.assertEqual(cfg["scale"]["max_delta_pp"], 0.5)
        self.assertEqual(cfg["scale"]["expect_channels"], 32)
        self.assertEqual(cfg["ack_data"]["min_success_pct"], 99.0)
        self.assertEqual(cfg["ack_data"]["max_delta_pp"], 1.0)
        self.assertEqual(cfg["usb_latency"]["max_ratio"], 1.1)
        self.assertTrue(cfg["conformance"]["require_byte_identical"])

    def test_no_case_is_allowed_to_differ_out_of_the_box(self):
        # An allowance added speculatively excuses a difference nobody looked
        # at. The version string will need one; it should be added when the
        # first real A/B runs, in a reviewable commit.
        self.assertEqual(gates()["gates"]["conformance"]
                         ["allowed_differing_cases"], [])

    def test_every_gate_the_code_knows_exists_in_the_file(self):
        known = {"conformance", "loss_exact", "unexplained_loss",
                 "accumulator_violations", "timing", "acquisition",
                 "sensitivity", "scale", "ack_data", "usb_latency",
                 "coexistence"}
        self.assertEqual(set(gates()["gates"]), known)

    def test_the_coexistence_gate_is_off_until_it_is_measured(self):
        # It is written and evaluated, and its thresholds are placeholders
        # until P3 measures them on the nRF54L15 DK. `enabled = false` is what
        # keeps it out of the table meanwhile - a gate that runs against a
        # guessed threshold is worse than one that does not run, because it
        # reports PASS.
        #
        # The DEFAULT is what this pins, not the mechanism: gate_coexistence()
        # is tested directly below, so turning the block on is a one-line
        # change with a working gate behind it.
        cfg = gates()["gates"]["coexistence"]
        self.assertFalse(cfg["enabled"])
        self.assertEqual(cfg["max_delta_pp"], 0.5)
        self.assertEqual(cfg["arbiter_only_max_delta_pp"], 1.5)

    def test_the_two_loss_gates_measure_different_things(self):
        # docs/backends.md has stated +0.5 pp for coexistence since before the
        # arbiter existed, and [gates.loss_exact] requires 0.2 pp - which reads
        # as though the looser number had been superseded. It has not: one
        # compares two BACKENDS with nothing else on the radio, the other the
        # SAME backend with a second stack on against off. Both must pass on a
        # combined build, and this is the assertion that stops someone
        # "reconciling" them.
        cfg = gates()["gates"]
        self.assertNotEqual(cfg["loss_exact"]["max_delta_pp"],
                            cfg["coexistence"]["max_delta_pp"])


if __name__ == "__main__":
    unittest.main()
