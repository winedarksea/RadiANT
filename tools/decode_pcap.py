# SPDX-License-Identifier: Apache-2.0

"""Decode a USBPcap capture of the dongle into one line per ANT serial message.

Companion to ``scripts/cap_zwift.ps1``. The original pair of scratch tools was
lost; ``archive/host-api/README.md`` describes the decode they produced, and
this reproduces that format:

    devices seen (addr: packets): {1: 43134}
    bulk payloads: 7614
         1.131  host->dev  SYSTEM_RESET 00
         1.209  host->dev  NETWORK_KEY 00b9a521fbbd72c345

Only bulk transfers carry the ANT serial stream. USB does not preserve message
boundaries, so payloads are concatenated per (bus, device, endpoint, direction) and re-framed
on the 0xA4 sync byte with the trailing XOR checksum verified - a message split
across two URBs decodes correctly, and a bad checksum is reported rather than
skipped silently. Zwift pads every OUT transfer with ``00 00``; skipped runs
that are all zero are counted as ``padding_bytes``, not ``discarded_bytes``.

Do not read "devices seen" as a device count. USBPcap writes 0xFF (255) in the
address field of URBs it cannot map, so ONE physical dongle routinely appears
as two entries: its control transfers under the real address and its bulk
stream under 255. A 2026-08-18 Zwift capture taken with `--devices 7` reported
`{7: 74, 255: 22162}`, and every ANT byte in it was under 255. Use --device
only to separate genuinely different devices in an -All capture, and check the
first bytes with --raw before trusting an address: ANT frames start a4.

Usage:
    python tools/decode_pcap.py capture.pcap [-o decoded.txt] [--raw] [--summary]
    python tools/decode_pcap.py capture.pcap --phase 3 2 [--period 3=8192]

With -o, the header (summary, diagnostics, warnings) or the --phase report
goes to stdout and the file holds only the body: decoded lines, --summary
counts or phase rows. Without -o, both go to stdout, header first.

The header has, per channel:

- ``transfers``: the acknowledged-data ledger. Submissions are counted from
  the host's own ACKNOWLEDGED_DATA, not from RESPONSE_NO_ERROR (the Garmin
  USB-m never sends one for 0x4F, so counting responses showed it accepting
  nothing). A refusal takes back the newest submission, a TX_COMPLETED or
  TX_FAILED settles the oldest. On a capture that starts mid-session, a
  channel's ledger starts at its ASSIGN_CHANNEL / OPEN_CHANNEL or at the first
  terminal event; everything before is ``presync``, outside the totals.
- ``slots``: loss from the dongle's receive timestamps (below), beside the
  EVENT_RX_FAIL count. The two disagree when a stack drops a slot without
  reporting it.
- one line per FE-C page (device type 17) of the transfer ledger, which is
  where "page 0x33 completion" comes from.

FE-C frames (a channel whose CHANNEL_ID, or whose frames' extended device id,
says device type 17) get a decoded suffix such as
``[FE-C 0x33 grade=+2.35% crr=0.0040]``. Page numbers mean different things on
other profiles, so nothing is decoded without that device type.

Slot timeline: the dongle's receive timestamp (the 0x20 extended field,
32768 Hz, LIB_CONFIG 0x20, which Zwift sets) places every heard frame on its
channel's slot grid. Misses between two heard frames are round(dticks /
period) - 1, re-anchored on every frame so crystal drift does not build up.
The 16-bit counter is unwrapped from elapsed host time, so a quiet gap longer
than the 2 s wrap is still counted right. A gap across a close, re-assign or
reset of the channel is not counted. The period comes from --period CH=N, else
the host's CHANNEL_PERIOD, else is inferred from the timestamps (the median
spacing of adjacent heard frames); the source is printed beside it.

--phase A B prints, for every slot of channel A, whether its frame reached
the host and where channel B's nearest frame sat relative to it, then a
histogram of A's misses by that offset and the longest run of consecutive
misses. Both channels must deliver timestamps. Offsets are B minus A:
negative means B's frame came first.
"""

from __future__ import annotations

import argparse
import bisect
import statistics
import struct
import sys
from collections import Counter, deque
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ant_pages  # noqa: E402
import ant_wire  # noqa: E402

SYNC = 0xA4

# Suffix on a frame recovered at EOF from inside an abandoned candidate.
HEURISTIC_MARK = "[heuristic: recovered at EOF]"

# USBPcap transfer types (USBPCAP_TRANSFER_* in USBPcap's driver headers).
TRANSFER_ISOCHRONOUS = 0
TRANSFER_INTERRUPT = 1
TRANSFER_CONTROL = 2
TRANSFER_BULK = 3

PCAP_MAGIC_US = 0xA1B2C3D4
PCAP_MAGIC_NS = 0xA1B23C4D
DLT_USBPCAP = 249

TICK_HZ = 32768
TICK_WRAP = 65536
DEFAULT_PERIOD = 8192
# An inferred period needs this many adjacent-frame spacings to stand on.
INFER_MIN_SPACINGS = 8
# Spacings below this are not slot spacings (ANT's fastest useful rate is far
# slower than 256 ticks, ~7.8 ms).
INFER_MIN_TICKS = 256

# Host messages after which a channel's slot grid does not carry on.
CHANNEL_BREAK_IDS = (ant_wire.MESG_UNASSIGN_CHANNEL_ID, ant_wire.MESG_ASSIGN_CHANNEL_ID,
                     ant_wire.MESG_OPEN_CHANNEL_ID, ant_wire.MESG_CLOSE_CHANNEL_ID)
# Host messages that put a channel's transfer ledger in step.
LEDGER_SYNC_IDS = (ant_wire.MESG_ASSIGN_CHANNEL_ID, ant_wire.MESG_OPEN_CHANNEL_ID)
RX_DATA_IDS = (ant_wire.MESG_BROADCAST_DATA_ID, ant_wire.MESG_ACKNOWLEDGED_DATA_ID)


def _message_names() -> dict[int, str]:
    """Build id -> name from ant_wire's MESG_*_ID constants.

    Reflection rather than a second hand-maintained table: a name that drifts
    from the wire module is worse than no name at all.
    """
    names: dict[int, str] = {}
    for attr in dir(ant_wire):
        if not attr.startswith("MESG_") or not attr.endswith("_ID"):
            continue
        value = getattr(ant_wire, attr)
        if not isinstance(value, int) or not 0 <= value <= 0xFF:
            continue
        name = attr[len("MESG_") : -len("_ID")]
        # Prefer the shortest spelling when several constants share an id.
        if value not in names or len(name) < len(names[value]):
            names[value] = name
    # Spellings the archived 2026-08-10 decode used, kept so old and new
    # decodes diff cleanly.
    names.setdefault(0x43, "CHANNEL_PERIOD")
    names[0x43] = "CHANNEL_PERIOD"
    names[0x51] = "CHANNEL_ID"
    return names


MESSAGE_NAMES = _message_names()
MESSAGE_IDS = {name: msg_id for msg_id, name in MESSAGE_NAMES.items()}


class PcapReader:
    """Minimal classic-pcap reader. USBPcapCMD writes classic pcap, not pcapng."""

    def __init__(self, path: Path, diagnostics=None):
        self.diagnostics = diagnostics if diagnostics is not None else Counter()
        self.data = path.read_bytes()
        if len(self.data) < 24:
            raise ValueError(f"{path} is too short to be a pcap file")
        magic = struct.unpack("<I", self.data[:4])[0]
        if magic in (PCAP_MAGIC_US, PCAP_MAGIC_NS):
            self.endian = "<"
        elif struct.unpack(">I", self.data[:4])[0] in (PCAP_MAGIC_US, PCAP_MAGIC_NS):
            self.endian = ">"
            magic = struct.unpack(">I", self.data[:4])[0]
        else:
            raise ValueError(
                f"{path} is not a classic pcap file (magic {magic:#010x}). "
                "If this is pcapng, re-capture with USBPcapCMD -o (it writes pcap)."
            )
        self.tick = 1e-9 if magic == PCAP_MAGIC_NS else 1e-6
        self.linktype = struct.unpack(self.endian + "I", self.data[20:24])[0]
        if self.linktype != DLT_USBPCAP:
            raise ValueError(
                f"{path} has link type {self.linktype}, expected {DLT_USBPCAP} (USBPcap)"
            )

    def packets(self):
        off = 24
        end = len(self.data)
        while off + 16 <= end:
            ts_sec, ts_frac, incl, _orig = struct.unpack(
                self.endian + "IIII", self.data[off : off + 16]
            )
            off += 16
            if off + incl > end:
                self.diagnostics['truncated_pcap_records'] += 1
                return
            if incl < _orig:
                self.diagnostics['snaplen_truncated_records'] += 1
            yield ts_sec + ts_frac * self.tick, self.data[off : off + incl]
            off += incl
        if off != end:
            self.diagnostics['truncated_pcap_records'] += 1


@dataclass(frozen=True)
class UsbRecord:
    device: int
    transfer: int
    from_device: bool
    payload: bytes
    bus: int
    endpoint: int
    status: int
    truncated: bool


def parse_usbpcap(buf: bytes):
    """Parse one USBPcap record, retaining stream identity and USB errors."""
    if len(buf) < 27:
        return None
    header_len = struct.unpack("<H", buf[:2])[0]
    if header_len < 27 or header_len > len(buf):
        return None
    # headerLen H, irpId Q, status I, function H, info B, bus H, device H,
    # endpoint B, transfer B, dataLength I
    (_hlen, _irp, _status, _func, info, _bus, device, endpoint, transfer,
     data_len) = struct.unpack("<HQIHBHHBBI", buf[:27])
    payload = buf[header_len : header_len + data_len]
    # info bit 0 set = PDO -> FDO, i.e. the completion coming back from the
    # device. For bulk the endpoint direction bit is the reliable signal.
    from_device = bool(endpoint & 0x80) if transfer == TRANSFER_BULK else bool(info & 0x01)
    return UsbRecord(device, transfer, from_device, payload, _bus, endpoint,
                     _status, header_len + data_len > len(buf))


def _skip(diagnostics, skipped) -> None:
    """Count bytes skipped on the way to a sync byte. An all-zero run is
    Zwift's ``00 00`` OUT padding, not lost data."""
    if not skipped:
        return
    if not any(skipped):
        diagnostics['padding_bytes'] += len(skipped)
    else:
        diagnostics['discarded_bytes'] += len(skipped)


def reframe(stream: bytearray, diagnostics=None, *, final=False):
    """Yield (msg_id, payload, checksum_ok, recovered) for frame candidates.

    Consumes what it decodes; leftovers stay in ``stream`` for the next URB.
    At EOF only, an incomplete candidate may be abandoned for a later complete,
    checksum-valid frame. Never do this while more URBs could complete it:
    a legitimate payload can itself contain an entire ANT frame.

    Every frame after such an abandonment is ``recovered``: it lies inside the
    abandoned candidate's claimed length, so it may be payload of a real but
    truncated message. A checksum cannot tell the two apart, so the caller
    must keep recovered frames out of anything it reports as a count.
    """
    diagnostics = diagnostics if diagnostics is not None else Counter()
    recovered = False
    while True:
        start = stream.find(SYNC)
        if start < 0:
            _skip(diagnostics, stream)
            stream.clear()
            return
        if start:
            _skip(diagnostics, stream[:start])
            del stream[:start]
        if len(stream) < 4:
            return
        length = stream[1]
        total = length + 4  # sync + len + id + payload + checksum
        if len(stream) < total:
            if final:
                for offset in range(1, len(stream) - 3):
                    if stream[offset] != SYNC:
                        continue
                    end = offset + stream[offset + 1] + 4
                    if end > len(stream):
                        continue
                    checksum = 0
                    for byte in stream[offset:end]:
                        checksum ^= byte
                    if checksum == 0:
                        diagnostics['incomplete_candidates_resynchronized'] += 1
                        diagnostics['discarded_bytes'] += offset
                        del stream[:offset]
                        recovered = True
                        break
                else:
                    return
                continue
            return
        msg = bytes(stream[:total])
        chk = 0
        for byte in msg[:-1]:
            chk ^= byte
        ok = chk == msg[-1]
        if not ok:
            # Not a real frame - the 0xA4 was data. Drop just that byte and
            # resynchronise rather than swallowing the whole candidate.
            del stream[:1]
            diagnostics['discarded_bytes'] += 1
            diagnostics['bad_checksum_candidates'] += 1
            yield msg[2], msg[3:-1], False, recovered
            continue
        del stream[:total]
        yield msg[2], msg[3:-1], True, recovered


# ---------------------------------------------------------------------------
# Receive timestamps and the slot timeline
# ---------------------------------------------------------------------------

EXT_DEVICE_ID = 0x80
EXT_RSSI = 0x40
EXT_TIMESTAMP = 0x20


def rx_ticks(body: bytes):
    """(channel, ticks) of a flagged BROADCAST_DATA body, or None without a
    timestamp. Layout: ch, 8 payload, flag, then the flagged fields in the
    order device id (4), RSSI (3), timestamp (2 LE). The timestamp is read
    from the end: the Garmin USB-m sends one byte more than that layout
    (20-byte body with all three flags), and its last two bytes are the
    ticks (they track host time at 32768 Hz; the computed offset does not)."""
    if len(body) < 10:
        return None
    flag = body[9]
    if not flag & EXT_TIMESTAMP:
        return None
    off = 10 + (4 if flag & EXT_DEVICE_ID else 0) + (3 if flag & EXT_RSSI else 0)
    if len(body) < off + 2:
        return None
    return body[0] & 0x1F, body[-2] | (body[-1] << 8)


def ext_device(body: bytes):
    """(device number, device type) from a flagged data body's extended
    device id, or None. The device id comes first after the flag byte, so the
    Garmin's extra trailing byte does not move it."""
    if len(body) < 14 or not body[9] & EXT_DEVICE_ID:
        return None
    return body[10] | (body[11] << 8), body[12] & 0x7F


def infer_period(ticks: list[float]) -> int | None:
    """The median spacing of adjacent heard frames: spacings under 1.5 x the
    smallest are one-slot spacings. None without enough of them."""
    spacings = [b - a for a, b in zip(ticks, ticks[1:]) if b - a >= INFER_MIN_TICKS]
    if not spacings:
        return None
    one_slot = [d for d in spacings if d < 1.5 * min(spacings)]
    if len(one_slot) < INFER_MIN_SPACINGS:
        return None
    return round(statistics.median(one_slot))


@dataclass
class ChannelPeriod:
    at: object                 # host time -> period in ticks
    text: str                  # e.g. "8192 (CHANNEL_PERIOD)"
    source: str                # "--period", "CHANNEL_PERIOD", "inferred", "default"
    first: int                 # period in effect at the first frame
    warning: str | None = None


@dataclass
class Timing:
    """What the slot timeline needs from one dongle's message stream.

    Fed in host-time order through note_host() / note_dev(), from decode()'s
    structured messages or from decoded text lines (timing_from_lines()).
    """
    rx: list = field(default_factory=list)       # (t, ch, raw ticks, ext device or None)
    periods: dict = field(default_factory=dict)  # ch -> [(t, period)]
    breaks: dict = field(default_factory=dict)   # ch -> [t]
    resets: list = field(default_factory=list)   # SYSTEM_RESET times: break every channel
    rx_frames: Counter = field(default_factory=Counter)
    rx_fail: Counter = field(default_factory=Counter)

    def note_host(self, t: float, msg_id: int, body: bytes) -> None:
        if msg_id == ant_wire.MESG_SYSTEM_RESET_ID:
            self.resets.append(t)
            return
        if not body:
            return
        ch = body[0] & 0x1F
        if msg_id == ant_wire.MESG_CHANNEL_MESG_PERIOD_ID and len(body) >= 3:
            self.periods.setdefault(ch, []).append((t, body[1] | (body[2] << 8)))
        elif msg_id in CHANNEL_BREAK_IDS:
            self.breaks.setdefault(ch, []).append(t)

    def note_dev(self, t: float, msg_id: int, body: bytes) -> None:
        if not body:
            return
        ch = body[0] & 0x1F
        if msg_id in RX_DATA_IDS:
            self.rx_frames[ch] += 1
            got = rx_ticks(body)
            if got is not None:
                self.rx.append((t, ch, got[1], ext_device(body)))
        elif (msg_id == ant_wire.MESG_RESPONSE_EVENT_ID and len(body) >= 3
              and body[1] == ant_wire.MESG_EVENT_ID):
            if body[2] == ant_wire.EVENT_RX_FAIL:
                self.rx_fail[ch] += 1
            elif body[2] == ant_wire.EVENT_CHANNEL_CLOSED:
                self.breaks.setdefault(ch, []).append(t)

    def frames(self) -> dict[int, list[tuple[float, float]]]:
        """ch -> [(host time, unwrapped ticks)].

        One counter serves every channel, so it is unwrapped once across all
        of them in host order: wraps = round((dhost * 32768 - dticks) / 65536)
        from the previous frame on any channel. The counter wraps every 2 s, so
        a gap with no frame on any channel can hide whole wraps; host time
        settles the count (USB jitter is milliseconds, the ambiguity is a full
        second)."""
        out: dict[int, list[tuple[float, float]]] = {}
        prev = None
        for t, ch, k, _dev in sorted(self.rx, key=lambda r: r[0]):
            if prev is not None:
                expect = prev[1] + (t - prev[0]) * TICK_HZ
                k += TICK_WRAP * round((expect - k) / TICK_WRAP)
            prev = (t, k)
            out.setdefault(ch, []).append((t, k))
        return out

    def devices(self, ch: int) -> set:
        return {dev[0] for _t, c, _k, dev in self.rx if c == ch and dev is not None}

    def period(self, ch: int, ticks: list[float], overrides=None) -> ChannelPeriod:
        """Period precedence: --period, then CHANNEL_PERIOD, then inferred."""
        first_t = None
        for t, c, _k, _d in self.rx:
            if c == ch:
                first_t = t
                break
        if overrides and ch in overrides:
            p = overrides[ch]
            return ChannelPeriod(lambda t: p, f'{p} (--period)', '--period', p)
        inferred = infer_period(ticks)
        declared = self.periods.get(ch, [])
        if declared:
            times = [t for t, _ in declared]

            def at(t, declared=declared, times=times):
                i = bisect.bisect_right(times, t) - 1
                return declared[max(i, 0)][1]

            values = sorted({p for _, p in declared})
            warning = None
            if inferred is not None and all(abs(inferred - v) > 0.01 * v for v in values):
                warning = (f'warning: ch={ch} inferred period {inferred} differs from '
                           f'CHANNEL_PERIOD {"/".join(map(str, values))} by more than 1 %')
            first = at(first_t if first_t is not None else 0.0)
            return ChannelPeriod(at, f'{"/".join(map(str, values))} (CHANNEL_PERIOD)',
                                 'CHANNEL_PERIOD', first, warning)
        if inferred is not None:
            return ChannelPeriod(lambda t: inferred, f'{inferred} (inferred)',
                                 'inferred', inferred)
        return ChannelPeriod(lambda t: DEFAULT_PERIOD,
                             f'{DEFAULT_PERIOD} (default: too few frames to infer)',
                             'default', DEFAULT_PERIOD)

    def channel_breaks(self, ch: int) -> list[float]:
        return sorted(self.breaks.get(ch, []) + self.resets)


def timeline(frames, period_at, breaks):
    """Per-slot rows (host time, heard, ticks) from one channel's heard frames.

    Misses between consecutive heard frames are round(dticks / period) - 1,
    re-anchored on each frame. A missed slot's ticks are interpolated between
    its neighbours and its host time extrapolated from the earlier one. A gap
    that spans a break (close, re-assign, reset) adds no rows: the channel was
    not supposed to be hearing anything across it.
    """
    rows = []
    prev = None
    for t, k in frames:
        if prev is not None:
            per = period_at(prev[0])
            n = round((k - prev[1]) / per)
            if n <= 0:
                continue        # a second frame in the same slot
            i = bisect.bisect_left(breaks, prev[0])
            if not (i < len(breaks) and breaks[i] <= t):
                for j in range(1, n):
                    rows.append((prev[0] + j * per / TICK_HZ, False,
                                 prev[1] + (k - prev[1]) * j / n))
        rows.append((t, True, k))
        prev = (t, k)
    return rows


def longest_run(rows) -> tuple[int, int | None]:
    """(length, index of its last row) of the longest run of missed rows."""
    run = best = 0
    best_end = None
    for i, r in enumerate(rows):
        run = run + 1 if not r[1] else 0
        if run > best:
            best, best_end = run, i
    return best, best_end


def timing_from_lines(lines) -> Timing:
    """Rebuild a Timing from decoded text lines (time, direction, name, hex,
    then an optional bracketed suffix that is ignored)."""
    timing = Timing()
    for line in lines:
        parts = line.split()
        if len(parts) < 4 or line.endswith(HEURISTIC_MARK):
            continue
        msg_id = MESSAGE_IDS.get(parts[2])
        if msg_id is None and parts[2].startswith('MSG_'):
            try:
                msg_id = int(parts[2][4:], 16)
            except ValueError:
                msg_id = None
        if msg_id is None:
            continue
        try:
            t = float(parts[0])
            body = bytes.fromhex(parts[3])
        except ValueError:
            continue
        if parts[1] == 'host->dev':
            timing.note_host(t, msg_id, body)
        elif parts[1] == 'dev->host':
            timing.note_dev(t, msg_id, body)
    return timing


def slot_timeline(lines, ch: int, period: int | None = None):
    """Per-slot heard/missed rows for channel `ch` from decoded text lines.
    Returns (rows, ChannelPeriod)."""
    timing = timing_from_lines(lines)
    frames = timing.frames().get(ch, [])
    per = timing.period(ch, [k for _, k in frames],
                        {ch: period} if period is not None else None)
    return timeline(frames, per.at, timing.channel_breaks(ch)), per


def slot_summary(timing: Timing, ch: int, overrides=None) -> list[str]:
    """The summary's slot-loss text for one channel (and a period warning)."""
    devices = timing.devices(ch)
    if len(devices) > 1:
        return [f'slots n/a (frames from {len(devices)} devices: a scan channel)']
    frames = timing.frames().get(ch, [])
    if not frames:
        return []
    per = timing.period(ch, [k for _, k in frames], overrides)
    rows = timeline(frames, per.at, timing.channel_breaks(ch))
    heard = sum(1 for r in rows if r[1])
    run, _ = longest_run(rows)
    fails = timing.rx_fail[ch]
    rx_total = fails + timing.rx_frames[ch]
    out = [f'slots={len(rows)} heard={heard} '
           f'loss={100.0 * (len(rows) - heard) / max(1, len(rows)):.1f} % '
           f'(rx_fail={fails}, {100.0 * fails / max(1, rx_total):.1f} %) '
           f'longest_run={run} period {per.text}']
    if per.warning:
        out.append(per.warning)
    return out


# ---------------------------------------------------------------------------
# FE-C
# ---------------------------------------------------------------------------

def fec_annotation(payload: bytes) -> str:
    """``[FE-C 0x33 grade=+2.35% crr=0.0040]`` for one 8-byte FE-C payload,
    decoded by ant_pages' FE-C table."""
    page = payload[0]
    try:
        d = ant_pages.decode(payload, ant_pages.FEC_DEVICE_TYPE)
    except (ValueError, KeyError, IndexError):
        d = {}
    state = ant_pages.FEC_STATE_NAMES.get(d.get('state'), d.get('state'))
    fields = ''
    if page == ant_pages.PAGE_FEC_TRACK_RESISTANCE:
        grade = d['grade_centi_pct']
        crr = d['rolling_resistance']
        fields = (f'grade={"unset(flat)" if grade is None else f"{grade / 100:+.2f}%"} '
                  f'crr={"default" if crr is None else f"{crr * 5e-5:.4f}"}')
    elif page == ant_pages.PAGE_FEC_TARGET_POWER:
        fields = f'target={d["power_quarter_w"] / 4:.2f}W'
    elif page == ant_pages.PAGE_FEC_TRAINER:
        cad = '-' if d['cadence'] is None else d['cadence']
        power = f'{d["inst_power"]}W' if d['power_valid'] else 'invalid'
        fields = (f'ev={d["event_count"]} cad={cad} accp={d["acc_power"]} '
                  f'power={power} status=0x{d["trainer_status"]:X} state={state}')
    elif page == ant_pages.PAGE_FEC_GENERAL:
        speed = f'{d["speed_mm_s"] / 1000:.3f}m/s' if d['speed_valid'] else '-'
        hr = d['heart_rate'] if d['heart_rate_valid'] else '-'
        fields = (f'type={d["equipment_type"]} t={d["elapsed_time_qs"] / 4:.2f}s '
                  f'dist={d["distance_m"]}m speed={speed} hr={hr} state={state}')
    elif page == ant_pages.PAGE_FEC_CMD_STATUS:
        status = ant_pages.FEC_CMD_STATUS_NAMES.get(d['status'], f'0x{d["status"]:02X}')
        fields = f'last=0x{d["last_command"]:02X} seq={d["sequence"]} status={status}'
    return f'[FE-C 0x{page:02X}{" " + fields if fields else ""}]'


# ---------------------------------------------------------------------------
# Acknowledged-data ledger
# ---------------------------------------------------------------------------

class Ledger:
    """One channel's acknowledged-data transfers, from both directions.

    - A host->dev ACKNOWLEDGED_DATA is a submission: pushed on a FIFO with
      its page.
    - A non-zero RESPONSE for 0x4F refuses the NEWEST submission (the
      response follows its submit straight away).
    - RESPONSE_NO_ERROR is counted as `confirmed` and nothing else: it is the
      same submission, not a second one, and the Garmin never sends it.
    - TX_COMPLETED / TX_FAILED settle the OLDEST submission.

    There is no "superseded" case: a submit while a transfer is pending is
    refused with TRANSFER_IN_PROGRESS, not swapped in for the pending one
    (radiant_ant_acknowledge_message_tx() in radiant/src/ant_api/radiant_ant.c;
    the Garmin does the same, 142 refusals in the 2026-11-15 22:06 capture),
    and a slave's command whose opener never comes still ends in TX_FAILED
    (the RADIANT_API_ACK_OPENER_PERIODS expiry). So every accepted submit
    gets exactly one terminal event, and the refusal rule alone keeps the
    FIFO aligned.

    Until the channel is in step - its ASSIGN_CHANNEL or OPEN_CHANNEL seen, or
    a SYSTEM_RESET - everything up to and including the first terminal event
    is `presync` and outside the totals: that event may settle a transfer
    submitted before the capture started.
    """

    COLUMNS = ('submitted', 'accepted', 'refused', 'completed', 'failed',
               'pending', 'presync', 'confirmed')
    PAGE_COLUMNS = ('submitted', 'accepted', 'refused', 'completed', 'failed',
                    'pending')

    # A gap this short between a TX_FAILED and a GO_TO_SEARCH counts as "at"
    # the search transition, for the `gts order` line.
    GTS_NEAR_S = 0.050

    def __init__(self, synced: bool = False):
        self.synced = synced
        self.fifo: deque[int | None] = deque()
        # Parallel to `fifo`: the channel state each pending submit has seen,
        # for the `by_state` breakdown (TODO item 3). The channel counts as
        # SEARCHING from EVENT_RX_FAIL_GO_TO_SEARCH until its next
        # BROADCAST_DATA.
        self.meta: deque[dict] = deque()
        self.counts: Counter[str] = Counter()
        self.pages: dict[int | None, Counter] = {}
        self.searching = False
        self.by_state: dict[str, Counter] = {}
        self.refused_behind: Counter[str] = Counter()
        self.gts_order: Counter[str] = Counter()
        self.last_gts: float | None = None
        self.last_failed: float | None = None

    @staticmethod
    def state_key(m: dict) -> str:
        return (('search' if m['searching'] else 'track') + '/'
                + ('crossed' if m['crossed'] else 'no_cross') + '/'
                + ('heard' if m['heard'] else 'unheard'))

    def _state(self, m: dict, what: str) -> None:
        self.by_state.setdefault(self.state_key(m), Counter())[what] += 1

    def go_to_search(self, t: float = 0.0) -> None:
        self.searching = True
        for m in self.meta:
            m['crossed'] = True
        if self.last_failed is not None and t - self.last_failed <= self.GTS_NEAR_S:
            self.gts_order['failed_then_gts'] += 1
        self.last_gts = t

    def broadcast(self, t: float = 0.0) -> None:
        """A master frame on this channel: tracking again. A pending submit
        made in search, or that crossed a GO_TO_SEARCH, was still alive when
        the channel reacquired (`heard`)."""
        if not self.searching:
            return
        self.searching = False
        for m in self.meta:
            if m['searching'] or m['crossed']:
                m['heard'] = True

    def _page(self, page, what: str) -> None:
        self.pages.setdefault(page, Counter())[what] += 1

    def submit(self, page, t: float = 0.0) -> None:
        if not self.synced:
            self.counts['presync'] += 1
            return
        self.counts['submitted'] += 1
        self._page(page, 'submitted')
        self.fifo.append(page)
        self.meta.append({'searching': self.searching, 'crossed': False,
                          'heard': False, 't': t})

    def response(self, code: int) -> None:
        if not self.synced:
            self.counts['presync'] += 1
        elif code == ant_wire.RESPONSE_NO_ERROR:
            self.counts['confirmed'] += 1
        elif self.fifo:
            self.counts['refused'] += 1
            self._page(self.fifo.pop(), 'refused')
            self._state(self.meta.pop(), 'refused')
            # What the refused submit was stuck behind: the oldest pending one.
            if self.meta:
                b = self.meta[0]
                self.refused_behind['crossed' if b['crossed'] else
                                    'search_submit' if b['searching'] else
                                    'tracking'] += 1
            else:
                self.refused_behind['nothing_pending'] += 1
        else:
            self.counts['unmatched'] += 1

    def terminal(self, completed: bool, t: float = 0.0) -> None:
        if not completed:
            self.last_failed = t
            if self.last_gts is not None and t - self.last_gts <= self.GTS_NEAR_S:
                self.gts_order['gts_then_failed'] += 1
        if not self.synced:
            self.counts['presync'] += 1
            self.synced = True
            return
        outcome = 'completed' if completed else 'failed'
        if self.fifo:
            self.counts[outcome] += 1
            self._page(self.fifo.popleft(), outcome)
            self._state(self.meta.popleft(), outcome)
        else:
            self.counts['unmatched'] += 1

    def reset(self) -> None:
        """SYSTEM_RESET: whatever was pending never settles."""
        while self.fifo:
            self.counts['pending'] += 1
            self._page(self.fifo.popleft(), 'pending')
            self._state(self.meta.popleft(), 'pending')
        self.synced = True
        self.searching = False

    def totals(self) -> Counter:
        c = Counter(self.counts)
        c['pending'] += len(self.fifo)
        c['accepted'] = c['submitted'] - c['refused']
        return c

    def page_totals(self) -> dict:
        out = {}
        for page, pc in self.pages.items():
            c = Counter(pc)
            c['pending'] += sum(1 for p in self.fifo if p == page)
            c['accepted'] = c['submitted'] - c['refused']
            out[page] = c
        return out

    STATE_COLUMNS = ('completed', 'failed', 'refused', 'pending')

    def state_totals(self) -> dict:
        out = {k: Counter(v) for k, v in self.by_state.items()}
        for m in self.meta:
            out.setdefault(self.state_key(m), Counter())['pending'] += 1
        return out

    def __bool__(self) -> bool:
        return bool(self.counts) or bool(self.fifo)


# ---------------------------------------------------------------------------
# decode()
# ---------------------------------------------------------------------------

def decode(path: Path, raw: bool = False, only_device: int | None = None,
           periods: dict[int, int] | None = None):
    """Decode `path`. Returns (header lines, body lines). `periods` maps a
    channel to a period that overrides CHANNEL_PERIOD and inference."""
    diagnostics: Counter[str] = Counter()
    reader = PcapReader(path, diagnostics)
    devices: Counter[int] = Counter()
    # Keyed by (bus, device, endpoint, direction), NOT direction alone. A capture taken with
    # -All carries every device behind the filter device, and concatenating two
    # devices' bulk bytes into one buffer splices unrelated streams together:
    # the reframer then resynchronises through the wreckage and emits messages
    # that were never sent - a real risk, not a theoretical one (see the module
    # docstring's `{7: 74, 255: 22162}` capture).
    streams: dict[tuple[int, int, int, bool], bytearray] = {}
    lines: list[str] = []
    warnings: list[str] = []
    channel_counts: Counter[tuple] = Counter()
    # The ledger pairs host->dev submits with dev->host responses under one
    # (bus, device, ch) key. That works because both bulk directions carry
    # the same address: checked on all seven 2026-11-11/15 Zwift captures,
    # 255 both ways on the Garmin and the RadiANT rigs, 8 both ways on the
    # one that USBPcap could map.
    ledgers: dict[tuple, Ledger] = {}
    reset_seen: set[tuple[int, int]] = set()
    timings: dict[tuple[int, int], Timing] = {}
    chan_type: dict[tuple, int] = {}
    fec_channels: set[tuple] = set()
    events: Counter[tuple] = Counter()
    responses: Counter[tuple] = Counter()
    event_names = {getattr(ant_wire, name): name for name in dir(ant_wire)
                   if name.startswith('EVENT_') and isinstance(getattr(ant_wire, name), int)}
    response_names = {getattr(ant_wire, name): name for name in (
        'RESPONSE_NO_ERROR', 'CHANNEL_IN_WRONG_STATE', 'INVALID_MESSAGE',
        'CHANNEL_NOT_OPENED', 'TRANSFER_IN_PROGRESS', 'INVALID_PARAMETER_PROVIDED')}
    t0 = None
    bulk_payloads = 0

    stream_times = {}

    def ledger(key) -> Ledger:
        if key not in ledgers:
            ledgers[key] = Ledger(synced=key[:2] in reset_seen)
        return ledgers[key]

    def is_fec(key, body, from_device) -> bool:
        if from_device:
            dev = ext_device(body)
            if dev is not None:
                return dev[1] == ant_pages.FEC_DEVICE_TYPE
        return chan_type.get(key) == ant_pages.FEC_DEVICE_TYPE

    def consume(stream_key, rel, *, final=False):
        bus, device, endpoint, from_device = stream_key
        stream = streams[stream_key]
        direction = "dev->host" if from_device else "host->dev"
        timing = timings.setdefault((bus, device), Timing())
        for msg_id, body, ok, recovered in reframe(stream, diagnostics, final=final):
            if not ok:
                warnings.append(f"warning: {rel:.3f}s bus={bus} device={device} "
                                f"endpoint=0x{endpoint:02x}: bad checksum candidate")
                continue
            name = MESSAGE_NAMES.get(msg_id, f"MSG_{msg_id:02X}")
            if recovered:
                # Possibly bytes of a truncated real message: shown, never
                # counted, never annotated.
                diagnostics['heuristic_messages'] += 1
                lines.append(f"{rel:10.3f}  {direction}  {name} {body.hex()}"
                             f"  {HEURISTIC_MARK}")
                continue
            line = f"{rel:10.3f}  {direction}  {name} {body.hex()}"
            if from_device:
                timing.note_dev(rel, msg_id, body)
            else:
                timing.note_host(rel, msg_id, body)
            if msg_id == ant_wire.MESG_SYSTEM_RESET_ID and not from_device:
                reset_seen.add((bus, device))
                for (b, d, _c), led in ledgers.items():
                    if (b, d) == (bus, device):
                        led.reset()
            if not body:
                lines.append(line)
                continue
            ch = body[0] & 0x1F
            key = (bus, device, ch)
            if from_device:
                dev = ext_device(body) if msg_id in RX_DATA_IDS else None
                if dev is not None:
                    chan_type[key] = dev[1]
            elif msg_id == ant_wire.MESG_CHANNEL_ID_ID and len(body) >= 4:
                chan_type[key] = body[3] & 0x7F
            if (len(body) >= 9 and (msg_id in RX_DATA_IDS if from_device
                                    else msg_id == ant_wire.MESG_ACKNOWLEDGED_DATA_ID)
                    and is_fec(key, body, from_device)):
                fec_channels.add(key)
                line += '  ' + fec_annotation(bytes(body[1:9]))
            lines.append(line)

            if not from_device:
                if msg_id in LEDGER_SYNC_IDS:
                    ledger(key).synced = True
                elif msg_id == ant_wire.MESG_ACKNOWLEDGED_DATA_ID:
                    ledger(key).submit(body[1] if len(body) >= 2 else None, rel)
                continue
            if msg_id == ant_wire.MESG_BROADCAST_DATA_ID:
                channel_counts[key] += 1
                ledger(key).broadcast(rel)
            if msg_id == ant_wire.MESG_RESPONSE_EVENT_ID and len(body) >= 3:
                ref, code = body[1:3]
                if ref == ant_wire.MESG_EVENT_ID:
                    events[(*key, code)] += 1
                    if code in (ant_wire.EVENT_TRANSFER_TX_COMPLETED,
                                ant_wire.EVENT_TRANSFER_TX_FAILED):
                        ledger(key).terminal(code == ant_wire.EVENT_TRANSFER_TX_COMPLETED,
                                             rel)
                    elif code == ant_wire.EVENT_RX_FAIL_GO_TO_SEARCH:
                        ledger(key).go_to_search(rel)
                else:
                    responses[(*key, ref, code)] += 1
                    if ref == ant_wire.MESG_ACKNOWLEDGED_DATA_ID:
                        ledger(key).response(code)

    for ts, buf in reader.packets():
        parsed = parse_usbpcap(buf)
        if parsed is None:
            diagnostics['malformed_usb_records'] += 1
            continue
        device, transfer = parsed.device, parsed.transfer
        from_device, payload = parsed.from_device, parsed.payload
        devices[device] += 1
        if only_device is not None and device != only_device:
            continue
        if parsed.status:
            diagnostics['nonzero_usb_statuses'] += 1
        if parsed.truncated:
            diagnostics['truncated_usb_payloads'] += 1
        if transfer != TRANSFER_BULK:
            continue
        stream = streams.setdefault((parsed.bus, device, parsed.endpoint, from_device),
                                    bytearray())
        if parsed.status or parsed.truncated:
            # A failed zero-length completion also breaks an existing stream.
            diagnostics['discarded_bytes'] += len(stream) + len(payload)
            stream.clear()
            bulk_payloads += bool(payload)
            continue
        if not payload:
            continue
        bulk_payloads += 1
        if t0 is None:
            t0 = ts
        rel = ts - t0
        direction = "dev->host" if from_device else "host->dev"
        if raw:
            lines.append(f"{rel:10.3f}  {direction}  raw {payload.hex()}")
        stream.extend(payload)
        stream_key = (parsed.bus, device, parsed.endpoint, from_device)
        stream_times[stream_key] = rel
        consume(stream_key, rel)

    # EOF can prove that a leading length byte will never be satisfied.
    # Recover complete frames behind it, with the last URB timestamp for
    # that stream (the same timestamp used for normal split-frame completion).
    for stream_key, rel in stream_times.items():
        consume(stream_key, rel, final=True)

    for stream in streams.values():
        if stream:
            diagnostics['incomplete_ant_streams'] += 1
            diagnostics['incomplete_ant_bytes'] += len(stream)

    header = [
        f"devices seen (addr: packets): {dict(devices)}",
        f"bulk payloads: {bulk_payloads}",
    ]
    diagnostic_names = ('bad_checksum_candidates', 'discarded_bytes', 'padding_bytes',
                        'incomplete_ant_streams', 'incomplete_ant_bytes',
                        'incomplete_candidates_resynchronized',
                        'heuristic_messages',
                        'truncated_pcap_records', 'snaplen_truncated_records',
                        'malformed_usb_records', 'truncated_usb_payloads',
                        'nonzero_usb_statuses')
    header.append('diagnostics: ' + ' '.join(f'{k}={diagnostics[k]}' for k in diagnostic_names))
    for (bus, device, ch), count in sorted(channel_counts.items()):
        header.append(f'bus={bus} device={device} ch={ch}: broadcasts={count}')
    for (bus, device), timing in sorted(timings.items()):
        for ch in sorted({r[1] for r in timing.rx}):
            for text in slot_summary(timing, ch, periods):
                header.append(f'bus={bus} device={device} ch={ch}: {text}')
    for (bus, device, ch), led in sorted(ledgers.items()):
        if not led:
            continue
        c = led.totals()
        text = ' '.join(f'{k}={c[k]}' for k in Ledger.COLUMNS)
        if c['unmatched']:
            text += f' unmatched={c["unmatched"]}'
        header.append(f'bus={bus} device={device} ch={ch}: transfers {text}')
        if (bus, device, ch) in fec_channels:
            for page, pc in sorted(led.page_totals().items(),
                                   key=lambda kv: -1 if kv[0] is None else kv[0]):
                label = '??' if page is None else f'0x{page:02X}'
                done = pc['completed'] + pc['failed']
                header.append(
                    f'bus={bus} device={device} ch={ch}: FE-C page {label} '
                    + ' '.join(f'{k}={pc[k]}' for k in Ledger.PAGE_COLUMNS)
                    + f' completion={100.0 * pc["completed"] / max(1, done):.1f} %')
        # Channel state per submit (TODO item 3): submitted while tracking or
        # searching / crossed a GO_TO_SEARCH while pending / a master frame
        # heard after search while it was still pending.
        for state, sc in sorted(led.state_totals().items()):
            header.append(f'bus={bus} device={device} ch={ch}: by_state {state} '
                          + ' '.join(f'{k}={sc[k]}' for k in Ledger.STATE_COLUMNS))
        if led.refused_behind:
            header.append(f'bus={bus} device={device} ch={ch}: refused behind '
                          + ' '.join(f'{k}={v}' for k, v in sorted(led.refused_behind.items())))
        if led.gts_order:
            header.append(f'bus={bus} device={device} ch={ch}: gts order '
                          + ' '.join(f'{k}={v}' for k, v in sorted(led.gts_order.items())))
    for (bus, device, ch, code), count in sorted(events.items()):
        header.append(f'bus={bus} device={device} ch={ch}: '
                      f'{event_names.get(code, f"EVENT_0x{code:02X}")}={count}')
    for (bus, device, ch, ref, code), count in sorted(responses.items()):
        header.append(f'bus={bus} device={device} ch={ch}: response '
                      f'{MESSAGE_NAMES.get(ref, f"0x{ref:02X}")} '
                      f'{response_names.get(code, f"0x{code:02X}")}={count}')
    header.extend(warnings)
    return header, lines


def phase(lines, ch_a: int, ch_b: int, bin_ms: float = 0.5,
          periods: dict[int, int] | None = None):
    """Per-slot table of channel A against channel B. Returns (rows, report):
    rows are (host_time, heard, b_minus_a_ms) for each slot of A between its
    first and last heard frame; report is a list of summary lines."""
    timing = timing_from_lines(lines)
    tl = timing.frames()
    a, b = tl.get(ch_a, []), tl.get(ch_b, [])
    if not a or not b:
        return [], [f'phase: no timestamped frames on channel '
                    f'{ch_a if not a else ch_b}']
    per_a = timing.period(ch_a, [k for _, k in a], periods)
    per_b = timing.period(ch_b, [k for _, k in b], periods)
    slots = timeline(a, per_a.at, timing.channel_breaks(ch_a))
    rows = []
    bk = [k for _, k in b]
    j = 0
    for t, heard, kf in slots:
        while j + 1 < len(bk) and abs(bk[j + 1] - kf) <= abs(bk[j] - kf):
            j += 1
        # Folded into +/- half of B's period: when B missed the frame of this
        # period, its nearest heard frame is a period away, but B drifts
        # slowly, so that frame's phase is still where this one would be.
        pb = per_b.at(t)
        d = bk[j] - kf
        d = (d + pb / 2) % pb - pb / 2
        rows.append((t, heard, d / 32.768))
    misses = [r for r in rows if not r[1]]
    best, best_end = longest_run(rows)
    source = '' if per_a.source == 'CHANNEL_PERIOD' else f' ({per_a.source})'
    report = [f'phase ch{ch_a} against ch{ch_b}: period {per_a.first}{source}, '
              f'{len(rows)} slots, {len(misses)} missed '
              f'({100.0 * len(misses) / max(1, len(rows)):.1f} %)']
    for p in (per_a, per_b):
        if p.warning:
            report.append(p.warning)
    if best_end is not None and best > 0:
        s = rows[best_end - best + 1]
        e = rows[best_end]
        report.append(f'longest miss run: {best} slots, {s[0]:.3f}-{e[0]:.3f} s, '
                      f'offset {s[2]:+.2f} .. {e[2]:+.2f} ms')
    bins: Counter[int] = Counter()
    totals: Counter[int] = Counter()
    for _, h, d in rows:
        if abs(d) <= 10.0:
            k = int(d // bin_ms)
            totals[k] += 1
            if not h:
                bins[k] += 1
    for k in sorted(totals):
        report.append(f'  {k * bin_ms:+6.1f} .. {(k + 1) * bin_ms:+6.1f} ms: '
                      f'{bins[k]:4d} / {totals[k]:4d} missed')
    return rows, report


def _period_arg(text: str) -> tuple[int, int]:
    try:
        ch, per = text.split('=', 1)
        ch_i, per_i = int(ch, 0), int(per, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f'expected CH=N, got {text!r}') from None
    if not 0 <= ch_i <= 31 or not 0 < per_i <= 0xFFFF:
        raise argparse.ArgumentTypeError(f'channel 0..31 and period 1..65535: {text!r}')
    return ch_i, per_i


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pcap", type=Path, help="capture from scripts/cap_zwift.ps1")
    ap.add_argument("-o", "--out", type=Path,
                    help="write the body here; the header goes to stdout")
    ap.add_argument("--raw", action="store_true",
                    help="also emit the raw bulk payload of every URB")
    ap.add_argument("--summary", action="store_true",
                    help="print message-id counts per direction instead of the log")
    ap.add_argument("--device", type=int, default=None,
                    help="decode only this USB device address. Note that one "
                         "dongle can appear under two addresses (see the "
                         "module docstring); default is all of them")
    ap.add_argument("--phase", type=int, nargs=2, metavar=("A", "B"),
                    help="per-slot heard/missed of channel A against channel "
                         "B's offset, from the receive timestamps")
    ap.add_argument("--period", type=_period_arg, action="append", metavar="CH=N",
                    help="channel period in 32768 Hz counts, overriding "
                         "CHANNEL_PERIOD and inference (repeatable)")
    args = ap.parse_args(argv)
    periods = dict(args.period) if args.period else None

    header, lines = decode(args.pcap, raw=args.raw, only_device=args.device,
                           periods=periods)

    if args.phase:
        rows, head = phase(lines, args.phase[0], args.phase[1], periods=periods)
        body = [f"{t:10.3f}  {'HEARD' if h else 'miss '}  "
                f"b-minus-a {d:+8.2f} ms" for t, h, d in rows]
    elif args.summary:
        counts: Counter[str] = Counter()
        for line in lines:
            parts = line.split()
            if len(parts) >= 3:
                suffix = " (heuristic)" if line.endswith(HEURISTIC_MARK) else ""
                counts[f"{parts[1]} {parts[2]}{suffix}"] += 1
        head = header
        body = [f"{n:6d}  {k}" for k, n in counts.most_common()]
    else:
        head = header
        body = lines

    if args.out:
        sys.stdout.write("\n".join(head) + "\n" if head else "")
        args.out.write_text("\n".join(body) + "\n" if body else "", encoding="utf-8")
        print(f"wrote {args.out} ({len(body)} lines)")
    else:
        sys.stdout.write("\n".join(head + body) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
