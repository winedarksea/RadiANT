"""G1 re-judge summary. Per arm: both receivers (G1's verify() parse), the TI's
0xF7 deltas (pre->post the receive window), the L15 power_diag S0->S4 rates and
pooled register samples, and for LP arms the radiant_nrf_lp_stats S1_lp->S3_lp delta.

Usage: python g1r_summary.py RUNDIR OUTJSON LABEL [LABEL ...]   (prints a markdown table)
"""
import json, os, sys

G1 = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "2026-11-18-power-g1", "scripts")
sys.path.insert(0, os.path.abspath(G1))
import g1_lpdecode  # noqa: E402
from g1_summary import verify, rates, snaps, jload, wilson  # noqa: E402

TI_F = [("api", "slots_missed"), ("backend_b", "crc_fail"), ("rx_integrity", "crc_repaired"),
        ("rx_integrity", "crc_unrepairable"), ("rx_integrity", "crc_repair_refuted"),
        ("rx_integrity", "crc_repair_implausible")]


def tidiag(d, lab, tag):
    p = os.path.join(d, f"{lab}_tidiag_{tag}.json")
    if not os.path.exists(p):
        return None
    j = jload(p)
    pages = j["pages"].values() if isinstance(j["pages"], dict) else j["pages"]
    out = {}
    for pg in pages:
        for k, v in pg["fields"].items():
            out[(pg["name"], k)] = v
    return out


def arm(d, lab):
    r = dict(label=lab)
    for name in ("promicro", "ti"):
        vp = os.path.join(d, f"{lab}_verify_{name}.json")
        if os.path.exists(vp):
            r[f"rx_{name}"] = verify(vp)
    a, b = tidiag(d, lab, "pre"), tidiag(d, lab, "post")
    if a and b:
        r["ti_diag_delta"] = {f"{pg}.{k}": ((b[(pg, k)] - a[(pg, k)]) & 0xFFFFFFFF
                                            if isinstance(b.get((pg, k)), int) and isinstance(a.get((pg, k)), int)
                                            else None) for pg, k in TI_F}
    ses = {}
    for t in ("S0", "S1_lp", "S3_lp", "S4"):
        p = os.path.join(d, f"{lab}_{t}.json")
        if os.path.exists(p):
            ses[t] = jload(p)
    r["liveness"] = {t: s["liveness"]["verdict"] for t, s in ses.items()}
    pool = dict(valid=0, hfxo_running=0, constlat_valid=0, constlat_on=0)
    for t in ("S0", "S4"):
        sm = (ses.get(t) or {}).get("samples") or {}
        for k in pool:
            pool[k] += int(sm.get(k) or 0)
    if pool["valid"]:
        r["samples_pooled"] = dict(pool, hfxo_ci95_pct=[round(100 * x, 2) for x in
                                                         wilson(pool["hfxo_running"], pool["valid"])])
    if "S0" in ses and "S4" in ses:
        s0, s4 = snaps(ses["S0"]), snaps(ses["S4"])
        if s0 and s4:
            r["pd_S0_S4"] = rates(s0[0], s4[-1])
    if "S1_lp" in ses and "S3_lp" in ses:
        _, sa = g1_lpdecode.load(os.path.join(d, f"{lab}_S1_lp.json"))
        _, sb = g1_lpdecode.load(os.path.join(d, f"{lab}_S3_lp.json"))
        x, y = sa[0][1], sb[-1][1]
        delta = {k: y[k] - x[k] for k in g1_lpdecode.FIELDS if k not in g1_lpdecode.GAUGES}
        span = ses["S3_lp"]["host_end_unix"] - ses["S1_lp"]["host_start_unix"]
        r["lp_window"] = dict(delta=delta, host_span_s=round(span, 1),
                              sleeps_per_s=round(delta["sleeps"] / span, 3),
                              gauges_end={k: y[k] for k in g1_lpdecode.GAUGES if k in y})
    return r


def main():
    d, outj = sys.argv[1], sys.argv[2]
    res = {lab: arm(d, lab) for lab in sys.argv[3:]}
    with open(outj, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    print("| arm | PM loss % (pkts) | TI loss % (pkts) | TI RX_FAIL (host) | TI unaccounted | TI crc_fail | "
          "TI crc_repaired | TI crc_unrepairable | TI slots_missed | TI dBm | TI period µs | TI off-grid µs | "
          "hfxo_req % | XO sampled | lp faults |")
    print("|---" * 15 + "|")
    for lab, r in res.items():
        pm, ti, td = r.get("rx_promicro", {}), r.get("rx_ti", {}), r.get("ti_diag_delta", {})
        lw = r.get("lp_window")
        faults = None
        if lw:
            dl = lw["delta"]
            faults = sum(dl[k] for k in ("late_anchors", "late_wakes", "hfxo_late_edge2", "range_errors",
                                         "spurious", "hfxo_errors", "edge_order", "disagreements"))
        sp = r.get("samples_pooled") or {}
        print(f"| {lab} | {pm.get('loss_pct')} ({pm.get('packets')}) | {ti.get('loss_pct')} ({ti.get('packets')}) | "
              f"{ti.get('rx_fail')} | {ti.get('unaccounted')} | {td.get('backend_b.crc_fail')} | "
              f"{td.get('rx_integrity.crc_repaired')} | {td.get('rx_integrity.crc_unrepairable')} | "
              f"{td.get('api.slots_missed')} | {ti.get('rssi_mean')} | {ti.get('radio_period_us')} | "
              f"{ti.get('radio_resid_stdev_us')} | {(r.get('pd_S0_S4') or {}).get('hfxo_req_pct')} | "
              f"{sp.get('hfxo_running')}/{sp.get('valid')} | {faults} |")


if __name__ == "__main__":
    main()
