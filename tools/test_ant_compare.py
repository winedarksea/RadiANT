#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""tools/ant_compare.py with no radio: rig validation, the statistics, every
pre-registered verdict rule, and the summary built from canned child JSON.

The verdict rules are the part that must not drift. They were written down
before any data was taken, and a rule that quietly changed shape after the
first run would be the same act as moving a threshold to fit a result.
"""

from __future__ import annotations

import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_compare as ac  # noqa: E402

RIG = Path(__file__).resolve().parent / "compare_rigs" / "2026-11-07-fixed.toml"


def minimal_raw(**round_overrides) -> dict:
    rnd = {"name": "A", "transmitters": ["TX"], "phases": ["loss"]}
    rnd.update(round_overrides)
    return {
        "meta": {"name": "t"},
        "device": [
            {"name": "Ref", "address": "usb:006", "stack": "reference"},
            {"name": "R1", "address": "usb:AAAA", "part": "nrf52840"},
            {"name": "TX", "address": "port:COM5", "part": "nrf5340net",
             "transmit": True, "device_number_base": 53000},
            {"name": "TX2", "address": "port:COM4", "part": "nrf54l15",
             "transmit": True, "device_number_base": 54000},
        ],
        "round": [rnd],
    }


class TestRigFile(unittest.TestCase):
    def test_the_shipped_rig_parses(self):
        rig = ac.load_rig(RIG)
        self.assertEqual(rig.reference.name, "Garmin")
        self.assertEqual([r.name for r in rig.rounds], ["A", "B", "C"])
        self.assertEqual(rig.round("C").repeats, {"multi": 2})
        # Round A: the idle 54L15 listens; the 5340 does not hear itself.
        names = [d.name for d in rig.receivers_for(rig.round("A"))]
        self.assertEqual(names[0], "Garmin", "the reference leads")
        self.assertIn("54L15", names)
        self.assertNotIn("5340", names)
        # Round C: both DKs transmit, so neither receives.
        names_c = [d.name for d in rig.receivers_for(rig.round("C"))]
        self.assertNotIn("5340", names_c)
        self.assertNotIn("54L15", names_c)

    def test_device_numbers_are_pinned_per_round_and_phase(self):
        rig = ac.load_rig(RIG)
        a = rig.device_number(rig.round("A"), "5340", "loss")
        b = rig.device_number(rig.round("B"), "54L15", "loss")
        self.assertTrue(53000 <= a < 54000)
        self.assertTrue(54000 <= b < 55000)
        self.assertNotEqual(rig.device_number(rig.round("A"), "5340", "loss"),
                            rig.device_number(rig.round("A"), "5340",
                                              "acquire"))

    def test_overrides_are_typed(self):
        rig = ac.load_rig(RIG, ["phases.loss.seconds=30",
                                "phases.ladder.rungs=levels"])
        self.assertEqual(rig.phase("loss")["seconds"], 30)
        self.assertEqual(rig.phase("ladder")["rungs"], "levels")

    def test_exactly_one_reference(self):
        raw = minimal_raw()
        raw["device"][1]["stack"] = "reference"
        with self.assertRaisesRegex(ac.RigError, "exactly one"):
            ac.parse_rig(raw)
        raw = minimal_raw()
        raw["device"][0]["stack"] = "radiant"
        with self.assertRaisesRegex(ac.RigError, "exactly one"):
            ac.parse_rig(raw)

    def test_the_reference_cannot_transmit(self):
        raw = minimal_raw()
        raw["device"][0]["transmit"] = True
        with self.assertRaisesRegex(ac.RigError, "must not transmit"):
            ac.parse_rig(raw)

    def test_a_transmitter_must_be_marked_and_numbered(self):
        raw = minimal_raw(transmitters=["R1"])
        with self.assertRaisesRegex(ac.RigError, "transmit = true"):
            ac.parse_rig(raw)
        raw = minimal_raw()
        del raw["device"][2]["device_number_base"]
        with self.assertRaisesRegex(ac.RigError, "device_number_base"):
            ac.parse_rig(raw)

    def test_a_tool_default_device_number_is_refused(self):
        raw = minimal_raw()
        raw["device"][2]["device_number_base"] = 14871
        with self.assertRaisesRegex(ac.RigError, "tool default"):
            ac.parse_rig(raw)

    def test_two_transmitters_cannot_share_device_numbers(self):
        raw = minimal_raw(transmitters=["TX", "TX2"], phases=["multi"],
                          multi_split={p: "TX" for p in
                                       ac.DEFAULT_MULTI_PROFILES})
        raw["device"][3]["device_number_base"] = 53000
        with self.assertRaisesRegex(ac.RigError, "used by both"):
            ac.parse_rig(raw)

    def test_a_two_transmitter_multi_needs_a_split(self):
        raw = minimal_raw(transmitters=["TX", "TX2"], phases=["multi"])
        with self.assertRaisesRegex(ac.RigError, "multi_split"):
            ac.parse_rig(raw)

    def test_a_ladder_needs_one_transmitter_with_a_part(self):
        raw = minimal_raw(transmitters=["TX", "TX2"], phases=["ladder"])
        with self.assertRaisesRegex(ac.RigError, "exactly one transmitter"):
            ac.parse_rig(raw)
        raw = minimal_raw(phases=["ladder"])
        del raw["device"][2]["part"]
        with self.assertRaisesRegex(ac.RigError, "part"):
            ac.parse_rig(raw)

    def test_identity_and_usb_are_not_round_phases(self):
        with self.assertRaisesRegex(ac.RigError, "run once"):
            ac.parse_rig(minimal_raw(phases=["usb"]))

    def test_unknown_phase_keys_are_refused_not_ignored(self):
        raw = minimal_raw()
        raw["phases"] = {"loss": {"secnds": 30}}
        with self.assertRaisesRegex(ac.RigError, "unknown keys"):
            ac.parse_rig(raw)

    def test_bad_address_is_refused(self):
        raw = minimal_raw()
        raw["device"][1]["address"] = "COM9"
        with self.assertRaises(ac.RigError):
            ac.parse_rig(raw)

    def test_explicit_receivers_must_include_the_reference(self):
        with self.assertRaisesRegex(ac.RigError, "reference"):
            ac.parse_rig(minimal_raw(receivers=["R1"]))


class TestStatistics(unittest.TestCase):
    def test_wilson_matches_a_known_interval(self):
        # 10/100: Wilson 95 % is 5.52 % .. 17.44 % (standard table value).
        p, lo, hi = ac.wilson(10, 100)
        self.assertAlmostEqual(p, 10.0)
        self.assertAlmostEqual(lo, 5.52, places=2)
        self.assertAlmostEqual(hi, 17.44, places=2)

    def test_wilson_at_zero_has_a_real_upper_bound(self):
        # The normal approximation would give [0, 0] here and claim
        # certainty from 1200 packets.
        p, lo, hi = ac.wilson(0, 1200)
        self.assertEqual(p, 0.0)
        self.assertEqual(lo, 0.0)
        self.assertGreater(hi, 0.2)
        self.assertLess(hi, 0.4)

    def test_wilson_refuses_nonsense(self):
        self.assertIsNone(ac.wilson(0, 0))
        with self.assertRaises(ValueError):
            ac.wilson(5, 4)

    def test_quantile_tolerates_censored_misses(self):
        self.assertEqual(ac.quantile([1.0, 2.0, 3.0], 0.5), 2.0)
        self.assertEqual(ac.quantile([1.0, math.inf, math.inf], 0.5),
                         math.inf)
        self.assertEqual(ac.quantile([1.0, 2.0, math.inf], 0.5), 2.0)
        self.assertIsNone(ac.quantile([], 0.5))


class TestVerdictRules(unittest.TestCase):
    def test_loss_better(self):
        v, _ = ac.verdict_rate((2, 1200), (60, 1200))
        self.assertEqual(v, ac.BETTER)

    def test_loss_equal(self):
        v, _ = ac.verdict_rate((2, 1200), (2, 1200))
        self.assertEqual(v, ac.EQUAL)
        # Pooled L1+L2 at the ~1 % floor clears the margin.
        self.assertEqual(ac.verdict_rate((24, 2400), (24, 2400))[0], ac.EQUAL)

    def test_identical_receivers_on_too_few_packets_are_not_called_equal(self):
        # The rule's power limit, pinned so nobody "fixes" it by loosening:
        # at 1 % loss one 300 s window (1200 packets) gives a Wilson upper
        # bound of ~1.74 %, past the 1.5 % non-inferiority line, so two
        # identical receivers are Inconclusive until more windows pool. At
        # the room's 5-8 % it takes far more packets still.
        self.assertEqual(ac.verdict_rate((12, 1200), (12, 1200))[0],
                         ac.INCONCLUSIVE)
        self.assertEqual(ac.verdict_rate((144, 2400), (144, 2400))[0],
                         ac.INCONCLUSIVE)

    def test_loss_worse(self):
        v, _ = ac.verdict_rate((80, 1200), (6, 1200))
        self.assertEqual(v, ac.WORSE)

    def test_loss_straddling_the_margin_is_inconclusive(self):
        # 1.0 % vs 0.5 % on 1200: the receiver's interval runs from below the
        # 1.0 pp line to above it - neither shown equal nor shown worse.
        v, detail = ac.verdict_rate((12, 1200), (6, 1200))
        self.assertEqual(v, ac.INCONCLUSIVE)
        self.assertIn("straddles", detail)

    def test_loss_missing_is_inconclusive(self):
        self.assertEqual(ac.verdict_rate(None, (6, 1200))[0], ac.INCONCLUSIVE)
        self.assertEqual(ac.verdict_rate((6, 1200), None)[0], ac.INCONCLUSIVE)

    def test_acquire_rules(self):
        ref = [2.0] * 20
        self.assertEqual(ac.verdict_acquire([2.1] * 20, ref)[0], ac.EQUAL)
        self.assertEqual(ac.verdict_acquire([0.5] * 20, ref)[0], ac.BETTER)
        self.assertEqual(ac.verdict_acquire([3.0] * 20, ref)[0], ac.WORSE)
        # Lower median but overlapping IQRs is only Equal, not Better.
        spread_rx = [0.5, 1.0, 1.5, 1.9, 2.5, 3.0] * 3
        spread_ref = [1.0, 1.5, 2.0, 2.5, 3.0, 3.5] * 3
        self.assertEqual(ac.verdict_acquire(spread_rx, spread_ref)[0],
                         ac.EQUAL)

    def test_acquire_with_too_many_misses_is_inconclusive(self):
        v, detail = ac.verdict_acquire([None] * 15 + [1.0] * 5, [2.0] * 20)
        self.assertEqual(v, ac.INCONCLUSIVE)
        self.assertIn("5 of 20 cycles; the rule needs at least 10", detail)
        self.assertEqual(ac.verdict_acquire(None, [2.0] * 20)[0],
                         ac.INCONCLUSIVE)

    def test_misses_count_against_the_median_rather_than_vanishing(self):
        # 8 misses of 20 drags the median up; dropping them would read 1.0 s.
        rx = [None] * 8 + [1.0] * 12
        stats = ac.acquire_stats(rx)
        self.assertEqual(stats["misses"], 8)
        self.assertEqual(stats["q3"], math.inf)

    def test_knee_rules(self):
        ref = {"knee_dbm": -20.0, "dial_trustworthy": True}
        self.assertEqual(ac.verdict_knee({"knee_dbm": -20.8,
                                          "dial_trustworthy": True}, ref)[0],
                         ac.EQUAL)
        self.assertEqual(ac.verdict_knee({"knee_dbm": -22.0,
                                          "dial_trustworthy": True}, ref)[0],
                         ac.BETTER)
        self.assertEqual(ac.verdict_knee({"knee_dbm": -18.5,
                                          "dial_trustworthy": True}, ref)[0],
                         ac.WORSE)

    def test_knee_without_a_trustworthy_dial_or_knee_is_inconclusive(self):
        ref = {"knee_dbm": -20.0, "dial_trustworthy": True}
        self.assertEqual(ac.verdict_knee({"knee_dbm": -25.0,
                                          "dial_trustworthy": False}, ref)[0],
                         ac.INCONCLUSIVE)
        v, detail = ac.verdict_knee({"knee_dbm": None,
                                     "dial_trustworthy": True,
                                     "reason": "below the bottom"}, ref)
        self.assertEqual(v, ac.INCONCLUSIVE)
        self.assertIn("never reached", detail)

    def test_a_receiver_without_rssi_inherits_the_ladder_dial(self):
        no_rssi = {"slope": None, "reason": "fewer than three rungs"}
        results = {
            "Garmin": {"knee_dbm": -20.0, "dial_trustworthy": False,
                       "rssi_slope": no_rssi},
            "Feather": {"knee_dbm": -20.5, "dial_trustworthy": True,
                        "rssi_slope": {"slope": 1.02}},
        }
        eff = ac.effective_dial(results)
        self.assertTrue(eff["Garmin"]["dial_trustworthy"])
        self.assertEqual(ac.verdict_knee(eff["Feather"], eff["Garmin"])[0],
                         ac.EQUAL)
        # A failed fit anywhere fails the inherited verdict too.
        results["Feather"]["dial_trustworthy"] = False
        eff = ac.effective_dial(results)
        self.assertFalse(eff["Garmin"]["dial_trustworthy"])
        # Nobody reporting RSSI: nothing to inherit, stays failed.
        only = {"Garmin": dict(results["Garmin"])}
        self.assertFalse(ac.effective_dial(only)["Garmin"]["dial_trustworthy"])

    def test_usb_rules(self):
        ref = {"p50": 1.0, "p99": 2.0}
        self.assertEqual(ac.verdict_usb({"p50": 1.05, "p99": 2.5}, ref)[0],
                         ac.EQUAL)
        self.assertEqual(ac.verdict_usb({"p50": 0.9, "p99": 1.5}, ref)[0],
                         ac.BETTER)
        # Lower median alone, with a worse tail, is Equal - not Better.
        self.assertEqual(ac.verdict_usb({"p50": 0.9, "p99": 3.0}, ref)[0],
                         ac.EQUAL)
        self.assertEqual(ac.verdict_usb({"p50": 1.2, "p99": 1.5}, ref)[0],
                         ac.WORSE)
        self.assertEqual(ac.verdict_usb(None, ref)[0], ac.INCONCLUSIVE)


def verify_json(missed: int, sent: int, dn: int, unaccounted: int = 0,
                channels: int = 1, exact: bool = True) -> dict:
    """A canned ant_verify --json document, cut down to what is read."""
    per, chans, idents = {}, [], {}
    for ch in range(channels):
        per[str(ch)] = {
            "channel": ch, "profile": "power" if ch == 0 else "heart-rate",
            "device_number": dn + ch, "packets": sent - missed,
            "exact_missed": missed if exact else None,
            "exact_sent": sent if exact else None,
        }
        chans.append({"channel": ch, "expected_packets": float(sent)})
        idents[str(ch)] = {"device_number": dn + ch, "device_type": 11}
    return {"derived": {"per_channel": per, "rssi_dbm_mean": -50.0},
            "channels": chans, "identities": idents,
            "accounting": {"unaccounted": unaccounted}, "pass": True}


def window(phase: str, rep: int, per_rx: dict[str, tuple[int, int]],
           warnings=None) -> dict:
    results = {rx: {"counts": ac.verify_counts(verify_json(m, s, 53000))}
               for rx, (m, s) in per_rx.items()}
    return {"phase": phase, "round": "A", "rep": rep, "results": results,
            "warnings": warnings or []}


class TestChildJson(unittest.TestCase):
    def test_exact_counts_are_read(self):
        c = ac.verify_counts(verify_json(3, 1200, 53000))
        self.assertEqual((c["missed"], c["sent"], c["basis"]),
                         (3, 1200, "exact"))

    def test_wall_clock_is_the_labelled_fallback(self):
        c = ac.verify_counts(verify_json(3, 1200, 53000, exact=False))
        self.assertEqual(c["basis"], "wall_clock")
        self.assertEqual(c["missed"], 3)

    def test_multi_channel_pools_as_one_ratio(self):
        c = ac.verify_counts(verify_json(6, 1200, 53000, channels=4))
        self.assertEqual((c["missed"], c["sent"]), (24, 4800))
        self.assertEqual(len(c["per_channel"]), 4)

    def test_no_json_counts_nothing(self):
        self.assertIsNone(ac.verify_counts(None)["sent"])

    def test_window_sanity_flags_each_trap(self):
        good = verify_json(0, 1200, 53000)
        self.assertEqual(ac.window_sanity(good, [53000], 300, 301), [])
        self.assertTrue(any("unaccounted" in p for p in ac.window_sanity(
            verify_json(0, 1200, 53000, unaccounted=4), [53000], 300, 301)))
        self.assertTrue(any("pinned" in p for p in ac.window_sanity(
            good, [53999], 300, 301)))
        self.assertTrue(any("dropped port" in p for p in ac.window_sanity(
            good, [53000], 300, 120)))
        self.assertEqual(ac.window_sanity(None, [53000], 300, 1),
                         ["no JSON written"])

    def test_collisions_in_an_old_document_are_not_host_loss(self):
        # Written before ant_verify counted CHANNEL_COLLISION: its
        # `unaccounted` includes them, and the sanity check takes them back.
        doc = verify_json(0, 1200, 53000, unaccounted=155)
        doc["events"] = {"EVENT_RX_FAIL": 11, "EVENT_CHANNEL_COLLISION": 156}
        self.assertEqual(ac.window_sanity(doc, [53000], 300, 301), [])
        doc["events"]["EVENT_CHANNEL_COLLISION"] = 150
        self.assertTrue(any("unaccounted=5" in p for p in ac.window_sanity(
            doc, [53000], 300, 301)))

    def test_a_new_document_is_taken_at_its_word(self):
        doc = verify_json(0, 1200, 53000, unaccounted=7)
        doc["accounting"]["collisions"] = 156
        doc["events"] = {"EVENT_CHANNEL_COLLISION": 156}
        self.assertTrue(any("unaccounted=7" in p for p in ac.window_sanity(
            doc, [53000], 300, 301)))

    def test_report_only_rejudges_stored_windows(self):
        import json
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            doc = verify_json(0, 1200, 53000, unaccounted=155)
            doc["events"] = {"EVENT_CHANNEL_COLLISION": 156}
            (out / "w.json").write_text(json.dumps(doc), encoding="utf-8")
            rec = {"phase": "multi", "round": "A", "rep": 1, "seconds": 300,
                   "device_numbers": [53000],
                   "warnings": ["Garmin: unaccounted=155 (loss after the "
                                "radio)", "5340: ant_sim rc 1"],
                   "results": {"Garmin": {
                       "json": "w.json", "wall_s": 301,
                       "problems": ["unaccounted=155 (loss after the radio)"]},
                       "Feather": {"json": "missing.json", "wall_s": 3,
                                   "problems": ["boom", "no JSON written"]}}}
            self.assertEqual(ac.resanitize([rec], out), 1)
            self.assertEqual(rec["results"]["Garmin"]["problems"], [])
            # The transmitter's warning stays; a window with no JSON keeps
            # what was recorded, error text included.
            self.assertEqual(rec["warnings"], ["5340: ant_sim rc 1"])
            self.assertEqual(rec["results"]["Feather"]["problems"],
                             ["boom", "no JSON written"])

    def test_probe_and_sim_logs(self):
        probe = ("  OK: 'RADIANT0.01B00'\n      max channels: 32, max "
                 "networks: 3\nPASS: reset -> startup")
        p = ac.parse_probe_log(probe)
        self.assertEqual((p["version"], p["channels"], p["networks"]),
                         ("RADIANT0.01B00", 32, 3))
        sim = ("  OK: channel 0 - bicycle power, device #53000 type 0x0B\n"
               "    12 sent on the wall-clock fallback\n")
        s = ac.parse_sim_log(sim)
        self.assertEqual(s["fallbacks"], 12)
        self.assertEqual(s["device_numbers"], [53000])
        self.assertFalse(s["paced"])


class TestSummary(unittest.TestCase):
    def setUp(self):
        self.rig = ac.load_rig(RIG)

    def test_loss_verdicts_from_two_windows(self):
        records = [
            window("loss", 1, {"Garmin": (12, 1200), "Feather": (2, 1200),
                               "Dongle": (12, 1200), "54L15": (80, 1200)}),
            window("loss", 2, {"Garmin": (14, 1200), "Feather": (3, 1200),
                               "Dongle": (11, 1200), "54L15": (85, 1200)}),
        ]
        s = ac.build_summary(self.rig, records)
        got = {v["receiver"]: v["verdict"] for v in s["verdicts"]
               if v["metric"] == "loss"}
        self.assertEqual(got["Feather"], ac.BETTER)
        self.assertEqual(got["Dongle"], ac.EQUAL)
        self.assertEqual(got["54L15"], ac.WORSE)
        self.assertNotIn("Garmin", got, "the reference is not judged")

    def test_a_gap_that_does_not_repeat_is_inconclusive(self):
        records = [
            window("loss", 1, {"Garmin": (12, 1200), "Feather": (2, 1200)}),
            window("loss", 2, {"Garmin": (12, 1200), "Feather": (60, 1200)}),
        ]
        s = ac.build_summary(self.rig, records)
        v = next(v for v in s["verdicts"] if v["receiver"] == "Feather")
        self.assertEqual(v["verdict"], ac.INCONCLUSIVE)
        self.assertIn("does not repeat", v["detail"])
        self.assertGreater(v["gap_spread_pp"], 1.0)

    def test_a_window_the_reference_missed_is_dropped_for_both(self):
        records = [
            window("loss", 1, {"Garmin": (30, 1200), "Feather": (2, 1200)}),
            window("loss", 2, {"Feather": (900, 1200)}),
        ]
        s = ac.build_summary(self.rig, records)
        v = next(v for v in s["verdicts"] if v["receiver"] == "Feather")
        self.assertEqual(v["windows"], 1)
        self.assertEqual(v["verdict"], ac.BETTER)

    def test_missing_receiver_is_inconclusive(self):
        records = [window("loss", 1, {"Garmin": (12, 1200)})]
        s = ac.build_summary(self.rig, records)
        got = {v["receiver"]: v["verdict"] for v in s["verdicts"]}
        self.assertEqual(got["Feather"], ac.INCONCLUSIVE)

    def test_every_phase_lands_in_the_summary_and_markdown(self):
        cycles = [{"Garmin": 2.0, "Feather": 1.0, "Dongle": None,
                   "54L15": 2.1} for _ in range(20)]
        records = [
            window("loss", 1, {"Garmin": (6, 1200), "Feather": (6, 1200)}),
            window("multi", 1, {"Garmin": (6, 1200), "Feather": (6, 1200)}),
            {"phase": "acquire", "round": "A", "rep": 1, "cycles": cycles,
             "warnings": []},
            {"phase": "ladder", "round": "A", "rep": 1, "warnings": [],
             "results": {"Garmin": {"knee_dbm": -20.0,
                                    "dial_trustworthy": True},
                         "Feather": {"knee_dbm": -22.5,
                                     "dial_trustworthy": True}}},
            {"phase": "acked", "round": "A", "rep": 1, "warnings": [],
             "blocks": [{"receiver": "Garmin", "sent": 60, "completed": 59,
                         "failed": 1, "peer_received": 59},
                        {"receiver": "Feather", "sent": 60, "completed": 60,
                         "failed": 0, "peer_received": 60}]},
            {"phase": "usb", "round": "-", "rep": 0, "warnings": [],
             "results": {"Garmin": {"p50": 1.0, "p99": 2.0},
                         "Feather": {"p50": 0.5, "p99": 1.0}}},
        ]
        s = ac.build_summary(self.rig, records)
        metrics = {(v["metric"], v["receiver"]): v["verdict"]
                   for v in s["verdicts"]}
        self.assertEqual(metrics[("acquire", "Feather")], ac.BETTER)
        self.assertEqual(metrics[("acquire", "Dongle")], ac.INCONCLUSIVE)
        self.assertEqual(metrics[("ladder", "Feather")], ac.BETTER)
        self.assertEqual(metrics[("ladder", "Dongle")], ac.INCONCLUSIVE)
        self.assertIn(("acked", "Feather"), metrics)
        self.assertEqual(metrics[("usb", "Feather")], ac.BETTER)
        self.assertIn(("loss (multi, pooled)", "Feather"), metrics)

        md = ac.render_markdown(self.rig, s, records)
        for heading in ("## Verdicts", "### acquisition", "### sensitivity",
                        "### acknowledged", "## USB round trip",
                        "## Decision rules", "## Caveats",
                        "multi, loss % per profile"):
            self.assertIn(heading, md)
        self.assertIn("0 dBm at most", md, "5340 ceiling caveat")
        self.assertIn("UART", md)

    def test_records_upsert_by_key_and_survive_a_rerun(self):
        a = window("loss", 1, {"Garmin": (6, 1200)})
        b = window("loss", 1, {"Garmin": (9, 1200)})
        c = window("loss", 2, {"Garmin": (9, 1200)})
        records = ac.upsert(ac.upsert(ac.upsert([], a), c), b)
        self.assertEqual(len(records), 2)
        self.assertIs(records[-1], b)
        with tempfile.TemporaryDirectory() as tmp:
            ac.save(self.rig, Path(tmp), records)
            self.assertEqual(len(ac.load_records(Path(tmp))), 2)
            self.assertTrue((Path(tmp) / "summary.md").exists())

    def test_sim_plan_splits_and_numbers_like_ant_sim(self):
        rnd = self.rig.round("C")
        per_tx, numbers = ac._sim_plan(self.rig, rnd, "multi",
                                       list(ac.DEFAULT_MULTI_PROFILES))
        self.assertEqual(per_tx, {"5340": ["power", "heart-rate"],
                                  "54L15": ["csc", "fec-treadmill"]})
        base53 = self.rig.device_number(rnd, "5340", "multi")
        base54 = self.rig.device_number(rnd, "54L15", "multi")
        self.assertEqual(numbers, [base53, base53 + 1, base54, base54 + 1])


try:
    import ant_probe
except SystemExit:  # pyusb missing: ant_probe exits rather than raising
    ant_probe = None


@unittest.skipIf(ant_probe is None, "pyusb is not installed")
class TestReferenceStickSerial(unittest.TestCase):
    def test_the_garmin_serial_is_cut_at_its_nul(self):
        # The exact descriptor the bench's USB-m returned on 2026-11-07.
        raw = ("006\x00\x06\x9fR\x00\x00\x00\x06\x9fR\x00\x00\x00GRMN\x00"
               "\x86\x00\x00\xa6")
        self.assertEqual(ant_probe._clean_serial(raw), "006")
        self.assertTrue(ant_probe._clean_serial(raw).endswith("006"))

    def test_an_ordinary_serial_is_untouched(self):
        self.assertEqual(ant_probe._clean_serial("755972D7183A6183"),
                         "755972D7183A6183")
        self.assertIsNone(ant_probe._clean_serial(None))


if __name__ == "__main__":
    unittest.main()
