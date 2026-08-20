#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Find the CC26x2 ROM serial bootloader on an unknown CC2652P + CP2102N module.

READ-ONLY. Nothing here writes flash. It answers the three questions that have
to be answered before any flash tool is pointed at a board nobody has a
schematic for:

  1. Does the ROM bootloader backdoor respond at all?  If it does not, this
     module can only ever be programmed over cJTAG, and the nRF5340 DK's
     J-Link OB cannot speak cJTAG (measured: "cJTAG is not supported by
     connected J-Link").
  2. Which CP2102N modem line is wired to RESET and which to the backdoor DIO,
     and at what polarity?  Every vendor picks differently and the wrong guess
     is indistinguishable from a dead bootloader.
  3. What part is it?  CMD_GET_CHIP_ID names the die, which is how an
     unbranded AliExpress module gets identified without opening it.

The serial bootloader protocol is the one in TI's swru393 (CC2538) carried
forward to CC13x2/CC26x2:

    packet   = [len][checksum][payload...]      len counts len and checksum
    checksum = sum(payload) & 0xFF
    ACK      = 00 CC          NAK = 00 33
    sync     = 55 55          (the ROM auto-bauds off this)

Usage:
    python scripts/ti/bsl_probe.py --port COM15
    python scripts/ti/bsl_probe.py --port COM15 --listen-only
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - environment problem, not a code path
    sys.exit("pyserial missing. Use the NCS toolchain interpreter:\n"
             r"  C:\ncs\toolchains\fd21892d0f\opt\bin\python.exe")

ACK = b"\x00\xcc"
NAK = b"\x00\x33"

CMD_PING = 0x20
CMD_GET_STATUS = 0x23
CMD_GET_CHIP_ID = 0x28

# CMD_GET_STATUS return codes, swru393 Table 2-2.
STATUS = {
    0x40: "SUCCESS",
    0x41: "UNKNOWN_CMD",
    0x42: "INVALID_CMD",
    0x43: "INVALID_ADR",
    0x44: "FLASH_FAIL",
}

# Wafer IDs seen in the CC13x2/CC26x2 family. The chip ID word CMD_GET_CHIP_ID
# returns is the ICEPICK device ID; the interesting part is bits 28:12.
CHIP_IDS = {
    0xB964: "CC2652R / CC1352R (cc13x2_cc26x2)",
    0xB965: "CC2652P / CC1352P (cc13x2_cc26x2, PA)",
    0xB968: "CC2652RB",
    0xB97E: "CC2652P7 / CC1352P7 (cc13x2x7_cc26x2x7)",
}


def encode(payload: bytes) -> bytes:
    return bytes([len(payload) + 2, sum(payload) & 0xFF]) + payload


class Bsl:
    def __init__(self, port: str, baud: int, verbose: bool):
        self.sp = serial.Serial(port, baud, timeout=0.4, write_timeout=1.0)
        self.verbose = verbose

    def close(self) -> None:
        self.sp.close()

    # ── entry ───────────────────────────────────────────────────────────────
    #
    # The board is reset with one modem line while the other holds the
    # backdoor DIO at its active level. Which line is which, and which way
    # round the polarity runs, is the thing being searched for - so this takes
    # both as arguments rather than assuming cc2538-bsl's default wiring.
    def invoke(self, reset_line: str, active_high: bool) -> None:
        boot_line = "rts" if reset_line == "dtr" else "dtr"

        def set_line(name: str, value: bool) -> None:
            # pyserial's dtr/rts setters drive the RS-232 line; a CP2102N
            # inverts that to a logic level on its DTR#/RTS# pin, so "True"
            # here means the pin is pulled low. The polarity search covers it.
            setattr(self.sp, name, value)

        set_line(boot_line, active_high)
        set_line(reset_line, True)   # into reset
        time.sleep(0.05)
        set_line(reset_line, False)  # release; backdoor sampled here
        time.sleep(0.15)
        set_line(boot_line, not active_high)
        self.sp.reset_input_buffer()

    # ── framing ─────────────────────────────────────────────────────────────
    def sync(self) -> bool:
        self.sp.write(b"\x55\x55")
        self.sp.flush()
        return self._expect_ack()

    def _expect_ack(self) -> bool:
        # The ROM may prefix the ACK with nulls; scan a small window rather
        # than demanding the two bytes land alone.
        deadline = time.time() + 0.5
        buf = b""
        while time.time() < deadline:
            buf += self.sp.read(8)
            if ACK in buf:
                return True
            if NAK in buf:
                return False
        if self.verbose and buf:
            print(f"      (no ack; saw {buf.hex()})")
        return False

    def cmd(self, opcode: int, payload: bytes = b"") -> bytes | None:
        self.sp.write(encode(bytes([opcode]) + payload))
        self.sp.flush()
        if not self._expect_ack():
            return None
        return self._read_response()

    def _read_response(self) -> bytes:
        hdr = self.sp.read(2)
        if len(hdr) < 2 or hdr[0] < 2:
            return b""
        body = self.sp.read(hdr[0] - 2)
        self.sp.write(ACK)
        self.sp.flush()
        return body


def listen(port: str, seconds: float) -> None:
    """What, if anything, the factory image says on this UART."""
    for baud in (115200, 57600, 500000, 921600, 9600):
        with serial.Serial(port, baud, timeout=0.3) as sp:
            sp.dtr = False
            sp.rts = False
            sp.reset_input_buffer()
            deadline = time.time() + seconds
            data = b""
            while time.time() < deadline:
                data += sp.read(256)
        if data:
            print(f"  {baud:>6}: {len(data)} bytes  {data[:64].hex()}")
            try:
                print(f"          {data[:120].decode('utf-8', 'replace')!r}")
            except Exception:
                pass
        else:
            print(f"  {baud:>6}: silent")


def hold_sync(port: str, seconds: float, verbose: bool) -> int:
    """Spam the sync pattern across bauds while a human works the buttons.

    Separate from the modem-line search because it answers a different
    question. That search assumes DTR/RTS reach RESET and the backdoor DIO;
    on a module where they reach nothing, the only way in is a finger on a
    BOOT button, and then the tool's job is simply to be already talking when
    the part comes out of reset.
    """
    print(f"Spamming sync at 115200 for {seconds:.0f}s. ONE open port, and the")
    print("modem lines are left alone - reopening the port would pulse DTR/RTS")
    print("and on a board where those reach RESET that fights the human.")
    print("Hold BOOT, tap RESET, release BOOT - repeatedly, now.")
    bsl = Bsl(port, 115200, verbose)
    try:
        bsl.sp.dtr = False
        bsl.sp.rts = False
        deadline = time.time() + seconds
        n = 0
        while time.time() < deadline:
            if bsl.sync():
                print(f"\n\nSYNC after {n} attempts.")
                chip = bsl.cmd(CMD_GET_CHIP_ID)
                if chip:
                    word = int.from_bytes(chip[:4], "big")
                    wafer = (word >> 12) & 0xFFFF
                    print(f"  CHIP_ID 0x{word:08X}  wafer 0x{wafer:04X}"
                          f" -> {CHIP_IDS.get(wafer, 'UNKNOWN')}")
                return 0
            n += 1
            if n % 10 == 0:
                print(".", end="", flush=True)
    finally:
        bsl.close()
    print("\nNo sync.")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200,
                    help="sync baud; the ROM auto-bauds off 55 55")
    ap.add_argument("--listen-only", action="store_true")
    ap.add_argument("--listen-seconds", type=float, default=2.0)
    ap.add_argument("--hold", type=float, default=0.0, metavar="SECONDS",
                    help="Skip the modem-line search and just spam the sync "
                         "bytes across every plausible baud for this long, "
                         "for a module whose bootloader is entered by holding "
                         "a BOOT button and tapping RESET by hand.")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.hold:
        return hold_sync(args.port, args.hold, args.verbose)

    print(f"== listening on {args.port} (factory image?) ==")
    listen(args.port, args.listen_seconds)
    if args.listen_only:
        return 0

    print(f"\n== searching for the ROM bootloader at {args.baud} ==")
    attempts = [("none", None, None)]
    for reset_line in ("dtr", "rts"):
        for active_high in (True, False):
            attempts.append((f"reset={reset_line} boot_active={active_high}",
                             reset_line, active_high))

    for label, reset_line, active_high in attempts:
        bsl = Bsl(args.port, args.baud, args.verbose)
        try:
            if reset_line is not None:
                bsl.invoke(reset_line, active_high)
            else:
                bsl.sp.dtr = False
                bsl.sp.rts = False
                bsl.sp.reset_input_buffer()

            ok = bsl.sync()
            print(f"  {label:<34} sync {'ACK' if ok else '--'}")
            if not ok:
                continue

            if bsl.cmd(CMD_PING) is None:
                print("      PING not acked")
                continue

            status = bsl.cmd(CMD_GET_STATUS)
            if status:
                code = status[0]
                print(f"      STATUS  0x{code:02X} {STATUS.get(code, '?')}")

            chip = bsl.cmd(CMD_GET_CHIP_ID)
            if chip:
                word = int.from_bytes(chip[:4], "big")
                wafer = (word >> 12) & 0xFFFF
                print(f"      CHIP_ID 0x{word:08X}  wafer 0x{wafer:04X} "
                      f"-> {CHIP_IDS.get(wafer, 'UNKNOWN')}")

            print(f"\nBootloader reachable. Entry sequence: {label}")
            return 0
        finally:
            bsl.close()

    print("\nNo response from the ROM bootloader on any entry sequence.")
    print("That means one of: the backdoor is disabled in CCFG, the backdoor")
    print("DIO is on a button rather than a modem line, or the CP2102N's UART")
    print("is not wired to the CC2652P's ROM bootloader pins (DIO2/DIO3 by")
    print("default, but CCFG can move them).")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
