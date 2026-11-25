#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Does a searching channel acquire on another device's ACK? (opener-gate A/B)

THE RIG. Three boards, all host-controlled from this one process:

    --master-port / --master-serial   a bidirectional MASTER that sends
        acknowledged data in most periods (pattern --pattern, default AAAB:
        three acknowledged periods, then one broadcast), payload byte 0
        0xAC for acknowledged data and 0xBB for broadcasts.
    --slave-port / --slave-serial     a bidirectional SLAVE tracking it. It
        ACKs every acknowledged message on its slot-reply turnaround
        (+2.19 ms). The ACK carries the ACKer's OWN buffer, not an echo: on
        the Garmin USB-m that is a never-written 00 00 51 5b 7d bf fb 16, so
        it shows as kind BC_X. It never writes broadcasts unless
        --slave-replies, so the only frames 2.19 ms into a period are ACKs.
    --dut-port / --dut-serial         the DUT: a RECEIVE-ONLY slave (0x40) for
        the same channel ID, closed and reopened --trials times with a
        RANDOM delay (memory bench-reopen-phase-lock: a fixed delay samples
        one search phase only).

PER TRIAL the DUT opens, waits for the first data message (the acquisition),
then records every message for --watch seconds and closes.

THE SIGNATURE. A DUT that acquired on the master's slot opener tracks the
master: it posts the master's acknowledged data (0x4F) and broadcasts (0x4E,
0xBB) nearly every period. A DUT that acquired on an ACK sits 2.19 ms late:
an image that re-anchors on any frame stays locked to the ACK grid, hears
ACKs (never posted) in acknowledged periods and nothing in broadcast periods
(EVENT_RX_FAIL), so it posts NO master data and an RX_FAIL every fourth
period. A trial is OFF-GRID when its RX_FAILs + GO_TO_SEARCHes in the watch
window outnumber its master-data posts.

Note the acquiring frame is ALWAYS posted as BROADCAST_DATA by radiant, so a
first post carrying 0xAC is expected on an opener acquisition too (the
master's 0xAA frame). "ACK payload posted as broadcast" is a first post that
carries none of the master's payloads (BC_X) on an OFF-GRID trial.

Output: one JSON line per trial (--out), and a summary on stdout.
"""
from __future__ import annotations

import argparse
import json
import random
import sys
import threading
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_probe import EP_OUT, FrameReader, close_device, open_device, reset_stack  # noqa: E402
from ant_wire import frame  # noqa: E402

ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])

M_BCAST, M_ACKD, M_BURST = 0x4E, 0x4F, 0x50
M_EVENT = 0x40
EV_RX_SEARCH_TIMEOUT, EV_RX_FAIL, EV_TX = 0x01, 0x02, 0x03
EV_TX_DONE, EV_TX_FAIL, EV_CLOSED, EV_GTS = 0x05, 0x06, 0x07, 0x08

ACK_BYTE, BC_BYTE, SLAVE_BYTE = 0xAC, 0xBB, 0x5A


def cmd(dev, rd, mid, payload, name, timeout=2.0, ok_codes=(0,)):
    dev.write(EP_OUT, frame(mid, payload))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r = rd.next_frame(min(end, time.monotonic() + 0.25))
        if r is None:
            continue
        rid, body = r
        if rid == M_EVENT and len(body) >= 3 and body[1] == mid:
            if body[2] not in ok_codes:
                raise RuntimeError(f"{name}: code {body[2]}")
            return body[2]
    raise RuntimeError(f"{name}: no answer")


def setup(dev, rd, label, ch_type, a, ext=0x00, power=None):
    if not reset_stack(dev, rd):
        raise RuntimeError(f"{label}: no STARTUP after reset")
    time.sleep(0.2)
    cmd(dev, rd, 0x6E, bytes([0, ext]), f"{label} lib config")
    cmd(dev, rd, 0x46, bytes([0]) + ANT_PLUS_KEY, f"{label} net key")
    cmd(dev, rd, 0x42, bytes([0, ch_type, 0]), f"{label} assign")
    cmd(dev, rd, 0x51, bytes([0, a.device & 0xFF, a.device >> 8, a.dev_type,
                              a.trans_type]), f"{label} channel id")
    cmd(dev, rd, 0x45, bytes([0, a.rf]), f"{label} rf")
    cmd(dev, rd, 0x43, bytes([0, a.period & 0xFF, a.period >> 8]), f"{label} period")
    if power is not None:
        cmd(dev, rd, 0x60, bytes([0, power]), f"{label} tx power")


def open_dev(port, serial, baud):
    if port:
        return open_device(False, port=port, baud=baud)
    return open_device(False, serial=serial)


class Master(threading.Thread):
    def __init__(self, dev, rd, pattern):
        super().__init__(daemon=True)
        self.dev, self.rd, self.pattern = dev, rd, pattern
        self.stop = threading.Event()
        self.n = {"tx": 0, "done": 0, "fail": 0, "acked_q": 0, "bc_q": 0}
        self.i = 0
        self.seq = 0

    def queue_next(self):
        kind = self.pattern[self.i % len(self.pattern)]
        self.i += 1
        self.seq = (self.seq + 1) & 0xFF
        if kind == "A":
            body = bytes([0, ACK_BYTE, self.seq, ACK_BYTE, ACK_BYTE, 1, 2, 3, 4])
            self.dev.write(EP_OUT, frame(M_ACKD, body))
            self.n["acked_q"] += 1
            return True
        body = bytes([0, BC_BYTE, self.seq, BC_BYTE, BC_BYTE, 1, 2, 3, 4])
        self.dev.write(EP_OUT, frame(M_BCAST, body))
        self.n["bc_q"] += 1
        return False

    def run(self):
        while not self.stop.is_set():
            r = self.rd.next_frame(time.monotonic() + 0.2)
            if r is None:
                continue
            mid, body = r
            if mid == M_EVENT and len(body) >= 3 and body[1] in (M_BCAST, M_ACKD):
                k = f"resp_{body[1]:02X}_{body[2]}"
                self.n[k] = self.n.get(k, 0) + 1
                continue
            if mid == M_EVENT and len(body) >= 3 and body[1] == 1:
                code = body[2]
                if code == EV_TX:
                    self.n["tx"] += 1
                elif code == EV_TX_DONE:
                    self.n["done"] += 1
                elif code == EV_TX_FAIL:
                    self.n["fail"] += 1
                else:
                    continue
                try:
                    self.queue_next()
                except Exception:  # noqa: BLE001
                    return


class Slave(threading.Thread):
    def __init__(self, dev, rd, replies):
        super().__init__(daemon=True)
        self.dev, self.rd, self.replies = dev, rd, replies
        self.stop = threading.Event()
        self.n = {"bcast": 0, "acked": 0, "rx_fail": 0, "gts": 0}

    def run(self):
        while not self.stop.is_set():
            r = self.rd.next_frame(time.monotonic() + 0.2)
            if r is None:
                continue
            mid, body = r
            if mid in (M_BCAST, M_ACKD):
                self.n["bcast" if mid == M_BCAST else "acked"] += 1
                if self.replies:
                    self.dev.write(EP_OUT, frame(M_BCAST, bytes(
                        [0, SLAVE_BYTE, SLAVE_BYTE, 0, 0, 0, 0, 0, 0])))
            elif mid == M_EVENT and len(body) >= 3 and body[1] == 1:
                if body[2] == EV_RX_FAIL:
                    self.n["rx_fail"] += 1
                elif body[2] == EV_GTS:
                    self.n["gts"] += 1


def ext_ts(body):
    """Rx timestamp (1/32768 s) from a flagged extended data message, or None."""
    if len(body) < 10:
        return None
    flag = body[9]
    off = 10
    if flag & 0x80:
        off += 4
    if flag & 0x40:
        off += 3
    if flag & 0x20 and len(body) >= off + 2:
        return body[off] | (body[off + 1] << 8)
    return None


def classify(mid, body):
    if mid == M_ACKD:
        return "ACKD"
    if mid == M_BURST:
        return "BURST"
    if mid == M_BCAST and len(body) >= 2:
        return {ACK_BYTE: "BC_A", BC_BYTE: "BC_B", SLAVE_BYTE: "BC_S"}.get(
            body[1], "BC_X")
    return f"0x{mid:02X}"


def trial(dev, rd, a, idx):
    t0 = time.perf_counter()
    dev.write(EP_OUT, frame(0x4B, b"\x00"))
    rec = {"i": idx, "acq_s": None, "first": None, "first_payload": None,
           "events": []}
    acq_deadline = t0 + a.acquire
    watch_end = None
    while True:
        now = time.perf_counter()
        end = watch_end if watch_end is not None else acq_deadline
        if now >= end:
            break
        r = rd.next_frame(time.monotonic() + (end - now))
        if r is None:
            continue
        t = time.perf_counter() - t0
        mid, body = r
        if mid in (M_BCAST, M_ACKD, M_BURST):
            kind = classify(mid, body)
            ts = ext_ts(body)
            if rec["acq_s"] is None:
                rec["acq_s"] = round(t, 4)
                rec["first"] = kind
                rec["first_payload"] = body[1:9].hex()
                rec["first_ts"] = ts
                watch_end = time.perf_counter() + a.watch
            rec["events"].append([round(t, 4), kind, ts, body[1:4].hex()])
        elif mid == M_EVENT and len(body) >= 3 and body[1] == 1:
            code = body[2]
            if code in (EV_RX_FAIL, EV_GTS, EV_RX_SEARCH_TIMEOUT, EV_CLOSED):
                rec["events"].append([round(time.perf_counter() - t0, 4),
                                      {EV_RX_FAIL: "RXF", EV_GTS: "GTS",
                                       EV_RX_SEARCH_TIMEOUT: "STO",
                                       EV_CLOSED: "CLOSED"}[code], None])
                if code in (EV_RX_SEARCH_TIMEOUT, EV_CLOSED) and rec["acq_s"] is None:
                    break
    # Close and wait for CHANNEL_CLOSED.
    dev.write(EP_OUT, frame(0x4C, b"\x00"))
    end = time.monotonic() + 3.0
    closed = False
    while time.monotonic() < end:
        r = rd.next_frame(min(end, time.monotonic() + 0.25))
        if r is None:
            continue
        mid, body = r
        if mid == M_EVENT and len(body) >= 3 and body[1] == 1 and body[2] == EV_CLOSED:
            closed = True
            break
    rec["closed"] = closed
    summarise(rec)
    return rec


def summarise(rec):
    if rec["acq_s"] is None:
        rec["off_grid"] = None
        return
    after = [e for e in rec["events"] if e[0] > rec["acq_s"]]
    master = sum(1 for e in after if e[1] in ("ACKD", "BC_B"))
    fails = sum(1 for e in after if e[1] in ("RXF", "GTS"))
    rec["n_master"] = master
    rec["n_rxf"] = sum(1 for e in after if e[1] == "RXF")
    rec["n_gts"] = sum(1 for e in after if e[1] == "GTS")
    rec["n_bc_a_later"] = sum(1 for e in after if e[1] == "BC_A")
    rec["n_bc_s"] = sum(1 for e in rec["events"] if e[1] == "BC_S")
    rec["off_grid"] = fails > master
    rec["ack_as_bcast"] = bool(rec["off_grid"] and rec["first"] == "BC_X")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--master-port"); ap.add_argument("--master-serial")
    ap.add_argument("--master-baud", type=int, default=115200)
    ap.add_argument("--slave-port"); ap.add_argument("--slave-serial")
    ap.add_argument("--slave-baud", type=int, default=115200)
    ap.add_argument("--dut-port"); ap.add_argument("--dut-serial")
    ap.add_argument("--dut-baud", type=int, default=57600)
    ap.add_argument("--device", type=int, default=24601)
    ap.add_argument("--dev-type", type=int, default=0x19)
    ap.add_argument("--trans-type", type=int, default=1)
    ap.add_argument("--rf", type=int, default=66, help="RF channel (2400+rf MHz)")
    ap.add_argument("--period", type=int, default=1024)
    ap.add_argument("--pattern", default="AAAB")
    ap.add_argument("--slave-replies", action="store_true")
    ap.add_argument("--slave-power", type=int, default=3,
                    help="0x60 level for the slave; Garmin 3 = 0 dBm, 4 = WEAKEST")
    ap.add_argument("--trials", type=int, default=200)
    ap.add_argument("--acquire", type=float, default=5.0)
    ap.add_argument("--watch", type=float, default=2.0)
    ap.add_argument("--delay-max", type=float, default=0.5,
                    help="random extra reopen delay, uniform 0..this (s)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--label", default="")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rnd = random.Random(a.seed)

    mdev = open_dev(a.master_port, a.master_serial, a.master_baud)
    sdev = open_dev(a.slave_port, a.slave_serial, a.slave_baud)
    ddev = open_dev(a.dut_port, a.dut_serial, a.dut_baud)
    mrd, srd, drd = FrameReader(mdev), FrameReader(sdev), FrameReader(ddev)

    setup(mdev, mrd, "master", 0x10, a)
    setup(sdev, srd, "slave", 0x00, a, power=a.slave_power)
    setup(ddev, drd, "dut", 0x40, a, ext=0xE0)
    cmd(ddev, drd, 0x44, bytes([0, 4]), "dut search timeout 10 s")

    master = Master(mdev, mrd, a.pattern)
    cmd(mdev, mrd, 0x4B, b"\x00", "master open")
    master.queue_next()
    master.start()
    cmd(sdev, srd, 0x4B, b"\x00", "slave open")
    slave = Slave(sdev, srd, a.slave_replies)
    slave.start()
    time.sleep(3.0)
    print(f"warm-up: master {master.n} slave {slave.n}", flush=True)
    if slave.n["acked"] == 0:
        print("WARNING: slave received no acknowledged data during warm-up", flush=True)

    recs = []
    t_start = time.time()
    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write(json.dumps({"meta": vars(a), "t_start": t_start}) + "\n")
        for i in range(a.trials):
            time.sleep(rnd.uniform(0.0, a.delay_max))
            rec = trial(ddev, drd, a, i)
            recs.append(rec)
            fh.write(json.dumps(rec) + "\n")
            fh.flush()
            if rec["acq_s"] is None:
                tag = "NO-ACQ"
            else:
                tag = (f"acq {rec['acq_s']*1000:7.1f} ms first {rec['first']:5s} "
                       f"master {rec['n_master']:3d} rxf {rec['n_rxf']:3d} "
                       f"gts {rec['n_gts']} {'OFF-GRID' if rec['off_grid'] else ''}")
            print(f"[{a.label}] {i:4d} {tag}", flush=True)
        tail = {"master": master.n, "slave": slave.n, "t_end": time.time()}
        fh.write(json.dumps({"tail": tail}) + "\n")

    master.stop.set(); slave.stop.set()
    time.sleep(0.3)
    for dev, rd in ((mdev, mrd), (sdev, srd), (ddev, drd)):
        try:
            reset_stack(dev, rd, 1.0)
        except Exception:  # noqa: BLE001
            pass
        close_device(dev)

    acq = [r for r in recs if r["acq_s"] is not None]
    off = [r for r in acq if r["off_grid"]]
    ackb = [r for r in acq if r["ack_as_bcast"]]
    gts = [r for r in acq if r["n_gts"]]
    anyrxf = [r for r in acq if r["n_rxf"]]
    bcs = [r for r in acq if r["n_bc_s"]]
    print(f"\n[{a.label}] trials {len(recs)} acquired {len(acq)} off_grid {len(off)} "
          f"ack_as_bcast {len(ackb)} with_gts {len(gts)} with_any_rxf {len(anyrxf)} "
          f"with_bc_s {len(bcs)}")
    print(f"master {master.n} slave {slave.n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
