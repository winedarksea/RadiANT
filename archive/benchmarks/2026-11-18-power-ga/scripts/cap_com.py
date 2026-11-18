import sys, time, serial
port, seconds, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = True
s.rts = True
end = time.time() + seconds
buf = b''
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    while time.time() < end:
        buf += s.read(512)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            t = time.time()
            f.write('%.3f %s\n' % (t, line.decode('utf-8', 'replace').rstrip('\r')))
            f.flush()
    if buf:
        f.write('%.3f %s\n' % (time.time(), buf.decode('utf-8', 'replace')))
s.close()
