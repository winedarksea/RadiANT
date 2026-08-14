"""Score a P4 soak (phase D) from its console capture.

The soak exists to test ONE invariant:

    exactly one radiant_nrf_gate_on_grant_end() per granted timeslot.

`end=` in the 1 Hz gate line counts those calls and `granted=` counts the
timeslots, so the invariant is `end == granted` at every sample. The three
counters that must stay zero are the exceptional routes the A phase added:
`late=` (a BLOCKED/CANCELLED that arrived while the radio was still ours),
`over=` (overstayed) and `inval=` (an invalid return from a signal callback).

WHY A FOURTH CHECK, AND WHY IT IS THE IMPORTANT ONE. A skipped hand-back is
silent: the 802.15.4 driver's write-once SUBSCRIBE_RXEN is never restored, so
that receiver never ramps again, and nothing faults or resets. The board goes on
answering, and the counters above may all still look perfect - because the fault
is not in the gate's own bookkeeping, it is in what the gate failed to hand
back. The visible signature is `granted` still climbing while the delivered
count sits frozen, so that trend is checked directly rather than inferred from
the totals.

Deliberately tolerant in one place only: the dump races a live grant, so `end`
can legitimately trail `granted` by one at the instant it is printed. A gap of
more than one is real.

Exit status 0 only if every check passes, so the script can gate an overnight
run without a human reading it.
"""
import re
import sys

# The 1 Hz line, parsed by name so a field being added or moved does not
# silently change what is scored.
FIELDS = {
    "granted":  r"granted=(\d+)",
    "in_grant": r"in_grant=(\d+)/",
    # Anchored on the "(norm=" that always follows it. The bare form matched
    # the trailing `end=` FLAG (g.ended, 0 or 1) on a line the capture had
    # mangled - the UART hands over whatever it buffered before the reset, so
    # the first line of any capture may be a fragment of two different ones
    # spliced together. Read as the counter, that flag says a hand-back was
    # skipped 963 times. Any field taken from a partial line is worthless, so
    # a row that does not match all of them is dropped entirely.
    "end":      r"\|\s*end=(\d+)\s*\(norm=",
    "late":     r"late=(\d+)",
    "over":     r"bad over=(\d+)",
    "inval":    r"inval=(\d+)",
}
TIME = re.compile(r"\[(\d+):(\d+):(\d+)\.(\d+)")

# A stall is only a stall if it lasted. Below this the sensor was simply between
# packets, or the sweep was between chunks.
STALL_SECONDS = 300
# ...and only if the gate was actually being granted air throughout, or a quiet
# arm with nothing to do would score as a wedge.
STALL_MIN_GRANTS = 200


def seconds(m):
    return (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
            + float("0." + m.group(4).replace(",", "")))


# One log RECORD, not one physical line. A reset splices the tail of the line
# the UART was midway through onto the head of the first line after it, on one
# physical line and with no newline between them:
#
#   ...gate: acq=963 ... granted=964 ... eagain=[00:12:17.191] ...gate: acq=0 ... end=1 (norm=1 ...
#
# Split on newlines and that reads as one sample with granted=964 and end=1 -
# a 963-deep hand-back failure that never happened, which is precisely the
# headline this script exists to report. Splitting on the timestamp instead
# yields two records, the truncated one is missing fields and is dropped, and
# the surviving one is true.
RECORD = re.compile(r"(?=\[\d+:\d+:\d+\.\d+)")


def parse(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    rows = []
    for rec in RECORD.split(text):
        if "gate: acq=" not in rec:
            continue
        row = {}
        for name, pat in FIELDS.items():
            m = re.search(pat, rec)
            if m is None:
                break
            row[name] = int(m.group(1))
        else:
            t = TIME.search(rec)
            row["t"] = seconds(t) if t else None
            rows.append(row)
    return rows


def main(path):
    rows = parse(path)
    if len(rows) < 2:
        print("FAIL  only %d gate line(s) in %s - nothing to score."
              % (len(rows), path))
        print("      Check DTR on the log VCOM and that the image was built")
        print("      with CONFIG_RADIANT_SWEEP_DEBUG=y.")
        return 1

    # A board that reset mid-soak restarts every counter from zero. That is not
    # a hand-back failure, but scoring across the discontinuity would invent
    # one, so it is reported separately and the counters are scored per segment.
    segments, cur = [], [rows[0]]
    for prev, row in zip(rows, rows[1:]):
        if row["granted"] < prev["granted"]:
            segments.append(cur)
            cur = []
        cur.append(row)
    segments.append(cur)

    failures = []
    if len(segments) > 1:
        failures.append("the counters restarted %d time(s) - the board reset "
                        "mid-soak, which a soak this is supposed to survive"
                        % (len(segments) - 1))

    span = (rows[-1]["t"] - rows[0]["t"]) if rows[0]["t"] is not None else None
    last = rows[-1]
    print("samples   %d over %s"
          % (len(rows), ("%.1f h" % (span / 3600.0)) if span else "unknown"))
    print("final     granted=%d end=%d in_grant=%d late=%d over=%d inval=%d"
          % (last["granted"], last["end"], last["in_grant"],
             last["late"], last["over"], last["inval"]))

    # 1. The invariant. Scored WITHIN segments only: across a reset boundary
    # `granted` restarts at zero while the pre-reset `end` is still the larger
    # number, which manufactures an enormous gap out of an unrelated fault and
    # would send the reader after a hand-back bug that never happened.
    gap = max(r["granted"] - r["end"] for seg in segments for r in seg)
    if gap > 1:
        failures.append("end= fell %d behind granted= (a hand-back was skipped; "
                        "one is the dump racing a live grant, more is real)" % gap)
    print("invariant end==granted: worst gap %d  -> %s"
          % (gap, "ok" if gap <= 1 else "FAIL"))

    # 2. The three must-be-zero counters.
    for name, what in (("late", "BLOCKED/CANCELLED arrived with the radio still ours"),
                       ("over", "overstayed a timeslot"),
                       ("inval", "invalid return from a signal callback")):
        peak = max(r[name] for r in rows)
        if peak:
            failures.append("%s= reached %d (%s)" % (name, peak, what))
        print("%-9s peak %d  -> %s" % (name, peak, "ok" if not peak else "FAIL"))

    # 3a. Delivery that never started at all. Reported separately from a stall
    # because it is a different fault with a different first suspect: a stall
    # means it worked and then stopped (a hand-back skipped partway through),
    # whereas never starting means the receiver was never usable in the first
    # place and the gate is not where to look. The contended arms before the
    # bug 19-23 fixes read exactly this way - 15449 grants, nothing delivered.
    if last["in_grant"] == 0 and last["granted"] >= STALL_MIN_GRANTS:
        failures.append("nothing was ever delivered across %d grants - the "
                        "channel never acquired, which is not a hand-back "
                        "failure and is not what this soak scores"
                        % last["granted"])

    # 3b. The silent one: air still being granted, delivery stopped.
    stall = None
    for seg in segments:
        i = 0
        for j in range(1, len(seg)):
            if seg[j]["in_grant"] != seg[i]["in_grant"]:
                i = j
                continue
            if seg[i]["t"] is None or seg[j]["t"] is None:
                continue
            dt = seg[j]["t"] - seg[i]["t"]
            dg = seg[j]["granted"] - seg[i]["granted"]
            if dt >= STALL_SECONDS and dg >= STALL_MIN_GRANTS:
                if stall is None or dt > stall[0]:
                    stall = (dt, dg, seg[i]["t"])
    if stall:
        failures.append("delivery stalled for %.0f s while %d grants were taken "
                        "(t=%.0f s) - the signature of a SUBSCRIBE_RXEN never "
                        "restored" % (stall[0], stall[1], stall[2]))
    print("delivery stall: %s"
          % ("none" if not stall else "FAIL %.0f s with %d grants"
             % (stall[0], stall[1])))

    print()
    if failures:
        print("SOAK FAILED")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("SOAK PASSED - %d grants, every one handed back exactly once."
          % last["granted"])
    return 0


if __name__ == "__main__":
    # --stall-seconds exists so the detector can be exercised against the short
    # captures this repo already has; an overnight soak should use the default.
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    for opt in sys.argv[1:]:
        if opt.startswith("--stall-seconds="):
            STALL_SECONDS = int(opt.split("=", 1)[1])
    if len(args) != 1:
        print("usage: p4_soak_score.py [--stall-seconds=N] <console.log>")
        raise SystemExit(2)
    raise SystemExit(main(args[0]))
