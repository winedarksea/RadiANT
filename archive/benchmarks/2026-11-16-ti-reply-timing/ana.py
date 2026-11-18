"""Per-frame analysis of a promisc capture: reply offsets after the master frame."""
import re, sys, statistics as S
rx = re.compile(r'P n=(\d+) .*?t=(\d+) .*? (OK|BAD|\w+) m=\d+ rssi=(-?\d+) .*?B3=(\w\w) pl=(\w+) crc=(\w+)')
for path in sys.argv[1:]:
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = rx.search(line)
        if m:
            rows.append((int(m[2]), m[3], int(m[4]), m[5], m[6], line))
    last_master = None
    groups = {}
    seq = []
    for t, ok, rssi, b3, pl, line in rows:
        if b3 in ('0A', '8A'):
            last_master = t
            continue
        if last_master is None:
            continue
        d = t - last_master
        groups.setdefault(b3, []).append((d, rssi, ok, pl))
        seq.append((t, b3, d, rssi, ok, pl))
    print('==', path, 'frames', len(rows))
    for b3, v in sorted(groups.items()):
        ds = [x[0] for x in v]
        rs = [x[1] for x in v]
        print(f'  B3={b3} n={len(v)} off min/med/max={min(ds)}/{int(S.median(ds))}/{max(ds)} '
              f'rssi min/med/max={min(rs)}/{int(S.median(rs))}/{max(rs)} notOK={sum(1 for x in v if x[2]!="OK")}')
    if '-v' in sys.argv[0:1] or len(sys.argv) == 2:
        for t, b3, d, rssi, ok, pl in seq:
            print(f'   t={t} B3={b3} +{d} rssi={rssi} {ok} pl={pl}')
