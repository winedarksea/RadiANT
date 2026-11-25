#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Kill a host mid-frame, reopen, and check the dongle answers the next opener.

The known issue this retests: a host process that dies while it is writing an
ANT frame (killed between the first bytes of a frame and the rest) leaves the
dongle's receive parser part-way through that frame. If the firmware does not
drop the stale partial frame, the next host's first request is glued onto it,
fails its checksum, and the new host sees no answer to its opener.

Per iteration:

  1. A CHILD process opens the device (ant_probe.open_device, the same open
     every tool here uses), writes the first --partial-bytes bytes of a
     capabilities request - SYNC 0xA4, length, id 0x4D, first payload byte by
     default - and dies with os._exit(), so nothing releases the device, no
     close or reset is sent and no Python cleanup runs. The OS reclaims the
     handle the way it does for a killed process.
  2. The PARENT opens the device fresh and sends one complete request:
     MESG_REQUEST (0x4D) for MESG_CAPABILITIES (0x54).
  3. The iteration passes if the 0x54 reply arrives within --reply-timeout
     (default 1 s) of the write.

Before the loop the parent sends the same request once with no partial frame
in front of it. A dongle that does not answer that is not measuring this
issue, so the run stops with exit status 2.

The delay between iterations is random, --delay-min..--delay-max seconds
(memory bench-reopen-phase-lock: a fixed close/reopen delay samples one search
phase of whatever the dongle is doing and reports that phase as the result).
--seed makes the delays repeatable.

--partial-bytes equal to the whole frame (6 for this request) is the control
arm: the child still dies without releasing anything, but leaves no partial
frame behind. A failure there is the process death, not the partial frame.

    python tools/bench_midframe_reopen.py --serial EE6986854FD72803 \\
        --count 200 --json midframe.json
    python tools/bench_midframe_reopen.py --serial ... --partial-bytes 6  # control

Prints N/M answered and a summary, writes the full result to --json, and
exits 0 only if every iteration's child wrote its partial frame and every
reopen was answered in time.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import subprocess
import sys
import time
from datetime import UTC, datetime
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ant_wire import (  # noqa: E402
    MESG_CAPABILITIES_ID,
    MESG_REQUEST_ID,
    SYNC_TX,
    frame,
)

# The one request this tool sends: "request message 0x54" on channel 0.
CAPABILITIES_REQUEST = frame(MESG_REQUEST_ID,
                             bytes([0x00, MESG_CAPABILITIES_ID]))

# SYNC, length, id, first payload byte: the frame cut off after its first
# payload byte, which leaves the parser waiting for one more byte and the
# checksum.
DEFAULT_PARTIAL_BYTES = 4

# Child exit codes. Anything else (open_device()'s sys.exit text gives 1) is
# reported as-is.
CHILD_WROTE = 0
CHILD_WRITE_FAILED = 3

# How long the parent waits for the child to open, write and die.
CHILD_TIMEOUT_S = 20.0


def partial_frame(n: int, request: bytes = CAPABILITIES_REQUEST) -> bytes:
    """The first `n` bytes of `request`, 1 <= n <= len(request).

    n == len(request) is the whole frame (the control arm).
    """
    if not 1 <= n <= len(request):
        raise ValueError(f"partial frame length {n} outside 1..{len(request)}")
    if request[0] != SYNC_TX:
        raise ValueError("request does not start with SYNC")
    return request[:n]


def partial_bytes_arg(text: str) -> int:
    """argparse type for --partial-bytes: 1..the request's full length."""
    try:
        value = int(text, 10)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"not a whole number: {text!r}") from None
    if not 1 <= value <= len(CAPABILITIES_REQUEST):
        raise argparse.ArgumentTypeError(
            f"must be 1..{len(CAPABILITIES_REQUEST)} (the request is "
            f"{len(CAPABILITIES_REQUEST)} bytes), got {value}")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial",
                        help="match a device whose serial ends with this")
    parser.add_argument("--port", help="UART transport: the serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--count", type=int, default=100,
                        help="iterations (default: 100)")
    parser.add_argument("--partial-bytes", type=partial_bytes_arg,
                        default=DEFAULT_PARTIAL_BYTES,
                        help="how much of the request the dying child "
                             f"writes (default: {DEFAULT_PARTIAL_BYTES}: "
                             "SYNC, length, id, first byte). The full length "
                             f"({len(CAPABILITIES_REQUEST)}) is the control "
                             "arm")
    parser.add_argument("--reply-timeout", type=float, default=1.0,
                        help="how long the reopened host waits for the "
                             "capabilities reply (default: 1)")
    parser.add_argument("--open-wait", type=float, default=5.0,
                        help="how long each open may wait for the device to "
                             "appear (default: 5)")
    parser.add_argument("--delay-min", type=float, default=0.05,
                        help="shortest random delay between iterations, s "
                             "(default: 0.05)")
    parser.add_argument("--delay-max", type=float, default=0.75,
                        help="longest random delay between iterations, s "
                             "(default: 0.75)")
    parser.add_argument("--seed", type=int, default=None,
                        help="seed for the delays, for a repeatable run")
    parser.add_argument("--json", default=None, help="write the result here")
    # Internal: the dying child. Not for interactive use.
    parser.add_argument("--child", action="store_true",
                        help=argparse.SUPPRESS)
    return parser


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.serial and args.port:
        parser.error("--serial and --port pick different transports; "
                     "give one")
    if args.count < 1:
        parser.error("--count must be at least 1")
    if args.reply_timeout <= 0:
        parser.error("--reply-timeout must be positive")
    if args.delay_min < 0 or args.delay_max < args.delay_min:
        parser.error("need 0 <= --delay-min <= --delay-max")
    return args


def child_argv(args: argparse.Namespace,
               script: str = os.path.abspath(__file__),
               python: str = sys.executable) -> list[str]:
    """The command line that runs one dying child against the same device."""
    argv = [python, script, "--child",
            "--partial-bytes", str(args.partial_bytes),
            "--open-wait", str(args.open_wait)]
    if args.serial:
        argv += ["--serial", args.serial]
    if args.port:
        argv += ["--port", args.port, "--baud", str(args.baud)]
    return argv


def pick_delay(rng: random.Random, lo: float, hi: float) -> float:
    """One inter-iteration delay, uniform in [lo, hi], rounded to 0.1 ms."""
    return round(rng.uniform(lo, hi), 4)


def summarise(rows: list[dict]) -> dict:
    """The run's N/M and failure figures from its per-iteration rows."""
    from ant_compare import quantile, wilson

    n = len(rows)
    valid = [r for r in rows if r["child_rc"] == CHILD_WROTE]
    answered = [r for r in valid if r["ok"]]
    failed = [r["i"] for r in valid if not r["ok"]]
    child_failed = [r["i"] for r in rows if r["child_rc"] != CHILD_WROTE]
    ci = wilson(len(failed), len(valid))
    lat = [r["round_trip_ms"] for r in answered]
    return {
        "iterations": n,
        "valid": len(valid),
        "answered": len(answered),
        "failures": len(failed),
        "failed_iterations": failed,
        "child_failures": len(child_failed),
        "child_failed_iterations": child_failed,
        "failure_rate_pct": ci[0] if ci else None,
        "failure_ci95_pct": [ci[1], ci[2]] if ci else None,
        "round_trip_ms": ({"min": min(lat), "p50": quantile(lat, 0.5),
                           "p90": quantile(lat, 0.9), "max": max(lat)}
                          if lat else {}),
    }


def passed(summary: dict) -> bool:
    return (summary["iterations"] > 0
            and summary["child_failures"] == 0
            and summary["failures"] == 0)


# -- device side -------------------------------------------------------------

def run_child(args: argparse.Namespace) -> None:
    """Open, write the partial frame, and die without releasing anything."""
    from ant_probe import EP_OUT, open_device

    dev = open_device(False, wait_s=args.open_wait, serial=args.serial,
                      port=args.port, baud=args.baud)
    try:
        dev.write(EP_OUT, partial_frame(args.partial_bytes))
    except Exception as exc:  # noqa: BLE001 - reported, then the hard exit
        print(f"child: write failed: {exc}", file=sys.stderr, flush=True)
        os._exit(CHILD_WRITE_FAILED)
    sys.stdout.flush()
    sys.stderr.flush()
    # The point of the tool: no close_device(), no reset, no finalisers.
    os._exit(CHILD_WROTE)


def spawn_child(args: argparse.Namespace) -> int | None:
    """Run one dying child. Its exit code, or None if it had to be killed."""
    proc = subprocess.Popen(child_argv(args))
    try:
        return proc.wait(timeout=CHILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return None


def request_capabilities(args: argparse.Namespace) -> float | None:
    """Open fresh, send one capabilities request, close.

    Returns the round trip in ms, or None if no 0x54 came back within
    --reply-timeout. Frames other than 0x54 are read past.
    """
    from ant_probe import EP_OUT, FrameReader, close_device, open_device

    dev = open_device(False, wait_s=args.open_wait, serial=args.serial,
                      port=args.port, baud=args.baud)
    try:
        reader = FrameReader(dev)
        t0 = time.perf_counter()
        dev.write(EP_OUT, CAPABILITIES_REQUEST)
        deadline = time.monotonic() + args.reply_timeout
        while True:
            got = reader.next_frame(deadline)
            elapsed = time.perf_counter() - t0
            if got is None or elapsed > args.reply_timeout:
                return None
            if got[0] == MESG_CAPABILITIES_ID:
                return round(elapsed * 1000.0, 3)
    finally:
        close_device(dev)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.child:
        run_child(args)
        return CHILD_WRITE_FAILED  # not reached: run_child() always exits

    try:
        import usb.core  # noqa: F401 - import-guard
    except ImportError:  # pragma: no cover - user-facing guidance
        sys.exit("pyusb is not installed. Run: pip install pyusb")

    report = {
        "recorded": datetime.now(UTC).isoformat(),
        "endpoint": args.port or args.serial,
        "partial_frame": partial_frame(args.partial_bytes).hex(),
        "control_arm": args.partial_bytes == len(CAPABILITIES_REQUEST),
        "args": {k: v for k, v in vars(args).items()
                 if k not in ("json", "child")},
    }

    print("Pre-check: one request with no partial frame in front of it")
    pre = request_capabilities(args)
    report["precheck_round_trip_ms"] = pre
    if pre is None:
        print(f"  no capabilities reply within {args.reply_timeout} s: the "
              "dongle is not answering even a clean open, so this run "
              "cannot measure the mid-frame case")
        if args.json:
            Path(args.json).write_text(json.dumps(report, indent=2) + "\n",
                                       encoding="utf-8")
        return 2
    print(f"  answered in {pre:.1f} ms")

    rng = random.Random(args.seed)
    rows: list[dict] = []
    print(f"\n{args.count} iterations, child writes "
          f"{report['partial_frame']} then dies")
    try:
        for i in range(args.count):
            delay = pick_delay(rng, args.delay_min, args.delay_max)
            time.sleep(delay)
            child_rc = spawn_child(args)
            row = {"i": i, "delay_s": delay, "child_rc": child_rc,
                   "ok": False, "round_trip_ms": None}
            if child_rc == CHILD_WROTE:
                rtt = request_capabilities(args)
                row["round_trip_ms"] = rtt
                row["ok"] = rtt is not None
                if rtt is None:
                    print(f"  iteration {i}: NO REPLY within "
                          f"{args.reply_timeout} s after the mid-frame death",
                          flush=True)
            else:
                print(f"  iteration {i}: child did not write its partial "
                      f"frame (exit {child_rc})", flush=True)
            rows.append(row)
            if (i + 1) % 25 == 0:
                s = summarise(rows)
                print(f"  {i + 1}/{args.count}: {s['answered']}/{s['valid']} "
                      "answered", flush=True)
    except KeyboardInterrupt:
        report["interrupted"] = True

    summary = summarise(rows)
    report["summary"] = summary
    report["passed"] = passed(summary)
    report["rows"] = rows

    print(f"\n{summary['answered']}/{summary['valid']} reopens answered "
          f"within {args.reply_timeout} s"
          + (f" ({summary['child_failures']} iteration(s) where the child "
             "never wrote)" if summary["child_failures"] else ""))
    print(json.dumps(summary, indent=2))
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2) + "\n",
                                   encoding="utf-8")
        print(f"wrote {args.json}")
    print("PASS" if report["passed"] else "FAIL")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
