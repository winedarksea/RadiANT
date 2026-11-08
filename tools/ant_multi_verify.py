#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Run ant_verify.py against several receivers at once, against one live
transmitter, and print their results side by side.

This answers a different question than tools/ant_ab.py. ant_ab.py compares
two *sittings* (a build vs. a build, minutes or days apart) and refuses to
trust the comparison unless they carry matching rig metadata, because
run-to-run drift on this bench rivals the effect being measured. This script
instead compares several *receivers* against one shared transmitter in the
same window, so there is no run-to-run drift to distrust: every target hears
the identical RF at the identical second, and any spread between them is a
receiver-side or link-margin difference, not a room-changed-between-runs one.

    python tools/ant_multi_verify.py --seconds 300 --device-number 33333 \\
        --target "Feather=usb:755972D7183A6183" \\
        --target "nRF52840 dongle=usb:3D55F77818BE772A" \\
        --target "L15 core DUT=port:COM8"

Each --target is NAME=usb:SERIAL (matched the way ant_verify.py's --serial
does, by suffix) or NAME=port:COMn (a UART build, e.g. the nRF54L15 DK's
core+nrf image talked to directly per [[hardware-bench]]). Extra arguments
after -- are forwarded verbatim to every child ant_verify.py invocation, so
--expect-watts, --profile, etc. all work here too.

Each target runs as its own ant_verify.py subprocess - the loss/jitter/
accounting logic is not reimplemented here, only launched concurrently and
summarised. A child's stdout/stderr is captured to a per-target log next to
its --json output so a failure can be read in full; this script only prints
the numbers that matter for a side-by-side read.

Exit status: 0 only if every target's ant_verify.py run passed; 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

_HERE = Path(__file__).resolve().parent


@dataclass
class Target:
    name: str
    kind: str  # "usb" or "port"
    value: str


def parse_target(spec: str) -> Target:
    if "=" not in spec:
        raise argparse.ArgumentTypeError(
            f"--target needs NAME=usb:SERIAL or NAME=port:COMn, got {spec!r}")
    name, rest = spec.split("=", 1)
    if ":" not in rest:
        raise argparse.ArgumentTypeError(
            f"--target value needs usb:SERIAL or port:COMn, got {rest!r}")
    kind, value = rest.split(":", 1)
    if kind not in ("usb", "port"):
        raise argparse.ArgumentTypeError(f"--target kind must be usb or port, got {kind!r}")
    return Target(name=name, kind=kind, value=value)


def slug_of(name: str) -> str:
    """The file stem a target's --json/log/antcap are written under."""
    return "".join(c if c.isalnum() else "_" for c in name)


def child_command(t: Target, seconds: float, device_numbers: list[int],
                  extra: list[str], json_path: Path, baud: int = 115200,
                  record_path: Path | None = None) -> list[str]:
    """The ant_verify.py invocation for one target. Split out so the
    orchestrator (tools/ant_compare.py) and the tests can see exactly what is
    launched without launching it."""
    cmd = [sys.executable, str(_HERE / "ant_verify.py"),
           "--seconds", str(seconds),
           "--json", str(json_path), "-q"]
    for d in device_numbers:
        cmd += ["--device-number", str(d)]
    if t.kind == "usb":
        cmd += ["--serial", t.value]
    else:
        cmd += ["--port", t.value, "--baud", str(baud)]
    if record_path is not None:
        cmd += ["--record", str(record_path)]
    cmd += list(extra)
    return cmd


def wait_each(procs: list, poll_s: float = 0.2) -> dict[int, tuple[int, float]]:
    """Wait for every child, noting the moment EACH one finished.

    Returns {id(proc): (returncode, monotonic finish time)}. `procs` holds the
    run_targets() tuples; only element 1 (the Popen) is used.

    Polled in one loop, not wait()ed in launch order. Waiting in order stamps
    every child after the first with the time the SLOWEST one before it
    finished: a receiver whose COM port died at 245 s of a 300 s window read
    back as wall_s=301, which disarmed ant_compare.py's dropped-port check for
    every receiver but the first (2026-11-07, the 54L15's VCOM death).
    """
    pending = {id(entry[1]): entry[1] for entry in procs}
    done: dict[int, tuple[int, float]] = {}
    while pending:
        for key, proc in list(pending.items()):
            rc = proc.poll()
            if rc is not None:
                done[key] = (rc, time.monotonic())
                del pending[key]
        if pending:
            time.sleep(poll_s)
    return done


def run_targets(targets: list[Target], seconds: float,
                device_numbers: list[int], extra: list[str], out_dir: Path,
                record_dir: Path | None = None, baud: int = 115200,
                prefix: str = "", verbose: bool = True,
                poll_s: float = 0.2) -> list[dict]:
    """Launch one ant_verify.py per target, concurrently, and collect them.

    Returns one entry per target, in the order given: `target`, `exit`,
    `json`, `log`, `wall_s` (how long THAT child ran, to within `poll_s` - a
    dropped COM port gives a short run whose JSON can still look like a pass;
    see wait_each() for why it is per child), and either `data` (the child's
    JSON) or `error`.

    `prefix` is prepended to every file stem so one results directory can hold
    many windows (the orchestrator's L1/L2/acquire-cycle files) without one
    overwriting another.
    """
    if "--record" in extra:
        raise ValueError("--record in extra sends every target to one file; "
                         "use record_dir")
    out_dir.mkdir(parents=True, exist_ok=True)
    if record_dir is not None:
        record_dir.mkdir(parents=True, exist_ok=True)

    procs = []
    for t in targets:
        slug = prefix + slug_of(t.name)
        json_path = out_dir / f"{slug}.json"
        log_path = out_dir / f"{slug}.log"
        # A stale file from an earlier run would be read back as this run's.
        json_path.unlink(missing_ok=True)
        record_path = (record_dir / f"{slug}.antcap"
                       if record_dir is not None else None)
        cmd = child_command(t, seconds, device_numbers, extra, json_path,
                            baud, record_path)
        log_f = open(log_path, "w", encoding="utf-8")
        if verbose:
            print(f"launching {t.name}: {' '.join(cmd)}")
        started = time.monotonic()
        proc = subprocess.Popen(cmd, stdout=log_f, stderr=subprocess.STDOUT)
        procs.append((t, proc, json_path, log_path, log_f, started))

    if verbose:
        print(f"\n{len(procs)} target(s) running concurrently for "
              f"{seconds:.0f} s ...")
    finished = wait_each(procs, poll_s)
    results = []
    for t, proc, json_path, log_path, log_f, started in procs:
        rc, ended = finished[id(proc)]
        wall = ended - started
        log_f.close()
        entry = {"target": t.name, "exit": rc, "json": str(json_path),
                 "log": str(log_path), "wall_s": wall}
        if json_path.exists():
            try:
                entry["data"] = json.loads(json_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as e:
                entry["error"] = f"bad json: {e}"
        else:
            entry["error"] = "no json written - see log"
        results.append(entry)
    return results


def _fmt(value, spec: str) -> str:
    return format(value, spec) if value is not None else "n/a"


def print_summary(results: list[dict]) -> None:
    header = (f"{'target':<24} {'loss_exact%':>11} {'unacct':>7} "
              f"{'rssi dBm':>9} {'ttfp s':>7} {'pass':>5}")
    print(header)
    print("-" * len(header))
    for r in results:
        data = r.get("data", {})
        d = data.get("derived", {})
        unacct = data.get("accounting", {}).get("unaccounted")
        pass_s = "OK" if data.get("pass") else "FAIL" if "data" in r else "ERR"
        print(f"{r['target']:<24} {_fmt(d.get('loss_exact_pct'), '.2f'):>11} "
              f"{_fmt(unacct, 'd'):>7} {_fmt(d.get('rssi_dbm_mean'), '.1f'):>9} "
              f"{_fmt(d.get('time_to_first_packet_s'), '.2f'):>7} {pass_s:>5}")

    # Per channel, only when there is more than one: on one channel it would
    # restate the row above. The spread between receivers on the SAME channel
    # is the comparison; the spread between channels on one receiver is a
    # scheduling effect.
    multi = [r for r in results
             if len(r.get("data", {}).get("derived", {})
                    .get("per_channel", {})) > 1]
    if not multi:
        return
    print("\nper channel, exact loss %:")
    keys: list[str] = []
    labels: dict[str, str] = {}
    for r in multi:
        for key, entry in r["data"]["derived"]["per_channel"].items():
            if key not in keys:
                keys.append(key)
                labels[key] = (f"ch{key} {entry.get('profile') or ''} "
                               f"#{entry.get('device_number')}").strip()
    print(f"  {'target':<24} " + " ".join(f"{labels[k]:>22}" for k in keys))
    for r in multi:
        per = r["data"]["derived"]["per_channel"]
        cells = []
        for k in keys:
            entry = per.get(k)
            cells.append(_fmt(entry.get("loss_exact_pct") if entry else None,
                              ".2f"))
        print(f"  {r['target']:<24} " + " ".join(f"{c:>22}" for c in cells))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", type=parse_target, action="append", required=True,
                         help="NAME=usb:SERIAL or NAME=port:COMn; repeatable")
    parser.add_argument("--seconds", type=float, default=300.0)
    parser.add_argument("--device-number", type=int, action="append", default=[],
                         help="forwarded to every child as --device-number "
                              "(repeat to match ant_verify.py's multi-channel form)")
    parser.add_argument("--out-dir", default=None,
                         help="where to write each target's --json/log; "
                              "defaults to a fresh temp directory")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--record-dir", default=None,
                         help="give each child its own --record "
                              "<dir>/<target>.antcap. Do not pass --record "
                              "after --: it would send every target to the "
                              "same file")
    parser.add_argument("extra", nargs=argparse.REMAINDER,
                         help="anything after -- is forwarded to every child ant_verify.py")
    args = parser.parse_args()

    extra = args.extra
    if extra and extra[0] == "--":
        extra = extra[1:]

    if "--record" in extra:
        parser.error("--record after -- sends every target to one file; "
                     "use --record-dir")

    out_dir = Path(args.out_dir) if args.out_dir else Path(tempfile.mkdtemp(prefix="ant_multi_"))
    record_dir = Path(args.record_dir) if args.record_dir else None

    results = run_targets(args.target, args.seconds, args.device_number,
                          extra, out_dir, record_dir, args.baud)

    print()
    print_summary(results)

    print(f"\nresults directory: {out_dir}")
    return 0 if all(r.get("data", {}).get("pass") for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
