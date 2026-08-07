#!/usr/bin/env python3
"""Probe an ANT+ USB dongle over its raw bulk endpoints.

Speaks the ANT serial protocol directly with pyusb, so it works identically on
Windows (WinUSB), Linux (libusb + the shipped udev rule) and macOS. It is the
primary bring-up tool for this firmware: there is no J-Link on the development
machine, so RTT logs are unreadable and this is how the board reports for duty.

    pip install pyusb
    python tools/ant_probe.py

Sequence: reset -> expect startup (0x6F) -> request capabilities (0x54) ->
request version (0x3E). Exit status is 0 only if every step succeeded.
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

VID = 0x0FCF
PID = 0x1008
EP_OUT = 0x01
EP_IN = 0x81

SYNC = 0xA4

MESG_SYSTEM_RESET_ID = 0x4A
MESG_REQUEST_ID = 0x4D
MESG_STARTUP_ID = 0x6F
MESG_CAPABILITIES_ID = 0x54
MESG_VERSION_ID = 0x3E
MESG_RESPONSE_EVENT_ID = 0x40

STARTUP_REASONS = {
    0x00: "power-on / command reset",
    0x01: "hardware reset line",
    0x02: "watchdog reset",
    0x20: "command reset",
    0x40: "synchronous reset",
    0x80: "suspend reset",
}


def frame(msg_id: int, payload: bytes = b"") -> bytes:
    """Wrap a message in the ANT serial frame: SYNC LEN ID payload XOR."""
    body = bytes([len(payload), msg_id]) + payload
    checksum = 0
    for byte in body:
        checksum ^= byte
    return bytes([SYNC]) + body + bytes([checksum])


class FrameReader:
    """Reassembles ANT frames from bulk-IN packets."""

    def __init__(self, dev, timeout_ms: int):
        self.dev = dev
        self.timeout_ms = timeout_ms
        self.buf = bytearray()

    def _fill(self) -> bool:
        try:
            data = self.dev.read(EP_IN, 64, timeout=self.timeout_ms)
        except usb.core.USBError as exc:
            if "timeout" in str(exc).lower() or getattr(exc, "errno", None) == 110:
                return False
            raise
        self.buf.extend(bytes(data))
        return True

    def next_frame(self, deadline: float):
        """Return (msg_id, payload) or None once `deadline` passes."""
        while True:
            # Resynchronise on SYNC.
            while self.buf and self.buf[0] != SYNC:
                del self.buf[0]

            if len(self.buf) >= 2:
                length = self.buf[1]
                total = length + 4
                if len(self.buf) >= total:
                    candidate = bytes(self.buf[:total])
                    del self.buf[:total]

                    checksum = 0
                    for byte in candidate[1:-1]:
                        checksum ^= byte
                    if checksum != candidate[-1]:
                        print(f"  ! checksum mismatch on {candidate.hex()}, dropping")
                        continue
                    return candidate[2], candidate[3:-1]

            if time.monotonic() > deadline:
                return None
            self._fill()


def expect(reader: FrameReader, msg_id: int, timeout_s: float):
    """Read frames until one with `msg_id` arrives, or time out."""
    deadline = time.monotonic() + timeout_s
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return None
        got_id, payload = result
        if got_id == msg_id:
            return payload
        if got_id == MESG_RESPONSE_EVENT_ID and len(payload) >= 3:
            print(
                f"  . response ch={payload[0]} to 0x{payload[1]:02X} "
                f"code={payload[2]}"
            )
        else:
            print(f"  . unsolicited 0x{got_id:02X} {payload.hex()}")


def open_device(verbose: bool):
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit(
            f"No USB device {VID:04X}:{PID:04X} found.\n"
            "  Windows: check Device Manager shows it under 'Universal Serial "
            "Bus devices' bound to winusb.sys.\n"
            "  Linux:   install 99-ant-usb.rules (see host/linux/) and replug.\n"
            "  macOS:   nothing extra needed; try a different cable."
        )

    if verbose:
        print(f"Found {VID:04X}:{PID:04X}  bcdDevice=0x{dev.bcdDevice:04X}")
        try:
            print(f"  manufacturer: {usb.util.get_string(dev, dev.iManufacturer)}")
            print(f"  product:      {usb.util.get_string(dev, dev.iProduct)}")
            print(f"  serial:       {usb.util.get_string(dev, dev.iSerialNumber)}")
        except (usb.core.USBError, ValueError) as exc:
            print(f"  (string descriptors unavailable: {exc})")

    # Linux only: the kernel does not claim a vendor interface, but detach
    # defensively in case something else did.
    if hasattr(dev, "is_kernel_driver_active"):
        try:
            if dev.is_kernel_driver_active(0):
                dev.detach_kernel_driver(0)
        except (NotImplementedError, usb.core.USBError):
            pass

    try:
        dev.set_configuration()
    except usb.core.USBError as exc:
        # WinUSB has already configured the device and refuses a redundant
        # SET_CONFIGURATION; the handle is still usable, so carry on.
        if verbose:
            print(f"  (set_configuration skipped: {exc})")

    return dev


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--timeout", type=float, default=3.0,
        help="seconds to wait for each reply (default: 3)",
    )
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()
    verbose = not args.quiet

    dev = open_device(verbose)
    reader = FrameReader(dev, timeout_ms=250)

    failures = []

    print("\n[1/3] reset")
    dev.write(EP_OUT, frame(MESG_SYSTEM_RESET_ID, b"\x00"))
    payload = expect(reader, MESG_STARTUP_ID, args.timeout)
    if payload is None:
        # A reset makes the firmware reboot, so the handle is stale either way.
        print("  FAIL: no startup message (0x6F)")
        failures.append("startup")
    else:
        reason = payload[0] if payload else 0
        print(f"  OK: startup, reason 0x{reason:02X} "
              f"({STARTUP_REASONS.get(reason, 'unknown')})")

    # The device reboots on reset, so re-open before continuing.
    usb.util.dispose_resources(dev)
    time.sleep(1.5)
    dev = open_device(False)
    reader = FrameReader(dev, timeout_ms=250)

    print("\n[2/3] request capabilities")
    dev.write(EP_OUT, frame(MESG_REQUEST_ID, bytes([0x00, MESG_CAPABILITIES_ID])))
    payload = expect(reader, MESG_CAPABILITIES_ID, args.timeout)
    if payload is None or len(payload) < 2:
        print("  FAIL: no capabilities message (0x54)")
        failures.append("capabilities")
    else:
        print(f"  OK: {payload.hex()}")
        print(f"      max channels: {payload[0]}, max networks: {payload[1]}")

    print("\n[3/3] request version")
    dev.write(EP_OUT, frame(MESG_REQUEST_ID, bytes([0x00, MESG_VERSION_ID])))
    payload = expect(reader, MESG_VERSION_ID, args.timeout)
    if payload is None:
        print("  FAIL: no version message (0x3E)")
        failures.append("version")
    else:
        text = payload.split(b"\x00")[0].decode("ascii", errors="replace")
        print(f"  OK: {text!r}")
        if text.startswith("STUB"):
            print("      (this is a stub build — the ANT radio is not present)")

    usb.util.dispose_resources(dev)

    print()
    if failures:
        print(f"FAILED: {', '.join(failures)}")
        return 1
    print("PASS: reset -> startup -> capabilities -> version")
    return 0


if __name__ == "__main__":
    sys.exit(main())
