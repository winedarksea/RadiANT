"""Per-arm table for the G1 diag runs: both receivers' loss and accounting,
the TI's 0xF7 counter deltas over the arm, and lp_stats for LP images.
Usage: python g1d_summary.py RUNDIR LABEL...  (prints a markdown table)"""
import json, os, sys

d = sys.argv[1]
F = [("api", "slots_missed"), ("backend_a", "term_empty"), ("backend_b", "crc_fail"),
     ("backend_b", "sw_addr_mismatch"), ("rx_integrity", "crc_repaired"),
     ("rx_integrity", "crc_unrepairable"), ("backend_a", "sync_entries"), ("backend_a", "rx_posted")]


def tidiag(lab, tag):
    p = os.path.join(d, f"{lab}_tidiag_{tag}.json")
    if not os.path.exists(p):
        return None
    out = {}
    for pg in json.load(open(p))["pages"].values() if isinstance(json.load(open(p))["pages"], dict) \
            else json.load(open(p))["pages"]:
        for k, v in pg["fields"].items():
            out[(pg["name"], k)] = v
    return out


def ver(lab, rx):
    p = os.path.join(d, f"{lab}_verify_{rx}.json")
    if not os.path.exists(p):
        return None
    j = json.load(open(p))
    ch = j["channels"][0]
    acc = {}
    def find(o):
        if isinstance(o, dict):
            for k in ("radio_fails", "unaccounted"):
                if k in o and k not in acc:
                    acc[k] = o[k]
            for v in o.values():
                find(v)
        elif isinstance(o, list):
            for v in o:
                find(v)
    find(j)
    return ch, acc


rows = []
print("| arm | PM loss % (pkts) | TI loss % (pkts) | TI RX_FAIL / unacc | TI diag: " +
      " / ".join(k for _, k in F) + " | TI dBm |")
print("|---" * 6 + "|")
for lab in sys.argv[2:]:
    pm = ver(lab, "promicro"); ti = ver(lab, "ti")
    a, b = tidiag(lab, "pre"), tidiag(lab, "post")
    dd = " / ".join(str((b[f] - a[f]) & 0xFFFFFFFF) if a and b and isinstance(b.get(f), int) and isinstance(a.get(f), int)
                    else "-" for f in F)
    def ls(v):
        return f"{v[0]['loss_pct']:.2f} ({v[0]['packets']})" if v else "-"
    print(f"| {lab} | {ls(pm)} | {ls(ti)} | "
          f"{ti[1].get('radio_fails','-')} / {ti[1].get('unaccounted','-')} | {dd} | "
          f"{ti[0]['signal']['mean_dbm']:.1f} |" if ti else f"| {lab} | {ls(pm)} | - | - | {dd} | - |")
