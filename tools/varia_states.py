#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per radar in a capture: its page-1 (device status) episodes, and when its
radar and light channels were last heard. For the shutdown/abort sessions.

    python tools/varia_states.py archive/captures/radio/2026-11-25-varia-c5a-abort-radiant.antcap
"""
from __future__ import annotations

import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402

PERIOD_S = 4084 / 32768


def episodes(path: str) -> None:
    recs = ap.read_capture_records(path)
    devs = sorted({r.device_number for r in recs if r.device_type == 0x28})
    print(f"== {path}")
    for dn in devs:
        v = [r for r in recs if r.device_type == 0x28 and r.device_number == dn
             and (r.kind is None or r.kind.isupper())]
        eps = []
        for r in v:
            if r.payload[0] != 0x01:
                continue
            key = r.payload.hex()
            if eps and eps[-1]["key"] == key and r.t - eps[-1]["end"] < 3.0:
                eps[-1]["end"] = r.t
                eps[-1]["n"] += 1
            else:
                eps.append({"key": key, "start": r.t, "end": r.t, "n": 1})
        light = [r.t for r in recs if r.device_type == 0x23
                 and r.device_number == dn]
        print(f"  radar #{dn}: {v[0].t:.1f} .. {v[-1].t:.1f}; light "
              + (f"{light[0]:.1f} .. {light[-1]:.1f}" if light else "none"))
        for e in eps:
            d = ap.decode(bytes.fromhex(e["key"]), 0x28)
            span = e["end"] - e["start"]
            inwin = [r for r in v if e["start"] <= r.t <= e["end"]]
            slots = round(span / PERIOD_S) + 1
            print(f"    p1 {e['key']} state {ap.RDR_STATE[d['state']]!r} "
                  f"clear={d['clear_targets']} {e['start']:.2f}..{e['end']:.2f}"
                  f" ({span:.1f} s) n={e['n']}, {len(inwin)} frames heard "
                  f"of ~{slots} slots")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        episodes(p)
