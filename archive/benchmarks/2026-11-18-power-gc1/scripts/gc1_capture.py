"""Capture a serial console with host timestamps. Optionally pulse nRESET via
xds110reset AFTER the port is open (DTR/RTS asserted), so the boot is caught."""
import subprocess
import sys
import time

import serial

port, baud, secs, out = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
reset = sys.argv[5] if len(sys.argv) > 5 else None

s = serial.Serial()
s.port = port
s.baudrate = baud
s.timeout = 0.5
s.dtr = True
s.rts = True
s.open()
t0 = time.time()
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write(f"# capture {port}@{baud} start {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t0))}\n")
    if reset:
        r = subprocess.run([reset, "-s", "L1100TY2"], capture_output=True, text=True)
        f.write(f"# xds110reset rc={r.returncode} at +{time.time() - t0:.3f}s\n")
    f.flush()
    buf = b""
    while time.time() - t0 < secs:
        chunk = s.read(4096)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            txt = line.decode("utf-8", "replace").rstrip("\r")
            f.write(f"{time.time() - t0:9.3f} {txt}\n")
            f.flush()
    if buf:
        f.write(f"{time.time() - t0:9.3f} {buf.decode('utf-8', 'replace')}\n")
    f.write(f"# end +{time.time() - t0:.3f}s\n")
s.close()
