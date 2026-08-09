#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Turn a Spike B capture into the control-byte table, and refuse to if it is incomplete.

Provenance: clean-room. It parses the output of `radiant_core/spike/promisc`, which
is this repository's own code, and nothing else. Nothing here derives from
sdk-ant, from libant.a, or from an adopter-gated ANT+ device profile document.

Two jobs, and the first one is the reason this exists as a program rather than a
grep.

**It proves the capture is complete before it reports anything.** The part 1
result was a histogram, where a lost frame costs a count. The part 2 result is a
*sequence* - packet 0, then 1, then 2, then the last one - and a silently
dropped frame in the middle of one reads as an encoding that skips a value. So
every P line carries `dr=`, the sniffer's ring-drop counter as it stood when
that record was made, and this script fails the run if it ever increments.
`--strict` turns that from a printed complaint into a non-zero exit, which is
what the documented gate uses.

**Then it decodes.** Byte 3 is printed as its four measured fields rather than
as a hex value, the exchanges are reassembled so the burst sequence walk is
visible as a line rather than as forty scattered ones, and the two turnaround
figures are computed from the microsecond timestamps.

Direction is decided by payload identity, never by timing or by RSSI:
`spike_b_drive.py` marks every block it sends in byte 0 (0xA0 acknowledged,
0xB0 broadcast, 0xC0 burst) with its own counter in byte 1, and `sim/` transmits
ANT+ bicycle-power pages, which start with a page number. So a frame says which
board sent it.

    py spike_b_analyse.py archive/captures/radio/2026-08-09-spike-b2-runA.log --strict
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys

P_LOOSE = re.compile(
    r"^P n=(\d+) dr=(\d+) q=(\d+) t=(\d+) dt=\s*([-+]?\d+) (OK|ER) m=(\d+) "
    r"rssi=(-?\d+) dn=(\d+) ty=([0-9A-F]{2})(!?) tt=(\d+) B3=([0-9A-F]{2}) "
    r"pl=([0-9A-F]{16}) crc=([0-9A-F]{4}) air=(\d+)$"
)

S_RE = re.compile(r"^S t=(\d+) ours=(\d+) foreign=(\d+) longframe=(\d+) "
                  r"oddtype=(\d+) crc_ok=(\d+) crc_err=(\d+) addr=(\d+) "
                  r"end=(\d+) dropped=(\d+)")

# Marker bytes spike_b_drive.py writes into byte 0 of every block it sends.
DRIVER_MARKS = {0xA0: "ack", 0xB0: "bcast", 0xC0: "burst"}

# An advanced-burst block is 24 bytes on the serial link and reaches the air as
# three 8-byte packets, so two thirds of them start with the *filler* byte the
# driver uses for the second and third octets - mark ^ 0xFF - rather than with
# the mark. Without this table those fragments read as if the master had sent
# them, which turns the single most informative capture in the set (a 17-block
# advanced burst, 51 on-air packets) into nonsense.
CONT_MARKS = {0x5F: "ack", 0x4F: "bcast", 0x3F: "burst"}

# An exchange is a run of frames closer together than this. The master's slot is
# 249.7 ms and the widest in-slot gap measured is about 3.2 ms, so anything in
# between separates exchanges unambiguously; 20 ms is in the middle of that gap
# by two orders of magnitude and the choice is therefore not delicate.
EXCHANGE_GAP_US = 20000


class Frame:
    __slots__ = ("n", "dr", "q", "t", "dt", "ok", "rssi", "b3", "pl", "air",
                 "odd", "line")

    def __init__(self, m: re.Match, line: str):
        (n, dr, q, t, dt, crc, _m, rssi, _dn, _ty, odd, _tt, b3, pl, _c16,
         air) = m.groups()
        self.n = int(n)
        self.dr = int(dr)
        self.q = int(q)
        self.t = int(t)
        self.dt = int(dt)
        self.ok = crc == "OK"
        self.rssi = int(rssi)
        self.b3 = int(b3, 16)
        self.pl = bytes.fromhex(pl)
        self.air = int(air)
        self.odd = odd == "!"
        self.line = line

    @property
    def source(self) -> str:
        """Which board sent it, by payload identity rather than by timing.

        `drv` is the board running spike_b_drive.py and `peer` is everything
        else - in these runs, `sim/` on the nRF54L15 DK. Deliberately not
        "master" and "slave": the driver is the slave in the runs that matter
        and the master in the control run, so naming the *program* rather than
        the *role* is the only labelling that stays true across the set.

        The filler `5A A5` in bytes 2-3 is required as well as the marker,
        because it is the part of spike_b_drive.py's payload that an ANT+ data
        page cannot plausibly reproduce. A marker byte alone would be one byte
        of coincidence away from misattributing a frame, and the whole value of
        marking the payloads was to make attribution an identity.
        """
        marked = self.pl[0] in DRIVER_MARKS or self.pl[0] in CONT_MARKS
        return "drv" if (marked and self.pl[2] == 0x5A and
                         self.pl[3] == 0xA5) else "peer"

    @property
    def what(self) -> str:
        if self.source == "peer":
            return f"page{self.pl[0]:02X}"
        if self.pl[0] in DRIVER_MARKS:
            return f"{DRIVER_MARKS[self.pl[0]]}#{self.pl[1]:02X}"
        # An advanced-burst fragment: byte 1 is its offset within the 24-byte
        # block the host handed over, so +08 and +10 are the second and third
        # on-air packets of one host block.
        return f"{CONT_MARKS[self.pl[0]]}+{self.pl[1]:02X}"


def decode(b3: int) -> str:
    """Byte 3 as the fields Spike B part 2 measured.

    Every bit named here was observed taking both values on this bench. The
    *meanings* are what the results document argues for; this function is the
    argument written down so a capture can be read against it, and a frame that
    does not fit shows up as a value the table has no name for rather than as a
    hex byte nobody looks at.
    """
    fields = []
    fields.append("BURST" if b3 & 0x80 else "bcast")
    fields.append("ACK " if b3 & 0x40 else "data")
    fields.append("LAST" if b3 & 0x20 else "----")
    fields.append(f"tog{1 if b3 & 0x10 else 0}")
    fields.append("SLOT" if b3 & 0x08 else "----")
    tail = b3 & 0x07
    fields.append(f"low={tail:03b}" + ("" if tail == 2 else " <-UNEXPECTED"))
    return " ".join(fields)


def load(path: str):
    frames, summaries, other = [], [], []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n").rstrip("\r")
            m = P_LOOSE.match(line)
            if m:
                frames.append(Frame(m, line))
                continue
            m = S_RE.match(line)
            if m:
                summaries.append(tuple(int(x) for x in m.groups()))
                continue
            if line.strip():
                other.append(line)
    return frames, summaries, other


def completeness(frames, summaries) -> list:
    """Everything that would make the sequence result untrustworthy."""
    problems = []
    if not frames:
        return ["no P lines in this capture at all"]

    first_dr = frames[0].dr
    last_dr = frames[-1].dr
    if last_dr != first_dr:
        problems.append(
            f"the sniffer ring dropped {last_dr - first_dr} frames during this "
            f"capture (dr {first_dr} -> {last_dr}); a burst sequence read from "
            f"it may be missing a packet")
    if first_dr != 0:
        problems.append(f"the ring had already dropped {first_dr} frames before "
                        f"the first line of this capture")

    prev = None
    for f in frames:
        if prev is not None and f.n < prev:
            problems.append(f"n went backwards at n={f.n} (previous {prev}); "
                            f"two captures concatenated?")
            break
        prev = f.n

    if summaries and summaries[-1][9] != 0:
        problems.append(f"the last S line reports dropped={summaries[-1][9]}")

    hi = max(f.q for f in frames)
    if hi > 400:
        problems.append(f"ring occupancy reached {hi} of 512 - close enough to "
                        f"full that the next run should raise RING_LEN")
    return problems


def exchanges(frames):
    out, cur = [], []
    for f in frames:
        if cur and (f.t - cur[-1].t) > EXCHANGE_GAP_US:
            out.append(cur)
            cur = []
        cur.append(f)
    if cur:
        out.append(cur)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero if the capture is not provably complete")
    ap.add_argument("--max-exchanges", type=int, default=40)
    args = ap.parse_args()

    failed = False

    for path in args.logs:
        frames, summaries, _other = load(path)
        print(f"\n=== {path} ===")
        print(f"{len(frames)} decoded frames, {len(summaries)} summary lines")

        problems = completeness(frames, summaries)
        if problems:
            failed = True
            for p in problems:
                print(f"  INCOMPLETE: {p}")
        else:
            print(f"  complete: dr=0 throughout, peak ring occupancy "
                  f"{max(f.q for f in frames)}/512")

        hist = {}
        for f in frames:
            if f.ok:
                hist[(f.b3, f.source)] = hist.get((f.b3, f.source), 0) + 1
        print("\n  byte 3, CRC-valid frames only")
        print("  value  from    count  decode")
        for (b3, src), count in sorted(hist.items()):
            print(f"  0x{b3:02X}   {src:<7} {count:6d}  {decode(b3)}")

        bad = [f for f in frames if not f.ok]
        if bad:
            print(f"\n  {len(bad)} frames read the channel ID correctly and then "
                  f"failed CRC (bit errors, or a payload that is not 8 bytes):")
            for f in bad[:8]:
                print(f"    n={f.n} B3=0x{f.b3:02X} pl={f.pl.hex().upper()}")

        # Exchanges: everything that happened inside one channel slot.
        multi = [e for e in exchanges(frames) if len(e) > 1]
        print(f"\n  {len(multi)} slots carried more than one frame")
        for e in multi[:args.max_exchanges]:
            parts = []
            for i, f in enumerate(e):
                gap = "" if i == 0 else f"+{e[i].t - e[i - 1].t}"
                parts.append(f"{f.what}/{f.b3:02X}{'' if f.ok else '(ER)'}"
                             f"{'' if not gap else ' ' + gap}")
            print("    " + " | ".join(parts))
        if len(multi) > args.max_exchanges:
            print(f"    ... {len(multi) - args.max_exchanges} more")

        # Turnarounds. Both are ADDRESS-to-ADDRESS, so both carry one interrupt
        # entry at each end and the offsets cancel.
        # Three intervals, not two, and separating them is the point: the gap
        # after the master's scheduled broadcast is a different mechanism from
        # the gap after its mid-burst acknowledgement, and averaging them
        # together produces a number that describes neither.
        open2s, ack2s, s2m = [], [], []
        for e in multi:
            for a, b in zip(e, e[1:]):
                if a.source == "peer" and b.source == "drv":
                    (open2s if not (a.b3 & 0x80) else ack2s).append(b.t - a.t)
                elif a.source == "drv" and b.source == "peer":
                    s2m.append(b.t - a.t)

        def report(name, xs):
            if not xs:
                print(f"  {name}: none")
                return
            print(f"  {name}: n={len(xs)} mean={statistics.mean(xs):.0f} us "
                  f"min={min(xs)} max={max(xs)}"
                  + (f" stdev={statistics.stdev(xs):.0f}" if len(xs) > 1 else ""))

        print("\n  turnaround, ADDRESS to ADDRESS")
        report("peer broadcast   -> driver reply     ", open2s)
        report("peer burst ack   -> next driver pkt  ", ack2s)
        report("driver frame     -> peer ack         ", s2m)

    if failed and args.strict:
        print("\nFAIL: at least one capture could not be shown to be complete.")
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
