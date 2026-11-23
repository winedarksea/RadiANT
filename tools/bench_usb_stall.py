#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Stall the host's bulk-IN reads and prove the dongle survives it.

This is the verification for the bounded bulk-IN wait in
apps/common/usb_ant_class.c. The bug it covers cannot be reached from a unit
test, on the target or off it: it needs a real USB host that stays CONFIGURED
and simply stops polling IN - a suspended, reset, or unplugged device takes a
different path entirely (the status callback tears the transfers down). So the
test is this script, against real hardware, and the counters in the 0xF6 reply
are how its result is read.

What used to happen: usb_ant_write() held tx_mutex across a usb_transfer_sync()
that waited K_FOREVER. One unread bulk-IN transfer parked the bridge thread
permanently, with the mutex held, so the USB TX thread queued behind it and the
dongle answered nothing ever again - short of a replug.

    # Feather DUT (legacy USB + health.conf), an ANT+ master transmitting
    python tools/bench_usb_stall.py
    python tools/bench_usb_stall.py --json stall-legacy.json

The shape of the run:

  1. read the 0xF6 counters (baseline)
  2. open a wildcard ANT+ slave channel, so the DONGLE is producing real radio
     traffic for the whole run - without a master on the air there is nothing
     to stall, and the script refuses to report a pass
  3. read normally for 5 s
  4. STOP READING for 15 s while the dongle keeps producing - no suspend, no
     reset, the device stays configured and the host just never submits an IN
     transfer
  5. resume, and time how long a `request capabilities` takes to come back
  6. read the counters again

Pass requires all three: the request round-trips in under 2 s, `tx_timeout` has
climbed (the firmware detected the stall and abandoned the wait), and
`tx_drops` has climbed (events really were produced with nowhere to go).

Needs CONFIG_ANT_DONGLE_HEALTH_COUNTERS=y - two of the three assertions are
counter reads. Build with `-DEXTRA_CONF_FILE=health.conf`.

THE tx_drops ASSERTION IS WRONG FOR THE FIXED IMAGE, and this is why the
verdict is the pair of runs, not one. Measured 2026-08-16 on a Feather, same
board, same HR sensor, same 30 s stall, only the firmware differing:

    pre-W6 (70128f8)   tx_timeout +0    tx_drops +88
    W6     (3136eaf)   tx_timeout +1    tx_drops  +0

CORRECTED 2026-10-31: the "+0" on the W6 image was a firmware bug, not
"nothing to drop". After the first 1 s timeout the controller still holds the
frame the host never read, so every later write fails at once - and
usb_transfer() reports that failure as a completion of size 0, which the
legacy usb_ant_write() returned as success. Every event of the stall was lost
and none was counted. usb_ant_write() now counts a short completion as
ANT_HEALTH_TX_DROP and returns -EIO, so on a fixed image tx_drops climbs
again, roughly by the events produced during the stall. The pre-W6 control is
still worth running: there the drops come from the msgq overflowing behind a
parked thread, and tx_timeout stays +0.

Also load-bearing: simply not calling read() does NOT stop the host from
draining bulk-IN, because libusb-win32 keeps its own buffer - a trickle of
small request/response replies, with no radio involved, is absorbed by the
host and the endpoint never backs up. A reduced variant built on that trickle
once reported PASS against both a fixed image and firmware that predates the
fix entirely, i.e. it was measuring nothing. Real radio traffic at a channel
period is what actually fills the endpoint faster than the host retires it.

--suspend-cycles N is a second, separate test (Linux only; it prints SKIP and
exits 0 anywhere else). It covers the USB_NEXT suspend leak: on the new stack
a bus suspend leaves the queued bulk-OUT buffer with the controller, and
firmware that forgot it queued one more on every resume, until the shared
UDC pool ran dry and no reply could be sent. The host side is sysfs runtime
PM, run as root:

    sudo python3 tools/bench_usb_stall.py --suspend-cycles 40

  1. set power/control=auto and a short power/autosuspend_delay_ms
  2. per cycle: close the device, wait for power/runtime_status=suspended,
     reopen (which resumes it) and time one `request capabilities`
  3. flood requests and count the replies, then read the 0xF6 counters
  4. put power/control and the delay back as they were

Pass is every per-cycle reply plus every flood reply. The reopen skips
SET_CONFIGURATION on purpose: Linux sends a repeat SET_CONFIGURATION for it,
which resets the host's data toggles but not USBD's (usbd_ch9.c returns early
on the current value), and the first packet after that can be lost as a
duplicate - a different defect, which would show up here as a fake failure.
No radio traffic is needed. rx_backpressure is reported, not judged: it counts
both a legitimate NAK and a dropped packet.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time

try:
    import usb.core  # noqa: F401 - import-guard, verifies pyusb is installed
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

# The 0xF6 request and its decode are ant_health.py's, not a second copy here.
# That module owns the field table and the length rule (a body shorter than the
# current layout is refused, so every field this test reads is always present);
# duplicating either is how two tools end up disagreeing about what a counter
# means.
from ant_health import decode as decode_health  # noqa: E402
from ant_health import request as request_health  # noqa: E402
from ant_probe import (  # noqa: E402
    EP_OUT,
    PIDS,
    VID,
    FrameReader,
    _backends,
    _serial_of,
    close_device,
    frame,
    open_device,
    reset_stack,
)
from ant_wire import (  # noqa: E402
    MESG_ACKNOWLEDGED_DATA_ID,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_BROADCAST_DATA_ID,
    MESG_BURST_DATA_ID,
    MESG_CAPABILITIES_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_REQUEST_ID,
    MESG_RESPONSE_EVENT_ID,
)

# Same public ANT+ key, frequency and wildcard slave setup as tools/ant_scan.py
# - see that file for what each step is for.
ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
ANT_PLUS_FREQ = 57
CHANNEL = 0

DATA_IDS = (MESG_BROADCAST_DATA_ID, MESG_ACKNOWLEDGED_DATA_ID,
            MESG_BURST_DATA_ID)

CAPABILITIES_REQUEST = frame(MESG_REQUEST_ID,
                             bytes([0x00, MESG_CAPABILITIES_ID]))


def command(dev, reader, msg_id: int, payload: bytes, name: str,
            timeout: float = 2.0) -> bool:
    """Send a configuration message and wait for its 0x40 acknowledgement."""
    dev.write(EP_OUT, frame(msg_id, payload))

    deadline = time.monotonic() + timeout
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            print(f"  FAIL: {name} was not acknowledged")
            return False

        got_id, body = result
        if got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
            if body[1] != msg_id:
                continue
            if body[2] != 0:
                print(f"  FAIL: {name} rejected, code {body[2]}")
                return False
            print(f"  OK: {name}")
            return True


def open_channel(dev, reader) -> bool:
    steps = [
        (MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "network key"),
        (MESG_ASSIGN_CHANNEL_ID, bytes([CHANNEL, 0x00, 0x00]),
         "assign channel (slave)"),
        (MESG_CHANNEL_ID_ID, bytes([CHANNEL, 0, 0, 0, 0]),
         "wildcard channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CHANNEL, ANT_PLUS_FREQ]),
         "radio frequency 2457 MHz"),
        (MESG_CHANNEL_MESG_PERIOD_ID, bytes([CHANNEL, 0x86, 0x1F]),
         "message period"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CHANNEL, 0xFF]),
         "infinite search timeout"),
        (MESG_OPEN_CHANNEL_ID, bytes([CHANNEL]), "open channel"),
    ]
    # all() over a generator short-circuits, so a refused step stops the
    # sequence rather than issuing the rest against a channel that never got
    # configured - command() has already printed which one it was.
    return all(command(dev, reader, msg_id, payload, name)
               for msg_id, payload, name in steps)


def read_for(reader, seconds: float) -> int:
    """Read frames for `seconds`, returning how many carried ANT data."""
    packets = 0
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 0.5))
        if result is None:
            continue
        if result[0] in DATA_IDS:
            packets += 1
    return packets


def await_capabilities(reader, timeout_s: float) -> float | None:
    """Time a capabilities reply. Returns seconds, or None if it never came.

    Anything queued from before the stall is read on the way past and counts
    against the elapsed time on purpose: "the dongle answers again" means the
    answer reached the host, backlog and all.
    """
    start = time.perf_counter()
    deadline = start + timeout_s
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return None
        if result[0] == MESG_CAPABILITIES_ID:
            return time.perf_counter() - start


def read_health(dev, reader, label: str) -> dict | None:
    body = request_health(dev, reader)
    if body is None:
        return None
    try:
        got = decode_health(body)
    except ValueError as exc:
        print(f"  malformed 0xF6 reply ({label}): {exc}")
        return None
    return got


# -- --suspend-cycles: the USB_NEXT suspend/resume leak ------------------------

SYSFS_USB_DEVICES = "/sys/bus/usb/devices"

# Documentation/ABI/testing/sysfs-devices-power. "unsupported" means runtime
# PM is disabled for the device, so it can never suspend and the test would
# measure nothing.
RUNTIME_STATES = ("active", "suspended", "suspending", "resuming", "error",
                  "unsupported")
POWER_CONTROL_VALUES = ("auto", "on")

# Consecutive per-cycle failures after which the loop stops: by then the
# answer is in, and a wedged board does not need 30 more cycles of proof.
MAX_CONSECUTIVE_FAILURES = 3


def suspend_cycles_arg(text: str) -> int:
    """argparse type for --suspend-cycles: a whole number, at least 1."""
    try:
        value = int(text, 10)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"not a whole number: {text!r}") from None
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be at least 1, got {value}")
    return value


def sysfs_usb_name(bus: int, port_numbers) -> str:
    """The kernel's name for a device: bus 5 ports (1,) is '5-1', (1, 4) '5-1.4'.

    `port_numbers` is pyusb's Device.port_numbers. None or empty is a root hub,
    which is never the dongle.
    """
    if not port_numbers:
        raise ValueError("no port path: not a device behind a hub port")
    return f"{int(bus)}-" + ".".join(str(int(p)) for p in port_numbers)


def parse_runtime_status(text: str) -> str:
    """power/runtime_status contents -> one of RUNTIME_STATES, else ValueError."""
    state = text.strip()
    if state not in RUNTIME_STATES:
        raise ValueError(f"unknown power/runtime_status {state!r}")
    return state


def parse_power_control(text: str) -> str:
    """power/control contents -> 'auto' or 'on', else ValueError."""
    value = text.strip()
    if value not in POWER_CONTROL_VALUES:
        raise ValueError(f"unknown power/control {value!r}")
    return value


def parse_sysfs_int(text: str) -> int:
    """A sysfs integer attribute (autosuspend_delay_ms, runtime_*_time)."""
    return int(text.strip(), 10)


def suspend_supported(platform: str, sysfs_root: str = SYSFS_USB_DEVICES
                      ) -> str | None:
    """None if --suspend-cycles can run here, else the reason it cannot."""
    if not platform.startswith("linux"):
        return f"needs Linux sysfs runtime PM (this is {platform})"
    if not os.path.isdir(sysfs_root):
        return f"{sysfs_root} does not exist"
    return None


def _sysfs_read(path: str) -> str:
    with open(path, encoding="ascii") as handle:
        return handle.read()


def _sysfs_write(path: str, value: str) -> None:
    with open(path, "w", encoding="ascii") as handle:
        handle.write(value)


def find_without_set_configuration(serial: str | None, wait_s: float):
    """Find and take the dongle, WITHOUT the SET_CONFIGURATION open_device() sends.

    Linux turns a SET_CONFIGURATION for the current value into
    usb_reset_configuration(), which resets the host's endpoint data toggles.
    USBD ignores a repeat of its current configuration (usbd_ch9.c), so the
    device keeps its toggles and the first packet after the reopen can be
    taken as a retransmission and dropped. That is a separate defect; this
    test must not trip over it. The kernel configured the device when it
    enumerated, so there is nothing to set.

    Reading the serial opens the device, which is what resumes it.
    """
    backend = (_backends() or [None])[0]
    deadline = time.monotonic() + wait_s
    while True:
        found = [d for pid in PIDS
                 for d in usb.core.find(idVendor=VID, idProduct=pid,
                                        find_all=True, backend=backend)]
        if serial is not None:
            found = [d for d in found
                     if (_serial_of(d) or "").endswith(serial)]
        if len(found) > 1:
            sys.exit(f"{len(found)} ANT devices are attached; pick one with "
                     "--serial")
        if found or time.monotonic() >= deadline:
            break
        time.sleep(0.1)
    if not found:
        return None
    dev = found[0]
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass
    return dev


def wait_runtime_status(path: str, want: str, timeout_s: float):
    """Poll power/runtime_status until it reads `want`. -> (reached, last)."""
    deadline = time.monotonic() + timeout_s
    while True:
        state = parse_runtime_status(_sysfs_read(path))
        if state == want:
            return True, state
        if time.monotonic() >= deadline:
            return False, state
        time.sleep(0.02)


def capabilities_round_trip(dev, timeout_s: float) -> float | None:
    """One request capabilities on a fresh reader. Seconds, or None."""
    reader = FrameReader(dev)
    try:
        dev.write(EP_OUT, CAPABILITIES_REQUEST, timeout=int(timeout_s * 1000))
    except usb.core.USBError as exc:
        print(f"    write failed: {exc}")
        return None
    return await_capabilities(reader, timeout_s)


def flood_capabilities(dev, count: int, timeout_s: float) -> tuple[int, int]:
    """Write `count` requests back to back while a thread reads the replies.

    Returns (written, replies). Reading concurrently matters: replies the host
    does not collect back up into the firmware's TX queue and are dropped
    there, which would read as a receive-side loss.
    """
    reader = FrameReader(dev)
    replies = [0]
    stop = threading.Event()

    def collect():
        deadline = time.monotonic() + timeout_s
        while replies[0] < count and not stop.is_set():
            result = reader.next_frame(min(deadline, time.monotonic() + 0.5))
            if result is not None and result[0] == MESG_CAPABILITIES_ID:
                replies[0] += 1
            elif result is None and time.monotonic() >= deadline:
                return

    thread = threading.Thread(target=collect, daemon=True)
    thread.start()
    written = 0
    for _ in range(count):
        try:
            dev.write(EP_OUT, CAPABILITIES_REQUEST, timeout=5000)
        except usb.core.USBError as exc:
            print(f"    flood write {written + 1} failed: {exc}")
            break
        written += 1
    thread.join(timeout_s + 2.0)
    stop.set()
    thread.join(2.0)
    return written, replies[0]


def run_suspend_cycles(args) -> int:
    reason = suspend_supported(sys.platform)
    if reason is not None:
        print(f"SKIP: --suspend-cycles {reason}")
        return 0

    dev = find_without_set_configuration(args.serial, 5.0)
    if dev is None:
        sys.exit("No ANT device found")
    name = sysfs_usb_name(dev.bus, dev.port_numbers)
    sysdir = os.path.join(SYSFS_USB_DEVICES, name)
    power = os.path.join(sysdir, "power")
    print(f"Device {name} ({sysdir}), serial {_serial_of(dev)}")

    before = read_health(dev, FrameReader(dev), "baseline")
    if before is None:
        sys.exit("no usable 0xF6 reply: build with health.conf, so the "
                 "rx_backpressure counter can be read")
    print(f"  baseline rx_backpressure={before['rx_backpressure']} "
          f"tx_drops={before['tx_drops']} tx_timeout={before['tx_timeout']}")

    control_path = os.path.join(power, "control")
    delay_path = os.path.join(power, "autosuspend_delay_ms")
    status_path = os.path.join(power, "runtime_status")
    suspended_time_path = os.path.join(power, "runtime_suspended_time")
    old_control = parse_power_control(_sysfs_read(control_path))
    old_delay = parse_sysfs_int(_sysfs_read(delay_path))

    results = {"device": name, "cycles_requested": args.suspend_cycles,
               "counters_before": before, "cycles": []}
    first_failure = None
    consecutive = 0
    suspended_ms0 = parse_sysfs_int(_sysfs_read(suspended_time_path))
    try:
        _sysfs_write(delay_path, str(args.autosuspend_ms))
        _sysfs_write(control_path, "auto")
        print(f"Runtime PM: control auto (was {old_control}), delay "
              f"{args.autosuspend_ms} ms (was {old_delay})")

        for cycle in range(1, args.suspend_cycles + 1):
            close_device(dev)
            reached, state = wait_runtime_status(status_path, "suspended",
                                                 args.suspend_timeout)
            dev = find_without_set_configuration(args.serial, 5.0)
            if dev is None:
                print(f"  cycle {cycle}: device gone after suspend")
                results["cycles"].append({"cycle": cycle, "suspended": reached,
                                          "round_trip_s": None})
                first_failure = first_failure or cycle
                break
            rtt = capabilities_round_trip(dev, args.resume_timeout)
            results["cycles"].append({"cycle": cycle, "suspended": reached,
                                      "round_trip_s": rtt})
            tag = "suspended" if reached else f"NOT suspended ({state})"
            shown = f"{rtt * 1000:.1f} ms" if rtt is not None else "NO REPLY"
            print(f"  cycle {cycle:3d}: {tag}, capabilities {shown}")
            if rtt is None:
                first_failure = first_failure or cycle
                consecutive += 1
                if consecutive >= MAX_CONSECUTIVE_FAILURES:
                    print(f"  {consecutive} failures in a row, stopping")
                    break
            else:
                consecutive = 0

        written, replies, after = 0, 0, None
        if dev is not None:
            print(f"\nFlood: {args.flood} requests")
            written, replies = flood_capabilities(dev, args.flood, 10.0)
            print(f"  wrote {written}, {replies} replies")
            after = read_health(dev, FrameReader(dev), "after")
    finally:
        try:
            _sysfs_write(control_path, old_control)
            _sysfs_write(delay_path, str(old_delay))
        except OSError as exc:
            print(f"  could not restore runtime PM settings: {exc}")
        if dev is not None:
            close_device(dev)

    suspended_ms = parse_sysfs_int(_sysfs_read(suspended_time_path)) \
        - suspended_ms0
    done = results["cycles"]
    suspended = sum(1 for c in done if c["suspended"])
    answered = sum(1 for c in done if c["round_trip_s"] is not None)
    results.update({
        "cycles_run": len(done), "cycles_suspended": suspended,
        "cycles_answered": answered, "first_failure": first_failure,
        "suspended_ms": suspended_ms, "flood_written": written,
        "flood_replies": replies, "counters_after": after,
    })
    if after is not None:
        for field in ("rx_backpressure", "tx_drops", "tx_timeout"):
            results[f"{field}_delta"] = after[field] - before[field]

    checks = [
        ("every cycle really suspended",
         suspended == len(done) == args.suspend_cycles,
         f"{suspended}/{args.suspend_cycles} ({suspended_ms} ms suspended)"),
        ("every per-cycle request answered",
         answered == args.suspend_cycles,
         f"{answered}/{args.suspend_cycles}"
         + (f", first failure at cycle {first_failure}"
            if first_failure else "")),
        ("every flood request answered",
         written == replies == args.flood, f"{replies}/{args.flood}"),
        ("0xF6 answers after the cycles", after is not None,
         f"rx_backpressure {results['rx_backpressure_delta']:+d} "
         f"tx_drops {results['tx_drops_delta']:+d} "
         f"tx_timeout {results['tx_timeout_delta']:+d}"
         if after is not None else "no reply"),
    ]
    print()
    passed = True
    for label, ok, detail in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}: {detail}")
        passed = passed and ok
    results["passed"] = passed
    if args.json:
        with open(args.json, "w") as handle:
            json.dump(results, handle, indent=2)
        print(f"\nwrote {args.json}")
    return 0 if passed else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--read-seconds", type=float, default=5.0,
                        help="normal reading before the stall (default: 5)")
    parser.add_argument("--stall-seconds", type=float, default=15.0,
                        help="how long to stop reading bulk-IN for. Must be "
                             "comfortably longer than the firmware's 1 s "
                             "per-transfer timeout, so that several transfers "
                             "time out and not just one (default: 15)")
    parser.add_argument("--resume-timeout", type=float, default=2.0,
                        help="how long a request may take after the stall "
                             "before this is a failure (default: 2)")
    parser.add_argument("--after-seconds", type=float, default=5.0,
                        help="reading after the stall, to show the event flow "
                             "came back and not just the command path "
                             "(default: 5)")
    parser.add_argument(
        "--negative-control", action="store_true",
        help="invert the verdict: expect the assertions to FAIL. Run this "
             "against a build WITHOUT the bounded wait (the pre-W6 legacy "
             "image) - if the script passes there, it is not stalling "
             "anything and a pass on the fixed image proves nothing. Run the "
             "plain command on the USB_NEXT build too (next.conf;health.conf) "
             "as the cross-stack control: that stack has had a bounded wait "
             "since W1, so it must pass identically, and a difference between "
             "the two stacks is a finding in itself.")
    parser.add_argument("--serial",
                        help="match a device whose serial ends with this")
    parser.add_argument("--json", help="write the result here")
    suspend = parser.add_argument_group(
        "suspend test (Linux, root; replaces the stall test)")
    suspend.add_argument(
        "--suspend-cycles", type=suspend_cycles_arg, metavar="N",
        help="run N runtime-PM suspend/resume cycles instead of the stall "
             "test (40 covers the ~30-cycle leak on unfixed USB_NEXT "
             "firmware). Prints SKIP and exits 0 off Linux")
    suspend.add_argument("--autosuspend-ms", type=int, default=100,
                         help="power/autosuspend_delay_ms during the test "
                              "(default: 100)")
    suspend.add_argument("--suspend-timeout", type=float, default=5.0,
                         help="how long to wait for runtime_status="
                              "suspended per cycle (default: 5)")
    suspend.add_argument("--flood", type=int, default=64,
                         help="requests sent back to back after the cycles "
                              "(default: 64)")
    args = parser.parse_args()

    if args.suspend_cycles is not None:
        return run_suspend_cycles(args)

    # No --port here, deliberately. A UART build has no bulk-IN endpoint to
    # stop polling, so the condition this measures cannot exist on one, and
    # tx_drops/tx_timeout are structurally zero there (see ant_health.py).
    dev = open_device(True, serial=args.serial)
    reader = FrameReader(dev)

    if not reset_stack(dev, reader):
        sys.exit("No startup message after reset - is this the ANT firmware?")

    print("\nBaseline counters")
    before = read_health(dev, reader, "baseline")
    if before is None:
        sys.exit(
            "no usable 0xF6 reply. This needs "
            "CONFIG_ANT_DONGLE_HEALTH_COUNTERS=y (build with\n"
            "-DEXTRA_CONF_FILE=health.conf), and firmware new enough to send "
            "the 30-byte layout:\ntwo of the three assertions are counter "
            "reads, and without them a pass would\nonly mean the dongle was "
            "still answering."
        )
    print(f"  tx_drops={before['tx_drops']} "
          f"tx_timeout={before['tx_timeout']}")

    print("\nOpening a wildcard ANT+ receive channel (the load)")
    if not open_channel(dev, reader):
        return 2

    print(f"\nReading normally for {args.read_seconds:.0f} s")
    before_packets = read_for(reader, args.read_seconds)
    rate = before_packets / args.read_seconds if args.read_seconds else 0.0
    print(f"  {before_packets} packets ({rate:.1f}/s)")
    if before_packets == 0:
        sys.exit(
            "nothing is transmitting. The dongle has to be PRODUCING for the "
            "stall to mean\nanything - with an idle radio, not reading is "
            "indistinguishable from having\nnothing to read. Put an ANT+ "
            "master on the air (apps/sim on the L15, or any\nsensor) and run "
            "this again."
        )

    print(f"\nNOT READING for {args.stall_seconds:.0f} s "
          "(device stays configured)")
    # Just sleeping is the whole mechanism: with no transfer submitted, the
    # host controller issues no IN token, the dongle's endpoint stays full and
    # its bulk-IN transfer never completes. The device is never told any of
    # this - which is exactly the case a suspend/reset/unplug does NOT cover.
    time.sleep(args.stall_seconds)

    print("\nResuming")
    dev.write(EP_OUT, CAPABILITIES_REQUEST)
    round_trip = await_capabilities(reader, args.resume_timeout)
    if round_trip is None:
        print(f"  no reply within {args.resume_timeout:.1f} s")
    else:
        print(f"  capabilities reply in {round_trip * 1000:.1f} ms")

    after_packets = read_for(reader, args.after_seconds)
    print(f"  {after_packets} packets in the {args.after_seconds:.0f} s after")

    after = read_health(dev, reader, "after")
    if after is None:
        # Not "the feature is absent" - the baseline read proved it is there.
        # A dongle that answered 0xF6 before the stall and not after it is the
        # signature of the unfixed firmware: the bridge thread is parked in
        # usb_ant_write() holding tx_mutex, which is the bug itself.
        print("  no 0xF6 reply after the stall - the bridge is not answering")

    results = {
        "packets_before": before_packets,
        "packets_after": after_packets,
        "round_trip_s": round_trip,
        "counters_before": before,
        "counters_after": after,
    }

    def delta(field: str) -> int | None:
        if after is None or field not in after or field not in before:
            return None
        return after[field] - before[field]

    tx_timeout_delta = delta("tx_timeout")
    tx_drops_delta = delta("tx_drops")
    results["tx_timeout_delta"] = tx_timeout_delta
    results["tx_drops_delta"] = tx_drops_delta

    checks = [
        ("request round-trips after the stall",
         round_trip is not None and round_trip < args.resume_timeout,
         f"{round_trip * 1000:.1f} ms" if round_trip is not None
         else f"no reply in {args.resume_timeout:.1f} s"),
        ("tx_timeout climbed (the firmware abandoned a wait)",
         bool(tx_timeout_delta), f"+{tx_timeout_delta}"
         if tx_timeout_delta is not None else "unreadable"),
        ("tx_drops climbed (events were produced with nowhere to go)",
         bool(tx_drops_delta), f"+{tx_drops_delta}"
         if tx_drops_delta is not None else "unreadable"),
        ("event flow resumed",
         after_packets > 0, f"{after_packets} packets"),
    ]

    print("\nClosing")
    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL]),
            "close channel", timeout=3.0)
    close_device(dev)

    print()
    passed = True
    for name, ok, detail in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        passed = passed and ok

    results["passed"] = passed
    results["negative_control"] = args.negative_control

    if args.json:
        with open(args.json, "w") as handle:
            json.dump(results, handle, indent=2)
        print(f"\nwrote {args.json}")

    if args.negative_control:
        print()
        if passed:
            print("NEGATIVE CONTROL FAILED: every check passed on a build "
                  "that is supposed to\nlack the fix. The stall did not "
                  "reproduce, so a pass on the fixed image\nproves nothing. "
                  "Check that this really is the pre-fix image and that the "
                  "host\nstack is not reading bulk-IN behind this script's "
                  "back.")
            return 1
        print("negative control as expected: the unfixed image does not "
              "survive the stall.")
        return 0

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
