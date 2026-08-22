#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Read - and, carefully, rewrite - the CP2102N's stored USB VID/PID.

WHY THIS EXISTS. The CC2652P stick reaches the host through a CP2102N, and
Zwift's ANT_DLL only accepts vendor IDs 0x0FCF and 0x1915 (see
`docs/ti-dongle-usb-identity.md`). Changing the stored VID is therefore the
first move in making the stick visible to Zwift - and it is a move that takes
away the way back: once the IDs change, `silabser.sys` no longer binds, COM15
disappears, and the Silicon Labs customisation utility can no longer see the
chip to put it right. This tool is the escape hatch, because whatever binds the
device afterwards, if it is `libusb0` or WinUSB then raw control transfers still
reach the configuration block.

    python tools/cp2102n_ids.py                      # find it, dump the IDs
    python tools/cp2102n_ids.py --dump               # whole block + checks
    python tools/cp2102n_ids.py --set-vid 0x0FCF --set-pid 0x1008 --commit

THE SAFETY RULE, AND IT IS NOT OPTIONAL. The configuration block ends in a
Fletcher checksum. Write a block whose checksum is wrong and the part can stop
enumerating, which on this board means no USB path at all - only the JTAG rig
still reaches the CC2652P. So `--commit` refuses to run until the checksum
routine here has REPRODUCED the checksum already stored in the block it just
read. That is a real test: if this file's idea of the algorithm, the field
offsets or the block length is wrong in any way, the recomputation misses and
nothing is written. Without `--commit` the tool never writes at all.

Protocol (CP2102N; the classic CP2102 is different and is NOT handled here):

    read   bmRequestType 0xC0, bRequest 0xFF, wValue 0x000E, wIndex 0
    write  bmRequestType 0x40, bRequest 0xFF, wValue 0x370F, wIndex 0

The block begins configSize:u16, configVersion:u8, enableBootloader:u8,
enableConfigUpdate:u8, then the device descriptor - so the VID sits at offset
13 and the PID at 15, both little-endian.
"""

from __future__ import annotations

import argparse
import sys

try:
    import usb.core
    import usb.util
except ImportError:                     # pragma: no cover - user guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

REQ_READ = dict(bmRequestType=0xC0, bRequest=0xFF, wValue=0x000E, wIndex=0)
REQ_WRITE = dict(bmRequestType=0x40, bRequest=0xFF, wValue=0x370F, wIndex=0)

OFF_CONFIG_SIZE = 0
OFF_VID = 13
OFF_PID = 15

# The block is a fixed size per part, and asking for the wrong length simply
# fails rather than returning something short - so try the documented sizes
# rather than assuming one.
CANDIDATE_LENGTHS = (0x02A6, 0x02A7, 0x02A5, 0x0400)

# Parts that might answer. The stock identity first, then the ones this project
# programs, so the tool still finds the chip after a change.
CANDIDATE_IDS = ((0x10C4, 0xEA60), (0x0FCF, 0x1008), (0x0FCF, 0x1009),
                 (0x0FCF, 0x1004))


def fletcher16(data: bytes) -> int:
    """Fletcher-16 over the block, as the CP2102N config stores it."""
    s1 = s2 = 0
    for byte in data:
        s1 = (s1 + byte) % 255
        s2 = (s2 + s1) % 255
    return (s2 << 8) | s1


def fletcher16_alt(data: bytes) -> int:
    """The 256-modulus variant, tried only to identify which one matches."""
    s1 = s2 = 0
    for byte in data:
        s1 = (s1 + byte) & 0xFF
        s2 = (s2 + s1) & 0xFF
    return (s2 << 8) | s1


CHECKSUM_VARIANTS = (
    ("fletcher16 mod 255, stored big-endian", fletcher16, "big"),
    ("fletcher16 mod 255, stored little-endian", fletcher16, "little"),
    ("fletcher16 mod 256, stored big-endian", fletcher16_alt, "big"),
    ("fletcher16 mod 256, stored little-endian", fletcher16_alt, "little"),
)


def find_device(vid=None, pid=None):
    if vid is not None and pid is not None:
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is None:
            sys.exit("No device at %04X:%04X." % (vid, pid))
        return dev
    for cand_vid, cand_pid in CANDIDATE_IDS:
        dev = usb.core.find(idVendor=cand_vid, idProduct=cand_pid)
        if dev is not None:
            return dev
    sys.exit(
        "No CP2102N found at any of: "
        + ", ".join("%04X:%04X" % ids for ids in CANDIDATE_IDS)
        + ".\nIf it is on COM15 under silabser.sys, raw control transfers are\n"
        "not available - that is the VCP driver, and this tool needs libusb0\n"
        "or WinUSB bound to the device. Use the Silicon Labs utility instead.")


def read_config(dev):
    """Return (block, length) or exit explaining why it could not be read."""
    last = None
    for length in CANDIDATE_LENGTHS:
        try:
            data = dev.ctrl_transfer(data_or_wLength=length, **REQ_READ)
        except usb.core.USBError as exc:
            last = exc
            continue
        block = bytes(data)
        if len(block) < 32:
            continue
        declared = int.from_bytes(block[OFF_CONFIG_SIZE:OFF_CONFIG_SIZE + 2],
                                  "little")
        # The block says how big it is; agreement is what tells us the read
        # length was right rather than merely accepted.
        if declared in (len(block), len(block) - 2):
            return block, length
        last = "read %d bytes but the block declares configSize=%d" % (
            len(block), declared)
    sys.exit("Could not read the configuration block (%s)." % (last,))


def identify_checksum(block):
    """Which checksum variant reproduces the stored value? None if no match."""
    stored_be = int.from_bytes(block[-2:], "big")
    stored_le = int.from_bytes(block[-2:], "little")
    body = block[:-2]
    for name, fn, order in CHECKSUM_VARIANTS:
        stored = stored_be if order == "big" else stored_le
        if fn(body) == stored:
            return name, fn, order
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--vid", type=lambda s: int(s, 0),
                    help="find the device at this vendor ID")
    ap.add_argument("--pid", type=lambda s: int(s, 0),
                    help="find the device at this product ID")
    ap.add_argument("--set-vid", type=lambda s: int(s, 0),
                    help="new vendor ID to store")
    ap.add_argument("--set-pid", type=lambda s: int(s, 0),
                    help="new product ID to store")
    ap.add_argument("--commit", action="store_true",
                    help="actually write. Without this nothing is written.")
    ap.add_argument("--dump", action="store_true",
                    help="hex dump the whole configuration block")
    args = ap.parse_args()

    dev = find_device(args.vid, args.pid)
    print("found %04X:%04X" % (dev.idVendor, dev.idProduct))
    try:
        print("  manufacturer: %r" % usb.util.get_string(dev, dev.iManufacturer))
        print("  product:      %r" % usb.util.get_string(dev, dev.iProduct))
        print("  serial:       %r" % usb.util.get_string(dev, dev.iSerialNumber))
    except Exception as exc:            # pragma: no cover - informational only
        print("  (string descriptors unreadable: %r)" % exc)

    block, length = read_config(dev)
    declared = int.from_bytes(block[:2], "little")
    stored_vid = int.from_bytes(block[OFF_VID:OFF_VID + 2], "little")
    stored_pid = int.from_bytes(block[OFF_PID:OFF_PID + 2], "little")
    print("\nconfiguration block: %d bytes read, configSize field says %d"
          % (len(block), declared))
    print("  stored VID at offset %d: %04X" % (OFF_VID, stored_vid))
    print("  stored PID at offset %d: %04X" % (OFF_PID, stored_pid))

    # THE decisive cross-check. If the offsets were wrong, these would not
    # agree with what the device actually enumerated as.
    agree = (stored_vid == dev.idVendor and stored_pid == dev.idProduct)
    print("  agrees with the live descriptors: %s" % ("YES" if agree else "NO"))
    if not agree:
        print("  -> refusing to trust these offsets. Do not write.")

    match = identify_checksum(block)
    if match:
        print("  checksum reproduced by: %s" % match[0])
    else:
        print("  checksum NOT reproduced by any known variant")

    if args.dump:
        print("\n--- block ---")
        for i in range(0, len(block), 16):
            row = block[i:i + 16]
            print("  %04X  %-47s  %s"
                  % (i, " ".join("%02x" % b for b in row),
                     "".join(chr(b) if 32 <= b < 127 else "." for b in row)))

    if args.set_vid is None and args.set_pid is None:
        return 0

    new_vid = args.set_vid if args.set_vid is not None else stored_vid
    new_pid = args.set_pid if args.set_pid is not None else stored_pid
    print("\nrequested: %04X:%04X -> %04X:%04X"
          % (stored_vid, stored_pid, new_vid, new_pid))

    if not agree or not match:
        print("REFUSING: this tool has not proved it understands the block.")
        print("Both of these must hold before any write is allowed:")
        print("  * the stored IDs match what the device enumerated as")
        print("  * the stored checksum is reproducible here")
        print("Writing without that risks a part that no longer enumerates,")
        print("and this board has no USB path back - only the JTAG rig.")
        return 2

    name, fn, order = match
    new = bytearray(block)
    new[OFF_VID:OFF_VID + 2] = new_vid.to_bytes(2, "little")
    new[OFF_PID:OFF_PID + 2] = new_pid.to_bytes(2, "little")
    new[-2:] = fn(bytes(new[:-2])).to_bytes(2, order)
    print("recomputed checksum with: %s" % name)

    if not args.commit:
        print("\nDRY RUN - nothing written. Re-run with --commit to apply.")
        return 0

    try:
        sent = dev.ctrl_transfer(data_or_wLength=bytes(new), **REQ_WRITE)
    except usb.core.USBError as exc:
        print("WRITE FAILED: %r" % exc)
        return 1
    print("wrote %d bytes." % sent)

    # A stored identity only takes effect at the next enumeration, and this
    # dongle is usually not somewhere convenient to unplug. A USB port reset
    # re-enumerates it in place. It legitimately throws afterwards, because the
    # handle refers to a device that has just gone away and come back as a
    # different one - so that is success, not failure.
    try:
        dev.reset()
        print("issued a USB reset; the dongle should re-enumerate as %04X:%04X"
              % (new_vid, new_pid))
    except usb.core.USBError:
        print("issued a USB reset (the handle went away, which is expected)")
    print("If it does not appear with the new identity, replug it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
