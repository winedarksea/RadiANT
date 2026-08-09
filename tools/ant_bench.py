#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Benchmark the dongle's USB path, to compare one USB stack against another.

The firmware can be built on either of Zephyr's two USB device stacks (see
CONFIG_ANT_DONGLE_TRANSPORT_* and src/ant_transport.h). They present an
identical device, so the only way to choose between them on a board where both
work is to measure. This measures the thing a fitness app actually depends on:
how fast a command gets an answer, and how many messages a second the path
sustains.

Both tests use `request capabilities` (0x4D -> 0x54). It is the cheapest
message with a guaranteed reply and no side effects, so the number reflects the
USB round trip and the bridge, not the radio.

    python tools/ant_bench.py --label legacy --json legacy.json
    # reflash with next.conf, then
    python tools/ant_bench.py --label usbd --json usbd.json
    python tools/ant_bench.py --compare legacy.json usbd.json

Run it on an otherwise idle machine and expect a few percent of run-to-run
noise: this measures a host USB stack and its scheduler as much as the device.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_probe import (  # noqa: E402
    EP_IN,
    EP_OUT,
    MESG_CAPABILITIES_ID,
    MESG_REQUEST_ID,
    FrameReader,
    frame,
    close_device,
    open_device,
    reset_stack,
)

REQUEST = frame(MESG_REQUEST_ID, bytes([0x00, MESG_CAPABILITIES_ID]))


def _drain(dev, seconds: float = 0.2) -> None:
    """Swallow anything already queued, so it is not counted as a reply."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            dev.read(EP_IN, 64, timeout=50)
        except usb.core.USBError:
            return


def _await_capabilities(reader: FrameReader, timeout_s: float) -> bool:
    """Read frames until the capabilities reply lands. True if it did."""
    deadline = time.monotonic() + timeout_s
    while True:
        got = reader.next_frame(deadline)
        if got is None:
            return False
        if got[0] == MESG_CAPABILITIES_ID:
            return True


def bench_latency(dev, reader: FrameReader, count: int, timeout_s: float):
    """One request at a time, timed end to end.

    This is the number that decides whether a stack feels different: a host
    library issues a command and blocks until the dongle answers, so every
    command costs a full round trip and they do not overlap.
    """
    samples: list[float] = []
    timeouts = 0

    for _ in range(count):
        start = time.perf_counter()
        dev.write(EP_OUT, REQUEST)
        if _await_capabilities(reader, timeout_s):
            samples.append((time.perf_counter() - start) * 1000.0)
        else:
            timeouts += 1

    return samples, timeouts


def bench_throughput(dev, reader: FrameReader, count: int, depth: int,
                     timeout_s: float):
    """Requests kept in flight, to find the ceiling rather than the latency.

    Depth is capped well inside the firmware's limits - the RX ring is 512
    bytes and the TX queue is 32 frames - because the point is to saturate the
    USB path, not to find out what happens when it overflows.
    """
    delivered = 0
    timeouts = 0
    outstanding = 0
    sent = 0

    start = time.perf_counter()
    while delivered + timeouts < count:
        while outstanding < depth and sent < count:
            dev.write(EP_OUT, REQUEST)
            outstanding += 1
            sent += 1

        if _await_capabilities(reader, timeout_s):
            delivered += 1
        else:
            timeouts += 1
        outstanding -= 1

    elapsed = time.perf_counter() - start

    return delivered, timeouts, elapsed


def summarise(label: str, samples, timeouts, delivered, thr_timeouts, elapsed,
              count: int, depth: int) -> dict:
    ordered = sorted(samples)

    def pct(p: float) -> float:
        if not ordered:
            return float("nan")
        idx = min(len(ordered) - 1, int(round(p / 100.0 * (len(ordered) - 1))))
        return ordered[idx]

    return {
        "label": label,
        "count": count,
        "depth": depth,
        "latency_ms": {
            "n": len(ordered),
            "timeouts": timeouts,
            "mean": statistics.fmean(ordered) if ordered else float("nan"),
            "p50": pct(50),
            "p90": pct(90),
            "p99": pct(99),
            "max": ordered[-1] if ordered else float("nan"),
        },
        "throughput": {
            "delivered": delivered,
            "timeouts": thr_timeouts,
            "elapsed_s": elapsed,
            "msgs_per_s": delivered / elapsed if elapsed > 0 else float("nan"),
        },
    }


def report(result: dict) -> None:
    lat = result["latency_ms"]
    thr = result["throughput"]
    print(f"\n=== {result['label']} ===")
    print(f"  round-trip latency over {lat['n']} requests")
    print(f"    mean {lat['mean']:.3f} ms   p50 {lat['p50']:.3f}   "
          f"p90 {lat['p90']:.3f}   p99 {lat['p99']:.3f}   max {lat['max']:.3f}")
    if lat["timeouts"]:
        print(f"    TIMEOUTS: {lat['timeouts']}")
    print(f"  throughput at depth {result['depth']}")
    print(f"    {thr['msgs_per_s']:.0f} msg/s "
          f"({thr['delivered']} in {thr['elapsed_s']:.2f} s)")
    if thr["timeouts"]:
        print(f"    TIMEOUTS: {thr['timeouts']}")


def compare(paths: list[str]) -> int:
    results = [json.load(open(p)) for p in paths]
    for r in results:
        report(r)

    if len(results) != 2:
        return 0

    a, b = results
    print(f"\n=== {b['label']} relative to {a['label']} ===")

    for key, unit, lower_better in (("p50", "ms", True), ("p90", "ms", True),
                                    ("mean", "ms", True)):
        va, vb = a["latency_ms"][key], b["latency_ms"][key]
        delta = (vb - va) / va * 100.0 if va else float("nan")
        verdict = "better" if (delta < 0) == lower_better else "worse"
        print(f"  latency {key:5} {va:7.3f} -> {vb:7.3f} {unit}  "
              f"{delta:+6.1f}%  {verdict}")

    va = a["throughput"]["msgs_per_s"]
    vb = b["throughput"]["msgs_per_s"]
    delta = (vb - va) / va * 100.0 if va else float("nan")
    print(f"  throughput  {va:7.0f} -> {vb:7.0f} msg/s  {delta:+6.1f}%  "
          f"{'better' if delta > 0 else 'worse'}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="run", help="name for this run")
    parser.add_argument("--count", type=int, default=500,
                        help="requests per test (default: 500)")
    parser.add_argument("--depth", type=int, default=8,
                        help="requests kept in flight in the throughput test "
                             "(default: 8)")
    parser.add_argument("--warmup", type=int, default=50,
                        help="untimed requests first (default: 50)")
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--serial", help="match a dongle by serial suffix")
    parser.add_argument("--json", help="write results here")
    parser.add_argument("--compare", nargs="+", metavar="JSON",
                        help="report on saved runs instead of measuring")
    parser.add_argument(
        "--port",
        help="talk to a UART build over this serial port (e.g. COM8, "
             "/dev/ttyACM1) instead of over USB",
    )
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    if args.compare:
        return compare(args.compare)

    if args.depth > 16:
        sys.exit("--depth above 16 exceeds what the firmware queues; "
                 "this measures overflow, not speed.")

    dev = open_device(True, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)

    if not reset_stack(dev, reader):
        sys.exit("No startup message after reset - is this the ANT firmware?")
    _drain(dev)

    print(f"\nwarming up ({args.warmup} requests)")
    bench_latency(dev, reader, args.warmup, args.timeout)
    _drain(dev)

    print(f"measuring latency ({args.count} requests)")
    samples, timeouts = bench_latency(dev, reader, args.count, args.timeout)
    _drain(dev)

    print(f"measuring throughput ({args.count} requests, depth {args.depth})")
    delivered, thr_timeouts, elapsed = bench_throughput(
        dev, reader, args.count, args.depth, args.timeout)
    _drain(dev)

    close_device(dev)

    result = summarise(args.label, samples, timeouts, delivered,
                       thr_timeouts, elapsed, args.count, args.depth)
    report(result)

    if args.json:
        with open(args.json, "w") as handle:
            json.dump(result, handle, indent=2)
        print(f"\nwrote {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
