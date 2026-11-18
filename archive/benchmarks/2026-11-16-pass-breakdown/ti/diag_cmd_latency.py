"""Diagnostic, not a leg: how long does the TI take to answer host commands
under the C2 workload? Every TI leg's scan child died on "-> no answer"
(2 s timeout). This runs the same masters and the same TI scan+track workload
for --seconds, but with a 15 s command timeout and a log line per command
whose answer took longer than 0.5 s (and a summary of all latencies).

    python diag_cmd_latency.py <outdir> [--seconds 150]
"""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import leg_ti as L  # noqa: E402

CHILD = r'''
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
'''


def main():
    outdir = sys.argv[1]
    secs = 150
    if "--seconds" in sys.argv:
        secs = int(sys.argv[sys.argv.index("--seconds") + 1])
    leg = L.Leg(outdir)
    leg.run("ti_probe", L.tool("ant_probe.py", *L.TI), 60)
    for board, (dev, _, _) in L.SIMS.items():
        leg.run(f"reset_{board}", L.tool("ant_probe.py", *dev), 60)
    sims = {}
    for board, (dev, profs, base) in L.SIMS.items():
        cmd = L.tool("ant_sim.py", *dev, "--seconds", str(secs + 90),
                     "--device-number", str(base), "--seed", str(base))
        for p in profs:
            cmd += ["--profile", p]
        sims[board] = leg.spawn(f"sim_{board}", cmd)
    time.sleep(8)
    child = os.path.join(outdir, "latency_child.py")
    with open(child, "w") as f:
        f.write(CHILD)
    tracks = []
    for keys in L.masters().values():
        tracks += keys
    tracks.append("52233:0x11")
    cmd = [L.PY, "-u", child, *L.TI, "--seconds", str(secs), "--reopen", "4.5",
           "--json", os.path.join(outdir, "scan_ti.json")]
    for t in tracks:
        cmd += ["--track", t]
    os.environ["LAT_JSON"] = os.path.join(outdir, "latency.json")
    leg.say("latency run start")
    p, f, path, t0 = leg.spawn("scan_ti", cmd)
    rc = p.wait(timeout=secs + 300)
    f.close()
    leg.say(f"scan_ti rc={rc} {time.time() - t0:.1f}s")
    leg.run("ti_health_after", L.tool("ant_health.py", *L.TI, "--json"), 60)
    for p2, f2, _, _ in sims.values():
        p2.kill()
        p2.wait(timeout=30)
        f2.close()
    for board, (dev, _, _) in L.SIMS.items():
        for attempt in range(3):
            if leg.run(f"reset_end_{board}_{attempt}", L.tool("ant_probe.py", *dev), 60) == 0:
                break
    leg.run("ti_reset_end", L.tool("ant_probe.py", *L.TI), 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
