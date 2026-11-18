#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Slave-side driver and analyser for the FE-C / power slot crossing.

Opens channels the way Zwift does (ANT+ key, slave type 0x00, pinned device
number and type with transmission type 0, search timeout 0xFF, LIB_CONFIG
0xE0) on HR ch1, power ch2 and FE-C ch3, logs every frame in each direction
as decode_pcap text lines, and reports what FE-C did at each crossing with
power (the stretch where the two channels' frames are within --band ms):

    # DUT: power + FE-C, one 0x33 (grade 0 %) pending at a time, 1 Hz
    python tools/ant_crossing.py --serial 4FD72803 --channels power,fec \\
        --log dut.log --stop-after 2
    # Control: FE-C only, no commands, nothing to schedule around
    python tools/ant_crossing.py --port COM11 --baud 57600 --channels fec \\
        --interval 0 --log ti.log
    # Afterwards, the controls in the DUT's crossing windows:
    python tools/ant_crossing.py --replay dut.log --control ti.log garmin.log

The log is decode_pcap's text format ("{t:10.3f}  dev->host  NAME <hex>"), so
decode_pcap.phase() and slot_timeline() run on it directly, and --replay also
accepts a decode_pcap -o file of a Zwift capture. Lines starting with '#' are
this tool's: the start epoch (for aligning logs from separate processes by
host time) and 0xF7 page snapshots.

Every --diag-every seconds the DUT's 0xF7 pages 0, 3, 7, 8 and 11 are read
in-process (ant_diag.read_pages); a build without diag (or a Garmin) is
detected on the first read and skipped from then on.

Offsets are power minus FE-C, as in decode_pcap --phase 3 2: negative means
power's frame came first.
"""
from __future__ import annotations

import argparse
import bisect
import json
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import contextlib

import ant_wire as w  # noqa: E402
import decode_pcap as dp  # noqa: E402

KEY = bytes.fromhex('b9a521fbbd72c345')     # ANT+ network key
ROLES = {                                   # name -> (channel, type, period)
    'hr': (1, 0x78, 8070),
    'power': (2, 0x0B, 8182),
    'fec': (3, 0x11, 8192),
}
DIAG_PAGES = [0, 3, 7, 8, 11]
# The counters the verdict table reads (plan step 3), by page.
FIELDS = {
    '0': ['slots_missed', 'slots_denied', 'sched_missed', 'sched_denied'],
    '3': ['rx_posted', 'rx_refused', 'term_ok', 'term_empty', 'term_error'],
    '7': ['trk_missed_behind_scan', 'rx_start_missed', 'tx_start_missed'],
    '8': ['pass_over_100us', 'drain_over_250us'],
    '11': ['slots_collided', 'turn_yields', 'arm_rejected', 'preempted',
           'windows_merged', 'replans'],
}
SERIAL_READ_TIMEOUT_MS = 50
PENDING_LIMIT_S = 2.5
GAP_S = 3.0             # in-band rows further apart than this split crossings
MARGIN_S = 0.5          # control window padding around a DUT crossing


# ---------------------------------------------------------------------------
# Analysis (no device; also what --replay runs)
# ---------------------------------------------------------------------------

def parse(lines):
    """(epoch, diag snapshots [(t, pages)], events [(t, dir, msg, body)]).

    epoch is None without an '# ant_crossing epoch=' header (a decode_pcap
    -o export): its times have no host-clock origin to align by. Lines
    decode_pcap marks as recovered at EOF are possibly bytes of a truncated
    message, so they are skipped, as decode_pcap's own parsers do."""
    epoch = None
    snaps, events = [], []
    for line in lines:
        if line.startswith('# ant_crossing'):
            for tok in line.split():
                if tok.startswith('epoch='):
                    epoch = float(tok[6:])
            continue
        if line.startswith('# diag '):
            _, _, t, payload = line.split(' ', 3)
            snaps.append((float(t), json.loads(payload)))
            continue
        parts = line.split()
        if (len(parts) < 4 or line.startswith('#')
                or line.rstrip().endswith(dp.HEURISTIC_MARK)):
            continue
        msg = dp.MESSAGE_IDS.get(parts[2])
        if msg is None and parts[2].startswith('MSG_'):
            try:
                msg = int(parts[2][4:], 16)
            except ValueError:
                continue
        if msg is None:
            continue
        try:
            events.append((float(parts[0]), parts[1], msg, bytes.fromhex(parts[3])))
        except ValueError:
            continue
    return epoch, snaps, events


def crossings(rows, band_ms: float, gap_s: float = GAP_S):
    """Group phase() rows into crossings: runs of in-band rows (|offset| <=
    band_ms) with no two consecutive in-band rows more than gap_s apart.
    Returns [(first index, last index)] into rows, last in-band row included."""
    out = []
    start = last = None
    for i, (t, _h, d) in enumerate(rows):
        if abs(d) > band_ms:
            continue
        if last is not None and t - rows[last][0] > gap_s:
            out.append((start, last))
            start = None
        if start is None:
            start = i
        last = i
    if start is not None:
        out.append((start, last))
    return out


def snapshot_delta(snaps, t0, t1):
    """(before t, after t, {page: {field: delta}}) over [t0, t1], or None."""
    times = [t for t, _ in snaps]
    i = bisect.bisect_right(times, t0) - 1
    j = bisect.bisect_left(times, t1)
    if i < 0 or j >= len(snaps):
        return None
    return snaps[i][0], snaps[j][0], _delta(snaps[i][1], snaps[j][1])


def _delta(before, after):
    out = {}
    for page, fields in FIELDS.items():
        if page not in after or page not in before:
            continue
        a, b = after[page]['fields'], before[page]['fields']
        out[page] = {f: (a[f] - b[f]) % (1 << 32) for f in fields
                     if isinstance(a.get(f), int) and isinstance(b.get(f), int)}
    return out


def baseline_rate(snaps, windows):
    """Per-second counter rates over snapshot intervals that overlap no
    crossing window: {page: {field: rate}}."""
    totals: dict = {}
    seconds = 0.0
    for (t0, p0), (t1, p1) in zip(snaps, snaps[1:], strict=False):
        if any(t0 <= b and a <= t1 for a, b in windows):
            continue
        seconds += t1 - t0
        for page, fields in _delta(p0, p1).items():
            for f, v in fields.items():
                totals.setdefault(page, Counter())[f] += v
    if seconds <= 0:
        return None, 0.0
    return {p: {f: v / seconds for f, v in c.items()} for p, c in totals.items()}, seconds


def channel_events(events, ch, t0, t1):
    """Counts of channel events and FE-C command outcomes in [t0, t1]."""
    c = Counter()
    for t, direction, msg, body in events:
        if not t0 <= t <= t1 or not body or body[0] & 0x1F != ch:
            continue
        if (direction == 'dev->host' and msg == w.MESG_RESPONSE_EVENT_ID
                and len(body) >= 3 and body[1] == w.MESG_EVENT_ID):
            code = body[2]
            if code == w.EVENT_RX_FAIL_GO_TO_SEARCH:
                c['go_to_search'] += 1
            elif code == w.EVENT_TRANSFER_TX_COMPLETED:
                c['cmd_completed'] += 1
            elif code == w.EVENT_TRANSFER_TX_FAILED:
                c['cmd_failed'] += 1
            elif code == w.EVENT_CHANNEL_COLLISION:
                c['collision'] += 1
    return c


def control_window(rows, offset, t0, t1):
    """(slots, missed, longest run) of a control's FE-C rows in DUT time
    [t0, t1]; offset = control epoch - DUT epoch (both known: see
    analyse())."""
    inside = [r for r in rows if t0 <= r[0] + offset <= t1]
    best, _ = dp.longest_run(inside)
    return len(inside), sum(not r[1] for r in inside), best


def analyse(lines, *, fec_ch=3, power_ch=2, band_ms=3.0, controls=(),
            out=sys.stdout):
    """Print the per-crossing report. controls: [(name, lines)]. Returns
    the phase rows (empty when the log has no power channel)."""
    epoch, snaps, events = parse(lines)
    rows, head = dp.phase(lines, fec_ch, power_ch)
    print('\n'.join(head[:2]), file=out)
    if not rows:
        return rows
    found = crossings(rows, band_ms)
    windows = [(rows[a][0], rows[b][0]) for a, b in found]
    far = [r for r in rows if abs(r[2]) > 10.0]
    print(f'background (|offset| > 10 ms): {sum(not r[1] for r in far)} / '
          f'{len(far)} missed ({100.0 * sum(not r[1] for r in far) / max(1, len(far)):.1f} %)',
          file=out)
    rates, rate_s = baseline_rate(snaps, windows)
    ctl = []
    for name, clines in controls:
        cepoch, _s, _e = parse(clines)
        crows, per = dp.slot_timeline(clines, fec_ch)
        # Without an epoch on both sides there is no common clock: a
        # substituted 0 would put the control decades away and report its
        # windows as empty rather than unaligned.
        missing = [side for side, e in (('DUT', epoch), ('control', cepoch))
                   if e is None]
        offset = None if missing else cepoch - epoch
        ctl.append((name, crows, offset))
        where = (f'offset to DUT {offset:+.3f} s' if offset is not None else
                 f'NOT ALIGNED (no epoch header in the {" or ".join(missing)} log)')
        print(f'control {name}: {len(crows)} FE-C slots, '
              f'{sum(not r[1] for r in crows)} missed, period {per.text}, '
              f'{where}', file=out)
    print(f'{len(found)} crossing(s) at |offset| <= {band_ms} ms', file=out)
    for n, (a, b) in enumerate(found, 1):
        t0, t1 = rows[a][0], rows[b][0]
        span = rows[a:b + 1]
        band = [r for r in span if abs(r[2]) <= band_ms]
        best, end = dp.longest_run(span)
        print(f'\ncrossing {n}: {t0:.3f}-{t1:.3f} s, offset {rows[a][2]:+.2f} .. '
              f'{rows[b][2]:+.2f} ms, FE-C {sum(not r[1] for r in band)} / '
              f'{len(band)} missed in band, longest run {best}', file=out)
        if end is not None and best:
            s = span[end - best + 1]
            print(f'  run {s[0]:.3f}-{span[end][0]:.3f} s, offset {s[2]:+.2f} .. '
                  f'{span[end][2]:+.2f} ms', file=out)
        print('  ' + ' '.join(f'{"x" if not h else "."}{d:+.2f}' for _t, h, d in span),
              file=out)
        ev = channel_events(events, fec_ch, t0 - 1.0, t1 + 3.0)
        if ev:
            print('  FE-C events (t0-1 .. t1+3 s): '
                  + ' '.join(f'{k}={v}' for k, v in sorted(ev.items())), file=out)
        for name, crows, offset in ctl:
            if offset is None:
                print(f'  control {name}: not aligned', file=out)
                continue
            slots, missed, run = control_window(crows, offset, t0 - MARGIN_S, t1 + MARGIN_S)
            print(f'  control {name}: {missed} / {slots} missed, longest run {run}',
                  file=out)
        delta = snapshot_delta(snaps, t0, t1)
        if delta is None:
            if snaps:
                print('  pages: no snapshot on both sides', file=out)
            continue
        before, after, pages = delta
        print(f'  pages {before:.1f} -> {after:.1f} s ({after - before:.1f} s; '
              f'baseline over {rate_s:.0f} s scaled to it in brackets):', file=out)
        for page, fields in pages.items():
            parts = []
            for f, v in fields.items():
                base = rates.get(page, {}).get(f) if rates else None
                parts.append(f'{f}={v}' + (f' [{base * (after - before):.1f}]'
                                           if base is not None else ''))
            print(f'    page {page}: ' + ' '.join(parts), file=out)
    return rows


# ---------------------------------------------------------------------------
# Live driver
# ---------------------------------------------------------------------------

class Log:
    def __init__(self, path: Path | None):
        self.lines: list[str] = []
        self.fh = path.open('w', encoding='utf-8', newline='\n') if path else None
        self.t0 = time.perf_counter()
        self.last_flush = 0.0

    def now(self) -> float:
        return time.perf_counter() - self.t0

    def add(self, line: str) -> None:
        self.lines.append(line)
        if self.fh:
            self.fh.write(line + '\n')
            if self.now() - self.last_flush > 1.0:
                self.fh.flush()
                self.last_flush = self.now()

    def message(self, direction: str, msg: int, body: bytes) -> None:
        name = dp.MESSAGE_NAMES.get(msg, f'MSG_{msg:02X}')
        self.add(f'{self.now():10.3f}  {direction}  {name} {bytes(body).hex()}')

    def close(self) -> None:
        if self.fh:
            self.fh.close()


class LoggedDevice:
    """Passes reads through and logs every frame written."""

    def __init__(self, dev, log: Log):
        self.dev, self.log = dev, log

    def write(self, ep, data, *args, **kwargs):
        data = bytes(data)
        if len(data) >= 4:
            self.log.message('host->dev', data[2], data[3:3 + data[1]])
        return self.dev.write(ep, data, *args, **kwargs)

    def read(self, *args, **kwargs):
        return self.dev.read(*args, **kwargs)


def make_reader(dev, log: Log, timeout_ms: int):
    from ant_probe import FrameReader

    class LoggedReader(FrameReader):
        def next_frame(self, deadline):
            got = super().next_frame(deadline)
            if got is not None:
                log.message('dev->host', got[0], got[1])
            return got

    return LoggedReader(dev, timeout_ms=timeout_ms)


def command(dev, reader, msg, body):
    from ant_probe import EP_OUT, frame
    dev.write(EP_OUT, frame(msg, body))
    end = time.monotonic() + 3
    while time.monotonic() < end:
        got = reader.next_frame(end)
        if (got and got[0] == w.MESG_RESPONSE_EVENT_ID and len(got[1]) >= 3
                and got[1][1] == msg):
            if got[1][2]:
                raise RuntimeError(f'command 0x{msg:02x} refused: {got[1].hex()}')
            return
    raise RuntimeError(f'command 0x{msg:02x} had no response')


def grade_page(percent: float) -> bytes:
    """Page 0x33 track resistance: grade in 0.01 % from -200 %, CRR unset."""
    raw = round((percent + 200.0) * 100)
    return bytes([0x33, 0xFF, 0xFF, 0xFF, 0xFF]) + raw.to_bytes(2, 'little') + b'\xff'


def snapshot(dev, reader, log: Log) -> bool:
    import ant_diag
    pages = ant_diag.read_pages(dev, reader, DIAG_PAGES, err=sys.stdout)
    log.add(f'# diag {log.now():.3f} {json.dumps(pages, separators=(",", ":"))}')
    return True


def run(args) -> int:
    from ant_probe import (
        EP_OUT,
        READ_TIMEOUT_MS,
        SerialDevice,
        close_device,
        frame,
        open_device,
        reset_stack,
    )

    roles = [r.strip() for r in args.channels.split(',') if r.strip()]
    for r in roles:
        if r not in ROLES:
            raise SystemExit(f'unknown channel role {r!r}; pick from {", ".join(ROLES)}')
    devnum = {'hr': args.hr_dev, 'power': args.power_dev, 'fec': args.fec_dev}
    log = Log(args.log)
    log.add(f'# ant_crossing epoch={time.time():.6f} channels={",".join(roles)} '
            f'target={args.port or args.serial} label={args.label or "-"}')
    raw = open_device(True, port=args.port, serial=args.serial, baud=args.baud)
    dev = LoggedDevice(raw, log)
    reader = make_reader(dev, log, SERIAL_READ_TIMEOUT_MS
                         if isinstance(raw, SerialDevice) else READ_TIMEOUT_MS)
    fec_ch = ROLES['fec'][0]
    sent = completed = failed = dropped = 0
    diag = False
    try:
        if not reset_stack(dev, reader):
            raise RuntimeError('no STARTUP after SYSTEM_RESET')
        command(dev, reader, w.MESG_NETWORK_KEY_ID, bytes([0]) + KEY)
        command(dev, reader, w.MESG_ANTLIB_CONFIG_ID, bytes([0, w.LIB_CONFIG_ALL_EXT_FIELDS]))
        for r in sorted(roles, key=lambda r: ROLES[r][0]):
            ch, dtype, period = ROLES[r]
            n = devnum[r]
            command(dev, reader, w.MESG_ASSIGN_CHANNEL_ID, bytes([ch, 0x00, 0]))
            command(dev, reader, w.MESG_CHANNEL_ID_ID, bytes([ch, n & 255, n >> 8, dtype, 0]))
            command(dev, reader, w.MESG_CHANNEL_MESG_PERIOD_ID,
                    bytes([ch, period & 255, period >> 8]))
            command(dev, reader, w.MESG_CHANNEL_RADIO_FREQ_ID, bytes([ch, 57]))
            command(dev, reader, w.MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([ch, 255]))
            command(dev, reader, w.MESG_OPEN_CHANNEL_ID, bytes([ch]))
        diag = not args.no_diag
        if diag:
            try:
                snapshot(dev, reader, log)
            except RuntimeError as exc:
                print(f'diag off: {exc}', flush=True)
                log.add(f'# diag unavailable: {exc}')
                diag = False
        commands = args.interval > 0 and 'fec' in roles
        start = time.monotonic()
        next_send = start + 2.0
        next_tick = start + args.diag_every
        pending_since = None
        reported = 0
        print('running; Ctrl-C to stop', flush=True)
        while args.seconds <= 0 or time.monotonic() < start + args.seconds:
            now = time.monotonic()
            if pending_since is not None and now - pending_since > PENDING_LIMIT_S:
                dropped += 1
                pending_since = None
                log.add(f'# pending command had no terminal event after {PENDING_LIMIT_S} s')
            if commands and pending_since is None and now >= next_send:
                dev.write(EP_OUT, frame(w.MESG_ACKNOWLEDGED_DATA_ID,
                                        bytes([fec_ch]) + grade_page(args.grade)))
                sent += 1
                pending_since = now
                next_send = max(next_send + args.interval, now)
            got = reader.next_frame(time.monotonic() + 0.05)
            if (got and got[0] == w.MESG_RESPONSE_EVENT_ID and len(got[1]) >= 3
                    and got[1][0] & 0x1F == fec_ch and got[1][1] == w.MESG_EVENT_ID
                    and got[1][2] in (w.EVENT_TRANSFER_TX_COMPLETED,
                                      w.EVENT_TRANSFER_TX_FAILED)):
                if got[1][2] == w.EVENT_TRANSFER_TX_COMPLETED:
                    completed += 1
                else:
                    failed += 1
                pending_since = None
            if time.monotonic() >= next_tick:
                next_tick += args.diag_every
                if diag:
                    try:
                        snapshot(dev, reader, log)
                    except RuntimeError as exc:
                        log.add(f'# diag snapshot failed: {exc}')
                if 'power' in roles and 'fec' in roles:
                    rows, _ = dp.phase(log.lines, fec_ch, ROLES['power'][0])
                    found = crossings(rows, args.band) if rows else []
                    # A crossing is closed once the offset has left the band
                    # for longer than the grouping gap.
                    closed = [c for c in found if rows[-1][0] - rows[c[1]][0] > GAP_S]
                    if len(closed) > reported:
                        reported = len(closed)
                        a, b = closed[-1]
                        span = rows[a:b + 1]
                        print(f'{log.now():8.1f} s: crossing {reported} closed, FE-C '
                              f'{sum(not r[1] for r in span)} / {len(span)} missed',
                              flush=True)
                    if args.stop_after and reported >= args.stop_after:
                        break
                print(f'{log.now():8.1f} s: commands sent={sent} completed={completed} '
                      f'failed={failed} no_terminal={dropped}', flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        if diag:
            with contextlib.suppress(Exception):
                snapshot(dev, reader, log)
        with contextlib.suppress(Exception):
            raw.write(EP_OUT, frame(w.MESG_SYSTEM_RESET_ID, b'\x00'))
        close_device(raw)
        log.add(f'# end commands sent={sent} completed={completed} failed={failed} '
                f'no_terminal={dropped}')
        log.close()
    print(f'commands sent={sent} completed={completed} failed={failed} '
          f'no_terminal={dropped}')
    if 'power' in roles and 'fec' in roles:
        analyse(log.lines, band_ms=args.band)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--replay', type=Path, help='analyse this log; no device')
    ap.add_argument('--control', type=Path, nargs='+', default=[],
                    help='with --replay: FE-C-only logs to align by host time')
    ap.add_argument('--rows', type=Path,
                    help='with --replay: write the phase rows in decode_pcap '
                         '--phase format')
    ap.add_argument('--serial', help='a USB dongle whose serial ends with this')
    ap.add_argument('--port', help='a UART build on this port (e.g. COM11)')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--channels', default='power,fec',
                    help='roles to open: any of hr, power, fec (default power,fec)')
    ap.add_argument('--power-dev', type=int, default=20329)
    ap.add_argument('--fec-dev', type=int, default=52233)
    ap.add_argument('--hr-dev', type=int, default=8070)
    ap.add_argument('--interval', type=float, default=1.0,
                    help='seconds between 0x33 commands, one pending; 0 = none')
    ap.add_argument('--grade', type=float, default=0.0,
                    help='grade %% sent in every 0x33 (constant: a real trainer obeys it)')
    ap.add_argument('--seconds', type=float, default=0, help='0 = until Ctrl-C')
    ap.add_argument('--stop-after', type=int, default=0,
                    help='stop after this many closed crossings (needs power,fec)')
    ap.add_argument('--diag-every', type=float, default=10.0)
    ap.add_argument('--no-diag', action='store_true')
    ap.add_argument('--band', type=float, default=3.0,
                    help='crossing half-width in ms (default 3)')
    ap.add_argument('--log', type=Path, help='write the text log here')
    ap.add_argument('--label', help='free text recorded in the log header')
    args = ap.parse_args(argv)

    if args.replay:
        lines = args.replay.read_text(encoding='utf-8').splitlines()
        controls = [(p.stem, p.read_text(encoding='utf-8').splitlines())
                    for p in args.control]
        rows = analyse(lines, band_ms=args.band, controls=controls)
        if args.rows:
            args.rows.write_text(''.join(
                f"{t:10.3f}  {'HEARD' if h else 'miss '}  b-minus-a {d:+8.2f} ms\n"
                for t, h, d in rows), encoding='utf-8')
        return 0
    if not (args.serial or args.port):
        ap.error('give --serial or --port (or --replay)')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
