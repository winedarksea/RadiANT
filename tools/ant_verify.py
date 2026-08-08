#!/usr/bin/env python3
"""Measure an ANT+ sensor: loss, jitter, decoded accuracy, accumulator sanity.

ant_scan.py answers "did anything arrive". This answers "was it right", which
is the question a closed loop needs. It opens a slave channel, decodes what it
hears, and reports numbers rather than adjectives:

    python tools/ant_verify.py --profile power --expect-watts 100 \
        --expect-rpm 80 --seconds 60 --json

It is told nothing about the transmitter. Every expectation is reconstructed
from the stream itself - the channel period gives the packet count to expect,
the accumulators give the power to expect, the page numbers give the rotation
to expect - because in Phase 2 the transmitter is sdk-ant firmware that this
script has no inside knowledge of. An identical pass against tools/ant_sim.py
and against sim/ is the evidence that the two agree.

Three ways to use it:

    --replay FILE   analyse a capture instead of a radio; no hardware at all
    --record FILE   save what was heard, for replay here and in
                    zephyr_aerosense's host tests
    --json          machine-readable results, following ant_bench.py

**Two boards are required for a live run.** This one receives; something else
has to transmit.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import statistics
import sys
import time
from collections import Counter

try:
    import usb.core
    import usb.util
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402
from ant_probe import (  # noqa: E402
    FrameReader,
    MESG_RESPONSE_EVENT_ID,
    close_device,
    open_device,
    reset_stack,
)
from ant_scan import (  # noqa: E402
    ANT_PLUS_KEY,
    ANT_PLUS_FREQ,
    MESG_ACKNOWLEDGED_DATA_ID,
    MESG_ANTLIB_CONFIG_ID,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_BROADCAST_DATA_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    command,
)
from ant_session import (  # noqa: E402
    EVENT_MARKER,
    EVENT_NAMES,
    MESG_UNASSIGN_CHANNEL_ID,
    wait_for_close,
)

CHANNEL_TYPE_SLAVE = 0x00
ANT_TICKS_PER_S = 32768.0

# The profile requires a common page at least once per 65 messages. Sixty-five
# is the limit, not the target, so it is checked as an inequality.
COMMON_PAGE_LIMIT = 65

# Default acceptance thresholds. They are arguments, not constants, because the
# right numbers depend on the transmitter's noise band - but a default that
# passes a good link and fails an obviously bad one is worth having.
DEFAULT_MAX_LOSS_PCT = 1.0
DEFAULT_JITTER_FACTOR = 0.5      # of one channel period, on the stddev


class ChannelAnalyzer:
    """Decodes one channel's stream and accumulates evidence about it.

    Live capture and replay both go through here, so a bug found on the bench
    can be reproduced from a saved file and vice versa - which is the whole
    point of --record.
    """

    def __init__(self, name: str, device_type: int, period: int,
                 expect_watts: float | None, expect_rpm: float | None,
                 wheel_circ_m: float):
        self.name = name
        self.device_type = device_type
        self.period = period
        self.period_s = period / ANT_TICKS_PER_S
        self.expect_watts = expect_watts
        self.expect_rpm = expect_rpm
        self.wheel_circ_m = wheel_circ_m

        self.packets = 0
        self.first_t: float | None = None
        self.last_t: float | None = None
        self.intervals: list[float] = []

        self.pages = Counter()
        self.messages_since_common: int | None = None
        self.common_gaps: list[int] = []
        self.common_pages_seen = Counter()

        self.violations: list[str] = []
        self.violation_count = 0
        self.wraps = Counter()

        # A transmitter can be perfectly alive on the air and dead as a sensor:
        # every packet identical, event counter frozen. Loss is zero, jitter is
        # zero and the decoded power is whatever the stuck packet says, so every
        # other check here passes. Counting how often the event counter actually
        # moves is the only thing that notices.
        self.event_comparisons = 0
        self.event_advances = 0

        self.power_samples: list[float] = []
        self.cadence_samples: list[float] = []
        self.speed_samples: list[float] = []

        # One baseline per page number, not one for the whole channel. Pages
        # 0x11 and 0x12 are independent accumulator series that happen to share
        # a channel; differencing one against the other is write-back item 1
        # and produces plausible-looking garbage rather than an error.
        self.torque_baseline: dict[int, dict] = {}
        self.std_baseline: dict | None = None
        self.tq_freq_baseline: dict | None = None
        self.csc_baseline: dict | None = None

    # -- helpers ---------------------------------------------------------

    def _violation(self, message: str) -> None:
        self.violation_count += 1
        if len(self.violations) < 20:
            self.violations.append(message)

    def _note_wrap(self, field: str, now: int, before: int) -> None:
        if now < before:
            self.wraps[field] += 1

    def _note_event(self, advanced: bool) -> None:
        self.event_comparisons += 1
        if advanced:
            self.event_advances += 1

    # -- ingestion -------------------------------------------------------

    def feed(self, t: float, payload: bytes) -> None:
        if len(payload) < 8:
            self._violation(f"payload of {len(payload)} bytes, expected 8")
            return
        payload = bytes(payload[:8])

        self.packets += 1
        if self.first_t is None:
            self.first_t = t
        else:
            self.intervals.append(t - self.last_t)
        self.last_t = t

        if self.device_type == ap.BSC_COMBINED_DEVICE_TYPE:
            # No page number in this page at all, so there is nothing to
            # histogram and no common pages to expect.
            self._feed_csc(payload)
            return

        page = payload[0]
        self.pages[page] += 1

        if page in (ap.PAGE_COMMON_MANUFACTURER, ap.PAGE_COMMON_PRODUCT):
            self.common_pages_seen[page] += 1
            if self.messages_since_common is not None:
                self.common_gaps.append(self.messages_since_common)
            self.messages_since_common = 0
            return

        if self.messages_since_common is not None:
            self.messages_since_common += 1
        elif self.packets > COMMON_PAGE_LIMIT:
            # Nothing common has arrived yet and the limit has already gone
            # past. Start counting from here so the gap is reported once the
            # first one does show up.
            self.messages_since_common = 0

        if page == ap.PAGE_POWER_STANDARD:
            self._feed_power_std(payload)
        elif page in (ap.PAGE_POWER_WHEEL_TORQUE, ap.PAGE_POWER_CRANK_TORQUE):
            self._feed_power_torque(payload)
        elif page == ap.PAGE_POWER_TORQUE_FREQ:
            self._feed_power_torque_freq(payload)

    def _feed_power_std(self, payload: bytes) -> None:
        got = ap.decode_power_std(payload)
        if got["inst_power"] != ap.INVALID_U16:
            self.power_samples.append(float(got["inst_power"]))
        if got["cadence"] is not None:
            self.cadence_samples.append(float(got["cadence"]))

        before = self.std_baseline
        self.std_baseline = got
        if before is None:
            return

        d_event = ap.delta_u8(got["event_count"], before["event_count"])
        d_acc = ap.delta_u16(got["acc_power"], before["acc_power"])
        self._note_wrap("event_count", got["event_count"],
                        before["event_count"])
        self._note_wrap("acc_power", got["acc_power"], before["acc_power"])
        self._note_event(d_event != 0)

        if d_event == 0:
            if d_acc != 0:
                self._violation(
                    f"page 0x10: accumulated power advanced by {d_acc} with no "
                    "new event")
            return

        # The accumulator is the sum of the instantaneous values, so its delta
        # divided by the number of events is the average power over them. That
        # has to agree with the instantaneous figure or one of the two fields
        # is being built from something the other is not.
        implied = d_acc / d_event
        inst = got["inst_power"]
        tolerance = max(15.0, 0.35 * max(inst, 1))
        if abs(implied - inst) > tolerance:
            self._violation(
                f"page 0x10: accumulated power implies {implied:.0f} W over "
                f"{d_event} event(s), instantaneous says {inst} W")

    def _feed_power_torque(self, payload: bytes) -> None:
        got = ap.decode_power_torque(payload)
        page = got["page"]
        if got["cadence"] is not None:
            self.cadence_samples.append(float(got["cadence"]))

        before = self.torque_baseline.get(page)
        self.torque_baseline[page] = got
        if before is None:
            return

        d_event = ap.delta_u8(got["event_count"], before["event_count"])
        d_period = ap.delta_u16(got["acc_period"], before["acc_period"])
        d_torque = ap.delta_u16(got["acc_torque"], before["acc_torque"])
        self._note_wrap(f"acc_period(0x{page:02X})", got["acc_period"],
                        before["acc_period"])
        self._note_wrap(f"acc_torque(0x{page:02X})", got["acc_torque"],
                        before["acc_torque"])
        self._note_wrap(f"event_count(0x{page:02X})", got["event_count"],
                        before["event_count"])
        self._note_event(d_event != 0)

        if d_event == 0:
            if d_period != 0 or d_torque != 0:
                self._violation(
                    f"page 0x{page:02X}: accumulators advanced "
                    f"(period +{d_period}, torque +{d_torque}) with no new "
                    "event")
            return

        if d_period == 0:
            self._violation(
                f"page 0x{page:02X}: {d_event} new event(s) with no elapsed "
                "period - power is not recoverable from this pair")
            return

        watts = ap.power_from_torque(d_torque, d_period)
        if watts > 3000.0:
            self._violation(
                f"page 0x{page:02X}: {watts:.0f} W is not a bicycle - a delta "
                "was taken in the wrong width or against the wrong series")
            return
        self.power_samples.append(watts)

    def _feed_power_torque_freq(self, payload: bytes) -> None:
        got = ap.decode_power_torque_freq(payload)
        before = self.tq_freq_baseline
        self.tq_freq_baseline = got
        if before is None:
            return

        d_event = ap.delta_u8(got["event_count"], before["event_count"])
        d_time = ap.delta_u16(got["time_stamp"], before["time_stamp"])
        d_ticks = ap.delta_u16(got["torque_ticks"], before["torque_ticks"])
        self._note_wrap("time_stamp", got["time_stamp"], before["time_stamp"])
        self._note_wrap("torque_ticks", got["torque_ticks"],
                        before["torque_ticks"])
        self._note_event(d_event != 0)

        if d_event == 0:
            if d_time != 0 or d_ticks != 0:
                self._violation(
                    "page 0x20: time or ticks advanced with no new event")
            return
        if d_time == 0:
            self._violation("page 0x20: new event with no elapsed time stamp")
            return
        if got["slope_tenth_nm_hz"] == 0:
            self._violation("page 0x20: slope of zero, torque is undefined")
            return

        watts = ap.power_from_torque_freq(d_event, d_time, d_ticks,
                                          got["slope_tenth_nm_hz"])
        if 0.0 <= watts <= 3000.0:
            self.power_samples.append(watts)
        else:
            self._violation(f"page 0x20: {watts:.0f} W is not a bicycle")

    def _feed_csc(self, payload: bytes) -> None:
        got = ap.decode_bsc_combined(payload)
        before = self.csc_baseline
        self.csc_baseline = got
        if before is None:
            return

        d_cad_t = ap.delta_u16(got["cad_event_time"], before["cad_event_time"])
        d_cad_r = ap.delta_u16(got["cad_revs"], before["cad_revs"])
        d_spd_t = ap.delta_u16(got["spd_event_time"], before["spd_event_time"])
        d_spd_r = ap.delta_u16(got["spd_revs"], before["spd_revs"])
        self._note_wrap("cad_event_time", got["cad_event_time"],
                        before["cad_event_time"])
        self._note_wrap("cad_revs", got["cad_revs"], before["cad_revs"])
        self._note_wrap("spd_event_time", got["spd_event_time"],
                        before["spd_event_time"])
        self._note_wrap("spd_revs", got["spd_revs"], before["spd_revs"])
        self._note_event(d_cad_r != 0 or d_spd_r != 0)

        for what, d_t, d_r in (("cadence", d_cad_t, d_cad_r),
                               ("speed", d_spd_t, d_spd_r)):
            if d_r == 0:
                if d_t != 0:
                    self._violation(
                        f"0x79 {what}: event time advanced by {d_t} with no "
                        "new revolution")
                continue
            if d_t == 0:
                self._violation(
                    f"0x79 {what}: {d_r} revolution(s) with no elapsed event "
                    "time")
                continue
            if what == "cadence":
                self.cadence_samples.append(ap.cadence_rpm_from(d_r, d_t))
            else:
                self.speed_samples.append(
                    ap.speed_mps_from(d_r, d_t, self.wheel_circ_m))

    # -- results ---------------------------------------------------------

    def summary(self, max_loss_pct: float, jitter_factor: float,
                max_error: float | None) -> dict:
        elapsed = ((self.last_t - self.first_t)
                   if self.first_t is not None and self.last_t is not None
                   else 0.0)
        # Expected count is elapsed/period + 1, not elapsed/period: the window
        # is measured between the first and last packet received, so both ends
        # are packets. A period of zero means the device type is not one this
        # tool knows, in which case there is no count to expect and loss is not
        # a number - saying so beats inventing a denominator.
        expected = ((elapsed / self.period_s) + 1.0
                    if elapsed > 0 and self.period_s > 0 else 0.0)
        loss_pct = (100.0 * (1.0 - self.packets / expected)
                    if expected > 0 else float("nan"))

        jitter = {
            "n": len(self.intervals),
            "mean_s": statistics.fmean(self.intervals) if self.intervals else None,
            "stdev_s": (statistics.pstdev(self.intervals)
                        if len(self.intervals) > 1 else None),
            "max_s": max(self.intervals) if self.intervals else None,
            "period_s": self.period_s,
        }

        def accuracy(samples, expected_value):
            if not samples:
                return None
            mean = statistics.fmean(samples)
            result = {
                "n": len(samples),
                "mean": mean,
                "min": min(samples),
                "max": max(samples),
            }
            if expected_value is not None:
                result["expected"] = expected_value
                result["mean_abs_error"] = statistics.fmean(
                    abs(s - expected_value) for s in samples)
                result["mean_error"] = mean - expected_value
            return result

        power = accuracy(self.power_samples, self.expect_watts)
        cadence = accuracy(self.cadence_samples, self.expect_rpm)
        speed = accuracy(self.speed_samples, None)

        checks = []

        def check(name: str, ok: bool, detail: str) -> None:
            checks.append({"name": name, "pass": bool(ok), "detail": detail})

        check("packets", self.packets > 0,
              f"{self.packets} received, {expected:.0f} expected over "
              f"{elapsed:.1f} s")
        if self.packets and expected > 0:
            check("loss", loss_pct <= max_loss_pct,
                  f"{loss_pct:.2f} % (limit {max_loss_pct:.2f} %)")
        if jitter["stdev_s"] is not None and self.period_s > 0:
            limit = jitter_factor * self.period_s
            check("jitter", jitter["stdev_s"] <= limit,
                  f"stddev {jitter['stdev_s'] * 1000:.1f} ms, max "
                  f"{jitter['max_s'] * 1000:.1f} ms "
                  f"(limit {limit * 1000:.1f} ms, period "
                  f"{self.period_s * 1000:.1f} ms)")

        check("accumulator continuity", self.violation_count == 0,
              f"{self.violation_count} violation(s)")

        if self.event_comparisons:
            # Not a rate threshold: a real sensor at a very low cadence can go
            # many messages between events quite legitimately. Never advancing
            # at all across the whole run is the thing that cannot happen to a
            # working sensor.
            check("sensor liveness", self.event_advances > 0,
                  f"the event counter advanced on {self.event_advances} of "
                  f"{self.event_comparisons} packet pairs")

        for label, result, expected_value in (("power", power, self.expect_watts),
                                              ("cadence", cadence, self.expect_rpm)):
            if result is None or expected_value is None:
                continue
            limit = (max_error if max_error is not None
                     else max(5.0, 0.10 * expected_value))
            check(f"{label} accuracy",
                  result["mean_abs_error"] <= limit,
                  f"mean abs error {result['mean_abs_error']:.2f} against "
                  f"{expected_value:.0f} (limit {limit:.2f})")

        if self.device_type != ap.BSC_COMBINED_DEVICE_TYPE:
            worst_gap = max(self.common_gaps) if self.common_gaps else None
            saw_both = (self.common_pages_seen[ap.PAGE_COMMON_MANUFACTURER] > 0
                        and self.common_pages_seen[ap.PAGE_COMMON_PRODUCT] > 0)
            if self.packets > 2 * COMMON_PAGE_LIMIT:
                check("common pages",
                      saw_both and worst_gap is not None
                      and worst_gap <= COMMON_PAGE_LIMIT,
                      f"80 x{self.common_pages_seen[ap.PAGE_COMMON_MANUFACTURER]}, "
                      f"81 x{self.common_pages_seen[ap.PAGE_COMMON_PRODUCT]}, "
                      f"worst gap {worst_gap} (limit {COMMON_PAGE_LIMIT})")

        return {
            "profile": self.name,
            "device_type": self.device_type,
            "period_ticks": self.period,
            "packets": self.packets,
            "elapsed_s": elapsed,
            "expected_packets": expected,
            "loss_pct": loss_pct,
            "jitter": jitter,
            "pages": {f"0x{page:02X}": count
                      for page, count in sorted(self.pages.items())},
            "common_page_gaps": {
                "count": len(self.common_gaps),
                "max": max(self.common_gaps) if self.common_gaps else None,
            },
            "power": power,
            "cadence": cadence,
            "speed_mps": speed,
            "accumulator_wraps": dict(self.wraps),
            "event_advances": self.event_advances,
            "event_comparisons": self.event_comparisons,
            "violations": {
                "count": self.violation_count,
                "first": self.violations,
            },
            "checks": checks,
            "pass": all(c["pass"] for c in checks),
        }


def profile_for(device_type: int, pages: Counter) -> str:
    """Name the profile a stream belongs to, from the stream alone.

    A capture carries no channel numbers and no --profile flag: the channel a
    packet arrived on is a fact about the receiver, not about the sensor. What
    it does carry is the device type and the pages, and between them those name
    the profile - which keeps the analysis transmitter-agnostic, the property
    that lets the same run judge ant_sim.py and sim/ firmware alike.
    """
    if device_type == ap.BSC_COMBINED_DEVICE_TYPE:
        return "csc"
    if pages[ap.PAGE_POWER_TORQUE_FREQ]:
        return "power-torque-freq"
    if pages[ap.PAGE_POWER_WHEEL_TORQUE] or pages[ap.PAGE_POWER_CRANK_TORQUE]:
        return "power-torque"
    return "power"


def period_for(device_type: int, profile: str) -> int:
    spec = ap.PROFILES.get(profile)
    if spec is not None and spec["device_type"] == device_type:
        return spec["period"]
    for spec in ap.PROFILES.values():
        if spec["device_type"] == device_type:
            return spec["period"]
    # An unknown device type is still worth analysing for continuity; only the
    # loss figure needs a period, so say so rather than inventing one.
    return 0


def open_slave(dev, reader, channel: int, device_type: int, period: int,
               device_number: int, trans_type: int) -> bool:
    """Assign, configure and open one slave channel on the ANT+ network."""
    steps = [
        (MESG_ASSIGN_CHANNEL_ID,
         bytes([channel, CHANNEL_TYPE_SLAVE, ap.ANT_PLUS_NETWORK_NUM]),
         "assign slave"),
        (MESG_CHANNEL_ID_ID,
         bytes([channel, device_number & 0xFF, (device_number >> 8) & 0xFF,
                device_type, trans_type]), "channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([channel, ANT_PLUS_FREQ]),
         "radio frequency"),
        (MESG_CHANNEL_MESG_PERIOD_ID,
         bytes([channel, period & 0xFF, period >> 8]), "message period"),
        # 0xFF is infinite search. A verifier that gives up and closes its own
        # channel would report the resulting silence as loss.
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([channel, 0xFF]),
         "search timeout"),
        (MESG_OPEN_CHANNEL_ID, bytes([channel]), "open"),
    ]
    for msg_id, payload, what in steps:
        if not command(dev, reader, msg_id, payload, f"ch{channel} {what}"):
            return False
    return True


def listen(dev, reader, analyzers: dict, seconds: float, verbose: bool,
           records: list) -> dict:
    """Collect broadcasts for `seconds`, timestamping each on arrival.

    Arrival time is the host's, not the sensor's, so the jitter figure includes
    the USB path - which is the honest thing to report, since that path is part
    of what is being validated.
    """
    events = Counter()
    identities = {}
    start = time.monotonic()
    end = start + seconds
    last_report = start

    while time.monotonic() < end:
        result = reader.next_frame(min(end, time.monotonic() + 0.25))
        if result is None:
            continue
        msg_id, body = result
        now = time.monotonic() - start

        if msg_id in (MESG_BROADCAST_DATA_ID, MESG_ACKNOWLEDGED_DATA_ID):
            if not body:
                continue
            channel = body[0]
            analyzer = analyzers.get(channel)
            if analyzer is None:
                continue
            payload = body[1:9]
            analyzer.feed(now, payload)
            # The device number is filled in afterwards from the identity the
            # extended messages carry - it is not known when the first packets
            # arrive, and a capture with the sensor left anonymous cannot be
            # split back into per-sensor streams on replay.
            records.append([now, analyzer.device_type, channel, payload])
            # Extended messages carry the channel id after the payload, which
            # is the only way to know which sensor was actually heard rather
            # than which one was asked for.
            if len(body) >= 13 and body[9] & 0x80:
                identities[channel] = (body[10] | (body[11] << 8),
                                       body[12] & 0x7F)
        elif msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
            if body[1] == EVENT_MARKER:
                events[body[2]] += 1

        if verbose and time.monotonic() - last_report >= 5.0:
            last_report = time.monotonic()
            print("  " + ", ".join(
                f"ch{ch} {a.packets} pkts" for ch, a in analyzers.items()))

    return {"events": {EVENT_NAMES.get(code, str(code)): count
                       for code, count in sorted(events.items())},
            "identities": {str(ch): {"device_number": ident[0],
                                     "device_type": ident[1]}
                           for ch, ident in identities.items()}}


def report(result: dict) -> None:
    for channel in result["channels"]:
        print(f"\n=== {channel['profile']} "
              f"(device type 0x{channel['device_type']:02X}, "
              f"{channel['period_ticks']} ticks) ===")
        for check in channel["checks"]:
            mark = "OK  " if check["pass"] else "FAIL"
            print(f"  {mark} {check['name']:22} {check['detail']}")

        if channel["pages"]:
            print("  pages: " + ", ".join(
                f"{page} x{count}" for page, count in channel["pages"].items()))
        if channel["accumulator_wraps"]:
            # A run that never wrapped has not tested the wrap. Say so, rather
            # than letting a clean pass imply coverage it does not have.
            print("  wraps observed: " + ", ".join(
                f"{field} x{count}"
                for field, count in sorted(channel["accumulator_wraps"].items())))
        else:
            print("  wraps observed: none - this run did not reach a 16-bit "
                  "accumulator wrap")

        for label in ("power", "cadence", "speed_mps"):
            stats = channel[label]
            if not stats:
                continue
            line = (f"  {label}: mean {stats['mean']:.2f} "
                    f"(min {stats['min']:.2f}, max {stats['max']:.2f}, "
                    f"n={stats['n']})")
            if "mean_abs_error" in stats:
                line += f", mean abs error {stats['mean_abs_error']:.2f}"
            print(line)

        for violation in channel["violations"]["first"]:
            print(f"  ! {violation}")
        extra = channel["violations"]["count"] - len(channel["violations"]["first"])
        if extra > 0:
            print(f"  ! ... and {extra} more")

    if result.get("events"):
        print("\nchannel events: " + ", ".join(
            f"{name} x{count}" for name, count in result["events"].items()))
    for channel, ident in result.get("identities", {}).items():
        print(f"heard on channel {channel}: device #{ident['device_number']}, "
              f"type 0x{ident['device_type']:02X}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile", action="append", choices=sorted(ap.PROFILES),
        help="profile to verify; repeat for several channels at once "
             "(default: power)",
    )
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="how long to listen (default: 60)")
    parser.add_argument("--expect-watts", type=float,
                        help="power the transmitter was told to produce")
    parser.add_argument("--expect-rpm", type=float,
                        help="cadence the transmitter was told to produce")
    parser.add_argument("--max-error", type=float,
                        help="mean absolute error allowed against those "
                             "expectations (default: 10 %% of the expected "
                             "value, at least 5)")
    parser.add_argument("--max-loss", type=float, default=DEFAULT_MAX_LOSS_PCT,
                        help=f"packet loss allowed, in percent "
                             f"(default: {DEFAULT_MAX_LOSS_PCT})")
    parser.add_argument("--jitter-factor", type=float,
                        default=DEFAULT_JITTER_FACTOR,
                        help="interval stddev allowed, as a fraction of one "
                             f"channel period (default: {DEFAULT_JITTER_FACTOR})")
    parser.add_argument("--wheel-circ", type=float, default=2.105,
                        help="wheel circumference in metres, for speed "
                             "(default: 2.105)")
    parser.add_argument("--device-number", type=int, default=0,
                        help="device number to pair with; 0 is a wildcard, "
                             "which is what a fitness app uses (default: 0)")
    parser.add_argument("--trans-type", type=int, default=0,
                        help="transmission type to pair with; 0 is a wildcard "
                             "(default: 0)")
    parser.add_argument("--channel", type=int, default=0,
                        help="first ANT channel to use (default: 0)")
    parser.add_argument("--record", metavar="FILE",
                        help="write every payload heard to a capture file, "
                             "for --replay and for the replay tests in "
                             "zephyr_aerosense")
    parser.add_argument("--replay", metavar="FILE",
                        help="analyse a capture file instead of opening a "
                             "radio; needs no hardware")
    parser.add_argument("--json", metavar="FILE", nargs="?", const="-",
                        help="write results as JSON (to stdout if no file)")
    parser.add_argument("--serial",
                        help="match a device whose serial ends with this")
    parser.add_argument(
        "--port",
        help="talk to a UART build over this serial port (e.g. COM8, "
             "/dev/ttyACM1) instead of over USB",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    profiles = args.profile or ["power"]
    json_to_stdout = args.json == "-"
    verbose = not args.quiet and not json_to_stdout

    # Everything up to the result is narration, and with --json - the caller
    # wants a parseable document on stdout and nothing else. Redirecting rather
    # than guarding each print catches the bring-up chatter from ant_scan's
    # command() too, which this file does not own.
    stack = contextlib.ExitStack()
    if json_to_stdout:
        stack.enter_context(contextlib.redirect_stdout(sys.stderr))

    def make_analyzer(name: str) -> ChannelAnalyzer:
        spec = ap.PROFILES[name]
        return ChannelAnalyzer(name, spec["device_type"], spec["period"],
                               args.expect_watts, args.expect_rpm,
                               args.wheel_circ)

    extra: dict = {}
    records: list = []

    if args.replay:
        # One analyser per sensor, discovered from the capture. Grouping by
        # device type alone would merge a standard-page power meter and a
        # torque-page one into a single stream and then report the merge as
        # loss, so the device number is part of the key.
        streams: dict = {}
        for t, device_type, device_number, payload in ap.read_capture(args.replay):
            streams.setdefault((device_type, device_number), []).append(
                (t, payload))

        ordered = []
        for (device_type, device_number), packets in sorted(streams.items()):
            pages = Counter(payload[0] for _, payload in packets)
            name = profile_for(device_type, pages)
            analyzer = ChannelAnalyzer(
                f"{name} #{device_number}", device_type,
                period_for(device_type, name), args.expect_watts,
                args.expect_rpm, args.wheel_circ)
            for t, payload in packets:
                analyzer.feed(t, payload)
            ordered.append(analyzer)

        if verbose:
            print(f"replayed {args.replay}: {len(ordered)} sensor stream(s)")
    else:
        dev = open_device(verbose, serial=args.serial, port=args.port,
                          baud=args.baud)
        reader = FrameReader(dev, timeout_ms=250)

        print("\nOpening ANT+ receive channels")
        if not reset_stack(dev, reader):
            print("  FAIL: no startup message after reset")
            return 1

        # Without ENABLE_CHANNEL_ID the broadcasts arrive anonymous, so a run
        # cannot say which sensor it measured. Worth having, not worth aborting
        # over - the measurements do not depend on it.
        command(dev, reader, MESG_ANTLIB_CONFIG_ID, bytes([0x00, 0x80]),
                "extended messages")
        if not command(dev, reader, MESG_NETWORK_KEY_ID,
                       bytes([ap.ANT_PLUS_NETWORK_NUM]) + ANT_PLUS_KEY,
                       "ANT+ network key"):
            return 1

        analyzers = {}
        for index, name in enumerate(profiles):
            channel = args.channel + index
            spec = ap.PROFILES[name]
            if not open_slave(dev, reader, channel, spec["device_type"],
                              spec["period"],
                              args.device_number + index if args.device_number
                              else 0,
                              args.trans_type):
                print(f"  FAIL: channel {channel} ({name}) did not open")
                return 1
            print(f"  OK: channel {channel} - {spec['label']}")
            analyzers[channel] = make_analyzer(name)

        print(f"\nListening for {args.seconds:.0f} s")
        extra = listen(dev, reader, analyzers, args.seconds, verbose, records)

        print("\nClosing")
        for channel in analyzers:
            command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([channel]),
                    f"close ch{channel}")
            if not wait_for_close(reader, channel):
                print(f"  ! channel {channel} never reported closed")
            command(dev, reader, MESG_UNASSIGN_CHANNEL_ID, bytes([channel]),
                    f"unassign ch{channel}")
        close_device(dev)
        ordered = list(analyzers.values())

    if args.record and not args.replay:
        identities = extra.get("identities", {})
        resolved = [
            (t, device_type,
             identities.get(str(channel), {}).get("device_number", 0), payload)
            for t, device_type, channel, payload in records
        ]
        ap.write_capture(args.record, resolved, comments=[
            f"ant_verify.py, {args.seconds:.0f} s, profiles: "
            f"{', '.join(profiles)}",
            f"expected {args.expect_watts} W, {args.expect_rpm} rpm",
        ])
        print(f"\nwrote {args.record} ({len(records)} packets)")

    result = {
        "channels": [a.summary(args.max_loss, args.jitter_factor,
                               args.max_error) for a in ordered],
    }
    result.update(extra)
    result["pass"] = all(c["pass"] for c in result["channels"])

    stack.close()

    if json_to_stdout:
        json.dump(result, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0 if result["pass"] else 1

    report(result)
    if args.json:
        with open(args.json, "w") as handle:
            json.dump(result, handle, indent=2)
        print(f"\nwrote {args.json}")

    print("\nPASS" if result["pass"] else "\nFAILED")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
