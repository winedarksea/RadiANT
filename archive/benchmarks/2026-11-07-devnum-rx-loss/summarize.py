"""Summarise every window directory here: one row per receiver.

loss = RX_FAIL / (packets + RX_FAIL), i.e. the share of tracked slots the
receiver reported as missed, with a 95 % Wilson interval. The wall-clock
loss_pct from ant_verify is printed beside it as a cross-check.

    python summarize.py [window_dir ...]
"""
import json
import math
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent


def wilson(k, n, z=1.96):
    if n == 0:
        return (float("nan"), float("nan"))
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (100 * max(0.0, c - h), 100 * (c + h))


def rows(win):
    for js in sorted(win.glob("*.json")):
        if js.name == "window.json":
            continue
        try:
            d = json.loads(js.read_text())
        except (ValueError, OSError):
            continue
        ch = d["channels"][0]
        fails = d.get("events", {}).get("EVENT_RX_FAIL", 0)
        pk = ch["packets"]
        n = pk + fails
        lo, hi = wilson(fails, n)
        yield js.stem, pk, fails, (100 * fails / n if n else float("nan")), lo, hi, ch["loss_pct"], ch["signal"]["mean_dbm"] if ch.get("signal") else None


def main(argv):
    dirs = [pathlib.Path(a) for a in argv] or sorted(p for p in HERE.iterdir() if p.is_dir())
    for win in dirs:
        meta = win / "window.json"
        tx = ""
        if meta.exists():
            try:
                m = json.loads(meta.read_text(encoding="utf-8-sig"))
                tx = f"tx {m.get('tx')} dn {m.get('dn')} {m.get('seconds')} s"
            except ValueError:
                pass
        print(f"== {win.name}  {tx}")
        for name, pk, fails, loss, lo, hi, wall, rssi in rows(win):
            r = f"{rssi:6.1f}" if rssi is not None else "   n/a"
            print(f"   {name:12s} pk {pk:5d} rxfail {fails:3d}  loss {loss:5.2f} % [{lo:4.2f}, {hi:4.2f}]  wall {wall:5.2f} %  rssi {r}")


if __name__ == "__main__":
    main(sys.argv[1:])
