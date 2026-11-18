import sys, time, serial
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = True
end = time.time() + secs
with open(out, 'w', encoding='utf-8', errors='replace') as f:
    buf = b''
    while time.time() < end:
        buf += s.read(4096)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            f.write('%.3f %s\n' % (time.time(), line.decode('ascii', 'replace').rstrip('\r')))
        f.flush()
