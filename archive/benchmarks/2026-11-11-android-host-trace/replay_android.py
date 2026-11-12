"""Replay one cycle of the Android ANT service's command sequence (captured
2026-11-11 from the nRF5340 DK trace) against a stick, printing every reply.

usage: replay_android.py SERIAL
"""
import sys
import time

sys.path.insert(0, r"C:\Users\Colin\ant_dongle\tools")
import ant_probe as P  # noqa: E402
from ant_wire import frame  # noqa: E402

KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])

SEQ = [
    ("reset", 0x4A, b"\x00"),
    ("netkey 1", 0x46, b"\x01" + KEY),
    ("req version", 0x4D, b"\x00\x3e"),
    ("req caps", 0x4D, b"\x00\x54"),
    ("adv burst cfg", 0x78, bytes.fromhex("0001030000000100 00".replace(" ", ""))[:9]),
    ("evbuf enable", 0x74, bytes.fromhex("0000ffffc800")),
    ("evbuf disable", 0x74, bytes.fromhex("000000000000")),
    ("req status", 0x4D, b"\x00\x52"),
    ("assign", 0x42, bytes.fromhex("00000100")),
    ("hp search 0", 0x44, b"\x00\x00"),
    ("lp search 6", 0x63, b"\x00\x06"),
    ("prox", 0x71, bytes(5)),
    ("search prio 7", 0x75, b"\x00\x07"),
    ("idlist excl 0", 0x5A, b"\x00\x00\x01"),
    ("libcfg 0x40", 0x6E, b"\x00\x40"),
    ("chan id wild", 0x51, bytes(5)),
    ("rf 57", 0x45, b"\x00\x39"),
    ("period 8192", 0x43, b"\x00\x00\x20"),
    ("prox", 0x71, bytes(5)),
    ("lp search 4", 0x63, b"\x00\x04"),
    ("hp search 0", 0x44, b"\x00\x00"),
    ("open", 0x4B, b"\x00"),
    ("WAIT", None, 2.0),
    ("idlist incl 0 (OPEN)", 0x5A, b"\x00\x00\x00"),
    ("idlist add (OPEN)", 0x59, bytes.fromhex("003412780100")),
    ("idlist excl 1 (OPEN)", 0x5A, b"\x00\x01\x01"),
    ("req status", 0x4D, b"\x00\x52"),
    ("req evbuf cfg", 0x4D, b"\x00\x74"),
    ("close", 0x4C, b"\x00"),
    ("WAIT", None, 0.5),
    ("unassign", 0x41, b"\x00"),
    ("libcfg 0", 0x6E, b"\x00\x00"),
    ("evbuf disable", 0x74, bytes(6)),
    ("reset", 0x4A, b"\x00"),
]


def drain(reader, secs):
    end = time.monotonic() + secs
    out = []
    while True:
        r = reader.next_frame(end)
        if r is None:
            return out
        out.append(r)


def main():
    dev = P.open_device(True, serial=sys.argv[1])
    reader = P.FrameReader(dev, timeout_ms=1000)
    drain(reader, 0.3)
    for name, mid, body in SEQ:
        if mid is None:
            for r in drain(reader, body):
                print(f"   ~ unsolicited 0x{r[0]:02X} {bytes(r[1]).hex(' ')}")
            continue
        dev.write(P.EP_OUT, frame(mid, body))
        got = drain(reader, 0.35 if mid != 0x4A else 0.8)
        txt = "; ".join(f"0x{i:02X} {bytes(p).hex(' ')}" for i, p in got) or "(nothing)"
        print(f"{name:24s} H> {mid:02X} {body.hex(' '):30s} <D {txt}")
    P.close_device(dev)


main()
