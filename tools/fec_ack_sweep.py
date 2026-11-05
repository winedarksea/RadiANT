#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Measure a dongle's FE-C acknowledgement window against apps/trainer.

The dongle under test is the ORIGINATOR (a pinned bidirectional slave, as Zwift
runs it); apps/trainer on a DK is the master, with its ACK behaviour set over
its shell. Every command's outcome at the dongle is joined to the trainer's own
ground truth for that command, so a failure is attributed to a side instead of
being one more TRANSFER_TX_FAILED.

    # the ideal baseline: 300 commands, expect ~0 % "our window missed it"
    python tools/fec_ack_sweep.py --sim-port COM4 --serial 3D55F77818BE772A \\
        --preset ideal --count 300

    # where does the window really end? one point per offset
    python tools/fec_ack_sweep.py --sim-port COM4 --serial 3D55F77818BE772A \\
        --offset=-400:600:50 --count 30 --json sweep.json

    # the lone-sim control: no originator at all, must record 0 frames
    python tools/fec_ack_sweep.py --sim-port COM4 --lone-seconds 60

HOW A COMMAND IS IDENTIFIED AT BOTH ENDS. FE-C control pages carry no sequence
number, so this tool makes one: every page 51 it sends carries a grade nobody
else in the run uses (see tag_grade_raw()). The trainer records the payload of
every acknowledged packet it receives, so the grade bytes are a join key that
cannot be confused by a lost packet, a retransmission or a clock offset.

THE VERDICTS (classify()):

    sim side                   our side    verdict
    ACK, on the air            completed   ok
    ACK, on the air            failed      window_missed   <- the one we measure
    ACK planned, not sent      failed      sim_no_tx       (too early to arm)
    MUTE or DEAF               failed      expected
    never received by the sim  failed      not_heard       (lost on the way in)
    anything not sent          completed   IMPOSSIBLE      -> instrument fault

An IMPOSSIBLE row stops the run: a completion with no acknowledgement on the
air means the instrument, not the dongle, is wrong, and nothing after it can
be trusted.

The sim's console needs DTR asserted (the J-Link VCOM goes silent without it),
and the port MOVES between sessions - identify it at run time.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import asdict, dataclass, field

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

# ── Pure helpers: everything below until the hardware section is unit-tested
#    by tools/test_fec_ack_sweep.py without a board. ─────────────────────────

GRADE_ZERO_RAW = 20000          # page 51: Grade% = raw x 0.01 - 200.00
TAG_SPAN = 4000                 # +-20.00 % in 0.01 % steps: 4000 unique tags

OK = "ok"
WINDOW_MISSED = "window_missed"
SIM_NO_TX = "sim_no_tx"
EXPECTED = "expected"
NOT_HEARD = "not_heard"
IMPOSSIBLE = "IMPOSSIBLE"
ORIGINATOR_ERROR = "originator_error"

VERDICTS = (OK, WINDOW_MISSED, SIM_NO_TX, EXPECTED, NOT_HEARD, IMPOSSIBLE,
            ORIGINATOR_ERROR)

COMPLETED = "TRANSFER_TX_COMPLETED"
FAILED = "TRANSFER_TX_FAILED"


def tag_grade_raw(index: int) -> int:
    """The page 51 grade field for command `index` of a run.

    Unique for TAG_SPAN consecutive commands and always inside +-20 %, a grade
    any trainer accepts. Centred on flat so a long run wanders around 0 %
    instead of climbing.
    """
    return GRADE_ZERO_RAW - TAG_SPAN // 2 + (index % TAG_SPAN)


def page51_tagged(index: int) -> bytes:
    raw = tag_grade_raw(index)
    return bytes([0x33, 0xFF, 0xFF, 0xFF, 0xFF, raw & 0xFF, raw >> 8, 0xFF])


def tag_of_payload(payload: bytes) -> int | None:
    """The join key a sim record carries: page 51's grade bytes."""
    if len(payload) != 8 or payload[0] != 0x33:
        return None
    return payload[5] | (payload[6] << 8)


@dataclass
class SimRecord:
    seq: int
    t_us: int
    payload: bytes
    action: str          # ack | mute | deaf
    cause: str
    offset_us: int
    tx: int              # -1 pending, 0 not on air, 1 on air


_AF_LINE = re.compile(
    r"AF seq=(\d+) t=(\d+) p=([0-9a-fA-F]{16}) act=(\w+) cause=(\w+) "
    r"off=(-?\d+) tx=(-?\d+)")
_AF_BEGIN = re.compile(r"AF-BEGIN n=(\d+) lost=(\d+) acks_sent=(\d+) "
                       r"too_early=(\d+)")


@dataclass
class SimDump:
    records: list[SimRecord] = field(default_factory=list)
    n: int = 0
    lost: int = 0
    acks_sent: int = 0
    too_early: int = 0
    complete: bool = False


def parse_af_dump(text: str) -> SimDump:
    """Parse `ackfault dump` output. Tolerates echo, prompts and log lines
    interleaved with it; refuses nothing, but says (complete=False) when the
    AF-END marker or any promised record is missing."""
    dump = SimDump()
    began = False
    for line in text.splitlines():
        m = _AF_BEGIN.search(line)
        if m:
            began = True
            dump.n, dump.lost, dump.acks_sent, dump.too_early = (
                int(g) for g in m.groups())
            continue
        m = _AF_LINE.search(line)
        if m and began:
            seq, t, p, act, cause, off, tx = m.groups()
            dump.records.append(SimRecord(int(seq), int(t), bytes.fromhex(p),
                                          act, cause, int(off), int(tx)))
            continue
        if began and "AF-END" in line:
            dump.complete = len(dump.records) == dump.n
    return dump


@dataclass
class Sent:
    index: int
    tag: int
    outcome: str          # COMPLETED | FAILED | anything else = error text
    t_host: float = 0.0


def classify(outcome: str, sim: list[SimRecord]) -> str:
    """One command's verdict from our outcome and the sim's record(s) of it.

    More than one sim record for one tag would be the originator transmitting
    the same command twice; the best evidence wins (an ACK that went out).
    """
    if outcome not in (COMPLETED, FAILED):
        return ORIGINATOR_ERROR
    on_air = any(r.action == "ack" and r.tx == 1 for r in sim)
    if outcome == COMPLETED:
        return OK if on_air else IMPOSSIBLE
    if not sim:
        return NOT_HEARD
    if on_air:
        return WINDOW_MISSED
    if any(r.action == "ack" for r in sim):
        return SIM_NO_TX
    return EXPECTED


def join(sent: list[Sent], dump: SimDump) -> list[dict]:
    by_tag: dict[int, list[SimRecord]] = {}
    for r in dump.records:
        tag = tag_of_payload(r.payload)
        if tag is not None:
            by_tag.setdefault(tag, []).append(r)
    rows = []
    for s in sent:
        recs = by_tag.get(s.tag, [])
        rows.append({
            "index": s.index,
            "tag": s.tag,
            "outcome": s.outcome,
            "sim": [{"act": r.action, "cause": r.cause, "off": r.offset_us,
                     "tx": r.tx, "seq": r.seq} for r in recs],
            "verdict": classify(s.outcome, recs),
        })
    return rows


def summarize(rows: list[dict]) -> dict:
    counts = {v: 0 for v in VERDICTS}
    for r in rows:
        counts[r["verdict"]] += 1
    acked = counts[OK] + counts[WINDOW_MISSED]
    counts["n"] = len(rows)
    # The headline: of the commands the sim really acknowledged, how many did
    # our window miss. Everything else is either injected or upstream.
    counts["window_miss_rate"] = (counts[WINDOW_MISSED] / acked) if acked else None
    return counts


def page71_advance(seqs: list[int]) -> int:
    """How far page 71's sequence moved across successive observations.

    The trainer's sequence is ITS count of commands applied (FE-C control
    pages carry none of their own), so this counts ACK + MUTE and never DEAF:
    the one thing a controller can see that tells "applied, ACK lost" from
    "never heard". A u8 that wraps, hence the per-step modulo.
    """
    return sum((b - a) % 256 for a, b in zip(seqs, seqs[1:]))


def applied_by_sim(dump: SimDump) -> int:
    """Commands the trainer's application received: everything not DEAF."""
    return sum(1 for r in dump.records if r.action in ("ack", "mute"))


def parse_sweep(spec: str) -> list[int]:
    """'-400:600:50' -> [-400, -350, ..., 600]. Inclusive of the end."""
    parts = spec.split(":")
    if len(parts) != 3:
        raise ValueError("sweep is START:STOP:STEP, e.g. -400:600:50")
    start, stop, step = (int(p) for p in parts)
    if step <= 0 or stop < start:
        raise ValueError("STEP must be positive and STOP >= START")
    return list(range(start, stop + 1, step))


def curve_lines(points: list[dict]) -> list[str]:
    out = ["  offset_us    n   ok  missed  expected  no_tx  not_heard  miss%"]
    for p in points:
        s = p["summary"]
        rate = s["window_miss_rate"]
        out.append(
            f"  {p['offset_us']:+9d} {s['n']:4d} {s[OK]:4d} {s[WINDOW_MISSED]:7d}"
            f" {s[EXPECTED]:9d} {s[SIM_NO_TX]:6d} {s[NOT_HEARD]:10d}"
            f"  {'-' if rate is None else f'{100 * rate:5.1f}'}")
    return out


# ── Hardware ───────────────────────────────────────────────────────────────

_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


class SimShell:
    """apps/trainer's shell over its console VCOM."""

    def __init__(self, port: str, baud: int = 115200):
        import serial  # pyserial; only the hardware path needs it

        self.ser = serial.Serial(port, baud, timeout=0.1)
        # Without DTR the J-Link VCOM hands over one chunk and goes silent,
        # which reads exactly like a hung board.
        self.ser.dtr = True
        self.ser.rts = True
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def run(self, cmd: str, until: str | None = None,
            timeout: float = 5.0) -> str:
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r\n").encode())
        buf = ""
        deadline = time.monotonic() + timeout
        quiet_since = time.monotonic()
        while time.monotonic() < deadline:
            chunk = self.ser.read(4096).decode(errors="replace")
            if chunk:
                buf += _ANSI.sub("", chunk)
                quiet_since = time.monotonic()
                if until is not None and until in buf:
                    break
            elif until is None and buf and time.monotonic() - quiet_since > 0.3:
                break
        if "bad parameter" in buf or "unknown preset" in buf:
            raise RuntimeError(f"sim refused '{cmd}': {buf.strip()}")
        return buf

    def dump(self) -> SimDump:
        return parse_af_dump(self.run("ackfault dump", until="AF-END",
                                      timeout=30.0))

    def close(self) -> None:
        self.ser.close()


def open_originator(device: int, serial: str | None, port: str | None):
    """A pinned bidirectional FE-C slave on the dongle under test, tracking."""
    from ant_fec_control import (ANT_PLUS_FREQ, ANT_PLUS_KEY, CHANNEL,
                                 FEC_DEVICE_TYPE, FEC_PERIOD, FEC_TRANS_TYPE,
                                 command)
    from ant_probe import FrameReader, open_device, reset_stack
    from ant_wire import (LIB_CONFIG_ALL_EXT_FIELDS, MESG_ANTLIB_CONFIG_ID,
                          MESG_ASSIGN_CHANNEL_ID, MESG_BROADCAST_DATA_ID,
                          MESG_CHANNEL_ID_ID, MESG_CHANNEL_MESG_PERIOD_ID,
                          MESG_CHANNEL_RADIO_FREQ_ID,
                          MESG_CHANNEL_SEARCH_TIMEOUT_ID, MESG_NETWORK_KEY_ID,
                          MESG_OPEN_CHANNEL_ID)

    dev = open_device(True, serial=serial, port=port)
    reader = FrameReader(dev)
    if not reset_stack(dev, reader):
        raise RuntimeError("originator: no startup message after reset")
    command(dev, reader, MESG_ANTLIB_CONFIG_ID,
            bytes([0x00, LIB_CONFIG_ALL_EXT_FIELDS]), "extended messages")
    steps = [
        (MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "network key"),
        # 0x00, bidirectional: a receive-only slave cannot transmit at all.
        (MESG_ASSIGN_CHANNEL_ID, bytes([CHANNEL, 0x00, 0x00]), "assign"),
        (MESG_CHANNEL_ID_ID, bytes([CHANNEL, device & 0xFF, device >> 8,
                                    FEC_DEVICE_TYPE, FEC_TRANS_TYPE]),
         f"channel id #{device}"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CHANNEL, ANT_PLUS_FREQ]), "rf"),
        (MESG_CHANNEL_MESG_PERIOD_ID,
         bytes([CHANNEL, FEC_PERIOD & 0xFF, FEC_PERIOD >> 8]), "period"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CHANNEL, 0xFF]), "search"),
        (MESG_OPEN_CHANNEL_ID, bytes([CHANNEL]), "open"),
    ]
    for msg_id, payload, name in steps:
        if not command(dev, reader, msg_id, payload, name):
            raise RuntimeError(f"originator: {name} refused")

    heard = 0
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline and heard < 8:
        r = reader.next_frame(min(deadline, time.monotonic() + 1.0))
        if r and r[0] == MESG_BROADCAST_DATA_ID:
            heard += 1
    if heard < 8:
        raise RuntimeError(f"originator: only {heard} broadcasts from "
                           f"#{device} in 15 s - not tracking")
    return dev, reader


MESG_BROADCAST = 0x4E
PAGE_CMD_STATUS = 0x47


def watch(frame_, seqs: list[int]) -> None:
    """Note page 71's sequence from any broadcast that carries it, so a run
    can say how often the trainer's own command count moved."""
    if frame_ and frame_[0] == MESG_BROADCAST and len(frame_[1]) >= 9 \
            and frame_[1][1] == PAGE_CMD_STATUS:
        seq = frame_[1][3]
        if not seqs or seqs[-1] != seq:
            seqs.append(seq)


def drain(reader, seconds: float, seqs: list[int]) -> None:
    """Wait by READING, not sleeping: frames that pile up unread are frames
    whose page 71 nobody saw."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        watch(reader.next_frame(min(end, time.monotonic() + 0.25)), seqs)


def send_one(dev, reader, payload: bytes, seqs: list[int]) -> str:
    """One acknowledged page 51, right after a broadcast (as a head unit
    paces), and its terminal event. Same parsing as ant_fec_control.py,
    including the body[1] == MESG_EVENT_ID rule that has faked two firmware
    faults when written as 0."""
    from ant_fec_control import CHANNEL
    from ant_probe import EP_OUT, frame
    from ant_wire import (EVENT_TRANSFER_TX_COMPLETED,
                          EVENT_TRANSFER_TX_FAILED, MESG_ACKNOWLEDGED_DATA_ID,
                          MESG_BROADCAST_DATA_ID, MESG_EVENT_ID,
                          MESG_RESPONSE_EVENT_ID)

    end = time.monotonic() + 0.6
    while time.monotonic() < end:
        r = reader.next_frame(min(end, time.monotonic() + 0.3))
        watch(r, seqs)
        if r and r[0] == MESG_BROADCAST_DATA_ID:
            break

    dev.write(EP_OUT, frame(MESG_ACKNOWLEDGED_DATA_ID,
                            bytes([CHANNEL]) + payload))
    end = time.monotonic() + 2.0
    while time.monotonic() < end:
        r = reader.next_frame(min(end, time.monotonic() + 0.25))
        if r is None:
            continue
        watch(r, seqs)
        msg_id, body = r
        if msg_id != MESG_RESPONSE_EVENT_ID or len(body) < 3:
            continue
        if body[1] == MESG_ACKNOWLEDGED_DATA_ID and body[2] != 0:
            return f"REFUSED code {body[2]}"
        if body[1] != MESG_EVENT_ID:
            continue
        if body[2] == EVENT_TRANSFER_TX_COMPLETED:
            return COMPLETED
        if body[2] == EVENT_TRANSFER_TX_FAILED:
            return FAILED
    return "no terminal event"


def run_point(sim: SimShell, dev, reader, setup: list[str], count: int,
              first_index: int, interval_s: float) -> dict:
    for cmd in setup:
        sim.run(cmd)
    sim.run("ackfault reset")
    sent = []
    seqs: list[int] = []
    for i in range(count):
        idx = first_index + i
        t0 = time.monotonic()
        outcome = send_one(dev, reader, page51_tagged(idx), seqs)
        sent.append(Sent(idx, tag_grade_raw(idx), outcome, t0))
        rest = interval_s - (time.monotonic() - t0)
        if rest > 0:
            drain(reader, rest, seqs)
    # Page 71 rides slot 3 only, up to ~2.25 s after a command; this also
    # lets the last ACK's completion reach the sim's acks_sent.
    drain(reader, 3.0, seqs)
    dump = sim.dump()
    rows = join(sent, dump)
    return {"setup": setup, "dump_complete": dump.complete,
            "dump_lost": dump.lost, "sim_too_early": dump.too_early,
            # The FIRST page 71 seen already answers some command, so a run
            # where page 71 tracks every applied command reads
            # advanced ~= applied - 1. DEAF commands must not count.
            "page71_seen": len(seqs),
            "page71_advanced": page71_advance(seqs),
            "sim_applied": applied_by_sim(dump),
            "rows": rows, "summary": summarize(rows)}


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sim-port", required=True,
                    help="apps/trainer's console VCOM (find it at run time)")
    ap.add_argument("--device", type=int, default=4244,
                    help="the sim's ANT device number (default 4244)")
    ap.add_argument("--serial", help="dongle under test, by USB serial suffix")
    ap.add_argument("--port", help="dongle under test on a UART instead")
    ap.add_argument("--preset", default="ideal",
                    help="sim preset for a single run, or the base of a sweep")
    ap.add_argument("--set", action="append", default=[], metavar="K=V",
                    help="extra `ackfault set` parameters, repeatable")
    ap.add_argument("--offset", metavar="START:STOP:STEP",
                    help="sweep offset_us over this range, one point each. "
                         "Write it --offset=-400:600:50: with a space, "
                         "argparse reads a negative START as an option")
    ap.add_argument("--count", type=int, default=30,
                    help="commands per point (default 30)")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="seconds between commands (default 1.0, Zwift-like)")
    ap.add_argument("--lone-seconds", type=float,
                    help="the lone-sim control: no originator, just wait and "
                         "require zero acked frames")
    ap.add_argument("--json", help="write every row and summary here")
    args = ap.parse_args()

    sim = SimShell(args.sim_port)
    try:
        if args.lone_seconds is not None:
            sim.run("ackfault preset ideal")
            sim.run("ackfault reset")
            print(f"lone-sim control: {args.lone_seconds:.0f} s, no originator")
            time.sleep(args.lone_seconds)
            out = sim.run("ackfault stats")
            m = re.search(r"acked_frames=(\d+)", out)
            n = int(m.group(1)) if m else -1
            print(out.strip())
            print("PASS" if n == 0 else f"FAIL: {n} acked frames with nobody "
                  "sending - the hook or the radio is inventing traffic")
            return 0 if n == 0 else 1

        if args.serial is None and args.port is None:
            ap.error("--serial or --port is required except with --lone-seconds")
        dev, reader = open_originator(args.device, args.serial, args.port)

        base = [f"ackfault preset {args.preset}"]
        if args.set:
            base.append("ackfault set " + " ".join(args.set))
        offsets = parse_sweep(args.offset) if args.offset else [None]

        points = []
        index = 0
        for off in offsets:
            setup = list(base)
            if off is not None:
                setup.append(f"ackfault set offset_us={off}")
            print(f"\npoint {setup}")
            p = run_point(sim, dev, reader, setup, args.count, index,
                          args.interval)
            index += args.count
            p["offset_us"] = 0 if off is None else off
            points.append(p)
            s = p["summary"]
            print("  " + "  ".join(f"{k}={s[k]}" for k in VERDICTS if s[k]))
            print(f"  page 71: advanced {p['page71_advanced']} over "
                  f"{p['page71_seen']} sightings; the sim applied "
                  f"{p['sim_applied']} of {s['n']} (ACK+MUTE)")
            if not p["dump_complete"]:
                print("  WARNING: the sim's dump was incomplete; rows without "
                      "a sim record may be misattributed")
            if s[IMPOSSIBLE]:
                print("\nSTOP: a command completed that the sim never put an "
                      "ACK on the air for. That is an instrument fault - the "
                      "join, the tags or the hook - and every number after it "
                      "would be meaningless.")
                break

        print("\n" + "\n".join(curve_lines(points)))
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump({"args": vars(args), "points": points}, f, indent=1,
                          default=lambda o: asdict(o) if hasattr(o, "__dataclass_fields__") else str(o))
            print(f"\nwrote {args.json}")
        return 1 if any(p["summary"][IMPOSSIBLE] for p in points) else 0
    finally:
        sim.close()


if __name__ == "__main__":
    sys.exit(main())
