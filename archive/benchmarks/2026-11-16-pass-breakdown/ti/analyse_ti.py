"""Summarise the C2-TI / C3 legs from the raw files in this directory.

    python analyse_ti.py leg1_Os leg2_O2 leg3_Os leg4_preB1 > summary.json

Per leg: TI and Pro Micro diag pages (before/after/diff; pages the image does
not have are skipped), 0xF6 for both, the same-window scan (both children),
per-master loss for the TI and the Pro Micro and TI-minus-Pro-Micro, in-window
accounting, master counts, elapsed times.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAGES = ("0", "3", "4", "7", "8", "9", "10", "11", "12")
sys.path.insert(0, r"C:\wt_c2\tools")
import ant_diag  # noqa: E402

PERIODS = {"0x0B": 8182, "0x78": 8070, "0x79": 8086, "0x11": 8192}


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def health(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    start = text.index("{")
    return json.loads(text[start:text.rindex("}") + 1])


def pages(d, who):
    before = load(os.path.join(d, f"{who}_diag_before.json"))["pages"]
    after = load(os.path.join(d, f"{who}_diag_after.json"))["pages"]
    diff = ant_diag.diff(before, after)
    rows = {}
    for p in PAGES:
        if p not in after or p not in before:
            continue
        rows[p] = {"name": after[p]["name"], "before": before[p]["fields"],
                   "after": after[p]["fields"],
                   "diff": diff.get(p, {}).get("fields")}
    return rows


def child_summary(c):
    s = c["summary"]
    acct = {}
    for k, v in s["tracked"].items():
        period = PERIODS[k.split(":")[1]]
        exp = s["seconds"] * 32768.0 / period
        acct[k] = {"expected": round(exp, 1),
                   "accounted": v["received"] + v["rx_fail"],
                   "short": round(exp - v["received"] - v["rx_fail"], 1)}
    return {"seconds": s["seconds"], "opens": s["opens"],
            "sightings_per_s": s["sightings_per_s"],
            "started_at": c.get("started_at"),
            "tracked": {k: {"received": v["received"], "rx_fail": v["rx_fail"],
                            "loss_pct": v["loss_pct"]}
                        for k, v in s["tracked"].items()},
            "events": s["events"],
            "devices": {k: {"per_s": v["per_s"],
                            "heard": f"{v['opens_heard']}/{v['opens']}"}
                        for k, v in s["devices"].items()},
            "accounting": acct}


def leg_summary(name):
    d = os.path.join(HERE, name)
    meta = load(os.path.join(d, "meta.json"))
    out = {"leg": name}
    out["ti"] = pages(d, "ti")
    out["pm"] = pages(d, "pm")
    out["health"] = {}
    for who in ("ti", "pm"):
        hb = health(os.path.join(d, f"{who}_health_before.txt"))
        ha = health(os.path.join(d, f"{who}_health_after.txt"))
        out["health"][who] = {"before": hb, "after": ha}
    out["scan_start_at"] = meta.get("start_at")
    kids = {}
    for who in ("ti", "pm"):
        scan = load(os.path.join(d, f"scan_{who}.json"))
        c = list(scan["dongles"].values())[0]
        kids[who] = child_summary(c) if "summary" in c else {"error": c.get("error")}
    with open(os.path.join(d, "scan_ti.txt"), encoding="utf-8") as f:
        out["ti_retries"] = [ln.strip() for ln in f if ln.startswith("RETRY")]
    out["scan"] = kids
    loss = {}
    if "tracked" in kids.get("ti", {}) and "tracked" in kids.get("pm", {}):
        for k in kids["ti"]["tracked"]:
            t = kids["ti"]["tracked"][k]
            p = kids["pm"]["tracked"].get(k)
            if p is None:
                continue
            tl, pl = t["loss_pct"], p["loss_pct"]
            loss[k] = {"ti": tl, "pm": pl,
                       "ti_minus_pm": (round(tl - pl, 2)
                                       if tl is not None and pl is not None else None)}
        # pooled over the 8 sims (trainer excluded)
        for who in ("ti", "pm"):
            r = sum(v["received"] for k, v in kids[who]["tracked"].items() if not k.startswith("52233"))
            f = sum(v["rx_fail"] for k, v in kids[who]["tracked"].items() if not k.startswith("52233"))
            loss[f"_pooled_sims_{who}"] = round(100.0 * f / (r + f), 2) if r + f else None
        a, b = loss["_pooled_sims_ti"], loss["_pooled_sims_pm"]
        loss["_pooled_sims_ti_minus_pm"] = round(a - b, 2) if a is not None and b is not None else None
    out["loss"] = loss
    out["window"] = meta.get("window")
    out["count_pre"] = {k: v for k, v in meta.get("count_pre", {}).items() if not k.startswith("_")}
    out["count_post"] = {k: v for k, v in meta.get("count_post", {}).items() if not k.startswith("_")}
    out["count_post_full"] = meta.get("count_post_full")
    out["sims"] = meta.get("sims")
    return out


def main():
    legs = [leg_summary(n) for n in sys.argv[1:]]
    print(json.dumps(legs, indent=1, default=str))


if __name__ == "__main__":
    main()
