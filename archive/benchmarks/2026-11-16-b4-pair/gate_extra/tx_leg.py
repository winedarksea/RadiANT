"""B4 gate_extra transmit-path leg runner (bench script only).

    python tx_leg.py <outdir> [--seconds 300] [--noack]

Masters: nRF5340 net core (COM5 @115200) HR #24001, nRF52840 Dongle
(3D55F77818BE772A) power #23002, both via ant_sim, reset first. The Pro Micro
runs tx_role.py pm (master #25001 + acked data + 2 tracks); the TI LaunchXL
runs tx_role.py ti (tracks #25001, #24001, #23002), same wall-clock window.
Pro Micro diag (0xF7) and health (0xF6) before and after. Every step bounded.
"""
import json
import os
import subprocess
import sys
import time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
TOOLS = r"C:\Users\Colin\ant_dongle\tools"
HERE = os.path.dirname(os.path.abspath(__file__))
PM = "EE6986854FD72803"
TI = ["--port", "COM11", "--baud", "57600"]
SIMS = {
    "n53": (["--port", "COM5", "--baud", "115200"], "heart-rate", 24001),
    "dongle": (["--serial", "3D55F77818BE772A"], "power", 23002),
}


def tool(name, *args):
    return [PY, "-u", os.path.join(TOOLS, name), *args]


def main():
    outdir = os.path.abspath(sys.argv[1])
    seconds = 300
    if "--seconds" in sys.argv:
        seconds = int(sys.argv[sys.argv.index("--seconds") + 1])
    pm_role = "pm-noack" if "--noack" in sys.argv else "pm"
    sim_s = seconds + 120
    os.makedirs(outdir, exist_ok=True)
    log = open(os.path.join(outdir, "leg.log"), "a", encoding="utf-8")
    meta = {"steps": [], "seconds": seconds, "pm_role": pm_role}

    def say(msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    def save():
        with open(os.path.join(outdir, "meta.json"), "w") as f:
            json.dump(meta, f, indent=1)

    def run(label, cmd, timeout):
        t0 = time.time()
        with open(os.path.join(outdir, f"{label}.txt"), "w", encoding="utf-8") as f:
            f.write("$ " + " ".join(cmd) + "\n")
            f.flush()
            try:
                rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT,
                                    timeout=timeout, cwd=TOOLS).returncode
            except subprocess.TimeoutExpired:
                rc = "timeout"
        meta["steps"].append({"label": label, "cmd": cmd, "rc": rc, "start": t0,
                              "secs": time.time() - t0})
        save()
        say(f"{label}: rc={rc} {time.time() - t0:.1f}s")
        return rc

    def spawn(label, cmd):
        f = open(os.path.join(outdir, f"{label}.txt"), "w", encoding="utf-8")
        f.write("$ " + " ".join(cmd) + "\n")
        f.flush()
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=TOOLS)
        meta["steps"].append({"label": label, "cmd": cmd, "spawned": time.time()})
        return p, f, f.name, time.time()

    def reset_masters(tag):
        for board, (dev, _, _) in SIMS.items():
            for attempt in range(3):
                if run(f"{tag}_reset_{board}_{attempt}", tool("ant_probe.py", *dev), 60) == 0:
                    break

    say(f"leg start -> {outdir}")
    run("pm_probe", tool("ant_probe.py", "--serial", PM), 60)
    run("ti_probe", tool("ant_probe.py", *TI), 60)
    reset_masters("pre")
    sims = {}
    for board, (dev, prof, dn) in SIMS.items():
        sims[board] = spawn(f"sim_{board}", tool("ant_sim.py", *dev, "--seconds", str(sim_s),
                                                  "--device-number", str(dn), "--seed", str(dn),
                                                  "--profile", prof))
    deadline = time.time() + 45
    ready = set()
    while time.time() < deadline and len(ready) < len(sims):
        for board, (_, _, path, _) in sims.items():
            with open(path, encoding="utf-8", errors="replace") as g:
                if "Transmitting for" in g.read():
                    ready.add(board)
        time.sleep(0.5)
    say(f"sims ready: {sorted(ready)}")
    if len(ready) < len(sims):
        say("ABORT: a sim did not start")
        for p, f, _, _ in sims.values():
            p.kill()
        reset_masters("abort")
        return 2

    run("diag_before", tool("ant_diag.py", "--serial", PM, "--json",
                            os.path.join(outdir, "diag_before.json")), 90)
    run("health_before", tool("ant_health.py", "--serial", PM, "--json"), 60)

    start_at = time.time() + 25
    meta["start_at"] = start_at
    roles = {}
    for role, tag in ((pm_role, "pm"), ("ti", "ti")):
        roles[tag] = spawn(f"role_{tag}", [PY, "-u", os.path.join(HERE, "tx_role.py"), role,
                                           os.path.join(outdir, f"role_{tag}.json"),
                                           f"{start_at:.3f}", str(seconds)])
    say(f"window start_at {time.strftime('%H:%M:%S', time.localtime(start_at))}")
    for tag, (p, f, _, t0) in roles.items():
        try:
            rc = p.wait(timeout=max(5, start_at + seconds + 90 - time.time()))
        except subprocess.TimeoutExpired:
            p.kill()
            rc = "timeout"
        f.close()
        meta.setdefault("roles", {})[tag] = rc
        say(f"role_{tag}: rc={rc}")
    save()

    run("diag_after", tool("ant_diag.py", "--serial", PM, "--json",
                           os.path.join(outdir, "diag_after.json")), 90)
    run("health_after", tool("ant_health.py", "--serial", PM, "--json"), 60)
    run("diag_diff", tool("ant_diag.py", "--diff", os.path.join(outdir, "diag_before.json"),
                          os.path.join(outdir, "diag_after.json")), 30)
    run("ti_reset", tool("ant_probe.py", *TI), 60)

    for board, (p, f, _, t0) in sims.items():
        try:
            rc = p.wait(timeout=max(5, t0 + sim_s + 60 - time.time()))
        except subprocess.TimeoutExpired:
            p.kill()
            rc = "timeout"
        f.close()
        meta.setdefault("sims", {})[board] = rc
        say(f"sim_{board}: rc={rc}")
    reset_masters("post")
    save()
    say("leg done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
