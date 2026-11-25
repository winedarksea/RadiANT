import sys, time, argparse
sys.path.insert(0, r"C:\Users\Colin\ant_dongle\tools")
import ant_prox_onair as T
from ant_probe import EP_OUT, FrameReader, close_device
from ant_wire import frame

a = argparse.Namespace(dev_type=120, trans_type=0, rf=57, period=8070)
dev = T.open_dev(None, "006", 0)
rd = FrameReader(dev)
def run(label, ext, prox, n=6, secs=4.0):
    T.setup(dev, rd, "garmin", 0x00, 0, a, ext=ext)
    T.cmd(dev, rd, 0x44, bytes([0, 2]), "search timeout")
    if prox is not None:
        code = T.cmd(dev, rd, 0x71, bytes([0]) + bytes(prox), "0x71", raise_=False)
        print(f"  0x71 {bytes(prox).hex()} -> {code}")
    dev.write(EP_OUT, frame(0x4B, b"\x00"))
    end = time.monotonic() + secs; got = 0
    print(f"== {label}: lib config 0x{ext:02x}, prox {prox}")
    while time.monotonic() < end and got < n:
        r = rd.next_frame(end)
        if r is None: continue
        mid, body = r
        if mid in (0x4E, 0x4F, 0x50):
            print(f"  0x{mid:02x} len {len(body)} {body.hex(' ')}"); got += 1
        elif mid == 0x40 and body[1] == 1:
            print(f"  EVENT code 0x{body[2]:02x}")
    T.cmd(dev, rd, 0x4C, b"\x00", "close", raise_=False); time.sleep(0.3)

run("rssi only, off", 0x40, None)
run("all ext, off", 0xE0, None)
run("rssi only, bin 10", 0x40, [10])
run("rssi only, bin 3", 0x40, [3], secs=8.0)
run("rssi only, custom 0x80/60", 0x40, [0x80, 60], secs=8.0)
close_device(dev)
