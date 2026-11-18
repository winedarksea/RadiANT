#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Run one whole 0xF5 pairing exchange through a real dongle's serial bridge.

Security review 2026-11-17, item 1: the X25519 used to overrun the 2,048 B
serial bridge thread that runs 0xF5, and every test passed because every test
ran on a bigger stack. radiant/tests/api runs the exchange on a bridge-sized
thread, but on a ztest image; this runs it on the image itself, through USB
(or a UART with --port), parser and all, on whatever core the board has - the
point being a Cortex-M4 nRF52840, where a big frame can step over the 32 B MPU
guard instead of faulting.

Both ends are channels of the ONE dongle (0xF5 is host-driven: the keys,
commitment and nonces cross through this script, never the air), with the
RFC 7748 section 6.1 scalars and the nonces tools/test_radiant_crypto.py pins,
so every number has an independent expectation:

  * each SCALAR reply's public key is RFC 7748's for that scalar - the curve
    running on the target, checked against the RFC rather than against itself
  * both PEER_NONCE replies carry the same fingerprint, and it is the one
    tools/radiant_crypto.py computes from the public keys and nonces (362706)
  * no MESG_STARTUP (a reset) arrives at any point, and the device still
    answers afterwards, reporting one enrolment on each channel (0xF4)

Exits 0 when all of that holds. The stack high-water itself is logged by the
image (CONFIG_INIT_STACKS: "bridge: 0xF5 stack high-water N of 2048 bytes");
on a board with no console, build with the flash log (diag.conf's mechanism)
and read it back with scripts/read_flash_log.ps1.

    python tools/ant_pair_bench.py --serial EE6986854FD72803
"""

from __future__ import annotations

import argparse
import sys
import time

import ant_sec
import radiant_crypto
from ant_probe import EP_OUT, FrameReader, close_device, open_device, reset_stack
from ant_wire import (
    CHANNEL_TYPE_MASTER,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_CHANNEL_ID_ID,
    MESG_REQUEST_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_STARTUP_MESG_ID,
    MESG_UNASSIGN_CHANNEL_ID,
    RESPONSE_NO_ERROR,
    frame,
)

# RFC 7748 section 6.1: Alice (A, the initiator) and Bob (B, the responder).
SCALAR_A = bytes.fromhex(
    "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
PUB_A = bytes.fromhex(
    "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
SCALAR_B = bytes.fromhex(
    "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
PUB_B = bytes.fromhex(
    "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
NONCE_A = bytes(range(0x10, 0x20))
NONCE_B = bytes(range(0xA0, 0xB0))
FP_VECTOR = 362706

CH_A, CH_B = 0, 1
DEVNUM_A, DEVNUM_B = 0x3A17, 0x51C4
DEVTYPE, TRANS = 0x0B, 0x05


class Bench:
    def __init__(self, dev, reader, timeout: float):
        self.dev = dev
        self.reader = reader
        self.timeout = timeout
        self.resets = 0
        self.log: list[str] = []

    def _frames_until(self, want, timeout):
        """Next frame `want(id, body)` accepts; counts any startup message."""
        deadline = time.monotonic() + timeout
        while True:
            got = self.reader.next_frame(deadline)
            if got is None:
                return None
            msg_id, body = got
            if msg_id == MESG_STARTUP_MESG_ID:
                self.resets += 1
                self.log.append(f"MESG_STARTUP {bytes(body).hex()} - the "
                                f"dongle reset")
                continue
            if want(msg_id, body):
                return msg_id, bytes(body)

    def command(self, msg_id: int, payload: bytes) -> int | None:
        self.dev.write(EP_OUT, frame(msg_id, payload))
        got = self._frames_until(
            lambda i, b: i == MESG_RESPONSE_EVENT_ID and len(b) >= 3
            and b[1] == msg_id, self.timeout)
        return None if got is None else got[1][2]

    def pair(self, body: bytes, what: str) -> dict:
        """One 0xF5 sub-command; its decoded reply, or raises with the code."""
        t0 = time.monotonic()
        self.dev.write(EP_OUT, frame(ant_sec.MESG_PAIRING, body))
        # The bridge acknowledges every command with a response event as
        # well as sending the 0xF5 reply, so a NO_ERROR response is the ack
        # (of this command or the last) and only a nonzero one is a refusal.
        got = self._frames_until(
            lambda i, b: i == ant_sec.MESG_PAIRING or (
                i == MESG_RESPONSE_EVENT_ID and len(b) >= 3
                and b[1] == ant_sec.MESG_PAIRING
                and b[2] != RESPONSE_NO_ERROR), self.timeout)
        ms = (time.monotonic() - t0) * 1000.0
        if got is None:
            raise RuntimeError(f"{what}: no answer in {self.timeout} s "
                               f"(a hung or reset bridge)")
        msg_id, reply = got
        if msg_id == MESG_RESPONSE_EVENT_ID:
            raise RuntimeError(f"{what}: refused with code {reply[2]}")
        self.log.append(f"{what:<22} {ms:7.1f} ms")
        return ant_sec.decode_pair_reply(reply)

    def status(self, ch: int):
        self.dev.write(EP_OUT, frame(MESG_REQUEST_ID,
                                     bytes([ch, ant_sec.MESG_STATUS])))
        got = self._frames_until(lambda i, b: i == ant_sec.MESG_STATUS,
                                 self.timeout)
        return None if got is None else ant_sec.decode_status(got[1])


def run(bench: Bench) -> list[str]:
    """The exchange. Returns the failed checks (empty means PASS)."""
    fails = []
    for ch, devnum in ((CH_A, DEVNUM_A), (CH_B, DEVNUM_B)):
        code = bench.command(MESG_ASSIGN_CHANNEL_ID,
                             bytes([ch, CHANNEL_TYPE_MASTER, 0]))
        if code != RESPONSE_NO_ERROR:
            return [f"assign channel {ch}: code {code}"]
        code = bench.command(MESG_CHANNEL_ID_ID,
                             bytes([ch, devnum & 0xFF, devnum >> 8, DEVTYPE,
                                    TRANS]))
        if code != RESPONSE_NO_ERROR:
            return [f"channel id {ch}: code {code}"]

    bench.pair(ant_sec.encode_pair_enter(CH_A), "A ENTER")
    bench.pair(ant_sec.encode_pair_enter(CH_B), "B ENTER")
    pub_a = bench.pair(ant_sec.encode_pair_scalar(CH_A, SCALAR_A),
                       "A SCALAR")["public_key"]
    pub_b = bench.pair(ant_sec.encode_pair_scalar(CH_B, SCALAR_B),
                       "B SCALAR")["public_key"]
    if pub_a != PUB_A:
        fails.append(f"A's public key {pub_a.hex()} is not RFC 7748's")
    if pub_b != PUB_B:
        fails.append(f"B's public key {pub_b.hex()} is not RFC 7748's")
    bench.pair(ant_sec.encode_pair_exchange(CH_A, pub_b), "A EXCHANGE")
    bench.pair(ant_sec.encode_pair_exchange(CH_B, pub_a), "B EXCHANGE")
    cb = bench.pair(ant_sec.encode_pair_nonce(
        CH_B, ant_sec.PAIR_ROLE_RESPONDER, NONCE_B), "B NONCE")["commit"]
    if cb != radiant_crypto.pair_f4(pub_b, pub_a, NONCE_B):
        fails.append("B's commitment is not f4(PKb, PKa, Nb)")
    bench.pair(ant_sec.encode_pair_nonce(
        CH_A, ant_sec.PAIR_ROLE_INITIATOR, NONCE_A), "A NONCE")
    bench.pair(ant_sec.encode_pair_peer_commit(CH_A, cb), "A PEER_COMMIT")
    fp_b = bench.pair(ant_sec.encode_pair_peer_nonce(CH_B, NONCE_A),
                      "B PEER_NONCE")["fingerprint"]
    fp_a = bench.pair(ant_sec.encode_pair_peer_nonce(CH_A, NONCE_B),
                      "A PEER_NONCE")["fingerprint"]
    want = radiant_crypto.pair_g2(pub_a, pub_b, NONCE_A, NONCE_B)
    bench.log.append(f"fingerprints A {fp_a:06d}, B {fp_b:06d}, "
                     f"radiant_crypto.py {want:06d}")
    if fp_a != fp_b:
        fails.append(f"the two ends show different digits: {fp_a} / {fp_b}")
    if fp_a != want or want != FP_VECTOR:
        fails.append(f"fingerprint {fp_a} is not the expected {want} "
                     f"(vector {FP_VECTOR})")

    for ch in (CH_A, CH_B):
        st = bench.status(ch)
        if st is None:
            fails.append(f"no 0xF4 status from channel {ch} after the exchange")
        elif st.enrolments != 1:
            fails.append(f"channel {ch} reports {st.enrolments} enrolments, "
                         f"want 1")
        bench.pair(ant_sec.encode_pair_leave(ch), f"{'AB'[ch]} LEAVE")
        bench.command(MESG_UNASSIGN_CHANNEL_ID, bytes([ch]))
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--serial", help="USB serial of the dongle to use")
    ap.add_argument("--port", help="UART transport instead of USB")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="seconds to wait for each answer (an X25519 on a "
                    "64 MHz Cortex-M4 takes under one)")
    args = ap.parse_args()

    dev = open_device(True, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)
    try:
        if not reset_stack(dev, reader):
            print("FAIL: no startup message after a system reset")
            return 1
        bench = Bench(dev, reader, args.timeout)
        try:
            fails = run(bench)
        except RuntimeError as err:
            fails = [str(err)]
        if bench.resets:
            fails.append(f"{bench.resets} reset(s) during the exchange")
        for line in bench.log:
            print(f"  {line}")
    finally:
        close_device(dev)

    for f in fails:
        print(f"FAIL: {f}")
    print("PASS" if not fails else "FAIL")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
