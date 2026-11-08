#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Loop SYSTEM_RESET against one endpoint and measure how often STARTUP is lost.

TODO.md item 2: in the 2026-11-07 Garmin comparison the nRF52840 Dongle once
failed to answer SYSTEM_RESET within 3 s (1 of ~94 opens). The suspect is the
host, not the firmware: ant_ack_gate.py read through FrameReader(timeout_ms=50),
the firmware sends STARTUP ~50 ms after the reset (handle_system_reset()'s
k_sleep), and under libusb-win32 a read timeout cancels the pending transfer -
a cancel that lands as a frame arrives loses that frame silently
(ant_probe.READ_TIMEOUT_MS's comment). This tool separates the two:

    # (a) baseline: a long read, which should never race - and the latency
    python tools/ant_reset_loop.py --serial 3D55F77818BE772A --count 1000
    # (b) the read timeout parked on the measured median latency
    python tools/ant_reset_loop.py --serial ... --count 1000 --read-timeout-ms 51
    # (c) the gate's own setting, idle and then with a channel tracking
    python tools/ant_reset_loop.py --serial ... --count 1000 --read-timeout-ms 50
    python tools/ant_reset_loop.py --serial ... --count 500 --read-timeout-ms 50 \\
        --traffic --device-number 54321

If (a) is clean and (b)/(c) fail, it is the host read race. If (a) fails too,
STARTUP really was dropped or delayed in the firmware; rerun against a
health.conf image (--health snapshots 0xF6 before and after - those counters
survive SYSTEM_RESET, so the delta spans the whole loop).

--reopen closes and reopens the device every iteration, the pattern
ant_ack_gate.py and ant_compare.py follow (one open per pass); without it every
reset goes down one handle.

--traffic opens one slave channel pinned to --device-number (an ant_sim master
should be on air) before each reset and dwells --dwell-s reading it, so the
reset lands with broadcasts flowing - the shape the gate's reset sees when a
previous tool left a channel up.

Prints one JSON document (and writes it to --json): per-iteration rows, the
failure count with its Wilson 95 % interval, and STARTUP latency percentiles.
Exit status 0 only if no iteration failed.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from datetime import UTC, datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ant_pages as ap  # noqa: E402
from ant_compare import quantile, wilson  # noqa: E402
from ant_wire import (  # noqa: E402
    CHANNEL_TYPE_SLAVE,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_STARTUP_MESG_ID,
    MESG_SYSTEM_RESET_ID,
    STARTUP_REASONS_BY_VALUE,
    frame,
)

ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
ANT_PLUS_FREQ = 57
CHANNEL = 0
EP_OUT = 0x01


def wait_startup(reader, t0: float, timeout_s: float,
                 clock=time.perf_counter) -> dict:
    """Read until STARTUP or `timeout_s` after `t0`. Returns the iteration row
    minus its index: ok, latency_ms (from the reset write), reason, and the
    message ids that arrived first (a reset that lands with traffic flowing
    should see broadcasts drain, then nothing, then STARTUP).

    `t0` is on `clock`, which is perf_counter and not monotonic: on Windows
    time.monotonic() ticks every ~15.6 ms, which quantised the first 1000-reset
    run's latencies to 47/63 ms - too coarse to aim run (b)'s read timeout at.
    The reader's deadline is monotonic, so it is converted here."""
    deadline = time.monotonic() + max(0.0, timeout_s - (clock() - t0))
    before: list[str] = []
    while True:
        got = reader.next_frame(deadline)
        if got is None:
            return {"ok": False, "latency_ms": None, "reason": None,
                    "frames_before": before}
        msg_id, body = got
        if msg_id == MESG_STARTUP_MESG_ID:
            reason = body[0] if body else None
            return {"ok": True,
                    "latency_ms": round((clock() - t0) * 1000.0, 3),
                    "reason": (STARTUP_REASONS_BY_VALUE.get(reason, reason)
                               if reason is not None else None),
                    "frames_before": before}
        before.append(f"0x{msg_id:02X}")


def _quiet_command(dev, reader, msg_id: int, payload: bytes,
                   timeout_s: float = 2.0) -> bool:
    dev.write(EP_OUT, frame(msg_id, payload))
    deadline = time.monotonic() + timeout_s
    while True:
        got = reader.next_frame(deadline)
        if got is None:
            return False
        rid, body = got
        if rid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
            return body[2] == 0


def open_traffic(dev, reader, device_number: int, device_type: int,
                 period: int) -> bool:
    steps = [
        (MESG_NETWORK_KEY_ID, bytes([ap.ANT_PLUS_NETWORK_NUM]) + ANT_PLUS_KEY),
        (MESG_ASSIGN_CHANNEL_ID,
         bytes([CHANNEL, CHANNEL_TYPE_SLAVE, ap.ANT_PLUS_NETWORK_NUM])),
        (MESG_CHANNEL_ID_ID, bytes([CHANNEL, device_number & 0xFF,
                                    device_number >> 8, device_type, 0])),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CHANNEL, ANT_PLUS_FREQ])),
        (MESG_CHANNEL_MESG_PERIOD_ID,
         bytes([CHANNEL, period & 0xFF, period >> 8])),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CHANNEL, 0xFF])),
        (MESG_OPEN_CHANNEL_ID, bytes([CHANNEL])),
    ]
    return all(_quiet_command(dev, reader, m, p) for m, p in steps)


def dwell(reader, seconds: float) -> int:
    """Read for `seconds`; return how many frames arrived."""
    end = time.monotonic() + seconds
    n = 0
    while time.monotonic() < end:
        if reader.next_frame(min(end, time.monotonic() + 0.1)) is not None:
            n += 1
    return n


def summarise(rows: list[dict], read_timeout_ms: int) -> dict:
    n = len(rows)
    failures = [r["i"] for r in rows if not r["ok"]]
    lat = [r["latency_ms"] for r in rows if r["latency_ms"] is not None]
    ci = wilson(len(failures), n)
    pct = {f"p{int(q * 100)}": quantile(lat, q)
           for q in (0.0, 0.1, 0.5, 0.9, 0.99, 1.0)} if lat else {}
    if lat:
        pct["min"] = pct.pop("p0")
        pct["max"] = pct.pop("p100")
    return {
        "iterations": n, "failures": len(failures),
        "failed_iterations": failures,
        "failure_rate_pct": ci[0] if ci else None,
        "failure_ci95_pct": [ci[1], ci[2]] if ci else None,
        "read_timeout_ms": read_timeout_ms,
        "latency_ms": pct,
        # Within one read timeout of the reset is where a timed-out read's
        # cancel can meet the STARTUP frame. Counted so (b) can show its
        # STARTUPs really did land in the window it was aiming at.
        "within_one_read_timeout": sum(1 for v in lat
                                       if v <= read_timeout_ms * 1.5),
    }


def _health(dev, reader) -> dict | None:
    import ant_health

    body = ant_health.request(dev, reader)
    return ant_health.decode(body) if body is not None else None


def main() -> int:
    from ant_probe import READ_TIMEOUT_MS, FrameReader, close_device, open_device

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial")
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--count", type=int, default=500)
    parser.add_argument("--read-timeout-ms", type=int, default=READ_TIMEOUT_MS,
                        help=f"FrameReader's per-read timeout (default "
                             f"{READ_TIMEOUT_MS}; ant_ack_gate.py used 50)")
    parser.add_argument("--reset-timeout-s", type=float, default=3.0,
                        help="how long to wait for STARTUP (reset_stack's 3 s)")
    parser.add_argument("--reopen", action="store_true",
                        help="close and reopen the device every iteration")
    parser.add_argument("--traffic", action="store_true",
                        help="open a pinned RX channel before each reset")
    parser.add_argument("--device-number", type=int, default=None)
    parser.add_argument("--device-type", type=int, default=11)
    parser.add_argument("--period", type=int, default=8182)
    parser.add_argument("--dwell-s", type=float, default=0.6,
                        help="with --traffic, read this long before resetting")
    parser.add_argument("--pause-s", type=float, default=0.0,
                        help="idle gap between iterations")
    parser.add_argument("--health", action="store_true",
                        help="snapshot 0xF6 before and after the loop")
    parser.add_argument("--json", default=None)
    args = parser.parse_args()
    if args.traffic and args.device_number is None:
        parser.error("--traffic needs --device-number (pin an ant_sim master)")

    def open_one():
        dev = open_device(False, serial=args.serial, port=args.port,
                          baud=args.baud, wait_s=10.0)
        return dev, FrameReader(dev, timeout_ms=args.read_timeout_ms)

    report = {"recorded": datetime.now(UTC).isoformat(),
              "endpoint": args.port or args.serial,
              "args": {k: v for k, v in vars(args).items() if k != "json"}}
    rows: list[dict] = []
    traffic_failures = 0
    framing = [0, 0]    # checksum errors, discarded bytes, over every reader
    dev, reader = open_one()
    try:
        if args.health:
            report["health_before"] = _health(dev, reader)
        for i in range(args.count):
            if args.reopen and i:
                framing[0] += reader.checksum_errors
                framing[1] += reader.discarded_bytes
                close_device(dev)
                dev, reader = open_one()
            heard = None
            if args.traffic:
                if open_traffic(dev, reader, args.device_number,
                                args.device_type, args.period):
                    heard = dwell(reader, args.dwell_s)
                else:
                    traffic_failures += 1
            t0 = time.perf_counter()
            dev.write(EP_OUT, frame(MESG_SYSTEM_RESET_ID, b"\x00"))
            row = {"i": i, **wait_startup(reader, t0, args.reset_timeout_s)}
            if heard is not None:
                row["frames_dwell"] = heard
            rows.append(row)
            if not row["ok"]:
                print(f"  iteration {i}: NO STARTUP within "
                      f"{args.reset_timeout_s} s (before: "
                      f"{row['frames_before']})", flush=True)
            if (i + 1) % 100 == 0:
                fails = sum(not r["ok"] for r in rows)
                print(f"{i + 1}/{args.count}: {fails} failed", flush=True)
            if args.pause_s:
                time.sleep(args.pause_s)
        if args.health:
            report["health_after"] = _health(dev, reader)
    except KeyboardInterrupt:
        report["interrupted"] = True
    finally:
        close_device(dev)

    report["summary"] = summarise(rows, args.read_timeout_ms)
    report["summary"]["checksum_errors"] = framing[0] + reader.checksum_errors
    report["summary"]["discarded_bytes"] = framing[1] + reader.discarded_bytes
    if args.traffic:
        report["summary"]["traffic_open_failures"] = traffic_failures
    report["rows"] = rows
    text = json.dumps(report, indent=2)
    if args.json:
        Path(args.json).write_text(text + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))
    return 0 if report["summary"]["failures"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
