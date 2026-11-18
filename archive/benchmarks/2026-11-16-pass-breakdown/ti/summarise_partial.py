"""Table of the TI legs that did run (all with the TI scan dead early, see
README). TI diag levels after / counter deltas, 0xF6, Pro Micro loss, window
facts. Prints JSON to stdout.

    python summarise_partial.py > summary_partial.json
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, r"C:\wt_c2\tools")
import ant_diag  # noqa: E402

LEGS = [("aborted_leg1_scan_csum", "B1 -Os"), ("aborted_leg1b_scan_csum", "B1 -Os"),
        ("aborted_leg1c_cmd_stall", "B1 -Os"), ("leg4_preB1", "pre-B1 dd17fc1")]


def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def health(p):
    t = open(p, encoding="utf-8").read()
    try:
        return json.loads(t[t.index("{"):t.rindex("}") + 1])
    except ValueError:
        return None


out = []
for name, image in LEGS:
    d = os.path.join(HERE, name)
    meta = load(os.path.join(d, "meta.json"))
    row = {"leg": name, "image": image,
           "count_pre": {k: len(v) for k, v in meta.get("count_pre", {}).items() if not k.startswith("_")},
           "count_post": {k: len(v) for k, v in meta.get("count_post", {}).items() if not k.startswith("_")},
           "window": meta.get("window")}
    try:
        b = load(os.path.join(d, "ti_diag_before.json"))["pages"]
        a = load(os.path.join(d, "ti_diag_after.json"))["pages"]
        df = ant_diag.diff(b, a)
        row["ti_pages"] = {p: df[p]["fields"] for p in ("0", "3", "4", "5", "7", "8", "9", "10", "11", "12") if p in df}
    except (OSError, KeyError, ValueError) as exc:
        row["ti_pages"] = {"error": repr(exc)}
    row["ti_health_after"] = health(os.path.join(d, "ti_health_after.txt"))
    for scan in ("scan.json", "scan_pm.json"):
        p = os.path.join(d, scan)
        if os.path.exists(p):
            s = load(p)
            for label, c in s["dongles"].items():
                if "summary" in c:
                    row["pm_scan"] = {"seconds": c["summary"]["seconds"], "opens": c["summary"]["opens"],
                                      "tracked_loss_pct": {k: v["loss_pct"] for k, v in c["summary"]["tracked"].items()}}
    for scan in ("scan.txt", "scan_ti.txt"):
        p = os.path.join(d, scan)
        if os.path.exists(p):
            row["ti_scan_log"] = [ln.strip() for ln in open(p, encoding="utf-8") if not ln.startswith("$")
                                  and ("COM11" in ln or "RETRY" in ln)]
    out.append(row)
print(json.dumps(out, indent=1, default=str))
