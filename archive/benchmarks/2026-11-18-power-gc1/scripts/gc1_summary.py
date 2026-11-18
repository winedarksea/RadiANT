"""Summarise a TI power-diag console capture (gc1_capture.py output).

Usage: python gc1_summary.py ti_console.log [--skip-s N]

Sums the per-line deltas (dt_ms, tx_us, rx_us, ops, idle) and the per-mille
fields weighted by dt_ms, converts rfpu_x10 back to power-ups per line, and
lists the uptime at which each cumulative counter (wraps, bks, cold, txl,
fsx) first reached each new value.
"""
import re
import statistics
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
LINE = re.compile(r"\[(\d+):(\d+):(\d+)\.(\d+),(\d+)\].*power (dt_ms=.*)$")


def parse(path, skip_s):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.search(ANSI.sub("", raw))
            if not m:
                continue
            h, mi, s, ms, us = (int(x) for x in m.groups()[:5])
            up = h * 3600 + mi * 60 + s + ms / 1e3 + us / 1e6
            kv = {}
            for tok in m.group(6).split():
                k, v = tok.split("=")
                if "/" in v:
                    v = v.split("/")[0]
                kv[k] = int(v)
            kv["uptime_s"] = up
            if up >= skip_s:
                rows.append(kv)
    return rows


def main():
    path = sys.argv[1]
    skip_s = 0.0
    if "--skip-s" in sys.argv:
        skip_s = float(sys.argv[sys.argv.index("--skip-s") + 1])
    rows = parse(path, skip_s)
    if not rows:
        print("no power lines")
        return 1
    dt_ms = sum(r["dt_ms"] for r in rows)
    dt_s = dt_ms / 1e3
    tot = {k: sum(r[k] for r in rows) for k in ("tx_us", "tx_ops", "rx_us", "rx_ops")}
    w = lambda k: sum(r[k] * r["dt_ms"] for r in rows) / dt_ms  # noqa: E731
    # rate_x10 = floor(n * 10000 / dt_ms), so n = ceil(x10 * dt_ms / 10000)
    # recovers the exact per-line count (the interval is ~1.0001 wide).
    pu = sum(-(-r["rfpu_x10"] * r["dt_ms"] // 10000) for r in rows)
    print(f"lines {len(rows)}  uptime {rows[0]['uptime_s']:.1f} .. {rows[-1]['uptime_s']:.1f} s  sum dt {dt_s:.1f} s")
    print(f"tx  {tot['tx_us'] / dt_s:.1f} us/s  {tot['tx_ops'] / dt_s:.4f} ops/s  {tot['tx_us'] / max(1, tot['tx_ops']):.1f} us/op")
    print(f"rx  {tot['rx_us'] / dt_s:.1f} us/s  {tot['rx_ops'] / dt_s:.4f} ops/s  {tot['rx_us'] / max(1, tot['rx_ops']):.1f} us/op")
    print(f"cpu_pm  mean {w('cpu_pm'):.2f}  min {min(r['cpu_pm'] for r in rows)}  max {max(r['cpu_pm'] for r in rows)}")
    print(f"idle/s  mean {w('idle_x10') / 10:.2f}")
    print(f"rfon_pm mean {w('rfon_pm'):.2f}  min {min(r['rfon_pm'] for r in rows)}  max {max(r['rfon_pm'] for r in rows)}")
    rate = pu / dt_s
    print(f"rfpu/s  {rate:.4f} ({pu} power-ups recovered from the floored x10 fields)  line min {min(r['rfpu_x10'] for r in rows) / 10}  max {max(r['rfpu_x10'] for r in rows) / 10}")
    print(f"power-ups per TX op  {pu / max(1, tot['tx_ops']):.4f}   per RX op  {pu / max(1, tot['rx_ops']):.4f}")
    # rfon_pm is floored per line: the true duty lies in [pm, pm + 1).
    lo, hi = w("rfon_pm") / 1e3, (w("rfon_pm") + 1) / 1e3
    print(f"rfon duty  {lo * 1e3:.2f}..{hi * 1e3:.2f} per mille (floored lines)   = {lo * 1e6:.0f}..{hi * 1e6:.0f} us/s")
    print(f"rfon per power-up  {lo / rate * 1e6:.0f}..{hi / rate * 1e6:.0f} us")
    pud = [r["pudur_us"] for r in rows if r["pudur_us"] >= 0]
    print(f"pudur_us  median {statistics.median(pud)}  min {min(pud)}  max {max(pud)}")
    last = rows[-1]
    print("final cumulative: " + " ".join(f"{k}={last[k]}" for k in ("fsx", "bks", "bks_max", "wraps", "cold", "txl", "txl_max_us")))
    for k in ("wraps", "bks", "cold", "txl", "fsx"):
        prev = rows[0][k]
        steps = [(rows[0]["uptime_s"], prev)] if prev else []
        for r in rows[1:]:
            if r[k] != prev:
                steps.append((r["uptime_s"], r[k]))
                prev = r[k]
        if steps:
            print(f"{k} changes (first line uptime at new value): " + ", ".join(f"{v}@{t:.1f}s" for t, v in steps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
