"""C2 load leg runner. One leg: sims on three master boards, Garmin pre-count,
Pro Micro diag before, 300 s scan+track on the Pro Micro with the Garmin
tracking one master in the same window, diag after, Garmin post-count, sims
exit. Every step bounded by a timeout; every output saved as it goes.

    python leg.py <outdir> [--trainer]
"""
import json
import os
import subprocess
import sys
import time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
TOOLS = r"C:\wt_c2\tools"
PM = "EE6986854FD72803"
GARMIN = "006"
DONGLE = "3D55F77818BE772A"
LEG_S = 300
SIM_S = 480

SIMS = {
    "l15": (["--port", "COM4", "--baud", "57600"],
            ["power", "heart-rate", "csc"], 21001),
    "n53": (["--port", "COM5", "--baud", "115200"],
            ["heart-rate", "csc", "power"], 24001),
    "dongle": (["--serial", DONGLE], ["csc", "power"], 23001),
}
TYPES = {"power": 0x0B, "heart-rate": 0x78, "csc": 0x79}
PERIODS = {0x0B: 8182, 0x78: 8070, 0x79: 8086}
GARMIN_REF = ("power", 21001)


def masters():
    out = {}
    for board, (_, profs, base) in SIMS.items():
        out[board] = [f"{base + i}:0x{TYPES[p]:02X}" for i, p in enumerate(profs)]
    return out


def tool(name, *args):
    return [PY, "-u", os.path.join(TOOLS, name), *args]


class Leg:
    def __init__(self, outdir):
        self.out = outdir
        os.makedirs(outdir, exist_ok=True)
        self.log = open(os.path.join(outdir, "leg.log"), "a", encoding="utf-8")
        self.meta = {"steps": []}

    def say(self, msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        self.log.write(line + "\n")
        self.log.flush()

    def run(self, label, cmd, timeout):
        t0 = time.time()
        path = os.path.join(self.out, f"{label}.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("$ " + " ".join(cmd) + "\n")
            f.flush()
            try:
                rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT,
                                    timeout=timeout, cwd=TOOLS).returncode
            except subprocess.TimeoutExpired:
                rc = "timeout"
        dt = time.time() - t0
        self.meta["steps"].append({"label": label, "cmd": cmd, "rc": rc,
                                   "start": t0, "secs": dt})
        self.say(f"{label}: rc={rc} {dt:.1f}s")
        self.save_meta()
        return rc

    def spawn(self, label, cmd):
        path = os.path.join(self.out, f"{label}.txt")
        f = open(path, "w", encoding="utf-8")
        f.write("$ " + " ".join(cmd) + "\n")
        f.flush()
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=TOOLS)
        return p, f, path, time.time()

    def save_meta(self):
        with open(os.path.join(self.out, "meta.json"), "w") as f:
            json.dump(self.meta, f, indent=1)

    def count(self, label):
        """Garmin 20 s scan: which sim devices are on air, per board."""
        js = os.path.join(self.out, f"{label}.json")
        self.run(label, tool("ant_scan_rate.py", "--serial", GARMIN,
                             "--seconds", "20", "--json", js), 90)
        self.run(label + "_garmin_reset", tool("ant_probe.py", "--serial", GARMIN), 60)
        seen = {}
        try:
            with open(js) as f:
                res = json.load(f)
            devs = set()
            for child in res["dongles"].values():
                if isinstance(child, dict) and "summary" in child:
                    devs |= set(child["summary"]["devices"])
            for board, keys in masters().items():
                seen[board] = sorted(k for k in keys if k in devs)
            seen["_all_devices"] = sorted(devs)
        except Exception as exc:  # noqa: BLE001
            seen["error"] = repr(exc)
        self.meta[label] = seen
        self.save_meta()
        self.say(f"{label}: " + ", ".join(
            f"{b}={len(v)}/{len(masters()[b])}" for b, v in seen.items()
            if b in masters()))
        return seen


def main():
    outdir = sys.argv[1]
    trainer = "--trainer" in sys.argv
    leg = Leg(outdir)
    leg.meta.update({"masters": masters(), "trainer_tracked": trainer,
                     "leg_s": LEG_S, "sim_s": SIM_S, "t_start": time.time()})
    leg.say(f"leg start -> {outdir}, trainer={trainer}")

    # Pro Micro alive + version.
    leg.run("pm_probe", tool("ant_probe.py", "--serial", PM), 60)

    def reset_masters(tag):
        for board, (dev, _, _) in SIMS.items():
            for attempt in range(3):
                rc = leg.run(f"{tag}_reset_{board}_{attempt}", tool("ant_probe.py", *dev), 60)
                if rc == 0:
                    break

    def start_sims(tag):
        sims = {}
        for board, (dev, profs, base) in SIMS.items():
            cmd = tool("ant_sim.py", *dev, "--seconds", str(SIM_S),
                       "--device-number", str(base), "--seed", str(base))
            for p in profs:
                cmd += ["--profile", p]
            sims[board] = leg.spawn(f"{tag}_sim_{board}", cmd)
            leg.say(f"{tag}_sim_{board} started pid={sims[board][0].pid}")
        deadline = time.time() + 45
        ready = set()
        while time.time() < deadline and len(ready) < len(sims):
            for board, (p, f, path, _) in sims.items():
                if board in ready:
                    continue
                with open(path, encoding="utf-8", errors="replace") as g:
                    if "Transmitting for" in g.read():
                        ready.add(board)
            time.sleep(0.5)
        leg.say(f"{tag} sims ready: {sorted(ready)}")
        time.sleep(3)
        return sims

    def kill_sims(sims):
        for p, f, _, _ in sims.values():
            p.kill()
            p.wait(timeout=30)
            f.close()

    sims = None
    for attempt in range(2):
        tag = f"try{attempt}"
        reset_masters(tag)
        sims = start_sims(tag)
        pre = leg.count(f"count_pre_{tag}")
        full = all(len(pre.get(b, [])) == len(k) for b, k in masters().items())
        if full:
            leg.meta["count_pre"] = pre
            break
        leg.say(f"{tag}: master count short, killing sims and retrying")
        kill_sims(sims)
        sims = None
    if sims is None:
        leg.say("ABORT: master count short twice")
        reset_masters("abort")
        leg.save_meta()
        return 2

    # Diag before.
    leg.run("diag_before", tool("ant_diag.py", "--serial", PM, "--json",
                                os.path.join(outdir, "diag_before.json")), 90)
    leg.run("health_before", tool("ant_health.py", "--serial", PM, "--json"), 60)

    tracks = []
    for keys in masters().values():
        tracks += keys
    if trainer:
        tracks.append("52233:0x11")
    scan_cmd = tool("ant_scan_rate.py", "--serial", PM, "--seconds", str(LEG_S),
                    "--reopen", "4.5", "--json", os.path.join(outdir, "scan_pm.json"))
    for t in tracks:
        scan_cmd += ["--track", t]
    gprof, gdev = GARMIN_REF
    gar_cmd = tool("ant_verify.py", "--serial", GARMIN, "--profile", gprof,
                   "--device-number", str(gdev), "--trans-type", "5",
                   "--seconds", str(LEG_S), "--json",
                   os.path.join(outdir, "garmin_verify.json"))
    leg.say("window start")
    scan = leg.spawn("scan_pm", scan_cmd)
    gar = leg.spawn("garmin_verify", gar_cmd)
    rcs = {}
    for label, (p, f, path, t0) in (("scan_pm", scan), ("garmin_verify", gar)):
        try:
            rc = p.wait(timeout=t0 + LEG_S + 120 - time.time())
        except subprocess.TimeoutExpired:
            p.kill()
            rc = "timeout"
        f.close()
        rcs[label] = {"rc": rc, "start": t0, "secs": time.time() - t0}
        leg.say(f"{label}: rc={rc} {time.time() - t0:.1f}s")
    leg.meta["window"] = rcs
    leg.save_meta()

    leg.run("diag_after", tool("ant_diag.py", "--serial", PM, "--json",
                               os.path.join(outdir, "diag_after.json")), 90)
    leg.run("health_after", tool("ant_health.py", "--serial", PM, "--json"), 60)
    leg.run("diag_diff", tool("ant_diag.py", "--diff",
                              os.path.join(outdir, "diag_before.json"),
                              os.path.join(outdir, "diag_after.json")), 30)

    post = leg.count("count_post")
    leg.meta["count_post_full"] = all(
        len(post.get(b, [])) == len(k) for b, k in masters().items())

    for board, (p, f, path, t0) in sims.items():
        try:
            rc = p.wait(timeout=max(5, t0 + SIM_S + 60 - time.time()))
        except subprocess.TimeoutExpired:
            p.kill()
            rc = "timeout"
        f.close()
        leg.meta.setdefault("sims", {})[board] = {"rc": rc, "start": t0,
                                                   "secs": time.time() - t0}
        leg.say(f"sim_{board}: rc={rc}")
    leg.meta["t_end"] = time.time()
    leg.save_meta()
    leg.say("leg done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
