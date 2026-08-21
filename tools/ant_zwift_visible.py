#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Ask Zwift's own ANT_DLL.dll whether it can see a dongle.

`ant_probe.py` answers "is the firmware alive". This answers a different and
much narrower question: **would Zwift find this dongle?** They are not the same
question, and on the CC2652P stick they have opposite answers - `ant_probe.py`
passes over COM15 while Zwift sees nothing at all.

The only trustworthy way to ask is to ask the binary Zwift asks. This loads
that exact DLL, drives the same enumeration entry points, and calls the same
`ANT_Init` Zwift calls. Anything else - reading INF files, comparing VID/PIDs
by eye - is inference, and inference is what cost this project two sessions.

    python tools/ant_zwift_visible.py
    python tools/ant_zwift_visible.py --dll "D:/Games/Zwift/ANT_DLL.dll"

Exit status is 0 only if ANT_DLL enumerated at least one device AND ANT_Init
succeeded, i.e. only if Zwift would work.

WHAT THIS TOOL MEASURED, so the next person need not re-derive it
-----------------------------------------------------------------
ANT_DLL (ALU3.200) carries three transports, visible in its RTTI:

  USBDeviceLibusb + LibusbLibrary   raw bulk through libusb0.dll
  USBDeviceSI     + SiLabsLibrary   Silicon Labs USBXpress (SI_Open/SI_Read/..)
  DSISerialVCP                      a COM port, opened as "\\\\.\\COM%u"

Only the first two are reachable from an application. Selecting the VCP
transport needs `ANT_InitExt`'s ucPortType argument, and **`ANT_InitExt` is not
exported** - the export table has `ANT_Init` and nothing else that takes a port
type. So "just put it on a COM port" cannot work however the dongle enumerates.

And the acceptance test is on the VENDOR ID ONLY. Two helpers do it:

    call [rax]          ; GetVid()
    mov  ecx, 0FCFh     ; Dynastream
    cmp  ax, cx
    je   accept
    mov  ecx, 1915h     ; Nordic Semiconductor
    cmp  ax, cx
    je   accept
    xor  eax, eax       ; reject

There is no product-ID comparison anywhere in the binary. That is what makes
the CP2102N impersonation tractable: the PID is free, so it can be chosen to
avoid colliding with the libusb0 binding that the nRF52840 sticks rely on.
"""

from __future__ import annotations

import argparse
import ctypes as C
import glob
import os
import pathlib
import sys

DEFAULT_DLL = r"C:\Program Files (x86)\Zwift\ANT_DLL.dll"

# The three backends, and where each is loaded from. ANT_DLL statically imports
# only KERNEL32 and USER32 - every one of these is reached through LoadLibrary,
# so a missing one fails SOFTLY and simply removes that transport. That is why
# Zwift runs happily with two of the three absent, and why "it found nothing"
# never points at the cause on its own.
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
    ap.add_argument("--logdir", help="collect ANT_DLL's own debug log here")
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

    print("\n--- devices ANT_DLL can enumerate ---")
    found = enumerate_devices(lib)
    for d in found:
        vid = d["vid"]
        note = ""
        if vid is not None:
            note = "  <- %s, accepted" % ACCEPTED_VIDS[vid] \
                if vid in ACCEPTED_VIDS else "  <- VID not accepted by ANT_DLL"
        print("  device %d: VID=%s PID=%s product=%r serial=%r%s"
              % (d["num"],
                 "%04X" % vid if vid is not None else "-",
                 "%04X" % d["pid"] if d["pid"] is not None else "-",
                 d["product"], d["serial"], note))
    if not found:
        print("  (none - ANT_DLL sees no ANT dongle, so neither would Zwift)")

    print("\n--- ANT_Init(%d, %d) : the call Zwift makes ---"
          % (args.device, args.baud))
    lib.ANT_Init.restype = C.c_bool
    lib.ANT_Init.argtypes = [C.c_ubyte, C.c_ulong]
    init = False
    try:
        init = bool(lib.ANT_Init(args.device, args.baud))
        print("  ANT_Init -> %s" % init)
        if init:
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
    if found and init:
        print("PASS: Zwift would find and open this dongle.")
        return 0
    if found and not init:
        print("PARTIAL: ANT_DLL enumerated a device but could not open it.")
        print("A CP210x reached through the raw-bulk backend fails exactly")
        print("here: the chip's endpoints are 0x02/0x82 and that path drives")
        print("0x01/0x81. Check which backend bound the device.")
        return 1
    print("FAIL: no dongle visible to Zwift.")
    print("If a dongle is plugged in and ant_probe.py passes on it, then it is")
    print("enumerating with a vendor ID ANT_DLL does not accept - it takes")
    print("0FCF (Dynastream) or 1915 (Nordic) and ignores the product ID.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
