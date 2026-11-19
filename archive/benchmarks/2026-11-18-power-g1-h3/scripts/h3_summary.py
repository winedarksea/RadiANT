"""G1 H3 per-arm table: receivers' loss, TI 0xF7 deltas, and (h3_ti_bits
receiver) the flipped-bit breakdown of repaired frames from page 12 fields
5-7 (diag/g1-h3 CONFIG_RADIANT_DIAG_CRC_BITS).
Usage: python h3_summary.py RUNDIR LABEL...   (markdown table)"""
import json, os, sys

d = sys.argv[1]
MODE = "bits"
if sys.argv[2] == "--crcbit":
    MODE = "crcbit"; del sys.argv[2]
F = [("api", "slots_missed"), ("backend_b", "crc_fail"), ("rx_integrity", "crc_repaired"),
     ("rx_integrity", "crc_unrepairable"), ("backend_a", "sync_entries")]


def tidiag(lab, tag):
    p = os.path.join(d, f"{lab}_tidiag_{tag}.json")
    if not os.path.exists(p):
        return None
    j = json.load(open(p))
    pages = j["pages"].values() if isinstance(j["pages"], dict) else j["pages"]
    out = {}
    for pg in pages:
        for k, v in pg["fields"].items():
            out[(pg["name"], k)] = v
    return out


def ver(lab, rx):
    p = os.path.join(d, f"{lab}_verify_{rx}.json")
    if not os.path.exists(p):
        return None
    return json.load(open(p))["channels"][0]


def bits(a, b):
    try:
        f5 = [a[("stale", "reserved_5")], b[("stale", "reserved_5")]]
        f6 = [a[("stale", "reserved_6")], b[("stale", "reserved_6")]]
        f7 = [a[("stale", "reserved_7")], b[("stale", "reserved_7")]]
    except (KeyError, TypeError):
        return None
    if not all(isinstance(x, int) for x in f5 + f6 + f7) or 0xFFFFFFFF in f5:
        return None
    h = lambda v, s, m: (v >> s) & m
    dl = lambda pair, s, m: (h(pair[1], s, m) - h(pair[0], s, m)) & m
    if MODE == "crcbit":
        # h3_ti_bits2: f6/f7 = syndrome bit pairs {0,1}..{14,15}; bit 0 = last bit on air
        return dict(rx1=dl(f5, 0, 0xFFFF), rx0=dl(f5, 16, 0xFFFF),
                    crc=[dl(f6, 8 * i, 0xFF) for i in range(4)] + [dl(f7, 8 * i, 0xFF) for i in range(4)])
    return dict(rx1=dl(f5, 0, 0xFFFF), rx0=dl(f5, 16, 0xFFFF), iso=dl(f6, 0, 0xFFFF),
                run=dl(f6, 16, 0xFFFF), pos=[dl(f7, 8 * i, 0xFF) for i in range(4)])


print("| arm | PM loss % | TI loss % | TI: " + " / ".join(k for _, k in F) +
      " | rx1 / rx0 | iso / run | pos b0-2 / b3-5 / b6-9 / crc | TI dBm |")
print("|---" * 8 + "|")
tot = {}
for lab in sys.argv[2:]:
    pm, ti = ver(lab, "promicro"), ver(lab, "ti")
    a, b = tidiag(lab, "pre"), tidiag(lab, "post")
    dd = " / ".join(str((b[f] - a[f]) & 0xFFFFFFFF) if a and b and isinstance(b.get(f), int)
                    and isinstance(a.get(f), int) else "-" for f in F)
    bt = bits(a, b) if a and b else None
    if bt and MODE == "crcbit":
        bs = f"{bt['rx1']} / {bt['rx0']} | - | CRC syndrome-bit pairs 0-1..14-15 (0 = last on air): " + " / ".join(map(str, bt["crc"]))
    else:
        bs = (f"{bt['rx1']} / {bt['rx0']} | {bt['iso']} / {bt['run']} | " +
              " / ".join(map(str, bt["pos"]))) if bt else "- | - | -"
    ls = lambda v: f"{v['loss_pct']:.2f}" if v else "-"
    dbm = f"{ti['signal']['mean_dbm']:.1f}" if ti else "-"
    print(f"| {lab} | {ls(pm)} | {ls(ti)} | {dd} | {bs} | {dbm} |")
