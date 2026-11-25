#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Open a slave onto a master that opens an exchange EVERY period.

Retests "acquiring on an exchange opener": when a slave channel's search
acquires on a master's acknowledged-data (0xAA) or burst (0x8A) opener, the
host should be handed ACKNOWLEDGED_DATA / BURST_DATA (and the transfer ACKed
when a bidirectional channel can still reach the reply slot), not
BROADCAST_DATA.

THE RIG. Two USB sticks held by this one process:

    --master-serial   the master (a reference stick, e.g. the Garmin "006").
                      It keeps an acknowledged message (or a 2-packet burst,
                      --bursts) queued for EVERY channel period, re-queued on
                      each terminal event, so no plain broadcast is ever on
                      the air once it starts.
    --slave-serial    the stick under test, opened as a BIDIRECTIONAL slave
                      pinned to the master's channel id, then closed and
                      reopened --opens times with a random delay
                      (--delay-min..--delay-max, memory bench-reopen-phase-lock).

Per open the tool records the slave's FIRST data message for the channel
(0x4E broadcast / 0x4F acknowledged / 0x50 burst), its payload index (each
master transfer carries a running index in data byte 0), and the master's
terminal event for that same index (TRANSFER_TX_COMPLETED = the slave ACKed
it, TRANSFER_TX_FAILED = no ACK). It also counts master EVENT_TX (a plain
broadcast slot went out, i.e. the master was not holding a transfer) so a run
where the opener was not really on air every period shows up.

Every host-side time uses time.perf_counter() (memory libusb0-short-read-race).

    python tools/bench_acquire_opener.py --master-serial 006 \\
        --slave-serial EE6986854FD72803 --opens 20 --json out.json
"""
from __future__ import annotations

import argparse
import json
import random
import sys
import threading
import time
from datetime import UTC, datetime

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_probe import (  # noqa: E402
    EP_OUT,
    FrameReader,
    close_device,
    frame,
    open_device,
    reset_stack,
)
from ant_wire import (  # noqa: E402
    EVENT_TRANSFER_TX_COMPLETED,
    EVENT_TRANSFER_TX_FAILED,
    EVENT_TX,
    MESG_ACKNOWLEDGED_DATA_ID,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_BROADCAST_DATA_ID,
    MESG_BURST_DATA_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_EVENT_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_RESPONSE_EVENT_ID,
)

ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
ANT_PLUS_FREQ = 57
CHANNEL = 0
DEVICE_TYPE = 17
TRANS_TYPE = 5
PERIOD = 8192
EVENT_CHANNEL_CLOSED = 0x07
EVENT_RX_SEARCH_TIMEOUT = 0x01
DATA_IDS = {MESG_BROADCAST_DATA_ID: "BROADCAST", MESG_ACKNOWLEDGED_DATA_ID: "ACKNOWLEDGED",
            MESG_BURST_DATA_ID: "BURST"}


def now() -> float:
    return time.perf_counter()


def command(dev, reader, msg_id, payload, name, timeout=2.0) -> bool:
    dev.write(EP_OUT, frame(msg_id, payload))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r = reader.next_frame(min(end, time.monotonic() + 0.25))
        if r is None:
            continue
        rid, body = r
        if rid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
            if body[2] != 0:
                print(f"  FAIL: {name} -> code {body[2]}")
                return False
            return True
    print(f"  FAIL: {name} not answered")
    return False


def configure(dev, reader, ch_type, device, label, power=None) -> bool:
    if not reset_stack(dev, reader):
        print(f"  {label}: no startup message after reset")
        return False
    steps = [
        (MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "network key"),
        (MESG_ASSIGN_CHANNEL_ID, bytes([CHANNEL, ch_type, 0]), "assign"),
        (MESG_CHANNEL_ID_ID, bytes([CHANNEL, device & 0xFF, device >> 8,
                                    DEVICE_TYPE, TRANS_TYPE]), "channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CHANNEL, ANT_PLUS_FREQ]), "freq"),
        (MESG_CHANNEL_MESG_PERIOD_ID, bytes([CHANNEL, PERIOD & 0xFF, PERIOD >> 8]),
         "period"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CHANNEL, 0xFF]), "search timeout"),
    ]
    if power is not None:
        steps.append((0x60, bytes([CHANNEL, power]), f"channel tx power {power}"))
    for mid, pl, name in steps:
        if not command(dev, reader, mid, pl, f"{label} {name}"):
            return False
    print(f"  {label}: configured")
    return True


class Master:
    """Keep one exchange queued for every period; log each terminal event."""

    def __init__(self, dev, reader, bursts: bool):
        self.dev, self.reader, self.bursts = dev, reader, bursts
        self.idx = 0
        self.pending = False
        self.events: dict[int, tuple[str, float]] = {}   # idx -> (outcome, t)
        self.slots_bcast = 0
        self.other: dict[int, int] = {}
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.started = False
        self.stale = 0

    def queue(self) -> None:
        i = self.idx & 0xFF
        d = bytes([i, 0xAC, 0xA5, 0x5A, 0x01, 0x02, 0x03, 0x04])
        if self.bursts:
            self.dev.write(EP_OUT, frame(MESG_BURST_DATA_ID, bytes([CHANNEL]) + d))
            d2 = bytes([i, 0xB2, 0xA5, 0x5A, 0x01, 0x02, 0x03, 0x04])
            self.dev.write(EP_OUT, frame(MESG_BURST_DATA_ID,
                                         bytes([CHANNEL | (1 << 5) | 0x80]) + d2))
        else:
            self.dev.write(EP_OUT, frame(MESG_ACKNOWLEDGED_DATA_ID, bytes([CHANNEL]) + d))
        self.pending = True
        self.stale = 0

    def run(self) -> None:
        self.queue()
        while not self.stop.is_set():
            r = self.reader.next_frame(time.monotonic() + 0.25)
            if r is None:
                continue
            mid, body = r
            if mid != MESG_RESPONSE_EVENT_ID or len(body) < 3:
                continue
            code = body[2]
            if body[1] == MESG_EVENT_ID:
                if code in (EVENT_TRANSFER_TX_COMPLETED, EVENT_TRANSFER_TX_FAILED):
                    with self.lock:
                        self.events[self.idx & 0xFF] = (
                            "COMPLETED" if code == EVENT_TRANSFER_TX_COMPLETED
                            else "FAILED", now())
                        self.started = True
                    self.idx += 1
                    self.pending = False
                    try:
                        self.queue()
                    except Exception:  # noqa: BLE001
                        return
                elif code == EVENT_TX:
                    self.slots_bcast += 1
                    self.stale += 1
                    # Not pending, or the queued transfer was refused / never
                    # started within two slots: queue again.
                    if not self.pending or self.stale > 2:
                        self.queue()
                else:
                    self.other[code] = self.other.get(code, 0) + 1
            elif code != 0:
                # A refused data write (e.g. TRANSFER_IN_PROGRESS): not queued.
                key = (body[1] << 8) | code
                self.other[key] = self.other.get(key, 0) + 1


def slave_open_once(dev, reader, open_timeout: float, follow_s: float) -> dict:
    """Open, return the first data message and what followed, close."""
    rec: dict = {"first": None}
    t_open = now()
    if not command(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([CHANNEL]), "slave open"):
        rec["error"] = "open refused"
        return rec
    rec["t_open"] = t_open
    seq: list = []
    end = now() + open_timeout
    first_t = None
    while now() < end:
        r = reader.next_frame(time.monotonic() + 0.25)
        if r is None:
            continue
        mid, body = r
        if mid in DATA_IDS and len(body) >= 9 and (body[0] & 0x1F) == CHANNEL:
            entry = {"kind": DATA_IDS[mid], "idx": body[1], "b2": body[2],
                     "t": now() - t_open, "t_abs": now()}
            if mid == MESG_BURST_DATA_ID:
                entry["seq"] = (body[0] >> 5) & 3
                entry["last"] = bool(body[0] & 0x80)
            seq.append(entry)
            if first_t is None:
                first_t = now()
                end = first_t + follow_s
        elif mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == MESG_EVENT_ID:
            seq.append({"kind": f"EVENT_0x{body[2]:02X}", "t": now() - t_open})
            if body[2] == EVENT_RX_SEARCH_TIMEOUT:
                break
    data = [e for e in seq if "idx" in e]
    rec["first"] = data[0] if data else None
    rec["sequence"] = seq[:12]
    dev.write(EP_OUT, frame(MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL])))
    cend = now() + 3.0
    while now() < cend:
        r = reader.next_frame(time.monotonic() + 0.25)
        if r is None:
            continue
        mid, body = r
        if (mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == MESG_EVENT_ID
                and body[2] == EVENT_CHANNEL_CLOSED):
            rec["closed"] = True
            break
    return rec


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--master-serial", required=True)
    ap.add_argument("--slave-serial", required=True)
    ap.add_argument("--device", type=int, default=24681)
    ap.add_argument("--opens", type=int, default=20)
    ap.add_argument("--bursts", action="store_true",
                    help="master holds a 2-packet burst each period instead of acked data")
    ap.add_argument("--master-power", type=int, default=3,
                    help="0x60 level on the master (Garmin: 3 = 0 dBm; 4 is its WEAKEST)")
    ap.add_argument("--delay-min", type=float, default=0.3)
    ap.add_argument("--delay-max", type=float, default=1.5)
    ap.add_argument("--open-timeout", type=float, default=15.0)
    ap.add_argument("--follow", type=float, default=1.2,
                    help="seconds to keep reading after the first data message")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--label", default="")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    mdev = open_device(False, serial=args.master_serial)
    mrd = FrameReader(mdev)
    sdev = open_device(False, serial=args.slave_serial)
    srd = FrameReader(sdev)
    if not configure(sdev, srd, 0x00, args.device, "slave"):
        return 1
    if not configure(mdev, mrd, 0x10, args.device, "master", args.master_power):
        return 1
    master = Master(mdev, mrd, args.bursts)
    if not command(mdev, mrd, MESG_OPEN_CHANNEL_ID, bytes([CHANNEL]), "master open"):
        return 1
    th = threading.Thread(target=master.run, daemon=True)
    th.start()
    time.sleep(2.0)
    print(f"master holding {'bursts' if args.bursts else 'acked data'}; "
          f"broadcast slots so far {master.slots_bcast}")

    rows = []
    for n in range(args.opens):
        bc_before = master.slots_bcast
        rec = slave_open_once(sdev, srd, args.open_timeout, args.follow)
        time.sleep(0.5)  # let the master's terminal event for that index arrive
        f = rec.get("first")
        out = "no data"
        if f is not None:
            with master.lock:
                ev = master.events.get(f["idx"])
            rec["master_outcome"] = ev[0] if ev else "unknown"
            if ev:
                rec["master_minus_slave_ms"] = round((ev[1] - f["t_abs"]) * 1000, 1)
            out = f"{f['kind']} idx {f['idx']} t+{f['t']:.3f}s master {rec['master_outcome']}"
        rec["master_bcast_slots_during"] = master.slots_bcast - bc_before
        rec["n"] = n + 1
        rows.append(rec)
        print(f"  open {n + 1:2d}/{args.opens}: {out}  "
              f"(master bcast slots {rec['master_bcast_slots_during']})")
        time.sleep(rng.uniform(args.delay_min, args.delay_max))

    master.stop.set()
    th.join(timeout=2)
    tally: dict[str, int] = {}
    for r in rows:
        f = r.get("first")
        key = "no data" if f is None else f"{f['kind']}/{r.get('master_outcome')}"
        tally[key] = tally.get(key, 0) + 1
    print("\nfirst message / master outcome for that transfer:")
    for k, v in sorted(tally.items()):
        print(f"  {k:28s} {v}/{len(rows)}")
    with master.lock:
        mo = [v[0] for v in master.events.values()]
    print(f"master transfers: {len(master.events)} "
          f"(completed {mo.count('COMPLETED')}, failed {mo.count('FAILED')}), "
          f"broadcast slots {master.slots_bcast}, other {master.other}")
    for d in (sdev, mdev):
        try:
            d.write(EP_OUT, frame(MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL])))
            time.sleep(0.2)
            close_device(d)
        except Exception:  # noqa: BLE001
            pass
    if args.json:
        for r in rows:
            for e in ([r["first"]] if r.get("first") else []) + r.get("sequence", []):
                e.pop("t_abs", None)
            r.pop("t_open", None)
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"tool": "bench_acquire_opener", "label": args.label,
                       "when": datetime.now(UTC).isoformat(), "args": vars(args),
                       "tally": tally, "master_bcast_slots": master.slots_bcast,
                       "master_other": {str(k): v for k, v in master.other.items()},
                       "rows": rows}, fh, indent=2)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
