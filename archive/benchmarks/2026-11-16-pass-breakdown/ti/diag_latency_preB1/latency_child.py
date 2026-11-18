
import os, sys, time, json
sys.path.insert(0, r"C:\wt_c2\tools")
import ant_scan_rate as asr
orig = asr._command
lat = []
T0 = time.time()
def cmd(dev, reader, msg_id, payload, timeout=2.0):
    t = time.monotonic()
    code = orig(dev, reader, msg_id, payload, 15.0)
    dt = time.monotonic() - t
    lat.append((round(time.time() - T0, 2), msg_id, round(dt, 3), code))
    if dt > 0.5 or code is None:
        print(f"SLOW t={time.time()-T0:.1f}s msg 0x{msg_id:02X} {dt:.2f}s code {code}", flush=True)
    return code
asr._command = cmd
try:
    rc = asr.main()
finally:
    json.dump(lat, open(os.environ["LAT_JSON"], "w"))
