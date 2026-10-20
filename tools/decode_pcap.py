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
skipped silently.

Do not read "devices seen" as a device count. USBPcap writes 0xFF (255) in the
address field of URBs it cannot map, so ONE physical dongle routinely appears
as two entries: its control transfers under the real address and its bulk
stream under 255. A 2026-08-18 Zwift capture taken with `--devices 7` reported
`{7: 74, 255: 22162}`, and every ANT byte in it was under 255. Use --device
only to separate genuinely different devices in an -All capture, and check the
first bytes with --raw before trusting an address: ANT frames start a4.

Usage:
    python tools/decode_pcap.py capture.pcap [-o decoded.txt] [--raw] [--summary]
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

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
            diagnostics['discarded_bytes'] += len(stream)
            stream.clear()
            return
        if start:
            diagnostics['discarded_bytes'] += start
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


def decode(path: Path, raw: bool = False, only_device: int | None = None):
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
    transfers: dict[tuple, Counter] = {}
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

    def consume(stream_key, rel, *, final=False):
        bus, device, endpoint, from_device = stream_key
        stream = streams[stream_key]
        direction = "dev->host" if from_device else "host->dev"
        for msg_id, body, ok, recovered in reframe(stream, diagnostics, final=final):
            if not ok:
                warnings.append(f"warning: {rel:.3f}s bus={bus} device={device} "
                                f"endpoint=0x{endpoint:02x}: bad checksum candidate")
                continue
            name = MESSAGE_NAMES.get(msg_id, f"MSG_{msg_id:02X}")
            if recovered:
                # Possibly bytes of a truncated real message: shown, never
                # counted.
                diagnostics['heuristic_messages'] += 1
                lines.append(f"{rel:10.3f}  {direction}  {name} {body.hex()}"
                             f"  {HEURISTIC_MARK}")
                continue
            lines.append(f"{rel:10.3f}  {direction}  {name} {body.hex()}")
            if not body:
                continue
            ch = body[0] & 0x1F
            key = (bus, device, ch)
            if from_device and msg_id == ant_wire.MESG_BROADCAST_DATA_ID:
                channel_counts[key] += 1
            if from_device and msg_id == ant_wire.MESG_RESPONSE_EVENT_ID and len(body) >= 3:
                ref, code = body[1:3]
                ledger = transfers.setdefault(key, Counter())
                if ref == ant_wire.MESG_EVENT_ID:
                    events[(*key, code)] += 1
                    if code in (ant_wire.EVENT_TRANSFER_TX_COMPLETED,
                                ant_wire.EVENT_TRANSFER_TX_FAILED):
                        outcome = ('completed' if code == ant_wire.EVENT_TRANSFER_TX_COMPLETED
                                   else 'failed')
                        if ledger['pending']:
                            ledger['pending'] -= 1
                            ledger[outcome] += 1
                        else:
                            ledger['unmatched_terminal_events'] += 1
                else:
                    responses[(*key, ref, code)] += 1
                    if ref == ant_wire.MESG_ACKNOWLEDGED_DATA_ID:
                        if code == 0:
                            ledger['accepted'] += 1
                            ledger['pending'] += 1
                        else:
                            ledger['refused'] += 1

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
    diagnostic_names = ('bad_checksum_candidates', 'discarded_bytes',
                        'incomplete_ant_streams', 'incomplete_ant_bytes',
                        'incomplete_candidates_resynchronized',
                        'heuristic_messages',
                        'truncated_pcap_records', 'snaplen_truncated_records',
                        'malformed_usb_records', 'truncated_usb_payloads',
                        'nonzero_usb_statuses')
    header.append('diagnostics: ' + ' '.join(f'{k}={diagnostics[k]}' for k in diagnostic_names))
    for (bus, device, ch), count in sorted(channel_counts.items()):
        header.append(f'bus={bus} device={device} ch={ch}: broadcasts={count}')
    for (bus, device, ch), ledger in sorted(transfers.items()):
        if ledger:
            header.append(f'bus={bus} device={device} ch={ch}: transfers ' + ' '.join(
                f'{k}={ledger[k]}' for k in ('accepted', 'completed', 'failed', 'pending',
                                           'refused', 'unmatched_terminal_events')))
    for (bus, device, ch, code), count in sorted(events.items()):
        header.append(f'bus={bus} device={device} ch={ch}: '
                      f'{event_names.get(code, f"EVENT_0x{code:02X}")}={count}')
    for (bus, device, ch, ref, code), count in sorted(responses.items()):
        header.append(f'bus={bus} device={device} ch={ch}: response '
                      f'{MESSAGE_NAMES.get(ref, f"0x{ref:02X}")} '
                      f'{response_names.get(code, f"0x{code:02X}")}={count}')
    header.extend(warnings)
    return header, lines


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pcap", type=Path, help="capture from scripts/cap_zwift.ps1")
    ap.add_argument("-o", "--out", type=Path, help="write here instead of stdout")
    ap.add_argument("--raw", action="store_true",
                    help="also emit the raw bulk payload of every URB")
    ap.add_argument("--summary", action="store_true",
                    help="print message-id counts per direction instead of the log")
    ap.add_argument("--device", type=int, default=None,
                    help="decode only this USB device address. Note that one "
                         "dongle can appear under two addresses (see the "
                         "module docstring); default is all of them")
    args = ap.parse_args(argv)

    header, lines = decode(args.pcap, raw=args.raw, only_device=args.device)

    if args.summary:
        counts: Counter[str] = Counter()
        for line in lines:
            parts = line.split()
            if len(parts) >= 3:
                suffix = " (heuristic)" if line.endswith(HEURISTIC_MARK) else ""
                counts[f"{parts[1]} {parts[2]}{suffix}"] += 1
        out = header + [f"{n:6d}  {k}" for k, n in counts.most_common()]
    else:
        out = header + lines

    text = "\n".join(out) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
        print(f"wrote {args.out} ({len(lines)} lines)")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
