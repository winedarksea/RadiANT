#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

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

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

# Every protocol constant, and the framing rule itself, comes from the
# generated module. It is produced from protocol/ant_wire.yaml alongside
# src/ant_wire.h, so the firmware and the tools cannot disagree about a value
# without the generator's --check failing first.
#
# The framing rule matters more than the constants. This file used to carry its
# own frame(), and the firmware carried a second one, and both left the SYNC
# byte out of the checksum - so they agreed with each other, disagreed with
# every real ANT host, and cost a week. There is now exactly one implementation
# of that rule in Python and one in C, generated from the same description.
#
# The names are the bare sdk-ant spellings. The ANTW_ prefix in the C header
# exists only to dodge a preprocessor collision with sdk-ant's own macros;
# Python has module namespaces and needs no such thing.
from ant_wire import (  # noqa: E402
    MESG_CAPABILITIES_ID,
    MESG_REQUEST_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_STARTUP_MESG_ID,
    MESG_SYSTEM_RESET_ID,
    MESG_VERSION_ID,
    MSG_OVERHEAD,
    STARTUP_REASONS_BY_VALUE,
    SYNC_TX,
    frame,
    unframe,
)

VID = 0x0FCF
# Every ANT stick Dynastream shipped, newest first. This firmware presents as
# an ANT USB-m; the others are here so the tools keep working against a real
# stick, and across a rebuild that changes which one is impersonated.
PIDS = (0x1009, 0x1008, 0x1004)
PID = PIDS[0]
EP_OUT = 0x01
EP_IN = 0x81

# How long a bulk-IN read waits before giving up, and why it is not 250 ms.
#
# libusb-win32 cancels the pending transfer when a read times out, and a cancel
# landing at the same moment the dongle is answering loses that answer: the host
# never sees the frame, and the dongle - which handed it to a USB stack that
# reported success - has no idea anything went missing.
#
# Every tool here used 250 ms against an ANT+ channel period of 249.7 ms, so
# essentially every read was racing the packet it was waiting for. It cost about
# 0.4 percentage points of apparent packet loss and several ms of apparent
# jitter, none of it real, and it was invisible because the invented losses look
# exactly like on-air ones. What gives it away is that the radio reports a
# RX_FAIL for a packet lost on the air and nothing at all for one lost here, so
# a run that loses more than it can account for is losing it locally.
#
# The value only has to be comfortably clear of the slowest period a tool waits
# on; nothing wants it short. It bounds how long a tool waits on a device that
# is not going to answer, and no more.
READ_TIMEOUT_MS = 1000


class FrameReader:
    """Reassembles ANT frames from bulk-IN packets."""

    def __init__(self, dev, timeout_ms: int = READ_TIMEOUT_MS):
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
            # Resynchronise on SYNC. unframe() validates the byte again rather
            # than trusting it, but it does not resynchronise - finding the
            # start of a frame in a stream is this buffer's job and not its.
            while self.buf and self.buf[0] != SYNC_TX:
                del self.buf[0]

            if len(self.buf) >= 2:
                length = self.buf[1]
                total = length + MSG_OVERHEAD
                if len(self.buf) >= total:
                    candidate = bytes(self.buf[:total])
                    del self.buf[:total]

                    # The checksum rule - SYNC included in the XOR - lives in
                    # ant_wire.unframe() and nowhere else here.
                    parsed = unframe(candidate)
                    if parsed is None:
                        print(f"  ! checksum mismatch on {candidate.hex()}, dropping")
                        continue
                    return parsed

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
    return expect(reader, MESG_STARTUP_MESG_ID, timeout_s) is not None


class SerialDevice:
    """A serial port wearing just enough of the pyusb device API.

    Builds for parts with no USB peripheral put the ANT serial protocol on a
    UART instead (CONFIG_ANT_DONGLE_TRANSPORT_UART) - the nRF54L15 has no USB
    device controller in silicon, so that is the only way to talk to one. The
    bytes are identical either way, which is the whole point: a USB ANT stick
    is a UART bridge with the UART hidden inside.

    Rather than fork every tool, this presents the four calls they make of a
    pyusb device - read, write, set_configuration, and the string lookups - so
    `--port COM8` works everywhere `--serial` does.
    """

    def __init__(self, port: str, baud: int):
        try:
            import serial
        except ImportError:  # pragma: no cover - user-facing guidance
            sys.exit("pyserial is not installed. Run: pip install pyserial")

        self._port = serial.Serial(port, baud, timeout=0)
        self.name = port
        # The tools print these; a UART carries no descriptors, so report the
        # identity the firmware would have presented over USB.
        self.idVendor = VID
        self.idProduct = PID
        self.bcdDevice = 0
        self.iManufacturer = 0
        self.iProduct = 0
        self.iSerialNumber = 0

    def write(self, endpoint, data, timeout=None):
        return self._port.write(bytes(data))

    def read(self, endpoint, size, timeout=None):
        """Return as soon as ANY bytes have arrived, never after `size` of them.

        A bulk-IN read hands back one packet the moment the endpoint has one,
        and `size` is the caller's buffer bound rather than an amount to wait
        for. `serial.Serial.read(size)` is the opposite: it blocks until it has
        collected exactly `size` bytes or the timeout expires.

        That difference is not cosmetic, and it invented a firmware bug. An
        EVENT_TX frame is 7 bytes and a master raises one every 249.7 ms, so a
        read for 64 bytes could never be satisfied by the traffic it was
        watching - every read ran the full 1000 ms and then delivered four
        events at once. `ant_sim.py` paces from EVENT_TX and falls back to the
        wall clock after two silent message periods, so it reported ~1 fallback
        a second and blamed the dongle's event filter, while the radio log
        showed every slot armed and transmitted on time. Same shape as the
        FrameReader timeout bug in docs/testing.md: the instrument, not the
        thing measured.

        So: block for the first byte, then take whatever else has already
        arrived and return. That is the bulk-IN semantics the tools were
        written against.
        """
        self._port.timeout = (timeout or 0) / 1000.0
        data = self._port.read(1)
        if not data:
            # FrameReader treats this as "nothing to say yet", the same as an
            # idle bulk-IN endpoint. Returning empty would spin instead.
            raise usb.core.USBTimeoutError("timeout", None, None)
        pending = min(size - 1, self._port.in_waiting)
        if pending > 0:
            data += self._port.read(pending)
        return data

    def set_configuration(self):
        pass

    def close(self):
        self._port.close()


def close_device(dev) -> None:
    """Release whichever kind of device open_device() handed back."""
    if isinstance(dev, SerialDevice):
        dev.close()
    else:
        usb.util.dispose_resources(dev)


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


def open_device(verbose: bool, wait_s: float = 0.0, serial: str | None = None,
                port: str | None = None, baud: int = 115200):
    if port is not None:
        dev = SerialDevice(port, baud)
        if verbose:
            print(f"Using {port} at {baud} baud (UART transport)")
        return dev

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
    parser.add_argument(
        "--port",
        help="talk to a UART build over this serial port (e.g. COM8, "
             "/dev/ttyACM1) instead of over USB",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()
    verbose = not args.quiet

    dev = open_device(verbose, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)

    failures = []

    print("\n[1/3] reset")
    dev.write(EP_OUT, frame(MESG_SYSTEM_RESET_ID, b"\x00"))
    payload = expect(reader, MESG_STARTUP_MESG_ID, args.timeout)
    if payload is None:
        print("  FAIL: no startup message (0x6F)")
        failures.append("startup")
    else:
        reason = payload[0] if payload else 0
        # The generated name rather than a prose gloss. The table this used to
        # carry described 0x00 as "power-on / command reset" and 0x20 as
        # "command reset", which is two names for one thing and one name for
        # two; STARTUP_POWER_ON_RESET and STARTUP_COMMAND_RESET are what the
        # protocol calls them and what src/ant_wire.h says as well.
        print(f"  OK: startup, reason 0x{reason:02X} "
              f"({STARTUP_REASONS_BY_VALUE.get(reason, 'unknown')})")

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

    close_device(dev)

    print()
    if failures:
        print(f"FAILED: {', '.join(failures)}")
        return 1
    print("PASS: reset -> startup -> capabilities -> version")
    return 0


if __name__ == "__main__":
    sys.exit(main())
