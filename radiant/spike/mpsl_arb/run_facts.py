# SPDX-License-Identifier: Apache-2.0
#
# Provenance: clean-room. Host driver for the mpsl_arb spike's ADR 0018
# Phase 0c facts (commands a, b, c). pyserial only; nothing here derives from
# sdk-ant.
#
# Finds the console port by J-Link serial number and USB interface (MI_xx),
# never by a remembered COM number, asserts DTR (the nRF54L15 DK's log VCOM
# hands over one chunk and goes silent without it), sends one command letter
# per fact and logs every line with a host timestamp until the fact's END
# line, an MPSL assert, or a timeout.
#
#   python run_facts.py --jlink 1057737173 --mi 2 --cmds abc \
#       --out C:\adr18_logs\phase-0c\none_run1.log

import argparse
import sys
import time

import serial
import serial.tools.list_ports as list_ports

END_FOR = {"a": "F1|END", "b": "F2|END", "c": "F3|END"}


def find_port(jlink, mi):
    want = "SER=%s" % jlink.lstrip("0")
    hits = []
    for p in list_ports.comports():
        hwid = p.hwid or ""
        ser = ""
        for tok in hwid.split():
            if tok.startswith("SER="):
                ser = "SER=" + tok[4:].lstrip("0")
        if ser != want:
            continue
        loc = ""
        for tok in hwid.split():
            if tok.startswith("LOCATION="):
                loc = tok
        hits.append((p.device, loc))
    # pyserial reports LOCATION=...:x.<interface> for some interfaces only;
    # an interface without one is matched by elimination.
    for dev, loc in hits:
        if loc.endswith(":x.%d" % mi):
            return dev
    if mi == 0:
        rest = [d for d, loc in hits if not loc]
        if len(rest) == 1:
            return rest[0]
    raise SystemExit("no port for J-Link %s MI_%02d among %r" % (jlink, mi, hits))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jlink", required=True)
    ap.add_argument("--mi", type=int, default=2)
    ap.add_argument("--port")
    ap.add_argument("--cmds", default="abc")
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=900.0)
    a = ap.parse_args()

    port = a.port or find_port(a.jlink, a.mi)
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = True
    s.rts = True
    s.open()

    out = open(a.out, "a", encoding="utf-8", newline="\n")

    def log(line):
        stamp = time.strftime("%Y-%m-%dT%H:%M:%S")
        out.write("%s %s\n" % (stamp, line))
        out.flush()
        print(line)
        sys.stdout.flush()

    log("# host: port=%s jlink=%s mi=%d cmds=%s" % (port, a.jlink, a.mi, a.cmds))
    buf = b""

    def read_lines():
        nonlocal buf
        chunk = s.read(s.in_waiting or 1)
        if not chunk:
            return []
        buf += chunk
        parts = buf.split(b"\n")
        buf = parts[-1]
        return [p.decode("utf-8", "replace").rstrip("\r") for p in parts[:-1]]

    # Liveness first: the help text proves the right port and a live image.
    s.write(b"?")
    t0 = time.time()
    alive = False
    while time.time() - t0 < 5:
        for ln in read_lines():
            log(ln)
            if "commands:" in ln:
                alive = True
        if alive:
            break
    if not alive:
        log("# host: no help text in 5 s - wrong port or dead image")
        return 2

    rc = 0
    for c in a.cmds:
        log("# host: send %r" % c)
        s.write(c.encode())
        t0 = time.time()
        done = False
        while time.time() - t0 < a.timeout and not done:
            for ln in read_lines():
                log(ln)
                if END_FOR.get(c) and ln.strip() == END_FOR[c]:
                    done = True
                if "MPSL ASSERT" in ln:
                    log("# host: assert during %r - waiting for reboot" % c)
                    rc = 3
                    t1 = time.time()
                    booted = False
                    while time.time() - t1 < 20 and not booted:
                        for l2 in read_lines():
                            log(l2)
                            if "commands:" in l2:
                                booted = True
                    done = True
                    break
        if not done:
            log("# host: timeout on %r" % c)
            rc = 4
        time.sleep(0.5)
    s.close()
    out.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
