"""Capture the Pro Micro hr_transmitter CDC console (DTR asserted) with host timestamps.
Usage: python b5_cdc_capture.py COMx SECONDS OUTFILE"""
import sys, time
import serial

port, secs, outp = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial()
s.port = port; s.baudrate = 115200; s.timeout = 0.5; s.dtr = True; s.rts = True
s.open()
end = time.time() + secs
buf = b""
with open(outp, "a", buffering=1, encoding="utf-8") as f:
    f.write(f"# capture {port} start {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    while time.time() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            f.write(f"{time.time():.3f} {line.decode('utf-8', 'replace').rstrip()}\n")
    if buf:
        f.write(f"{time.time():.3f} {buf.decode('utf-8', 'replace').rstrip()}\n")
s.close()
