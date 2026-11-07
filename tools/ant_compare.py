#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Head-to-head receiver comparison against a reference dongle, in one window.

A loss figure taken on its own means nothing on this bench: run-to-run drift
(0.4 % one day, 5-8 % another) is as large as any effect worth looking for.
So every comparison here is made **in the same window** - every receiver hears
the same transmitter at the same moment - and only the gap between a receiver
and the reference carries weight. That is what tools/ant_multi_verify.py does
for loss; this wraps it, together with the sensitivity ladder, acquisition
timing, acknowledged transfers and the USB round trip, behind one rig file.

    python tools/ant_compare.py --rig tools/compare_rigs/2026-11-07-fixed.toml
    python tools/ant_compare.py --rig ... --phases identity,loss --rounds A \\
        --set phases.loss.seconds=30 --out C:/tmp/shakeout
    python tools/ant_compare.py --rig ... --report-only   # re-derive verdicts

The rig file (TOML) names the devices, which one is the reference, the rounds
(which DK transmits in each) and the phase settings, plus a free-form
placement block. It is copied into the results directory verbatim, so a
later re-placement run - copy the rig, edit the placement and addresses,
rerun - can be set beside this one.

Phases, each optional (--phases):

  identity  ant_probe + ant_features on every device; asserts every
            stack="radiant" device reports a RADIANT version string
  loss      ant_sim power master on the round's DK (a background subprocess,
            run to its own end and never killed), every receiver through
            ant_multi_verify.run_targets for the window; repeated
            (`repeats`, default 2) so the gap can be shown to repeat
  multi     power + heart-rate + speed/cadence + FE-C (the Zwift shape), every
            receiver tracking all four; in a two-transmitter round the
            profiles are split across the DKs, so mixed periods on two
            independent clocks give cross-board overlaps one board never can
  acquire   N short ant_verify windows against a transmitter that stays up;
            time_to_first_packet_s per receiver per cycle
  ladder    tools/ant_sens.py with every receiver as --rx, one knee each
  acked     tools/ant_ack_pair.py, each receiver in turn as the originator
            against the DK master, interleaved in short blocks
            (A B C D A B C D) so drift lands on everyone equally
  usb       tools/ant_bench.py on the USB receivers (UART DKs excluded)

Verdicts are pre-registered in VERDICT_RULES below and applied per round,
per receiver, against the reference measured in the same window. Each comes
out Equal (non-inferior), Better, Worse or Inconclusive - and the summary
says whichever it is.

Results: one directory holding every child's JSON and log, `summary.json`
(records + derived metrics + verdicts) and `summary.md`. Re-running a subset
into the same directory replaces just those records (keyed round/phase/rep)
and re-derives the verdicts from everything present.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import re
import shutil
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

# ant_multi_verify imports nothing that needs a radio, so this module stays
# importable (and testable) on a host without pyusb.
from ant_multi_verify import Target, parse_target, run_targets  # noqa: E402

PHASES = ("identity", "loss", "multi", "acquire", "ladder", "acked", "usb")

# The Zwift shape. ant_sim and ant_verify share these names (ant_pages.PROFILES
# and ant_sim.SENSORS); fec-treadmill is the only FE-C master ant_sim carries,
# and device type 0x11 / 4 Hz is what the receiver tracks either way.
DEFAULT_MULTI_PROFILES = ("power", "heart-rate", "csc", "fec-treadmill")

# Device-number offsets within one round's base, one block per phase, so a
# master left on air by one phase cannot be acquired by the next by mistake.
PHASE_DN_OFFSET = {"loss": 0, "multi": 10, "acquire": 20, "ladder": 30,
                   "acked": 40}

# Never the sim's default: #14871 is CONFIG_ANT_SIM_DEVICE_NUMBER and
# ant_ack_pair's, so a wildcard or stale master can answer to it.
FORBIDDEN_DEVICE_NUMBERS = {0, 14871, 0x3A17}

VERDICT_RULES = {
    "loss": ("Wilson 95 % CI on exact loss. Equal: RadiANT upper CI <= "
             "reference point + 0.5 pp. Better: RadiANT upper CI < reference "
             "lower CI. Worse: RadiANT lower CI > reference point + 0.5 pp."),
    "acquire": ("Median time to first packet over the cycles. Equal: <= "
                "reference median + 0.25 s. Better: lower median AND "
                "RadiANT Q3 < reference Q1. Worse: > reference median + "
                "0.25 s."),
    "ladder": ("5 %-loss TX power. Equal: within 1 dB (the ab_gates "
               "repeatability limit). Better: more than 1 dB lower. Worse: "
               "more than 1 dB higher. Inconclusive if either dial check "
               "failed or either ladder never reached its knee."),
    "acked": ("Wilson 95 % CI on the acknowledged-transfer failure rate "
              "(sent - completed), the loss rule applied unchanged."),
    "usb": ("ant_bench round-trip latency. Equal: median <= reference median "
            "x 1.1. Better: lower median AND lower p99. Worse: median > "
            "reference median x 1.1."),
}

LOSS_MARGIN_PP = 0.5
ACQUIRE_MARGIN_S = 0.25
KNEE_MARGIN_DB = 1.0
USB_MEDIAN_FACTOR = 1.1
Z95 = 1.959963984540054

EQUAL, BETTER, WORSE, INCONCLUSIVE = "Equal", "Better", "Worse", "Inconclusive"


# ---------------------------------------------------------------------------
# The rig
# ---------------------------------------------------------------------------

class RigError(ValueError):
    pass


@dataclass
class Device:
    name: str
    target: Target
    stack: str                      # "radiant" or "reference"
    part: str | None = None
    receive: bool = True
    transmit: bool = False
    device_number_base: int | None = None
    baud: int = 115200
    notes: str = ""

    @property
    def address(self) -> str:
        return f"{self.target.kind}:{self.target.value}"

    @property
    def is_usb(self) -> bool:
        return self.target.kind == "usb"


@dataclass
class Round:
    name: str
    transmitters: list[str]
    phases: list[str]
    device_number_offset: int = 0
    multi_split: dict[str, str] = field(default_factory=dict)
    receivers: list[str] | None = None
    notes: str = ""
    repeats: dict[str, int] = field(default_factory=dict)


@dataclass
class Rig:
    name: str
    path: Path | None
    devices: dict[str, Device]
    rounds: list[Round]
    phases: dict[str, dict]
    placement: dict
    results_dir: str | None
    caveats: list[str]
    raw: dict

    @property
    def reference(self) -> Device:
        return next(d for d in self.devices.values()
                    if d.stack == "reference")

    def round(self, name: str) -> Round:
        for r in self.rounds:
            if r.name == name:
                return r
        raise RigError(f"no round {name!r}; the rig has "
                       f"{[r.name for r in self.rounds]}")

    def receivers_for(self, rnd: Round) -> list[Device]:
        """Every receiving device that is not transmitting this round, in rig
        order, or the round's explicit list. The reference is always first
        among them, so its column leads every table."""
        if rnd.receivers is not None:
            chosen = [self.devices[n] for n in rnd.receivers]
        else:
            chosen = [d for d in self.devices.values()
                      if d.receive and d.name not in rnd.transmitters]
        return sorted(chosen, key=lambda d: d.stack != "reference")

    def phase(self, name: str) -> dict:
        return self.phases.get(name, {})

    def device_number(self, rnd: Round, tx: str, phase: str) -> int:
        base = self.devices[tx].device_number_base
        return base + rnd.device_number_offset + PHASE_DN_OFFSET[phase]


PHASE_DEFAULTS: dict[str, dict] = {
    "loss": {"seconds": 300.0, "margin_s": 30.0, "repeats": 2,
             "profile": "power", "max_gap_spread_pp": 1.0},
    "multi": {"seconds": 300.0, "margin_s": 30.0, "repeats": 1,
              "profiles": list(DEFAULT_MULTI_PROFILES)},
    "acquire": {"cycles": 20, "seconds": 10.0, "cycle_overhead_s": 8.0,
                "margin_s": 30.0},
    "ladder": {"seconds": 60.0, "settle": 2.0, "rungs": "fine",
               "top_dbm": None, "bottom_dbm": None},
    "acked": {"block_seconds": 60.0, "passes": 2, "interval": 1.0,
              "tx_power_level": 3, "acquire_seconds": 20.0},
    "usb": {"count": 500, "depth": 8},
    "housekeeping": {"reset_retries": 6, "reset_pause_s": 2.0,
                     "sim_start_timeout_s": 25.0},
}


def _merge_defaults(phases: dict) -> dict:
    merged = {}
    for name, defaults in PHASE_DEFAULTS.items():
        given = phases.get(name, {})
        if not isinstance(given, dict):
            raise RigError(f"[phases.{name}] must be a table")
        unknown = set(given) - set(defaults)
        if unknown:
            raise RigError(f"[phases.{name}] has unknown keys "
                           f"{sorted(unknown)}; known: {sorted(defaults)}")
        merged[name] = {**defaults, **given}
    extra = set(phases) - set(PHASE_DEFAULTS)
    if extra:
        raise RigError(f"[phases] has unknown tables {sorted(extra)}")
    return merged


def apply_overrides(raw: dict, overrides: list[str]) -> dict:
    """`--set a.b.c=VALUE`, VALUE parsed as a TOML value (so 30, 30.0, "x",
    true and [1, 2] all mean what they look like; a bare word is a string)."""
    for item in overrides:
        if "=" not in item:
            raise RigError(f"--set needs KEY=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        try:
            parsed = tomllib.loads(f"v = {value}")["v"]
        except tomllib.TOMLDecodeError:
            parsed = value
        node = raw
        parts = key.split(".")
        for part in parts[:-1]:
            node = node.setdefault(part, {})
            if not isinstance(node, dict):
                raise RigError(f"--set {key}: {part} is not a table")
        node[parts[-1]] = parsed
    return raw


def parse_rig(raw: dict, path: Path | None = None) -> Rig:
    """Validate a rig document. Every refusal names the field, because a rig
    that half-parses runs a bench session that measures the wrong thing."""
    meta = raw.get("meta", {})
    name = meta.get("name") or (path.stem if path else "rig")

    devices: dict[str, Device] = {}
    for entry in raw.get("device", []):
        try:
            dname = entry["name"]
            target = parse_target(f"{dname}={entry['address']}")
        except KeyError as exc:
            raise RigError(f"[[device]] is missing {exc}") from None
        except argparse.ArgumentTypeError as exc:
            raise RigError(str(exc)) from None
        if dname in devices:
            raise RigError(f"device name {dname!r} appears twice")
        stack = entry.get("stack", "radiant")
        if stack not in ("radiant", "reference"):
            raise RigError(f"device {dname}: stack must be radiant or "
                           f"reference, got {stack!r}")
        base = entry.get("device_number_base")
        devices[dname] = Device(
            name=dname, target=target, stack=stack, part=entry.get("part"),
            receive=bool(entry.get("receive", True)),
            transmit=bool(entry.get("transmit", False)),
            device_number_base=base, baud=int(entry.get("baud", 115200)),
            notes=entry.get("notes", ""))

    if not devices:
        raise RigError("the rig names no [[device]]")
    refs = [d for d in devices.values() if d.stack == "reference"]
    if len(refs) != 1:
        raise RigError(f"exactly one device must have stack = \"reference\"; "
                       f"found {[d.name for d in refs]}")
    if not refs[0].receive:
        raise RigError(f"the reference {refs[0].name} must receive - every "
                       f"verdict is against it")
    if refs[0].transmit:
        raise RigError(f"the reference {refs[0].name} must not transmit: a "
                       f"round where it transmits has nothing to compare "
                       f"against")
    addresses = [d.address for d in devices.values()]
    if len(set(addresses)) != len(addresses):
        raise RigError(f"two devices share an address: {addresses}")

    phases = _merge_defaults(raw.get("phases", {}))

    rounds: list[Round] = []
    for index, entry in enumerate(raw.get("round", [])):
        rname = entry.get("name")
        if not rname:
            raise RigError(f"[[round]] #{index + 1} has no name")
        if any(r.name == rname for r in rounds):
            raise RigError(f"round {rname!r} appears twice")
        txs = list(entry.get("transmitters", []))
        if not txs:
            raise RigError(f"round {rname}: no transmitters")
        for tx in txs:
            dev = devices.get(tx)
            if dev is None:
                raise RigError(f"round {rname}: transmitter {tx!r} is not a "
                               f"[[device]]")
            if not dev.transmit:
                raise RigError(f"round {rname}: {tx} is not marked "
                               f"transmit = true")
            if dev.device_number_base is None:
                raise RigError(f"device {tx}: a transmitter needs "
                               f"device_number_base")
        rphases = list(entry.get("phases", []))
        bad = [p for p in rphases if p not in PHASES or p in ("identity",
                                                              "usb")]
        if bad:
            raise RigError(f"round {rname}: phases {bad} are not per-round "
                           f"phases (identity and usb run once, outside any "
                           f"round)")
        if "ladder" in rphases:
            if len(txs) != 1:
                raise RigError(f"round {rname}: a ladder needs exactly one "
                               f"transmitter (one power dial)")
            if not devices[txs[0]].part:
                raise RigError(f"device {txs[0]}: the ladder needs part = "
                               f"... for its TXPOWER register table")
        if "acked" in rphases and len(txs) != 1:
            raise RigError(f"round {rname}: acked needs exactly one "
                           f"transmitter (the master the originators target)")
        split = dict(entry.get("multi_split", {}))
        if "multi" in rphases and len(txs) > 1:
            profiles = phases["multi"]["profiles"]
            missing = [p for p in profiles if p not in split]
            if missing:
                raise RigError(f"round {rname}: two transmitters, so "
                               f"multi_split must say which one runs each "
                               f"profile; missing {missing}")
        for prof, tx in split.items():
            if tx not in txs:
                raise RigError(f"round {rname}: multi_split sends {prof} to "
                               f"{tx}, which does not transmit this round")
        receivers = entry.get("receivers")
        if receivers is not None:
            for rx in receivers:
                if rx not in devices:
                    raise RigError(f"round {rname}: receiver {rx!r} is not a "
                                   f"[[device]]")
                if rx in txs:
                    raise RigError(f"round {rname}: {rx} cannot both "
                                   f"transmit and receive")
            if refs[0].name not in receivers:
                raise RigError(f"round {rname}: the reference "
                               f"{refs[0].name} must be among the receivers")
        offset = int(entry.get("device_number_offset", index * 100))
        repeats = {k: int(v) for k, v in entry.get("repeats", {}).items()}
        for k, v in repeats.items():
            if k not in ("loss", "multi") or v < 1:
                raise RigError(f"round {rname}: repeats applies to loss and "
                               f"multi, at least 1; got {k} = {v}")
        rnd = Round(name=rname, transmitters=txs, phases=rphases,
                    device_number_offset=offset, multi_split=split,
                    receivers=list(receivers) if receivers else None,
                    notes=entry.get("notes", ""), repeats=repeats)
        rounds.append(rnd)

    rig = Rig(name=name, path=path, devices=devices, rounds=rounds,
              phases=phases, placement=raw.get("placement", {}),
              results_dir=meta.get("results_dir"),
              caveats=list(meta.get("caveats", [])), raw=raw)

    # Device numbers: in range, never a default, never shared between two
    # transmitters' blocks anywhere in the rig.
    seen: dict[int, str] = {}
    for rnd in rounds:
        for tx in rnd.transmitters:
            for phase in PHASE_DN_OFFSET:
                for k in range(4):
                    dn = rig.device_number(rnd, tx, phase) + k
                    if not 1 <= dn <= 0xFFFF:
                        raise RigError(f"round {rnd.name}, {tx}: device "
                                       f"number {dn} is out of range")
                    if dn in FORBIDDEN_DEVICE_NUMBERS:
                        raise RigError(f"round {rnd.name}, {tx}: device "
                                       f"number {dn} is a tool default - pin "
                                       f"something nothing else uses")
                    owner = f"{rnd.name}/{tx}/{phase}"
                    if dn in seen and seen[dn] != owner:
                        raise RigError(f"device number {dn} is used by both "
                                       f"{seen[dn]} and {owner}")
                    seen[dn] = owner
    return rig


def load_rig(path: Path, overrides: list[str] | None = None) -> Rig:
    with open(path, "rb") as handle:
        raw = tomllib.load(handle)
    raw = apply_overrides(raw, overrides or [])
    return parse_rig(raw, path)


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def wilson(k: int, n: int, z: float = Z95) -> tuple[float, float, float] | None:
    """(point, lower, upper) in PERCENT, Wilson score interval. None for n=0.

    Wilson rather than the normal approximation because the interesting
    figures are near 0 %, where the normal interval goes negative and is
    narrowest exactly where it is least right."""
    if n <= 0:
        return None
    if not 0 <= k <= n:
        raise ValueError(f"k={k} outside 0..n={n}")
    p = k / n
    denom = 1.0 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    # At k = 0 (k = n) the bound is exactly 0 (100) %; the arithmetic leaves
    # a 1e-17 residue that would print as a nonzero lower bound.
    lo = 0.0 if k == 0 else max(0.0, centre - half)
    hi = 1.0 if k == n else min(1.0, centre + half)
    return 100.0 * p, 100.0 * lo, 100.0 * hi


def quantile(values: list[float], q: float) -> float | None:
    """Linear-interpolated quantile that tolerates +inf (a missed acquisition
    is censored at infinity, not dropped: dropping misses would flatter the
    receiver that misses most)."""
    if not values:
        return None
    ordered = sorted(values)
    pos = q * (len(ordered) - 1)
    lo = math.floor(pos)
    hi = math.ceil(pos)
    a, b = ordered[lo], ordered[hi]
    if lo == hi or a == b:
        return a
    return a + (b - a) * (pos - lo)


# ---------------------------------------------------------------------------
# Verdict rules - pure, one per metric
# ---------------------------------------------------------------------------

def verdict_rate(rx: tuple[int, int] | None, ref: tuple[int, int] | None,
                 margin_pp: float = LOSS_MARGIN_PP) -> tuple[str, str]:
    """Loss-shaped rule on (failures, trials). Lower is better."""
    if rx is None or ref is None:
        return INCONCLUSIVE, "a figure is missing"
    r = wilson(*rx)
    g = wilson(*ref)
    if r is None or g is None:
        return INCONCLUSIVE, "no trials on one side"
    rp, rlo, rhi = r
    gp, glo, ghi = g
    detail = (f"{rp:.2f} % [{rlo:.2f}, {rhi:.2f}] vs reference "
              f"{gp:.2f} % [{glo:.2f}, {ghi:.2f}]")
    if rhi < glo:
        return BETTER, detail
    if rhi <= gp + margin_pp:
        return EQUAL, detail
    if rlo > gp + margin_pp:
        return WORSE, detail
    return INCONCLUSIVE, detail + (f"; the interval straddles the "
                                   f"{margin_pp} pp margin")


def acquire_stats(values: list[float | None]) -> dict:
    data = [math.inf if v is None else float(v) for v in values]
    finite = [v for v in data if math.isfinite(v)]
    return {"n": len(data), "acquired": len(finite),
            "misses": len(data) - len(finite),
            "median": quantile(data, 0.5), "q1": quantile(data, 0.25),
            "q3": quantile(data, 0.75)}


def verdict_acquire(rx: list[float | None] | None,
                    ref: list[float | None] | None,
                    margin_s: float = ACQUIRE_MARGIN_S) -> tuple[str, str]:
    if not rx or not ref:
        return INCONCLUSIVE, "a figure is missing"
    r = acquire_stats(rx)
    g = acquire_stats(ref)
    for who, s in (("receiver", r), ("reference", g)):
        need = max(5, (s["n"] + 1) // 2)
        if s["acquired"] < need:
            return INCONCLUSIVE, (f"the {who} acquired on {s['acquired']} of "
                                  f"{s['n']} cycles; the rule needs at least "
                                  f"{need}")
    detail = (f"median {r['median']:.2f} s (IQR {r['q1']:.2f}-{r['q3']:.2f}, "
              f"{r['misses']} miss) vs reference {g['median']:.2f} s "
              f"(IQR {g['q1']:.2f}-{g['q3']:.2f}, {g['misses']} miss)")
    if r["median"] < g["median"] and r["q3"] < g["q1"]:
        return BETTER, detail
    if r["median"] <= g["median"] + margin_s:
        return EQUAL, detail
    return WORSE, detail


def verdict_knee(rx: dict | None, ref: dict | None,
                 margin_db: float = KNEE_MARGIN_DB) -> tuple[str, str]:
    """rx/ref: {"knee_dbm": float|None, "dial_trustworthy": bool}."""
    if not rx or not ref:
        return INCONCLUSIVE, "a figure is missing"
    for who, s in (("receiver", rx), ("reference", ref)):
        if s.get("knee_dbm") is None:
            return INCONCLUSIVE, (f"the {who}'s ladder never reached its "
                                  f"knee: {s.get('reason', '')}").rstrip(": ")
        if not s.get("dial_trustworthy"):
            return INCONCLUSIVE, f"the {who}'s dial (RSSI slope) check failed"
    delta = rx["knee_dbm"] - ref["knee_dbm"]
    detail = (f"{rx['knee_dbm']:.2f} dBm vs reference {ref['knee_dbm']:.2f} "
              f"dBm ({delta:+.2f} dB)")
    if delta < -margin_db:
        return BETTER, detail
    if abs(delta) <= margin_db:
        return EQUAL, detail
    return WORSE, detail


def effective_dial(results: dict[str, dict]) -> dict[str, dict]:
    """The dial check judges the TRANSMITTER (did the ladder move the power it
    asked for), so it is one fact per ladder, not per receiver.

    A receiver that reports no RSSI (the Garmin USB-m returns the channel id
    but no RSSI) can never fit a slope of its own, and left alone would make
    every knee verdict against it Inconclusive for a reason that has nothing
    to do with it. Such a receiver inherits the ladder's verdict: trustworthy
    only if at least one receiver could fit a slope and every receiver that
    could, passed. A receiver that fitted a slope keeps its own verdict.
    """
    fitted = [r for r in results.values()
              if (r.get("rssi_slope") or {}).get("slope") is not None]
    ladder_ok = bool(fitted) and all(r.get("dial_trustworthy")
                                     for r in fitted)
    out = {}
    for name, r in results.items():
        r = dict(r)
        if (r.get("rssi_slope") or {}).get("slope") is None and fitted:
            r["dial_trustworthy"] = ladder_ok
            r["dial_basis"] = "inherited from the receivers that report RSSI"
        out[name] = r
    return out


def verdict_usb(rx: dict | None, ref: dict | None,
                factor: float = USB_MEDIAN_FACTOR) -> tuple[str, str]:
    """rx/ref: {"p50": ms, "p99": ms}."""
    if not rx or not ref or rx.get("p50") is None or ref.get("p50") is None:
        return INCONCLUSIVE, "a figure is missing"
    detail = (f"p50 {rx['p50']:.3f} ms / p99 {rx['p99']:.3f} ms vs reference "
              f"p50 {ref['p50']:.3f} / p99 {ref['p99']:.3f}")
    if rx["p50"] < ref["p50"] and rx["p99"] < ref["p99"]:
        return BETTER, detail
    if rx["p50"] <= ref["p50"] * factor:
        return EQUAL, detail
    return WORSE, detail


# ---------------------------------------------------------------------------
# Reading child JSON
# ---------------------------------------------------------------------------

def verify_counts(data: dict | None) -> dict:
    """Loss counts out of one ant_verify JSON, exact where the transmitter's
    event counter allows it and wall-clock otherwise - labelled, because the
    two are different instruments.

    Returns {"missed", "sent", "basis", "per_channel": {...}} with missed/sent
    None when nothing could be counted. Pooled over channels as one ratio
    (sum missed / sum sent), the same composition ant_verify.derive() uses.
    """
    if not data:
        return {"missed": None, "sent": None, "basis": None,
                "per_channel": {}}
    per = data.get("derived", {}).get("per_channel", {})
    channels = {str(c.get("channel")): c for c in data.get("channels", [])}
    missed = sent = 0
    bases = set()
    out: dict[str, dict] = {}
    for key, entry in per.items():
        if entry.get("exact_sent"):
            m, s, basis = entry["exact_missed"], entry["exact_sent"], "exact"
        else:
            ch = channels.get(key, {})
            expected = ch.get("expected_packets")
            packets = entry.get("packets", 0)
            if not expected:
                out[key] = {"profile": entry.get("profile"), "missed": None,
                            "sent": None, "basis": None}
                continue
            s = int(round(expected))
            m = max(0, s - packets)
            m = min(m, s)
            basis = "wall_clock"
        out[key] = {"profile": entry.get("profile"), "missed": m, "sent": s,
                    "basis": basis,
                    "loss_pct": 100.0 * m / s if s else None,
                    "device_number": entry.get("device_number")}
        missed += m
        sent += s
        bases.add(basis)
    if not sent:
        return {"missed": None, "sent": None, "basis": None,
                "per_channel": out}
    return {"missed": missed, "sent": sent,
            "basis": "+".join(sorted(bases)), "per_channel": out}


def window_sanity(data: dict | None, pinned: list[int], seconds: float,
                  wall_s: float | None) -> list[str]:
    """What makes a window untrustworthy, as strings. Empty means clean."""
    if data is None:
        return ["no JSON written"]
    problems = []
    accounting = data.get("accounting") or {}
    unacct = accounting.get("unaccounted")
    if unacct and "collisions" not in accounting:
        # A document written before ant_verify counted CHANNEL_COLLISION as
        # the stack owning up to a slot: subtract them here, or a receiver
        # whose own channels contend reads as a host-path fault.
        collided = sum(n for name, n in (data.get("events") or {}).items()
                       if name in ("EVENT_CHANNEL_COLLISION",
                                   "CHANNEL_COLLISION"))
        unacct = max(0, unacct - collided)
    if unacct:
        problems.append(f"unaccounted={unacct} (loss after the radio)")
    identities = data.get("identities", {})
    for index, dn in enumerate(pinned):
        heard = identities.get(str(index), {}).get("device_number")
        if heard is None:
            problems.append(f"ch{index}: never identified a device")
        elif heard != dn:
            problems.append(f"ch{index}: heard #{heard}, pinned #{dn}")
    if wall_s is not None and wall_s < 0.95 * seconds:
        problems.append(f"ran {wall_s:.0f} s of a {seconds:.0f} s window "
                        f"(dropped port?)")
    return problems


def parse_probe_log(text: str) -> dict:
    version = re.search(r"OK: '([^']*)'", text)
    caps = re.search(r"max channels: (\d+), max networks: (\d+)", text)
    return {"version": version.group(1) if version else None,
            "channels": int(caps.group(1)) if caps else None,
            "networks": int(caps.group(2)) if caps else None,
            "pass": "PASS:" in text}


def parse_sim_log(text: str) -> dict:
    fallbacks = sum(int(n) for n in
                    re.findall(r"(\d+) sent on the wall-clock fallback", text))
    opened = re.findall(r"OK: channel \d+ - .*?device #(\d+)", text)
    return {"fallbacks": fallbacks,
            "device_numbers": [int(d) for d in opened],
            "paced": "PASS: every message was paced by EVENT_TX" in text}


# ---------------------------------------------------------------------------
# Summary: records -> metrics -> verdicts (pure; no hardware)
# ---------------------------------------------------------------------------

def record_key(record: dict) -> str:
    return "/".join(str(record.get(k, "")) for k in ("round", "phase", "rep"))


def _paired_rate(records: list[dict], rx: str, ref: str,
                 field_name: str = "counts") -> tuple[tuple[int, int] | None,
                                                      tuple[int, int] | None,
                                                      list[dict]]:
    """Pool (missed, sent) over the windows where BOTH rx and the reference
    produced a figure. A window one side lacks is dropped for both: pooling
    unequal windows would compare two different rooms, which is what the
    same-window design exists to avoid."""
    rk = rn = gk = gn = 0
    used = []
    for rec in records:
        a = rec["results"].get(rx, {}).get(field_name, {})
        b = rec["results"].get(ref, {}).get(field_name, {})
        if a.get("sent") and b.get("sent"):
            rk += a["missed"]
            rn += a["sent"]
            gk += b["missed"]
            gn += b["sent"]
            used.append({"rep": rec.get("rep"),
                         "rx_pct": 100.0 * a["missed"] / a["sent"],
                         "ref_pct": 100.0 * b["missed"] / b["sent"]})
    if not rn or not gn:
        return None, None, used
    return (rk, rn), (gk, gn), used


def summarise_round(rig: Rig, rnd: Round, records: list[dict]) -> dict:
    ref = rig.reference.name
    receivers = [d.name for d in rig.receivers_for(rnd)]
    mine = [r for r in records if r.get("round") == rnd.name]
    by_phase: dict[str, list[dict]] = {}
    for rec in mine:
        by_phase.setdefault(rec["phase"], []).append(rec)

    metrics: dict[str, dict] = {}
    verdicts: list[dict] = []
    warnings: list[str] = []
    for rec in mine:
        for w in rec.get("warnings", []):
            warnings.append(f"{rec['phase']} {rec.get('rep', '')}: {w}".strip())

    def add(metric: str, rx: str, result: tuple[str, str], extra=None):
        verdicts.append({"round": rnd.name, "metric": metric, "receiver": rx,
                         "verdict": result[0], "detail": result[1],
                         **(extra or {})})

    radiant = [rx for rx in receivers
               if rx != ref and rig.devices[rx].stack == "radiant"]

    # loss and multi share the rate rule
    for phase, label in (("loss", "loss"), ("multi", "loss (multi, pooled)")):
        recs = sorted(by_phase.get(phase, []), key=lambda r: r.get("rep", 0))
        if not recs:
            continue
        per_rx = {}
        for rx in receivers:
            reps = []
            for rec in recs:
                c = rec["results"].get(rx, {}).get("counts", {})
                reps.append(100.0 * c["missed"] / c["sent"]
                            if c.get("sent") else None)
            per_rx[rx] = reps
        metrics[label] = per_rx
        if phase == "multi":
            # Per profile, pooled over windows: a scheduler effect lands on
            # one channel of a set, which the pooled figure dilutes.
            per_profile: dict[str, dict] = {}
            for rx in receivers:
                pooled: dict[str, list[int]] = {}
                bases: dict[str, str] = {}
                for rec in recs:
                    pc = (rec["results"].get(rx, {}).get("counts", {})
                          .get("per_channel", {}))
                    for entry in pc.values():
                        if not entry.get("sent"):
                            continue
                        acc = pooled.setdefault(entry["profile"], [0, 0])
                        acc[0] += entry["missed"]
                        acc[1] += entry["sent"]
                        bases[entry["profile"]] = entry["basis"]
                per_profile[rx] = {
                    prof: {"loss_pct": 100.0 * m / s, "basis": bases[prof]}
                    for prof, (m, s) in pooled.items()}
            metrics["multi per profile"] = per_profile
        tainted = any(rec.get("warnings") for rec in recs)
        for rx in radiant:
            a, b, used = _paired_rate(recs, rx, ref)
            result = verdict_rate(a, b)
            extra = {"windows": len(used)}
            # Does the GAP repeat? Not the absolute figure - the room moves
            # that by several points between windows and it carries no
            # weight here; the receiver-minus-reference gap is the claim.
            if len(used) >= 2 and phase == "loss":
                gaps = [u["rx_pct"] - u["ref_pct"] for u in used]
                spread = max(gaps) - min(gaps)
                extra["gap_spread_pp"] = spread
                limit = rig.phase("loss")["max_gap_spread_pp"]
                if spread > limit and result[0] != INCONCLUSIVE:
                    result = (INCONCLUSIVE,
                              result[1] + f"; but the gap moved {spread:.2f} pp "
                              f"between windows (limit {limit} pp), so it "
                              f"does not repeat")
            if tainted and result[0] != INCONCLUSIVE:
                result = (result[0], result[1] + " (window warnings present - "
                          "see below)")
            add(label, rx, result, extra)

    recs = by_phase.get("acquire", [])
    if recs:
        series = {rx: [] for rx in receivers}
        for rec in recs:
            for cycle in rec.get("cycles", []):
                for rx in receivers:
                    if rx in cycle:
                        series[rx].append(cycle[rx])
        metrics["acquire"] = {rx: acquire_stats(v) for rx, v in series.items()
                              if v}
        for rx in radiant:
            add("acquire", rx, verdict_acquire(series.get(rx),
                                               series.get(ref)))

    recs = by_phase.get("ladder", [])
    if recs:
        rec = recs[-1]
        results = effective_dial(rec["results"])
        metrics["ladder"] = results
        for rx in radiant:
            add("ladder", rx, verdict_knee(results.get(rx), results.get(ref)))

    recs = by_phase.get("acked", [])
    if recs:
        totals: dict[str, dict] = {}
        for rec in recs:
            for block in rec.get("blocks", []):
                t = totals.setdefault(block["receiver"],
                                      {"sent": 0, "completed": 0,
                                       "failed": 0, "peer_received": 0,
                                       "blocks": 0, "errors": 0})
                for k in ("sent", "completed", "failed", "peer_received"):
                    t[k] += block.get(k) or 0
                t["blocks"] += 1
                t["errors"] += len(block.get("errors", []))
        metrics["acked"] = totals

        def rate(name):
            t = totals.get(name)
            if not t or not t["sent"]:
                return None
            return (t["sent"] - t["completed"], t["sent"])
        for rx in radiant:
            add("acked", rx, verdict_rate(rate(rx), rate(ref)))

    return {"round": rnd.name, "transmitters": rnd.transmitters,
            "receivers": receivers, "metrics": metrics, "verdicts": verdicts,
            "warnings": warnings}


def summarise_usb(rig: Rig, records: list[dict]) -> dict | None:
    recs = [r for r in records if r.get("phase") == "usb"]
    if not recs:
        return None
    results = recs[-1]["results"]
    ref = rig.reference.name
    verdicts = []
    for rx, value in results.items():
        if rx == ref or rig.devices.get(rx) is None:
            continue
        if rig.devices[rx].stack != "radiant":
            continue
        v, detail = verdict_usb(value, results.get(ref))
        verdicts.append({"round": "-", "metric": "usb", "receiver": rx,
                         "verdict": v, "detail": detail})
    return {"metrics": results, "verdicts": verdicts}


def build_summary(rig: Rig, records: list[dict]) -> dict:
    rounds = [summarise_round(rig, rnd, records) for rnd in rig.rounds
              if any(r.get("round") == rnd.name for r in records)]
    usb = summarise_usb(rig, records)
    identity = next((r for r in records if r.get("phase") == "identity"),
                    None)
    verdicts = [v for r in rounds for v in r["verdicts"]]
    if usb:
        verdicts += usb["verdicts"]
    tally: dict[str, int] = {}
    for v in verdicts:
        tally[v["verdict"]] = tally.get(v["verdict"], 0) + 1
    return {"rig": rig.name, "reference": rig.reference.name,
            "generated": _dt.datetime.now(_dt.UTC).isoformat(),
            "identity": identity["results"] if identity else None,
            "rounds": rounds, "usb": usb, "verdicts": verdicts,
            "tally": tally, "rules": VERDICT_RULES,
            "caveats": caveats(rig, records)}


def caveats(rig: Rig, records: list[dict]) -> list[str]:
    out = ["Every figure is for this fixed placement: antenna and position "
           "are part of each result until a swap round separates them."]
    out += rig.caveats
    if any(d.part == "nrf5340net" and d.transmit for d in rig.devices.values()):
        out.append("The nRF5340 network core transmits at 0 dBm at most "
                   "(no positive TXPOWER setting in silicon), up to 8 dB "
                   "below the other parts - its rounds sit lower on the "
                   "link budget.")
    uart = [d.name for d in rig.devices.values()
            if d.receive and not d.is_usb]
    if uart:
        out.append(f"{', '.join(uart)} attach over UART, not USB: excluded "
                   f"from the usb phase, and their receive path includes a "
                   f"UART rather than the USB stack a real dongle has.")
    out.append("RSSI is not comparable across chip families (each reports on "
               "its own calibration); it is shown, never judged.")
    bases = {res.get("counts", {}).get("basis")
             for rec in records if rec.get("phase") in ("loss", "multi")
             for res in rec.get("results", {}).values()}
    if any(b and "wall_clock" in b for b in bases):
        out.append("Some loss figures are wall-clock (received radio messages "
                   "against the window's slot count), not the transmitter's "
                   "event counter: ant_verify withholds 'loss (exact)' once the "
                   "counter stands still even once, and the DK master "
                   "occasionally re-sends its previous buffer when a host "
                   "refill misses a slot. That repeat lands on every receiver "
                   "in the window alike, so the receiver-minus-reference gap "
                   "is unaffected; the absolute figures assume the "
                   "transmitter's crystal is exact.")
    return out


def _fmt(v, spec=".2f", none="-"):
    if v is None:
        return none
    if isinstance(v, float) and math.isinf(v):
        return "miss"
    return format(v, spec)


def render_markdown(rig: Rig, summary: dict, records: list[dict]) -> str:
    lines = [f"# {rig.name}: receivers vs {summary['reference']}", "",
             f"Generated {summary['generated']}. Reference: "
             f"**{summary['reference']}** "
             f"({rig.reference.address}).", ""]
    tally = summary["tally"]
    if tally:
        lines.append("**Verdict tally:** " + ", ".join(
            f"{k} {tally[k]}" for k in (BETTER, EQUAL, WORSE, INCONCLUSIVE)
            if k in tally))
        lines.append("")

    lines += ["## Devices", "", "| device | address | stack | part | version "
              "| channels/networks |", "|---|---|---|---|---|---|"]
    ident = summary.get("identity") or {}
    for d in rig.devices.values():
        i = ident.get(d.name, {})
        lines.append(f"| {d.name} | `{d.address}` | {d.stack} | "
                     f"{d.part or '-'} | {i.get('version') or '-'} | "
                     f"{i.get('channels') or '-'}/{i.get('networks') or '-'} |")
    lines.append("")

    if rig.placement:
        lines += ["## Placement", "", "```toml"]
        lines += _toml_dump({"placement": rig.placement}).rstrip().splitlines()
        lines += ["```", ""]

    lines += ["## Verdicts", "", "| round | metric | receiver | verdict | "
              "detail |", "|---|---|---|---|---|"]
    for v in summary["verdicts"]:
        lines.append(f"| {v['round']} | {v['metric']} | {v['receiver']} | "
                     f"**{v['verdict']}** | {v['detail']} |")
    lines.append("")

    for rnd in summary["rounds"]:
        lines += [f"## Round {rnd['round']} - transmitter(s): "
                  f"{', '.join(rnd['transmitters'])}", ""]
        m = rnd["metrics"]
        for label in ("loss", "loss (multi, pooled)"):
            if label in m:
                reps = max(len(v) for v in m[label].values())
                lines += [f"### {label}, % per window", "",
                          "| receiver | " + " | ".join(
                              f"W{i + 1}" for i in range(reps)) + " |",
                          "|---|" + "---|" * reps]
                for rx, vals in m[label].items():
                    lines.append(f"| {rx} | " + " | ".join(
                        _fmt(v) for v in vals) + " |")
                lines.append("")
        if "multi per profile" in m:
            profs: list[str] = []
            for per in m["multi per profile"].values():
                profs += [p for p in per if p not in profs]
            lines += ["### multi, loss % per profile (pooled over windows; "
                      "`wc` = wall-clock basis, no exact counter)", "",
                      "| receiver | " + " | ".join(profs) + " |",
                      "|---|" + "---|" * len(profs)]
            for rx, per in m["multi per profile"].items():
                cells = []
                for p in profs:
                    e = per.get(p)
                    cells.append("-" if e is None else
                                 _fmt(e["loss_pct"])
                                 + (" wc" if e["basis"] == "wall_clock"
                                    else ""))
                lines.append(f"| {rx} | " + " | ".join(cells) + " |")
            lines.append("")
        if "acquire" in m:
            lines += ["### acquisition, time to first packet (s)", "",
                      "| receiver | median | Q1 | Q3 | acquired | misses |",
                      "|---|---|---|---|---|---|"]
            for rx, s in m["acquire"].items():
                lines.append(f"| {rx} | {_fmt(s['median'])} | "
                             f"{_fmt(s['q1'])} | {_fmt(s['q3'])} | "
                             f"{s['acquired']}/{s['n']} | {s['misses']} |")
            lines.append("")
        if "ladder" in m:
            lines += ["### sensitivity knee (5 % loss)", "",
                      "| receiver | knee TX dBm | knee at RX dBm | dial | "
                      "note |", "|---|---|---|---|---|"]
            for rx, s in m["ladder"].items():
                lines.append(f"| {rx} | {_fmt(s.get('knee_dbm'))} | "
                             f"{_fmt(s.get('knee_rssi_dbm'), '.1f')} | "
                             f"{'ok' if s.get('dial_trustworthy') else 'FAIL'}"
                             f" | {s.get('reason', '')} |")
            lines.append("")
        if "acked" in m:
            lines += ["### acknowledged transfers (receiver as originator)",
                      "", "| receiver | sent | completed | failed | "
                      "peer received | blocks | errors |",
                      "|---|---|---|---|---|---|---|"]
            for rx, t in m["acked"].items():
                lines.append(f"| {rx} | {t['sent']} | {t['completed']} | "
                             f"{t['failed']} | {t['peer_received']} | "
                             f"{t['blocks']} | {t['errors']} |")
            lines.append("")
        if rnd["warnings"]:
            lines += ["### window warnings", ""]
            lines += [f"- {w}" for w in rnd["warnings"]]
            lines.append("")

    if summary.get("usb"):
        lines += ["## USB round trip", "", "| receiver | p50 ms | p90 ms | "
                  "p99 ms | max ms | timeouts | msg/s |",
                  "|---|---|---|---|---|---|---|"]
        for rx, s in summary["usb"]["metrics"].items():
            lines.append(f"| {rx} | {_fmt(s.get('p50'), '.3f')} | "
                         f"{_fmt(s.get('p90'), '.3f')} | "
                         f"{_fmt(s.get('p99'), '.3f')} | "
                         f"{_fmt(s.get('max'), '.3f')} | "
                         f"{s.get('timeouts', '-')} | "
                         f"{_fmt(s.get('msgs_per_s'), '.0f')} |")
        lines.append("")

    lines += ["## Decision rules (pre-registered)", ""]
    lines += [f"- **{k}**: {v}" for k, v in VERDICT_RULES.items()]
    lines += ["", "## Caveats", ""]
    lines += [f"- {c}" for c in summary["caveats"]]
    lines += ["", "## Commands", "", "Every child command is in "
              "`summary.json` under `records[].commands`; to repeat the whole "
              "run:", "", "```",
              f"python tools/ant_compare.py --rig {rig.path or '<rig>'}",
              "```", ""]
    return "\n".join(lines)


def _toml_dump(data: dict, prefix: str = "") -> str:
    """A small TOML writer for the placement block (strings, numbers, bools,
    lists of those, nested tables). Enough to echo what tomllib read."""
    def value(v):
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, (int, float)):
            return repr(v)
        if isinstance(v, list):
            return "[" + ", ".join(value(x) for x in v) + "]"
        return json.dumps(str(v))
    scalars = {k: v for k, v in data.items() if not isinstance(v, dict)}
    tables = {k: v for k, v in data.items() if isinstance(v, dict)}
    out = [f"{k} = {value(v)}" for k, v in scalars.items()]
    for k, v in tables.items():
        name = f"{prefix}.{k}" if prefix else k
        out.append(f"\n[{name}]")
        out.append(_toml_dump(v, name))
    return "\n".join(out)


# ---------------------------------------------------------------------------
# The bench: child processes
# ---------------------------------------------------------------------------

class Bench:
    """Owns the results directory and every child process. One place that
    launches things, so --dry-run can print instead and every command lands
    in the record that used it."""

    def __init__(self, rig: Rig, out: Path, dry_run: bool = False):
        self.rig = rig
        self.out = out
        self.dry_run = dry_run
        self.hk = rig.phase("housekeeping")
        out.mkdir(parents=True, exist_ok=True)

    def py(self, tool: str, *args) -> list[str]:
        # -u: a child writing to a log file is block-buffered otherwise, so
        # ant_sim's "Transmitting for" line only reached the log when it
        # exited and wait_for_text() declared a healthy transmitter dead.
        return [sys.executable, "-u", str(_HERE / tool), *map(str, args)]

    def addr_args(self, dev: Device, serial_flag="--serial",
                  port_flag="--port", baud_flag: str | None = "--baud"):
        if dev.is_usb:
            return [serial_flag, dev.target.value]
        out = [port_flag, dev.target.value]
        if baud_flag:
            out += [baud_flag, str(dev.baud)]
        return out

    def run(self, cmd: list[str], log: Path, timeout: float | None = None
            ) -> int:
        print(f"  $ {' '.join(cmd)}")
        if self.dry_run:
            return 0
        with open(log, "w", encoding="utf-8") as handle:
            try:
                return subprocess.run(cmd, stdout=handle,
                                      stderr=subprocess.STDOUT,
                                      timeout=timeout).returncode
            except subprocess.TimeoutExpired:
                handle.write(f"\n[ant_compare] TIMEOUT after {timeout} s\n")
                return 124

    def spawn(self, cmd: list[str], log: Path):
        print(f"  $ {' '.join(cmd)} &")
        if self.dry_run:
            return None
        handle = open(log, "w", encoding="utf-8")
        proc = subprocess.Popen(cmd, stdout=handle, stderr=subprocess.STDOUT)
        proc._ant_log = handle  # closed in finish()
        return proc

    def finish(self, proc, timeout: float) -> int | None:
        """Wait for a background transmitter to end ON ITS OWN. Killing
        ant_sim leaves its master channels on air (see the
        multichannel-bench-traps note), so this waits; only a hang well past
        its own end is killed, and the caller then resets the board."""
        if proc is None:
            return 0
        try:
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            print("  ! transmitter did not end on its own; killing it - the "
                  "board will be reset before anything else runs")
            proc.kill()
            rc = proc.wait()
            rc = -9 if rc == 0 else rc
        proc._ant_log.close()
        return rc

    def wait_for_text(self, log: Path, text: str, proc, timeout: float
                      ) -> bool:
        if self.dry_run:
            return True
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if text in log.read_text(encoding="utf-8", errors="replace"):
                    return True
            except FileNotFoundError:
                pass
            if proc is not None and proc.poll() is not None:
                return False
            time.sleep(0.25)
        return False

    def ensure_ready(self, dev: Device, tag: str) -> bool:
        """ant_probe until rc 0. The 5340's first probe after a flash, or
        after a killed sim, can fail every step and the next one pass - and a
        silent failure there leaves a master ON AIR."""
        tries = int(self.hk["reset_retries"])
        for attempt in range(1, tries + 1):
            log = self.out / f"{tag}_reset_{_slug(dev.name)}_{attempt}.log"
            rc = self.run(self.py("ant_probe.py", *self.addr_args(dev)), log,
                          timeout=30)
            if rc == 0:
                return True
            time.sleep(float(self.hk["reset_pause_s"]))
        print(f"  ! {dev.name} never answered ant_probe after {tries} tries")
        return False


def _slug(name: str) -> str:
    return "".join(c if c.isalnum() else "_" for c in name)


def _targets(devs: list[Device]) -> list[Target]:
    return [d.target for d in devs]


def phase_identity(bench: Bench) -> dict:
    results = {}
    commands = []
    for dev in bench.rig.devices.values():
        slug = _slug(dev.name)
        cmd = bench.py("ant_probe.py", *bench.addr_args(dev))
        commands.append(cmd)
        log = bench.out / f"identity_probe_{slug}.log"
        rc = bench.run(cmd, log, timeout=60)
        parsed = (parse_probe_log(log.read_text(encoding="utf-8",
                                                errors="replace"))
                  if not bench.dry_run else {})
        fcmd = bench.py("ant_features.py", *bench.addr_args(dev))
        commands.append(fcmd)
        frc = bench.run(fcmd, bench.out / f"identity_features_{slug}.log",
                        timeout=120)
        version = parsed.get("version") or ""
        results[dev.name] = {**parsed, "probe_rc": rc, "features_rc": frc,
                             "stack": dev.stack,
                             "is_radiant": version.startswith("RADIANT")}
    warnings = []
    for name, r in results.items():
        if bench.dry_run:
            break
        if r["stack"] == "radiant" and not r["is_radiant"]:
            warnings.append(f"{name} is declared radiant but reports "
                            f"{r.get('version')!r}")
        if r["probe_rc"] != 0:
            warnings.append(f"{name}: ant_probe failed (rc {r['probe_rc']})")
    return {"phase": "identity", "round": "-", "rep": 0, "results": results,
            "warnings": warnings, "commands": commands}


def _sim_plan(rig: Rig, rnd: Round, phase: str, profiles: list[str]
              ) -> tuple[dict[str, list[str]], list[int]]:
    """Which transmitter runs which profiles, and the receiver's per-channel
    device numbers in profile order. ant_sim numbers a board's profiles base,
    base+1, ... in the order given, so the receiver's list is built the same
    way."""
    per_tx: dict[str, list[str]] = {tx: [] for tx in rnd.transmitters}
    for prof in profiles:
        tx = rnd.multi_split.get(prof, rnd.transmitters[0])
        per_tx[tx].append(prof)
    numbers = []
    for prof in profiles:
        tx = rnd.multi_split.get(prof, rnd.transmitters[0])
        numbers.append(rig.device_number(rnd, tx, phase)
                       + per_tx[tx].index(prof))
    return {tx: p for tx, p in per_tx.items() if p}, numbers


def _start_sims(bench: Bench, rnd: Round, phase: str, tag: str,
                per_tx: dict[str, list[str]], seconds: float):
    sims = []
    for tx, profs in per_tx.items():
        dev = bench.rig.devices[tx]
        cmd = bench.py("ant_sim.py", *bench.addr_args(dev),
                       "--seconds", f"{seconds:.0f}",
                       "--device-number",
                       bench.rig.device_number(rnd, tx, phase))
        for p in profs:
            cmd += ["--profile", p]
        log = bench.out / f"{tag}_sim_{_slug(tx)}.log"
        sims.append((tx, cmd, log, bench.spawn(cmd, log)))
    ok = True
    for tx, _, log, proc in sims:
        if not bench.wait_for_text(log, "Transmitting for", proc,
                                   float(bench.hk["sim_start_timeout_s"])):
            print(f"  ! {tx}: ant_sim never started transmitting - see {log}")
            ok = False
    return sims, ok


def _finish_sims(bench: Bench, sims, seconds: float) -> tuple[dict, list[str]]:
    status, warnings = {}, []
    for tx, _, log, proc in sims:
        rc = bench.finish(proc, timeout=seconds + 120)
        parsed = (parse_sim_log(log.read_text(encoding="utf-8",
                                              errors="replace"))
                  if not bench.dry_run else {"fallbacks": 0})
        status[tx] = {"rc": rc, **parsed}
        if rc != 0:
            warnings.append(f"{tx}: ant_sim rc {rc}")
        if parsed.get("fallbacks"):
            warnings.append(f"{tx}: {parsed['fallbacks']} messages on the "
                            f"wall-clock fallback (pacing guessed)")
        if rc not in (0, 1):
            bench.ensure_ready(bench.rig.devices[tx], f"{log.stem}_recover")
    return status, warnings


def phase_window(bench: Bench, rnd: Round, phase: str, rep: int) -> dict:
    """loss and multi: transmitters up, every receiver for one window."""
    cfg = bench.rig.phase(phase)
    profiles = ([cfg["profile"]] if phase == "loss"
                else list(cfg["profiles"]))
    per_tx, numbers = _sim_plan(bench.rig, rnd, phase, profiles)
    seconds = float(cfg["seconds"])
    tag = f"{rnd.name}_{phase}_{rep}"
    print(f"\n== round {rnd.name}: {phase} window {rep} ({seconds:.0f} s, "
          f"{', '.join(f'{tx}: {p}' for tx, p in per_tx.items())})")
    for tx in rnd.transmitters:
        bench.ensure_ready(bench.rig.devices[tx], tag)

    sims, started = _start_sims(bench, rnd, phase, tag, per_tx,
                                seconds + float(cfg["margin_s"]))
    receivers = bench.rig.receivers_for(rnd)
    extra = []
    for p in profiles:
        extra += ["--profile", p]
    commands = [c for _, c, _, _ in sims]
    results = {}
    warnings = []
    if started and not bench.dry_run:
        rows = run_targets(_targets(receivers), seconds, numbers, extra,
                           bench.out, prefix=f"{tag}_")
        for dev, row in zip(receivers, rows, strict=True):
            data = row.get("data")
            problems = window_sanity(data, numbers, seconds, row["wall_s"])
            if "error" in row:
                problems.insert(0, row["error"])
            results[dev.name] = {
                "json": row["json"], "log": row["log"], "exit": row["exit"],
                "wall_s": row["wall_s"], "counts": verify_counts(data),
                "rssi_dbm_mean": (data or {}).get("derived", {})
                .get("rssi_dbm_mean"),
                "problems": problems}
            warnings += [f"{dev.name}: {p}" for p in problems]
    elif not started:
        warnings.append("a transmitter never started; window not measured")
    elif bench.dry_run:
        for dev in receivers:
            print(f"  (would run ant_verify on {dev.name} with "
                  f"--device-number {numbers})")
    status, sim_warn = _finish_sims(bench, sims,
                                    seconds + float(cfg["margin_s"]))
    warnings += sim_warn
    return {"phase": phase, "round": rnd.name, "rep": rep,
            "seconds": seconds, "profiles": profiles,
            "device_numbers": numbers, "transmitters": status,
            "results": results, "warnings": warnings, "commands": commands}


def phase_acquire(bench: Bench, rnd: Round) -> dict:
    cfg = bench.rig.phase("acquire")
    cycles = int(cfg["cycles"])
    seconds = float(cfg["seconds"])
    profile = bench.rig.phase("loss")["profile"]
    if len(rnd.transmitters) > 1:
        # Every board on air with its multi_split share, so the one channel
        # being acquired is found against cross-board traffic - the
        # condition a real ride starts in - while only it is timed.
        everything = list(bench.rig.phase("multi")["profiles"])
        if profile not in everything:
            everything.insert(0, profile)
        per_tx, all_numbers = _sim_plan(bench.rig, rnd, "acquire", everything)
        numbers = [all_numbers[everything.index(profile)]]
    else:
        per_tx, numbers = _sim_plan(bench.rig, rnd, "acquire", [profile])
    budget = cycles * (seconds + float(cfg["cycle_overhead_s"]))
    tx_seconds = budget + float(cfg["margin_s"])
    tag = f"{rnd.name}_acquire_1"
    print(f"\n== round {rnd.name}: acquire, {cycles} x {seconds:.0f} s")
    for tx in rnd.transmitters:
        bench.ensure_ready(bench.rig.devices[tx], tag)
    sims, started = _start_sims(bench, rnd, "acquire", tag, per_tx,
                                tx_seconds)
    receivers = bench.rig.receivers_for(rnd)
    out_cycles = []
    warnings = []
    end_by = time.monotonic() + tx_seconds - seconds - 5.0
    if started and not bench.dry_run:
        for i in range(cycles):
            if time.monotonic() > end_by:
                warnings.append(f"stopped after {i} cycles: the transmitter's "
                                f"window would end inside the next one "
                                f"(raise cycle_overhead_s)")
                break
            rows = run_targets(_targets(receivers), seconds, numbers,
                               ["--profile", profile], bench.out,
                               prefix=f"{tag}_c{i + 1:02d}_", verbose=False)
            cycle = {}
            for dev, row in zip(receivers, rows, strict=True):
                data = row.get("data") or {}
                ttfp = data.get("derived", {}).get("time_to_first_packet_s")
                heard = (data.get("identities", {}).get("0", {})
                         .get("device_number"))
                if heard is not None and heard != numbers[0]:
                    warnings.append(f"cycle {i + 1} {dev.name}: heard "
                                    f"#{heard}, pinned #{numbers[0]}")
                    ttfp = None
                cycle[dev.name] = ttfp
            out_cycles.append(cycle)
            print(f"  cycle {i + 1}/{cycles}: " + ", ".join(
                f"{k} {_fmt(v)}" for k, v in cycle.items()))
    elif not started:
        warnings.append("the transmitter never started")
    status, sim_warn = _finish_sims(bench, sims, tx_seconds)
    warnings += sim_warn
    return {"phase": "acquire", "round": rnd.name, "rep": 1,
            "seconds": seconds, "device_numbers": numbers,
            "transmitters": status, "cycles": out_cycles,
            "warnings": warnings, "commands": [c for _, c, _, _ in sims]}


def phase_ladder(bench: Bench, rnd: Round) -> dict:
    cfg = bench.rig.phase("ladder")
    tx = bench.rig.devices[rnd.transmitters[0]]
    receivers = bench.rig.receivers_for(rnd)
    tag = f"{rnd.name}_ladder_1"
    json_path = bench.out / f"{tag}.json"
    print(f"\n== round {rnd.name}: ladder ({tx.part}, {cfg['rungs']})")
    bench.ensure_ready(tx, tag)
    cmd = bench.py("ant_sens.py",
                   *bench.addr_args(tx, "--tx-serial", "--tx-port",
                                    "--tx-baud"),
                   "--rungs", cfg["rungs"], "--tx-part", tx.part,
                   "--seconds", cfg["seconds"], "--settle", cfg["settle"],
                   "--device-number",
                   bench.rig.device_number(rnd, tx.name, "ladder"),
                   "--json", json_path, "-q")
    for key, flag in (("top_dbm", "--top-dbm"), ("bottom_dbm",
                                                 "--bottom-dbm")):
        if cfg.get(key) is not None:
            cmd += [flag, cfg[key]]
    bauds = {d.baud for d in receivers if not d.is_usb}
    if len(bauds) > 1:
        raise RigError("ant_sens takes one --rx-baud for every port: "
                       "receiver; the rig's UART receivers disagree")
    if bauds:
        cmd += ["--rx-baud", bauds.pop()]
    for d in receivers:
        cmd += ["--rx", f"{d.name}={d.address}"]
    rungs = None
    try:
        sys.path.insert(0, str(_HERE))
        import ant_sens  # needs pyusb; only on the bench
        top = cfg.get("top_dbm")
        rungs = ant_sens.ladder_dbm(cfg["rungs"], tx.part,
                                    min(top if top is not None else 99,
                                        ant_sens.PART_MAX_DBM[tx.part]),
                                    cfg.get("bottom_dbm"))
    except (SystemExit, ImportError, ValueError, KeyError):
        pass
    est = (len(rungs) if rungs else 25) * (float(cfg["seconds"])
                                           + float(cfg["settle"]))
    rc = bench.run([str(c) for c in cmd], bench.out / f"{tag}.log",
                   timeout=est + 600)
    results, warnings = {}, []
    if not bench.dry_run:
        try:
            data = json.loads(json_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError) as exc:
            data = None
            warnings.append(f"no ladder JSON: {exc}")
        if data:
            for name, block in data.get("receivers", {}).items():
                knee = block.get("knee") or {}
                results[name] = {
                    "knee_dbm": block.get("loss5pct_tx_power_dbm"),
                    "knee_rssi_dbm": block.get("loss5pct_rssi_dbm"),
                    "dial_trustworthy": block.get("dial_trustworthy"),
                    "rssi_slope": block.get("rssi_slope"),
                    "reason": knee.get("reason", ""),
                    "steps": [{"tx_power_dbm": s["tx_power_dbm"],
                               "loss_pct": s["loss_pct"],
                               "loss_basis": s["loss_basis"],
                               "rssi_dbm_mean": s["rssi_dbm_mean"]}
                              for s in block.get("steps", [])]}
            warnings += [f"{c['name']}: {c['detail']}"
                         for c in data.get("checks", []) if not c["pass"]
                         and c["name"].startswith(("EVENT_TX",
                                                   "transmit power"))]
    return {"phase": "ladder", "round": rnd.name, "rep": 1, "rc": rc,
            "tx_part": tx.part, "results": results, "warnings": warnings,
            "commands": [[str(c) for c in cmd]]}


def phase_acked(bench: Bench, rnd: Round) -> dict:
    cfg = bench.rig.phase("acked")
    tx = bench.rig.devices[rnd.transmitters[0]]
    receivers = bench.rig.receivers_for(rnd)
    block_s = float(cfg["block_seconds"])
    sends = max(1, int(block_s / float(cfg["interval"])))
    dn = bench.rig.device_number(rnd, tx.name, "acked")
    print(f"\n== round {rnd.name}: acked, {cfg['passes']} pass(es) of "
          f"{len(receivers)} x {block_s:.0f} s")
    blocks, commands, warnings = [], [], []
    for p in range(int(cfg["passes"])):
        for dev in receivers:
            tag = f"{rnd.name}_acked_p{p + 1}_{_slug(dev.name)}"
            json_path = bench.out / f"{tag}.json"
            json_path.unlink(missing_ok=True)
            bench.ensure_ready(tx, tag)
            cmd = bench.py("ant_ack_pair.py",
                           *bench.addr_args(tx, "--master-serial",
                                            "--master-port", "--master-baud"),
                           *bench.addr_args(dev, "--slave-serial",
                                            "--slave-port", None),
                           "--device", dn, "--sends", sends,
                           "--interval", cfg["interval"],
                           "--seconds", block_s,
                           "--acquire-seconds", cfg["acquire_seconds"],
                           "--tx-power", cfg["tx_power_level"],
                           "--json", json_path)
            cmd = [str(c) for c in cmd]
            commands.append(cmd)
            rc = bench.run(cmd, bench.out / f"{tag}.log",
                           timeout=block_s + float(cfg["acquire_seconds"])
                           + 120)
            block = {"receiver": dev.name, "pass": p + 1, "rc": rc}
            if not bench.dry_run:
                try:
                    data = json.loads(json_path.read_text(encoding="utf-8"))
                except (FileNotFoundError, json.JSONDecodeError) as exc:
                    data = {"errors": [f"no JSON: {exc}"]}
                for k in ("sent", "completed", "failed", "peer_received",
                          "accepted", "seconds"):
                    block[k] = data.get(k)
                block["errors"] = data.get("errors", [])
                if block["errors"]:
                    warnings.append(f"pass {p + 1} {dev.name}: "
                                    f"{'; '.join(block['errors'])}")
            blocks.append(block)
    return {"phase": "acked", "round": rnd.name, "rep": 1,
            "device_number": dn, "blocks": blocks, "warnings": warnings,
            "commands": commands}


def phase_usb(bench: Bench) -> dict:
    cfg = bench.rig.phase("usb")
    results, commands, warnings = {}, [], []
    print("\n== usb round trip")
    for dev in bench.rig.devices.values():
        if not (dev.receive and dev.is_usb):
            continue
        json_path = bench.out / f"usb_{_slug(dev.name)}.json"
        json_path.unlink(missing_ok=True)
        cmd = [str(c) for c in bench.py(
            "ant_bench.py", "--serial", dev.target.value, "--label",
            dev.name, "--count", cfg["count"], "--depth", cfg["depth"],
            "--json", json_path)]
        commands.append(cmd)
        rc = bench.run(cmd, bench.out / f"usb_{_slug(dev.name)}.log",
                       timeout=600)
        if bench.dry_run:
            continue
        try:
            data = json.loads(json_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError) as exc:
            warnings.append(f"{dev.name}: no ant_bench JSON ({exc})")
            continue
        lat = data["latency_ms"]
        results[dev.name] = {"p50": lat["p50"], "p90": lat["p90"],
                             "p99": lat["p99"], "max": lat["max"],
                             "mean": lat["mean"], "timeouts": lat["timeouts"],
                             "msgs_per_s": data["throughput"]["msgs_per_s"],
                             "rc": rc}
    return {"phase": "usb", "round": "-", "rep": 0, "results": results,
            "warnings": warnings, "commands": commands}


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def load_records(out: Path) -> list[dict]:
    path = out / "summary.json"
    if not path.exists():
        return []
    try:
        return json.loads(path.read_text(encoding="utf-8")).get("records", [])
    except json.JSONDecodeError:
        return []


def resanitize(records: list[dict], out: Path) -> int:
    """Re-judge every stored loss/multi window from its own JSON with the
    current window_sanity, so a sanity rule fixed after a run reaches that
    run's report. Only the receiver-problem warnings are replaced; the
    transmitter warnings stay as recorded. Returns how many changed."""
    changed = 0
    for rec in records:
        if rec.get("phase") not in ("loss", "multi"):
            continue
        for name, res in rec.get("results", {}).items():
            path = Path(res.get("json") or "")
            if not path.is_absolute():
                path = out / path
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue    # nothing to re-judge; the recorded error stays
            new = window_sanity(data, rec.get("device_numbers", []),
                                float(rec.get("seconds") or 0),
                                res.get("wall_s"))
            old = res.get("problems", [])
            if new == old:
                continue
            stale = {f"{name}: {p}" for p in old}
            rec["warnings"] = ([w for w in rec.get("warnings", [])
                                if w not in stale]
                               + [f"{name}: {p}" for p in new])
            res["problems"] = new
            changed += 1
    return changed


def save(rig: Rig, out: Path, records: list[dict]) -> dict:
    summary = build_summary(rig, records)
    doc = {**summary, "records": records}
    (out / "summary.json").write_text(json.dumps(doc, indent=2) + "\n",
                                      encoding="utf-8")
    (out / "summary.md").write_text(render_markdown(rig, summary, records),
                                    encoding="utf-8")
    return summary


def upsert(records: list[dict], record: dict) -> list[dict]:
    key = record_key(record)
    return [r for r in records if record_key(r) != key] + [record]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rig", type=Path, required=True)
    parser.add_argument("--phases", default=",".join(PHASES),
                        help=f"comma list from {','.join(PHASES)} "
                             f"(default: all)")
    parser.add_argument("--rounds", default=None,
                        help="comma list of round names (default: all)")
    parser.add_argument("--out", type=Path, default=None,
                        help="results directory (default: the rig's "
                             "meta.results_dir)")
    parser.add_argument("--set", action="append", default=[],
                        metavar="KEY=VALUE",
                        help="override a rig value, e.g. "
                             "--set phases.loss.seconds=30")
    parser.add_argument("--dry-run", action="store_true",
                        help="print every command, run nothing")
    parser.add_argument("--report-only", action="store_true",
                        help="rebuild summary.json/.md from the records "
                             "already in --out")
    parser.add_argument("--no-identity-gate", action="store_true",
                        help="carry on even if a radiant device does not "
                             "report a RADIANT version")
    args = parser.parse_args()

    try:
        rig = load_rig(args.rig, args.set)
    except (RigError, tomllib.TOMLDecodeError, OSError) as exc:
        sys.exit(f"rig: {exc}")

    out = args.out or (Path(rig.results_dir) if rig.results_dir else None)
    if out is None:
        sys.exit("no --out and the rig has no meta.results_dir")
    out = out if out.is_absolute() else (_HERE.parent / out)
    out.mkdir(parents=True, exist_ok=True)

    records = load_records(out)
    if args.report_only:
        changed = resanitize(records, out)
        if changed:
            print(f"re-judged {changed} stored window result(s) with the "
                  f"current sanity rules")
        summary = save(rig, out, records)
        print(f"rebuilt {out / 'summary.md'}: {summary['tally']}")
        return 0

    phases = [p.strip() for p in args.phases.split(",") if p.strip()]
    bad = [p for p in phases if p not in PHASES]
    if bad:
        sys.exit(f"unknown phases {bad}")
    rounds = ([rig.round(n.strip()) for n in args.rounds.split(",")]
              if args.rounds else rig.rounds)

    if not args.dry_run:
        shutil.copyfile(args.rig, out / "rig.toml")
        if args.set:
            (out / "rig_overrides.txt").write_text("\n".join(args.set) + "\n",
                                                   encoding="utf-8")
    bench = Bench(rig, out, args.dry_run)

    if "identity" in phases:
        rec = phase_identity(bench)
        records = upsert(records, rec)
        if not args.dry_run:
            save(rig, out, records)
        if rec["warnings"]:
            print("\nidentity warnings:\n  " + "\n  ".join(rec["warnings"]))
            if not args.no_identity_gate and any(
                    "declared radiant" in w for w in rec["warnings"]):
                print("\nstopping: a device declared radiant is not running "
                      "RadiANT (--no-identity-gate to override)")
                return 1

    for rnd in rounds:
        for phase in phases:
            if phase not in rnd.phases:
                continue
            if phase in ("loss", "multi"):
                reps = rnd.repeats.get(phase,
                                       int(rig.phase(phase)["repeats"]))
                for rep in range(1, reps + 1):
                    rec = phase_window(bench, rnd, phase, rep)
                    records = upsert(records, rec)
                    if not args.dry_run:
                        save(rig, out, records)
            else:
                runner = {"acquire": phase_acquire, "ladder": phase_ladder,
                          "acked": phase_acked}[phase]
                rec = runner(bench, rnd)
                records = upsert(records, rec)
                if not args.dry_run:
                    save(rig, out, records)

    if "usb" in phases:
        records = upsert(records, phase_usb(bench))

    if args.dry_run:
        print("\n(dry run: nothing executed, nothing written)")
        return 0
    summary = save(rig, out, records)
    print(f"\nwrote {out / 'summary.md'}")
    print(f"verdicts: {summary['tally']}")
    for v in summary["verdicts"]:
        print(f"  {v['round']:>2} {v['metric']:<22} {v['receiver']:<16} "
              f"{v['verdict']:<13} {v['detail']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
