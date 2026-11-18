"""Per-leg tracked loss, PM vs TI (and any scan_*.json in the leg dir). python loss.py <legdir>..."""
import json, os, sys, glob
for d in sys.argv[1:]:
    print("==", d)
    rows = {}
    for p in sorted(glob.glob(os.path.join(d, "scan_*.json"))):
        who = os.path.basename(p)[5:-5]
        s = json.load(open(p))
        for c in s["dongles"].values():
            if isinstance(c, dict) and "summary" in c:
                for k, v in c["summary"]["tracked"].items():
                    rows.setdefault(k, {})[who] = (v.get("received"), v.get("rx_fail"), v["loss_pct"])
    whos = sorted({w for r in rows.values() for w in r})
    print("%-12s" % "track" + "".join("%26s" % w for w in whos))
    for k in sorted(rows):
        print("%-12s" % k + "".join("%26s" % ("%s/%s %.2f%%" % rows[k][w] if w in rows[k] else "-") for w in whos))
