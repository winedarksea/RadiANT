"""C2 gate: one table across arms. For each LABEL in RUNDIR reads
LABEL_ti_console.log (TI power + rxpath lines), LABEL_verify_promicro.json
(ant_verify) and LABEL_acked_l15.json (c2_acked_probe) and prints the
per-arm figures side by side.

Usage: python c2_summary.py RUNDIR A1 B A2 [--skip-s 20]
Power lines before --skip-s of uptime are dropped (the first line after boot
is fine, but this keeps the arms alike). The TI fields are summed exactly as
gc1_summary.py does.
"""
import json
import os
import re
import statistics
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
TS = re.compile(r"\[(\d+):(\d+):(\d+)\.(\d+),(\d+)\]")
PWR = re.compile(r"power (dt_ms=.*)$")
RXP = re.compile(r"rxpath/(\d+)ms sched=(\d+) .*\| xfer=(\d+) dropped=(\d+) tot=(\d+)")
XF = re.compile(r"xfer failures: no_opener=(\d+) lost_track=(\d+) no_ack=(\d+) bad_ack=(\d+) radio=(\d+)")


def up_s(m):
    h, mi, s, ms, us = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1e3 + us / 1e6


def ti(path, skip_s):
    rows, rxp, xf, other = [], None, None, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = ANSI.sub("", raw).rstrip()
            t = TS.search(line)
            m = PWR.search(line)
            if m and t:
                kv = {}
                for tok in m.group(1).split():
                    k, v = tok.split("=")
                    kv[k] = int(v.split("/")[0])
                kv["uptime_s"] = up_s(t)
                if kv["uptime_s"] >= skip_s:
                    rows.append(kv)
                continue
            m = RXP.search(line)
            if m:
                rxp = dict(tot=int(m.group(5)), dropped=int(m.group(4)), xfer=int(m.group(3)),
                           uptime_s=up_s(t) if t else None)
                continue
            m = XF.search(line)
            if m:
                xf = dict(zip(("no_opener", "lost_track", "no_ack", "bad_ack", "radio"),
                              (int(x) for x in m.groups())))
                continue
            if ("<err>" in line or "<wrn>" in line) and "radiant_power" not in line:
                other.append(line.strip())
    dt_ms = sum(r["dt_ms"] for r in rows)
    dt = dt_ms / 1e3
    w = lambda k: sum(r[k] * r["dt_ms"] for r in rows) / dt_ms  # noqa: E731
    pu = sum(-(-r["rfpu_x10"] * r["dt_ms"] // 10000) for r in rows)
    pud = [r["pudur_us"] for r in rows if r["pudur_us"] >= 0]
    tx_ops = sum(r["tx_ops"] for r in rows)
    rx_ops = sum(r["rx_ops"] for r in rows)
    last, first = rows[-1], rows[0]
    rate = pu / dt
    return dict(
        lines=len(rows), seconds=round(dt, 1),
        uptime=(round(first["uptime_s"], 1), round(last["uptime_s"], 1)),
        tx_us_s=round(sum(r["tx_us"] for r in rows) / dt, 1), tx_ops_s=round(tx_ops / dt, 4),
        rx_us_s=round(sum(r["rx_us"] for r in rows) / dt, 1), rx_ops_s=round(rx_ops / dt, 4),
        rx_us_per_op=round(sum(r["rx_us"] for r in rows) / max(1, rx_ops), 1),
        rfon_pm_mean=round(w("rfon_pm"), 2), rfon_pm_min=min(r["rfon_pm"] for r in rows),
        rfon_pm_max=max(r["rfon_pm"] for r in rows),
        rfpu_s=round(rate, 4), rfpu_per_tx=round(pu / max(1, tx_ops), 4),
        rfpu_x10_min=min(r["rfpu_x10"] for r in rows), rfpu_x10_max=max(r["rfpu_x10"] for r in rows),
        rfon_per_pu_us=(round(w("rfon_pm") / 1e3 / rate * 1e6), round((w("rfon_pm") + 1) / 1e3 / rate * 1e6)),
        pudur_median=statistics.median(pud), pudur_min=min(pud), pudur_max=max(pud),
        pudur_hist={str(v): pud.count(v) for v in sorted(set(pud))},
        cpu_pm_mean=round(w("cpu_pm"), 2), cpu_pm_min=min(r["cpu_pm"] for r in rows),
        cpu_pm_max=max(r["cpu_pm"] for r in rows),
        idle_s=round(w("idle_x10") / 10, 2),
        final={k: last[k] for k in ("cold", "txl", "txl_max_us", "fsx", "bks", "bks_max", "wraps")},
        growth={k: last[k] - first[k] for k in ("cold", "txl", "fsx", "bks", "wraps")},
        ti_rx_frames_total=rxp["tot"] if rxp else 0, ti_rx_dropped=rxp["dropped"] if rxp else 0,
        ti_xfer_failures=xf, warnings=other[:20], n_warnings=len(other),
    )


PERIOD_S = 8070 / 32768


def verify(path):
    """The ant_verify figures this gate compares (heart-rate has no exact
    counter, so loss_pct against the period-derived expected count)."""
    with open(path, encoding="utf-8") as f:
        j = json.load(f)
    c = j["channels"][0]
    acc = j.get("accounting", {})
    return dict(packets=c["packets"], expected=round(c["expected_packets"], 1),
                loss_pct=round(c["loss_pct"], 2), rx_fail=acc.get("radio_fails"),
                unaccounted=acc.get("unaccounted"),
                jitter_stdev_ms=round(c["jitter"]["stdev_s"] * 1e3, 1),
                gap_max_ms=round(c["jitter"]["max_s"] * 1e3, 1),
                offgrid_host_ms=round(c["jitter"]["offgrid_host_s"] * 1e3, 2),
                offgrid_radio_ms=round(c["jitter"]["offgrid_radio_s"] * 1e3, 4),
                rssi_mean=round(c["signal"]["mean_dbm"], 1), rssi_min=c["signal"]["min_dbm"],
                rssi_max=c["signal"]["max_dbm"])


def rec(path):
    """Misses per minute and the lengths of consecutive-miss runs, from the
    receive timestamps in an ant_verify --record capture (host clock, so a
    gap is rounded to whole periods)."""
    ts = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            ts.append(float(line.split()[0]))
    runs, per_min = {}, {}
    for a, b in zip(ts, ts[1:]):
        k = round((b - a) / PERIOD_S) - 1
        if k > 0:
            runs[k] = runs.get(k, 0) + 1
            m = int(a // 60)
            per_min[m] = per_min.get(m, 0) + k
    return dict(miss_runs={str(k): runs[k] for k in sorted(runs)},
                misses_per_minute=[per_min.get(m, 0) for m in range(int(ts[-1] // 60) + 1)])


def main():
    d = sys.argv[1]
    skip = 20.0
    args = sys.argv[2:]
    if "--skip-s" in args:
        i = args.index("--skip-s")
        skip = float(args[i + 1])
        args = args[:i] + args[i + 2:]
    out = {}
    for lab in args:
        r = dict(ti=ti(os.path.join(d, f"{lab}_ti_console.log"), skip))
        p = os.path.join(d, f"{lab}_verify_promicro.json")
        if os.path.exists(p):
            r["verify_promicro"] = verify(p)
        p = os.path.join(d, f"{lab}_verify.rec")
        if os.path.exists(p):
            r["verify_promicro"].update(rec(p))
        p = os.path.join(d, f"{lab}_acked_l15.json")
        if os.path.exists(p):
            with open(p, encoding="utf-8") as f:
                a = json.load(f)
            a.pop("per_minute", None)
            exp = a["seconds"] / PERIOD_S
            a["broadcast_expected"] = round(exp, 1)
            a["broadcast_loss_pct"] = round(100.0 * (1 - a["broadcasts"] / exp), 2)
            # The TI's own count of frames it received (rxpath tot, since boot,
            # and the probe is the only originator) against what the probe sent.
            a["ti_heard_pct"] = round(100.0 * r["ti"]["ti_rx_frames_total"] / a["sent"], 2) if a["sent"] else None
            r["acked_l15"] = a
        out[lab] = r
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
