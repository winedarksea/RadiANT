"""Gate G1 summary: one JSON + text per arm from the files g1_arm.py wrote.

Usage: python g1_summary.py RUNDIR OUTDIR LABEL [LABEL ...]
Writes OUTDIR/g1_summary.json, OUTDIR/<LABEL>_pooled_peek.json (estimator input:
first power_diag snapshot of S0, last of S4, register samples pooled over every
session of the arm) and prints a table.
"""
import json
import math
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g1_lpdecode  # noqa: E402

PERIOD_S = 8070 / 32768


def jload(p):
    with open(p, encoding="utf-8-sig") as f:
        return json.load(f)


def wilson(k, n, z=1.96):
    if n == 0:
        return None
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return [max(0.0, c - h), min(1.0, c + h)]


def snaps(j):
    return [s for s in j["diag"]["snapshots"] if s.get("ok") and s.get("fields")]


def rates(a, b):
    fa, fb = a["fields"], b["fields"]
    dt_ms = fb["uptime_ms"] - fa["uptime_ms"]
    dt_us = dt_ms * 1000.0
    dts = dt_ms / 1000.0

    def d(k):
        return (fb[k] - fa[k]) & 0xFFFFFFFF

    return dict(span_s=round(dts, 3),
                hfxo_req_pct=round(100 * d("hfxo_req_us") / dt_us, 4),
                hfxo_requests_per_s=round(d("hfxo_requests") / dts, 4),
                hfxo_us_per_request=round(d("hfxo_req_us") / max(1, d("hfxo_requests")), 1),
                tx_us_per_s=round(d("radio_tx_us") / dts, 2),
                rx_us_per_s=round(d("radio_rx_us") / dts, 2),
                tx_ops_per_s=round(d("radio_tx_ops") / dts, 4),
                rx_ops_per_s=round(d("radio_rx_ops") / dts, 4),
                cpu_permille=round(1000 * d("cpu_active_us") / dt_us, 3),
                idle_per_s=round(d("idle_entries") / dts, 3),
                flags_end=hex(fb["flags"]), constlat_stat_end=fb["constlat_stat"])


def verify(path):
    j = jload(path)
    c = j["channels"][0]
    acc = j.get("accounting", {})
    ri = c.get("g1_radio_intervals_s") or []
    pairs = [(x, round(x / PERIOD_S)) for x in ri]
    pairs = [(x, k) for x, k in pairs if k >= 1]
    per = (sum(x for x, _ in pairs) / sum(k for _, k in pairs)) if pairs else None
    one = [x for x, k in pairs if k == 1]
    resid = [x - k * per for x, k in pairs] if per else []
    sig = c.get("signal") or {}
    jit = c["jitter"]
    hr = c.get("heart_rate_bpm") or {}
    return dict(packets=c["packets"], expected=round(c["expected_packets"], 1),
                loss_pct=round(c["loss_pct"], 3), rx_fail=acc.get("radio_fails"),
                unaccounted=acc.get("unaccounted"), collisions=acc.get("collisions"),
                host_interval_mean_ms=round(jit["mean_s"] * 1e3, 3),
                host_interval_stdev_ms=round(jit["stdev_s"] * 1e3, 2),
                gap_max_ms=round(jit["max_s"] * 1e3, 1),
                offgrid_radio_us=(round(jit["offgrid_radio_s"] * 1e6, 3)
                                  if jit.get("offgrid_radio_s") is not None else None),
                radio_intervals=len(pairs), radio_slots=sum(k for _, k in pairs),
                radio_period_us=round(per * 1e6, 4) if per else None,
                radio_period_minus_nominal_us=round((per - PERIOD_S) * 1e6, 4) if per else None,
                radio_single_interval_mean_us=round(statistics.fmean(one) * 1e6, 3) if one else None,
                radio_single_interval_stdev_us=round(statistics.pstdev(one) * 1e6, 3) if len(one) > 1 else None,
                radio_resid_stdev_us=round(statistics.pstdev(resid) * 1e6, 3) if len(resid) > 1 else None,
                rssi_mean=round(sig["mean_dbm"], 1) if sig else None,
                rssi_min=sig.get("min_dbm"), rssi_max=sig.get("max_dbm"),
                bpm=hr.get("mean") if isinstance(hr, dict) else hr)


def arm(rundir, outdir, lab):
    def p(t):
        return os.path.join(rundir, f"{lab}_{t}.json")

    r = dict(label=lab)
    sessions = {}
    names = ["Sboot_lp", "S0", "S1_lp", "S2"] + [f"S2_{i}" for i in range(12)] + ["S3_lp", "S4"]
    for t in names:
        if os.path.exists(p(t)):
            sessions[t] = jload(p(t))
    r["liveness"] = {t: s["liveness"]["verdict"] for t, s in sessions.items()}
    r["security_lines"] = {t: s.get("security_lines") for t, s in sessions.items()}
    pool = dict(valid=0, hfxo_running=0, constlat_valid=0, constlat_on=0)
    per = {}
    for t, s in sessions.items():
        sm = s.get("samples") or {}
        per[t] = (f"{sm.get('hfxo_running')}/{sm.get('valid')} xo, "
                  f"{sm.get('constlat_on')}/{sm.get('constlat_valid')} cl")
        for k in pool:
            pool[k] += int(sm.get(k) or 0)
    r["samples_per_session"] = per
    r["samples_pooled"] = dict(
        pool, hfxo_pct=round(100 * pool["hfxo_running"] / pool["valid"], 2),
        hfxo_ci95_pct=[round(100 * x, 2) for x in wilson(pool["hfxo_running"], pool["valid"])],
        constlat_pct=round(100 * pool["constlat_on"] / pool["constlat_valid"], 2),
        constlat_ci95_pct=[round(100 * x, 2) for x in wilson(pool["constlat_on"], pool["constlat_valid"])])
    s0, s4 = snaps(sessions["S0"]), snaps(sessions["S4"])
    s2 = snaps(sessions["S2"]) if "S2" in sessions else snaps(sessions["S2_0"])
    r["pd_full"] = rates(s0[0], s4[-1])
    r["pd_unattached"] = rates(s0[-1], s2[0])
    if len(s2) >= 2:
        r["pd_attached_S2"] = rates(s2[0], s2[-1])
    else:
        r["pd_attached_S2"] = f"not usable: {len(s2)} good snapshot(s) in S2"
    r["uptime_s_start"] = s0[0]["fields"]["uptime_ms"] / 1000
    pooled = dict(sessions["S4"])
    pooled["diag"] = dict(sessions["S4"]["diag"], snapshots=[s0[0], s4[-1]])
    sm = dict(sessions["S4"]["samples"])
    sm.update(requested=pool["valid"], valid=pool["valid"], hfxo_running=pool["hfxo_running"],
              hfxo_duty=pool["hfxo_running"] / pool["valid"], constlat_on=pool["constlat_on"],
              constlat_valid=pool["constlat_valid"],
              hfxo_ci95=wilson(pool["hfxo_running"], pool["valid"]),
              constlat_ci95=wilson(pool["constlat_on"], pool["constlat_valid"]))
    pooled["samples"] = sm
    pooled["host_start_unix"] = sessions["S0"]["host_start_unix"]
    pooled["g1_note"] = "merged by g1_summary.py: S0 first + S4 last snapshot, samples pooled over the arm"
    with open(os.path.join(outdir, f"{lab}_pooled_peek.json"), "w", encoding="utf-8") as f:
        json.dump(pooled, f, indent=1)
    dec = {}
    for t in ("Sboot_lp", "S1_lp", "S3_lp"):
        if os.path.exists(p(t)):
            _, ss = g1_lpdecode.load(p(t))
            dec[t] = [d for _, d in ss]
    if dec:
        r["lp"] = dec
    if "S1_lp" in dec and "S3_lp" in dec:
        a, b = dec["S1_lp"][0], dec["S3_lp"][-1]
        delta = {k: b[k] - a[k] for k in g1_lpdecode.FIELDS if k not in g1_lpdecode.GAUGES}
        span = sessions["S3_lp"]["host_end_unix"] - sessions["S1_lp"]["host_start_unix"]
        r["lp_window"] = dict(delta=delta, host_span_s=round(span, 1),
                              sleeps_per_s=round(delta["sleeps"] / span, 3),
                              wakes_per_s=round(delta["wakes"] / span, 3),
                              gauges_end={k: b[k] for k in g1_lpdecode.GAUGES})
    for name in ("promicro", "ti"):
        vp = os.path.join(rundir, f"{lab}_verify_{name}.json")
        if os.path.exists(vp):
            r[f"rx_{name}"] = verify(vp)
    return r


def main():
    rundir, outdir = sys.argv[1], sys.argv[2]
    res = {lab: arm(rundir, outdir, lab) for lab in sys.argv[3:]}
    with open(os.path.join(outdir, "g1_summary.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    for lab, r in res.items():
        print(f"== {lab}")
        for k in ("liveness", "samples_per_session", "samples_pooled", "pd_full", "pd_unattached",
                  "pd_attached_S2", "lp_window", "rx_promicro", "rx_ti"):
            if k in r:
                print(f"  {k}: {json.dumps(r[k])}")


if __name__ == "__main__":
    main()
