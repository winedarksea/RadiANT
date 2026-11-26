#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture the promiscuous sniffer (radiant/spike/promisc) and turn its P
lines into an .antcap v2 file: both ends of a link, with direction.

    # live: read the L15 log VCOM (DTR asserted) for 10 minutes
    python tools/promisc_log.py --port COM3 --seconds 600 \
        --raw sniff.log --out sniff.antcap

    # offline: convert a raw log captured earlier
    python tools/promisc_log.py --convert sniff.log --out sniff.antcap

Build the sniffer for the device(s) under test without a source edit
(radiant/spike/promisc/CMakeLists.txt):

    west build ... -- -DSPIKE_DEVNUM=<n> -DSPIKE_DTYPE=0x28 -DSPIKE_TTYPE=5 \
                      -DSPIKE_DEVNUM2=<m> -DSPIKE_DTYPE2=0x23

DIRECTION AND KIND come from the on-air control byte B3 (docs/spike-b-
results.md): bit 3 set = the frame opens the slot, which only the master
does; bit 6 = an acknowledgement frame (written as a '# ACK' comment, not a
data line - its payload is not data); bit 7 clear = broadcast, bit 7 set =
acknowledged/burst, where a first packet with bit 5 ("last") set is a whole
acknowledged message ('A') and anything else is a burst packet ('S'). An
in-slot data frame is the slave's (lower case) unless it continues a burst
the master was sending in that slot.

Time: the sniffer's t= microseconds (32-bit, unwrapped here), so the t
column is radio time, not host time; dt between a master frame and the
slave's reply is the turnaround.
"""
from __future__ import annotations

import argparse
import re
import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402

P_LINE = re.compile(
    r"^P n=(?P<n>\d+) dr=(?P<dr>\d+) q=\d+ t=(?P<t>\d+) dt=\s*[+-]?\d+ "
    r"(?P<crc>OK|ER) m=\d+ rssi=(?P<rssi>-?\d+) dn=(?P<dn>\d+) "
    r"ty=(?P<ty>[0-9A-Fa-f]{2})!? tt=(?P<tt>\d+) B3=(?P<b3>[0-9A-Fa-f]{2}) "
    r"pl=(?P<pl>[0-9A-Fa-f]{16})")

CTRL_SLOT_OPENER = 0x08
CTRL_ACK = 0x40
CTRL_EXCHANGE = 0x80
CTRL_LAST = 0x20
# A burst continuation follows its predecessor by one packet time plus
# turnaround; anything slower is a new slot.
CONTINUATION_US = 20000


def classify(b3: int, prev: dict | None, t_us: int) -> tuple[str | None, bool]:
    """(kind letter or None for an ACK frame, from_master) for one frame.

    `prev` is the previous DATA frame of the same device: {"t", "master",
    "open_burst"}; it decides whether an in-slot frame continues a master
    burst."""
    if b3 & CTRL_ACK:
        return None, False
    if b3 & CTRL_SLOT_OPENER:
        master = True
    else:
        master = bool(prev and prev["master"] and prev["open_burst"]
                      and t_us - prev["t"] < CONTINUATION_US)
    if not b3 & CTRL_EXCHANGE:
        letter = "B"
    else:
        first = not (prev and prev["open_burst"] and prev["master"] == master
                     and t_us - prev["t"] < CONTINUATION_US)
        letter = "A" if (first and b3 & CTRL_LAST) else "S"
    return (letter if master else letter.lower()), master


def convert(lines, comments=()) -> tuple[list, list[str], dict]:
    """P lines -> (v2 records, comment lines, stats). CRC-failed frames are
    dropped (counted); ring drops (dr) are reported, since a capture with
    dr > 0 is incomplete."""
    records, notes = [], []
    stats = {"frames": 0, "crc_err": 0, "acks": 0, "max_dr": 0,
             "unparsed_p": 0}
    t_base = None
    last_raw = None
    wraps = 0
    prev: dict[int, dict] = {}
    last_host = last_t_s = None     # host<->radio anchor for MARK lines
    for raw in lines:
        line = raw.rstrip("\r\n")
        if line.startswith("#"):
            mark = re.match(r"^# MARK host (\d+\.\d+) (.*)$", line)
            if mark and last_host is not None:
                t_est = last_t_s + float(mark[1]) - last_host
                notes.append(f"MARK {t_est:.6f} {mark[2]}")
            else:
                notes.append(line.lstrip("# "))
            continue
        host = re.match(r"^(\d+\.\d+) ", line)
        # A host prefix "<host_s> " (written by the live path) is optional.
        body = line.split(" ", 1)[1] if re.match(r"^\d+\.\d+ P ", line) \
            else line
        if not body.startswith("P "):
            if body.startswith(("S ", "[cfg]", "Listening", "Second device",
                                "Board:")):
                notes.append(f"sniffer: {body}")
            continue
        m = P_LINE.match(body)
        if m is None:
            stats["unparsed_p"] += 1
            continue
        stats["max_dr"] = max(stats["max_dr"], int(m["dr"]))
        if m["crc"] != "OK":
            stats["crc_err"] += 1
            continue
        t_raw = int(m["t"])
        if last_raw is not None and t_raw < last_raw - (1 << 31):
            wraps += 1
        last_raw = t_raw
        t_us = t_raw + (wraps << 32)
        if t_base is None:
            t_base = t_us
        dn, ty, b3 = int(m["dn"]), int(m["ty"], 16) & 0x7F, int(m["b3"], 16)
        payload = bytes.fromhex(m["pl"])
        kind, master = classify(b3, prev.get(dn), t_us)
        t_s = (t_us - t_base) / 1e6
        if host:
            last_host, last_t_s = float(host[1]), t_s
        if kind is None:
            stats["acks"] += 1
            notes.append(f"ACK {t_s:.6f} dev {dn} type 0x{ty:02X} B3=0x{b3:02X}")
            continue
        stats["frames"] += 1
        records.append((t_s, ty, dn, payload, kind, int(m["rssi"])))
        prev[dn] = {"t": t_us, "master": master,
                    "open_burst": bool(b3 & CTRL_EXCHANGE)
                    and not b3 & CTRL_LAST}
    return records, list(comments) + notes, stats


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="sniffer log VCOM (DTR is asserted)")
    src.add_argument("--convert", metavar="RAW.log",
                     help="convert an existing raw log instead")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--raw", metavar="FILE.log",
                        help="live: write every console line here, prefixed "
                             "with host seconds")
    parser.add_argument("--out", metavar="FILE.antcap")
    parser.add_argument("--mark", action="store_true",
                        help="live: each stdin line becomes a '# MARK' line")
    args = parser.parse_args()

    if args.convert:
        with open(args.convert, errors="replace") as handle:
            lines = handle.readlines()
        header = [f"promisc_log.py --convert {args.convert}"]
    else:
        import threading

        import serial
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
        ser.dtr = True
        lines = []
        raw = open(args.raw, "w") if args.raw else None  # noqa: SIM115 - closed below
        t0 = time.monotonic()
        lock = threading.Lock()

        def put(text):
            stamped = f"{time.monotonic() - t0:.3f} {text}"
            with lock:
                lines.append(stamped if not text.startswith("#") else text)
                if raw:
                    raw.write((stamped if not text.startswith("#") else text)
                              + "\n")
                    raw.flush()

        if args.mark:
            def marks():
                for line in sys.stdin:
                    put(f"# MARK host {time.monotonic() - t0:.3f} "
                        f"{line.strip() or '-'}")
            threading.Thread(target=marks, daemon=True).start()
        buf = b""
        end = t0 + args.seconds
        n_p = 0
        next_report = t0 + 10
        try:
            while time.monotonic() < end:
                buf += ser.read(4096)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("ascii", "replace").rstrip("\r")
                    if text:
                        put(text)
                        n_p += text.startswith("P ")
                if time.monotonic() >= next_report:
                    print(f"[{time.monotonic() - t0:6.0f}s] {n_p} P lines")
                    next_report += 10
        except KeyboardInterrupt:
            pass
        ser.close()
        if raw:
            raw.close()
        header = [f"promisc_log.py --port {args.port} --seconds "
                  f"{args.seconds:g}",
                  f"started {time.strftime('%Y-%m-%d %H:%M:%S')}"]

    records, notes, stats = convert(lines, header)
    print(f"frames {stats['frames']}  acks {stats['acks']}  crc_err "
          f"{stats['crc_err']}  max dr {stats['max_dr']}  unparsed "
          f"{stats['unparsed_p']}")
    if stats["max_dr"]:
        print("WARNING: the sniffer's ring dropped frames (dr > 0): the "
              "capture is incomplete")
    if args.out:
        ap.write_capture(args.out, records, comments=notes + [
            f"stats {stats}"])
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
