#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Apply the Tier 2 A/B gates to two bench sittings and print the table.

Consumes the baseline JSON that a bench sitting produces - one file per backend,
carrying the verbatim output of `tools/ant_verify.py --json` and
`tools/ant_bench.py --json` plus a `derived` block per run - validates each one
against `archive/benchmarks/baseline.schema.json`, applies the thresholds in
`tools/ab_gates.toml`, and prints a comparison in the shape of the USB-stack
table in `docs/backends.md`.

    python tools/ant_ab.py archive/benchmarks/2026-08-15-sdk-ant.json \\
                           archive/benchmarks/2026-08-15-core.json

    # A/B/A, which is the form that can actually be trusted:
    python tools/ant_ab.py a1.json b.json a2.json

Exit status is 0 only if every enabled gate passed.

What this tool refuses to do
----------------------------

**It will not compare two runs that are not from one sitting.** The run-to-run
spread on this bench is comparable to the effect being measured - four identical
300 s runs landed between 0.26 % and 0.60 % - so a number recorded today against
a number recorded last week measures the room, the neighbours' Wi-Fi and the
position of the boards on the desk. If `meta.recorded` or `rig` differ, this
refuses. If they match but only two files were given, it prints the A/B/A rule
in full, because two files cannot show whether the two A runs would have agreed.

**It will not read a number it was not asked to read.** The loss gate reads
`derived.loss_exact_pct` and there is no code path here that reads `loss_pct`.
The timing gate reads `derived.timing_offgrid_host_ms` and there is no code path
that reads a jitter figure. Both of those were mistakes waiting to be made:
`loss_pct` divides by elapsed time over the nominal period and assumes the
transmitter's crystal is exact, and `jitter` is a standard deviation over raw
gaps, so one lost packet turns a 250 ms gap into 500 ms and six losses in 1,200
packets produce its entire figure. Gating on jitter gates on loss twice under a
different name.

**It will not treat a missing measurement as a pass.** A gate marked
`required` in `ab_gates.toml` whose field is absent FAILS, and the failure names
the field. A gate not marked required reports SKIP in its own column and is
counted separately in the summary; it never silently disappears. The two gates
that are legitimately optional - the 32-channel scale run and acknowledged-data
success - are optional because `libant.a` cannot reach 32 channels and there is
no baseline to invent, not because they are less important.

The schema validator here is a small one written into this file rather than
`jsonschema` from PyPI. The CI `host-tests` job installs `pyusb` and nothing
else, and that job is the only one that runs on a fork; a validator that worked
on the development machine and skipped in CI would be worse than none.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from dataclasses import dataclass, field
from typing import Any

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

_HERE = __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0]
DEFAULT_GATES = _HERE + "/ab_gates.toml"
DEFAULT_SCHEMA = _HERE + "/../archive/benchmarks/baseline.schema.json"


# ---------------------------------------------------------------------------
# A very small JSON Schema subset
#
# Covers exactly what `baseline.schema.json` uses: $ref into $defs, type
# (including union types), required, properties, additionalProperties, enum,
# const, minimum, maximum, minItems, items and pattern. Anything the schema
# grows that is not on that list raises rather than being ignored - a validator
# that quietly skips a keyword reports "valid" for a document it never checked.
# ---------------------------------------------------------------------------

_KNOWN_KEYWORDS = {
    "$schema", "$id", "$defs", "$ref", "title", "description", "type",
    "properties", "required", "additionalProperties", "items", "enum", "const",
    "minimum", "maximum", "minItems", "pattern", "propertyNames",
}

_TYPE_CHECKS = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    # bool is a subclass of int in Python and is not a number in JSON Schema.
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: (isinstance(v, (int, float))
                         and not isinstance(v, bool)),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}


class SchemaError(Exception):
    """The schema itself uses something this validator does not implement."""


def validate(instance: Any, schema: dict, *, root: dict | None = None,
             path: str = "$") -> list[str]:
    """Return a list of human-readable errors. Empty means valid."""
    root = root if root is not None else schema
    errors: list[str] = []

    unknown = set(schema) - _KNOWN_KEYWORDS
    if unknown:
        raise SchemaError(
            f"{path}: schema uses keyword(s) {sorted(unknown)} that "
            f"tools/ant_ab.py's validator does not implement. Implement them "
            f"rather than letting a document through unchecked.")

    if "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/$defs/"):
            raise SchemaError(f"{path}: only #/$defs/ refs are supported, "
                              f"not {ref!r}")
        target = root.get("$defs", {}).get(ref[len("#/$defs/"):])
        if target is None:
            raise SchemaError(f"{path}: unresolved $ref {ref!r}")
        return validate(instance, target, root=root, path=path)

    types = schema.get("type")
    if types is not None:
        wanted = [types] if isinstance(types, str) else list(types)
        for name in wanted:
            if name not in _TYPE_CHECKS:
                raise SchemaError(f"{path}: unknown type {name!r}")
        if not any(_TYPE_CHECKS[name](instance) for name in wanted):
            return [f"{path}: expected {'/'.join(wanted)}, got "
                    f"{type(instance).__name__}"]

    if "const" in schema and instance != schema["const"]:
        errors.append(f"{path}: must be {schema['const']!r}, got "
                      f"{instance!r}")
    if "enum" in schema and instance not in schema["enum"]:
        errors.append(f"{path}: must be one of {schema['enum']}, got "
                      f"{instance!r}")

    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            errors.append(f"{path}: {instance} is below the minimum "
                          f"{schema['minimum']}")
        if "maximum" in schema and instance > schema["maximum"]:
            errors.append(f"{path}: {instance} is above the maximum "
                          f"{schema['maximum']}")

    if isinstance(instance, str) and "pattern" in schema:
        if re.search(schema["pattern"], instance) is None:
            errors.append(f"{path}: {instance!r} does not match "
                          f"{schema['pattern']!r}")

    if isinstance(instance, dict):
        properties = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in instance:
                errors.append(f"{path}: missing required property {name!r}")
        if schema.get("additionalProperties") is False:
            for name in instance:
                if name not in properties:
                    errors.append(f"{path}: unexpected property {name!r}")
        for name, value in instance.items():
            if name in properties:
                errors.extend(validate(value, properties[name], root=root,
                                       path=f"{path}.{name}"))

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            errors.append(f"{path}: needs at least {schema['minItems']} "
                          f"item(s), has {len(instance)}")
        if "items" in schema:
            for index, value in enumerate(instance):
                errors.extend(validate(value, schema["items"], root=root,
                                       path=f"{path}[{index}]"))

    return errors


# ---------------------------------------------------------------------------
# Reading a baseline
# ---------------------------------------------------------------------------

class MissingField(Exception):
    """A gate asked for a number the baseline does not carry.

    Carries the field's path in the words `archive/benchmarks/README.md` uses,
    so the message says which slot of which run a bench sitting failed to fill.
    """


@dataclass
class Baseline:
    path: str
    data: dict

    @property
    def label(self) -> str:
        """What to call this column. The backend, which is what differs."""
        return self.data["meta"]["backend"].replace("_", "-")

    @property
    def recorded_date(self) -> str:
        # Date-time to date: one sitting can straddle an hour, not a night.
        return str(self.data["meta"]["recorded"])[:10]

    def radio_runs(self, *, min_seconds: float = 0.0,
                   single_channel: bool = True) -> list[dict]:
        runs = []
        for run in self.data.get("radio_runs", []):
            if run.get("seconds", 0) < min_seconds:
                continue
            if single_channel and run.get("channels", 1) != 1:
                continue
            runs.append(run)
        return runs

    def worst(self, field_name: str, *, min_seconds: float = 0.0,
              single_channel: bool = True) -> tuple[float, dict]:
        """The worst (largest) value of a derived field, and the run it came from.

        Worst rather than mean: a sitting that produced one bad 300 s run
        produced one bad 300 s run, and averaging it away is how a regression
        gets shipped. The run is returned too so the report can name it.
        """
        runs = self.radio_runs(min_seconds=min_seconds,
                               single_channel=single_channel)
        if not runs:
            raise MissingField(
                f"{self.path}: no radio_runs entry of at least {min_seconds} s"
                + (" on a single channel" if single_channel else ""))
        values = []
        for run in runs:
            value = run.get("derived", {}).get(field_name)
            if value is None:
                raise MissingField(
                    f"{self.path}: radio_runs[profile={run.get('profile')!r}]"
                    f".derived.{field_name} is missing")
            values.append((float(value), run))
        return max(values, key=lambda pair: pair[0])

    def usb_worst(self, field_name: str) -> float:
        runs = self.data.get("usb_runs", [])
        if not runs:
            raise MissingField(f"{self.path}: no usb_runs")
        values = []
        for run in runs:
            value = run.get("derived", {}).get(field_name)
            if value is None:
                raise MissingField(
                    f"{self.path}: usb_runs[label={run.get('label')!r}]"
                    f".derived.{field_name} is missing")
            values.append(float(value))
        return max(values)

    def block(self, name: str) -> dict:
        value = self.data.get(name)
        if value is None:
            raise MissingField(f"{self.path}: no {name!r} block")
        return value


def load_baseline(path: str, schema: dict) -> Baseline:
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    errors = validate(data, schema, path=path)
    if errors:
        raise SystemExit(
            f"{path} does not validate against the baseline schema:\n  "
            + "\n  ".join(errors)
            + "\n\nA baseline that does not validate cannot be gated. Fix the "
              "file, or the schema if the schema is what is wrong.")
    return Baseline(path, data)


# ---------------------------------------------------------------------------
# Gates
# ---------------------------------------------------------------------------

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


@dataclass
class GateResult:
    name: str
    a: str
    b: str
    threshold: str
    verdict: str
    detail: str = ""
    # Which column to embolden in the table, the way docs/backends.md does.
    better: str | None = None
    warnings: list[str] = field(default_factory=list)


def _cmp_better(a: float, b: float, lower_is_better: bool = True) -> str | None:
    if a == b:
        return None
    if (b < a) == lower_is_better:
        return "b"
    return "a"


def _ratio_pass(a: float, b: float, max_ratio: float, floor: float) -> bool:
    """b within a x max_ratio, with an absolute floor underwriting the ratio.

    A ratio against a number that is already tiny goes unstable: 25 % of 0.009 ms
    is 2 microseconds, which is nothing but noise. The floor says "anything this
    small passes regardless", and is stated per gate in ab_gates.toml.
    """
    return b <= max(a * max_ratio, a + floor)


def gate_conformance(cfg: dict, a: Baseline, b: Baseline,
                     override: dict | None) -> GateResult:
    """Tier 1. Byte-identical is the pass.

    Takes its answer from `tools/ant_conformance.py --compare` when the two
    transcripts were handed over, and otherwise from the `conformance.sha256`
    each baseline recorded. Comparing the two hashes is the same test - a hash
    of a file that is byte-identical is byte-identical - and it means a
    committed baseline can be gated long after the transcripts moved.
    """
    allowed = set(cfg.get("allowed_differing_cases", []))
    threshold = "byte-identical" + (f" except {len(allowed)} allowed case(s)"
                                    if allowed else "")

    if override is not None:
        identical = override["byte_identical"]
        unexpected = [c for c in override.get("differing_cases", [])
                      if c not in allowed]
        verdict = PASS if (identical or not unexpected) else FAIL
        detail = ("byte-identical" if identical
                  else f"{override['differing_records']} record(s) differ in "
                       f"{len(override['differing_cases'])} case(s); "
                       f"{len(unexpected)} not allowed")
        return GateResult("Tier-1 conformance diff",
                          override["a"]["sha256"][:12],
                          override["b"]["sha256"][:12],
                          threshold, verdict, detail)

    try:
        sha_a = a.block("conformance")["sha256"]
        sha_b = b.block("conformance")["sha256"]
    except MissingField as exc:
        return GateResult("Tier-1 conformance diff", "-", "-", threshold,
                          FAIL if cfg.get("required", True) else SKIP,
                          str(exc))

    identical = sha_a == sha_b
    detail = ("transcripts hash the same"
              if identical else
              "transcripts differ; run tools/ant_conformance.py --compare to "
              "see which cases")
    if not identical and allowed:
        detail += " (allowances cannot be checked from hashes alone - pass "
        detail += "--conformance-a/--conformance-b)"
    return GateResult("Tier-1 conformance diff", sha_a[:12], sha_b[:12],
                      threshold, PASS if identical else FAIL, detail)


def gate_loss(cfg: dict, a: Baseline, b: Baseline) -> GateResult:
    seconds = cfg.get("min_seconds", 300)
    threshold = f"<= A + {cfg['max_delta_pp']} pp"
    try:
        value_a, run_a = a.worst("loss_exact_pct", min_seconds=seconds)
        value_b, run_b = b.worst("loss_exact_pct", min_seconds=seconds)
    except MissingField as exc:
        return GateResult("loss (exact), %ds runs" % seconds, "-", "-",
                          threshold, FAIL, str(exc))

    ok = value_b <= value_a + cfg["max_delta_pp"]
    detail = (f"worst {seconds:.0f} s single-channel run: "
              f"{run_a.get('profile')} vs {run_b.get('profile')}")
    ceiling = cfg.get("absolute_ceiling_pct")
    if ceiling is not None and value_b > ceiling:
        ok = False
        detail += f"; candidate is over the {ceiling} % acceptance ceiling"

    warnings = []
    floor = cfg.get("suspicious_below_pct")
    if floor is not None:
        for label, value in (("A", value_a), ("B", value_b)):
            if value < floor:
                warnings.append(
                    f"{label} loss {value:.3f} % is below the characterised "
                    f"bench floor of {floor} % - that is suspicious, not "
                    f"excellent. Check the measurement before believing it")

    return GateResult(f"loss (exact), {seconds:.0f} s runs",
                      f"{value_a:.3f} %", f"{value_b:.3f} %", threshold,
                      PASS if ok else FAIL, detail,
                      _cmp_better(value_a, value_b), warnings)


def gate_count(cfg: dict, a: Baseline, b: Baseline, field_name: str,
               label: str) -> GateResult:
    threshold = f"<= {cfg['max']}"
    try:
        value_a, _ = a.worst(field_name, single_channel=False)
        value_b, _ = b.worst(field_name, single_channel=False)
    except MissingField as exc:
        return GateResult(label, "-", "-", threshold, FAIL, str(exc))

    ok = value_b <= cfg["max"]
    detail = ""
    if value_a > cfg["max"]:
        # The reference having the same problem does not excuse it: the whole
        # sitting is suspect, per archive/benchmarks/README.md.
        detail = ("the sdk-ant reference is also nonzero - this invalidates "
                  "the sitting rather than failing one gate")
        ok = False
    return GateResult(label, f"{value_a:.0f}", f"{value_b:.0f}", threshold,
                      PASS if ok else FAIL, detail,
                      _cmp_better(value_a, value_b))


def gate_timing(cfg: dict, a: Baseline, b: Baseline) -> GateResult:
    threshold = f"<= A x {cfg['max_ratio']}"
    try:
        value_a, _ = a.worst("timing_offgrid_host_ms")
        value_b, _ = b.worst("timing_offgrid_host_ms")
    except MissingField as exc:
        return GateResult("timing (off-grid, host clock)", "-", "-", threshold,
                          FAIL, str(exc))

    ok = _ratio_pass(value_a, value_b, cfg["max_ratio"], cfg.get("floor_ms", 0))
    detail = "intervals minus whole periods - not the jitter line"
    warnings = []
    # The radio-clock figure is not gated, but its absence means lib config
    # 0xE0 was not on, and that is worth saying out loud because the same
    # setting is what the sub-millisecond fusion capability rests on.
    for base in (a, b):
        try:
            base.worst("timing_offgrid_radio_ms")
        except MissingField:
            warnings.append(
                f"{base.path}: no timing_offgrid_radio_ms - the run did not "
                f"have lib config 0xE0 (channel id + RSSI + RX timestamp) on, "
                f"so the host clock is the only instrument in this sitting")
    return GateResult("timing (off-grid, host clock)", f"{value_a:.3f} ms",
                      f"{value_b:.3f} ms", threshold, PASS if ok else FAIL,
                      detail, _cmp_better(value_a, value_b), warnings)


def gate_acquisition(cfg: dict, a: Baseline, b: Baseline) -> list[GateResult]:
    results = []
    threshold = (f"<= A x {cfg['max_ratio']} and <= "
                 f"{cfg['max_absolute_s']} s")
    for field_name, label in (("time_to_first_packet_s", "time to first packet"),
                              ("reacquire_s", "re-acquisition")):
        try:
            value_a, _ = a.worst(field_name)
            value_b, _ = b.worst(field_name)
        except MissingField as exc:
            results.append(GateResult(label, "-", "-", threshold,
                                      FAIL if cfg.get("required", True)
                                      else SKIP, str(exc)))
            continue
        ok = (value_b <= value_a * cfg["max_ratio"]
              and value_b <= cfg["max_absolute_s"])
        results.append(GateResult(label, f"{value_a:.2f} s",
                                  f"{value_b:.2f} s", threshold,
                                  PASS if ok else FAIL, "",
                                  _cmp_better(value_a, value_b)))
    return results


def gate_sensitivity(cfg: dict, a: Baseline, b: Baseline) -> GateResult:
    threshold = f"within {cfg['max_delta_db']} dB-equivalent"
    try:
        block_a = a.block("sensitivity")
        block_b = b.block("sensitivity")
    except MissingField as exc:
        return GateResult("sensitivity (5 % loss point)", "-", "-", threshold,
                          FAIL if cfg.get("required", True) else SKIP,
                          str(exc))

    key = "loss5pct_attenuation_db"
    value_a, value_b = block_a.get(key), block_b.get(key)
    if value_a is None or value_b is None:
        # Distance is the documented fallback when there is no inline
        # attenuator, and it is only comparable within one sitting - which the
        # sitting checks have already enforced by this point.
        key = "loss5pct_distance_m"
        value_a, value_b = block_a.get(key), block_b.get(key)
        if value_a is None or value_b is None:
            return GateResult(
                "sensitivity (5 % loss point)", "-", "-", threshold, FAIL,
                "neither sensitivity.loss5pct_attenuation_db nor "
                "loss5pct_distance_m is present in both baselines")
    if block_a.get("method") != block_b.get("method"):
        return GateResult("sensitivity (5 % loss point)", str(value_a),
                          str(value_b), threshold, FAIL,
                          f"different methods ({block_a.get('method')} vs "
                          f"{block_b.get('method')}) are not comparable")

    unit = "dB" if key.endswith("_db") else "m"
    # More attenuation (or more distance) at the 5 % point is better hearing.
    ok = value_a - value_b <= cfg["max_delta_db"]
    return GateResult("sensitivity (5 % loss point)", f"{value_a:.1f} {unit}",
                      f"{value_b:.1f} {unit}", threshold,
                      PASS if ok else FAIL, f"method {block_a.get('method')}",
                      _cmp_better(value_a, value_b, lower_is_better=False))


def gate_scale(cfg: dict, a: Baseline, b: Baseline) -> GateResult:
    threshold = f"<= single-channel + {cfg['max_delta_pp']} pp (absolute)"
    try:
        block = b.block("scale")
    except MissingField as exc:
        return GateResult("32-channel per-channel loss", "n/a", "-", threshold,
                          FAIL if cfg.get("required", False) else SKIP,
                          f"{exc}. libant.a allocates 8 channels, so this gate "
                          f"is absolute and has no sdk-ant reference by "
                          f"construction")

    per_channel = block["per_channel_loss_pct"]
    single = block["single_channel_loss_pct"]
    ok = per_channel <= single + cfg["max_delta_pp"]
    channels = block["channels_tracked"]
    detail = f"{channels} channels tracked vs {single:.3f} % on one"
    if channels < cfg.get("expect_channels", 32):
        ok = False
        detail += (f"; the gate is about {cfg.get('expect_channels', 32)} "
                   f"channels")
    return GateResult("32-channel per-channel loss", "n/a (libant.a: 8)",
                      f"{per_channel:.3f} %", threshold,
                      PASS if ok else FAIL, detail)


def gate_ack(cfg: dict, a: Baseline, b: Baseline) -> GateResult:
    threshold = (f">= {cfg['min_success_pct']} % and >= A - "
                 f"{cfg['max_delta_pp']} pp")
    try:
        block_a, block_b = a.block("ack_data"), b.block("ack_data")
    except MissingField as exc:
        return GateResult("ack-data success (ERG)", "-", "-", threshold,
                          FAIL if cfg.get("required", False) else SKIP,
                          str(exc))

    def percent(block: dict, source: Baseline) -> float:
        value = block.get("success_pct")
        if value is not None:
            return float(value)
        # Derivable, and deriving it beats failing a real measurement over a
        # field the operator did not have to fill in.
        if not block.get("attempts"):
            raise MissingField(f"{source.path}: ack_data has no attempts")
        return 100.0 * block["successes"] / block["attempts"]

    try:
        value_a, value_b = percent(block_a, a), percent(block_b, b)
    except MissingField as exc:
        return GateResult("ack-data success (ERG)", "-", "-", threshold, FAIL,
                          str(exc))

    ok = (value_b >= cfg["min_success_pct"]
          and value_b >= value_a - cfg["max_delta_pp"])
    return GateResult("ack-data success (ERG)", f"{value_a:.2f} %",
                      f"{value_b:.2f} %", threshold, PASS if ok else FAIL, "",
                      _cmp_better(value_a, value_b, lower_is_better=False))


def gate_usb(cfg: dict, a: Baseline, b: Baseline) -> GateResult:
    threshold = f"<= A x {cfg['max_ratio']}"
    try:
        value_a = a.usb_worst("latency_p50_ms")
        value_b = b.usb_worst("latency_p50_ms")
    except MissingField as exc:
        return GateResult("USB round-trip latency (p50)", "-", "-", threshold,
                          FAIL if cfg.get("required", True) else SKIP,
                          str(exc))
    ok = _ratio_pass(value_a, value_b, cfg["max_ratio"], cfg.get("floor_ms", 0))
    return GateResult("USB round-trip latency (p50)", f"{value_a:.3f} ms",
                      f"{value_b:.3f} ms", threshold, PASS if ok else FAIL,
                      "the radio is not on this path, so expect equality",
                      _cmp_better(value_a, value_b))


def evaluate(gates: dict, a: Baseline, b: Baseline,
             conformance: dict | None = None) -> list[GateResult]:
    """Run every enabled gate, in the order docs/testing.md lists them."""
    results: list[GateResult] = []

    def cfg(name: str) -> dict | None:
        block = gates.get("gates", {}).get(name)
        if block is None or not block.get("enabled", True):
            return None
        return block

    block = cfg("conformance")
    if block:
        results.append(gate_conformance(block, a, b, conformance))
    block = cfg("loss_exact")
    if block:
        results.append(gate_loss(block, a, b))
    block = cfg("unexplained_loss")
    if block:
        results.append(gate_count(block, a, b, "unexplained_loss",
                                  "unexplained loss (no RX_FAIL)"))
    block = cfg("accumulator_violations")
    if block:
        results.append(gate_count(block, a, b, "accumulator_violations",
                                  "accumulator continuity violations"))
    block = cfg("timing")
    if block:
        results.append(gate_timing(block, a, b))
    block = cfg("acquisition")
    if block:
        results.extend(gate_acquisition(block, a, b))
    block = cfg("sensitivity")
    if block:
        results.append(gate_sensitivity(block, a, b))
    block = cfg("scale")
    if block:
        results.append(gate_scale(block, a, b))
    block = cfg("ack_data")
    if block:
        results.append(gate_ack(block, a, b))
    block = cfg("usb_latency")
    if block:
        results.append(gate_usb(block, a, b))

    return results


# ---------------------------------------------------------------------------
# The sitting checks
# ---------------------------------------------------------------------------

AB_A_RULE = """\
A/B/A, in one sitting, or the numbers do not mean what they look like.

Board A is the transmitter and is always sdk-ant (sim/ on the nRF54L15 DK).
Board B is the receiver and alternates backends. Measure sdk-ant, then the
other backend, then sdk-ant again, without moving anything - then pass all
three files here, oldest first.

Only two baselines were given, so the repeated A run does not exist and
nothing here can tell you whether the difference you are about to read was
the backend or the afternoon. The run-to-run spread on this bench is
comparable to the effect being measured: four identical 300 s runs landed
between 0.26 % and 0.60 %."""


def check_sitting(cfg: dict, baselines: list[Baseline]) -> list[str]:
    """Refusals. A non-empty list means the comparison must not be made."""
    problems = []
    first = baselines[0]

    if cfg.get("require_same_recorded_date", True):
        dates = {base.recorded_date for base in baselines}
        if len(dates) > 1:
            problems.append(
                "these baselines are from different sittings ("
                + ", ".join(f"{base.path} on {base.recorded_date}"
                            for base in baselines)
                + "). A/B/A runs in one sitting; comparing across sessions "
                  "measures the room, not the backend.")

    if cfg.get("require_same_rig", True):
        for base in baselines[1:]:
            if base.data["rig"] != first.data["rig"]:
                problems.append(
                    f"{base.path} and {first.path} describe different rigs. "
                    f"Two results are only comparable if their rigs are.")

    return problems


def check_repeat_a(cfg: dict, first: Baseline, last: Baseline) -> str | None:
    """Did the two sdk-ant runs agree? If not, the sitting is void."""
    if first.data["meta"]["backend"] != last.data["meta"]["backend"]:
        return (f"A/B/A needs the same backend first and last, but "
                f"{first.path} is {first.label} and {last.path} is "
                f"{last.label}")
    try:
        value_first, _ = first.worst("loss_exact_pct")
        value_last, _ = last.worst("loss_exact_pct")
    except MissingField as exc:
        return f"cannot check the repeated A run: {exc}"

    delta = abs(value_first - value_last)
    limit = cfg.get("repeat_a_max_delta_pp", 0.35)
    if delta > limit:
        return (f"the two {first.label} runs disagree by {delta:.3f} pp "
                f"({value_first:.3f} % then {value_last:.3f} %), more than the "
                f"{limit} pp this bench's own spread allows. The sitting is "
                f"void and the candidate's number means nothing.")
    return None


# ---------------------------------------------------------------------------
# The table
# ---------------------------------------------------------------------------


def render(results: list[GateResult], label_a: str, label_b: str) -> str:
    """A markdown table in the shape of the USB-stack comparison in backends.md.

    Same columns, same bolding of whichever side is better, so a result can be
    pasted straight into `docs/backends.md` next to the table it is modelled on.
    """
    # The first header cell is blank, exactly as in docs/backends.md, so the
    # metric column reads as a row label rather than as a heading. The two
    # extra columns are what makes it a gate table rather than a comparison.
    rows = [("", label_a, label_b, "Threshold", "Verdict")]
    for result in results:
        a_text, b_text = result.a, result.b
        if result.better == "a":
            a_text = f"**{a_text}**"
        elif result.better == "b":
            b_text = f"**{b_text}**"
        rows.append((result.name, a_text, b_text, result.threshold,
                     result.verdict))

    widths = [max(len(row[i]) for row in rows) for i in range(5)]
    lines = []
    for index, row in enumerate(rows):
        lines.append("| " + " | ".join(cell.ljust(widths[i])
                                       for i, cell in enumerate(row)) + " |")
        if index == 0:
            lines.append("|" + "|".join("-" * (width + 2) for width in widths)
                         + "|")
    return "\n".join(lines)


def report(results: list[GateResult], label_a: str, label_b: str) -> bool:
    print()
    print(render(results, label_a, label_b))

    details = [r for r in results if r.detail]
    if details:
        print()
        for result in details:
            print(f"  {result.name}: {result.detail}")

    warnings = [(r.name, w) for r in results for w in r.warnings]
    if warnings:
        print()
        for name, text in warnings:
            print(f"  ! {name}: {text}")

    failed = [r for r in results if r.verdict == FAIL]
    skipped = [r for r in results if r.verdict == SKIP]

    print()
    if skipped:
        # Loud, and in its own sentence. A gate that could not be evaluated is
        # not a gate that passed, and the difference has to survive a glance.
        print(f"{len(skipped)} gate(s) COULD NOT BE EVALUATED and were not "
              f"counted either way:")
        for result in skipped:
            print(f"  SKIP {result.name}: {result.detail}")
    if failed:
        print(f"FAILED: {len(failed)} of {len(results)} gate(s) - "
              + ", ".join(r.name for r in failed))
        return False
    print(f"PASS: {len(results) - len(skipped)} gate(s), 0 failed")
    return True


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("baselines", nargs="+", metavar="BASELINE.json",
                        help="two baselines (A then B), or three for A/B/A "
                             "with the repeated reference run last")
    parser.add_argument("--gates", default=DEFAULT_GATES,
                        help=f"thresholds (default: {DEFAULT_GATES})")
    parser.add_argument("--schema", default=DEFAULT_SCHEMA,
                        help="baseline JSON schema (default: "
                             "archive/benchmarks/baseline.schema.json)")
    parser.add_argument("--conformance-a", metavar="FILE",
                        help="the Tier 1 .antser transcript for A. With its "
                             "pair, the conformance gate diffs the transcripts "
                             "case by case instead of comparing two hashes")
    parser.add_argument("--conformance-b", metavar="FILE")
    parser.add_argument("--json", metavar="FILE", nargs="?", const="-",
                        help="write the results as JSON (default: stdout)")
    args = parser.parse_args()

    if not 2 <= len(args.baselines) <= 3:
        sys.exit("give two baselines (A then B) or three (A, B, A again)")

    with open(args.gates, "rb") as handle:
        gates = tomllib.load(handle)
    with open(args.schema, "r", encoding="utf-8") as handle:
        schema = json.load(handle)

    baselines = [load_baseline(path, schema) for path in args.baselines]
    sitting = gates.get("sitting", {})

    problems = check_sitting(sitting, baselines)
    if problems:
        print("REFUSING to compare these baselines:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 2

    reference, candidate = baselines[0], baselines[1]
    void = None
    if len(baselines) == 3:
        void = check_repeat_a(sitting, baselines[0], baselines[2])
        if void:
            print(f"SITTING VOID: {void}", file=sys.stderr)
            return 2
        print(f"A/B/A: the two {reference.label} runs agree, so the "
              f"{candidate.label} number is readable.")
    else:
        print(AB_A_RULE)

    conformance = None
    if args.conformance_a and args.conformance_b:
        import ant_conformance

        allowed = set(gates.get("gates", {}).get("conformance", {})
                      .get("allowed_differing_cases", []))
        conformance = ant_conformance.compare(args.conformance_a,
                                              args.conformance_b, allowed)

    results = evaluate(gates, reference, candidate, conformance)
    ok = report(results, reference.label, candidate.label)

    if args.json:
        payload = {
            "tool": "ant_ab.py",
            "reference": reference.path,
            "candidate": candidate.path,
            "aba": len(baselines) == 3,
            "pass": ok,
            "gates": [
                {"name": r.name, "a": r.a, "b": r.b,
                 "threshold": r.threshold, "verdict": r.verdict,
                 "detail": r.detail, "warnings": r.warnings}
                for r in results
            ],
        }
        if args.json == "-":
            json.dump(payload, sys.stdout, indent=2)
            sys.stdout.write("\n")
        else:
            with open(args.json, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, indent=2)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
