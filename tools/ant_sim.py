#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Turn a board running this firmware into a realistic ANT+ sensor.

The dongle is a transparent bridge with no profile logic of its own, but it
will happily be an ANT+ *master* if a host asks - nothing in the firmware
needs changing. This is that host: point it at a spare board and the dongle
under test hears a power meter or speed-and-cadence sensor that does not
exist.

    python tools/ant_sim.py --profile power --watts 100 --cadence 80 --seed 1

That covers having a Nordic DK and no ANT+ sensors, and it doubles as the
known-truth transmitter for tools/ant_verify.py: the verifier is told nothing
about this script, so an identical pass against the sim firmware in sim/ is
evidence the firmware matches this reference.

**Two boards are required.** One transmits, one receives; a single board
cannot hear itself. Use --serial (USB) or --port (UART builds) to say which is
which.

Pacing comes from EVENT_TX, not a wall clock - the stack raises it exactly
when the next payload should be loaded, so this makes no assumption about host
or USB speed. A wall-clock fallback exists for when the event is filtered out,
and it says so loudly rather than silently hiding that failure.
"""

from __future__ import annotations

import argparse
import math
import random
import sys
import time
from pathlib import Path

try:
    import usb.core  # noqa: F401 - import-guard, verifies pyusb is installed
    import usb.util  # noqa: F401
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_identity as ai  # noqa: E402
import ant_pages as ap  # noqa: E402
import radiant_crypto as rc  # noqa: E402
from ant_probe import (  # noqa: E402
    EP_OUT,
    MESG_RESPONSE_EVENT_ID,
    FrameReader,
    close_device,
    frame,
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
DEFAULT_BPM = 145.0

# The compat layer's demonstration key. NOT A SECRET AND NOT MEANT TO BE: it is
# the root a --replay fixture and a bench receiver both need in order to check
# an attestation tag, and every tool that ships one ships this one.
DEFAULT_COMPAT_ROOT = bytes(range(16))


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

    # The RF index this master transmits on, as an offset from 2400 MHz -
    # the ANT+ default (57, 2457 MHz) unless a caller moves it.
    #
    # Exists for RF-7's gate (is the ~0.4 % loss floor really Wi-Fi collision
    # on 2457 MHz): both ends must move together, since ant_verify.py's
    # --rf-freq on the receiving side alone would just move the receiver away
    # from the transmitter and read as spectacular loss rather than a
    # misconfiguration.
    rf_freq: int = ap.ANT_PLUS_RF_FREQ

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
        # The RadiANT compat layer, or None for a plain ANT+ sensor - which is
        # what a shipped strap is by default and what every profile here still
        # is unless --attest is given. Set after construction for the same
        # reason serial_number is: every profile takes the same five positional
        # arguments and this must not become a sixth.
        self.compat: CompatAttestation | None = None

    # A clock that is not the one it advertises, in ppm. `period` is the
    # advertised slot spacing; this offsets the TRUE spacing, modelling an
    # RC-oscillator node (250-500 ppm) against the +/-50 ppm every receive
    # window here was sized from - at a 2 s heartbeat, 300 ppm is 600 us of
    # error against a 400 us window, a lost node with no error code unless it
    # announces its accuracy.
    #
    # Only bites in --dry-run: on the live path the dongle's own stack times
    # the slots and this script only refills on EVENT_TX, so nothing here can
    # skew them. The ztest against the mock radio covers the deterministic
    # case; this needs a master with a genuinely bad oscillator.
    period_ppm: int = 0

    @property
    def period_s(self) -> float:
        return self.period * (1.0 + self.period_ppm / 1e6) / ANT_TICKS_PER_S

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
             bytes([self.channel, self.rf_freq]), "radio frequency"),
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

        payload = self._choose_payload()
        if self.compat is not None:
            self.compat.transmitted(payload)
        return payload

    def _choose_payload(self) -> bytes:
        """Which page gets this slot.

        The order is a priority, and the first two entries are the ones that
        cannot move. Tier II must land on the N-th transmitted message exactly
        or its window is not the window it claims to be; a queued common page
        must go out before anything else queues behind it.
        """
        if self.compat is not None and self.compat.tier2_due():
            return self.compat.tier2_page()

        if self.pending_common:
            return self.pending_common.pop(0)

        if self.data_pages_since_common >= ap.COMMON_PAGE_INTERVAL:
            # The profiles require both common pages at least once per 65
            # messages. Sending 80 and 81 back to back keeps the spacing well
            # inside that even when a data page is dropped.
            #
            # The beacon joins the back of that burst rather than getting a
            # cadence of its own: one frame per cycle, ~0.8 % of slots, the same
            # rotation every deployed receiver already absorbs for 80 and 81.
            self.data_pages_since_common = 0
            self.pending_common = [
                ap.encode_common_81(SW_REVISION, self.serial_number),
            ]
            if self.compat is not None:
                # Counted as a data page, because it DISPLACES one. Appending it
                # to the burst without charging it a slot would lengthen the
                # cycle instead of riding it, and the first thing that notices
                # is a receiver's common-page gap check - which is the profile
                # requirement this rotation exists to satisfy.
                self.pending_common.append(self.compat.beacon_frame())
                self.data_pages_since_common = 1
            return ap.encode_common_80(HW_REVISION, MANUFACTURER_ID,
                                       MODEL_NUMBER)

        if self.compat is not None and self.compat.tier1_due(self.t):
            # Same rule: a displaced data page, not an extra transmission the
            # profile never budgeted for.
            self.data_pages_since_common += 1
            return self.compat.tier1_page(self.t)

        self.data_pages_since_common += 1
        return self.data_page()

    def advance(self, dt: float) -> None:
        """Move the simulated physics forward. Overridden where there is any."""

    def data_page(self) -> bytes:
        raise NotImplementedError


class CompatAttestation:
    """The RadiANT compat layer riding an ordinary ANT+ profile's slots.

    docs/radiant-security.md section 11. Three things go on the air, none
    touching a page any receiver already understands:

      * the capability beacon, one frame per 121-message cycle, riding the
        existing 80/81 rotation rather than inventing a cadence of its own;
      * Tier I, one self-contained page every `interval_s` seconds - decoupled
        from the data rate, which is the whole compatibility argument (at 4 Hz
        and the 20 s default that's one page in ~81, below ANT+'s own 1.65 %
        common-page cost);
      * Tier II, off unless a window is given, one page in every N transmitted
        messages, covering the N-1 before it.

    Knows no page layout and computes no tag - ant_pages.py packs the fields
    and radiant_crypto.py computes the MACs; this only decides which slot gets
    what.
    """

    def __init__(self, root: bytes, epoch: int, devnum: int,
                 interval_s: float = ap.COMPAT_DEFAULT_TIER_I_INTERVAL_S,
                 window: int | None = None,
                 policy: int = ap.COMPAT_POLICY_NEVER,
                 target_device_number: int = 0,
                 target_period: int = ap.RADIANT_TLM_PERIOD_DEFAULT):
        if interval_s <= 0.0:
            raise ValueError("the Tier I interval is in seconds and positive")
        if window is not None and window not in ap.COMPAT_WINDOW_SIZES:
            raise ValueError(f"N must be one of {ap.COMPAT_WINDOW_SIZES}")
        keys = rc.derive_keys(root, epoch, devnum)
        self.k_auth = keys[rc.LABEL_AUTH]
        self.k_id = keys[rc.LABEL_ID]
        self.epoch = epoch
        self.devnum = devnum
        self.interval_s = interval_s
        self.window = window

        private = policy != ap.COMPAT_POLICY_NEVER
        self.beacon = ap.CompatBeacon(
            attest_available=True,
            private_available=private,
            policy=policy,
            window=window if window is not None else ap.COMPAT_DEFAULT_WINDOW,
            key_group_hint=rc.compat_key_group_hint(self.k_id, epoch),
            target_device_type=ap.RADIANT_TLM_DEVICE_TYPE if private else 0,
            target_device_number=target_device_number if private else 0,
            target_period=target_period if private else 0,
        )
        self.beacon_frames = ap.encode_compat_beacon(self.beacon)
        self.beacon_next = 0

        # The N-1 transmitted messages the next Tier II tag will cover. The
        # window is N CONSECUTIVE TRANSMITTED MESSAGES, so the common pages, the
        # beacon and any Tier I page in the window are covered too - which is
        # what keeps the tag profile-agnostic.
        self.pending: list[bytes] = []
        self.window_index = 0
        self.att_counter = -1

        self.beacons_sent = 0
        self.tier1_sent = 0
        self.tier2_sent = 0

    def beacon_frame(self) -> bytes:
        """The next beacon frame. The two alternate, one per cycle."""
        frame_bytes = self.beacon_frames[self.beacon_next]
        self.beacon_next = (self.beacon_next + 1) % len(self.beacon_frames)
        self.beacons_sent += 1
        return frame_bytes

    def tier2_due(self) -> bool:
        return self.window is not None and len(self.pending) == self.window - 1

    def tier2_page(self) -> bytes:
        tag = rc.compat_tier2_tag(self.k_auth, self.epoch, self.devnum,
                                  self.window_index, self.pending)
        page = ap.encode_compat_attest_tier2(self.window_index, tag)
        self.pending = []
        self.window_index = (self.window_index + 1) % ap.U16_WRAP
        self.tier2_sent += 1
        return page

    def tier1_due(self, t: float) -> bool:
        return int(t / self.interval_s) > self.att_counter

    def tier1_page(self, t: float) -> bytes:
        # DERIVED FROM TIME, NOT FROM A SEND COUNT. A receiver reconstructs the
        # same counter from its own clock, so a gap in the stream costs it
        # nothing and a replayed page is rejected on a counter already seen.
        self.att_counter = int(t / self.interval_s)
        tag = rc.compat_tier1_tag(self.k_auth, self.epoch, self.devnum,
                                  self.att_counter & 0xFFFF)
        self.tier1_sent += 1
        return ap.encode_compat_attest_tier1(self.att_counter & 0xFFFF, tag)

    def transmitted(self, payload: bytes) -> None:
        """Record one message as covered by the window under construction."""
        if self.window is None:
            return
        if (payload[0] & ap.HR_PAGE_NUMBER_MASK) == ap.COMPAT_PAGE_ATTEST_TIER_II:
            # The tag page is the N-th message of its own window, not the first
            # of the next one.
            return
        self.pending.append(bytes(payload))


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


class HeartRateSensor(Sensor):
    """Heart rate, device type 0x78. The other compat target.

    Two things here that no other profile in this file has:

      * A PAGE-CHANGE TOGGLE IN BYTE 0's HIGH BIT, flipped every four messages.
        It is not part of the page number, which is why page numbers on this
        device type are 7-bit and why the compat pages had to fit under 0x7F.
      * A MAIN PAGE AND A BACKGROUND ROTATION. Page 0x04 carries the data every
        message; pages 0x00 to 0x03 take one slot each in turn. Bytes [4..7] are
        the same on all of them, so the rotation costs a receiver nothing.
    """

    device_type = ap.HRM_DEVICE_TYPE
    period = ap.HRM_PERIOD
    label = "heart rate"

    TOGGLE_INTERVAL = 4
    BACKGROUND_INTERVAL = 64
    BACKGROUND_PAGES = (ap.PAGE_HR_DEFAULT, ap.PAGE_HR_CUMULATIVE_TIME,
                        ap.PAGE_HR_MANUFACTURER, ap.PAGE_HR_PRODUCT)

    def __init__(self, *args, bpm: float = DEFAULT_BPM, **kwargs):
        super().__init__(*args, **kwargs)
        # The heart rate reuses the cadence signal's noise and generator so that
        # --seed still makes the whole run reproducible; a second rng would make
        # one seeded stream depend on how many others were built first.
        self.bpm = Signal(bpm, self.rpm.noise, self.rpm.rng,
                          sway=self.rpm.sway)
        self.beat = RevolutionCounter()
        self.beats = 0
        self.event_time = 0
        self.previous_event_time = 0
        self.computed_hr = 0
        self.background_next = 0
        self.data_pages = 0

    def advance(self, dt: float) -> None:
        bpm = max(0.0, self.bpm.at(self.t))
        for beat_period in self.beat.advance(dt, bpm / 60.0):
            self.previous_event_time = self.event_time
            self.event_time = (self.event_time
                               + int(round(ap.HR_EVENT_TIME_HZ * beat_period))
                               ) % ap.U16_WRAP
            self.beats = (self.beats + 1) % ap.U8_WRAP
        self.computed_hr = int(round(bpm))

    @property
    def toggle(self) -> bool:
        return bool((self.messages // self.TOGGLE_INTERVAL) & 1)

    def data_page(self) -> bytes:
        self.data_pages += 1
        tail = dict(event_time=self.event_time, beat_count=self.beats,
                    computed_hr=self.computed_hr or None, toggle=self.toggle)

        if self.data_pages % self.BACKGROUND_INTERVAL:
            return ap.encode_hr_previous_beat(self.previous_event_time, **tail)

        page = self.BACKGROUND_PAGES[self.background_next]
        self.background_next = ((self.background_next + 1)
                                % len(self.BACKGROUND_PAGES))
        if page == ap.PAGE_HR_DEFAULT:
            return ap.encode_hr_default(**tail)
        if page == ap.PAGE_HR_CUMULATIVE_TIME:
            return ap.encode_hr_cumulative_time(
                int(self.t / ap.HR_OPERATING_TIME_UNIT_S), **tail)
        if page == ap.PAGE_HR_MANUFACTURER:
            return ap.encode_hr_manufacturer(MANUFACTURER_ID & 0xFF,
                                             (self.serial_number or 0) & 0xFFFF,
                                             **tail)
        return ap.encode_hr_product(HW_REVISION, SW_REVISION,
                                    MODEL_NUMBER & 0xFF, **tail)


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


# ---------------------------------------------------------------------------
# The RadiANT telemetry envelope, device type 0x60
# ---------------------------------------------------------------------------


class TelemetrySensor(Sensor):
    """A RadiANT generic telemetry node, device type 0x60.

    Four fields over two data pages. tools/ant_verify.py is told nothing about
    this class - it recovers the schema from the descriptor the node
    broadcasts and decodes data pages against that, which is the envelope's
    whole claim, exercised end to end with no board.

    The field set is not arbitrary: section 5's "anything integrable is
    published as an accumulating field" means power gets a cumulative-energy
    accumulator with instantaneous power alongside as a convenience - the same
    shape as ANT+ page 0x10, giving a receiver an accumulator to check the
    instantaneous value against.
    """

    device_type = ap.RADIANT_TLM_DEVICE_TYPE
    period = ap.RADIANT_TLM_PERIOD_DEFAULT
    label = "RadiANT generic telemetry (0x60)"

    SCHEMA_ID = 0x2B
    FIELD_HEART_RATE = 1
    FIELD_TEMPERATURE = 2
    FIELD_ENERGY = 3
    FIELD_POWER = 4

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.descriptor = ap.TlmDescriptor(
            schema_id=self.SCHEMA_ID,
            period=self.period,
            fields=[
                ap.TlmField(id=self.FIELD_HEART_RATE, type=0x26, page=1,
                            bit_offset=0, width_code=4),
                ap.TlmField(id=self.FIELD_TEMPERATURE, type=0x10, page=1,
                            bit_offset=8, width_code=7, exponent=-2),
                ap.TlmField(id=self.FIELD_ENERGY, type=0x30, page=2,
                            bit_offset=0, width_code=10, accumulate=True),
                ap.TlmField(id=self.FIELD_POWER, type=0x1C, page=2,
                            bit_offset=32, width_code=6, signed=True),
            ],
        )
        self.energy_j = 0.0
        self.inst_power = 0
        self.heart_rate = 0
        self.temperature_ck = 29315   # 293.15 K at exp -2
        self._sched = None

    # The scheduler is built lazily because --privacy-pages is applied to the
    # instance after construction, and it changes what the common-page
    # builders emit.
    @property
    def sched(self) -> ap.TlmPageScheduler:
        if self._sched is None:
            self._sched = ap.TlmPageScheduler(self.descriptor, {
                "data_page": self._data_page,
                "common_80": self._common_80,
                "common_81": self._common_81,
            })
        return self._sched

    def _data_page(self, page: int, counter: int) -> bytes:
        return ap.encode_tlm_data(self.descriptor, page, counter, {
            self.FIELD_HEART_RATE: self.heart_rate,
            self.FIELD_TEMPERATURE: self.temperature_ck,
            self.FIELD_ENERGY: int(self.energy_j) % (1 << 32),
            self.FIELD_POWER: self.inst_power,
        })

    def _common_80(self) -> bytes:
        return ap.encode_common_80(HW_REVISION, MANUFACTURER_ID, MODEL_NUMBER)

    def _common_81(self) -> bytes:
        return ap.encode_common_81(SW_REVISION, self.serial_number)

    def advance(self, dt: float) -> None:
        watts = self.watts.at(self.t)
        self.inst_power = int(round(watts))
        self.heart_rate = min(255, int(round(self.rpm.at(self.t))))
        # Energy is the integral of power, accumulated in joules and emitted
        # in 32 bits, where it is MEANT to wrap.
        self.energy_j += watts * dt
        # A slow thermal drift, so the temperature field is not a constant -
        # a constant hides an encoder that recomputes rather than accumulates.
        self.temperature_ck = 29315 + int(round(50.0 * math.sin(self.t / 120.0)))

    def next_payload(self):
        self.t += self.period_s
        self.messages += 1
        self.advance(self.period_s)
        kind, body = self.sched.next()
        return body


class AssetTagSensor(Sensor):
    """The envelope with everything turned off: a sparse node with no fields.

    Free to encode, and the cheapest exercise of the sparse path: a heartbeat
    carrying the whole descriptor set, silence in between, page 82 configured
    then suppressed.

    The privacy rule is the point: a stable 16-bit device number plus page
    81's 32-bit serial every 30 s is a tracking beacon, and page 82's
    operating-time counter is monotone - it survives an identity change and
    fingerprints a battery swap. For a tag, whose entire payload is an
    identity, that matters more than for a strap, so the mitigation is serial
    0xFFFFFFFF and a suppressed page 82.

    Page 82 is emitted only when --privacy-pages is off, so a capture can show
    both sides of the rule.
    """

    device_type = ap.RADIANT_TLM_DEVICE_TYPE
    period = ap.RADIANT_TLM_PERIOD_DEFAULT
    label = "RadiANT sparse asset tag (0x60, no fields)"

    HEARTBEAT_S = 30

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.descriptor = ap.TlmDescriptor(
            schema_id=0x01,
            period=self.period,
            fields=[],
            flags=ap.TLM_FLAG_SPARSE,
            heartbeat_s=self.HEARTBEAT_S,
            k_code=ap.TLM_K_CODE_3,
        )
        self.operating_time_16s = 0
        self._sched = None

    @property
    def suppress_page_82(self) -> bool:
        # serial_number is None exactly when --privacy-pages is on, and the
        # two halves of the section 6 rule travel together by construction
        # rather than by a second flag somebody can forget.
        return self.serial_number is None

    @property
    def sched(self) -> ap.TlmPageScheduler:
        if self._sched is None:
            builders = {}
            if not self.suppress_page_82:
                builders["common_82"] = self._common_82
            self._sched = ap.TlmPageScheduler(self.descriptor, builders)
        return self._sched

    def _common_82(self) -> bytes:
        return ap.encode_common_82(fractional_voltage=0x80, coarse_voltage=3,
                                   status=3,
                                   operating_time=self.operating_time_16s,
                                   time_resolution_16s=True)

    def advance(self, dt: float) -> None:
        self.operating_time_16s = int(self.t / 16.0)

    def next_payload(self):
        """Returns None for a slot the node declines to transmit in.

        That is sparse mode, and it is not a simulator shortcut: "the node
        keeps a channel period configured and simply declines to transmit in
        most of its slots". Callers must handle None - see dry_run() and
        send() below, and the note there about what a stock ANT master does
        with a slot nobody loaded.
        """
        self.t += self.period_s
        self.messages += 1
        self.advance(self.period_s)
        kind, body = self.sched.next()
        return body


class TreadmillSensor(Sensor):
    """Fitness Equipment Control, device type 0x11, as a TREADMILL.

    The receiving-side counterpart of apps/treadmill's FE-C master, and the
    instrument for two things a bench needs and cannot otherwise get: a second
    FE-C source to test a dongle against without a second DK, and N synthetic
    treadmills for the binding-table scaling measurement that
    docs/treadmill-reference-design.md 8 says has to happen before anybody
    promises a gym anything.

    IT IGNORES --watts AND --cadence, AND THAT IS DELIBERATE. Every profile in
    this file takes the same five positional arguments and the base class's own
    comment says a sixth must not be added, so a treadmill's belt speed has
    nowhere to arrive from. It runs its own workout instead - the same 150 s
    profile apps/treadmill's simulator runs, so a capture from this and a
    capture from the firmware are the same shape of stream.

    THE INTERLEAVE HERE IS THE BASE CLASS'S, NOT SS10.1's. Sensor._choose_payload()
    sends the common pages every 121 messages; FE-C wants them every 66, as two
    consecutive background pages. That difference is real and it is left alone:
    this is a stimulus generator for a receiver, and a receiver does not care.
    The firmware's own interleave is the one that has to be right, and
    radiant/tests/src/test_profile_fec_tx.c is what measures it.
    """

    device_type = ap.FEC_DEVICE_TYPE
    period = ap.FEC_PERIOD
    label = "fitness equipment (treadmill), device type 0x11"

    # The same five-message group apps/treadmill uses: two page 16, two page
    # 19, one page 17. The 80/81 pair rides the base class's own cadence.
    ROTATION = (ap.PAGE_FEC_GENERAL, ap.PAGE_FEC_GENERAL,
                ap.PAGE_FEC_TREADMILL, ap.PAGE_FEC_TREADMILL,
                ap.PAGE_FEC_SETTINGS)

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.speed_mm_s = 0.0
        self.incline_centi = 0
        self.distance_mm = 0.0
        self.elapsed_ms = 0.0
        self.strides = 0.0
        self.pos_vertical_mm = 0.0
        self.neg_vertical_mm = 0.0
        self.state = ap.FEC_STATE_READY
        self.slot = 0

    def advance(self, dt: float) -> None:
        cycle = self.t % 150.0
        if cycle < 10.0:
            want, state = 0.0, ap.FEC_STATE_READY
        elif cycle < 40.0:
            want, state = 1400.0, ap.FEC_STATE_IN_USE
        elif cycle < 100.0:
            want, state = 3000.0, ap.FEC_STATE_IN_USE
        elif cycle < 130.0:
            want, state = 1200.0, ap.FEC_STATE_IN_USE
        else:
            want, state = 0.0, ap.FEC_STATE_FINISHED

        # Ramp at 300 mm/s per second, as a real belt does. A treadmill that
        # jumped from 0 to 3 m/s would throw its rider off, and a receiver
        # tested only against step changes has never seen an intermediate
        # value.
        step = 300.0 * dt
        self.speed_mm_s += max(-step, min(step, want - self.speed_mm_s))
        self.state = state

        if state == ap.FEC_STATE_IN_USE:
            self.elapsed_ms += dt * 1000.0
        tick_mm = self.speed_mm_s * dt
        self.distance_mm += tick_mm
        # Grade is a percent in HUNDREDTHS, so the divisor is 10000 and not
        # 100 - the same trap treadmill_state.c carries the comment for.
        vertical = tick_mm * self.incline_centi / 10000.0
        if vertical >= 0:
            self.pos_vertical_mm += vertical
        else:
            self.neg_vertical_mm -= vertical
        # Strides, not steps. Roughly 62.6 + 6.1 * v strides/min.
        if self.speed_mm_s >= 500.0:
            self.strides += (62.6 + 6.1 * self.speed_mm_s / 1000.0) * dt / 60.0

    def data_page(self) -> bytes:
        page = self.ROTATION[self.slot % len(self.ROTATION)]
        self.slot += 1

        if page == ap.PAGE_FEC_GENERAL:
            return ap.encode_fec_general(
                ap.FEC_TYPE_TREADMILL,
                elapsed_time_qs=int(self.elapsed_ms / 250.0) & 0xFF,
                distance_m=int(self.distance_mm / 1000.0) & 0xFF,
                speed_mm_s=int(self.speed_mm_s),
                heart_rate=None,
                state=self.state)
        if page == ap.PAGE_FEC_TREADMILL:
            cadence = None
            if self.speed_mm_s >= 500.0:
                cadence = int(round(62.6 + 6.1 * self.speed_mm_s / 1000.0))
            return ap.encode_fec_treadmill(
                cadence,
                neg_vertical_dm=int(self.neg_vertical_mm / 100.0) & 0xFF,
                pos_vertical_dm=int(self.pos_vertical_mm / 100.0) & 0xFF,
                state=self.state,
                capabilities=(ap.FEC_TREADMILL_CAP_NEG_VERTICAL |
                              ap.FEC_TREADMILL_CAP_POS_VERTICAL))
        stride_cm = 0
        if self.strides >= 1.0:
            stride_cm = min(255, int(self.distance_mm / self.strides / 10.0))
        return ap.encode_fec_settings(stride_cm or None, self.incline_centi,
                                      None, self.state)


class StrideSensor(Sensor):
    """Stride Based Speed and Distance, device type 0x7C.

    The other half of apps/treadmill's ANT+ surface, and the one a watch or
    Zwift Run pairs with. Same workout as TreadmillSensor above and the same
    reason for ignoring --watts and --cadence.

    THE PERIOD IS 8134 AND IT IS NOT A TYPO. It is close enough to heart
    rate's 8070 and FE-C's 8192 to read as one in a diff, and a receiver told
    either of those never opens the channel at all - with nothing on either
    side naming the period as the reason.

    AND THERE IS NO 0xFF SENTINEL ANYWHERE ON THIS PROFILE. An unused field is
    zero and validity is out of band in page 22, which is the reverse of every
    other profile in this file.
    """

    device_type = ap.SDM_DEVICE_TYPE
    period = ap.SDM_PERIOD
    label = "stride-based speed and distance, device type 0x7C"

    # 1, 1, X, X - the profile's own interleave, with X alternating between
    # page 2 and page 3 group by group.
    GROUP = 4

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.speed_mm_s = 0.0
        self.distance_mm = 0.0
        self.elapsed_ms = 0.0
        self.strides = 0.0
        self.calories = 0.0
        self.slot = 0
        self.group = 0

    def advance(self, dt: float) -> None:
        cycle = self.t % 150.0
        if cycle < 10.0:
            want = 0.0
        elif cycle < 40.0:
            want = 1400.0
        elif cycle < 100.0:
            want = 3000.0
        elif cycle < 130.0:
            want = 1200.0
        else:
            want = 0.0

        step = 300.0 * dt
        self.speed_mm_s += max(-step, min(step, want - self.speed_mm_s))
        self.elapsed_ms += dt * 1000.0
        self.distance_mm += self.speed_mm_s * dt
        if self.speed_mm_s >= 500.0:
            self.strides += (62.6 + 6.1 * self.speed_mm_s / 1000.0) * dt / 60.0
        # ~13 kcal/min while running, the same order the firmware's ACSM
        # estimate produces. A stimulus generator does not need the equation.
        if self.speed_mm_s > 0:
            self.calories += 13.0 * dt / 60.0

    def _cadence_16(self) -> int:
        if self.speed_mm_s < 500.0:
            return 0
        return int(round((62.6 + 6.1 * self.speed_mm_s / 1000.0) * 16))

    def data_page(self) -> bytes:
        position = self.slot % self.GROUP
        self.slot += 1
        if position == self.GROUP - 1:
            self.group += 1

        speed_int, speed_frac = ap.sdm_speed_split(
            min(int(self.speed_mm_s), ap.SDM_SPEED_MAX_MM_S))

        if position < 2:
            return ap.encode_sdm_default(
                time_frac_200=int((self.elapsed_ms % 1000.0) *
                                  ap.SDM_TIME_FRAC_DEN / 1000.0),
                time_s=int(self.elapsed_ms / 1000.0) & 0xFF,
                distance_m=int(self.distance_mm / 1000.0) & 0xFF,
                distance_frac_16=int((self.distance_mm % 1000.0) *
                                     ap.SDM_DIST_FRAC_DEN / 1000.0),
                speed_int_mps=speed_int, speed_frac_256=speed_frac,
                strides=int(self.strides) & 0xFF,
                # 1/32 s of staleness, which is the firmware's node tick.
                latency_32=3)

        cadence_16 = self._cadence_16()
        page = (ap.PAGE_SDM_CALORIES if (self.group % 2)
                else ap.PAGE_SDM_BASE)
        return ap.encode_sdm_supplementary(
            page,
            cadence_strides_min=cadence_16 // 16,
            cadence_frac_16=cadence_16 % 16,
            speed_int_mps=speed_int, speed_frac_256=speed_frac,
            status=ap.sdm_status(ap.SDM_LOC_OTHER, ap.SDM_BATTERY_NEW,
                                 ap.SDM_HEALTH_OK,
                                 ap.SDM_USE_ACTIVE if self.speed_mm_s > 0
                                 else ap.SDM_USE_INACTIVE),
            calories=int(self.calories) & 0xFF)


SENSORS = {
    "power": StandardPowerSensor,
    "power-torque": TorquePowerSensor,
    "power-torque-freq": TorqueFrequencySensor,
    "csc": CombinedSpeedCadenceSensor,
    "heart-rate": HeartRateSensor,
    "telemetry": TelemetrySensor,
    "asset-tag": AssetTagSensor,
    # apps/treadmill's two masters, as stimulus for a receiver. Two --profile
    # arguments run both at once, which is the mix nothing in this project has
    # measured - 8192 and 8134 counts beat against each other with a period of
    # about 32 s.
    "fec-treadmill": TreadmillSensor,
    "sdm": StrideSensor,
}

# Which profiles --attest can be applied to. Device type 0x79 is absent
# permanently and structurally: its page has no page-number byte, so an inserted
# page decodes as speed and cadence and steps four accumulators. The 0x60
# profiles are absent because they are already RadiANT natives - X_CONF and
# X_AUTH secure them, and attestation is the mechanism for the channels that
# cannot have those.
ATTESTABLE = ("power", "power-torque", "power-torque-freq", "heart-rate")


def run(dev, reader, sensors: list[Sensor], seconds: float,
        verbose: bool) -> dict:
    """Feed every open channel from its EVENT_TX, for `seconds`.

    One receive loop drives all of them. Sensors run at different periods, so
    a loop per sensor would need a thread per sensor and a lock on the device;
    the events already carry the channel number, so they do not.
    """
    stats = {s.channel: dict(sent=0, events=0, fallback=0, skipped=0)
             for s in sensors}
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
        payload = sensor.next_payload()
        deadlines[sensor.channel] = time.monotonic() + 2.0 * sensor.period_s
        if payload is None:
            # A sparse node declining this slot. Nothing is written, so the
            # stack retransmits whatever is already in its buffer - which is
            # what a sparse node running on a stock ANT master ACTUALLY does,
            # and is worth knowing rather than papering over: skipping a slot
            # for real needs a master that can be told not to transmit.
            # --dry-run has no such constraint and is exact.
            stats[sensor.channel]["skipped"] += 1
            return
        dev.write(EP_OUT, frame(MESG_BROADCAST_DATA_ID,
                                bytes([sensor.channel]) + payload))
        stats[sensor.channel]["sent"] += 1

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
                print("    That is a real finding: either the event filter is "
                      "suppressing EVENT_TX (0x6E)\n"
                      "    or the channel is not transmitting. Pacing from "
                      "here on is a guess.")
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
            if payload is None:
                # A sparse node declining this slot. The slot still happened -
                # the timer ran, the counter advanced - and nothing went on
                # the air, which is exactly what the capture should show.
                continue
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
                        help="; ".join(f"{tier} = {ai.TIER_DESCRIPTIONS[tier]}"
                                       for tier in ai.TIERS)
                             + ". Only used when the record is being created")
    parser.add_argument("--privacy-pages", action="store_true",
                        help="emit page 81 with the not-supplied serial "
                             "sentinel and a generic page 80. A 32-bit serial "
                             "in the clear is strictly more identifying than "
                             "the device number and defeats a re-roll "
                             "outright, so this is what a node with any "
                             "privacy posture must do")
    parser.add_argument("--attest", action="store_true",
                        help="add the RadiANT compat layer to an ANT+ profile: "
                             "a capability beacon in the common-page rotation "
                             "and Tier I identity attestation. The data pages "
                             "stay byte-exact ANT+ and a legacy receiver skips "
                             "the two added page numbers, which is the whole "
                             "mechanism. Off by default, because a shipped "
                             "strap is a plain ANT+ sensor unless somebody "
                             "configures it otherwise")
    parser.add_argument("--attest-interval", type=float, metavar="T",
                        default=ap.COMPAT_DEFAULT_TIER_I_INTERVAL_S,
                        help="Tier I interval in SECONDS, decoupled from the "
                             "data rate - which is what makes the cost 1.2 %% "
                             "of slots at 4 Hz rather than a fraction of the "
                             f"stream (default: "
                             f"{ap.COMPAT_DEFAULT_TIER_I_INTERVAL_S:.0f})")
    parser.add_argument("--attest-window", type=int, metavar="N",
                        choices=ap.COMPAT_WINDOW_SIZES, default=None,
                        help="also emit Tier II data attestation, one page in "
                             "N transmitted messages. Off by default: N is "
                             "simultaneously the airtime cost, the verification "
                             "latency and the DoS amplification factor, and one "
                             "lost packet unverifies the whole window")
    parser.add_argument("--private-policy", type=int,
                        choices=ap.COMPAT_POLICIES,
                        default=ap.COMPAT_POLICY_NEVER,
                        help="what the beacon advertises the node WILL do: "
                             "0 never, 1 physical, 2 command, 3 always "
                             "(default: 0). The locator fields are zero on a "
                             "`never` node because there is nowhere for it to "
                             "go")
    parser.add_argument("--compat-key", metavar="HEX",
                        default=DEFAULT_COMPAT_ROOT.hex(),
                        help="16-byte root key as hex, for the attestation "
                             "tags. The default is a published demonstration "
                             "key and is not a secret")
    parser.add_argument("--epoch", type=int, default=1,
                        help="epoch the keys and nonces are derived under "
                             "(default: 1). It is never broadcast: for a "
                             "hostless node the epoch is the boot counter, and "
                             "a receiver recovers it by searching forward "
                             "against the beacon's key-group hint")
    parser.add_argument("--bpm", type=float, default=DEFAULT_BPM,
                        help=f"heart rate to simulate, in beats per minute "
                             f"(default: {DEFAULT_BPM:.0f})")
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
        "--period-ppm", type=int, default=0, metavar="PPM",
        help="offset the master's TRUE slot spacing by this many ppm while it "
             "keeps advertising the nominal period - an RC-clocked node, which "
             "is 250-500 ppm against the +/-50 ppm every window here was sized "
             "from. Takes effect in --dry-run only; see the note on "
             "Sensor.period_ppm",
    )
    parser.add_argument(
        "--clock-accuracy", type=int, default=None, metavar="CODE",
        help="announce a clock-accuracy ceiling in a 0x60 node's descriptor: "
             "0=500ppm (an RC oscillator), 5=50ppm, 6=30ppm (a 32 kHz "
             "crystal), 7=20ppm. Omit and the node announces nothing, which is "
             "what every node built before the schedule block does",
    )
    parser.add_argument(
        "--rf-freq", type=int, default=None, metavar="N",
        help="transmit on RF index N (2400+N MHz) instead of the ANT+ default "
             "57. The counterpart of ant_verify.py's --rf-freq, and both ends "
             "must be given the same value or the run measures an empty "
             "channel. For RF-7's quiet-channel gate the candidates are 2, 26 "
             "and 80, which sit in the gaps between Wi-Fi 1, 6 and 11",
    )
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

    # Identity provisioning. Without --identity-file, a fixed number from the
    # command line (existing invocations and captures unchanged). With it, the
    # number comes from a provisioning record and the tier's power-up rule is
    # applied - for Tier 2 that means a different number every run, by design.
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
            print(f"identity: device #{identity.device_number} "
                  f"(base #{identity.base_device_number}), tier "
                  f"{identity.tier} - "
                  f"{ai.TIER_DESCRIPTIONS[identity.tier]}")

    def build_sensors() -> list[Sensor]:
        built = []
        for index, name in enumerate(profiles):
            rng = random.Random(args.seed + index)
            kwargs = {}
            if name in ("power-torque", "csc"):
                kwargs = dict(wheel_circ_m=args.wheel_circ,
                              speed_kph=args.speed)
            elif name == "heart-rate":
                kwargs = dict(bpm=args.bpm)
            sensor = SENSORS[name](
                args.channel + index, base_device_number + index,
                args.trans_type,
                Signal(args.watts, args.noise, rng, args.sway),
                Signal(args.cadence, args.noise, rng, args.sway),
                **kwargs)
            sensor.serial_number = serial_number
            sensor.period_ppm = args.period_ppm
            if args.rf_freq is not None:
                sensor.rf_freq = args.rf_freq
            if args.attest:
                if name not in ATTESTABLE:
                    sys.exit(f"--attest is for an ANT+ compat profile; {name} "
                             f"is not one of {', '.join(ATTESTABLE)}")
                sensor.compat = CompatAttestation(
                    bytes.fromhex(args.compat_key), args.epoch,
                    sensor.device_number,
                    interval_s=args.attest_interval,
                    window=args.attest_window,
                    policy=args.private_policy,
                    target_device_number=sensor.device_number ^ 0xA5A5)
            # Set on the descriptor rather than passed to a constructor, for
            # the same reason serial_number is: the `sched` property is lazy
            # precisely so that post-construction settings reach the encoded
            # set. A node that announces nothing is left alone entirely.
            if args.clock_accuracy is not None:
                descriptor = getattr(sensor, "descriptor", None)
                if descriptor is None:
                    sys.exit(f"--clock-accuracy needs a device type with a "
                             f"descriptor; {name} has none")
                descriptor.clock_stated = True
                descriptor.clock_accuracy = args.clock_accuracy
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
