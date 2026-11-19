"""Do the two receivers miss the SAME slots? Align each receiver's packets on the
transmitter's slot grid by host time (P = the measured radio period), then compare
the missed-slot sets over the common span. Coincident misses beyond chance point at
the transmitter; independent misses at the links.
Usage: python b5_miss_overlap.py LABEL PERIOD_US [RXa RXb]"""
import sys
lab, P = sys.argv[1], float(sys.argv[2]) / 1e6
rxs = sys.argv[3:5] or ["l15", "ti"]
def load(rx):
    t0 = None; ts = []
    for line in open(f"{lab}_verify_{rx}.rec"):
        if line.startswith("# listen_start_unix="):
            t0 = float(line.split("=")[1])
        elif not line.startswith("#") and line.strip():
            ts.append(t0 + float(line.split()[0]))
    return ts
a, b = load(rxs[0]), load(rxs[1])
ref = a[0]
# per-receiver phase: median residual so host latency offsets cancel
def slots(ts):
    raw = [(t - ref) / P for t in ts]
    ph = sorted(x - round(x) for x in raw)[len(raw) // 2]
    return {round(x - ph) for x in raw}
sa, sb = slots(a), slots(b)
lo, hi = max(min(sa), min(sb)), min(max(sa), max(sb))
n = hi - lo + 1
ma = {k for k in range(lo, hi + 1) if k not in sa}
mb = {k for k in range(lo, hi + 1) if k not in sb}
both = ma & mb
exp = len(ma) * len(mb) / n
print(f"{lab}: slots {n}, {rxs[0]} missed {len(ma)}, {rxs[1]} missed {len(mb)}, both {len(both)} (independent expectation {exp:.2f})")
def runs(m):
    s = sorted(m); r = []; c = 1
    for i in range(1, len(s)):
        if s[i] == s[i-1] + 1: c += 1
        else: r.append(c); c = 1
    if s: r.append(c)
    return max(r) if r else 0, sum(1 for x in r if x > 1)
for nm, m in ((rxs[0], ma), (rxs[1], mb)):
    mx, multi = runs(m)
    per_min = [0] * 11
    for k in m: per_min[min(10, int((k - lo) * P // 60))] += 1
    print(f"  {nm}: longest run {mx}, runs>1 {multi}, per minute {per_min}")
