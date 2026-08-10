#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Measure a receiver's sensitivity by walking the *transmitter* down a power ladder.

READ THIS BEFORE STOPPING THE TOOL BECAUSE IT LOOKS LIKE A SWEEP THAT WAS BANNED.

There are two power sweeps in this project's history and they are not the same
experiment:

  * The one that is closed swept the **dongle's own transmit power** against
    packet loss at high SNR, looking for a link that was never marginal. It
    found nothing, because at 60 dB of margin nothing is there to find. Do not
    revive it.
  * This one steps the **master's** transmit power down until the *receiver*
    starts missing packets, and reports the power at which it misses 5 %. The
    quantity being measured is the receiver's knee - its sensitivity - and the
    transmitter is only the instrument's dial.

`[gates.sensitivity]` in tools/ab_gates.toml requires two backends' 5 %-loss
points to agree within 1 dB, and the methods it originally named - an inline
attenuator, or a repeatable distance - are both hardware this bench does not
have. Transmit power is the software substitute: it is the same attenuation,
applied at the other end of the link, and it is already plumbed end to end.

    # coarse ladder, six ANT power levels, no firmware change needed anywhere
    python tools/ant_sens.py --tx-serial 1234 --rx-serial 5678 --seconds 60

    # the Phase 0 acceptance run: two ladders back to back, must agree to 1 dB
    python tools/ant_sens.py --tx-serial 1234 --rx-serial 5678 --repeat 2 \
        --json sens.json

**Two boards are required, and this script drives both of them.** One is opened
as an ANT+ master and paced from EVENT_TX exactly as tools/ant_sim.py does it
(on a thread, because the receive loop is the other half of the measurement);
the other is opened as a slave and its stream is handed to
tools/ant_verify.py's analyser unchanged. Nothing about loss is recounted here -
`loss (exact)`, counted from the transmitter's own event counter, is read out of
that analyser's summary.

What the ladder can and cannot reach
------------------------------------

The six ANT power levels span -20 to +8 dBm: 28 dB, in steps of 4 and 8. With
`--rungs fine` and a transmitter whose firmware carries the custom byte of
MESG_CHANNEL_RADIO_TX_POWER (0x60), the nRF52840 register table reaches -40 dBm
and the span becomes 48 dB in mostly 1-4 dB steps.

Either way the span is finite, and a desk pair at +8 dBm sits around -30 dBm at
the receiver while the knee is near -93. **If the bottom rung still shows no
loss, the knee was not reached** and this reports that rather than
extrapolating: move the boards apart, put something absorbing between them, and
run it again. A number extrapolated past the end of the ladder would be a
fabricated sensitivity figure, which is worse than no figure at all.

The RSSI slope check is not decoration
--------------------------------------

Every rung records the mean RSSI the receiver reported. Across the ladder that
must fall roughly 1 dB per commanded dB. It does not if:

  * `CONFIG_ANT_DONGLE_TX_POWER_BOOST` is set on the transmitter, which folds
    levels 3 and 4 onto the same +8 dBm and flattens two rungs into one;
  * `--rungs fine` is used against a transmitter whose part is not the one
    `--tx-part` names, in which case the raw register values mean something
    else entirely and the dial is lying;
  * the transmitter's firmware predates the custom byte and silently transmits
    at 0 dBm for every fine rung.

All three produce a plausible-looking curve and a fabricated dB figure. The
check is what turns those from silent wrong answers into a failure, and it is
why the ladder is measured on both axes rather than trusted on one.

A note on the transmitter's provenance
--------------------------------------

archive/captures/README.md rules that `ant_sim_py` is not an acceptable
transmitter for a *profile-conformance* baseline, and that stands. It is
acceptable here, and the reason the two do not conflict: a sensitivity ladder
measures RF level, the host pacing this master affects timing, and both sides of
any A/B see the identical transmitter within one sitting. The JSON records
`transmitter: ant_sim_py` so nobody has to reconstruct that argument later.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import random
import statistics
import sys
import threading
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

# These exit rather than raise when pyusb is missing, so importing this module
# exits too. tools/test_ant_sens.py catches that and skips, the same way
# tools/test_ant_sim.py does.
import ant_pages as ap  # noqa: E402
import ant_sim  # noqa: E402
import ant_verify  # noqa: E402
from ant_probe import (  # noqa: E402
    EP_OUT,
    FrameReader,
    close_device,
    frame,
    open_device,
    reset_stack,
)
from ant_scan import ANT_PLUS_KEY, ANT_PLUS_FREQ, command  # noqa: E402
from ant_session import wait_for_close  # noqa: E402
from ant_wire import (  # noqa: E402
    MESG_CHANNEL_RADIO_TX_POWER_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_NETWORK_KEY_ID,
    MESG_UNASSIGN_CHANNEL_ID,
    RADIO_TX_POWER_LVL_CUSTOM,
)


# ---------------------------------------------------------------------------
# The dial
# ---------------------------------------------------------------------------

# The six levels ANT names, and the dBm each one means. tools/ant_wire.py holds
# the same table in its constant comments and src/ant_serial_bridge.c holds the
# firmware half; this is the one a ladder is built from.
LEVEL_DBM = {0: -20, 1: -12, 2: -4, 3: 0, 4: 4, 5: 8}
DBM_LEVEL = {dbm: level for level, dbm in LEVEL_DBM.items()}

# Raw TXPOWER register values, per part, for the fine ladder.
#
# THIS TABLE IS PART-SPECIFIC AND THERE IS NO WAY TO ASK THE BOARD WHICH PART IT
# IS. radiant_core/src/radiant_radio_nrf.c says why at length: nRF52840 encodes
# TXPOWER as signed dBm, so +8 is 0x08 and -40 is 0xD8, and nRF54L15 encodes +8
# as 0x3F and 0 as 0x18. The same byte means two different powers on the two
# parts, with no error anywhere on the wrong one.
#
# So only parts with an entry here can drive a fine ladder, and the RSSI slope
# check below is what catches the case where --tx-part named the wrong one.
PART_FINE_DBM = {
    # radiant_core/src/radiant_radio_nrf.c radiant_txp_table[], which is every
    # setting the part has. The register value is the dBm in two's complement.
    "nrf52840": (8, 7, 6, 5, 4, 3, 2, 0, -4, -8, -12, -16, -20, -40),
}


def encode_power(dbm: int, part: str | None = None) -> tuple[int, int]:
    """The (level, custom) bytes of MESG_CHANNEL_RADIO_TX_POWER for `dbm`.

    A dBm that is one of the six named levels is sent as that level and needs
    nothing part-specific. Anything else needs the custom escape, which is a raw
    register value - so it needs `part`, and refuses rather than guessing.
    """
    if dbm in DBM_LEVEL:
        return DBM_LEVEL[dbm], 0
    if part is None:
        raise ValueError(
            f"{dbm} dBm is not one of the six ANT power levels "
            f"({sorted(LEVEL_DBM.values())}), so it needs the custom register "
            f"escape and therefore needs a part name")
    if part not in PART_FINE_DBM:
        raise ValueError(
            f"no TXPOWER register table for {part!r}. Known parts: "
            f"{sorted(PART_FINE_DBM)}. A raw register value guessed for the "
            f"wrong part transmits at some unrelated power with no error "
            f"anywhere - see radiant_core/src/radiant_radio_nrf.c")
    if dbm not in PART_FINE_DBM[part]:
        raise ValueError(f"{part} has no {dbm} dBm setting; it has "
                         f"{sorted(PART_FINE_DBM[part])}")
    return RADIO_TX_POWER_LVL_CUSTOM, dbm & 0xFF


def ladder_dbm(rungs: str, part: str | None, top: int | None = None,
               bottom: int | None = None) -> list[int]:
    """The ladder, loudest first. Loudest first is the point: the receiver has to
    be tracking before the link is walked into the ground, and a ladder that
    started at the bottom would spend every rung in search.
    """
    if rungs == "levels":
        steps = sorted(LEVEL_DBM.values(), reverse=True)
    elif rungs == "fine":
        if part not in PART_FINE_DBM:
            raise ValueError(
                f"--rungs fine needs a known --tx-part; {part!r} is not one of "
                f"{sorted(PART_FINE_DBM)}")
        steps = sorted(PART_FINE_DBM[part], reverse=True)
    else:
        raise ValueError(f"unknown rung set {rungs!r}")

    if top is not None:
        steps = [d for d in steps if d <= top]
    if bottom is not None:
        steps = [d for d in steps if d >= bottom]
    if len(steps) < 2:
        raise ValueError("a ladder needs at least two rungs")
    return steps


# ---------------------------------------------------------------------------
# Reading the curve
# ---------------------------------------------------------------------------

# Below this the interpolation is between two rungs that are far enough apart
# that the answer is mostly the straight line's opinion rather than the link's.
# Not an error - the coarse ladder's -12 to -20 gap is 8 dB by construction -
# but it is reported, because it bounds what the 1 dB gate can mean.
WIDE_BRACKET_DB = 5.0


def interpolate_knee(steps: list[dict], target_pct: float = 5.0) -> dict:
    """Where the loss curve crosses `target_pct`, in dBm.

    `steps` are rungs in the order they were measured - loudest first - each a
    dict with `tx_power_dbm` and `loss_pct`. The crossing taken is the first one
    walking *down* from the loud end: below the knee the curve is noise around
    the bench floor and can wander back and forth across a 5 % line, and the
    first time it goes over is the knee. Taking the last crossing instead would
    report whichever rung happened to bounce lowest at 90 % loss.

    Linear in (dBm, loss %) between the two bracketing rungs. Packet error rate
    against SNR is not linear, so this is only as good as the rungs are close -
    which is exactly what `bracket_db` is reported for, and exactly why the
    Phase 0 gate is a repeat run rather than an appeal to the arithmetic.

    Returns a dict that always carries `reason`; `tx_power_dbm` is None whenever
    the ladder did not actually contain the crossing. Nothing here extrapolates.
    """
    if len(steps) < 2:
        return {"tx_power_dbm": None, "reason": "fewer than two rungs"}

    ordered = sorted(steps, key=lambda s: -s["tx_power_dbm"])
    first = ordered[0]
    if first["loss_pct"] > target_pct:
        return {
            "tx_power_dbm": None,
            "reason": (f"the loudest rung ({first['tx_power_dbm']} dBm) already "
                       f"loses {first['loss_pct']:.2f} %, over the "
                       f"{target_pct:.1f} % target - the link is broken before "
                       f"the ladder starts, so this measures the bench and not "
                       f"the receiver"),
        }

    for above, below in zip(ordered, ordered[1:]):
        if below["loss_pct"] <= target_pct:
            continue
        span = below["loss_pct"] - above["loss_pct"]
        if span <= 0:
            # Cannot happen given the guard above, but a zero divisor here
            # would be an invented number rather than an exception.
            continue
        fraction = (target_pct - above["loss_pct"]) / span
        dbm = (above["tx_power_dbm"]
               + fraction * (below["tx_power_dbm"] - above["tx_power_dbm"]))
        bracket = abs(above["tx_power_dbm"] - below["tx_power_dbm"])
        result = {
            "tx_power_dbm": dbm,
            "bracket_db": bracket,
            "bracket": [above["tx_power_dbm"], below["tx_power_dbm"]],
            "bracket_loss_pct": [above["loss_pct"], below["loss_pct"]],
            "reason": "interpolated between the two rungs that bracket it",
        }
        if bracket > WIDE_BRACKET_DB:
            result["reason"] += (
                f"; the bracket is {bracket:.0f} dB wide, which is most of "
                f"what this number is made of - use --rungs fine for a "
                f"tighter one")
        return result

    last = ordered[-1]
    return {
        "tx_power_dbm": None,
        "reason": (f"the quietest rung ({last['tx_power_dbm']} dBm) still only "
                   f"loses {last['loss_pct']:.2f} %, so the knee is below the "
                   f"bottom of the ladder. The ladder spans "
                   f"{first['tx_power_dbm'] - last['tx_power_dbm']} dB and the "
                   f"link needs more: move the boards apart and run it again. "
                   f"Extrapolating past the last rung would invent the answer"),
    }


def rssi_slope(steps: list[dict]) -> dict:
    """How far the measured RSSI actually moved per commanded dB.

    A least-squares slope of mean RSSI against commanded transmit power, over
    the rungs where the receiver still heard enough to have an opinion. One is
    the right answer and the only right answer: the path between the boards does
    not care what the transmitter's register says, so a dB commanded is a dB
    received.

    Rungs past the knee are excluded. Once most packets are gone the survivors
    are the ones that faded up, so their mean RSSI stops falling and the slope
    flattens for a reason that has nothing to do with the dial.
    """
    usable = [s for s in steps
              if s.get("rssi_dbm_mean") is not None
              and s.get("rssi_samples", 0) >= 10
              and s["loss_pct"] <= 20.0]
    if len(usable) < 3:
        return {"slope": None, "n": len(usable),
                "reason": "fewer than three rungs with usable RSSI below the knee"}

    xs = [s["tx_power_dbm"] for s in usable]
    ys = [s["rssi_dbm_mean"] for s in usable]
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    denominator = sum((x - mean_x) ** 2 for x in xs)
    if denominator == 0:
        return {"slope": None, "n": len(usable),
                "reason": "every usable rung commanded the same power"}
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / denominator
    residuals = [y - (mean_y + slope * (x - mean_x)) for x, y in zip(xs, ys)]
    return {
        "slope": slope,
        "n": len(usable),
        "worst_residual_db": max(abs(r) for r in residuals),
        "reason": "least squares of mean RSSI against commanded transmit power",
    }


# How far the slope may sit from 1, and how far any one rung may sit off the
# fitted line.
#
# The residual bound is not redundant with the slope bound and is the one that
# does the work. CONFIG_ANT_DONGLE_TX_POWER_BOOST folds levels 3 and 4 onto +8
# dBm, which collapses the top three rungs of the coarse ladder into one point
# and leaves the bottom three where they were: the best-fit slope through that
# is 1.12, comfortably inside any sane slope tolerance, while three of the six
# rungs sit 3-6 dB off the line. A ladder whose top is squashed and whose bottom
# is not still walks the right total number of dB, so only the shape gives it
# away.
#
# 3 dB is loose against a real path - the RSSI figures are means over hundreds
# of packets on a fixed bench, so the residual is the part's own transmit-power
# accuracy and little else - and tight against every folding failure.
SLOPE_TOLERANCE = 0.25
MAX_RESIDUAL_DB = 3.0


def slope_verdict(slope: dict, tolerance: float = SLOPE_TOLERANCE,
                  max_residual: float = MAX_RESIDUAL_DB) -> tuple[bool, str]:
    """Whether the dial can be believed. See the module docstring for the three
    ways it comes out wrong, all of which produce a curve that looks fine."""
    if slope["slope"] is None:
        return False, (f"the transmit-power dial was never checked against "
                       f"measured RSSI ({slope['reason']}), so nothing here "
                       f"knows whether the ladder moved the power it asked for")
    blame = ("Check CONFIG_ANT_DONGLE_TX_POWER_BOOST on the transmitter (it "
             "folds levels 3 and 4 onto the same power), and --tx-part if "
             "--rungs fine is in use")
    if abs(slope["slope"] - 1.0) > tolerance:
        return False, (
            f"RSSI moved {slope['slope']:.2f} dB per commanded dB, not ~1. The "
            f"transmitter is not transmitting at the powers it was told to. "
            f"{blame}")
    if slope["worst_residual_db"] > max_residual:
        return False, (
            f"the ladder's average slope is {slope['slope']:.2f} dB per "
            f"commanded dB, but one rung sits "
            f"{slope['worst_residual_db']:.1f} dB off the line (limit "
            f"{max_residual:.1f}) - so the ladder walks about the right total "
            f"distance while some of its rungs are not where they were asked "
            f"to be. {blame}")
    return True, (f"RSSI moved {slope['slope']:.2f} dB per commanded dB over "
                  f"{slope['n']} rungs, worst rung "
                  f"{slope['worst_residual_db']:.1f} dB off the line, so the "
                  f"dial is real")


def agreement_db(values: list[float]) -> float | None:
    """Spread across repeated ladders. The Phase 0 gate is on this, not on the
    ladder's own arithmetic: two runs of a broken instrument agree perfectly."""
    clean = [v for v in values if v is not None]
    if len(clean) < 2:
        return None
    return max(clean) - min(clean)


# ---------------------------------------------------------------------------
# Driving the master
# ---------------------------------------------------------------------------

class MasterDriver(threading.Thread):
    """tools/ant_sim.py's EVENT_TX pacing loop, on a thread, with a power dial.

    The pacing is deliberately not reimplemented: `ant_sim.Sensor` builds the
    pages and `run()`'s rule - load the next payload when the stack says the
    last one went out - is reproduced here rather than approximated, because a
    master that falls back to wall-clock pacing transmits at a rate the receiver
    is not expecting and manufactures exactly the loss this tool is measuring.

    A thread, and not a subprocess per rung, so the receiver stays tracked for
    the whole ladder. Every rung is then the same acquired link at a different
    power, which is the measurement; restarting the master per rung would put a
    re-acquisition inside every step and count it as loss.
    """

    def __init__(self, dev, reader, sensor, verbose: bool = False):
        super().__init__(daemon=True, name="ant-sens-master")
        self.dev = dev
        self.reader = reader
        self.sensor = sensor
        self.verbose = verbose

        self.sent = 0
        self.events = 0
        self.fallbacks = 0
        self.failures: list[str] = []

        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._pending: tuple[int, int] | None = None
        self._applied = threading.Event()

    # -- the dial, called from the measuring thread --------------------------

    def set_power(self, level: int, custom: int, timeout: float = 5.0) -> bool:
        """Ask for a transmit power and block until the master has applied it.

        The command has to be sent from the thread that owns the reader, or its
        acknowledgement is consumed by whichever loop reads next and the
        confirmation is lost.
        """
        with self._lock:
            self._pending = (level, custom)
            self._applied.clear()
        return self._applied.wait(timeout)

    def stop(self) -> None:
        self._stop.set()

    # -- the loop ------------------------------------------------------------

    def _send(self) -> None:
        self.dev.write(EP_OUT, frame(ant_sim.MESG_BROADCAST_DATA_ID,
                                     bytes([self.sensor.channel])
                                     + self.sensor.next_payload()))
        self.sent += 1

    def _apply_pending(self) -> None:
        with self._lock:
            pending = self._pending
            self._pending = None
        if pending is None:
            return
        level, custom = pending
        # Three bytes, not two. A stick that predates the custom byte ignores
        # the third and applies the level, which is exactly right for the six
        # named levels and exactly wrong for a custom one - which is what the
        # RSSI slope check is there to notice.
        payload = bytes([self.sensor.channel, level, custom])
        if not command(self.dev, self.reader, MESG_CHANNEL_RADIO_TX_POWER_ID,
                       payload, f"tx power level 0x{level:02X} "
                                f"custom 0x{custom:02X}"):
            self.failures.append(
                f"the transmitter refused level 0x{level:02X}")
        self._applied.set()

    def run(self) -> None:
        deadline = time.monotonic() + 2.0 * self.sensor.period_s
        while not self._stop.is_set():
            self._apply_pending()

            result = self.reader.next_frame(time.monotonic() + 0.25)
            if result is not None:
                msg_id, body = result
                if (msg_id == ant_sim.MESG_RESPONSE_EVENT_ID and len(body) >= 3
                        and body[1] == ant_sim.EVENT_MARKER
                        and body[2] == ant_sim.EVENT_TX
                        and body[0] == self.sensor.channel):
                    self.events += 1
                    self._send()
                    deadline = (time.monotonic()
                                + 2.0 * self.sensor.period_s)

            if time.monotonic() >= deadline:
                # Same rule and the same reason as ant_sim.run(): two periods,
                # not one, so ordinary jitter does not double the message rate.
                self.fallbacks += 1
                self._send()
                deadline = time.monotonic() + 2.0 * self.sensor.period_s

        # Whatever is pending when the ladder ends is nobody's business, but a
        # caller blocked in set_power() is.
        self._applied.set()


# ---------------------------------------------------------------------------
# The measurement
# ---------------------------------------------------------------------------

def measure_rung(rx_dev, rx_reader, channel: int, profile: str, seconds: float,
                 master: MasterDriver, verbose: bool) -> dict:
    """One rung: listen, then let ant_verify.py say how much was lost.

    Loss is not counted here. `ChannelAnalyzer.summary()` reports `exact_loss`
    from the transmitter's own event counter - no clock, no nominal period, no
    rounding - and that is the figure every gate in this project reads. The one
    thing this adds is a fallback for the rungs past the knee, where too few
    packets arrive for that counter to be readable at all and the analyser
    rightly declines to report it.
    """
    spec = ap.PROFILES[profile]
    analyzer = ant_verify.ChannelAnalyzer(profile, spec["device_type"],
                                          spec["period"], None, None, 2.105)
    sent_before = master.sent
    # The thresholds are irrelevant - this reads numbers out of the summary and
    # never its verdicts - but they have to be permissive or the summary spends
    # its time deciding a rung past the knee has failed, which is the point of
    # the rung.
    extra = ant_verify.listen(rx_dev, rx_reader, {channel: analyzer}, seconds,
                              False, [])
    sent = master.sent - sent_before
    summary = analyzer.summary(100.0, 10.0, None)

    exact = summary.get("exact_loss")
    if exact is not None:
        loss_pct = exact["loss_pct"]
        basis = "event_counter"
    elif sent > 0:
        # Past the knee. The receiver has too little of the stream left to read
        # the transmitter's counter out of it, so the count this host handed the
        # transmitter stands in - which is the same quantity by a shorter path,
        # and only ever used where the answer is "most of it is gone".
        loss_pct = 100.0 * max(0, sent - summary["packets"]) / sent
        basis = "master_sent"
    else:
        loss_pct = float("nan")
        basis = "none"

    signal = summary.get("signal")
    fails = sum(count for name, count in extra.get("events", {}).items()
                if name in ant_verify.RADIO_FAIL_EVENTS)

    return {
        "loss_pct": loss_pct,
        "loss_basis": basis,
        "packets": summary["packets"],
        "master_sent": sent,
        "rx_fail": fails,
        "rssi_dbm_mean": signal["mean_dbm"] if signal else None,
        "rssi_samples": signal["n"] if signal else 0,
        "accumulator_violations": summary["violations"]["count"],
        "events": extra.get("events", {}),
    }


def run_ladder(rx_dev, rx_reader, channel: int, profile: str, rungs: list[int],
               part: str | None, seconds: float, settle: float,
               master: MasterDriver, verbose: bool) -> list[dict]:
    steps = []
    for dbm in rungs:
        level, custom = encode_power(dbm, part)
        if not master.set_power(level, custom):
            print(f"  ! the transmitter did not confirm {dbm} dBm within 5 s")
        # Give the master a slot or two at the new power before anything is
        # counted. A rung that includes the change includes a partial rung.
        time.sleep(settle)
        step = measure_rung(rx_dev, rx_reader, channel, profile, seconds,
                            master, verbose)
        step["tx_power_dbm"] = dbm
        step["tx_level"] = level
        step["tx_custom"] = custom
        steps.append(step)
        if verbose:
            rssi = step["rssi_dbm_mean"]
            print(f"  {dbm:+4d} dBm: loss {step['loss_pct']:6.2f} % "
                  f"({step['loss_basis']}), {step['packets']} packets"
                  + (f", RSSI {rssi:.1f} dBm" if rssi is not None else ""))
    return steps


def derive(steps: list[dict], target_pct: float) -> dict:
    """Everything the ladder says, including what it declines to say."""
    knee = interpolate_knee(steps, target_pct)
    slope = rssi_slope(steps)
    trustworthy, slope_detail = slope_verdict(slope)

    result = {
        "loss5pct_tx_power_dbm": knee["tx_power_dbm"],
        "knee": knee,
        "rssi_slope": slope,
        "dial_trustworthy": trustworthy,
        "dial_detail": slope_detail,
    }

    # The knee expressed as received power rather than as transmitted power,
    # which is the number that means something off this bench. Derived from the
    # reference rung and the dB walked down from it rather than read off the
    # rung at the knee: past the knee the packets that survive are the ones that
    # faded up, so their RSSI is biased high by exactly the thing being
    # measured. The path loss does not change, so the subtraction is exact and
    # the selection bias never enters.
    reference = next((s for s in sorted(steps, key=lambda s: -s["tx_power_dbm"])
                      if s.get("rssi_dbm_mean") is not None
                      and s.get("rssi_samples", 0) >= 10
                      and s["loss_pct"] <= 5.0), None)
    if reference is not None and knee["tx_power_dbm"] is not None:
        result["loss5pct_rssi_dbm"] = (
            reference["rssi_dbm_mean"]
            - (reference["tx_power_dbm"] - knee["tx_power_dbm"]))
        result["rssi_reference"] = {
            "tx_power_dbm": reference["tx_power_dbm"],
            "rssi_dbm_mean": reference["rssi_dbm_mean"],
        }
    else:
        result["loss5pct_rssi_dbm"] = None
    return result


# ---------------------------------------------------------------------------
# Bring-up
# ---------------------------------------------------------------------------

def open_master(args, verbose: bool):
    dev = open_device(verbose, serial=args.tx_serial, port=args.tx_port,
                      baud=args.baud)
    reader = FrameReader(dev)
    if not reset_stack(dev, reader):
        sys.exit("the transmitter sent no startup message after reset")
    if not command(dev, reader, MESG_NETWORK_KEY_ID,
                   bytes([ap.ANT_PLUS_NETWORK_NUM]) + ANT_PLUS_KEY,
                   "tx ANT+ network key"):
        sys.exit("the transmitter refused the ANT+ network key")

    rng = random.Random(args.seed)
    sensor = ant_sim.SENSORS[args.profile](
        args.channel, args.device_number, args.trans_type,
        ant_sim.Signal(args.watts, args.noise, rng),
        ant_sim.Signal(args.cadence, args.noise, rng))
    if not sensor.bring_up(dev, reader):
        sys.exit(f"the transmitter's channel {sensor.channel} did not open")
    return dev, reader, sensor


def open_receiver(args, verbose: bool):
    dev = open_device(verbose, serial=args.rx_serial, port=args.rx_port,
                      baud=args.baud)
    reader = FrameReader(dev)
    if not reset_stack(dev, reader):
        sys.exit("the receiver sent no startup message after reset")
    # 0xE0: channel id, RSSI and the receive timestamp. RSSI is not optional
    # here - it is the axis the dial is checked against, and without it this
    # tool cannot tell a working ladder from a stuck one.
    command(dev, reader, ant_verify.MESG_ANTLIB_CONFIG_ID,
            bytes([0x00, ant_verify.LIB_CONFIG_ALL_EXT_FIELDS]),
            "rx extended messages")
    if not command(dev, reader, MESG_NETWORK_KEY_ID,
                   bytes([ap.ANT_PLUS_NETWORK_NUM]) + ANT_PLUS_KEY,
                   "rx ANT+ network key"):
        sys.exit("the receiver refused the ANT+ network key")

    spec = ap.PROFILES[args.profile]
    if not ant_verify.open_slave(dev, reader, args.channel, spec["device_type"],
                                 spec["period"], 0, 0, args.rf_freq):
        sys.exit(f"the receiver's channel {args.channel} did not open")
    return dev, reader


def close_channel(dev, reader, channel: int, what: str) -> None:
    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([channel]),
            f"{what} close ch{channel}")
    if not wait_for_close(reader, channel):
        print(f"  ! {what} channel {channel} never reported closed")
    command(dev, reader, MESG_UNASSIGN_CHANNEL_ID, bytes([channel]),
            f"{what} unassign ch{channel}")


def report(result: dict) -> None:
    for index, ladder in enumerate(result["ladders"], start=1):
        print(f"\n=== ladder {index} of {len(result['ladders'])} ===")
        for step in ladder["steps"]:
            rssi = step["rssi_dbm_mean"]
            print(f"  {step['tx_power_dbm']:+4d} dBm  "
                  f"loss {step['loss_pct']:6.2f} %  "
                  f"{step['packets']:5d} packets  "
                  f"{step['rx_fail']:5d} RX_FAIL  "
                  + (f"RSSI {rssi:6.1f} dBm" if rssi is not None
                     else "RSSI      -   ")
                  + f"  [{step['loss_basis']}]")
        knee = ladder["knee"]
        if knee["tx_power_dbm"] is None:
            print(f"  no 5 % point: {knee['reason']}")
        else:
            print(f"  5 % loss at {knee['tx_power_dbm']:.2f} dBm transmitted "
                  f"({knee['reason']})")
            if ladder.get("loss5pct_rssi_dbm") is not None:
                print(f"    which is {ladder['loss5pct_rssi_dbm']:.1f} dBm at "
                      f"the receiver, from the "
                      f"{ladder['rssi_reference']['tx_power_dbm']:+d} dBm rung")
        print(f"  dial: {ladder['dial_detail']}")

    for check in result["checks"]:
        print(f"\n{'OK  ' if check['pass'] else 'FAIL'} {check['name']}: "
              f"{check['detail']}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile", default="power", choices=sorted(ap.PROFILES),
                        help="profile to transmit and measure (default: power)")
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="how long to measure each rung (default: 60). At "
                             "~4 Hz that is 240 packets, so one packet is "
                             "0.42 %% - the resolution the 5 %% point is read "
                             "at")
    parser.add_argument("--settle", type=float, default=2.0,
                        help="seconds after changing power before counting "
                             "again (default: 2)")
    parser.add_argument("--target", type=float, default=5.0,
                        help="loss percentage the reported point is at "
                             "(default: 5)")
    parser.add_argument("--repeat", type=int, default=1,
                        help="run the whole ladder this many times. Two is the "
                             "Phase 0 acceptance run: the instrument is only "
                             "good enough to grade a 1 dB claim if two ladders "
                             "on an unchanged rig agree to within 1 dB")
    parser.add_argument("--max-repeat-delta-db", type=float, default=1.0,
                        help="how far repeated ladders may disagree "
                             "(default: 1.0, matching gates.sensitivity)")
    parser.add_argument("--rungs", default="levels", choices=("levels", "fine"),
                        help="levels: the six ANT power levels, -20 to +8 dBm, "
                             "which every stick supports. fine: the part's own "
                             "register table via the custom byte, which needs "
                             "--tx-part and needs the transmitter's firmware to "
                             "carry byte 2 of 0x60 (default: levels)")
    parser.add_argument("--tx-part", default="nrf52840",
                        choices=sorted(PART_FINE_DBM),
                        help="the transmitter's SoC, for --rungs fine. The raw "
                             "TXPOWER encoding differs between parts and there "
                             "is no way to ask the board (default: nrf52840)")
    parser.add_argument("--top-dbm", type=int,
                        help="drop rungs above this")
    parser.add_argument("--bottom-dbm", type=int,
                        help="drop rungs below this")
    parser.add_argument("--rf-freq", type=int, default=ANT_PLUS_FREQ,
                        help="RF frequency as MHz above 2400 (default: 57)")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--device-number", type=int, default=0x3A17)
    parser.add_argument("--trans-type", type=int, default=5)
    parser.add_argument("--watts", type=float, default=100.0)
    parser.add_argument("--cadence", type=float, default=80.0)
    parser.add_argument("--noise", type=float, default=3.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--tx-serial", help="serial suffix of the transmitter")
    parser.add_argument("--tx-port", help="serial port of the transmitter")
    parser.add_argument("--rx-serial", help="serial suffix of the receiver")
    parser.add_argument("--rx-port", help="serial port of the receiver")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--json", metavar="FILE", nargs="?", const="-",
                        help="write the sensitivity block as JSON, in the shape "
                             "archive/benchmarks/baseline.schema.json expects "
                             "(to stdout if no file)")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    json_to_stdout = args.json == "-"
    verbose = not args.quiet and not json_to_stdout

    stack = contextlib.ExitStack()
    if json_to_stdout:
        stack.enter_context(contextlib.redirect_stdout(sys.stderr))

    part = args.tx_part if args.rungs == "fine" else None
    try:
        rungs = ladder_dbm(args.rungs, part, args.top_dbm, args.bottom_dbm)
    except ValueError as exc:
        sys.exit(str(exc))

    print(f"Ladder: {', '.join(f'{d:+d}' for d in rungs)} dBm, "
          f"{args.seconds:.0f} s per rung, {args.repeat} pass(es)")
    print(f"Estimated bench time: "
          f"{args.repeat * len(rungs) * (args.seconds + args.settle) / 60.0:.0f} "
          f"minutes")

    print("\nOpening the transmitter")
    tx_dev, tx_reader, sensor = open_master(args, verbose)
    print("\nOpening the receiver")
    rx_dev, rx_reader = open_receiver(args, verbose)

    master = MasterDriver(tx_dev, tx_reader, sensor, verbose)
    master.start()

    ladders = []
    try:
        for pass_index in range(args.repeat):
            print(f"\nLadder {pass_index + 1} of {args.repeat}")
            steps = run_ladder(rx_dev, rx_reader, args.channel, args.profile,
                               rungs, part, args.seconds, args.settle, master,
                               verbose)
            ladder = {"steps": steps}
            ladder.update(derive(steps, args.target))
            ladders.append(ladder)
    finally:
        master.stop()
        master.join(timeout=5.0)
        print("\nClosing")
        close_channel(rx_dev, rx_reader, args.channel, "rx")
        close_channel(tx_dev, tx_reader, args.channel, "tx")
        close_device(rx_dev)
        close_device(tx_dev)

    knees = [ladder["loss5pct_tx_power_dbm"] for ladder in ladders]
    checks = []

    def check(name: str, ok: bool, detail: str) -> None:
        checks.append({"name": name, "pass": bool(ok), "detail": detail})

    check("knee reached", all(k is not None for k in knees),
          "; ".join(ladder["knee"]["reason"] for ladder in ladders))
    check("dial", all(ladder["dial_trustworthy"] for ladder in ladders),
          "; ".join(ladder["dial_detail"] for ladder in ladders))
    if master.fallbacks:
        check("EVENT_TX pacing", False,
              f"{master.fallbacks} of {master.sent} messages went out on the "
              f"wall-clock fallback rather than paced by EVENT_TX, so the "
              f"transmitter's rate was guessed and the loss figures are not "
              f"the link's")
    else:
        check("EVENT_TX pacing", True,
              f"all {master.sent} messages paced by EVENT_TX")
    for failure in master.failures:
        check("transmit power accepted", False, failure)

    spread = agreement_db(knees)
    if spread is not None:
        # The Phase 0 gate. Everything downstream of this file is graded against
        # a 1 dB claim, and an instrument whose own repeat is worse than 1 dB
        # cannot grade one - so this failing means Phases 2 and 5 have no gate,
        # not that this run was unlucky.
        check("repeatability", spread <= args.max_repeat_delta_db,
              f"{len(knees)} ladders spread {spread:.2f} dB "
              f"(limit {args.max_repeat_delta_db:.2f} dB). Below the limit the "
              f"instrument can grade a 1 dB claim; above it, nothing that "
              f"depends on one has a gate")
    elif args.repeat > 1:
        check("repeatability", False,
              "not every ladder produced a 5 % point, so they cannot be "
              "compared")

    result = {
        "method": "tx_ladder",
        "transmitter": "ant_sim_py",
        "profile": args.profile,
        "rf_channel": args.rf_freq,
        "seconds_per_step": args.seconds,
        "rungs": rungs,
        "tx_part": part,
        "ladders": ladders,
        # The fields archive/benchmarks/baseline.schema.json names, lifted from
        # the first ladder, so this block can be pasted into a baseline whole.
        "steps": [{"tx_power_dbm": s["tx_power_dbm"],
                   "loss_exact_pct": s["loss_pct"],
                   # Which counter the figure came from. Named, because past
                   # the knee it is not the transmitter's event counter and a
                   # field called loss_exact_pct would otherwise say it was.
                   "loss_basis": s["loss_basis"],
                   "packets": s["packets"],
                   "rx_fail": s["rx_fail"],
                   "rssi_dbm_mean": s["rssi_dbm_mean"]}
                  for s in ladders[0]["steps"]] if ladders else [],
        "loss5pct_tx_power_dbm": knees[0] if knees else None,
        "loss5pct_rssi_dbm": (ladders[0]["loss5pct_rssi_dbm"]
                              if ladders else None),
        "repeat_spread_db": spread,
        "checks": checks,
        "pass": all(c["pass"] for c in checks),
    }

    stack.close()

    if json_to_stdout:
        json.dump(result, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0 if result["pass"] else 1

    report(result)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=2)
        print(f"\nwrote {args.json}")

    print("\nPASS" if result["pass"] else "\nFAILED")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
