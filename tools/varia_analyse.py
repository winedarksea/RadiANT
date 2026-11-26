#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarise a bike radar / bike light capture (.antcap v1 or v2).

    python tools/varia_analyse.py archive/captures/radio/2026-11-25-varia-c2-idle-radiant.antcap

Per (device type, device number): message count and rate, period from the
receive timestamps, page histogram, the background-page interval (radar:
messages between successive non-48/49 pages, and their order), the light
channel's rotation pattern, every distinct non-idle radar target frame
decoded, and every page 1 / 2 / 70 / 87 seen. Loss is estimated from gaps
in t against the 4084-count period.
"""
from __future__ import annotations

import argparse
import statistics
import sys
from collections import Counter, defaultdict

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402

PERIOD_S = 4084 / 32768


def slots(records) -> list[int]:
    """Slot index of each record (0-based), from t against the period."""
    t0 = records[0].t
    return [round((r.t - t0) / PERIOD_S) for r in records]


def analyse(path: str, show_frames: int = 40) -> None:
    recs = [r for r in ap.read_capture_records(path)
            if r.kind is None or r.kind.isupper()]
    by = defaultdict(list)
    for r in recs:
        by[(r.device_type, r.device_number)].append(r)
    print(f"== {path}: {len(recs)} master-direction records")
    for (dtype, dnum), v in sorted(by.items()):
        span = v[-1].t - v[0].t
        idx = slots(v)
        expected = idx[-1] + 1
        gaps = [b.t - a.t for a, b in zip(v, v[1:], strict=False)]
        one = [g for g in gaps if 0.5 * PERIOD_S < g < 1.5 * PERIOD_S]
        per = statistics.mean(one) * 32768 if one else 0.0
        pages = Counter(r.payload[0] for r in v)
        rssi = [r.rssi for r in v if r.rssi is not None]
        print(f"\n0x{dtype:02X} #{dnum}: {len(v)} msgs over {span:.1f} s, "
              f"{len(v)}/{expected} slots ({100 * (1 - len(v) / expected):.2f}"
              f" % missed), period {per:.2f}"
              + (f", rssi median {statistics.median(rssi)}" if rssi else ""))
        print(f"  pages {dict(sorted(pages.items()))}")
        if dtype == ap.RDR_DEVICE_TYPE:
            bg = [(i, r.payload[0]) for i, r in zip(idx, v, strict=True)
                  if r.payload[0] not in (0x30, 0x31)]
            if len(bg) > 1:
                d = [b[0] - a[0] for a, b in zip(bg, bg[1:], strict=False)]
                print(f"  background slot spacing {dict(sorted(Counter(d).items()))}")
                order = "".join({0x50: "0", 0x51: "1", 0x52: "2"}.get(p, "?")
                                for _, p in bg)
                print(f"  background order (80=0 81=1 82=2): {order[:60]}")
            busy = [(r.t, r.payload) for r in v
                    if r.payload[0] in (0x30, 0x31) and any(r.payload[1:])]
            print(f"  non-idle target frames: {len(busy)}")
            seen = set()
            for t, p in busy:
                if p in seen:
                    continue
                seen.add(p)
                if len(seen) > show_frames:
                    break
                d = ap.decode(p, dtype)
                tgt = "; ".join(
                    f"T{x['index']} {ap.RDR_THREAT[x['threat']]} "
                    f"{ap.RDR_SIDE[x['side']]} {x['range_m']:.1f}m "
                    f"{x['speed_mps']}m/s" + ("" if x["valid"] else " (inv)")
                    for x in d["targets"] if x["threat"] or x["range_code"]
                    or x["speed_code"] or x["side"])
                print(f"    {t:9.3f} {p.hex()}  {tgt}")
            for r in v:
                if r.payload[0] in (0x01, 0x57):
                    print(f"  {r.t:9.3f} p{r.payload[0]} {r.payload.hex()} "
                          f"{ap.decode(r.payload, dtype)}")
        elif dtype == 0x23:
            seq = [r.payload[0] for r in v[:400]]
            # Shortest repeating unit over consecutive slots.
            consec = [r.payload[0] for i, r in enumerate(v[:200])
                      if i == 0 or idx[i] - idx[i - 1] == 1]
            print(f"  first pages: {' '.join(str(p) for p in seq[:24])}")
            for n in range(2, 13):
                if all(consec[i] == consec[i % n] for i in range(min(
                        len(consec), 60))):
                    print(f"  repeating unit ({n}): {consec[:n]}")
                    break
            for p, payload in sorted({r.payload[0]: r.payload
                                      for r in v}.items()):
                print(f"  p{p}: {payload.hex()}  ({sum(1 for r in v if r.payload == payload)}"
                      f" of {pages[p]} identical)")
    others = [r for r in ap.read_capture_records(path)
              if r.kind is not None and r.kind.islower()]
    if others:
        print(f"\nto-master records: {len(others)}")
        for r in others[:40]:
            print(f"  {r.t:9.3f} 0x{r.device_type:02X} #{r.device_number} "
                  f"{r.kind} {r.payload.hex()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--frames", type=int, default=40,
                        help="distinct non-idle target frames to print")
    args = parser.parse_args()
    for path in args.captures:
        analyse(path, args.frames)
    return 0


if __name__ == "__main__":
    sys.exit(main())
