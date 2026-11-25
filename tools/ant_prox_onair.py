#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""On-air check of host proximity search (0x71), with TX power standing in
for distance.

THE RIG. A broadcast MASTER whose transmit power steps (--master-*), and the
DUT (--dut-*), a slave searching for the master's exact channel ID. Optional
second master (--far-*) for the near/far pair test. Nothing moves: the
"far" sensor is the same transmitter turned down.

MODES (--mode, may repeat; run in the order given):

  wire       0x54 CAPABILITIES (advanced options 2 bit 0x10), then the 0x71
             responses: [ch, 0x80, 60] (custom -60 dBm), [ch, 11], [ch, 0x80]
             two-byte, [ch, 0] (off). Each RESPONSE code is logged.
  ladder     For each --levels entry: set the master's power, run --cal
             acquisitions with proximity off (RSSI distribution from the
             extended RSSI field), then pick a bin clearly ABOVE the signal
             (expect 0 acquisitions) and one clearly BELOW it (expect normal
             acquisition), and run --trials open/acquire/close trials of each
             with 0x71 sent before EVERY open (it is one-shot).
  behaviour  --rounds of: weak level, 0x71 bin B, open (expect timeout);
             reopen WITHOUT 0x71 (expect timeout: a timeout keeps it); strong
             level, reopen without 0x71 (expect acquire, byte 2 = B's dBm);
             weak level, reopen without 0x71 (expect acquire: the acquisition
             cleared it, byte 2 = -128, the AN16 "off" value).
  pair       Two masters, same RF and period, device numbers --device and
             --far-device; the DUT searches with a WILDCARD device number.
             --trials with prox off, then --trials with bin --pair-bin.

With no --master-port/--master-serial there is no master: point the DUT at
real sensors with --levels=fixed, --device 0 (wildcard) and their own
--dev-type/--rf/--period; --pass-bin N then runs only bin N (the near/far
test when two sensors of one type are on the air).

Every open is preceded by a random delay U(0, --delay-max) so acquisitions
do not phase-lock to the master grid (memory bench-reopen-phase-lock).
Output: one JSON line per trial/check (--out) and a summary on stdout.
"""
from __future__ import annotations

import argparse
import json
import random
import statistics
import sys
import threading
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_probe import EP_OUT, FrameReader, close_device, open_device, reset_stack  # noqa: E402
from ant_wire import frame  # noqa: E402

ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
M_EVENT, M_BCAST, M_ACKD, M_BURST, M_CAPS = 0x40, 0x4E, 0x4F, 0x50, 0x54
EV_STO, EV_RXF, EV_TX, EV_CLOSED, EV_GTS = 0x01, 0x02, 0x03, 0x07, 0x08

# radiant's bin table (radiant_channel.c prox_bin_dbm).
BIN_DBM = {n: -44 - 4 * (n - 1) for n in range(1, 11)}

# nRF54L15 master (radiant): named levels and the -28 dBm raw rung
# (tools/ant_sens.py PART_FINE_DBM). Garmin USB-m: 0..3 = -20/-12/-6/0 dBm
# nominal, 4 = weakest (memory radiant-master-acked-ack-miss).
POWER = {
    "l15": {"+8": (5,), "+4": (4,), "0": (3,), "-4": (2,), "-12": (1,),
            "-20": (0,), "-28": (0x80, 0x01)},
    "garmin": {"0": (3,), "-6": (2,), "-12": (1,), "-20": (0,), "w4": (4,)},
}


def cmd(dev, rd, mid, payload, name, timeout=2.0, ok_codes=(0,), raise_=True):
    dev.write(EP_OUT, frame(mid, payload))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r = rd.next_frame(min(end, time.monotonic() + 0.25))
        if r is None:
            continue
        rid, body = r
        if rid == M_EVENT and len(body) >= 3 and body[1] == mid:
            if body[2] not in ok_codes and raise_:
                raise RuntimeError(f"{name}: code {body[2]}")
            return body[2]
    if raise_:
        raise RuntimeError(f"{name}: no answer")
    return None


def open_dev(port, serial, baud):
    if port:
        return open_device(False, port=port, baud=baud)
    return open_device(False, serial=serial)


def setup(dev, rd, label, ch_type, device, a, ext=0x00):
    if not reset_stack(dev, rd):
        raise RuntimeError(f"{label}: no STARTUP after reset")
    time.sleep(0.2)
    cmd(dev, rd, 0x6E, bytes([0, ext]), f"{label} lib config")
    cmd(dev, rd, 0x46, bytes([0]) + ANT_PLUS_KEY, f"{label} net key")
    cmd(dev, rd, 0x42, bytes([0, ch_type, 0]), f"{label} assign")
    cmd(dev, rd, 0x51, bytes([0, device & 0xFF, device >> 8, a.dev_type,
                              a.trans_type]), f"{label} channel id")
    cmd(dev, rd, 0x45, bytes([0, a.rf]), f"{label} rf")
    cmd(dev, rd, 0x43, bytes([0, a.period & 0xFF, a.period >> 8]), f"{label} period")


class Master(threading.Thread):
    """Broadcast master; drains its port and logs RESPONSEs to 0x60."""

    def __init__(self, dev, rd, tag):
        super().__init__(daemon=True)
        self.dev, self.rd, self.tag = dev, rd, tag
        self.stop = threading.Event()
        self.n = {"tx": 0}
        self.seq = 0
        self.resp = {}
        self.lock = threading.Lock()

    def bcast(self):
        self.seq = (self.seq + 1) & 0xFF
        self.dev.write(EP_OUT, frame(M_BCAST, bytes(
            [0, self.tag, self.seq, self.tag, 0, 1, 2, 3, 4])))

    def set_power(self, spec):
        with self.lock:
            self.resp.pop(0x60, None)
        self.dev.write(EP_OUT, frame(0x60, bytes([0]) + bytes(spec)))
        end = time.monotonic() + 2.0
        while time.monotonic() < end:
            with self.lock:
                if 0x60 in self.resp:
                    return self.resp[0x60]
            time.sleep(0.02)
        return None

    def run(self):
        while not self.stop.is_set():
            r = self.rd.next_frame(time.monotonic() + 0.2)
            if r is None:
                continue
            mid, body = r
            if mid == M_EVENT and len(body) >= 3:
                if body[1] == 1 and body[2] == EV_TX:
                    self.n["tx"] += 1
                    try:
                        self.bcast()
                    except Exception:  # noqa: BLE001
                        return
                elif body[1] != 1:
                    with self.lock:
                        self.resp[body[1]] = body[2]


def ext_fields(body):
    """(device number, rssi dBm, threshold byte as signed) from a flagged message."""
    if len(body) < 10:
        return None, None, None
    flag, off = body[9], 10
    dev = rssi = thr = None
    if flag & 0x80 and len(body) >= off + 4:
        dev = body[off] | (body[off + 1] << 8)
        off += 4
    if flag & 0x40 and len(body) >= off + 3:
        # Only measurement type 0x20 is dBm with a threshold byte (AN16
        # 4.3.1). A Garmin ANTUSB-m sends type 0x10 in a 4-byte field
        # (archive/benchmarks/2026-11-25-garmin-rssi-byte2): not decoded.
        if body[off] == 0x20:
            s8 = lambda v: v - 256 if v > 127 else v  # noqa: E731
            rssi, thr = s8(body[off + 1]), s8(body[off + 2])
        off += 3
    return dev, rssi, thr


def trial(dut, rd, a, prox=None, watch=None):
    """prox: None = send nothing; else bytes after the channel number of 0x71."""
    rec = {"prox": None if prox is None else list(prox)}
    if prox is not None:
        rec["prox_resp"] = cmd(dut, rd, 0x71, bytes([0]) + bytes(prox), "0x71",
                               raise_=False)
    t0 = time.perf_counter()
    dut.write(EP_OUT, frame(0x4B, b"\x00"))
    watch = a.watch if watch is None else watch
    acq_deadline = t0 + a.acquire
    watch_end = None
    later_thr, rssis = set(), []
    sto = closed = False
    while True:
        now = time.perf_counter()
        end = watch_end if watch_end is not None else acq_deadline
        if now >= end:
            break
        r = rd.next_frame(time.monotonic() + (end - now))
        if r is None:
            continue
        mid, body = r
        if mid in (M_BCAST, M_ACKD, M_BURST):
            dev, rssi, thr = ext_fields(body)
            if "acq_s" not in rec:
                rec.update(acq_s=round(time.perf_counter() - t0, 4), dev=dev,
                           rssi0=rssi, thr0=thr)
                watch_end = time.perf_counter() + watch
            else:
                later_thr.add(thr)
            if rssi is not None:
                rssis.append(rssi)
        elif mid == M_EVENT and len(body) >= 3 and body[1] == 1:
            if body[2] == EV_STO:
                sto = True
                rec["sto_s"] = round(time.perf_counter() - t0, 3)
            elif body[2] == EV_CLOSED:
                closed = True
                break
    if not closed:
        dut.write(EP_OUT, frame(0x4C, b"\x00"))
        end = time.monotonic() + 3.0
        while time.monotonic() < end:
            r = rd.next_frame(min(end, time.monotonic() + 0.25))
            if r and r[0] == M_EVENT and len(r[1]) >= 3 and r[1][1] == 1:
                if r[1][2] == EV_CLOSED:
                    closed = True
                    break
                if r[1][2] == EV_STO:
                    sto = True
    rec.update(acquired="acq_s" in rec, sto=sto, closed=closed,
               later_thr=sorted(t for t in later_thr if t is not None),
               n_msgs=len(rssis))
    if rssis:
        rec.update(rssi_mean=round(statistics.mean(rssis), 2),
                   rssi_min=min(rssis), rssi_max=max(rssis))
    return rec


def thr_label(p):
    if p is None:
        return "off"
    if p[0] == 0x80:
        return f"custom{-p[1]}"
    return f"bin{p[0]}({BIN_DBM.get(p[0])})"


# The RSSI field's byte 2 with proximity search off: -128 (0x80), AN16
# section 4.3.3. radiant sent 0 until 2ca4e25; pass --off-byte 0 to judge a
# log or an image from before it.
THR_OFF = -128


def expected_dbm(p):
    if p is None or p[0] == 0:
        return THR_OFF
    if p[0] == 0x80:
        return -p[1]
    return BIN_DBM[p[0]]


def pick_bins(lo, hi, margin):
    """A threshold clearly above hi (reject) and one clearly below lo (pass)."""
    above = [n for n in range(10, 0, -1) if BIN_DBM[n] >= hi + margin]
    rej = (above[0],) if above else (
        (0x80, -(hi + margin)) if 0 < -(hi + margin) <= 127 else None)
    below = [n for n in range(1, 11) if BIN_DBM[n] <= lo - margin]
    pas = (below[0],) if below else (
        (0x80, -(lo - margin)) if 0 < -(lo - margin) <= 127 else None)
    return rej, pas


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--master-port"); ap.add_argument("--master-serial")
    ap.add_argument("--master-baud", type=int, default=115200)
    ap.add_argument("--master-kind", choices=sorted(POWER), default="l15")
    ap.add_argument("--far-port"); ap.add_argument("--far-serial")
    ap.add_argument("--far-baud", type=int, default=115200)
    ap.add_argument("--far-kind", choices=sorted(POWER), default="garmin")
    ap.add_argument("--far-level", default="w4")
    ap.add_argument("--far-device", type=int, default=24602)
    ap.add_argument("--dut-port"); ap.add_argument("--dut-serial")
    ap.add_argument("--dut-baud", type=int, default=57600)
    ap.add_argument("--device", type=int, default=24601)
    ap.add_argument("--dev-type", type=int, default=0x19)
    ap.add_argument("--trans-type", type=int, default=1)
    ap.add_argument("--rf", type=int, default=66)
    ap.add_argument("--period", type=int, default=4096)
    ap.add_argument("--search-timeout", type=int, default=2,
                    help="0x44 value (x2.5 s); 0x63 low-priority set to 0")
    ap.add_argument("--mode", action="append", required=True,
                    choices=["wire", "ladder", "behaviour", "pair"])
    ap.add_argument("--levels", default="+8,0,-12,-20,-28")
    ap.add_argument("--cal", type=int, default=20)
    ap.add_argument("--trials", type=int, default=20)
    ap.add_argument("--margin", type=int, default=6)
    ap.add_argument("--strong", default="+8"); ap.add_argument("--weak", default="-28")
    ap.add_argument("--behaviour-bin", type=int, default=None)
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--pair-bin", type=int, default=None)
    ap.add_argument("--pair-level", default="+8")
    ap.add_argument("--pass-bin", type=int, default=None,
                    help="ladder: run only this bin (no reject arm); with a wildcard "
                    "device number it is the near/far test on real sensors")
    ap.add_argument("--acquire", type=float, default=8.0)
    ap.add_argument("--watch", type=float, default=1.0)
    ap.add_argument("--delay-max", type=float, default=0.5)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--off-byte", type=int, default=-128,
                    help="byte 2 with proximity off: -128 (AN16), or 0 for "
                         "a radiant image from before 2ca4e25")
    a = ap.parse_args()
    global THR_OFF
    THR_OFF = a.off_byte
    rnd = random.Random(a.seed)
    fh = open(a.out, "w", encoding="utf-8")

    def log(obj, line=None):
        fh.write(json.dumps(obj) + "\n")
        fh.flush()
        if line:
            print(line, flush=True)

    log({"meta": vars(a), "t_start": time.time()})

    ddev = open_dev(a.dut_port, a.dut_serial, a.dut_baud)
    drd = FrameReader(ddev)
    master = None
    if a.master_port or a.master_serial:
        mdev = open_dev(a.master_port, a.master_serial, a.master_baud)
        mrd = FrameReader(mdev)
        setup(mdev, mrd, "master", 0x10, a.device, a)
        master = Master(mdev, mrd, 0xA1)
        cmd(mdev, mrd, 0x4B, b"\x00", "master open")
        master.bcast()
        master.start()
    # No master: a real sensor in the room (--levels fixed, --device 0 for
    # a wildcard, and its own --dev-type/--rf/--period).

    far = None
    if "pair" in a.mode:
        fdev = open_dev(a.far_port, a.far_serial, a.far_baud)
        frd = FrameReader(fdev)
        setup(fdev, frd, "far", 0x10, a.far_device, a)
        cmd(fdev, frd, 0x60, bytes([0]) + bytes(POWER[a.far_kind][a.far_level]),
            "far power")
        far = Master(fdev, frd, 0xF2)
        cmd(fdev, frd, 0x4B, b"\x00", "far open")
        far.bcast()
        far.start()

    def dut_setup(device):
        setup(ddev, drd, "dut", 0x00, device, a, ext=0xE0)
        cmd(ddev, drd, 0x63, bytes([0, 0]), "dut lp search timeout 0")
        cmd(ddev, drd, 0x44, bytes([0, a.search_timeout]), "dut search timeout")

    def set_level(lv):
        if master is None:
            log({"set_level": lv, "resp": None}, f"-- no master, level {lv}")
            return
        code = master.set_power(POWER[a.master_kind][lv])
        log({"set_level": lv, "resp": code}, f"-- master level {lv}: resp {code}")
        time.sleep(0.3)

    def run(label, n, prox, level, resend=True, expect=None):
        """expect: byte 2 due on the acquiring frame when it is not
        prox's own (behaviour step 3 sends nothing but runs on the kept
        threshold); default expected_dbm(prox)."""
        want0 = expected_dbm(prox) if expect is None else expect
        recs = []
        for i in range(n):
            time.sleep(rnd.uniform(0.0, a.delay_max))
            rec = trial(ddev, drd, a, prox if (resend or i == 0) else None)
            rec.update(label=label, level=level, i=i, thr=thr_label(prox))
            recs.append(rec)
            tag = (f"ACQ {rec['acq_s']*1000:6.0f} ms dev {rec['dev']} rssi0 "
                   f"{rec['rssi0']} thr0 {rec['thr0']} later {rec['later_thr']}"
                   if rec["acquired"] else
                   f"no-acq sto={rec['sto']} {rec.get('sto_s')}")
            log(rec, f"[{label}] {level:>4} {thr_label(prox):>14} {i:3d} {tag}")
        acq = [r for r in recs if r["acquired"]]
        bad0 = [r for r in acq if r["thr0"] != want0]
        badl = [r for r in acq if any(t != THR_OFF for t in r["later_thr"])]
        s = {"summary": label, "level": level, "thr": thr_label(prox), "n": n,
             "acquired": len(acq), "sto": sum(r["sto"] for r in recs),
             "thr0_mismatch": len(bad0), "later_thr_not_off": len(badl),
             "prox_resp": sorted({r.get("prox_resp") for r in recs} - {None})}
        if acq:
            s["acq_ms_median"] = round(1000 * statistics.median(r["acq_s"] for r in acq))
            rs = [r["rssi0"] for r in acq if r["rssi0"] is not None]
            if rs:
                s.update(rssi0_min=min(rs), rssi0_median=statistics.median(rs),
                         rssi0_max=max(rs))
        log(s, f"== {json.dumps(s)}")
        return recs, s

    dut_setup(a.device)

    for mode in a.mode:
        if mode == "wire":
            ddev.write(EP_OUT, frame(0x4D, bytes([0, M_CAPS])))
            caps = None
            end = time.monotonic() + 2
            while time.monotonic() < end:
                r = drd.next_frame(end)
                if r and r[0] == M_CAPS:
                    caps = r[1]
                    break
            adv2 = caps[4] if caps and len(caps) > 4 else None
            log({"caps": caps.hex() if caps else None, "adv2": adv2,
                 "prox_bit": bool(adv2 is not None and adv2 & 0x10)},
                f"CAPS {caps.hex() if caps else None} adv2=0x{adv2 or 0:02x} "
                f"prox bit {bool(adv2 is not None and adv2 & 0x10)}")
            for name, body in (("custom_60", [0x80, 60]), ("bin_11", [11]),
                               ("custom_2byte", [0x80]), ("bin_10", [10]),
                               ("bin_1", [1]), ("custom_0", [0x80, 0]),
                               ("custom_128", [0x80, 128]),
                               ("bin_3_with_3rd", [3, 99]), ("off", [0])):
                code = cmd(ddev, drd, 0x71, bytes([0] + body), name, raise_=False)
                log({"wire_0x71": name, "body": body, "code": code},
                    f"0x71 {name:15s} {bytes([0] + body).hex()} -> code {code}")
        elif mode == "ladder":
            for lv in a.levels.split(","):
                set_level(lv)
                cal, cs = run("cal", a.cal, None, lv)
                rs = [r["rssi0"] for r in cal if r["acquired"] and r["rssi0"] is not None]
                ms = [r["rssi_min"] for r in cal if "rssi_min" in r]
                mx = [r["rssi_max"] for r in cal if "rssi_max" in r]
                if not rs:
                    log({"skip": lv, "why": "no acquisitions with prox off"},
                        f"level {lv}: nothing acquired with prox off; skipped")
                    continue
                lo, hi = min(rs + ms), max(rs + mx)
                rej, pas = pick_bins(lo, hi, a.margin)
                if a.pass_bin is not None:
                    pas = (a.pass_bin,)
                    rej = None
                log({"bracket": lv, "lo": lo, "hi": hi, "reject": rej, "pass": pas},
                    f"level {lv}: RSSI {lo}..{hi}; reject {thr_label(rej)} "
                    f"pass {thr_label(pas)}")
                if rej:
                    run("reject", a.trials, rej, lv)
                if pas:
                    run("pass", a.trials, pas, lv)
        elif mode == "behaviour":
            b = (a.behaviour_bin,)
            for k in range(a.rounds):
                set_level(a.weak)
                run(f"bh{k}_1_weak_set", 1, b, a.weak)
                run(f"bh{k}_2_weak_noset", 1, None, a.weak)
                set_level(a.strong)
                # Nothing sent, but the timeouts kept bin B: byte 2 is B's.
                run(f"bh{k}_3_strong_noset", 1, None, a.strong,
                    expect=expected_dbm(b))
                set_level(a.weak)
                run(f"bh{k}_4_weak_noset_after_acq", 1, None, a.weak)
        elif mode == "pair":
            set_level(a.pair_level)
            dut_setup(0)   # wildcard device number
            for label, prox in (("pair_off", None), ("pair_bin", (a.pair_bin,)),
                                ("pair_off2", None)):
                recs, _ = run(label, a.trials, prox, a.pair_level)
                devs = {}
                for r in recs:
                    if r["acquired"]:
                        devs[r["dev"]] = devs.get(r["dev"], 0) + 1
                log({"pair_devs": label, "devs": devs}, f"{label}: devices {devs}")
            dut_setup(a.device)

    for m in (master, far):
        if m:
            m.stop.set()
    time.sleep(0.3)
    log({"tail": {"master": master.n if master else None,
                  "far": far.n if far else None, "t_end": time.time()}})
    for dev, rd in tuple((m.dev, m.rd) for m in (master, far) if m) + ((ddev, drd),):
        try:
            reset_stack(dev, rd, 1.0)
        except Exception:  # noqa: BLE001
            pass
        close_device(dev)
    fh.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
