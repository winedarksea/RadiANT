# SPDX-License-Identifier: Apache-2.0

"""tools/ant_power_est.py's arithmetic, with no board and no meter.

The estimate is only as honest as three things these pin: counters are
differenced modulo 2^32 (a wrap must not read as a 4-billion-us burst, and an
interval too long to difference must be refused, not guessed); a reboot
between snapshots is caught rather than read as a huge or negative interval;
and a current nobody could cite is excluded from the total, visibly, instead
of being quietly counted as zero or as a made-up number.
"""

from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ant_power_est as pe  # noqa: E402

NM = pe.NOT_MEASURED
ALL_FLAGS = (pe.F_HFXO_MEASURED | pe.F_CONSTLAT_HW_VALID | pe.F_CPU_LOAD | pe.F_IDLE)


def snap(**kw):
    f = {"magic": pe.MAGIC, "version": pe.VERSION, "seq": 2, "uptime_ms": 0,
         "hfxo_req_us": 0, "radio_tx_us": 0, "radio_rx_us": 0, "radio_tx_ops": 0,
         "radio_rx_ops": 0, "cpu_active_us": 0, "idle_entries": 0,
         "flags": ALL_FLAGS, "constlat_stat": 1}
    host = kw.pop("host_time_unix", None)
    f.update(kw)
    s = {"fields": f}
    if host is not None:
        s["host_time_unix"] = host
    return s


class Arithmetic(unittest.TestCase):
    def test_u32_delta_wraps(self):
        self.assertEqual(pe.u32_delta(5, 3), 2)
        self.assertEqual(pe.u32_delta(0x10, 0xFFFFFFF0), 0x20)
        self.assertEqual(pe.u32_delta(0, 0xFFFFFFFF), 1)

    def test_wilson_matches_hand_values(self):
        lo, hi = pe.wilson_interval(5, 10)
        self.assertAlmostEqual(lo, 0.23659, places=4)
        self.assertAlmostEqual(hi, 0.76341, places=4)
        lo, hi = pe.wilson_interval(0, 10)
        self.assertEqual(lo, 0.0)
        self.assertAlmostEqual(hi, 0.27753, places=4)
        lo, hi = pe.wilson_interval(400, 400)
        self.assertEqual(hi, 1.0)
        self.assertAlmostEqual(lo, 0.99050, places=4)

    def test_wilson_rejects_nonsense(self):
        for k, n in ((1, 0), (-1, 5), (6, 5)):
            with self.subTest(k=k, n=n), self.assertRaises(ValueError):
                pe.wilson_interval(k, n)

    def test_wilson_contains_the_point_and_narrows(self):
        wide = pe.wilson_interval(9, 10)
        narrow = pe.wilson_interval(900, 1000)
        self.assertTrue(wide[0] < 0.9 < wide[1])
        self.assertTrue(narrow[0] < 0.9 < narrow[1])
        self.assertLess(narrow[1] - narrow[0], wide[1] - wide[0])

    def test_charge_and_period(self):
        self.assertEqual(pe.charge_nc(1.0, 100.0), 100.0)        # 1 mA x 100 us
        self.assertAlmostEqual(pe.HR_PERIOD_S, 0.246276855, places=8)
        # 100 nC every 8070/32768 s = 406.04 nA
        self.assertAlmostEqual(pe.ua_at_period(100.0), 0.40604, places=4)
        self.assertAlmostEqual(pe.ua_at_period(100.0, 1.0), 0.1)
        with self.assertRaises(ValueError):
            pe.ua_at_period(1.0, 0.0)

    def test_parse_duty(self):
        self.assertEqual(pe.parse_duty("0.5"), (0.5, None))
        d, ci = pe.parse_duty("388/400")
        self.assertAlmostEqual(d, 0.97)
        self.assertEqual(ci, pe.wilson_interval(388, 400))
        import argparse
        for bad in ("3/0", "5/4", "-1/4", "a/4", "1.5", "x"):
            with self.subTest(bad=bad), self.assertRaises(argparse.ArgumentTypeError):
                pe.parse_duty(bad)


class Diff(unittest.TestCase):
    def test_plain_interval(self):
        a = snap(uptime_ms=10_000, hfxo_req_us=1_000_000, radio_tx_us=100, radio_tx_ops=1,
                 cpu_active_us=5_000, idle_entries=50, seq=10)
        b = snap(uptime_ms=40_000, hfxo_req_us=31_000_000, radio_tx_us=12_280, radio_tx_ops=123,
                 cpu_active_us=305_000, idle_entries=650, seq=70)
        iv = pe.diff_snapshots(a, b)
        self.assertEqual(iv.dt_s, 30.0)
        self.assertEqual(iv.hfxo_us, 30_000_000)
        self.assertEqual(iv.tx_us, 12_180)
        self.assertEqual(iv.tx_ops, 122)
        self.assertEqual(iv.cpu_us, 300_000)
        self.assertEqual(iv.idle_entries, 600)
        self.assertIs(iv.constlat, True)
        self.assertEqual(iv.problems, [])

    def test_counters_and_uptime_wrap(self):
        a = snap(uptime_ms=0xFFFFF000, hfxo_req_us=0xFFFF0000, radio_tx_us=0xFFFFFF00,
                 radio_tx_ops=0xFFFFFFFF)
        b = snap(uptime_ms=0x00001000, hfxo_req_us=0x007A0000,
                 radio_tx_us=0x00000100, radio_tx_ops=3)
        iv = pe.diff_snapshots(a, b)
        self.assertEqual(iv.dt_s, 0x2000 / 1000)          # 8.192 s across the uptime wrap
        self.assertEqual(iv.hfxo_us, 0x007A0000 + 0x10000)
        self.assertEqual(iv.tx_us, 0x200)
        self.assertEqual(iv.tx_ops, 4)   # 0xFFFFFFFF is a value here: radio_* have no flag

    def test_flagged_counter_at_the_sentinel_value_is_still_a_value(self):
        a = snap(uptime_ms=0, hfxo_req_us=0xFFFFFFFF)
        b = snap(uptime_ms=1000, hfxo_req_us=999_999)
        self.assertEqual(pe.diff_snapshots(a, b).hfxo_us, 1_000_000)
        # ...but with no flags word, the sentinel is all there is to go on.
        a["fields"].pop("flags"); b["fields"].pop("flags")
        self.assertIsNone(pe.diff_snapshots(a, b).hfxo_us)

    def test_interval_too_long_for_us_counters_is_refused(self):
        a = snap(uptime_ms=0, hfxo_req_us=5, radio_tx_us=5, radio_tx_ops=0)
        b = snap(uptime_ms=4_400_000, hfxo_req_us=10, radio_tx_us=10, radio_tx_ops=17_000)
        iv = pe.diff_snapshots(a, b)
        self.assertIsNone(iv.hfxo_us)
        self.assertIsNone(iv.tx_us)
        self.assertEqual(iv.tx_ops, 17_000)               # counts are still fine
        self.assertTrue(any("71.6 min" in p for p in iv.problems))

    def test_impossible_us_delta_is_not_trusted(self):
        a = snap(uptime_ms=1000, cpu_active_us=0)
        b = snap(uptime_ms=2000, cpu_active_us=5_000_000)  # 5 s busy in 1 s
        iv = pe.diff_snapshots(a, b)
        self.assertIsNone(iv.cpu_us)
        self.assertTrue(iv.problems)

    def test_radio_counter_may_run_one_operation_past_the_interval(self):
        # A1 charges an operation's whole on-time when it ends; a scan chunk
        # is up to 250 ms, so 10.2 s of RX in a 10 s window is legitimate.
        a = snap(uptime_ms=0, radio_rx_us=0)
        b = snap(uptime_ms=10_000, radio_rx_us=10_200_000)
        iv = pe.diff_snapshots(a, b)
        self.assertEqual(iv.rx_us, 10_000_000)                 # clamped to 100 %
        self.assertTrue(any("clamped" in p for p in iv.problems))
        b = snap(uptime_ms=10_000, radio_rx_us=10_000_000 + 100_000 + pe.MAX_OP_US + 1)
        self.assertIsNone(pe.diff_snapshots(a, b).rx_us)       # more than one op past: refused
        # cpu/hfxo keep the tight tolerance
        b = snap(uptime_ms=10_000, cpu_active_us=10_200_000)
        self.assertIsNone(pe.diff_snapshots(snap(uptime_ms=0), b).cpu_us)

    def test_not_measured_and_flags(self):
        a = snap(uptime_ms=0, cpu_active_us=NM, idle_entries=NM, hfxo_req_us=NM,
                 flags=pe.F_CONSTLAT_REQ)
        b = snap(uptime_ms=1000, cpu_active_us=NM, idle_entries=NM, hfxo_req_us=NM,
                 flags=pe.F_CONSTLAT_REQ)
        iv = pe.diff_snapshots(a, b)
        self.assertIsNone(iv.cpu_us)
        self.assertIsNone(iv.idle_entries)
        self.assertIsNone(iv.hfxo_us)
        self.assertIsNone(iv.constlat)                     # HW_VALID not set

    def test_flag_clear_overrides_a_plausible_value(self):
        a = snap(uptime_ms=0, cpu_active_us=0, flags=pe.F_HFXO_MEASURED)
        b = snap(uptime_ms=1000, cpu_active_us=10, flags=pe.F_HFXO_MEASURED)
        self.assertIsNone(pe.diff_snapshots(a, b).cpu_us)

    def test_reboot_detected_by_seq(self):
        with self.assertRaisesRegex(pe.InputError, "rebooted"):
            pe.diff_snapshots(snap(uptime_ms=50_000, seq=100), snap(uptime_ms=60_000, seq=4))

    def test_reboot_detected_by_host_clock(self):
        a = snap(uptime_ms=50_000, host_time_unix=1000.0)
        b = snap(uptime_ms=5_000, host_time_unix=1030.0)  # would read as ~49.7 days
        with self.assertRaisesRegex(pe.InputError, "host clock"):
            pe.diff_snapshots(a, b)
        ok = pe.diff_snapshots(snap(uptime_ms=50_000, host_time_unix=1000.0),
                               snap(uptime_ms=80_400, host_time_unix=1030.9))
        self.assertAlmostEqual(ok.dt_s, 30.4)

    def test_wrong_magic_and_zero_interval(self):
        with self.assertRaisesRegex(pe.InputError, "magic"):
            pe.diff_snapshots(snap(magic=0x1234), snap(uptime_ms=10))
        with self.assertRaisesRegex(pe.InputError, "zero interval"):
            pe.diff_snapshots(snap(uptime_ms=10), snap(uptime_ms=10))

    def test_bare_fields_object_accepted(self):
        s = pe.normalise_snapshot({"uptime_ms": 5, "seq": 2})
        self.assertEqual(s, {"fields": {"uptime_ms": 5, "seq": 2}})


PEEK = {
    "tool": "jlink_peek", "schema": 1, "preset": "nrf54l15",
    "host_start_unix": 1000.0, "host_end_unix": 1030.2,
    "parse_ok": True, "parse_error": None,
    "liveness": {"verdict": "running"},
    "samples": {"requested": 100, "valid": 100, "hfxo_running": 97, "hfxo_duty": 0.97,
                "constlat_on": 100, "constlat_valid": 100},
    "diag": {"snapshots": [
        {"when": "before", "ok": True, "fields": snap(uptime_ms=100_000, seq=200,
                                                       hfxo_req_us=99_990_000)["fields"]},
        {"when": "after", "ok": True, "fields": snap(uptime_ms=130_000, seq=260,
                                                      hfxo_req_us=129_990_000,
                                                      radio_tx_us=12_180,
                                                      radio_tx_ops=122)["fields"]},
    ]},
}


class Inputs(unittest.TestCase):
    def _write(self, obj, d, name):
        p = os.path.join(d, name)
        with open(p, "w", encoding="utf-8") as fh:
            json.dump(obj, fh)
        return p

    def test_single_peek_file_gives_both_snapshots_and_samples(self):
        with tempfile.TemporaryDirectory() as d:
            iv, peek = pe.load_inputs([self._write(PEEK, d, "pd.json")], None)
        self.assertEqual(iv.dt_s, 30.0)
        self.assertEqual(iv.hfxo_us, 30_000_000)
        self.assertEqual(peek["samples"]["hfxo_running"], 97)

    def test_sampled_duty_used_only_from_a_running_core(self):
        duty, ci, cl, notes = pe.sampled_duty(PEEK)
        self.assertAlmostEqual(duty, 0.97)
        self.assertEqual(ci, pe.wilson_interval(97, 100))
        self.assertEqual(cl, 1.0)
        self.assertEqual(notes, [])
        for verdict in ("halted", "lockup", "unknown", None):
            peek = json.loads(json.dumps(PEEK))
            peek["liveness"]["verdict"] = verdict
            with self.subTest(verdict=verdict):
                duty, ci, cl, notes = pe.sampled_duty(peek)
                self.assertEqual((duty, ci, cl), (None, None, None))
                self.assertTrue(any("refused" in n for n in notes))
        no_liveness = {k: v for k, v in PEEK.items() if k != "liveness"}
        self.assertIsNone(pe.sampled_duty(no_liveness)[0])

    def test_parse_problem_warns_but_keeps_the_aligned_samples(self):
        peek = json.loads(json.dumps(PEEK))
        peek["parse_ok"] = False
        peek["parse_error"] = "misaligned at read 's.7.xo'"
        duty, _, _, notes = pe.sampled_duty(peek)
        self.assertAlmostEqual(duty, 0.97)
        self.assertTrue(any("could not align" in n for n in notes))

    def test_halted_peek_falls_back_to_the_counter_end_to_end(self):
        peek = json.loads(json.dumps(PEEK))
        peek["liveness"]["verdict"] = "halted"
        with tempfile.TemporaryDirectory() as d:
            p = self._write(peek, d, "pd.json")
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(pe.main(["--board", "nrf54l15dk", p]), 0)
        text = buf.getvalue()
        self.assertIn("hfxo_req_us counter", text)
        self.assertIn("WARNING: jlink_peek liveness verdict is 'halted'", text)

    def test_torn_or_unusable_snapshots_are_skipped(self):
        peek = json.loads(json.dumps(PEEK))
        peek["diag"]["snapshots"][1]["ok"] = False
        with tempfile.TemporaryDirectory() as d, self.assertRaisesRegex(pe.InputError, "two usable"):
            pe.load_inputs([self._write(peek, d, "pd.json")], None)

    def test_two_simple_files(self):
        with tempfile.TemporaryDirectory() as d:
            a = self._write(snap(uptime_ms=0, radio_tx_us=0), d, "a.json")
            b = self._write(snap(uptime_ms=10_000, radio_tx_us=1000), d, "b.json")
            iv, peek = pe.load_inputs([a, b], None)
        self.assertEqual((iv.dt_s, iv.tx_us, peek), (10.0, 1000, None))

    def test_log_lines_summed(self):
        lines = [
            "[00:00:10.000,000] <inf> radiant_power: power dt_ms=10000 hfxo_pm=1000 tx_us=406 "
            "tx_ops=41 rx_us=0 rx_ops=0 cpu_pm=12 idle_x10=245 constlat=1/1",
            "unrelated line tx_us=999999",
            "[00:00:20.000,000] <inf> radiant_power: power dt_ms=10000 hfxo_pm=500 tx_us=394 "
            "tx_ops=40 rx_us=6700 rx_ops=40 cpu_pm=8 idle_x10=255 constlat=1/0",
        ]
        iv = pe.parse_log(lines)
        self.assertEqual(iv.dt_s, 20.0)
        self.assertEqual((iv.tx_us, iv.tx_ops, iv.rx_us, iv.rx_ops), (800, 81, 6700, 40))
        self.assertEqual(iv.hfxo_us, 15_000_000)      # mean 750 per-mille of 20 s
        self.assertEqual(iv.cpu_us, 200_000)          # mean 10 per-mille
        self.assertEqual(iv.idle_entries, 500)        # 24.5/s and 25.5/s for 10 s each
        self.assertIs(iv.constlat, False)             # hardware state of the last line

    def test_log_unmeasured_fields_stay_unmeasured(self):
        iv = pe.parse_log(["power dt_ms=1000 hfxo_pm=-1 tx_us=1 tx_ops=1 rx_us=0 rx_ops=0 "
                           "cpu_pm=-1 idle_x10=-1 constlat=0/-1"])
        self.assertIsNone(iv.hfxo_us)
        self.assertIsNone(iv.cpu_us)
        self.assertIsNone(iv.idle_entries)
        self.assertIsNone(iv.constlat)

    def test_log_without_power_lines_refused(self):
        with self.assertRaises(pe.InputError):
            pe.parse_log(["nothing here"])


class Estimate(unittest.TestCase):
    def iv(self, **kw):
        base = dict(dt_s=10.0, tx_us=4_000, rx_us=0, tx_ops=41, rx_ops=0,
                    hfxo_us=10_000_000, cpu_us=100_000, idle_entries=245, constlat=False)
        base.update(kw)
        return pe.Interval(**base)

    def test_unknown_current_is_excluded_and_listed(self):
        b = pe.resolve_board("cc2652", None, False)
        e = pe.estimate(b, self.iv())
        names = {c.name: c for c in e.components}
        self.assertIsNone(names["HFXO (outside radio)"].ua)
        self.assertIn("HFXO (outside radio)", e.excluded)
        self.assertIn("UNKNOWN", names["HFXO (outside radio)"].why_excluded)
        self.assertIn("XOSC_HF", names["HFXO (outside radio)"].why_excluded)
        self.assertAlmostEqual(e.total_ua, sum(c.ua for c in e.components if c.ua is not None))

    def test_breakdown_numbers_nrf52840(self):
        b = pe.resolve_board("nrf52840", None, False)
        e = pe.estimate(b, self.iv())
        c = {x.name: x.ua for x in e.components}
        floor = 3.16
        self.assertAlmostEqual(c["idle floor"], floor)
        # 0.04 % TX duty x (6.40 mA - floor) in uA
        self.assertAlmostEqual(c["radio TX +0 dBm"], 4_000 / 10e6 * (6.40 - 0.00316) * 1000)
        self.assertAlmostEqual(c["CPU active"], 0.01 * (3.3 - 0.00316) * 1000)
        self.assertAlmostEqual(c["radio RX"], 0.0)
        # HFXO held 100 %, minus the 0.04 % already inside the TX row, at the
        # ITIMER2 whole-chip total over the floor.
        self.assertAlmostEqual(c["HFXO (outside radio)"], (1 - 0.0004) * (0.646 - 0.00316) * 1000)
        self.assertEqual(e.excluded, [])

    def test_hfxo_term_excludes_radio_time_and_prefers_samples(self):
        b = pe.resolve_board("nrf5340net", None, False)
        e = pe.estimate(b, self.iv(tx_us=1_000_000, hfxo_us=5_000_000), hfxo_row="X2",
                        hfxo_duty=0.6, hfxo_ci=(0.5, 0.7))
        h = next(x for x in e.components if x.name.startswith("HFXO"))
        self.assertAlmostEqual(h.ua, (0.6 - 0.1) * 0.135 * 1000)   # crystal row: no floor
        self.assertEqual(e.hfxo_duty, 0.6)
        self.assertTrue(any("request counter" in n for n in e.notes))

    def test_hfxo_rows_default_and_alternatives(self):
        b = pe.resolve_board("nrf54l15dk", None, False)
        e = pe.estimate(b, self.iv(hfxo_us=10_000_000, tx_us=0))
        h = next(x for x in e.components if x.name.startswith("HFXO"))
        self.assertIn("ITIMER3", h.current.where)
        self.assertAlmostEqual(h.ua, (0.240 - 0.0029) * 1000)
        e2 = pe.estimate(b, self.iv(hfxo_us=10_000_000, tx_us=0), hfxo_row="X2")
        self.assertAlmostEqual(next(x for x in e2.components if x.name.startswith("HFXO")).ua, 34.0)
        with self.assertRaises(pe.InputError):
            pe.estimate(b, self.iv(), hfxo_row="X9")
        net = pe.estimate(pe.resolve_board("nrf5340net", None, False), self.iv(tx_us=0))
        self.assertAlmostEqual(next(x for x in net.components if x.name.startswith("HFXO")).ua,
                               (0.400 - 0.0017) * 1000)
        # LDO on the net core: no TIMER-on-HFXO row, so UNKNOWN until a crystal row.
        ldo = pe.resolve_board("nrf5340net", "ldo", False)
        self.assertIn("HFXO (outside radio)", pe.estimate(ldo, self.iv()).excluded)
        self.assertNotIn("HFXO (outside radio)", pe.estimate(ldo, self.iv(), hfxo_row="X0").excluded)

    def test_counter_past_the_interval_is_clamped(self):
        b = pe.resolve_board("nrf54l15dk", None, False)
        e = pe.estimate(b, self.iv(rx_us=10_200_000))
        rx = next(x for x in e.components if x.name == "radio RX")
        self.assertAlmostEqual(rx.ua, (3.4 - 0.0029) * 1000)
        self.assertTrue(any("clamped" in n for n in e.notes))

    def test_cpu_band(self):
        b = pe.resolve_board("nrf54l15dk", None, False)
        e = pe.estimate(b, self.iv(), cpu_band_pm=(0.0, 1.0))
        self.assertIsNotNone(e.total_band_ua)
        self.assertAlmostEqual(e.total_band_ua[0], e.total_ua)
        self.assertAlmostEqual(e.total_band_ua[1] - e.total_ua, 0.001 * (2.6 - 0.0029) * 1000)
        self.assertIsNone(pe.estimate(b, self.iv()).total_band_ua)   # default (0, 0): no band
        self.assertEqual(pe.parse_band("-1,0"), (-1.0, 0.0))
        for bad in ("1", "2,1", "a,b"):
            with self.subTest(bad=bad), self.assertRaises(Exception):
                pe.parse_band(bad)

    def test_constlat_l15(self):
        b = pe.resolve_board("nrf54l15dk", None, False)
        on = pe.estimate(b, self.iv(constlat=True))
        c = next(x for x in on.components if x.name == "CONSTLAT")
        self.assertAlmostEqual(c.ua, (0.55 - 0.0008) * 1000)
        off = pe.estimate(b, self.iv(constlat=False))
        self.assertEqual(next(x for x in off.components if x.name == "CONSTLAT").ua, 0.0)
        unread = pe.estimate(b, self.iv(constlat=None))
        self.assertIn("CONSTLAT", unread.excluded)
        frac = pe.estimate(b, self.iv(constlat=None), constlat_fraction=0.5)
        self.assertAlmostEqual(next(x for x in frac.components if x.name == "CONSTLAT").ua,
                               0.5 * (0.55 - 0.0008) * 1000)

    def test_unmeasured_cpu_excluded(self):
        b = pe.resolve_board("nrf54l15dk", None, False)
        e = pe.estimate(b, self.iv(cpu_us=None))
        self.assertIn("CPU active", e.excluded)

    def test_uart_only_when_declared(self):
        b = pe.resolve_board("cc2652", None, False)
        self.assertNotIn("UART (declared on)", [c.name for c in pe.estimate(b, self.iv()).components])
        e = pe.estimate(b, self.iv(hfxo_us=None), uart_on=True)
        u = next(x for x in e.components if x.name == "UART (declared on)")
        self.assertAlmostEqual(u.ua, 167.5)
        self.assertIn("HFXO (outside radio)", e.excluded)
        e52 = pe.estimate(pe.resolve_board("nrf52840", None, False), self.iv(), uart_on=True)
        self.assertIn("UART (declared on)", e52.excluded)

    def test_tx_row_missing_is_unknown_not_zero(self):
        b = pe.resolve_board("promicro", None, False)
        e = pe.estimate(b, self.iv(), tx_dbm=8)
        self.assertIn("radio TX +8 dBm", e.excluded)
        e = pe.estimate(b, self.iv(), tx_dbm=-4)
        self.assertIn("radio TX -4 dBm", e.excluded)

    def test_regulator_and_ti_standby(self):
        ldo = pe.resolve_board("nrf52840", "ldo", False)
        self.assertEqual(ldo.rx.ma, 10.10)
        self.assertEqual(pe.resolve_board("cc2652", None, True).floor.ma, 0.00094)
        with self.assertRaises(pe.InputError):
            pe.resolve_board("promicro", "dcdc", False)
        with self.assertRaises(pe.InputError):
            pe.resolve_board("nrf52840", None, True)


class Table(unittest.TestCase):
    def test_every_known_current_is_cited_and_every_unknown_says_why(self):
        seen = 0
        for name, b in pe.BOARDS.items():
            cur = [b.floor, b.rx, b.cpu, b.uart, *b.tx.values()]
            cur += list(b.hfxo.values())
            if b.hfxo_default is not None:
                self.assertIn(b.hfxo_default, b.hfxo)
            cur += [c for c in (b.constlat, b.constlat_base) if c is not None]
            for c in cur:
                seen += 1
                with self.subTest(board=name, where=c.where):
                    if c.known:
                        self.assertIn(c.doc, pe.DOCS)
                        self.assertGreater(c.ma, 0)
                        self.assertTrue(c.where and not c.where.startswith("UNKNOWN"))
                    else:
                        self.assertTrue(c.where.startswith("UNKNOWN: ") and len(c.where) > 15)
        self.assertGreater(seen, 40)


class Cli(unittest.TestCase):
    def test_end_to_end_banner_and_exclusions(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "pd.json")
            out_json = os.path.join(d, "est.json")
            with open(p, "w", encoding="utf-8") as fh:
                json.dump(PEEK, fh)
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = pe.main(["--board", "nrf54l15dk", p, "--json", out_json])
            with open(out_json, encoding="utf-8") as fh:
                js = json.load(fh)
        text = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertTrue(text.startswith(pe.BANNER))
        self.assertIn("95 % CI", text)
        self.assertEqual(js["excluded"], [])
        self.assertAlmostEqual(js["hfxo_duty"], 0.97)
        buf = io.StringIO()
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "pd.json")
            with open(p, "w", encoding="utf-8") as fh:
                json.dump(PEEK, fh)
            with redirect_stdout(buf):
                self.assertEqual(pe.main(["--board", "cc2652", p]), 0)
        self.assertIn("EXCLUDED from the total: HFXO (outside radio)", buf.getvalue())

    def test_bad_input_exit_1(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.json")
            with open(p, "w", encoding="utf-8") as fh:
                json.dump({"uptime_ms": 5}, fh)
            err = io.StringIO()
            from contextlib import redirect_stderr
            with redirect_stderr(err):
                self.assertEqual(pe.main(["--board", "cc2652", p]), 1)


if __name__ == "__main__":
    unittest.main()
