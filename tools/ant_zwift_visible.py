#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Ask Zwift's own ANT_DLL.dll whether it can see a dongle.

`ant_probe.py` answers "is the firmware alive". This answers a different and
much narrower question: **would Zwift find this dongle?** They are not the same
question, and on the CC2652P stick they have opposite answers - `ant_probe.py`
passes over COM15 while Zwift sees nothing at all.

The only trustworthy way to ask is to ask the binary Zwift asks. This loads
that exact DLL, drives the same enumeration entry points, and calls the same
`ANT_Init` Zwift calls.

    python tools/ant_zwift_visible.py
    python tools/ant_zwift_visible.py --dll "D:/Games/Zwift/ANT_DLL.dll"

Exit status is 0 only if ANT_DLL enumerated at least one device AND ANT_Init
succeeded, i.e. only if Zwift would work.

The trap: as of 2026-08-21 this tool has never returned PASS, so it has no
positive control. A FALSE from `ANT_Init` has two readings that cannot yet be
told apart: "Zwift could not see the dongle", or "this harness does not set
something up the way Zwift does". **Run it once with a known-good nRF52840
stick attached and confirm it PASSes; until then, treat every FAIL here as
unproven.**

ANT_DLL's acceptance test is on the VENDOR ID ONLY - `0x0FCF` or `0x1915`,
with no product-ID comparison anywhere in the binary (full disassembly
findings in `docs/ti-dongle-usb-identity.md`). That is what makes the CP2102N
impersonation tractable: the PID is free, so it can be chosen to avoid
colliding with the libusb0 binding the nRF52840 sticks rely on.
"""

from __future__ import annotations

import argparse
import ctypes as C
import glob
import os
import pathlib
import sys
import time

DEFAULT_DLL = r"C:\Program Files (x86)\Zwift\ANT_DLL.dll"

# Every backend is reached through LoadLibrary (ANT_DLL statically imports
# only KERNEL32/USER32), so a missing one fails SOFTLY and simply removes
# that transport - which is why "it found nothing" never points at the cause
# on its own.
BACKENDS = (
    ("libusb0.dll", "raw bulk (nRF52840 sticks, 0FCF:1008/1009)"),
    ("DSI_SiUSBXp_3_1.DLL", "Silicon Labs USBXpress (CP210x-based sticks)"),
    ("DSI_CP210xManufacturing_3_1.dll", "reads a CP210x's VID/PID for the above"),
)

ACCEPTED_VIDS = {0x0FCF: "Dynastream", 0x1915: "Nordic Semiconductor"}


def load(dll_path: str):
    """Load ANT_DLL with the search path Zwift.exe would give it."""
    root = os.path.dirname(dll_path)
    for d in (root, os.path.join(root, "Windows ANT Dongle Driver", "amd64")):
        if os.path.isdir(d):
            try:
                os.add_dll_directory(d)
            except OSError:
                pass
    if os.path.isdir(root):
        os.chdir(root)
    return C.WinDLL(dll_path)


def report_backends(root: str) -> None:
    print("backend DLLs (all LoadLibrary'd, so a missing one just removes that")
    print("transport rather than failing loudly):")
    search = [root, os.path.join(root, "Windows ANT Dongle Driver", "amd64"),
              r"C:\Windows\System32"]
    for name, what in BACKENDS:
        where = next((os.path.join(d, name) for d in search
                      if os.path.exists(os.path.join(d, name))), None)
        mark = "present" if where else "ABSENT "
        print("  %s  %-32s %s" % (mark, name, what))
        if where:
            print("            %s" % where)


def enumerate_devices(lib, limit: int = 8):
    lib.ANT_GetDeviceUSBVID.restype = C.c_bool
    lib.ANT_GetDeviceUSBVID.argtypes = [C.c_ubyte, C.POINTER(C.c_ushort)]
    lib.ANT_GetDeviceUSBPID.restype = C.c_bool
    lib.ANT_GetDeviceUSBPID.argtypes = [C.c_ubyte, C.POINTER(C.c_ushort)]
    lib.ANT_GetDeviceUSBInfo.restype = C.c_bool
    lib.ANT_GetDeviceUSBInfo.argtypes = [C.c_ubyte, C.c_char_p, C.c_char_p]

    found = []
    for num in range(limit):
        vid = C.c_ushort(0)
        pid = C.c_ushort(0)
        got_vid = lib.ANT_GetDeviceUSBVID(num, C.byref(vid))
        got_pid = lib.ANT_GetDeviceUSBPID(num, C.byref(pid))
        product = C.create_string_buffer(256)
        serial = C.create_string_buffer(256)
        got_info = lib.ANT_GetDeviceUSBInfo(num, product, serial)
        if not (got_vid or got_pid or got_info):
            continue
        found.append({
            "num": num,
            "vid": vid.value if got_vid else None,
            "pid": pid.value if got_pid else None,
            "product": product.value.decode("latin1", "replace") if got_info else "",
            "serial": serial.value.decode("latin1", "replace") if got_info else "",
        })
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dll", default=DEFAULT_DLL,
                    help="path to ANT_DLL.dll (default: Zwift's)")
    ap.add_argument("--device", type=int, default=0,
                    help="device number to pass to ANT_Init (default 0)")
    ap.add_argument("--baud", type=int, default=57600)
    ap.add_argument("--port-type", type=int, default=0, choices=(0, 1),
                    help="0 = USB (what Zwift uses), 1 = COM port. On the COM "
                         "path the device number IS the COM port number.")
    ap.add_argument("--logdir", help="collect ANT_DLL's own debug log here")
    ap.add_argument("--enumerate-usb", action="store_true",
                    help="also call ANT_GetDeviceUSB* after opening. Off by "
                         "default: it faults on the USBXpress/COM transport, "
                         "which has no USB descriptor behind it.")
    args = ap.parse_args()

    if os.name != "nt":
        print("ANT_DLL.dll is a Windows DLL; this tool only runs on Windows.")
        return 2
    if not os.path.exists(args.dll):
        print("ANT_DLL.dll not found at %s\nPass --dll with the real path."
              % args.dll)
        return 2

    root = os.path.dirname(args.dll)
    report_backends(root)
    print()

    lib = load(args.dll)
    lib.ANT_LibVersion.restype = C.c_char_p
    try:
        print("ANT_LibVersion: %s" % lib.ANT_LibVersion().decode("latin1"))
    except Exception as exc:            # pragma: no cover - defensive
        print("ANT_LibVersion failed: %r" % exc)

    logdir = None
    if args.logdir:
        logdir = pathlib.Path(args.logdir)
        logdir.mkdir(parents=True, exist_ok=True)
        for stale in logdir.glob("*"):
            if stale.is_file():
                stale.unlink()
        lib.ANT_SetDebugLogDirectory.restype = C.c_bool
        lib.ANT_SetDebugLogDirectory.argtypes = [C.c_char_p]
        ok = lib.ANT_SetDebugLogDirectory(str(logdir).encode("latin1"))
        print("ANT_SetDebugLogDirectory -> %s" % ok)

    print("\n--- ANT_Init(%d, %d, port_type=%d, framer=0) ---"
          % (args.device, args.baud, args.port_type))
    # FOUR arguments, not two. The export named ANT_Init is really ANT_InitExt:
    # its prologue reads cl, edx, r8b and r9b -
    #     movzx ebp, cl    ; device number
    #     mov   esi, edx   ; baud rate
    #     movzx ebx, r8b   ; port type   0 = USB, 1 = COM
    #     movzx edi, r9b   ; framer type 0 = basic
    # Calling it with two arguments leaves r8/r9 holding garbage and it fails
    # without a single I/O operation - reads exactly like "no dongle" and isn't.
    lib.ANT_Init.restype = C.c_bool
    lib.ANT_Init.argtypes = [C.c_ubyte, C.c_ulong, C.c_ubyte, C.c_ubyte]
    init = False
    found = []
    try:
        init = bool(lib.ANT_Init(args.device, args.baud, args.port_type, 0))
        print("  ANT_Init -> %s" % init)
        if init:
            reset = getattr(lib, "ANT_ResetSystem", None)
            if reset is not None:
                reset.restype = C.c_bool
                print("  ANT_ResetSystem -> %s" % bool(reset()))
                time.sleep(1.0)

            # AFTER the open, never before: ANT_GetDeviceUSB* report on a
            # device that is already open, so enumerating first returns
            # nothing.
            #
            # Off by default: it FAULTS on the USBXpress/COM transport (no
            # USB descriptor behind it, so ANT_DLL null-derefs writing the
            # result - "access violation writing 0x0"), and a crash here
            # would skip ANT_Close and leave the port open.
            if args.enumerate_usb:
                print("\n--- what ANT_DLL says it opened ---")
                try:
                    found = enumerate_devices(lib)
                except OSError as exc:
                    print("  faulted: %s" % exc)
                    print("  (expected on the USBXpress/COM transport)")
                for d in found:
                    vid = d["vid"]
                    note = ""
                    if vid is not None:
                        note = ("  <- %s, accepted" % ACCEPTED_VIDS[vid]
                                if vid in ACCEPTED_VIDS
                                else "  <- VID not accepted by ANT_DLL")
                    print("  device %d: VID=%s PID=%s product=%r serial=%r%s"
                          % (d["num"],
                             "%04X" % vid if vid is not None else "-",
                             "%04X" % d["pid"] if d["pid"] is not None else "-",
                             d["product"], d["serial"], note))

            lib.ANT_Close.restype = None
            lib.ANT_Close()
            print("  ANT_Close -> done")
    except Exception as exc:            # pragma: no cover - defensive
        print("  ANT_Init raised %r" % exc)

    if logdir:
        print("\n--- ANT_DLL's own debug log ---")
        files = sorted(glob.glob(str(logdir / "*")))
        if not files:
            print("  (library wrote nothing)")
        for f in files:
            print("  == %s ==" % os.path.basename(f))
            try:
                with open(f, "r", errors="replace") as fh:
                    for line in fh.read().splitlines()[:80]:
                        print("     %s" % line)
            except OSError as exc:
                print("     unreadable: %r" % exc)

    print()
    # Opening is the whole test. A device reached over the USBXpress/COM
    # transport has no USB identity to report, so requiring enumeration as well
    # would fail a dongle that demonstrably works.
    if init:
        print("PASS: Zwift would find and open this dongle.")
        return 0
    print("FAIL: no dongle visible to Zwift.")
    print("Checks worth making, in order:")
    print("  * is the dongle on its COM port, and does ant_probe.py pass?")
    print("  * is something else holding that port open (ant_scan, a capture)?")
    print("  * for a USB stick, does it enumerate with vendor 0FCF or 1915?")
    print("    ANT_DLL accepts only those two and ignores the product ID.")
    print("  * for the CC2652P stick, are the USBXpress shims installed?")
    print("    scripts\\zwift_ant_shim.ps1")
    return 1


if __name__ == "__main__":
    sys.exit(main())
