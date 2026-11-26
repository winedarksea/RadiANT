#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Record what an ANT+ device sends, with message kind, RSSI and timing, and
optionally talk back to it. A generic bench recorder and a crude display.

    # Every master on 2457 MHz, in Rx scan mode (one channel hears them all):
    python tools/ant_record.py --scan --out cap.antcap --seconds 120

    # One pinned slave channel per device (dev type : dev number : trans type
    # [: period]); 0 is the wildcard:
    python tools/ant_record.py --chan 0x28:0:0 --chan 0x23:0:0 --out c.antcap

    # Ask the radar for page 81 at t=5 s, then send a raw acked page at 20 s:
    python tools/ant_record.py --chan 0x28:0:0 --send p70:81@5 \
        --send 02fdffffffffffff@20 --out c4.antcap

Output is .antcap v2 (tools/ant_pages.py): one line per message with a kind
letter whose case is the direction (B/A/S from the master; b/a from us) and
the RSSI in dBm when the receiver reports dBm. Everything else - channel
events, TX results, the channel IDs seen, the lines typed with --mark - goes
in as '#' comment lines at their timestamp, so the capture is the whole log.

--send SPEC[@T][#CH], repeatable. SPEC is 16 hex digits (an 8-byte page), or
p70:PAGE[:COUNT][:ack] (Common Page 70; COUNT defaults to 4, ':ack' sets the
"reply acknowledged" bit). Prefix 'b:' to send it as a broadcast instead of
an acknowledged message. T is seconds after the channels open (default 0,
or right after the previous --send); CH picks the --chan index (default 0).
Only pinned channels can send: in --scan mode the target is ambiguous.

--mark reads stdin: every line typed is written as '# MARK <t> <text>' -
walk-test annotations ("start L", "pass", "stop"). An empty line writes
'# MARK <t> -'.

Time: the t column is host monotonic seconds since open, refined by the
receiver's own 32768 Hz RX timestamp (lib config 0x20) when it is present,
so a period measured from successive t's is not USB polling jitter.
"""
from __future__ import annotations

import argparse
import statistics
import sys
import threading
import time
from collections import Counter, defaultdict
from dataclasses import dataclass

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402
from ant_wire import (  # noqa: E402
    CHANNEL_TYPE_SLAVE,
    EVENT_CHANNEL_CLOSED,
    EVENT_RX_FAIL,
    EVENT_RX_FAIL_GO_TO_SEARCH,
    EVENT_RX_SEARCH_TIMEOUT,
    EVENT_TRANSFER_TX_COMPLETED,
    EVENT_TRANSFER_TX_FAILED,
    EXT_FLAG_CHANNEL_ID,
    EXT_FLAG_RSSI,
    EXT_FLAG_RX_TIMESTAMP,
    LIB_CONFIG_ALL_EXT_FIELDS,
    MESG_ACKNOWLEDGED_DATA_ID,
    MESG_ANTLIB_CONFIG_ID,
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
    MESG_OPEN_RX_SCAN_MODE_ID,
    MESG_RESPONSE_EVENT_ID,
    RSSI_MEASUREMENT_TYPE_DBM,
)

ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
DATA_KINDS = {MESG_BROADCAST_DATA_ID: "B", MESG_ACKNOWLEDGED_DATA_ID: "A",
              MESG_BURST_DATA_ID: "S"}
# Garmin's ANTUSB-m reports RSSI as measurement type 0x10 in FOUR bytes
# (memory garmin-rssi-field-format); AN16's dBm form is 0x20 in three.
RSSI_FIELD_LEN = {RSSI_MEASUREMENT_TYPE_DBM: 3, 0x10: 4}
EVENT_NAMES = {
    EVENT_RX_SEARCH_TIMEOUT: "RX_SEARCH_TIMEOUT", EVENT_RX_FAIL: "RX_FAIL",
    EVENT_TRANSFER_TX_COMPLETED: "TRANSFER_TX_COMPLETED",
    EVENT_TRANSFER_TX_FAILED: "TRANSFER_TX_FAILED",
    EVENT_CHANNEL_CLOSED: "CHANNEL_CLOSED",
    EVENT_RX_FAIL_GO_TO_SEARCH: "RX_FAIL_GO_TO_SEARCH",
    0x04: "TRANSFER_RX_FAILED", 0x09: "CHANNEL_COLLISION",
    0x0A: "TRANSFER_TX_START",
}
TICKS_PER_S = 32768
PAGE_REQUEST = 0x46


# ---------------------------------------------------------------------------
# Pure parsing (unit-tested in test_ant_record.py)
# ---------------------------------------------------------------------------

@dataclass
class Rx:
    channel: int
    kind: str
    payload: bytes
    device_number: int | None = None
    device_type: int | None = None
    trans_type: int | None = None
    rssi: int | None = None
    rx_ticks: int | None = None


def parse_rx(msg_id: int, body: bytes) -> Rx | None:
    """A broadcast/acked/burst message (with any extended fields) as an Rx.

    Burst messages carry a sequence number in the top three bits of the
    channel byte; the channel is the low five."""
    kind = DATA_KINDS.get(msg_id)
    if kind is None or len(body) < 9:
        return None
    channel = body[0] & 0x1F if msg_id == MESG_BURST_DATA_ID else body[0]
    rx = Rx(channel, kind, bytes(body[1:9]))
    if len(body) < 10:
        return rx
    flags, at = body[9], 10
    if flags & EXT_FLAG_CHANNEL_ID and len(body) >= at + 4:
        rx.device_number = body[at] | (body[at + 1] << 8)
        rx.device_type = body[at + 2]
        rx.trans_type = body[at + 3]
        at += 4
    if flags & EXT_FLAG_RSSI and len(body) > at:
        mtype = body[at]
        if mtype == RSSI_MEASUREMENT_TYPE_DBM and len(body) >= at + 2:
            v = body[at + 1]
            rx.rssi = v - 256 if v > 127 else v
        at += RSSI_FIELD_LEN.get(mtype, 3)
    if flags & EXT_FLAG_RX_TIMESTAMP and len(body) >= at + 2:
        rx.rx_ticks = body[at] | (body[at + 1] << 8)
    return rx


def page70(requested: int, count: int = 4, acked: bool = False) -> bytes:
    """Common Page 70, Request Data Page (same bytes as ant_fec_control's):
    byte 5 bits 0-6 = times to send, bit 7 = reply acknowledged; byte 7 =
    command type 1 (request data page). A count above 0x7F is taken as the
    raw byte 5 (0x80 = "until acknowledged", 0x00 = invalid)."""
    byte5 = count if count > 0x7F else (count & 0x7F) | (0x80 if acked else 0)
    return bytes([PAGE_REQUEST, 0xFF, 0xFF, 0xFF, 0xFF, byte5 & 0xFF,
                  requested & 0xFF, 0x01])


@dataclass
class Send:
    at: float | None        # seconds after open; None = after the previous
    payload: bytes
    acked: bool = True
    chan_index: int = 0
    spec: str = ""


def parse_send(spec: str) -> Send:
    """--send SPEC[@T][#CH]; see the module docstring."""
    text = spec.strip()
    chan_index = 0
    if "#" in text:
        text, ch = text.rsplit("#", 1)
        chan_index = int(ch, 0)
    at = None
    if "@" in text:
        text, t = text.rsplit("@", 1)
        at = float(t)
    acked = True
    if text.startswith("b:"):
        acked, text = False, text[2:]
    if text.lower().startswith("p70:"):
        parts = text[4:].split(":")
        page = int(parts[0], 0)
        count, ack_reply = 4, False
        for extra in parts[1:]:
            if extra.lower() == "ack":
                ack_reply = True
            else:
                count = int(extra, 0)
        payload = page70(page, count, ack_reply)
    else:
        payload = bytes.fromhex(text)
        if len(payload) != 8:
            raise ValueError(f"--send {spec!r}: need 8 bytes, got "
                             f"{len(payload)}")
    return Send(at, payload, acked, chan_index, spec)


def parse_chan(spec: str) -> tuple[int, int, int, int | None]:
    """--chan DT:DN:TT[:PERIOD] (numbers in any base int() takes)."""
    parts = [int(p, 0) for p in spec.split(":")]
    if len(parts) not in (3, 4):
        raise ValueError(f"--chan {spec!r}: want DT:DN:TT[:PERIOD]")
    return (parts[0], parts[1], parts[2], parts[3] if len(parts) == 4 else None)


def default_period(device_type: int) -> int:
    """The profile period for a device type PROFILES knows, else 8192."""
    for prof in ap.PROFILES.values():
        if prof.get("device_type") == device_type:
            return prof["period"]
    return 8192


class TickClock:
    """Host time refined by the receiver's 16-bit 32768 Hz RX timestamp.

    The timestamp wraps every 2 s, so each step is unwrapped by choosing the
    wrap count that best matches the host-clock step. The host clock carries
    the long-run anchor; the ticks carry the sub-millisecond spacing."""

    def __init__(self):
        self.host0 = None
        self.last_host = None
        self.last_raw = None
        self.ticks = 0      # unwrapped ticks since the first timestamp

    def t(self, host: float, raw: int | None) -> float:
        if raw is None or self.last_raw is None:
            if raw is not None:
                self.host0, self.last_host, self.last_raw = host, host, raw
                self.ticks = 0
            return host
        dh = host - self.last_host
        step = (raw - self.last_raw) & 0xFFFF
        wraps = max(0, round((dh * TICKS_PER_S - step) / 65536))
        self.ticks += step + 65536 * wraps
        self.last_host, self.last_raw = host, raw
        refined = self.host0 + self.ticks / TICKS_PER_S
        # Never let a bad timestamp drag t more than 50 ms from the host.
        if abs(refined - host) > 0.05:
            self.host0, self.ticks = host, 0
            return host
        return refined


# ---------------------------------------------------------------------------
# Live recording
# ---------------------------------------------------------------------------

class Recorder:
    def __init__(self, args):
        self.args = args
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.t0 = None
        self.clock = TickClock()
        self.seen_ids: set[tuple] = set()
        self.per_dev = defaultdict(lambda: {"pages": Counter(), "rssi": [],
                                            "kinds": Counter(), "last": {}})
        self.events = Counter()
        self.chan_ids: dict[int, tuple[int, int, int]] = {}
        # Held open for the whole recording; close() closes it.
        self.out = open(args.out, "w") if args.out else None  # noqa: SIM115
        if self.out:
            self.out.write(ap.CAPTURE_HEADER_V2 + "\n")
            self.out.write(f"# ant_record.py {' '.join(sys.argv[1:])}\n")
            self.out.write(f"# started {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            self.out.flush()

    def now(self) -> float:
        return time.monotonic() - self.t0

    def comment(self, text: str) -> None:
        with self.lock:
            if self.out:
                self.out.write(f"# {text}\n")
                self.out.flush()
        if not self.args.quiet:
            print(f"  {text}")

    def record(self, t: float, dtype: int, dnum: int, payload: bytes,
               kind: str, rssi: int | None) -> None:
        line = ap.format_capture_line(t, dtype, dnum, payload, kind, rssi)
        with self.lock:
            if self.out:
                self.out.write(line + "\n")
        if self.args.verbose:
            try:
                dec = ap.decode(payload, dtype & 0x7F)
            except Exception as exc:  # noqa: BLE001
                dec = {"error": str(exc)}
            print(f"  {line}  {dec}")

    def on_rx(self, rx: Rx, host: float) -> None:
        t = self.clock.t(host, rx.rx_ticks)
        dnum, dtype, ttype = rx.device_number, rx.device_type, rx.trans_type
        if dnum is None and rx.channel in self.chan_ids:
            dtype, dnum, ttype = self.chan_ids[rx.channel]
        dnum = dnum or 0
        dtype = dtype or 0
        key = (rx.channel, dnum, dtype, ttype)
        if dnum and key not in self.seen_ids:
            self.seen_ids.add(key)
            self.comment(f"ID {t:.6f} ch {rx.channel} dev {dnum} type "
                         f"0x{dtype:02X} trans 0x{(ttype or 0):02X}"
                         f"{' (pairing bit)' if dtype & 0x80 else ''}")
        self.record(t, dtype & 0x7F, dnum, rx.payload, rx.kind, rx.rssi)
        st = self.per_dev[(dtype & 0x7F, dnum)]
        st["pages"][rx.payload[0]] += 1
        st["kinds"][rx.kind] += 1
        st["last"][rx.payload[0]] = rx.payload
        if rx.rssi is not None:
            st["rssi"].append(rx.rssi)
            if len(st["rssi"]) > 64:
                del st["rssi"][:-64]

    def status(self) -> None:
        # Data lines are buffered for speed; flush here so a capture can be
        # read while it runs (an unflushed file looks like a dead recorder).
        with self.lock:
            if self.out:
                self.out.flush()
        if self.args.quiet or not self.args.status:
            return
        parts = []
        for (dtype, dnum), st in sorted(self.per_dev.items()):
            pages = " ".join(f"{p}:{n}" for p, n in sorted(st["pages"].items()))
            rssi = (f" rssi {statistics.median(st['rssi']):.0f}"
                    if st["rssi"] else "")
            parts.append(f"0x{dtype:02X}#{dnum} [{pages}]{rssi}")
        ev = " ".join(f"{EVENT_NAMES.get(c, hex(c))}:{n}"
                      for c, n in sorted(self.events.items()))
        print(f"[{self.now():7.1f}s] " + "; ".join(parts) + (f" | {ev}" if ev
                                                             else ""))
        if self.args.show_decoded:
            for (dtype, dnum), st in sorted(self.per_dev.items()):
                for page, payload in sorted(st["last"].items()):
                    try:
                        dec = ap.decode(payload, dtype)
                    except Exception as exc:  # noqa: BLE001
                        dec = {"error": str(exc)}
                    dec.pop("raw", None)
                    print(f"      0x{dtype:02X}#{dnum} p{page}: {dec}")

    def close(self, summary: str = "") -> None:
        if self.out:
            if summary:
                self.out.write(f"# {summary}\n")
            self.out.close()


def command(dev, reader, msg_id, payload, name, timeout=2.0) -> bool:
    from ant_probe import EP_OUT
    from ant_wire import frame
    dev.write(EP_OUT, frame(msg_id, payload))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r = reader.next_frame(end)
        if r is None:
            break
        mid, body = r
        if mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
            if body[2] != 0:
                print(f"  FAIL: {name} code {body[2]}")
                return False
            return True
    print(f"  FAIL: {name}: no answer")
    return False


def mark_reader(rec: Recorder, stop: threading.Event) -> None:
    for line in sys.stdin:
        if stop.is_set():
            return
        text = line.strip() or "-"
        rec.comment(f"MARK {rec.now():.6f} {text}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--scan", action="store_true",
                      help="Rx scan mode (0x5B): every master on --rf")
    mode.add_argument("--chan", action="append", metavar="DT:DN:TT[:PERIOD]",
                      help="a pinned slave channel; repeat for more")
    parser.add_argument("--scan-id", metavar="DT:DN:TT", default="0:0:0",
                        help="channel ID filter for --scan (default wildcard)")
    parser.add_argument("--rf", type=int, default=57)
    parser.add_argument("--send", action="append", default=[],
                        metavar="SPEC[@T][#CH]")
    parser.add_argument("--mark", action="store_true",
                        help="write each line typed on stdin as a MARK")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--out", metavar="FILE.antcap")
    parser.add_argument("--status", type=float, default=2.0,
                        help="status line every N s (0 = off)")
    parser.add_argument("--show-decoded", action="store_true",
                        help="print the last decoded page of each kind with "
                             "every status line")
    parser.add_argument("--log-rx-fail", action="store_true",
                        help="write every RX_FAIL as a comment (counted "
                             "either way)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="print every message")
    parser.add_argument("-q", "--quiet", action="store_true")
    parser.add_argument("--serial")
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    sends = [parse_send(s) for s in args.send]
    if sends and args.scan:
        parser.error("--send needs a pinned --chan (scan mode has no target)")
    chans = [parse_chan(c) for c in (args.chan or [])]
    for s in sends:
        if s.chan_index >= len(chans):
            parser.error(f"--send {s.spec}: no --chan index {s.chan_index}")

    from ant_probe import EP_OUT, FrameReader, close_device, open_device, reset_stack
    from ant_wire import frame

    dev = open_device(not args.quiet, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)
    if not reset_stack(dev, reader):
        print("FAIL: no startup message after reset")
        return 1
    time.sleep(0.2)
    rec = Recorder(args)
    ok = command(dev, reader, MESG_ANTLIB_CONFIG_ID,
                 bytes([0, LIB_CONFIG_ALL_EXT_FIELDS]), "lib config 0xE0")
    ok = ok and command(dev, reader, MESG_NETWORK_KEY_ID,
                        bytes([0]) + ANT_PLUS_KEY, "network key")
    if not ok:
        return 1

    if args.scan:
        dt, dn, tt, _ = parse_chan(args.scan_id)
        steps = [(MESG_ASSIGN_CHANNEL_ID, bytes([0, CHANNEL_TYPE_SLAVE, 0]),
                  "assign"),
                 (MESG_CHANNEL_ID_ID, bytes([0, dn & 0xFF, dn >> 8, dt, tt]),
                  "channel id"),
                 (MESG_CHANNEL_RADIO_FREQ_ID, bytes([0, args.rf]), "rf"),
                 (MESG_OPEN_RX_SCAN_MODE_ID, bytes([0]), "open rx scan mode")]
        for mid, pl, name in steps:
            if not command(dev, reader, mid, pl, name):
                return 1
        rec.comment(f"OPEN scan id {dt:#x}:{dn}:{tt:#x} rf {args.rf}")
    else:
        for ch, (dt, dn, tt, period) in enumerate(chans):
            period = period or default_period(dt)
            rec.chan_ids[ch] = (dt, dn, tt)
            steps = [(MESG_ASSIGN_CHANNEL_ID,
                      bytes([ch, CHANNEL_TYPE_SLAVE, 0]), "assign"),
                     (MESG_CHANNEL_ID_ID,
                      bytes([ch, dn & 0xFF, dn >> 8, dt, tt]), "channel id"),
                     (MESG_CHANNEL_RADIO_FREQ_ID, bytes([ch, args.rf]), "rf"),
                     (MESG_CHANNEL_MESG_PERIOD_ID,
                      bytes([ch, period & 0xFF, period >> 8]), "period"),
                     (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([ch, 0xFF]),
                      "search timeout infinite"),
                     (MESG_OPEN_CHANNEL_ID, bytes([ch]), "open")]
            for mid, pl, name in steps:
                if not command(dev, reader, mid, pl, f"ch{ch} {name}"):
                    return 1
            rec.comment(f"OPEN ch {ch} id {dt:#x}:{dn}:{tt:#x} period "
                        f"{period} rf {args.rf}")

    rec.t0 = time.monotonic()
    stop = threading.Event()
    if args.mark:
        threading.Thread(target=mark_reader, args=(rec, stop),
                         daemon=True).start()
        print("--mark: type a label and Enter to annotate")

    # Schedule the sends: absolute times, a missing @T chains 0.5 s after the
    # previous one.
    queue, last = [], 0.0
    for s in sends:
        at = s.at if s.at is not None else last + 0.5
        queue.append((at, s))
        last = at
    queue.sort(key=lambda q: q[0])
    pending: dict[int, Send] = {}   # channel -> send awaiting its result

    end = rec.t0 + args.seconds
    # --status 0 prints nothing but still flushes every 2 s.
    tick = args.status or 2.0
    next_status = rec.t0 + tick
    try:
        while time.monotonic() < end:
            now = time.monotonic()
            while queue and now - rec.t0 >= queue[0][0] and \
                    queue[0][1].chan_index not in pending:
                _, s = queue.pop(0)
                ch = s.chan_index
                dt, dn, _ = rec.chan_ids[ch]
                mid = MESG_ACKNOWLEDGED_DATA_ID if s.acked else \
                    MESG_BROADCAST_DATA_ID
                dev.write(EP_OUT, frame(mid, bytes([ch]) + s.payload))
                t = rec.now()
                rec.record(t, dt, dn, s.payload, "a" if s.acked else "b", None)
                rec.comment(f"TX {t:.6f} ch {ch} "
                            f"{'acked' if s.acked else 'broadcast'} "
                            f"{s.payload.hex()} ({s.spec})")
                if s.acked:
                    pending[ch] = s
            r = reader.next_frame(min(end, next_status, now + 0.1))
            host = time.monotonic() - rec.t0
            if r is not None:
                mid, body = r
                rx = parse_rx(mid, body)
                if rx is not None:
                    rec.on_rx(rx, host)
                elif (mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3
                      and body[1] == MESG_EVENT_ID):
                    ch, code = body[0], body[2]
                    rec.events[code] += 1
                    if code != EVENT_RX_FAIL or args.log_rx_fail:
                        rec.comment(f"EVENT {host:.6f} ch {ch} "
                                    f"{EVENT_NAMES.get(code, hex(code))}")
                    if code in (EVENT_TRANSFER_TX_COMPLETED,
                                EVENT_TRANSFER_TX_FAILED):
                        pending.pop(ch, None)
                    if code == EVENT_CHANNEL_CLOSED and not args.scan:
                        command(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([ch]),
                                f"ch{ch} reopen")
                        rec.comment(f"REOPEN {rec.now():.6f} ch {ch}")
                elif mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
                    rec.comment(f"RESPONSE {host:.6f} ch {body[0]} to "
                                f"0x{body[1]:02X} code {body[2]}")
            if time.monotonic() >= next_status:
                rec.status()
                next_status += tick
    except KeyboardInterrupt:
        rec.comment(f"INTERRUPTED {rec.now():.6f}")
    finally:
        stop.set()
        n = sum(sum(st["pages"].values()) for st in rec.per_dev.values())
        summary = (f"END {rec.now():.6f} messages {n} events "
                   + " ".join(f"{EVENT_NAMES.get(c, hex(c))}:{k}"
                              for c, k in sorted(rec.events.items())))
        rec.status()
        rec.close(summary)
        try:
            for ch in (range(len(chans)) if chans else [0]):
                command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([ch]),
                        f"close ch{ch}", timeout=1.0)
        except Exception:  # noqa: BLE001
            pass
        close_device(dev)
    print(summary)
    if args.out:
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
