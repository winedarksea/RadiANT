"""FE-C acked TX against the REAL trainer #52233 (no apps/trainer sim).

tools/fec_ack_sweep.py needs apps/trainer on a DK (--sim-port is required),
and this bench step may flash only the TI, so the sim cannot be set up. Its
originator path also opens a UART at 115200 only, and the TI runs at 57600.
This script reuses fec_ack_sweep's own originator steps and send_one() /
page51_tagged() unchanged, against the real trainer, and records only what the
originator can see: TRANSFER_TX_COMPLETED / FAILED per command, plus page 71's
sequence advances (the trainer's own count of applied commands).

Without the sim's ground truth there is no window_missed / not_heard split:
a FAILED is either our command not heard by the trainer, or its ACK not heard
by us. It compares images, it does not attribute.

    python fec_acked_real.py (--serial SN | --port COMn --baud B) \
        --count 300 --interval 1.0 --json out.json
"""
import argparse
import json
import sys
import time

sys.path.insert(0, r"C:\wt_c2\tools")

import ant_probe  # noqa: E402
import fec_ack_sweep as fas  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial")
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--device", type=int, default=52233)
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--json")
    args = ap.parse_args()

    # fec_ack_sweep.open_originator() calls open_device(True, serial=, port=)
    # with the default baud; bind the baud here so the TI's 57600 is used.
    real_open = ant_probe.open_device

    def open_with_baud(verbose, wait_s=0.0, serial=None, port=None, baud=None):
        return real_open(verbose, wait_s, serial=serial, port=port,
                         baud=args.baud if baud is None else baud)

    ant_probe.open_device = open_with_baud

    t_start = time.time()
    dev, reader = fas.open_originator(args.device, args.serial, args.port)
    rows = []
    seqs: list[int] = []
    for i in range(args.count):
        t0 = time.monotonic()
        outcome = fas.send_one(dev, reader, fas.page51_tagged(i), seqs)
        rows.append({"i": i, "grade_raw": fas.tag_grade_raw(i),
                     "outcome": outcome, "t": time.time()})
        rest = args.interval - (time.monotonic() - t0)
        if rest > 0:
            fas.drain(reader, rest, seqs)
    fas.drain(reader, 3.0, seqs)
    counts: dict[str, int] = {}
    for r in rows:
        counts[r["outcome"]] = counts.get(r["outcome"], 0) + 1
    n = len(rows)
    ok = counts.get(fas.COMPLETED, 0)
    summary = {"n": n, "counts": counts,
               "completed_pct": round(100.0 * ok / n, 2) if n else None,
               "page71_seen": len(seqs),
               "page71_advanced": fas.page71_advance(seqs),
               "secs": round(time.time() - t_start, 1)}
    print(json.dumps(summary, indent=1))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"args": vars(args), "summary": summary, "rows": rows,
                       "page71_seqs": seqs}, f, indent=1)
    try:
        ant_probe.reset_stack(dev, reader)
    except Exception:  # noqa: BLE001
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
