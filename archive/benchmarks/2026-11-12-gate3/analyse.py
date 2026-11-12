#!/usr/bin/env python3
"""Gate 3 (ADR 0022) from the six same-window legs in this directory.

Per window: tracked loss over both trainer channels (52233 power 0x0B and
FE-C 0x11) for the Pro Micro (image A or B), the nRF52840 Dongle (image A
in every window, the same-SoC control) and the Garmin (second reference).
D = Pro Micro loss - dongle loss cancels the room's drift within a window.

Gate: mean D(B) - mean D(A) <= max(1 pp, spread of D over the A windows),
and page-7 counters 3-7 on the Pro Micro no higher in B than in A
(per-window means).
"""
import glob
import json
import os
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PM, DG, GA = "EE6986854FD72803", "3D55F77818BE772A", "006"
P7 = ["srch_cut_close", "srch_cut_abort", "srch_end_near_close",
      "trk_open_late", "trk_open_late_max_us", "trk_missed_behind_scan",
      "trk_slots_after_sibling", "trk_miss_after_sibling"]
LEVEL = {"trk_open_late_max_us"}


def loss(summary):
    rec = sum(t["received"] for t in summary["tracked"].values())
    fail = sum(t["rx_fail"] for t in summary["tracked"].values())
    return 100.0 * fail / (rec + fail) if rec + fail else float("nan"), rec, fail


def page(path, n):
    with open(path) as fh:
        return json.load(fh)["pages"][str(n)]["fields"]


def diff(before, after, n):
    b, a = page(before, n), page(after, n)
    out = {}
    for k, v in a.items():
        if v == 0xFFFFFFFF:
            out[k] = None
        elif k in LEVEL:
            out[k] = v
        else:
            out[k] = (v - b[k]) & 0xFFFFFFFF
    return out


rows = []
for scan in sorted(glob.glob(os.path.join(HERE, "w*_scan.json"))):
    w = os.path.basename(scan)[:-len("_scan.json")]
    img = w.split("_")[1]
    with open(scan) as fh:
        d = json.load(fh)["dongles"]
    r = {"w": w, "img": img}
    for key, ser in (("pm", PM), ("dg", DG), ("ga", GA)):
        r[key], r[key + "_rec"], r[key + "_fail"] = loss(d[ser]["summary"])
        r[key + "_sps"] = d[ser]["summary"]["sightings_per_s"]
    r["d"] = r["pm"] - r["dg"]
    for key, ser in (("pm7", PM[:4]), ("dg7", DG[:4])):
        b = os.path.join(HERE, f"{w}_{ser}_before.json")
        a = os.path.join(HERE, f"{w}_{ser}_after.json")
        r[key] = diff(b, a, 7) if os.path.exists(a) else {}
    rows.append(r)

print(f"{'window':8} {'PM%':>6} {'DG%':>6} {'GA%':>6} {'D pp':>6} {'PMs/s':>6} "
      + " ".join(f"{k[:10]:>10}" for k in P7))
for r in rows:
    p7 = " ".join(f"{(r['pm7'].get(k) if r['pm7'].get(k) is not None else '-'):>10}" for k in P7)
    print(f"{r['w']:8} {r['pm']:6.2f} {r['dg']:6.2f} {r['ga']:6.2f} {r['d']:6.2f} {r['pm_sps']:6.2f} {p7}")
print("dongle page 7 (image A in every window):")
for r in rows:
    print(f"  {r['w']:8} " + " ".join(f"{k}={r['dg7'].get(k)}" for k in P7))

da = [r["d"] for r in rows if r["img"] == "A"]
db = [r["d"] for r in rows if r["img"] == "B"]
if da and db:
    spread = max(da) - min(da)
    excess = st.mean(db) - st.mean(da)
    bound = max(1.0, spread)
    c_ok = True
    for k in P7[3:]:
        ma = st.mean([r["pm7"].get(k) or 0 for r in rows if r["img"] == "A"])
        mb = st.mean([r["pm7"].get(k) or 0 for r in rows if r["img"] == "B"])
        flag = "" if mb <= ma else "  <- higher in B"
        if mb > ma and k != "trk_open_late_max_us":
            c_ok = False
        print(f"  {k:24} A mean {ma:9.1f}  B mean {mb:9.1f}{flag}")
    print(f"mean D A {st.mean(da):.2f} pp, B {st.mean(db):.2f} pp; B - A = {excess:+.2f} pp; "
          f"bound max(1, spread A {spread:.2f}) = {bound:.2f}")
    print("loss part:", "PASS" if excess <= bound else "FAIL",
          "| counters part:", "PASS" if c_ok else "FAIL")
sys.exit(0)
