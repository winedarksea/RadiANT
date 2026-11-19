"""B5 rerun summary: per arm and receiver, loss/RSSI/period from g1_verify JSON, TI 0xF7 deltas,
and the pairwise coincident-miss counts (same slot-grid method as b5_miss_overlap.py).

Usage: python b5r_summary.py RUNDIR OUT_JSON LABEL [LABEL ...]
"""
import itertools
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "2026-11-18-power-g1", "scripts"))
import g1_summary  # noqa: E402

TI_KEYS = [("api", "slots_missed"), ("backend_b", "crc_fail"),
           ("rx_integrity", "crc_repaired"), ("rx_integrity", "crc_unrepairable")]


def ti_fields(path):
    j = g1_summary.jload(path)
    out = {}
    for pg in j["pages"].values():
        for k, v in pg.get("fields", {}).items():
            out[(pg["name"], k)] = v
    return out


def load_rec(path):
    t0 = None
    ts = []
    for line in open(path):
        if line.startswith("# listen_start_unix="):
            t0 = float(line.split("=")[1])
        elif not line.startswith("#") and line.strip():
            ts.append(t0 + float(line.split()[0]))
    return ts


def overlap(recs, period_s):
    ref = min(v[0] for v in recs.values())

    def slots(ts):
        raw = [(t - ref) / period_s for t in ts]
        ph = sorted(x - round(x) for x in raw)[len(raw) // 2]
        return {round(x - ph) for x in raw}

    s = {k: slots(v) for k, v in recs.items()}
    lo = max(min(v) for v in s.values())
    hi = min(max(v) for v in s.values())
    n = hi - lo + 1
    miss = {k: {i for i in range(lo, hi + 1) if i not in v} for k, v in s.items()}
    res = {"slots": n, "missed": {k: len(v) for k, v in miss.items()}, "pairs": {}}
    for a, b in itertools.combinations(sorted(miss), 2):
        both = miss[a] & miss[b]
        res["pairs"][f"{a}+{b}"] = {"both": len(both),
                                    "independent_expectation": round(len(miss[a]) * len(miss[b]) / n, 2)}
    if len(miss) >= 3:
        allm = set.intersection(*miss.values())
        res["all"] = len(allm)
    return res


def main():
    rundir, outp, labels = sys.argv[1], sys.argv[2], sys.argv[3:]
    summ = {}
    for lab in labels:
        arm = {}
        recs = {}
        for rx in ("l15", "ti", "garmin"):
            jp = os.path.join(rundir, f"{lab}_verify_{rx}.json")
            if not os.path.exists(jp):
                continue
            try:
                arm[rx] = g1_summary.verify(jp)
            except Exception as e:  # noqa: BLE001
                arm[rx] = {"error": repr(e)}
            rp = os.path.join(rundir, f"{lab}_verify_{rx}.rec")
            if os.path.exists(rp):
                recs[rx] = load_rec(rp)
        try:
            pre = ti_fields(os.path.join(rundir, f"{lab}_tidiag_pre.json"))
            post = ti_fields(os.path.join(rundir, f"{lab}_tidiag_post.json"))
            arm["ti_diag"] = {f"{p}.{k}": post[(p, k)] - pre[(p, k)]
                              for p, k in TI_KEYS if (p, k) in pre and (p, k) in post}
        except Exception as e:  # noqa: BLE001
            arm["ti_diag"] = {"error": repr(e)}
        per = (arm.get("ti") or {}).get("radio_period_us") or 246270.0
        arm["overlap"] = overlap(recs, per / 1e6) if len(recs) >= 2 else None
        summ[lab] = arm
    with open(outp, "w") as f:
        json.dump(summ, f, indent=1)
    for lab, arm in summ.items():
        for rx in ("l15", "ti", "garmin"):
            r = arm.get(rx)
            if r and "error" not in r:
                print(f"{lab:3} {rx:6} loss {r['loss_pct']:6.3f}% pkts {r['packets']} "
                      f"rxfail {r['rx_fail']} unacc {r['unaccounted']} rssi {r.get('rssi_mean')} "
                      f"period {r.get('radio_period_us')}")
            elif r:
                print(f"{lab:3} {rx:6} {r['error']}")
        print(f"{lab:3} ti_diag {arm['ti_diag']}")
        print(f"{lab:3} overlap {arm['overlap']}")


if __name__ == "__main__":
    main()
