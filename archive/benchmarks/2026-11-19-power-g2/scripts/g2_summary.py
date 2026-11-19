"""Gate G2 summary: per arm, the sweep verdicts, burst outcomes, the trainer's reply
turnaround histograms, sampled XO.STAT/CONSTLAT, power-diag deltas (S0 -> S4) and the
lp_stats window (S1_lp -> S5_lp; decoded with g1_lpdecode's layout).
usage: python g2_summary.py RUNS_DIR [--json out.json]"""
import glob
import json
import os
import re
import sys

sys.path.insert(0, r"C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-18-power-g1\scripts")
from g1_lpdecode import decode, GAUGES, WRAPS  # noqa: E402

runs = sys.argv[1]
out_json = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--json" else None
arms = sorted({os.path.basename(p).split("_")[0] for p in glob.glob(os.path.join(runs, "*_plan.txt"))},
              key=lambda s: (s[1:], s[0]))


def turn(path):
    if not os.path.exists(path):
        return None
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"TURN n=(\d+) min=(-?\d+) max=(-?\d+) sum=(-?\d+) under=(\d+) over=(\d+) later=(\d+)", txt)
    if not m:
        return None
    n, mn, mx, s, un, ov, la = (int(x) for x in m.groups())
    h = {int(a): int(b) for a, b in re.findall(r"TURNH (\d+):(\d+)", txt)}
    return {"n": n, "min": mn, "max": mx, "mean": round(s / n, 2) if n else None,
            "under": un, "over": ov, "later": la, "hist": h}


def stats_line(path):
    if not os.path.exists(path):
        return None
    m = re.search(r"acked_frames=.*", open(path, encoding="utf-8").read())
    return m.group(0).strip() if m else None


def load(p):
    return json.load(open(p)) if os.path.exists(p) else None


res = {}
for arm in arms:
    r = {}
    sw = load(os.path.join(runs, f"{arm}_sweep.json"))
    if sw:
        r["sweep"] = sw["points"][0]["summary"]
        r["sweep"]["page71_advanced"] = sw["points"][0]["page71_advanced"]
    b = load(os.path.join(runs, f"{arm}_burst.json"))
    if b:
        r["burst"] = b["summary"]
    r["turn_sweep"] = turn(os.path.join(runs, f"{arm}_sim_sweep.txt"))
    r["turn_burst"] = turn(os.path.join(runs, f"{arm}_sim_burst.txt"))
    r["sim_stats_sweep"] = stats_line(os.path.join(runs, f"{arm}_sim_sweep.txt"))
    r["sim_stats_burst"] = stats_line(os.path.join(runs, f"{arm}_sim_burst.txt"))
    # sampled registers over every -Decode session
    hx = cl = n = 0
    for p in sorted(glob.glob(os.path.join(runs, f"{arm}_S[024]*.json"))):
        if "_lp" in p:
            continue
        s = load(p)["samples"]
        n += s["valid"]
        hx += s["hfxo_running"]
        cl += s["constlat_on"]
    r["sampled"] = {"n": n, "hfxo_running": hx, "constlat_on": cl}
    s0, s4 = load(os.path.join(runs, f"{arm}_S0.json")), load(os.path.join(runs, f"{arm}_S4.json"))
    if s0 and s4:
        f0 = s0["diag"]["snapshots"][0]["fields"]
        f4 = s4["diag"]["snapshots"][-1]["fields"]
        dt_ms = f4["uptime_ms"] - f0["uptime_ms"]
        d = {k: f4[k] - f0[k] for k in ("hfxo_req_us", "hfxo_requests", "hfxo_releases", "radio_tx_us",
                                       "radio_rx_us", "radio_tx_ops", "radio_rx_ops", "cpu_active_us",
                                       "idle_entries")}
        r["power_diag"] = {"window_s": dt_ms / 1000.0, **d,
                           "hfxo_req_pct": round(100.0 * d["hfxo_req_us"] / (dt_ms * 1000.0), 3),
                           "hfxo_us_per_request": round(d["hfxo_req_us"] / d["hfxo_requests"], 1)
                           if d["hfxo_requests"] else None,
                           "flags_end": hex(f4["flags"]), "constlat_stat_end": f4["constlat_stat"]}
    lps = []
    for tag in ("Sboot_lp", "S1_lp", "S3_lp", "S5_lp"):
        j = load(os.path.join(runs, f"{arm}_{tag}.json"))
        if j:
            for snap in j["diag"]["snapshots"]:
                if snap.get("ok"):
                    lps.append((tag, decode(snap["words"])))
    if lps:
        win = [x for x in lps if x[0] != "Sboot_lp"]
        first, last = win[0][1], win[-1][1]
        delta = {}
        for k, v in last.items():
            if k in GAUGES:
                delta[k] = v
            elif k in WRAPS:
                delta[k] = (v - first[k]) & 0xFFFFFFFF
            else:
                delta[k] = v - first[k]
        r["lp_boot"] = lps[0][1]
        r["lp_window"] = {"from": win[0][0], "to": win[-1][0], **delta}
    res[arm] = r

for arm, r in res.items():
    print(f"== {arm}")
    for k, v in r.items():
        if k in ("lp_boot",):
            continue
        print(f"  {k}: {v}")
if out_json:
    json.dump(res, open(out_json, "w"), indent=1)
