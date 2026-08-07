#!/usr/bin/env python3
"""Probe an ANT+ USB dongle over its raw bulk endpoints.

Speaks the ANT serial protocol directly with pyusb, so it works identically on
Windows (libusb-win32, the driver the README tells you to install), Linux
(libusb + the shipped udev rule) and macOS. It is the primary bring-up tool for
this firmware: there is no J-Link on the development machine, so RTT logs are
unreadable and this is how the board reports for duty.

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
# Every ANT stick Dynastream shipped, newest first. This firmware presents as
# an ANT USB-m; the others are here so the tools keep working against a real
# stick, and across a rebuild that changes which one is impersonated.
PIDS = (0x1009, 0x1008, 0x1004)
PID = PIDS[0]
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
    """Wrap a message in the ANT serial frame: SYNC LEN ID payload XOR.

    The checksum covers the SYNC byte too. Omitting it produces a value 0x A4
    off, which a real ANT host rejects - and which this file and the firmware
    once got wrong in the same direction, so they agreed with each other and
    with nothing else.
    """
    out = bytes([SYNC, len(payload), msg_id]) + payload
    checksum = 0
    for byte in out:
        checksum ^= byte
    return out + bytes([checksum])


class FrameReader:
    """Reassembles ANT frames from bulk-IN packets."""

    def __init__(self, dev, timeout_ms: int):
        self.dev = dev
        self.timeout_ms = timeout_ms
        self.buf = bytearray()

    def _fill(self) -> bool:
        try:
            data = self.dev.read(EP_IN, 64, timeout=self.timeout_ms)
        except usb.core.USBTimeoutError:
            # Expected whenever the dongle has nothing to say. Matching on the
            # message text instead would work on Linux and not on Windows,
            # where it reads "Operation timed out".
            return False
        except usb.core.USBError as exc:
            if getattr(exc, "errno", None) == 110:
                return False
            # libusb0 (libusb-win32) does not raise USBTimeoutError and leaves
            # errno as None, so a plain idle read looks like a hard failure and
            # takes the tool down mid-listen. Its message is the only thing
            # left to go on: "[_usb_reap_async] timeout error".
            if "timeout" in str(exc).lower():
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
                    for byte in candidate[:-1]:   # SYNC included - see frame()
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


def reset_stack(dev, reader: FrameReader, timeout_s: float = 3.0) -> bool:
    """Reset the ANT stack and wait for the startup message.

    Every host application opens a session with this, and channels left behind
    by the previous one are why: assigning a channel that is still assigned is
    refused with CHANNEL_IN_WRONG_STATE, so a tool that skips the reset
    inherits whatever the last tool forgot to clean up.
    """
    dev.write(EP_OUT, frame(MESG_SYSTEM_RESET_ID, b"\x00"))
    return expect(reader, MESG_STARTUP_ID, timeout_s) is not None


def _serial_of(dev):
    try:
        return usb.util.get_string(dev, dev.iSerialNumber)
    except (usb.core.USBError, ValueError):
        return None


def _pip_libusb1():
    """The libusb-1.0 DLL from `pip install libusb`, if that package is here.

    Linux and macOS ship libusb where pyusb already looks. Windows does not, so
    `pip install libusb` is the least painful way to get one - but that package
    only unpacks the DLL, it does not put it anywhere pyusb searches. Handing
    the path over explicitly is what makes `pip install pyusb libusb` enough to
    run this on Windows.
    """
    import os

    import usb.backend.libusb1

    try:
        import libusb
    except ImportError:
        return None

    root = os.path.join(os.path.dirname(libusb.__file__), "_platform", "_windows")
    arch = "x64" if sys.maxsize > 2**32 else "x86"
    dll = os.path.join(root, arch, "libusb-1.0.dll")
    if not os.path.exists(dll):
        return None

    return usb.backend.libusb1.get_backend(find_library=lambda _: dll)


def _backends():
    """Every libusb backend that loads here, best first.

    libusb0 matters as much as libusb1 on Windows: the driver this project
    tells you to install is libusb-win32, because that is the one Zwift's
    ANT_DLL can talk to (see the README). A device bound to it is invisible to
    the libusb-1.0 backend, so trying only libusb1 makes the tools report "no
    libusb backend" on precisely the setup they are meant to test. Both are
    tried, and open_device() searches all of them before giving up.
    """
    import usb.backend.libusb0
    import usb.backend.libusb1

    candidates = [usb.backend.libusb1.get_backend(),
                  usb.backend.libusb0.get_backend(),
                  _pip_libusb1()]
    return [b for b in candidates if b is not None]


def open_device(verbose: bool, wait_s: float = 0.0, serial: str | None = None):
    backends = _backends()
    if not backends:
        sys.exit(
            "No libusb backend available.\n"
            "  Windows: install libusb-win32 (see the README), or pip install libusb\n"
            "  Linux:   apt install libusb-1.0-0 (or your distro's equivalent)\n"
            "  macOS:   brew install libusb"
        )

    # Poll rather than sleep-then-look. How long a re-enumeration takes is the
    # host's business, not ours: Windows has to rediscover the device and rebind
    # its driver, which is comfortably slower than the device itself reboots.
    deadline = time.monotonic() + wait_s
    while True:
        # First backend that sees anything wins. Concatenating them instead
        # would list one dongle twice if two backends can both reach it, which
        # reads as "two dongles attached" and stops the tool.
        found = []
        for backend in backends:
            found = [d for pid in PIDS
                     for d in usb.core.find(idVendor=VID, idProduct=pid,
                                            find_all=True, backend=backend)]
            if found:
                break
        if serial is not None:
            found = [d for d in found
                     if _serial_of(d) and _serial_of(d).endswith(serial)]
        dev = found[0] if found else None

        if len(found) > 1:
            sys.exit(
                f"{len(found)} ANT devices are attached. Pick one with "
                "--serial:\n  " +
                "\n  ".join(str(_serial_of(d)) for d in found)
            )
        if dev is not None or time.monotonic() >= deadline:
            break
        time.sleep(0.25)

    if dev is None:
        sys.exit(
            f"No ANT device found ({VID:04X}:"
            + "/".join(f"{p:04X}" for p in PIDS) + ").\n"
            "  Windows: check Device Manager shows it under 'libusb-win32 "
            "devices' bound to libusb0 (see the README).\n"
            "  Linux:   install 99-ant-usb.rules (see host/linux/) and replug.\n"
            "  macOS:   nothing extra needed; try a different cable."
        )

    if verbose:
        print(f"Found {dev.idVendor:04X}:{dev.idProduct:04X}  "
              f"bcdDevice=0x{dev.bcdDevice:04X}")
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
    parser.add_argument(
        "--serial",
        help="match a device whose serial ends with this, for when more "
             "than one dongle is attached",
    )
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()
    verbose = not args.quiet

    dev = open_device(verbose, serial=args.serial)
    reader = FrameReader(dev, timeout_ms=250)

    failures = []

    print("\n[1/3] reset")
    dev.write(EP_OUT, frame(MESG_SYSTEM_RESET_ID, b"\x00"))
    payload = expect(reader, MESG_STARTUP_ID, args.timeout)
    if payload is None:
        print("  FAIL: no startup message (0x6F)")
        failures.append("startup")
    else:
        reason = payload[0] if payload else 0
        print(f"  OK: startup, reason 0x{reason:02X} "
              f"({STARTUP_REASONS.get(reason, 'unknown')})")

    # A reset resets the ANT stack, not the device: the handle stays valid, the
    # same way it does for a real stick. If this ever needs a re-open, the
    # firmware is rebooting when it should not be.

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
