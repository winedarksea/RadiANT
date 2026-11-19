"""Decode radiant_nrf_lp_stats dumps from jlink_peek -Symbol radiant_nrf_lp_stats -Json
(layout: the header comment of radiant/src/radiant_radio_nrf_lp.c, 0x5C bytes, u32 LE).
Usage: python g1_lpdecode.py FILE.json [...]  -> prints each snapshot and the first->last delta."""
import json
import sys

FIELDS = ["magic", "size", "state", "sleeps", "wakes", "late_anchors", "late_wakes",
          "hfxo_late_edge2", "reanchors", "range_errors", "lf_taken", "timer_taken",
          "disagreements", "max_disagree_us", "max_wake_slack_us", "max_wake_late_us",
          "asap_wakes", "release_cancels", "spurious", "hfxo_errors", "refused",
          "last_disagree_q", "edge_order"]
STATES = {0: "off", 1: "awake", 2: "releasing", 3: "asleep", 4: "waking"}
GAUGES = {"magic", "size", "state", "max_disagree_us", "max_wake_slack_us",
          "max_wake_late_us", "refused", "last_disagree_q"}


def decode(words):
    d = {}
    for i, n in enumerate(FIELDS):
        v = int(words[i]) & 0xFFFFFFFF
        if n == "last_disagree_q" and v & 0x80000000:
            v -= 1 << 32
        d[n] = v
    return d


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        j = json.load(f)
    snaps = []
    for s in (j.get("diag") or {}).get("snapshots") or []:
        if s.get("words"):
            snaps.append((s.get("when"), decode(s["words"])))
    return j, snaps


def main():
    allsnaps = []
    for p in sys.argv[1:]:
        j, snaps = load(p)
        for when, d in snaps:
            allsnaps.append((p, when, j.get("host_start_unix"), d))
    out = {"snapshots": [], "delta": None}
    for p, when, t, d in allsnaps:
        ok = d["magic"] == 0x52504C31 and d["size"] == 0x5C
        print(f"{p} [{when}] magic_ok={ok} state={STATES.get(d['state'], d['state'])}")
        print("   " + " ".join(f"{k}={v}" for k, v in d.items() if k not in ("magic", "size")))
        out["snapshots"].append(dict(file=p, when=when, magic_ok=ok, **d))
    if len(allsnaps) >= 2:
        a, b = allsnaps[0][3], allsnaps[-1][3]
        delta = {k: b[k] - a[k] for k in FIELDS if k not in GAUGES}
        print("delta first->last: " + " ".join(f"{k}={v}" for k, v in delta.items()))
        out["delta"] = delta
    if "--json" in sys.argv:
        pass
    return out


if __name__ == "__main__":
    args = [x for x in sys.argv[1:] if not x.startswith("--out=")]
    outp = [x[6:] for x in sys.argv[1:] if x.startswith("--out=")]
    sys.argv = [sys.argv[0]] + args
    r = main()
    if outp:
        with open(outp[0], "w", encoding="utf-8") as f:
            json.dump(r, f, indent=1)
