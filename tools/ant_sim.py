#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Turn a board running this firmware into a realistic ANT+ sensor.

The dongle is a transparent bridge, so it has no profile logic of its own - but
it will happily be an ANT+ *master* if a host asks it to, and nothing in the
firmware needs changing for that. This is that host. Point it at a spare board
and the dongle under test hears a power meter or a speed-and-cadence sensor
that does not exist.

    python tools/ant_sim.py --profile power --watts 100 --cadence 80 --seed 1

That unblocks the common case of having a Nordic DK and no ANT+ sensors, and it
doubles as the known-truth transmitter for tools/ant_verify.py: the verifier is
deliberately told nothing about this script, so an identical pass against the
sim firmware in sim/ is evidence the firmware matches this reference.

**Two boards are required.** One transmits, one receives; a single board cannot
hear itself. Use --serial (USB) or --port (UART builds) to say which is which.

Pacing comes from EVENT_TX, not from a wall clock. The stack raises that event
each time it has put a payload on the air, which is exactly the moment the next
one should be loaded, and it means this script makes no assumption about how
fast the host or the USB path is. A wall-clock fallback exists for the case
where the event is filtered out, and it says so loudly - a silent fallback
would hide the very failure worth knowing about.
"""

from __future__ import annotations

import argparse
import math
import random
import sys
import time
from pathlib import Path

try:
    import usb.core
    import usb.util
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_identity as ai  # noqa: E402
import ant_pages as ap  # noqa: E402
from ant_probe import (  # noqa: E402
    EP_OUT,
    FrameReader,
    MESG_RESPONSE_EVENT_ID,
    frame,
    close_device,
    open_device,
    reset_stack,
)
from ant_scan import (  # noqa: E402
    ANT_PLUS_KEY,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_BROADCAST_DATA_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    command,
)
from ant_session import (  # noqa: E402
    EVENT_MARKER,
    MESG_UNASSIGN_CHANNEL_ID,
    wait_for_close,
)

CHANNEL_TYPE_MASTER = 0x10
EVENT_TX = 3

# 32768 counts a second is the ANT timebase; a channel period is expressed in
# those counts and 8182 of them is the ~4.005 Hz the power profile transmits at.
ANT_TICKS_PER_S = 32768.0

# Identity the simulated sensors report in their common pages. 0xFF is the
# "development" manufacturer id, which is what an unregistered device is
# supposed to use rather than borrowing somebody else's.
MANUFACTURER_ID = 0x00FF
MODEL_NUMBER = 0x0001
HW_REVISION = 1
SW_REVISION = 1
SERIAL_NUMBER = 0x00544553      # "TES" - visible in a capture without a decoder

DEFAULT_WHEEL_CIRC_M = 2.105
DEFAULT_SPEED_KPH = 30.0


class Signal:
    """A target value with bounded noise and an optional slow sway.

    A constant 100 W is not a useful test signal: it makes every delta identical
    and hides an accumulator that is being recomputed from the target rather
    than accumulated. The sway makes the trace look like a rider, and the seed
    makes it replayable - a measurement harness that cannot be replayed is not
    a measurement harness.
    """

    def __init__(self, target: float, noise: float, rng: random.Random,
                 sway: float = 0.0, sway_period_s: float = 45.0):
        self.target = target
        self.noise = noise
        self.rng = rng
        self.sway = sway
        self.sway_period_s = sway_period_s

    def at(self, t: float) -> float:
        value = self.target
        if self.sway:
            value += self.sway * math.sin(2.0 * math.pi * t / self.sway_period_s)
        if self.noise:
            value += self.rng.uniform(-self.noise, self.noise)
        return max(0.0, value)


class Sensor:
    """One simulated ANT+ master channel.

    Subclasses supply the device type, the message period and the data page.
    Everything shared - the simulated clock, the common-page rotation, the
    channel bring-up - lives here so a new profile is a page builder and
    nothing else.
    """

    device_type = 0
    period = 0
    label = "sensor"

    # What page 81 reports. None emits the "not supplied" sentinel
    # (0xFFFFFFFF), which is the whole of the page 81 privacy rule - see
    # docs/radiant-security.md 5.4. It is an instance attribute so that
    # --privacy-pages can set it without touching a subclass constructor;
    # every profile below takes the same five positional arguments and adding
    # a sixth to all of them for one flag would be a worse trade.
    serial_number: int | None = SERIAL_NUMBER

    def __init__(self, channel: int, device_number: int, trans_type: int,
                 watts: Signal, rpm: Signal):
        self.channel = channel
        self.device_number = device_number
        self.trans_type = trans_type
        self.watts = watts
        self.rpm = rpm
        self.t = 0.0
        self.messages = 0
        self.data_pages_since_common = 0
        self.pending_common: list[bytes] = []

    @property
    def period_s(self) -> float:
        return self.period / ANT_TICKS_PER_S

    def bring_up(self, dev, reader) -> bool:
        """Assign, configure and open the channel as a master.

        Message order matters: a channel must be assigned before it has an id,
        and every one of these is refused with CHANNEL_IN_WRONG_STATE if the
        channel is already open. A master needs no search timeout - it is not
        searching for anything.
        """
        steps = [
            (MESG_ASSIGN_CHANNEL_ID,
             bytes([self.channel, CHANNEL_TYPE_MASTER, ap.ANT_PLUS_NETWORK_NUM]),
             "assign master"),
            (MESG_CHANNEL_ID_ID,
             bytes([self.channel,
                    self.device_number & 0xFF, (self.device_number >> 8) & 0xFF,
                    self.device_type, self.trans_type]),
             "channel id"),
            (MESG_CHANNEL_RADIO_FREQ_ID,
             bytes([self.channel, ap.ANT_PLUS_RF_FREQ]), "radio frequency"),
            (MESG_CHANNEL_MESG_PERIOD_ID,
             bytes([self.channel, self.period & 0xFF, self.period >> 8]),
             "message period"),
        ]
        for msg_id, payload, what in steps:
            if not command(dev, reader, msg_id, payload,
                           f"ch{self.channel} {what}"):
                return False

        # The first payload goes in before the channel opens. A master starts
        # transmitting the instant it is opened, and whatever is in the buffer
        # at that moment goes on the air - eight zero bytes, if nothing was
        # loaded, which a receiver decodes as page 0x00.
        dev.write(EP_OUT, frame(MESG_BROADCAST_DATA_ID,
                                bytes([self.channel]) + self.next_payload()))

        return command(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([self.channel]),
                       f"ch{self.channel} open")

    def next_payload(self) -> bytes:
        """Advance the simulated sensor by one message period and build a page."""
        self.t += self.period_s
        self.messages += 1
        self.advance(self.period_s)

        if self.pending_common:
            return self.pending_common.pop(0)

        if self.data_pages_since_common >= ap.COMMON_PAGE_INTERVAL:
            # The profiles require both common pages at least once per 65
            # messages. Sending 80 and 81 back to back keeps the spacing well
            # inside that even when a data page is dropped.
            self.data_pages_since_common = 0
            self.pending_common = [
                ap.encode_common_81(SW_REVISION, self.serial_number),
            ]
            return ap.encode_common_80(HW_REVISION, MANUFACTURER_ID,
                                       MODEL_NUMBER)

        self.data_pages_since_common += 1
        return self.data_page()

    def advance(self, dt: float) -> None:
        """Move the simulated physics forward. Overridden where there is any."""

    def data_page(self) -> bytes:
        raise NotImplementedError


class RevolutionCounter:
    """Counts revolutions of something spinning at a variable rate.

    The torque and speed-and-cadence pages report per-revolution events, not
    per-message ones. At 80 rpm and a ~4 Hz channel only about one message in
    three carries a new event, and that is precisely the case that exercises a
    receiver's delta arithmetic - so revolutions are counted properly rather
    than approximated one per message.
    """

    def __init__(self):
        self.phase = 0.0
        self.revs = 0

    def advance(self, dt: float, rev_per_s: float) -> list[float]:
        """Advance by dt. Returns the duration of each revolution completed."""
        completed = []
        if rev_per_s <= 0.0:
            return completed
        self.phase += dt * rev_per_s
        rev_period = 1.0 / rev_per_s
        while self.phase >= 1.0:
            self.phase -= 1.0
            self.revs += 1
            completed.append(rev_period)
        return completed


class StandardPowerSensor(Sensor):
    """Bicycle power, page 0x10. The page every power meter emits."""

    device_type = ap.BPWR_DEVICE_TYPE
    period = ap.BPWR_PERIOD
    label = "power (page 0x10)"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.event_count = 0
        self.acc_power = 0
        self.inst_power = 0
        self.cadence = 0

    def advance(self, dt: float) -> None:
        self.inst_power = int(round(self.watts.at(self.t)))
        self.cadence = int(round(self.rpm.at(self.t)))
        # One power event per message for this page, and the accumulator takes
        # the instantaneous value each time - so a receiver can recover the
        # same average two ways and the two must agree.
        self.event_count = (self.event_count + 1) % ap.U8_WRAP
        self.acc_power = (self.acc_power + self.inst_power) % ap.U16_WRAP

    def data_page(self) -> bytes:
        return ap.encode_power_std(self.event_count, self.cadence,
                                   self.acc_power, self.inst_power)


class TorquePowerSensor(Sensor):
    """Bicycle power, pages 0x11 and 0x12, alternating.

    Two *independent* accumulator series on one channel: the wheel page counts
    wheel revolutions and the crank page counts crank revolutions, and at
    30 km/h on a 2.105 m wheel the wheel turns about three times per crank
    revolution. A receiver that keeps one baseline for both pages differences
    one series against the other and reports nonsense - which is exactly the
    aerosense defect this profile exists to reproduce.
    """

    device_type = ap.BPWR_DEVICE_TYPE
    period = ap.BPWR_PERIOD
    label = "power (pages 0x11 and 0x12)"

    def __init__(self, *args, wheel_circ_m: float = DEFAULT_WHEEL_CIRC_M,
                 speed_kph: float = DEFAULT_SPEED_KPH, **kwargs):
        super().__init__(*args, **kwargs)
        self.wheel_circ_m = wheel_circ_m
        self.wheel_rev_per_s = (speed_kph / 3.6) / wheel_circ_m
        self.crank = RevolutionCounter()
        self.wheel = RevolutionCounter()
        self.series = {
            ap.PAGE_POWER_WHEEL_TORQUE: dict(event_count=0, ticks=0,
                                             acc_period=0, acc_torque=0),
            ap.PAGE_POWER_CRANK_TORQUE: dict(event_count=0, ticks=0,
                                             acc_period=0, acc_torque=0),
        }
        self.cadence = 0
        self.next_page = ap.PAGE_POWER_WHEEL_TORQUE

    def _accumulate(self, page: int, rev_periods: list[float],
                    rev_per_s: float) -> None:
        state = self.series[page]
        watts = self.watts.at(self.t)
        torque_nm = watts / (2.0 * math.pi * rev_per_s) if rev_per_s > 0 else 0.0
        for rev_period in rev_periods:
            state["event_count"] = (state["event_count"] + 1) % ap.U8_WRAP
            state["ticks"] = (state["ticks"] + 1) % ap.U8_WRAP
            state["acc_period"] = (state["acc_period"]
                                   + int(round(2048.0 * rev_period))) % ap.U16_WRAP
            state["acc_torque"] = (state["acc_torque"]
                                   + int(round(32.0 * torque_nm))) % ap.U16_WRAP

    def advance(self, dt: float) -> None:
        rpm = self.rpm.at(self.t)
        self.cadence = int(round(rpm))
        crank_rev_per_s = rpm / 60.0

        self._accumulate(ap.PAGE_POWER_CRANK_TORQUE,
                         self.crank.advance(dt, crank_rev_per_s),
                         crank_rev_per_s)
        self._accumulate(ap.PAGE_POWER_WHEEL_TORQUE,
                         self.wheel.advance(dt, self.wheel_rev_per_s),
                         self.wheel_rev_per_s)

    def data_page(self) -> bytes:
        page = self.next_page
        self.next_page = (ap.PAGE_POWER_CRANK_TORQUE
                          if page == ap.PAGE_POWER_WHEEL_TORQUE
                          else ap.PAGE_POWER_WHEEL_TORQUE)
        state = self.series[page]
        return ap.encode_power_torque(page, state["event_count"],
                                      state["ticks"], self.cadence,
                                      state["acc_period"],
                                      state["acc_torque"])


class TorqueFrequencySensor(Sensor):
    """Bicycle power, page 0x20 only. Some meters emit nothing else.

    Big-endian fields and a slope that a receiver must divide by, which makes
    it the page most likely to be silently wrong. aerosense ignores it today;
    having a transmitter for it is what turns that into a testable gap.
    """

    device_type = ap.BPWR_DEVICE_TYPE
    period = ap.BPWR_PERIOD
    label = "power (page 0x20, torque frequency)"

    SLOPE_TENTH_NM_HZ = 100      # 10.0 Nm/Hz

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.crank = RevolutionCounter()
        self.event_count = 0
        self.time_stamp = 0
        self.torque_ticks = 0

    def advance(self, dt: float) -> None:
        rpm = self.rpm.at(self.t)
        rev_per_s = rpm / 60.0
        watts = self.watts.at(self.t)
        torque_nm = watts / (2.0 * math.pi * rev_per_s) if rev_per_s > 0 else 0.0
        hz = torque_nm * (self.SLOPE_TENTH_NM_HZ / 10.0)

        for rev_period in self.crank.advance(dt, rev_per_s):
            self.event_count = (self.event_count + 1) % ap.U8_WRAP
            self.time_stamp = (self.time_stamp
                               + int(round(2000.0 * rev_period))) % ap.U16_WRAP
            self.torque_ticks = (self.torque_ticks
                                 + int(round(hz * rev_period))) % ap.U16_WRAP

    def data_page(self) -> bytes:
        return ap.encode_power_torque_freq(self.event_count,
                                           self.SLOPE_TENTH_NM_HZ,
                                           self.time_stamp, self.torque_ticks)


class CombinedSpeedCadenceSensor(Sensor):
    """Combined speed and cadence, device type 0x79.

    The page carries no page number and no common pages are defined for it, so
    the base class's common-page rotation is switched off: a receiver keyed on
    the device type would decode a 0x50 as a cadence event time of 0xFF50.
    """

    device_type = ap.BSC_COMBINED_DEVICE_TYPE
    period = ap.BSC_COMBINED_PERIOD
    label = "combined speed and cadence (0x79)"

    def __init__(self, *args, wheel_circ_m: float = DEFAULT_WHEEL_CIRC_M,
                 speed_kph: float = DEFAULT_SPEED_KPH, **kwargs):
        super().__init__(*args, **kwargs)
        self.wheel_circ_m = wheel_circ_m
        self.wheel_rev_per_s = (speed_kph / 3.6) / wheel_circ_m
        self.crank = RevolutionCounter()
        self.wheel = RevolutionCounter()
        self.cad_event_time = 0
        self.spd_event_time = 0

    def advance(self, dt: float) -> None:
        rpm = self.rpm.at(self.t)
        for rev_period in self.crank.advance(dt, rpm / 60.0):
            self.cad_event_time = (self.cad_event_time
                                   + int(round(1024.0 * rev_period))) % ap.U16_WRAP
        for rev_period in self.wheel.advance(dt, self.wheel_rev_per_s):
            self.spd_event_time = (self.spd_event_time
                                   + int(round(1024.0 * rev_period))) % ap.U16_WRAP

    def next_payload(self) -> bytes:
        self.t += self.period_s
        self.messages += 1
        self.advance(self.period_s)
        return self.data_page()

    def data_page(self) -> bytes:
        return ap.encode_bsc_combined(self.cad_event_time,
                                      self.crank.revs % ap.U16_WRAP,
                                      self.spd_event_time,
                                      self.wheel.revs % ap.U16_WRAP)


SENSORS = {
    "power": StandardPowerSensor,
    "power-torque": TorquePowerSensor,
    "power-torque-freq": TorqueFrequencySensor,
    "csc": CombinedSpeedCadenceSensor,
}


def run(dev, reader, sensors: list[Sensor], seconds: float,
        verbose: bool) -> dict:
    """Feed every open channel from its EVENT_TX, for `seconds`.

    One receive loop drives all of them. Sensors run at different periods, so
    a loop per sensor would need a thread per sensor and a lock on the device;
    the events already carry the channel number, so they do not.
    """
    stats = {s.channel: dict(sent=0, events=0, fallback=0) for s in sensors}
    # Fall back two periods after the last event, not one: one period is the
    # nominal spacing, so a fallback armed at exactly that fires on ordinary
    # jitter and doubles the message rate on a channel that is working.
    deadlines = {s.channel: time.monotonic() + 2.0 * s.period_s
                 for s in sensors}
    by_channel = {s.channel: s for s in sensors}
    warned = set()

    end = time.monotonic() + seconds
    last_report = time.monotonic()

    def send(sensor: Sensor) -> None:
        dev.write(EP_OUT, frame(MESG_BROADCAST_DATA_ID,
                                bytes([sensor.channel]) + sensor.next_payload()))
        stats[sensor.channel]["sent"] += 1
        deadlines[sensor.channel] = time.monotonic() + 2.0 * sensor.period_s

    while time.monotonic() < end:
        result = reader.next_frame(min(end, time.monotonic() + 0.25))
        if result is not None:
            msg_id, body = result
            if (msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3
                    and body[1] == EVENT_MARKER and body[2] == EVENT_TX
                    and body[0] in by_channel):
                stats[body[0]]["events"] += 1
                send(by_channel[body[0]])

        now = time.monotonic()
        for channel, sensor in by_channel.items():
            if now < deadlines[channel]:
                continue
            if channel not in warned:
                warned.add(channel)
                print(f"  ! channel {channel}: no EVENT_TX within two message "
                      f"periods - falling back to wall-clock pacing.")
                print(f"    That is a real finding: either the event filter is "
                      f"suppressing EVENT_TX (0x6E)\n"
                      f"    or the channel is not transmitting. Pacing from "
                      f"here on is a guess.")
            stats[channel]["fallback"] += 1
            send(sensor)

        if verbose and time.monotonic() - last_report >= 5.0:
            last_report = time.monotonic()
            summary = ", ".join(
                f"ch{s.channel} {stats[s.channel]['sent']} msgs"
                for s in sensors)
            print(f"  {summary}")

    return stats


def dry_run(sensors: list[Sensor], seconds: float) -> list:
    """Run the sensors with no radio, returning capture records.

    Every other check in here costs two boards and a flash. This one costs
    nothing and still catches the mistakes that matter most - a page built
    wrong, an accumulator that resets instead of wrapping, a common page that
    never appears - because ant_verify.py's analysis runs over these records
    unchanged. It is also how a Phase 3 replay capture gets made when the bench
    is not to hand.
    """
    records = []
    for sensor in sensors:
        count = int(seconds / sensor.period_s)
        for _ in range(count):
            # sensor.t is advanced by next_payload, so read it afterwards and
            # the timestamp is the moment the payload went out rather than the
            # moment before.
            payload = sensor.next_payload()
            records.append((sensor.t, sensor.device_type,
                            sensor.device_number, payload))
    records.sort(key=lambda record: record[0])
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile", action="append", choices=sorted(SENSORS),
        help="sensor to simulate; repeat for several at once on separate "
             "channels (default: power)",
    )
    parser.add_argument("--watts", type=float, default=100.0,
                        help="target power (default: 100)")
    parser.add_argument("--cadence", type=float, default=80.0,
                        help="target cadence in rpm (default: 80)")
    parser.add_argument("--noise", type=float, default=3.0,
                        help="+/- noise band on both signals (default: 3)")
    parser.add_argument("--sway", type=float, default=0.0,
                        help="amplitude of a slow sinusoid on top of the "
                             "target, so the trace looks like a rider rather "
                             "than a constant (default: 0)")
    parser.add_argument("--speed", type=float, default=DEFAULT_SPEED_KPH,
                        help="road speed in km/h, for the wheel-based pages "
                             f"(default: {DEFAULT_SPEED_KPH:.0f})")
    parser.add_argument("--wheel-circ", type=float,
                        default=DEFAULT_WHEEL_CIRC_M,
                        help="wheel circumference in metres "
                             f"(default: {DEFAULT_WHEEL_CIRC_M})")
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="how long to transmit (default: 60)")
    parser.add_argument("--device-number", type=int, default=0x3A17,
                        help="ANT device number of the first sensor; further "
                             "profiles take the next numbers up")
    parser.add_argument("--identity-file", type=Path,
                        help="provision the device number from this record "
                             "instead of --device-number, applying the tier's "
                             "power-up rule. See tools/ant_identity.py; the "
                             "record is created on first use (Tier 0)")
    parser.add_argument("--identity-tier", type=int, choices=ai.TIERS,
                        default=0,
                        help="; ".join("%d = %s" % (tier, ai.TIER_DESCRIPTIONS[tier])
                                       for tier in ai.TIERS)
                             + ". Only used when the record is being created")
    parser.add_argument("--privacy-pages", action="store_true",
                        help="emit page 81 with the not-supplied serial "
                             "sentinel and a generic page 80. A 32-bit serial "
                             "in the clear is strictly more identifying than "
                             "the device number and defeats a re-roll "
                             "outright, so this is what a node with any "
                             "privacy posture must do")
    parser.add_argument("--trans-type", type=int, default=5,
                        help="transmission type (default: 5, matching the "
                             "aerosense master path and sim/)")
    parser.add_argument("--channel", type=int, default=0,
                        help="first ANT channel to use (default: 0)")
    parser.add_argument("--seed", type=int, default=1,
                        help="seed for the noise, so a run can be replayed "
                             "(default: 1)")
    parser.add_argument("--serial",
                        help="match a device whose serial ends with this")
    parser.add_argument(
        "--port",
        help="talk to a UART build over this serial port (e.g. COM8, "
             "/dev/ttyACM1) instead of over USB",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--dry-run", action="store_true",
        help="build the payload stream without a board and write it out; "
             "feed the result to ant_verify.py --replay",
    )
    parser.add_argument("--record", metavar="FILE",
                        help="write the transmitted payloads to a capture "
                             "file (required by --dry-run)")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    profiles = args.profile or ["power"]
    verbose = not args.quiet

    # Identity provisioning. Without --identity-file this is exactly what it
    # always was: a fixed number from the command line, so every existing
    # invocation and every recorded capture is unchanged.
    #
    # With it, the number comes from a provisioning record and the tier's
    # power-up rule is applied on the way in - which for Tier 2 means a
    # different number every run, and is the point. Nothing about this is a
    # protocol feature: a device number is chosen by the host or by the node
    # application, and the tiers are a rule about how it is chosen.
    base_device_number = args.device_number
    serial_number = None if args.privacy_pages else SERIAL_NUMBER
    if args.identity_file:
        identity = ai.provision(args.identity_file, tier=args.identity_tier,
                                privacy_pages=args.privacy_pages)
        identity.on_boot()
        ai.save(args.identity_file, identity)
        base_device_number = identity.device_number
        if identity.privacy_pages:
            serial_number = None
        if verbose:
            print("identity: device #%d (base #%d), tier %d - %s"
                  % (identity.device_number, identity.base_device_number,
                     identity.tier, ai.TIER_DESCRIPTIONS[identity.tier]))

    def build_sensors() -> list[Sensor]:
        built = []
        for index, name in enumerate(profiles):
            rng = random.Random(args.seed + index)
            kwargs = {}
            if name in ("power-torque", "csc"):
                kwargs = dict(wheel_circ_m=args.wheel_circ,
                              speed_kph=args.speed)
            sensor = SENSORS[name](
                args.channel + index, base_device_number + index,
                args.trans_type,
                Signal(args.watts, args.noise, rng, args.sway),
                Signal(args.cadence, args.noise, rng, args.sway),
                **kwargs)
            sensor.serial_number = serial_number
            built.append(sensor)
        return built

    if args.dry_run:
        if not args.record:
            sys.exit("--dry-run needs --record FILE to write the stream to")
        sensors = build_sensors()
        records = dry_run(sensors, args.seconds)
        ap.write_capture(args.record, records, comments=[
            f"ant_sim.py --dry-run, seed {args.seed}",
            f"target {args.watts:.1f} W, {args.cadence:.1f} rpm, "
            f"noise +/-{args.noise:.1f}, sway {args.sway:.1f}",
            f"profiles: {', '.join(profiles)}",
        ])
        for sensor in sensors:
            print(f"  channel {sensor.channel}: {sensor.messages} messages - "
                  f"{sensor.label}")
        print(f"\nwrote {args.record} ({len(records)} packets, no radio used)")
        return 0

    dev = open_device(verbose, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)

    print("\nOpening ANT+ master channels")
    # Same reason every other tool in here opens with a reset: a channel left
    # assigned by the previous run refuses to be assigned again.
    if not reset_stack(dev, reader):
        print("  FAIL: no startup message after reset")
        return 1

    if not command(dev, reader, MESG_NETWORK_KEY_ID,
                   bytes([ap.ANT_PLUS_NETWORK_NUM]) + ANT_PLUS_KEY,
                   "ANT+ network key"):
        return 1

    sensors: list[Sensor] = []
    for sensor in build_sensors():
        if not sensor.bring_up(dev, reader):
            print(f"  FAIL: channel {sensor.channel} ({sensor.label}) "
                  "did not open")
            return 1
        print(f"  OK: channel {sensor.channel} - {sensor.label}, "
              f"device #{sensor.device_number} type 0x{sensor.device_type:02X} "
              f"trans {sensor.trans_type}, {sensor.period} ticks "
              f"({1.0 / sensor.period_s:.2f} Hz)")
        sensors.append(sensor)

    print(f"\nTransmitting for {args.seconds:.0f} s "
          f"({args.watts:.0f} W, {args.cadence:.0f} rpm, "
          f"+/-{args.noise:.0f}, seed {args.seed})")
    stats = run(dev, reader, sensors, args.seconds, verbose)

    print("\nClosing")
    for sensor in sensors:
        command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([sensor.channel]),
                f"close ch{sensor.channel}")
        # A close is asynchronous. Unassigning before EVENT_CHANNEL_CLOSED
        # arrives is refused with CHANNEL_IN_WRONG_STATE, and then the next run
        # of this script cannot assign the channel either.
        if not wait_for_close(reader, sensor.channel):
            print(f"  ! channel {sensor.channel} never reported closed")
        command(dev, reader, MESG_UNASSIGN_CHANNEL_ID, bytes([sensor.channel]),
                f"unassign ch{sensor.channel}")

    close_device(dev)

    print()
    fell_back = False
    for sensor in sensors:
        st = stats[sensor.channel]
        expected = args.seconds / sensor.period_s
        print(f"  channel {sensor.channel}: {st['sent']} messages "
              f"({expected:.0f} expected), {st['events']} EVENT_TX")
        if st["fallback"]:
            fell_back = True
            print(f"    {st['fallback']} sent on the wall-clock fallback")

    if fell_back:
        print("\nFAILED: EVENT_TX pacing did not hold. Check the event filter "
              "(0x6E) on this\nboard - the payloads went out, but their timing "
              "was guessed rather than paced.")
        return 1

    print("\nPASS: every message was paced by EVENT_TX")
    return 0


if __name__ == "__main__":
    sys.exit(main())
