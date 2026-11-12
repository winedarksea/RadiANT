#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""How fast does a background scan report the devices in the room?

    python tools/ant_scan_rate.py --serial EE6986854FD72803 --serial 006 \
        --seconds 300 --json scan.json
    python tools/ant_scan_rate.py --serial EE6986854FD72803 --serial 006 \
        --seconds 300 --track 20329:0x0B --track 1458:0x78 --reopen 4.5
    python tools/ant_scan_rate.py --port COM11 --baud 57600 --serial 006         --seconds 300 --reopen 4.5 --track 52233:0x11

The instrument for discovery speed (the 2026-11-11 Zwift captures: RadiANT's
scan reported about 0.3 sightings/s across five sensors, a Garmin stick
2.6-4.9/s). It opens the same never-locking background scan Zwift opens
(MESG_ASSIGN_CHANNEL with EXT_PARAM_ALWAYS_SEARCH, as tools/ant_rf6_capture.py
does) and reports, per device heard:

- sightings/s over the window;
- time to first sighting after each open of the scan channel, which is what a
  pairing screen waits on. With --reopen the scan is closed, unassigned,
  reassigned and reopened every S seconds, as Zwift does every ~4.5 s, so
  there are many opens to take a median over. Without it there is one;
- and, for each --track, the tracked channel's loss (RX_FAIL against
  broadcasts received), because faster discovery bought with tracked packets
  is not a win.

SEVERAL DONGLES AT ONCE. Give --serial more than once and each dongle runs in
its own process (the tools/ant_compare.py pattern: one USB handle per
process), all starting their capture at the same wall-clock instant. The room
at this bench drifts too much for a sequential A/B (memory: ant-bench-loss-
floor), so a same-window reference such as the Garmin stick is what makes a
rate mean anything. Results are merged into one report and one --json.

A UART build (--port, with --baud, default 115200) joins that mode as one more
child beside the --serial ones: --port COM11 --baud 57600 --serial 006 runs a
board on COM11 and a USB stick in the same window, reported side by side. The
port child is labelled by its port name. --baud applies to the port child only.
Each child's JSON records `started_at` (its wall-clock capture start) and the
merged JSON records the shared `start_at`, so a shared window can be checked.

What this does NOT read: radiant_search_stats. They are on 0xF7 diagnostic
pages 5-6 (ADR 0023): read them with tools/ant_diag.py --pages 5-6 AFTER the
window only, never as a before/after diff, because radiant_search_init()
zeroes them on every stack reset and key set (which this tool's --reopen
causes). The host-side numbers here are the gates.

Exit status: 0 when every dongle completed its window; 1 on a setup failure.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ant_wire import (  # noqa: E402
    CHANNEL_TYPE_SLAVE,
    EVENT_RX_FAIL,
    EXT_PARAM_ALWAYS_SEARCH,
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
    MESG_REQUEST_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_UNASSIGN_CHANNEL_ID,
    MESG_VERSION_ID,
    EVENT_CHANNEL_CLOSED,
)

SCAN_CHANNEL = 0

# Channel periods by device type, in 1/32768 s. The ANT+ profile periods for
# the sensors this bench has; anything else must be given explicitly. Not
# imported from ant_pages because a tracked channel's period is a channel
# setting, and the only ones needed here are these.
DEFAULT_PERIODS = {
    0x0B: 8182,   # bicycle power
    0x11: 8192,   # fitness equipment
    0x78: 8070,   # heart rate
    0x79: 8086,   # speed and cadence
    0x7A: 8102,   # cadence
    0x7B: 8118,   # speed
}

# The scan channel's own period. A background scan hears every profile
# whatever this is; it is required before the channel will open, and 8070 is
# what tools/ant_rf6_capture.py and Zwift use.
SCAN_PERIOD = 8070


# ---------------------------------------------------------------------------
# Pure parts: argument parsing and the arithmetic. Unit-tested.
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Track:
    device_number: int
    device_type: int
    period: int

    @property
    def key(self) -> str:
        return f"{self.device_number}:0x{self.device_type:02X}"


def parse_track(text: str) -> Track:
    """`devnum:type[:period]`, each decimal or 0x-hex."""
    parts = text.split(":")
    if len(parts) not in (2, 3):
        raise argparse.ArgumentTypeError(
            f"{text!r}: expected devnum:type or devnum:type:period")
    try:
        values = [int(p, 0) for p in parts]
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r}: not numbers") from None
    number, dtype = values[0], values[1]
    if not 1 <= number <= 0xFFFF:
        raise argparse.ArgumentTypeError(
            f"{text!r}: a tracked channel needs a real device number "
            "(1..65535), not the wildcard")
    if not 0 < dtype <= 0x7F:
        raise argparse.ArgumentTypeError(f"{text!r}: device type 1..127")
    if len(values) == 3:
        period = values[2]
    elif dtype in DEFAULT_PERIODS:
        period = DEFAULT_PERIODS[dtype]
    else:
        raise argparse.ArgumentTypeError(
            f"{text!r}: no default period for type 0x{dtype:02X}; give it "
            "as devnum:type:period")
    if not 0 < period <= 0xFFFF:
        raise argparse.ArgumentTypeError(f"{text!r}: period out of range")
    return Track(number, dtype, period)


@dataclass
class ScanLog:
    """Everything one dongle saw, as times relative to the capture start.

    `opens` are the instants the scan channel's OPEN was acknowledged and
    `closes` the instants its CLOSE was sent (the last open runs to `end`).
    `sightings` maps "devnum:0xTT" to the times that device was reported on
    the scan channel.
    """

    end: float = 0.0
    opens: list[float] = field(default_factory=list)
    closes: list[float] = field(default_factory=list)
    sightings: dict[str, list[float]] = field(default_factory=dict)
    tracked: dict[str, dict] = field(default_factory=dict)
    events: dict[str, int] = field(default_factory=dict)

    def windows(self) -> list[tuple[float, float]]:
        """(open, close) pairs. A close with no open after it is dropped."""
        out = []
        closes = sorted(self.closes)
        for start in sorted(self.opens):
            stop = next((c for c in closes if c > start), self.end)
            out.append((start, stop))
        return out

    def listening_seconds(self) -> float:
        return sum(stop - start for start, stop in self.windows())


def first_after_open(times: list[float],
                     windows: list[tuple[float, float]]) -> list[float | None]:
    """Per scan window, seconds from open to the device's first report in it.

    None for a window in which it was never reported - a miss, which is
    kept rather than dropped, because dropping misses is how a median turns
    "heard half the time" into "heard fast".
    """
    out: list[float | None] = []
    ordered = sorted(times)
    for start, stop in windows:
        hit = next((t for t in ordered if start <= t < stop), None)
        out.append(None if hit is None else hit - start)
    return out


def median_with_misses(values: list[float | None]) -> float | None:
    """The median, counting a miss as longer than any hit.

    So a device missed in more than half the windows has no median (None)
    rather than the median of the windows it happened to be heard in.
    """
    if not values:
        return None
    ordered = sorted(values, key=lambda v: (v is None, v or 0.0))
    mid = ordered[(len(ordered) - 1) // 2]
    if len(ordered) % 2 == 0:
        upper = ordered[len(ordered) // 2]
        if mid is None or upper is None:
            return None
        return (mid + upper) / 2
    return mid


def loss(received: int, failed: int) -> float | None:
    total = received + failed
    return None if total == 0 else 100.0 * failed / total


def summarise(log: ScanLog) -> dict:
    """The per-dongle result, ready for JSON."""
    windows = log.windows()
    listening = log.listening_seconds()
    devices = {}
    for key, times in sorted(log.sightings.items()):
        firsts = first_after_open(times, windows)
        hits = [v for v in firsts if v is not None]
        devices[key] = {
            "sightings": len(times),
            "per_s": (len(times) / listening) if listening > 0 else None,
            "opens": len(firsts),
            "opens_heard": len(hits),
            "first_after_open_median_s": median_with_misses(firsts),
            "first_after_open_max_s": max(hits) if hits else None,
        }
    tracked = {}
    for key, rec in sorted(log.tracked.items()):
        tracked[key] = dict(rec, loss_pct=loss(rec["received"], rec["rx_fail"]))
    total = sum(len(t) for t in log.sightings.values())
    return {
        "seconds": log.end,
        "listening_s": listening,
        "opens": len(windows),
        "sightings_total": total,
        "sightings_per_s": (total / listening) if listening > 0 else None,
        "devices": devices,
        "tracked": tracked,
        "events": dict(sorted(log.events.items())),
    }


def render(results: dict[str, dict]) -> str:
    """One table per dongle, then a side-by-side per device."""
    lines = []
    for name, res in results.items():
        if "error" in res:
            lines.append(f"{name}: FAILED - {res['error']}")
            continue
        s = res["summary"]
        rate = s["sightings_per_s"]
        lines.append(
            f"{name} ({res.get('version', '?')}): {s['sightings_total']} "
            f"sightings in {s['listening_s']:.1f} s listening, "
            f"{rate:.2f}/s, {s['opens']} open(s)" if rate is not None else
            f"{name}: no listening time")
        lines.append(f"  {'device':>14} {'per s':>7} {'heard/opens':>12} "
                     f"{'median 1st':>11} {'max 1st':>8}")
        for key, d in s["devices"].items():
            med = d["first_after_open_median_s"]
            mx = d["first_after_open_max_s"]
            lines.append(
                f"  {key:>14} {d['per_s']:>7.2f} "
                f"{d['opens_heard']:>5}/{d['opens']:<6} "
                f"{'-' if med is None else f'{med:.2f} s':>11} "
                f"{'-' if mx is None else f'{mx:.2f} s':>8}")
        for key, t in s["tracked"].items():
            lp = t["loss_pct"]
            lines.append(
                f"  tracked {key}: {t['received']} received, {t['rx_fail']} "
                f"RX_FAIL, loss {'-' if lp is None else f'{lp:.2f} %'}")
        if s["events"]:
            lines.append("  events: " + ", ".join(
                f"{k} x{v}" for k, v in s["events"].items()))
        lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Talking to one dongle
# ---------------------------------------------------------------------------

def _command(dev, reader, msg_id: int, payload: bytes,
             timeout: float = 2.0) -> int | None:
    """Send a command, return its response code (None on no answer)."""
    from ant_probe import EP_OUT, frame
    dev.write(EP_OUT, frame(msg_id, payload))
    deadline = time.monotonic() + timeout
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return None
        got_id, body = result
        if got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 \
                and body[1] == msg_id:
            return body[2]


def _must(dev, reader, msg_id: int, payload: bytes, what: str) -> None:
    code = _command(dev, reader, msg_id, payload)
    if code != 0:
        raise RuntimeError(f"{what} -> "
                           f"{'no answer' if code is None else f'code {code}'}")


# The two slave types a scan channel can be assigned as. 0x40 is what Zwift
# sends; 0x00 is what this tool always sent before --scan-type existed.
SCAN_TYPES = (CHANNEL_TYPE_SLAVE, 0x40)


def _open_scan(dev, reader, scan_type: int = CHANNEL_TYPE_SLAVE) -> None:
    ch = SCAN_CHANNEL
    _must(dev, reader, MESG_ASSIGN_CHANNEL_ID,
          bytes([ch, scan_type, 0x00, EXT_PARAM_ALWAYS_SEARCH]),
          "assign background scan")
    _must(dev, reader, MESG_CHANNEL_ID_ID, bytes([ch, 0, 0, 0, 0]),
          "wildcard channel id")
    from ant_scan import ANT_PLUS_FREQ
    _must(dev, reader, MESG_CHANNEL_RADIO_FREQ_ID, bytes([ch, ANT_PLUS_FREQ]),
          "scan frequency")
    _must(dev, reader, MESG_CHANNEL_MESG_PERIOD_ID,
          bytes([ch]) + SCAN_PERIOD.to_bytes(2, "little"), "scan period")
    _must(dev, reader, MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([ch, 0xFF]),
          "scan search timeout")
    _must(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([ch]), "open scan")


def _close_scan(dev, reader, timeout: float = 2.0) -> None:
    """Close and unassign, waiting for EVENT_CHANNEL_CLOSED in between."""
    from ant_probe import EP_OUT, frame
    _must(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([SCAN_CHANNEL]),
          "close scan")
    deadline = time.monotonic() + timeout
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            break
        got_id, body = result
        if got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 \
                and body[0] == SCAN_CHANNEL and body[1] == MESG_EVENT_ID \
                and body[2] == EVENT_CHANNEL_CLOSED:
            break
    dev.write(EP_OUT, frame(MESG_UNASSIGN_CHANNEL_ID, bytes([SCAN_CHANNEL])))


def _open_track(dev, reader, ch: int, track: Track) -> None:
    from ant_scan import ANT_PLUS_FREQ
    _must(dev, reader, MESG_ASSIGN_CHANNEL_ID,
          bytes([ch, CHANNEL_TYPE_SLAVE, 0x00]), f"assign track ch{ch}")
    _must(dev, reader, MESG_CHANNEL_ID_ID,
          bytes([ch]) + track.device_number.to_bytes(2, "little")
          + bytes([track.device_type, 0]), f"channel id ch{ch}")
    _must(dev, reader, MESG_CHANNEL_RADIO_FREQ_ID, bytes([ch, ANT_PLUS_FREQ]),
          f"frequency ch{ch}")
    _must(dev, reader, MESG_CHANNEL_MESG_PERIOD_ID,
          bytes([ch]) + track.period.to_bytes(2, "little"), f"period ch{ch}")
    # Pinned and never giving up, which is the Zwift channel model.
    _must(dev, reader, MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([ch, 0xFF]),
          f"search timeout ch{ch}")
    _must(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([ch]), f"open ch{ch}")


def _version(dev, reader) -> str:
    from ant_probe import EP_OUT, expect, frame
    dev.write(EP_OUT, frame(MESG_REQUEST_ID, bytes([0, MESG_VERSION_ID])))
    payload = expect(reader, MESG_VERSION_ID, 2.0)
    if payload is None:
        return "?"
    return payload.split(b"\x00")[0].decode("ascii", errors="replace")


def run_one(serial: str | None, port: str | None, seconds: float,
            tracks: list[Track], reopen: float | None,
            start_at: float | None, baud: int = 115200,
            scan_type: int = CHANNEL_TYPE_SLAVE) -> dict:
    """Run the whole window on one dongle and return its result dict."""
    from ant_probe import FrameReader, close_device, open_device, reset_stack
    from ant_scan import ANT_PLUS_KEY
    from ant_verify import extended_fields

    dev = open_device(False, serial=serial, port=port, baud=baud)
    reader = FrameReader(dev)
    log = ScanLog()
    try:
        if not reset_stack(dev, reader):
            raise RuntimeError("no startup message after reset")
        version = _version(dev, reader)
        _must(dev, reader, MESG_ANTLIB_CONFIG_ID,
              bytes([0, LIB_CONFIG_ALL_EXT_FIELDS]), "extended messages")
        _must(dev, reader, MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY,
              "network key")
        track_by_ch = {}
        for i, track in enumerate(tracks):
            ch = SCAN_CHANNEL + 1 + i
            _open_track(dev, reader, ch, track)
            track_by_ch[ch] = track
            log.tracked[track.key] = {"channel": ch, "received": 0,
                                      "rx_fail": 0}

        if start_at is not None:
            while time.time() < start_at:
                reader.next_frame(min(time.monotonic() + 0.05,
                                      time.monotonic() + start_at - time.time()))

        t0 = time.monotonic()
        started_at = time.time()
        _open_scan(dev, reader, scan_type)
        log.opens.append(time.monotonic() - t0)
        next_reopen = None if reopen is None else t0 + reopen
        deadline = t0 + seconds

        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            if next_reopen is not None and now >= next_reopen:
                log.closes.append(now - t0)
                _close_scan(dev, reader)
                _open_scan(dev, reader, scan_type)
                log.opens.append(time.monotonic() - t0)
                next_reopen = time.monotonic() + reopen
            wait_until = min(deadline, next_reopen or deadline, now + 0.5)
            result = reader.next_frame(wait_until)
            if result is None:
                continue
            msg_id, body = result
            t = time.monotonic() - t0
            if msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 \
                    and body[1] == MESG_EVENT_ID:
                ch, code = body[0], body[2]
                if ch in track_by_ch and code == EVENT_RX_FAIL:
                    log.tracked[track_by_ch[ch].key]["rx_fail"] += 1
                else:
                    name = f"ch{ch}:0x{code:02X}"
                    log.events[name] = log.events.get(name, 0) + 1
                continue
            if msg_id not in (MESG_BROADCAST_DATA_ID,
                              MESG_ACKNOWLEDGED_DATA_ID, MESG_BURST_DATA_ID):
                continue
            ch = body[0] & 0x1F
            if ch in track_by_ch:
                log.tracked[track_by_ch[ch].key]["received"] += 1
                continue
            if ch != SCAN_CHANNEL:
                continue
            fields = extended_fields(body)
            if "device_number" not in fields:
                log.events["anonymous"] = log.events.get("anonymous", 0) + 1
                continue
            key = f"{fields['device_number']}:0x{fields['device_type']:02X}"
            log.sightings.setdefault(key, []).append(t)
        log.end = time.monotonic() - t0
        try:
            _close_scan(dev, reader)
        except RuntimeError:
            pass
    finally:
        close_device(dev)
    return {"version": version, "started_at": started_at,
            "summary": summarise(log),
            "raw": {"opens": log.opens, "closes": log.closes,
                    "sightings": log.sightings}}


# ---------------------------------------------------------------------------
# Entry point: one dongle in-process, several as child processes
# ---------------------------------------------------------------------------

def plan_children(serials: list[str], port: str | None,
                  baud: int) -> list[tuple[str, list[str]]]:
    """(label, device arguments) for each device in a same-window run.

    The --serial dongles come first, in the order given, then the --port
    child, labelled by its port name. --baud goes to the port child only.
    """
    plan = [(serial, ["--serial", serial]) for serial in serials]
    if port is not None:
        plan.append((port, ["--port", port, "--baud", str(baud)]))
    return plan


def _child_args(args, device_args: list[str], out: str,
                start_at: float) -> list[str]:
    cmd = [sys.executable, os.path.abspath(__file__), *device_args,
           "--seconds", str(args.seconds), "--child-json", out,
           "--start-at", repr(start_at)]
    for track in args.track:
        cmd += ["--track",
                f"{track.device_number}:{track.device_type}:{track.period}"]
    if args.reopen is not None:
        cmd += ["--reopen", str(args.reopen)]
    cmd += ["--scan-type", f"0x{args.scan_type:02X}"]
    return cmd


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", action="append", default=[],
                        help="a dongle whose USB serial ends with this. "
                             "Repeat for a same-window A/B")
    parser.add_argument("--port", help="a UART build instead of USB. Alone it "
                                       "is the single device; with --serial "
                                       "it joins the same-window run as one "
                                       "more child, labelled by this name")
    parser.add_argument("--baud", type=int, default=115200,
                        help="--port's baud rate (default 115200); not used "
                             "by --serial dongles")
    parser.add_argument("--seconds", type=float, default=300.0)
    parser.add_argument("--track", type=parse_track, action="append",
                        default=[], metavar="DEVNUM:TYPE[:PERIOD]",
                        help="also track this device on its own pinned "
                             "channel and report its loss. Repeatable")
    parser.add_argument("--reopen", type=float, metavar="S",
                        help="close/unassign/reassign/reopen the scan every S "
                             "seconds (Zwift: ~4.5)")
    parser.add_argument("--scan-type", type=lambda t: int(t, 0),
                        choices=SCAN_TYPES, default=CHANNEL_TYPE_SLAVE,
                        metavar="{0x00,0x40}",
                        help="the scan channel's type: 0x00 bidirectional "
                             "slave (default, as every run before "
                             "2026-11-12) or 0x40 receive-only (Zwift's)")
    parser.add_argument("--json", help="write the merged result here")
    parser.add_argument("--start-at", type=float, help=argparse.SUPPRESS)
    parser.add_argument("--child-json", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.reopen is not None and args.reopen < 1.0:
        parser.error("--reopen below 1 s leaves no time to listen")

    plan = plan_children(args.serial, args.port, args.baud)
    start_at = None
    if args.child_json or len(plan) <= 1:
        serial = args.serial[0] if args.serial else None
        name = serial or args.port or "dongle"
        try:
            result = run_one(serial, args.port, args.seconds, args.track,
                             args.reopen, args.start_at, args.baud,
                             args.scan_type)
        except (RuntimeError, OSError) as exc:
            result = {"error": str(exc)}
        if args.child_json:
            with open(args.child_json, "w", encoding="utf-8") as handle:
                json.dump(result, handle)
            return 0 if "error" not in result else 1
        results = {name: result}
    else:
        # Long enough for every child to open its device and set up its
        # tracked channels before the shared start.
        start_at = time.time() + 6.0 + 1.0 * len(args.track)
        results = {}
        with tempfile.TemporaryDirectory() as tmp:
            procs = []
            for label, device_args in plan:
                out = os.path.join(tmp, f"{len(procs)}.json")
                log = open(os.path.join(tmp, f"{len(procs)}.log"), "w")
                procs.append((label, out, log, subprocess.Popen(
                    _child_args(args, device_args, out, start_at),
                    stdout=log, stderr=subprocess.STDOUT)))
            for serial, out, log, proc in procs:
                proc.wait()
                log.close()
                try:
                    with open(out, encoding="utf-8") as handle:
                        results[serial] = json.load(handle)
                except (OSError, ValueError):
                    with open(log.name, encoding="utf-8",
                              errors="replace") as handle:
                        tail = handle.read()[-400:]
                    results[serial] = {"error": f"child failed: {tail}"}

    print(render(results))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump({"tool": "ant_scan_rate.py",
                       "seconds": args.seconds,
                       "reopen": args.reopen,
                       "scan_type": args.scan_type,
                       "track": [t.key for t in args.track],
                       "start_at": start_at,
                       "dongles": results}, handle, indent=2)
    return 0 if all("error" not in r for r in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
