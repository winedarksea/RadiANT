"""ISR-blocker hunt: N tracked channels on the TI, latency probe read out.

    python lat_run.py <outdir> --tracks N [--seconds 120] [--reopen S]
                      [--no-sims]

Starts the C2 masters (resetting them first), opens N tracked channels on the
TI (COM11 @ 57600), resets the ad hoc latency probe (MESG_REQUEST [0xFF,0xFE]),
waits, and reads the probe's pages (0..8) plus 0xF6. Then reads 0xF7 with
ant_diag.py after the port is closed. Results in <outdir>/lat.json.
"""
import argparse
import json
import os
import subprocess
import sys
import time

TOOLS = r"C:\wt_tistall\tools"
sys.path.insert(0, TOOLS)
sys.path.insert(0, r"C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-16-pass-breakdown\ti")
import leg_ti as L  # noqa: E402
L.TOOLS = TOOLS
import ant_scan_rate as asr  # noqa: E402
from ant_probe import EP_OUT, FrameReader, close_device, expect, frame, open_device, reset_stack  # noqa: E402
import ant_health  # noqa: E402
from ant_scan import ANT_PLUS_KEY  # noqa: E402
from ant_wire import (LIB_CONFIG_ALL_EXT_FIELDS, MESG_ANTLIB_CONFIG_ID,  # noqa: E402
                      MESG_NETWORK_KEY_ID, MESG_REQUEST_ID)

PY = sys.executable


def u32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def lat_page(dev, reader, page):
    dev.write(EP_OUT, frame(MESG_REQUEST_ID, bytes([page, 0xFE])))
    return expect(reader, 0xFE, 2.0)


def lat_read(dev, reader, reset=False):
    p0 = lat_page(dev, reader, 0xFF if reset else 0)
    if p0 is None:
        return None
    out = {"samples": u32(p0, 1), "max_cyc": u32(p0, 5), "min_cyc": u32(p0, 9),
           "hist": [u32(p0, 13 + 4 * i) for i in range(6)]}
    out["hist"].append(out["samples"] - sum(out["hist"]))
    out["max_us"] = round(out["max_cyc"] / 48, 1)
    out["min_us"] = round(out["min_cyc"] / 48, 2)
    tab = []
    if not reset:
        for page in range(1, 9):
            b = lat_page(dev, reader, page)
            if b is None:
                continue
            for k in range(2):
                e = {"pc": u32(b, 1 + 16 * k), "lr": u32(b, 5 + 16 * k),
                     "max_cyc": u32(b, 9 + 16 * k), "n": u32(b, 13 + 16 * k),
                     "ipsr": b[33 + k]}
                if e["n"]:
                    e["max_us"] = round(e["max_cyc"] / 48, 1)
                    e["pc"] = hex(e["pc"])
                    e["lr"] = hex(e["lr"])
                    tab.append(e)
    out["sites"] = sorted(tab, key=lambda e: -e["max_cyc"])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--tracks", type=int, default=9)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--reopen", type=float, default=None)
    ap.add_argument("--no-sims", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    leg = L.Leg(a.outdir)
    sims = {}
    if not a.no_sims:
        for board, (devargs, _, _) in L.SIMS.items():
            for attempt in range(3):
                if leg.run(f"reset_{board}_{attempt}",
                           L.tool("ant_probe.py", *devargs), 60) == 0:
                    break
        for board, (devargs, profs, base) in L.SIMS.items():
            cmd = L.tool("ant_sim.py", *devargs, "--seconds", str(int(a.seconds + 90)),
                         "--device-number", str(base), "--seed", str(base))
            for p in profs:
                cmd += ["--profile", p]
            sims[board] = leg.spawn(f"sim_{board}", cmd)
        time.sleep(10)
    tracks = []
    for keys in L.masters().values():
        tracks += keys
    tracks.append("52233:0x11")
    tracks = [asr.parse_track(t) for t in tracks[:a.tracks]]
    res = {"tracks": [t.key for t in tracks], "seconds": a.seconds,
           "reopen": a.reopen}
    leg.run("ti_diag_before", L.tool("ant_diag.py", "--port", "COM11", "--baud", "57600",
                                     "--json", os.path.join(a.outdir, "ti_diag_before.json")), 180)
    dev = open_device(False, port="COM11", baud=57600)
    reader = FrameReader(dev)
    frames = {}
    try:
        reset_stack(dev, reader)
        asr._must(dev, reader, MESG_ANTLIB_CONFIG_ID,
                  bytes([0, LIB_CONFIG_ALL_EXT_FIELDS]), "ext")
        asr._must(dev, reader, MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "key")
        for i, tr in enumerate(tracks):
            asr._open_track(dev, reader, 1 + i, tr)
        if a.reopen:
            asr._open_scan(dev, reader)
        time.sleep(2)
        lat_read(dev, reader, reset=True)
        t0 = time.monotonic()
        next_reopen = t0 + a.reopen if a.reopen else None
        while time.monotonic() < t0 + a.seconds:
            if next_reopen and time.monotonic() >= next_reopen:
                asr._close_scan(dev, reader)
                asr._open_scan(dev, reader)
                next_reopen = time.monotonic() + a.reopen
            r = reader.next_frame(time.monotonic() + 0.05)
            if r is not None:
                body = r[1]
                if r[0] in (0x4E, 0x4F, 0x50) and body:
                    ch = body[0] & 0x1F
                    frames[ch] = frames.get(ch, 0) + 1
                elif r[0] == 0x40 and len(body) >= 3 and body[1] == 1:
                    k = f"ev{body[0]}:{body[2]:02x}"
                    frames[k] = frames.get(k, 0) + 1
        res["lat"] = lat_read(dev, reader)
        hb = ant_health.request(dev, reader)
        res["health"] = ant_health.decode(hb) if hb else None
        res["rx"] = frames
        res["secs"] = round(time.monotonic() - t0, 1)
        if a.reopen:
            asr._close_scan(dev, reader)
    finally:
        close_device(dev)
        for p, f, _, _ in sims.values():
            p.kill()
            p.wait(timeout=30)
            f.close()
    leg.run("ti_diag_after", L.tool("ant_diag.py", "--port", "COM11", "--baud", "57600",
                                    "--json", os.path.join(a.outdir, "ti_diag_after.json")), 180)
    leg.run("ti_reset_end", L.tool("ant_probe.py", "--port", "COM11", "--baud", "57600"), 60)
    with open(os.path.join(a.outdir, "lat.json"), "w") as f:
        json.dump(res, f, indent=1)
    lat = res.get("lat") or {}
    print(json.dumps({k: lat.get(k) for k in ("samples", "max_us", "min_us", "hist")}))
    for e in (lat.get("sites") or [])[:8]:
        print(json.dumps(e))
    return 0


if __name__ == "__main__":
    sys.exit(main())
