"""Summarise the B4 pair legs from the raw files in this directory (C2 analyse.py
without the Garmin section).

    python analyse.py leg1_A leg2_B leg3_A leg4_B > summary.json
"""
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAGES = ("0", "2", "3", "4", "7", "8", "9", "10", "11", "12")  # C2 set plus 2, 3


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def health(path):
    """ant_health --json output, after the '$ cmd' header line."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    start = text.index("{")
    return json.loads(text[start:text.rindex("}") + 1])


def find(obj, key):
    if isinstance(obj, dict):
        if key in obj:
            return obj[key]
        for v in obj.values():
            r = find(v, key)
            if r is not None:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = find(v, key)
            if r is not None:
                return r
    return None


PERIODS = {"0x0B": 8182, "0x78": 8070, "0x79": 8086, "0x11": 8192}


def scan_periods(scan, key):
    return PERIODS[key.split(":")[1]]


def leg_summary(name):
    d = os.path.join(HERE, name)
    meta = load(os.path.join(d, "meta.json"))
    before = load(os.path.join(d, "diag_before.json"))["pages"]
    after = load(os.path.join(d, "diag_after.json"))["pages"]
    out = {"leg": name}
    rows = {}
    from_tools = os.path.join(r"C:\wt_c2\tools")
    sys.path.insert(0, from_tools)
    import ant_diag  # noqa: E402
    diff = ant_diag.diff(before, after)
    for p in PAGES:
        rows[p] = {"name": after[p]["name"], "before": before[p]["fields"],
                   "after": after[p]["fields"], "diff": diff[p]["fields"]}
    out["pages"] = rows
    p9 = after["9"]["fields"]
    b4 = None
    if all(isinstance(p9[k], int) for k in ("pass_to_arm_max_us", "sweep_max_us", "decide_max_us")):
        b4 = p9["pass_to_arm_max_us"] - p9["sweep_max_us"] - p9["decide_max_us"]
    out["b4_estimate_us"] = b4
    w5 = p9["pass_to_arm_max_us"]
    out["c4_candidate_us"] = (int(math.ceil((1.25 * w5 + 20) / 10.0)) * 10
                              if isinstance(w5, int) else None)
    hb = health(os.path.join(d, "health_before.txt"))
    ha = health(os.path.join(d, "health_after.txt"))
    out["health"] = {k: {"before": find(hb, k), "after": find(ha, k)}
                     for k in ("tx_high_water", "tx_drops")}
    scan = load(os.path.join(d, "scan_pm.json"))
    pm = list(scan["dongles"].values())[0]
    s = pm["summary"]
    out["scan"] = {"seconds": s["seconds"], "opens": s["opens"],
                   "sightings_per_s": s["sightings_per_s"],
                   "started_at": pm.get("started_at"),
                   "tracked": {k: {"received": v["received"],
                                   "rx_fail": v["rx_fail"],
                                   "loss_pct": v["loss_pct"]}
                               for k, v in s["tracked"].items()},
                   "events": s["events"],
                   "devices": {k: {"per_s": v["per_s"],
                                   "heard": f"{v['opens_heard']}/{v['opens']}"}
                               for k, v in s["devices"].items()}}
    # In-window accounting: received + RX_FAIL against the slots the master
    # sent in the window (window seconds x 32768 / period). A shortfall here
    # would be events lost after the radio (e.g. USB tx_drops) in the window.
    acct = {}
    for k, v in s["tracked"].items():
        period = scan_periods(scan, k)
        exp = s["seconds"] * 32768.0 / period
        acct[k] = {"period": period, "expected": round(exp, 1),
                   "accounted": v["received"] + v["rx_fail"],
                   "short": round(exp - v["received"] - v["rx_fail"], 1)}
    out["scan"]["accounting"] = acct
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
