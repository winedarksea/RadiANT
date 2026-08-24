#!/usr/bin/env python3
"""Check that the dongle reports bulk-IN before bulk-OUT, and prove why it matters.

    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe tools\\ant_ep_order.py

WHY THIS EXISTS, AND WHY NO OTHER TOOL IN THIS DIRECTORY CATCHES IT.

A USB interface descriptor lists its endpoints in an order, and that order is
data the host can use. Two idioms exist for picking the pipes back out:

  by ADDRESS   - look at bEndpointAddress, take the one with bit 7 set as IN.
  by POSITION  - take endpoints[0] as IN and endpoints[1] as OUT.

Everything in tools/ uses the first idiom (ant_probe.py hardcodes EP_OUT = 0x01,
EP_IN = 0x81), and so does openant, which matches on usb.util.endpoint_direction.
So the entire Windows bench is blind to the order.

Plenty of real hosts use the second. node ant-plus - the library most
third-party ANT applications are built on - does exactly this:

    this.inEp  = this.iface.endpoints[0] as InEndpoint;
    this.outEp = this.iface.endpoints[1] as OutEndpoint;

and Android's UsbInterface.getEndpoint(0) is the same shape. That library works
against retail sticks, which is evidence that a retail ANT USB-m lists IN first.

A device that lists OUT first is therefore fully functional under one idiom and
completely mute under the other: a positional host writes its commands into the
IN pipe, polls the OUT pipe, and reads nothing forever. From the outside that is
indistinguishable from a dead dongle - it enumerates, it claims, it goes quiet -
which is exactly the symptom an ANT USB stick shows on Android when the ANT+ USB
service grants permission and then surfaces no sensors.

The two arms below are the point. Run both against one device:

  --by-address   the idiom our tools and openant use. Passes either way.
  --positional   the idiom ant-plus and Android use. Fails on an OUT-first device.

A split verdict IS the diagnosis, and it needs no phone to obtain.
"""
from __future__ import annotations

import argparse
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

from ant_wire import (  # noqa: E402
    MESG_CAPABILITIES_ID,
    MESG_REQUEST_ID,
    MESG_STARTUP_MESG_ID,
    MESG_SYSTEM_RESET_ID,
    MSG_OVERHEAD,
    SYNC_TX,
    frame,
    unframe,
)

VID = 0x0FCF
PIDS = (0x1009, 0x1008, 0x1004)


def find_devices(serial: str | None):
    found = []
    for dev in usb.core.find(find_all=True, idVendor=VID):
        if dev.idProduct not in PIDS:
            continue
        if serial:
            try:
                if (dev.serial_number or "").upper() != serial.upper():
                    continue
            except (usb.core.USBError, NotImplementedError):
                continue
        found.append(dev)
    return found


def describe(dev) -> tuple[list, str]:
    """Print the descriptor and return (endpoints in declared order, verdict)."""
    print(f"device {dev.idVendor:04x}:{dev.idProduct:04x} bcdDevice={dev.bcdDevice:04x}")
    try:
        print(f"  product={dev.product!r} serial={dev.serial_number!r}")
    except (usb.core.USBError, NotImplementedError) as exc:
        print(f"  strings unreadable: {exc}")

    cfg = dev.get_active_configuration()
    intf = cfg[(0, 0)]
    print(
        f"  interface {intf.bInterfaceNumber} class=0x{intf.bInterfaceClass:02x} "
        f"nEP={intf.bNumEndpoints}"
    )

    eps = list(intf)
    for i, ep in enumerate(eps):
        direction = "IN " if usb.util.endpoint_direction(ep.bEndpointAddress) else "OUT"
        print(
            f"    endpoints[{i}] addr=0x{ep.bEndpointAddress:02x} {direction} "
            f"mps={ep.wMaxPacketSize}"
        )

    if len(eps) < 2:
        return eps, "MALFORMED"
    first_is_in = bool(usb.util.endpoint_direction(eps[0].bEndpointAddress))
    verdict = "IN-first" if first_is_in else "OUT-first"
    print(f"    -> declared order is {verdict}")
    return eps, verdict


def read_frames(read_fn, deadline: float, want_id: int):
    """Reassemble ANT frames until `want_id` arrives or `deadline` passes."""
    buf = bytearray()
    while True:
        while buf and buf[0] != SYNC_TX:
            del buf[0]
        if len(buf) >= 2:
            total = buf[1] + MSG_OVERHEAD
            if len(buf) >= total:
                candidate = bytes(buf[:total])
                del buf[:total]
                parsed = unframe(candidate)
                if parsed is not None:
                    got_id, payload = parsed
                    if got_id == want_id:
                        return payload
                    print(f"    . unsolicited 0x{got_id:02X} {payload.hex()}")
                continue
        if time.monotonic() > deadline:
            return None
        try:
            data = read_fn()
        except usb.core.USBError as exc:
            # A read aimed at an OUT endpoint fails here rather than timing
            # out. That IS the failure being demonstrated, so report and stop
            # instead of retrying to the deadline.
            print(f"    ! read failed: {exc}")
            return None
        if data:
            buf.extend(bytes(data))


def handshake(write_fn, read_fn, label: str) -> bool:
    """Reset the stack, await startup, then request capabilities."""
    print(f"  [{label}] system reset ->")
    try:
        write_fn(frame(MESG_SYSTEM_RESET_ID, b"\x00"))
    except usb.core.USBError as exc:
        print(f"    ! write failed: {exc}")
        print(f"  [{label}] FAIL - could not transmit")
        return False

    startup = read_frames(read_fn, time.monotonic() + 3.0, MESG_STARTUP_MESG_ID)
    if startup is None:
        print(f"  [{label}] FAIL - no startup message (0x6F)")
        return False
    print(f"    startup 0x6F reason=0x{startup[0]:02X}")

    try:
        write_fn(frame(MESG_REQUEST_ID, bytes([0x00, MESG_CAPABILITIES_ID])))
    except usb.core.USBError as exc:
        print(f"    ! write failed: {exc}")
        print(f"  [{label}] FAIL - could not request capabilities")
        return False

    caps = read_frames(read_fn, time.monotonic() + 3.0, MESG_CAPABILITIES_ID)
    if caps is None:
        print(f"  [{label}] FAIL - no capabilities reply (0x54)")
        return False
    print(f"    capabilities channels={caps[0]} networks={caps[1]}")
    print(f"  [{label}] PASS")
    return True


def arm_by_address(dev, eps) -> bool:
    """The idiom our tools and openant use: pick pipes by direction."""
    ep_in = next(e for e in eps if usb.util.endpoint_direction(e.bEndpointAddress))
    ep_out = next(e for e in eps if not usb.util.endpoint_direction(e.bEndpointAddress))
    return handshake(
        lambda data: dev.write(ep_out.bEndpointAddress, data),
        lambda: dev.read(ep_in.bEndpointAddress, 64, timeout=500),
        "by-address",
    )


def arm_positional(dev, eps) -> bool:
    """The idiom ant-plus and Android use: endpoints[0] is IN, [1] is OUT."""
    ep_in, ep_out = eps[0], eps[1]
    print(
        f"  [positional] assuming IN=0x{ep_in.bEndpointAddress:02x} "
        f"OUT=0x{ep_out.bEndpointAddress:02x}"
    )
    return handshake(
        lambda data: dev.write(ep_out.bEndpointAddress, data),
        lambda: dev.read(ep_in.bEndpointAddress, 64, timeout=500),
        "positional",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", help="pick one dongle by USB serial number")
    parser.add_argument(
        "--by-address", action="store_true", help="run only the by-address arm"
    )
    parser.add_argument(
        "--positional", action="store_true", help="run only the positional arm"
    )
    parser.add_argument(
        "--descriptor-only",
        action="store_true",
        help="report the declared order and exit, opening no pipes",
    )
    args = parser.parse_args()

    devices = find_devices(args.serial)
    if not devices:
        print("no ANT dongle found (VID 0x0FCF)")
        return 2
    if len(devices) > 1 and not args.serial:
        print(f"{len(devices)} dongles attached; pass --serial to pick one")
        for dev in devices:
            describe(dev)
        return 2

    dev = devices[0]
    eps, verdict = describe(dev)
    if verdict == "MALFORMED":
        print("FAIL - interface does not expose two endpoints")
        return 1

    if args.descriptor_only:
        print()
        if verdict == "IN-first":
            print("PASS - matches the retail ANT USB-m order")
            return 0
        print("FAIL - OUT-first; positional hosts (ant-plus, Android) will read nothing")
        return 1

    dev.set_configuration()
    print()

    run_both = not (args.by_address or args.positional)
    results = {}
    if run_both or args.by_address:
        results["by-address"] = arm_by_address(dev, eps)
    if run_both or args.positional:
        results["positional"] = arm_positional(dev, eps)

    print()
    for name, ok in results.items():
        print(f"{name:12s} {'PASS' if ok else 'FAIL'}")

    if run_both and results["by-address"] and not results["positional"]:
        print()
        print("SPLIT VERDICT: the dongle works when pipes are chosen by address")
        print("and is mute when they are chosen by position. This is the Android")
        print("symptom reproduced on the bench - fix the descriptor order.")
        return 1

    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
