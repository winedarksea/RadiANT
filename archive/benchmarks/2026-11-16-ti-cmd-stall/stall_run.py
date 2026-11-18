"""TI command-stall bisection runner (2026-11-16).

One process drives the TI (COM11 @ 57600): N tracked channels (the C2 masters,
plus the trainer), optionally the 4.5 s scan close/unassign/assign/reopen, and
optionally a steady "probe" stream of harmless commands (channel status
request, 0x4D/0x52) so host->TI loss can be measured even without reopens.

Unlike ant_scan_rate this never gives up: every command gets up to RETRIES
resends; every command's latency/outcome is logged, and 0xF6 is read every
--health-every seconds (in band; a lost read is just logged).

    python stall_run.py <outdir> [--tracks 9] [--reopen 4.5] [--probe-hz 0]
                        [--seconds 150] [--no-sims] [--reset-masters]
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = r"C:\wt_tistall\tools"
sys.path.insert(0, TOOLS)
sys.path.insert(0, r"C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-16-pass-breakdown\ti")

import leg_ti as L  # noqa: E402
L.TOOLS = TOOLS
import ant_scan_rate as asr  # noqa: E402
from ant_probe import EP_OUT, FrameReader, close_device, frame, open_device, reset_stack  # noqa: E402
import ant_health  # noqa: E402

RETRIES = 6
T0 = time.time()
LOG = []
OUT = None


def note(kind, **kw):
    rec = {"t": round(time.time() - T0, 3), "kind": kind, **kw}
    LOG.append(rec)
    if kind != "cmd" or kw.get("code") != 0 or kw.get("tries", 1) > 1 or kw.get("dt", 0) > 0.5:
        print(json.dumps(rec), flush=True)


def command(dev, reader, msg_id, payload, timeout=1.5):
    """Send with resends; return final code (None = never answered)."""
    from ant_wire import MESG_RESPONSE_EVENT_ID
    t = time.monotonic()
    code = None
    tries = 0
    while tries <= RETRIES:
        tries += 1
        dev.write(EP_OUT, frame(msg_id, payload))
        deadline = time.monotonic() + timeout
        while True:
            r = reader.next_frame(deadline)
            if r is None:
                break
            got_id, body = r
            if got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
                code = body[2]
                break
        if code is not None:
            break
    note("cmd", id=msg_id, payload=payload.hex(), code=code, tries=tries,
         dt=round(time.monotonic() - t, 3))
    return code


def must(dev, reader, msg_id, payload, what):
    code = command(dev, reader, msg_id, payload)
    if code != 0:
        note("bad", what=what, code=code)


asr._command = command
asr._must = must


def health(dev, reader, tag):
    body = ant_health.request(dev, reader)
    if body is None:
        note("health", tag=tag, ok=False)
        return None
    d = ant_health.decode(body)
    note("health", tag=tag, ok=True, **{k: d[k] for k in (
        "host_csum", "host_desync", "rx_backpressure", "key_rejects",
        "rx_not_owner", "tx_drops", "tx_high_water", "tx_timeout",
        "event_overflows", "event_high_water")})
    return d


def main():
    global OUT
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--tracks", type=int, default=9)
    ap.add_argument("--reopen", type=float, default=None)
    ap.add_argument("--probe-hz", type=float, default=0.0)
    ap.add_argument("--seconds", type=float, default=150)
    ap.add_argument("--health-every", type=float, default=10)
    ap.add_argument("--no-sims", action="store_true")
    ap.add_argument("--port", default="COM11")
    ap.add_argument("--baud", type=int, default=57600)
    a = ap.parse_args()
    OUT = a.outdir
    os.makedirs(OUT, exist_ok=True)
    leg = L.Leg(OUT)
    sims = {}
    if not a.no_sims:
        for board, (devargs, _, _) in L.SIMS.items():
            for attempt in range(3):
                if leg.run(f"reset_{board}_{attempt}", L.tool("ant_probe.py", *devargs), 60) == 0:
                    break
        for board, (devargs, profs, base) in L.SIMS.items():
            cmd = L.tool("ant_sim.py", *devargs, "--seconds", str(int(a.seconds + 120)),
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
    note("config", tracks=[t.key for t in tracks], reopen=a.reopen,
         probe_hz=a.probe_hz, seconds=a.seconds)

    from ant_scan import ANT_PLUS_KEY
    from ant_wire import MESG_ANTLIB_CONFIG_ID, MESG_NETWORK_KEY_ID, LIB_CONFIG_ALL_EXT_FIELDS
    dev = open_device(False, port=a.port, baud=a.baud)
    reader = FrameReader(dev)
    n_frames = 0
    n_bytes_est = 0
    try:
        if not reset_stack(dev, reader):
            note("reset_failed")
        health(dev, reader, "start")
        must(dev, reader, MESG_ANTLIB_CONFIG_ID, bytes([0, LIB_CONFIG_ALL_EXT_FIELDS]), "ext")
        must(dev, reader, MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "key")
        for i, tr in enumerate(tracks):
            asr._open_track(dev, reader, 1 + i, tr)
        if a.reopen:
            asr._open_scan(dev, reader)
        t0 = time.monotonic()
        deadline = t0 + a.seconds
        next_reopen = t0 + a.reopen if a.reopen else None
        next_probe = t0 + (1 / a.probe_hz) if a.probe_hz else None
        next_health = t0 + a.health_every
        probe_ch = 1
        while time.monotonic() < deadline:
            now = time.monotonic()
            if next_reopen and now >= next_reopen:
                asr._close_scan(dev, reader)
                asr._open_scan(dev, reader)
                next_reopen = time.monotonic() + a.reopen
            if next_probe and now >= next_probe:
                # Set channel period to its own value: harmless, answered
                # with a RESPONSE (0x43), exercises the host->TI path.
                if tracks:
                    tr = tracks[(probe_ch - 1) % len(tracks)]
                    command(dev, reader, 0x43,
                            bytes([1 + (probe_ch - 1) % len(tracks)]) + tr.period.to_bytes(2, "little"))
                    probe_ch += 1
                next_probe = time.monotonic() + 1 / a.probe_hz
            if now >= next_health:
                health(dev, reader, "periodic")
                next_health = time.monotonic() + a.health_every
            r = reader.next_frame(time.monotonic() + 0.02)
            if r is not None:
                n_frames += 1
                n_bytes_est += len(r[1]) + 4
        if a.reopen:
            asr._close_scan(dev, reader)
        health(dev, reader, "end")
        note("rx_stats", frames=n_frames, bytes_est=n_bytes_est,
             host_checksum_errors=reader.checksum_errors,
             host_discarded=reader.discarded_bytes,
             secs=round(time.monotonic() - t0, 1))
        reset_stack(dev, reader)
    finally:
        close_device(dev)
        for p, f, _, _ in sims.values():
            p.kill()
            p.wait(timeout=30)
            f.close()
        with open(os.path.join(OUT, "run.json"), "w") as f:
            json.dump(LOG, f, indent=0)
        cmds = [r for r in LOG if r["kind"] == "cmd"]
        summ = {
            "cmds": len(cmds),
            "unanswered_first_try": sum(1 for r in cmds if r["tries"] > 1 or r["code"] is None),
            "never_answered": sum(1 for r in cmds if r["code"] is None),
            "nonzero": sum(1 for r in cmds if r["code"] not in (0, None)),
            "health": [r for r in LOG if r["kind"] == "health"],
            "rx_stats": [r for r in LOG if r["kind"] == "rx_stats"],
        }
        with open(os.path.join(OUT, "summary.json"), "w") as f:
            json.dump(summ, f, indent=1)
        print(json.dumps({k: v for k, v in summ.items() if k != "health"}), flush=True)
        if summ["health"]:
            print(json.dumps(summ["health"][-1]), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
