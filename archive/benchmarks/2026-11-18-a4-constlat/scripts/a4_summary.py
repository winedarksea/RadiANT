#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarise the A4 CONSTLAT recheck's jlink_peek JSON files.

    python scripts/a4_summary.py <folder>

For each run: CONSTLATSTAT on-fraction (95 % Wilson), the m_power_mode_refs
histogram joint with CONSTLAT, and the radio RX duty from the two
radiant_power_diag snapshots (radio_rx_us delta / uptime_ms delta). Writes
a4_constlat_summary.json and a4_constlat_summary.txt into the folder.
"""

import json
import sys
from pathlib import Path

RUNS = [
    # (file stem, image, case)
    ("nohold_idle", "hold=n", "idle, no channel"),
    ("nohold_scan_peek1", "hold=n", "background scan 0x40, early"),
    ("nohold_scan_peek2", "hold=n", "background scan 0x40, late"),
    ("nohold_track_peek", "hold=n", "wildcard slave, tracking"),
    ("hold_idle", "hold=y", "idle, no channel"),
    ("hold_scan_peek1", "hold=y", "background scan 0x40, early"),
    ("hold_scan_peek2", "hold=y", "background scan 0x40, late (600 samples)"),
    ("hold_postscan_idle", "hold=y", "idle after the scan closed"),
    ("hold_track_peek", "hold=y", "wildcard slave, tracking"),
    ("hold_posttrack_idle", "hold=y", "idle after tracking closed"),
]

# radiant_power_diag v1 word indices (all u32)
UPTIME_MS, RX_US, RX_OPS = 4, 9, 11


def main() -> int:
    folder = Path(sys.argv[1])
    out = []
    lines = []
    for stem, image, case in RUNS:
        d = json.loads((folder / f"{stem}.json").read_text())
        s = d["samples"]
        snaps = d["diag"]["snapshots"]
        w0, w1 = snaps[0]["words"], snaps[-1]["words"]
        dt_ms = w1[UPTIME_MS] - w0[UPTIME_MS]
        rx_us = w1[RX_US] - w0[RX_US]
        rx_ops = w1[RX_OPS] - w0[RX_OPS]
        hist = {h["value"]: (h["count"], h["constlat_on"]) for h in d["extra"]["histogram"]}
        rec = {
            "run": stem, "image": image, "case": case,
            "liveness": d["liveness"]["verdict"],
            "uptime_ms": [w0[UPTIME_MS], w1[UPTIME_MS]],
            "samples": s["valid"],
            "constlat_on": s["constlat_on"],
            "constlat_ci95": [round(x, 4) for x in s["constlat_ci95"]],
            "rx_duty": round(rx_us / (dt_ms * 1000.0), 5) if dt_ms else None,
            "rx_ops_per_s": round(rx_ops * 1000.0 / dt_ms, 3) if dt_ms else None,
            "rx_us_per_op": round(rx_us / rx_ops, 1) if rx_ops else None,
            "refs_hist": {str(k): {"samples": v[0], "constlat_on": v[1]} for k, v in sorted(hist.items())},
            "refs_max": max(hist),
        }
        out.append(rec)
        hs = ", ".join(f"refs={k}: {v[0]} ({v[1]} CONSTLAT on)" for k, v in sorted(hist.items()))
        lines.append(
            f"{image:7s} {case:42s} up {w0[UPTIME_MS]/1000:6.1f}-{w1[UPTIME_MS]/1000:6.1f} s  "
            f"CONSTLAT {s['constlat_on']}/{s['valid']} "
            f"({100*s['constlat_ci95'][0]:.1f}-{100*s['constlat_ci95'][1]:.1f} %)  "
            f"RX duty {100*rec['rx_duty']:.2f} %  {rec['rx_ops_per_s']} ops/s  | {hs}")
    (folder / "a4_constlat_summary.json").write_text(json.dumps(out, indent=1) + "\n")
    (folder / "a4_constlat_summary.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
