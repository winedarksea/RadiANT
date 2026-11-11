#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Discovery under tracking: B's discovery outcome against B's phase.

The ADR 0018 rig, and the one candidates assert against. Both HR masters are
driven from THIS process, so every EVENT_TX of both is timestamped on one host
clock. B is (re)opened at a chosen delay after one of A's EVENT_TX, which sets
B's phase relative to A - and A's slot is the DUT's tracked slot. The
host-measured phase is checked against the DUT's own per-frame phase (the
`TDIAG frame` log lines) wherever the DUT hears B at all.

    python tools/ant_disc_phase.py C:/logs/leg_a \\
        --a-serial 3D55F77818BE772A --b-serial EE6986854FD72803 \\
        --bridge COM4 --log COM3 --delays 0,15,30

Masters are named by --a-serial/--b-serial (USB) or --a-port/--b-port (UART).
There are no defaults for ports: COM numbers move between enumerations.

--r1 instead: both masters a few ms apart, the DUT tracking both, a reference
receiver tracking the later master in the same window.

--r1-walk: the R1 reading that separates R1 from the discovery band. B is
opened well clear of A (--r1-start-ms), the DUT acquires it there, and only
then is B's phase WALKED to --r1-target-ms by running B's channel period a few
counts short (or long) and putting it back on arrival. The DUT tracks B
through the walk, so the reading is the tracked-slot loss alone. The DUT's
session is recorded (ant_verify --record) and each master's packets are
counted in the measuring window only. B must be a stack that accepts a period
change on an open channel (a RadiANT master refuses it; a Garmin USB-m
accepts it); the walk aborts with a message if B refuses.

The DUT image must have CONFIG_RADIANT_SEARCH_PHASE_DIAG=y. A run whose DUT
prints neither `TDIAG frame` nor `TEMPDIAG chunks` is an error, not an empty
result: without them the trial records nothing and looks like a clean run.
"""

from __future__ import annotations

import argparse
import json
import random
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import ant_sim  # noqa: E402
from ant_probe import FrameReader, open_device, reset_stack  # noqa: E402

PY = sys.executable
PERIOD_S = 8070 / 32768.0
PERIOD_MS = PERIOD_S * 1000.0
DIAG_FRAME_RE = re.compile(r"TDIAG frame")
DIAG_CHUNK_RE = re.compile(r"TEMPDIAG chunks")
FRAME_LINE_RE = re.compile(
    r"dev=(\d+) type=0x([0-9a-f]+).*ph=(\d+).*claim=(-?\d+)")
BINS_RE = re.compile(r"phase_bins=(0x[0-9a-f]+)")
NO_DIAG_MSG = (
    "the DUT log carried neither 'TDIAG frame' nor 'TEMPDIAG chunks' lines. "
    "Build with CONFIG_RADIANT_SEARCH_PHASE_DIAG=y (and confirm the log port)."
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", help="output directory (run.txt, results.jsonl, ...)")
    ap.add_argument("--a-serial", help="master A's USB serial")
    ap.add_argument("--a-port", help="master A's UART port")
    ap.add_argument("--b-serial", help="master B's USB serial")
    ap.add_argument("--b-port", help="master B's UART port")
    ap.add_argument("--a-num", type=int, default=2002)
    ap.add_argument("--b-num", type=int, default=5005)
    ap.add_argument("--ch0", type=int, default=None,
                    help="device number pinned on the DUT's channel 0 "
                         "(default: master A's number)")
    ap.add_argument("--bridge", required=True, help="the DUT's ANT serial port")
    ap.add_argument("--log", required=True,
                    help="the DUT's log port (DTR is asserted)")
    ap.add_argument("--seconds", type=float, default=60.0,
                    help="seconds per trial")
    ap.add_argument("--delays", default="random:6",
                    help="B's open delays after A's EVENT_TX, in ms: "
                         "comma list or random:N")
    ap.add_argument("--a-off", action="store_true",
                    help="control: take A off air after setting B's phase")
    ap.add_argument("--shuffle-seed", type=int, default=None,
                    help="shuffle the --delays list with this seed")
    ap.add_argument("--r1", action="store_true",
                    help="one R1 reading instead of the discovery trials")
    ap.add_argument("--r1-delay-ms", type=float, default=12.0,
                    help="B's open delay after A in --r1 mode")
    ap.add_argument("--ref-serial", default=None,
                    help="reference receiver's USB serial, in --r1 modes")
    ap.add_argument("--r1-walk", action="store_true",
                    help="acquire B clear of A, then walk its phase (see above)")
    ap.add_argument("--r1-start-ms", type=float, default=40.0,
                    help="where B is opened and acquired, ms after A")
    ap.add_argument("--r1-target-ms", type=float, default=12.0,
                    help="where B is walked to, ms after A")
    ap.add_argument("--r1-step-counts", type=int, default=2,
                    help="period offset during the walk, 1/32768 s counts "
                         "(2 = 61 us a period; keep it inside a locked "
                         "tracking guard)")
    ap.add_argument("--r1-acquire-s", type=float, default=40.0,
                    help="how long to wait for the DUT to acquire B")
    ap.add_argument("--r1-walk-max-s", type=float, default=240.0,
                    help="give up the walk after this long")
    ap.add_argument("--ch1-trans-type", type=int, default=5,
                    help="transmission type the wildcard channel 1 pairs "
                         "with. The harness's masters send 5; a stranger "
                         "with the same device type on air sends something "
                         "else, and 0 here would bind it instead of B")
    ap.add_argument("--r1-search-dev", type=int, default=None,
                    help="with --r1-walk: a third DUT channel searching for "
                         "this (absent) device number, so the pair is held "
                         "with a search running")
    args = ap.parse_args(argv)
    if bool(args.a_serial) == bool(args.a_port):
        ap.error("pass exactly one of --a-serial, --a-port")
    if bool(args.b_serial) == bool(args.b_port):
        ap.error("pass exactly one of --b-serial, --b-port")
    if args.r1 and args.r1_walk:
        ap.error("--r1 and --r1-walk are two different readings; pick one")
    if args.r1_step_counts < 1 or args.r1_step_counts > 40:
        ap.error("--r1-step-counts must be 1..40")
    if args.ch0 is None:
        args.ch0 = args.a_num
    return args


def delays_for(spec: str, seed: int | None) -> list[float]:
    """Resolve the --delays string to a list of ms, shuffled when seeded."""
    if spec.startswith("random:"):
        n = int(spec.split(":")[1])
        delays = [random.uniform(0, PERIOD_MS) for _ in range(n)]
    else:
        delays = [float(x) for x in spec.split(",") if x.strip()]
    if seed is not None:
        random.Random(seed).shuffle(delays)
    return delays


def phase_ms(a_times: list[float], b_times: list[float]) -> tuple[float | None, float | None]:
    """Median (B - A) mod period over events where both are flowing."""
    ph = []
    for tb in b_times[-40:]:
        prior = [ta for ta in a_times if ta <= tb]
        if not prior:
            continue
        ph.append(((tb - prior[-1]) % PERIOD_S) * 1000.0)
    if len(ph) < 5:
        return None, None
    # Circular spread guard: a phase near 0/246 straddles the wrap.
    return statistics.median(ph), statistics.pstdev(ph)


def frames_by_dev(lines: list[str]) -> dict[str, dict]:
    """Per-device summary of `TDIAG frame` lines."""
    devs: dict[int, list[tuple[int, int]]] = {}
    for line in lines:
        m = FRAME_LINE_RE.search(line)
        if m:
            devs.setdefault(int(m.group(1)), []).append(
                (int(m.group(3)), int(m.group(4))))
    return {str(dv): {"n": len(v), "ph_us": sorted(p for p, _ in v)[:6],
                      "claims": sorted({c for _, c in v})}
            for dv, v in devs.items()}


def phase_bin_masks(lines: list[str]) -> list[str]:
    """The phase_bins values of `TEMPDIAG chunks` lines, in order."""
    return [m.group(1) for line in lines if (m := BINS_RE.search(line))]


def summarize(results: list[dict], *, target_dev: int) -> dict:
    """The found/not-found tally the ADR reports, per trial row in order."""
    rows = []
    for r in results:
        chans = r.get("channels") or []
        ch1 = next((c for c in chans if c.get("ch") == 1), None)
        pkts = (ch1 or {}).get("packets") or 0
        first = (ch1 or {}).get("first_s")
        # A wildcard binds whatever it hears first. Packets from anything but
        # B are not B found: on 2026-11-11 a stranger's strap read as 17/17.
        bound = (ch1 or {}).get("bound_dev")
        if bound is not None and bound != target_dev:
            pkts = 0
            first = None
        heard = r.get("frames_by_dev", {}).get(str(target_dev), {}).get("n", 0)
        rows.append({"trial": r["trial"], "delay_ms": r["delay_ms"],
                     "host_phase_ms": r.get("host_phase_ms"),
                     "found": bool(pkts), "first_s": first, "packets": pkts,
                     "tdiag_frames_of_b": heard})
    found = sum(1 for r in rows if r["found"])
    masks: list[str] = []
    for r in results:
        masks.extend(r.get("masks", []))
    return {"trials": rows, "found": found, "total": len(rows), "masks": masks}


PERIOD_COUNTS = 8070


def circ_diff_ms(to_ms: float, from_ms: float) -> float:
    """Signed shortest phase difference to - from, in (-period/2, period/2]."""
    d = (to_ms - from_ms) % PERIOD_MS
    if d > PERIOD_MS / 2.0:
        d -= PERIOD_MS
    return d


def walk_plan(cur_ms: float, target_ms: float, step_counts: int) -> tuple[int, float]:
    """The period B runs at to walk its phase from cur to target, and roughly
    how many periods it takes. A shorter period moves B earlier against A."""
    d = circ_diff_ms(target_ms, cur_ms)
    shift_ms = step_counts / 32768.0 * 1000.0
    period = PERIOD_COUNTS - step_counts if d < 0 else PERIOD_COUNTS + step_counts
    return period, abs(d) / shift_ms


def capture_listen_start(path: Path) -> float | None:
    """The `listen_start_unix=` comment ant_verify writes into a capture."""
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"listen_start_unix=([0-9.]+)", line)
        if m:
            return float(m.group(1))
    return None


def count_in_window(records, listen_start_unix: float, w0_unix: float,
                    w1_unix: float) -> dict[int, int]:
    """Packets per device number whose arrival falls in [w0, w1). `records`
    are (t_rel_s, device_type, device_number, payload) as read_capture gives."""
    counts: dict[int, int] = {}
    for rec in records:
        t = listen_start_unix + float(rec[0])
        if w0_unix <= t < w1_unix:
            counts[int(rec[2])] = counts.get(int(rec[2]), 0) + 1
    return counts


def window_loss_pct(packets: int, seconds: float) -> float:
    """Loss against the packets one ANT+ HR master sends in `seconds`."""
    expected = seconds / PERIOD_S
    return max(0.0, 100.0 * (1.0 - packets / expected)) if expected else 0.0


class Log:
    """The DUT's radiant log, DTR asserted (silent without it)."""

    def __init__(self, port: str, out: Path, t0: float):
        import serial  # noqa: PLC0415 - optional at import time, for the tests

        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.dtr = True
        self.t0 = t0
        self.lines: list[tuple[float, str]] = []
        self.lock = threading.Lock()
        self.f = open(out / "dut_log.txt", "a", encoding="utf-8")
        threading.Thread(target=self.run, daemon=True).start()

    def run(self) -> None:
        buf = b""
        while True:
            buf += self.s.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = re.sub(r"\x1b\[[0-9;]*m", "",
                              raw.decode("utf-8", "replace")).rstrip()
                with self.lock:
                    self.lines.append((time.time(), text))
                self.f.write(f"{time.time() - self.t0:8.2f} {text}\n")
                self.f.flush()

    def mark(self) -> int:
        with self.lock:
            return len(self.lines)

    def since(self, mark: int, pattern: re.Pattern[str]) -> list[str]:
        with self.lock:
            return [line for _t, line in self.lines[mark:] if pattern.search(line)]


class Master:
    """One HR master, fed from its own EVENT_TX, every event timestamped."""

    def __init__(self, label, devnum, serial_no=None, port=None):
        self.label, self.devnum = label, devnum
        self.dev = open_device(False, serial=serial_no, port=port)
        self.reader = FrameReader(self.dev)
        self.times: list[float] = []
        self.stop_flag = threading.Event()
        self.thread = None
        self.sensor = None
        # One writer at a time: the loop answers every EVENT_TX, and the
        # R1 walk writes a channel period from the main thread.
        self.wlock = threading.Lock()
        # The last channel response to a MESG_CHANNEL_MESG_PERIOD (code).
        self.period_resp: int | None = None

    def set_period(self, counts: int) -> None:
        """Change the open channel's period. Answered asynchronously; the
        loop records the response code in period_resp."""
        self.period_resp = None
        with self.wlock:
            self.dev.write(ant_sim.EP_OUT, ant_sim.frame(
                ant_sim.MESG_CHANNEL_MESG_PERIOD_ID,
                bytes([0, counts & 0xFF, counts >> 8])))

    def _sensor(self):
        rng = random.Random(1)
        return ant_sim.HeartRateSensor(
            0, self.devnum, 5,
            ant_sim.Signal(100, 0, rng), ant_sim.Signal(80, 0, rng), bpm=120)

    def off(self) -> bool:
        """Stack reset: the only thing that takes a master channel off air."""
        self.stop_flag.set()
        if self.thread:
            self.thread.join()
        self.thread = None
        ok = reset_stack(self.dev, self.reader)
        self.times = []
        return ok

    def on(self) -> float:
        self.off()
        if not ant_sim.command(self.dev, self.reader, ant_sim.MESG_NETWORK_KEY_ID,
                               bytes([0]) + ant_sim.ANT_PLUS_KEY, "key"):
            raise RuntimeError(f"{self.label}: key refused")
        self.sensor = self._sensor()
        t_open = time.perf_counter()
        if not self.sensor.bring_up(self.dev, self.reader):
            raise RuntimeError(f"{self.label}: bring-up failed")
        self.stop_flag = threading.Event()
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()
        return t_open

    def _loop(self) -> None:
        while not self.stop_flag.is_set():
            r = self.reader.next_frame(time.monotonic() + 0.1)
            if r is None:
                continue
            mid, body = r
            if (mid == ant_sim.MESG_RESPONSE_EVENT_ID and len(body) >= 3
                    and body[0] == 0
                    and body[1] == ant_sim.MESG_CHANNEL_MESG_PERIOD_ID):
                self.period_resp = body[2]
            if (mid == ant_sim.MESG_RESPONSE_EVENT_ID and len(body) >= 3
                    and body[0] == 0 and body[1] == ant_sim.EVENT_MARKER
                    and body[2] == ant_sim.EVENT_TX):
                self.times.append(time.perf_counter())
                with self.wlock:
                    self.dev.write(ant_sim.EP_OUT, ant_sim.frame(
                        ant_sim.MESG_BROADCAST_DATA_ID,
                        bytes([0]) + self.sensor.next_payload()))


def set_b_phase(a: Master, b: Master, delay_ms: float) -> None:
    """Open B `delay_ms` after one of A's EVENT_TX. Bring-up is several round
    trips, so the target is the OPEN; the measured phase is the truth."""
    b.off()
    time.sleep(0.5)
    while not a.times or time.perf_counter() - a.times[-1] > 0.02:
        time.sleep(0.0005)
    target = a.times[-1] + delay_ms / 1000.0
    while time.perf_counter() < target:
        pass
    b.on()


def run_verify(args, out: Path, tag: str, *cmd_tail: str) -> dict | None:
    js = out / f"{tag}.json"
    txt = out / f"{tag}_verify.txt"
    cmd = [PY, str(TOOLS / "ant_verify.py"), *cmd_tail,
           "--seconds", str(int(args.seconds)), "--json", str(js)]
    with open(txt, "w", encoding="utf-8") as f:
        subprocess.run(cmd, cwd=TOOLS, stdout=f, stderr=subprocess.STDOUT,
                       check=False)
    try:
        return json.loads(js.read_text())
    except Exception:  # noqa: BLE001 - a missing json is a host fault
        return None


def discovery_verify_args(args) -> list[str]:
    return ["--port", args.bridge,
            "--profile", "heart-rate", "--profile", "heart-rate",
            "--channel", "0", "--channel", "1",
            "--device-number", str(args.ch0), "--device-number", "0",
            "--trans-type", "0", "--trans-type", str(args.ch1_trans_type)]


def require_diag(log: Log, note) -> None:
    if not log.since(0, DIAG_FRAME_RE) and not log.since(0, DIAG_CHUNK_RE):
        note(NO_DIAG_MSG)
        raise SystemExit(2)


def run_discovery(args, out: Path, note) -> int:
    delays = delays_for(args.delays, args.shuffle_seed)
    note(f"delays: {delays}")
    t0 = time.time()
    log = Log(args.log, out, t0)
    a = Master("A", args.a_num, serial_no=args.a_serial, port=args.a_port)
    b = Master("B", args.b_num, serial_no=args.b_serial, port=args.b_port)
    a.on()
    time.sleep(3)
    note(f"A #{args.a_num} on air, {len(a.times)} EVENT_TX in 3 s")

    results = []
    for k, d in enumerate(delays):
        set_b_phase(a, b, d)
        time.sleep(4)
        ph, sp = phase_ms(a.times, b.times)
        note(f"trial {k}: delay {d:6.1f} ms -> host phase B-A {ph} ms (sd {sp})")
        mark = log.mark()
        tag = f"t{k:02d}"
        if args.a_off:
            a.off()
        j = run_verify(args, out, tag, *discovery_verify_args(args))
        if j is None:
            note(f"trial {k}: no ant_verify JSON (host fault, see {tag}_verify.txt)")
        if args.a_off:
            a.on()
            time.sleep(1)
        ph2, _ = phase_ms(a.times, b.times)
        row = {"trial": k, "delay_ms": d, "host_phase_ms": ph, "host_phase_sd": sp,
               "host_phase_end_ms": ph2,
               "frames_by_dev": frames_by_dev(log.since(mark, DIAG_FRAME_RE)),
               "masks": phase_bin_masks(log.since(mark, DIAG_CHUNK_RE))}
        if j:
            row["channels"] = [
                {"ch": c.get("channel"), "dev": c.get("device_number"),
                 "packets": c.get("packets"),
                 "first_s": c.get("time_to_first_packet_s"),
                 "bound_dev": (j.get("identities", {})
                               .get(str(c.get("channel"))) or {})
                 .get("device_number")}
                for c in j.get("channels", [])]
        results.append(row)
        note(json.dumps(row))
        with open(out / "results.jsonl", "a", encoding="utf-8") as f:
            f.write(json.dumps(row) + "\n")
        if k == 0:
            require_diag(log, note)

    require_diag(log, note)
    b.off()
    a.off()
    note("both masters off air (stack reset)")
    tally = summarize(results, target_dev=args.b_num)
    (out / "summary.json").write_text(json.dumps(tally, indent=2))
    note(f"found {tally['found']}/{tally['total']} of #{args.b_num}")
    note(f"masks seen: {sorted(set(tally['masks']))}")
    return 0


def run_r1(args, out: Path, note) -> int:
    t0 = time.time()
    log = Log(args.log, out, t0)
    a = Master("A", args.a_num, serial_no=args.a_serial, port=args.a_port)
    b = Master("B", args.b_num, serial_no=args.b_serial, port=args.b_port)
    a.on()
    time.sleep(3)
    note(f"A #{args.a_num} on air, {len(a.times)} EVENT_TX in 3 s")

    ph = None
    for attempt in range(6):
        set_b_phase(a, b, args.r1_delay_ms)
        time.sleep(4)
        ph, sp = phase_ms(a.times, b.times)
        note(f"r1 attempt {attempt}: open delay {args.r1_delay_ms} ms -> "
             f"host phase B-A {ph} ms (sd {sp})")
        if ph is not None and 5.0 <= ph <= 20.0:
            break
    else:
        note("r1: could not place B 5-20 ms after A in 6 attempts; aborting")
        b.off()
        a.off()
        return 1

    results = {}
    procs = {}
    dut_args = ["--port", args.bridge,
                "--profile", "heart-rate", "--profile", "heart-rate",
                "--channel", "0", "--channel", "1",
                "--device-number", str(args.a_num),
                "--device-number", str(args.b_num)]
    ref_args = ["--serial", args.ref_serial, "--profile", "heart-rate",
                "--device-number", str(args.b_num)] if args.ref_serial else None

    def launch(tag, tail):
        procs[tag] = subprocess.Popen(
            [PY, str(TOOLS / "ant_verify.py"), *tail,
             "--seconds", str(int(args.seconds)),
             "--json", str(out / f"r1_{tag}.json")],
            cwd=TOOLS, stdout=open(out / f"r1_{tag}_verify.txt", "w",
                                   encoding="utf-8"),
            stderr=subprocess.STDOUT)

    launch("dut", dut_args)
    if ref_args:
        launch("ref", ref_args)
    note(f"r1 window: DUT tracking #{args.a_num} and #{args.b_num}; "
         f"reference on #{args.b_num}: {args.ref_serial}")
    for tag, proc in procs.items():
        proc.wait()
        note(f"r1 {tag} ant_verify exited {proc.returncode}")
        try:
            results[tag] = json.loads((out / f"r1_{tag}.json").read_text())
        except Exception:  # noqa: BLE001 - a missing json is a host fault
            note(f"r1 {tag}: no JSON")
            results[tag] = None
    (out / "r1_summary.json").write_text(json.dumps(
        {"phase_ms": ph, "results": results}, indent=2))
    require_diag(log, note)
    b.off()
    a.off()
    note("both masters off air (stack reset)")
    return 0


def phase_now_ms(a_times: list[float], b_times: list[float], n: int = 8) -> float | None:
    """B - A over the last `n` of B's events only: what the walk steers by.
    phase_ms() medians 40 events, ~10 s, too slow to stop a walk on."""
    ph = []
    for tb in b_times[-n:]:
        prior = [ta for ta in a_times[-200:] if ta <= tb]
        if prior:
            ph.append(((tb - prior[-1]) % PERIOD_S) * 1000.0)
    return statistics.median(ph) if len(ph) >= max(3, n // 2) else None


def acquired(log: Log, mark: int, devnum: int) -> bool:
    """The DUT's channel 1 claimed a frame from `devnum` (a TDIAG line)."""
    for line in log.since(mark, DIAG_FRAME_RE):
        m = FRAME_LINE_RE.search(line)
        if m and int(m.group(1)) == devnum and int(m.group(4)) == 1:
            return True
    return False


def run_r1_walk(args, out: Path, note) -> int:
    log = Log(args.log, out, time.time())
    a = Master("A", args.a_num, serial_no=args.a_serial, port=args.a_port)
    b = Master("B", args.b_num, serial_no=args.b_serial, port=args.b_port)
    a.on()
    time.sleep(3)
    note(f"A #{args.a_num} on air, {len(a.times)} EVENT_TX in 3 s")

    # 1. B opened well clear of A. The open delay is not the phase (B's
    # bring-up adds tens of ms), so correct by the measured error and retry.
    delay = args.r1_start_ms
    ph = None
    for attempt in range(6):
        set_b_phase(a, b, delay % PERIOD_MS)
        time.sleep(4)
        ph, sp = phase_ms(a.times, b.times)
        note(f"walk place {attempt}: open delay {delay % PERIOD_MS:.1f} ms -> "
             f"B-A {ph} ms (sd {sp})")
        if ph is not None and abs(circ_diff_ms(args.r1_start_ms, ph)) <= 8.0:
            break
        if ph is not None:
            delay += circ_diff_ms(args.r1_start_ms, ph)
    else:
        note("walk: could not place B near --r1-start-ms; aborting")
        b.off()
        a.off()
        return 1

    # 2. The DUT session spans acquisition, the walk and the window.
    dur = args.r1_acquire_s + args.r1_walk_max_s + args.seconds + 15.0
    cap = out / "r1w_dut.antser"
    mark = log.mark()
    dut = subprocess.Popen(
        [PY, str(TOOLS / "ant_verify.py"), "--port", args.bridge,
         "--profile", "heart-rate", "--profile", "heart-rate",
         "--channel", "0", "--channel", "1",
         "--device-number", str(args.a_num), "--device-number", str(args.b_num),
         *(["--profile", "heart-rate", "--channel", "2",
            "--device-number", str(args.r1_search_dev)]
           if args.r1_search_dev is not None else []),
         "--seconds", str(int(dur)), "--record", str(cap),
         "--json", str(out / "r1w_dut.json")],
        cwd=TOOLS, stdout=open(out / "r1w_dut_verify.txt", "w", encoding="utf-8"),
        stderr=subprocess.STDOUT)
    note(f"walk: DUT session {dur:.0f} s, recording {cap.name}")

    t_end = time.time() + args.r1_acquire_s
    while time.time() < t_end and not acquired(log, mark, args.b_num):
        time.sleep(0.5)
    got = acquired(log, mark, args.b_num)
    note(f"walk: B #{args.b_num} acquired by the DUT's ch1: {got}")
    summary: dict = {"start_phase_ms": ph, "acquired": got}

    # 3. The walk.
    walked = False
    if got:
        period, n_est = walk_plan(ph, args.r1_target_ms, args.r1_step_counts)
        b.set_period(period)
        time.sleep(1.0)
        note(f"walk: B period {period} (est. {n_est:.0f} periods), "
             f"response {b.period_resp}")
        if b.period_resp != 0:
            note("walk: master B refused the period change on an open channel "
                 "(a RadiANT master does); use a stack that accepts it")
        else:
            t_walk = time.time()
            last = None
            while time.time() - t_walk < args.r1_walk_max_s:
                cur = phase_now_ms(a.times, b.times)
                if cur is not None:
                    d = circ_diff_ms(args.r1_target_ms, cur)
                    if last is not None and (abs(d) < 0.3 or (d > 0) != (last > 0)):
                        walked = True
                        break
                    last = d
                time.sleep(0.25)
            b.set_period(PERIOD_COUNTS)
            time.sleep(3.0)
            summary["walk_s"] = round(time.time() - t_walk, 1)
            note(f"walk: arrived={walked} after {summary['walk_s']} s, "
                 f"B period restored (response {b.period_resp})")
    summary["walked"] = walked
    summary["phase_before_window_ms"] = phase_ms(a.times, b.times)[0]

    # 4. The window, with the reference on B at the same instant.
    w0 = time.time()
    ref = None
    if args.ref_serial:
        ref = subprocess.Popen(
            [PY, str(TOOLS / "ant_verify.py"), "--serial", args.ref_serial,
             "--profile", "heart-rate", "--device-number", str(args.b_num),
             "--seconds", str(int(args.seconds)),
             "--json", str(out / "r1w_ref.json")],
            cwd=TOOLS, stdout=open(out / "r1w_ref_verify.txt", "w",
                                   encoding="utf-8"),
            stderr=subprocess.STDOUT)
    time.sleep(args.seconds)
    w1 = time.time()
    if ref:
        ref.wait()
    summary["phase_after_window_ms"] = phase_ms(a.times, b.times)[0]
    summary["window_unix"] = [round(w0, 3), round(w1, 3)]
    note(f"walk: window {w1 - w0:.1f} s, B-A {summary['phase_before_window_ms']}"
         f" -> {summary['phase_after_window_ms']} ms; waiting for the DUT session")
    dut.wait()

    # 5. Count each master's packets in the window from the DUT's recording.
    try:
        import ant_pages  # noqa: PLC0415
        start = capture_listen_start(cap)
        counts = count_in_window(ant_pages.read_capture(str(cap)), start, w0, w1)
    except Exception as exc:  # noqa: BLE001 - a missing capture is a host fault
        note(f"walk: no DUT capture ({exc})")
        counts = {}
    secs = w1 - w0
    for tag, num in (("a", args.a_num), ("b", args.b_num)):
        n = counts.get(num, 0)
        summary[f"dut_{tag}_packets"] = n
        summary[f"dut_{tag}_loss_pct"] = round(window_loss_pct(n, secs), 2)
    try:
        refj = json.loads((out / "r1w_ref.json").read_text())
        summary["ref"] = refj.get("channels")
    except Exception:  # noqa: BLE001
        summary["ref"] = None
    (out / "r1w_summary.json").write_text(json.dumps(summary, indent=2))
    note(f"walk summary: {json.dumps(summary)}")
    b.off()
    a.off()
    note("both masters off air (stack reset)")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()

    def note(msg: str) -> None:
        line = f"[{time.time() - t0:8.2f}] {msg}"
        print(line, flush=True)
        with open(out / "run.txt", "a", encoding="utf-8") as f:
            f.write(line + "\n")

    if args.r1:
        return run_r1(args, out, note)
    if args.r1_walk:
        return run_r1_walk(args, out, note)
    return run_discovery(args, out, note)


if __name__ == "__main__":
    sys.exit(main())
