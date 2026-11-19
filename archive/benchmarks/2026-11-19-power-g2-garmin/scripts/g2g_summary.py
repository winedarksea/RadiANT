"""Gate G2 Garmin rerun summary: per arm and phase, the trainer's reply-turnaround stats
(`ackfault turn`; mean = sum/n over every value < 3000 us, so values above the 2150-2229 us
histogram are still in the mean), sweep verdicts, burst outcomes, and for B arms the HFXO
duty (S0 -> S4) and the lp_stats window (S1_lp -> S5_lp, G1's decoder).
usage: python g2g_summary.py RUNS_DIR [--json out.json]"""
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
PHASES = ("gsweep", "gburst", "tsweep", "tburst")


def load(p):
    return json.load(open(p)) if os.path.exists(p) else None


def turn(path):
    if not os.path.exists(path):
        return None
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"TURN n=(\d+) min=(-?\d+) max=(-?\d+) sum=(-?\d+) under=(\d+) over=(\d+) later=(\d+)", txt)
    if not m:
        return None
    n, mn, mx, s, un, ov, la = (int(x) for x in m.groups())
    h = {int(a): int(b) for a, b in re.findall(r"TURNH (\d+):(\d+)", txt)}
    r = {"n": n, "min": mn, "max": mx, "mean": round(s / n, 2) if n else None,
         "under": un, "over": ov, "later": la, "hist": h}
    # median from the histogram when it is inside the binned range
    if n and un < n / 2 and un + sum(h.values()) >= n / 2:
        acc = un
        for k in sorted(h):
            acc += h[k]
            if acc >= n / 2:
                r["median"] = k
                break
    inband = sum(v for k, v in h.items() if 2186 <= k <= 2191)
    r["in_2186_2191"] = inband
    r["in_2186_2191_pct"] = round(100.0 * inband / n, 1) if n else None
    acked = re.search(r"acked_frames=.*", txt)
    r["stats"] = acked.group(0).strip() if acked else None
    return r


res = {}
for arm in arms:
    r = {}
    for ph in PHASES:
        p = {}
        j = load(os.path.join(runs, f"{arm}_{ph}.json"))
        if j and ph.endswith("sweep"):
            p["sweep"] = j["points"][0]["summary"]
            p["sweep"]["page71_advanced"] = j["points"][0].get("page71_advanced")
        elif j:
            p["burst"] = j["summary"]
        t = turn(os.path.join(runs, f"{arm}_sim_{ph}.txt"))
        if t:
            p["turn"] = t
        if p:
            r[ph] = p
    s0, s4 = load(os.path.join(runs, f"{arm}_S0.json")), load(os.path.join(runs, f"{arm}_S4.json"))
    if s0 and s4:
        f0 = s0["diag"]["snapshots"][0]["fields"]
        f4 = s4["diag"]["snapshots"][-1]["fields"]
        dt_ms = f4["uptime_ms"] - f0["uptime_ms"]
        d = {k: f4[k] - f0[k] for k in ("hfxo_req_us", "hfxo_requests", "radio_tx_ops", "radio_rx_ops")}
        r["power_diag"] = {"window_s": dt_ms / 1000.0, **d,
                           "hfxo_req_pct": round(100.0 * d["hfxo_req_us"] / (dt_ms * 1000.0), 3),
                           "flags_end": hex(f4["flags"])}
        hx = n = 0
        for sj in (s0, s4):
            n += sj["samples"]["valid"]
            hx += sj["samples"]["hfxo_running"]
        r["sampled_xo"] = f"{hx}/{n}"
    lps = []
    for tag in ("S1_lp", "S5_lp"):
        j = load(os.path.join(runs, f"{arm}_{tag}.json"))
        if j:
            for snap in j["diag"]["snapshots"]:
                if snap.get("ok"):
                    lps.append(decode(snap["words"]))
    if len(lps) >= 2:
        first, last = lps[0], lps[-1]
        delta = {}
        for k, v in last.items():
            if k in GAUGES:
                delta[k] = v
            elif k in WRAPS:
                delta[k] = (v - first[k]) & 0xFFFFFFFF
            else:
                delta[k] = v - first[k]
        r["lp_window"] = delta
    res[arm] = r

for arm, r in res.items():
    print(f"== {arm}")
    for ph in PHASES:
        if ph not in r:
            continue
        p = r[ph]
        t = p.get("turn", {})
        line = (f"  {ph}: turn n={t.get('n')} mean={t.get('mean')} med={t.get('median')} "
                f"min={t.get('min')} max={t.get('max')} over={t.get('over')} later={t.get('later')} "
                f"band={t.get('in_2186_2191')}")
        if "sweep" in p:
            s = p["sweep"]
            line += f" | sweep {json.dumps(s)}"
        if "burst" in p:
            line += f" | burst {p['burst']['outcomes']} ev {p['burst']['other_events']}"
        print(line)
        print(f"     hist {t.get('hist')}")
    for k in ("power_diag", "sampled_xo", "lp_window"):
        if k in r:
            print(f"  {k}: {r[k]}")
if out_json:
    json.dump(res, open(out_json, "w"), indent=1)
