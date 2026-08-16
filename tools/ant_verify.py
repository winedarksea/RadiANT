#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Measure an ANT+ sensor: loss, jitter, decoded accuracy, accumulator sanity.

ant_scan.py answers "did anything arrive". This answers "was it right", which
is the question a closed loop needs. It opens a slave channel, decodes what it
hears, and reports numbers rather than adjectives:

    python tools/ant_verify.py --profile power --expect-watts 100 \
        --expect-rpm 80 --seconds 60 --json

Several channels at once, each pinned to its own sensor - which is the shape a
fitness app runs and the only shape in which a multi-channel scheduling fault
is visible at all:

    python tools/ant_verify.py --seconds 300 \
        --channel 0 --device-number 4660 \
        --channel 1 --device-number 4661 \
        --channel 2 --device-number 4662

`derived.per_channel` then carries a `loss_exact_pct` per channel beside the
pooled `derived.loss_exact_pct`, because a scheduler that drops the later of
two nearby receive windows costs one channel of a pair and leaves the other
alone: the pooled figure halves it, and the per-channel spread shows it.

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
import math
import statistics
import sys
import time
from collections import Counter

try:
    import usb.core  # noqa: F401 - import-guard, verifies pyusb is installed
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402
import radiant_crypto as rc  # noqa: E402
from ant_probe import (  # noqa: E402
    FrameReader,
    close_device,
    open_device,
    reset_stack,
)
from ant_scan import (  # noqa: E402
    ANT_PLUS_FREQ,
    ANT_PLUS_KEY,
    command,
)
from ant_session import wait_for_close  # noqa: E402

# Protocol constants come from the generated module, never from a second copy
# here. See tools/ant_wire.py and protocol/ant_wire.yaml.
#
# The extended-field flags used to be bare 0x80/0x40/0x20 literals inside
# extended_fields(), which is the one function here where an unnamed bit is
# genuinely dangerous: the fields are positional, so mistaking one flag for
# another does not fail, it silently reports a device number as a signal
# strength.
from ant_wire import (  # noqa: E402
    CHANNEL_TYPE_SLAVE,
    EVENT_CODES_BY_VALUE,
    EVENT_RX_FAIL,
    EVENT_RX_FAIL_GO_TO_SEARCH,
    EXT_FLAG_CHANNEL_ID,
    EXT_FLAG_RSSI,
    EXT_FLAG_RX_TIMESTAMP,
    LIB_CONFIG_ALL_EXT_FIELDS,
    MESG_ACKNOWLEDGED_DATA_ID,
    MESG_ANTLIB_CONFIG_ID,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_BROADCAST_DATA_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_EVENT_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_UNASSIGN_CHANNEL_ID,
    RSSI_MEASUREMENT_TYPE_DBM,
)

ANT_TICKS_PER_S = 32768.0
TICK_WRAP_S = 65536 / ANT_TICKS_PER_S   # the receive timestamp is 16 bits

# How many messages a sensor may go between common pages.
#
# 121, not the generic ANT+ guidance of 65: sdk-ant's own bicycle power
# profile (lib/ant_profiles/ant_bpwr/ant_bpwr.c) interleaves pages 80/81 every
# 121 messages, and that is Garmin's certified implementation. At 65 this
# check failed a worst gap of 118 against it.
COMMON_PAGE_LIMIT = 121

# Default acceptance thresholds, exposed as arguments since the right numbers
# depend on the transmitter's noise band.
#
# 1.5 was set after a FrameReader read-timeout bug (250 ms against a 249.7 ms
# channel period, cancelling and losing whatever packet it was waiting for)
# was fixed: with that gone, four 300 s bench runs measure 0.26-0.60 % loss,
# every packet of it RX_FAIL-accounted, so 1.5 is about 3x the worst of those
# - room for a busier day, not for a fault (a wrong-sensor pairing reads ~25 %).
# loss_accounting() is the real check now: a threshold alone can't tell air
# loss from a bug, which is why it fails independently when RX_FAIL doesn't
# account for the gap.
DEFAULT_MAX_LOSS_PCT = 1.5
DEFAULT_JITTER_FACTOR = 0.5      # of one channel period, on the stddev


# ── The four things a receiver can say about a compat stream ────────────────
# docs/radiant-security.md section 11.4:
#
#   CLEAR       no key here. Not "unprotected" - a receiver holding no key has
#               no opinion, and CLEAR must keep meaning exactly that.
#   VERIFIED    a tag arrived and checked out.
#   UNVERIFIED  something arrived and did NOT check out (forged tag, wrong
#               key, replayed counter), OR a key is installed, attestation is
#               expected, and nothing came - this is what stops a naive
#               receiver falling back to clear when the beacon is stripped.
#   LOST        a page that never arrived. Not the same as UNVERIFIED -
#               conflating them reports the ~0.4 % loss floor as an attack.
ATTEST_CLEAR = "clear"
ATTEST_VERIFIED = "verified"
ATTEST_UNVERIFIED = "unverified"
ATTEST_LOST = "lost"


class CompatVerifier:
    """The receiver half of docs/radiant-security.md section 11.4.

    Holds a key and answers "is this stream from the holder of it, now". It is
    fed every payload the channel delivers, because Tier II's window is N
    consecutive TRANSMITTED messages rather than N data ones - the common pages,
    the beacon and the Tier I page are all inside it.

    Tier I's loss independence is the claim worth reading the counters for:
    because its tag covers no payload, its verification rate equals its page
    delivery rate no matter what else is lost. Tier II's does not, and the
    numbers below are where that shows up.
    """

    def __init__(self, root: bytes, epoch: int, devnum: int,
                 window: int | None = None):
        keys = rc.derive_keys(root, epoch, devnum)
        self.k_auth = keys[rc.LABEL_AUTH]
        self.k_id = keys[rc.LABEL_ID]
        self.epoch = epoch
        self.devnum = devnum
        self.window = window
        self.expected_hint = rc.compat_key_group_hint(self.k_id, epoch)

        self.tier1 = Counter()
        self.tier2 = Counter()
        self.beacon_frames = 0
        self.beacon_malformed = 0
        self.hint_matches = 0
        self.announce_verified = 0
        self.announce_unverified = 0

        self.last_counter: int | None = None
        self.last_window_index: int | None = None
        self.windows_seen = 0
        self.pending: list[bytes] = []
        self.announce_frame_a: bytes | None = None
        self.beacon: ap.CompatBeacon | None = None
        self._frames: dict = {}

    # -- ingestion --------------------------------------------------------

    def feed(self, payload: bytes) -> str | None:
        """One delivered message. Returns its attestation outcome, or None."""
        page = payload[0] & ap.HR_PAGE_NUMBER_MASK

        if page == ap.COMPAT_PAGE_ATTEST_TIER_II:
            # The tag page is the N-th message of its own window, not the first
            # of the next one.
            return self._feed_tier2(payload)

        # EVERYTHING ELSE IS COVERED BY WHATEVER TIER II WINDOW IS OPEN - the
        # window is N consecutive transmitted messages and not N data ones, so
        # the common pages, the beacon and the Tier I page are all inside it.
        self.pending.append(bytes(payload))
        if page == ap.COMPAT_PAGE_ATTEST_TIER_I:
            return self._feed_tier1(payload)
        if page == ap.COMPAT_PAGE_BEACON:
            return self._feed_beacon(payload)
        return None

    def _feed_beacon(self, payload: bytes) -> str | None:
        self.beacon_frames += 1
        try:
            got = ap.decode_compat_beacon_frame(payload)
        except ValueError:
            self.beacon_malformed += 1
            return ATTEST_UNVERIFIED

        index = got["frame_index"]
        if index == ap.COMPAT_FRAME_ANNOUNCE_A:
            self.announce_frame_a = bytes(payload)
            return None
        if index == ap.COMPAT_FRAME_ANNOUNCE_B:
            return self._feed_announce_b(got["tag"])

        self._frames[index] = bytes(payload)
        # The hint is not a security claim - 24 bits, resolved by the 40- or
        # 48-bit attestation tag - but it is what a receiver with several
        # roots uses to skip the ones that cannot match, and what a receiver
        # with a stale epoch searches forward against.
        if (index == ap.COMPAT_FRAME_BEACON_0
                and got["key_group_hint"] == self.expected_hint):
            self.hint_matches += 1
        if (ap.COMPAT_FRAME_BEACON_0 in self._frames
                and ap.COMPAT_FRAME_BEACON_1 in self._frames):
            try:
                self.beacon = ap.decode_compat_beacon(
                    [self._frames[ap.COMPAT_FRAME_BEACON_0],
                     self._frames[ap.COMPAT_FRAME_BEACON_1]])
            except ValueError:
                # private-available disagreeing with the policy, a reserved bit
                # set, a locator on a `never` node: all malformed, and a
                # receiver says so rather than believing half of it.
                self.beacon_malformed += 1
                return ATTEST_UNVERIFIED
        return None

    def _feed_announce_b(self, tag: bytes) -> str:
        """A SWITCH/RETURN frame is acted on only after its tag verifies."""
        if self.announce_frame_a is None or self.last_counter is None:
            self.announce_unverified += 1
            return ATTEST_UNVERIFIED
        expected = rc.compat_announce_tag(self.k_auth, self.epoch, self.devnum,
                                          self.last_counter,
                                          self.announce_frame_a)
        self.announce_frame_a = None
        if expected == tag:
            self.announce_verified += 1
            return ATTEST_VERIFIED
        self.announce_unverified += 1
        return ATTEST_UNVERIFIED

    def _feed_tier1(self, payload: bytes) -> str:
        got = ap.decode_compat_attest_tier1(payload)
        counter = got["att_counter"]

        if self.last_counter is not None:
            if counter <= self.last_counter:
                # Replay. Closed by the counter rather than by payload coverage,
                # which is what lets the tag cover nothing at all.
                self.tier1["replay"] += 1
                self.tier1[ATTEST_UNVERIFIED] += 1
                return ATTEST_UNVERIFIED
            missing = ap.delta_u16(counter, self.last_counter) - 1
            if missing:
                self.tier1[ATTEST_LOST] += missing
        self.last_counter = counter

        expected = rc.compat_tier1_tag(self.k_auth, self.epoch, self.devnum,
                                       counter)
        if expected == got["tag"]:
            self.tier1[ATTEST_VERIFIED] += 1
            return ATTEST_VERIFIED
        self.tier1[ATTEST_UNVERIFIED] += 1
        return ATTEST_UNVERIFIED

    def _feed_tier2(self, payload: bytes) -> str:
        got = ap.decode_compat_attest_tier2(payload)
        covered, self.pending = self.pending, []

        # The page carries the window index's LOW 8 BITS and the nonce takes 16,
        # so the high half is reconstructed from the receiver's own count of
        # windows rather than read off the air. Skip this and the tags agree for
        # 256 windows - about nine minutes at 4 Hz with N = 8 - and then every
        # one of them fails, which looks exactly like an attack.
        index, _ = rc.resolve_counter(self.windows_seen, got["window_index"])
        if self.last_window_index is not None:
            missing = index - self.last_window_index - 1
            if missing > 0:
                self.tier2[ATTEST_LOST] += missing
        self.last_window_index = index
        self.windows_seen = index + 1

        if self.window is None:
            self.tier2["unconfigured"] += 1
            return ATTEST_UNVERIFIED
        if len(covered) != self.window - 1:
            # NOT a failed tag. The window's own messages did not all arrive, so
            # there is nothing to check it against - which is the honest
            # regression a window CMAC has and Tier I does not.
            self.tier2[ATTEST_LOST] += 1
            return ATTEST_LOST

        expected = rc.compat_tier2_tag(self.k_auth, self.epoch, self.devnum,
                                       index, covered)
        if expected == got["tag"]:
            self.tier2[ATTEST_VERIFIED] += 1
            return ATTEST_VERIFIED
        self.tier2[ATTEST_UNVERIFIED] += 1
        return ATTEST_UNVERIFIED

    # -- results ----------------------------------------------------------

    def tier_verdict(self, counts: Counter, enabled: bool) -> str:
        if not enabled:
            return ATTEST_CLEAR
        if counts[ATTEST_UNVERIFIED]:
            return ATTEST_UNVERIFIED
        if counts[ATTEST_VERIFIED]:
            return ATTEST_VERIFIED
        return ATTEST_UNVERIFIED

    def verdict(self) -> str:
        if (self.tier1[ATTEST_UNVERIFIED] or self.tier2[ATTEST_UNVERIFIED]
                or self.beacon_malformed or self.announce_unverified):
            return ATTEST_UNVERIFIED
        if self.tier1[ATTEST_VERIFIED] or self.tier2[ATTEST_VERIFIED]:
            return ATTEST_VERIFIED
        # A key is installed and attestation was expected. Nothing arrived, so
        # this is a downgrade, not a clear stream.
        return ATTEST_UNVERIFIED

    def summary(self) -> dict:
        delivered = self.tier1[ATTEST_VERIFIED] + self.tier1[ATTEST_UNVERIFIED]
        return {
            "verdict": self.verdict(),
            "epoch": self.epoch,
            "beacon_frames": self.beacon_frames,
            "beacon_malformed": self.beacon_malformed,
            "key_group_hint_matches": self.hint_matches,
            "beacon": None if self.beacon is None else {
                "version": self.beacon.version,
                "attest_available": self.beacon.attest_available,
                "private_available": self.beacon.private_available,
                "policy": self.beacon.policy,
                "window": self.beacon.window,
                "pending_switch": self.beacon.pending_switch,
                "target_device_type": self.beacon.target_device_type,
                "target_device_number": self.beacon.target_device_number,
                "target_period": self.beacon.target_period,
            },
            "tier_i": {
                "verdict": self.tier_verdict(self.tier1, True),
                "verified": self.tier1[ATTEST_VERIFIED],
                "unverified": self.tier1[ATTEST_UNVERIFIED],
                "lost": self.tier1[ATTEST_LOST],
                "replays": self.tier1["replay"],
                # The claim the tier exists for, as a number: of the pages that
                # were DELIVERED, how many verified. Loss belongs in the other
                # column and must not be allowed to move this one.
                "verified_of_delivered": (
                    self.tier1[ATTEST_VERIFIED] / delivered
                    if delivered else None),
            },
            "tier_ii": {
                "verdict": self.tier_verdict(self.tier2,
                                             self.window is not None),
                "window": self.window,
                "verified": self.tier2[ATTEST_VERIFIED],
                "unverified": self.tier2[ATTEST_UNVERIFIED],
                "lost": self.tier2[ATTEST_LOST],
            },
            "announce": {
                "verified": self.announce_verified,
                "unverified": self.announce_unverified,
            },
        }


def reacquire_from_intervals(intervals: list[float],
                             dropout_s: float) -> float | None:
    """Time to re-acquire after a staged dropout: the worst gap, minus the
    dropout itself.

    `intervals` is `ChannelAnalyzer.intervals` - the gaps between consecutive
    delivered packets. A staged --expect-dropout SECONDS closes the channel
    for exactly `dropout_s`, so the widest gap in the run is that dropout plus
    however long the receiver took to notice the channel was open again; every
    other gap is ordinary jitter around one period and is far smaller. Taking
    the max rather than searching for "the" dropout gap is what keeps this
    correct with no knowledge of when the dropout happened - only that it did.

    None with no intervals to look at (a channel that heard at most one
    packet has no gap at all).
    """
    if not intervals:
        return None
    return max(intervals) - dropout_s


class ChannelAnalyzer:
    """Decodes one channel's stream and accumulates evidence about it.

    Live capture and replay both go through here, so a bug found on the bench
    can be reproduced from a saved file and vice versa - which is the whole
    point of --record.
    """

    def __init__(self, name: str, device_type: int, period: int,
                 expect_watts: float | None, expect_rpm: float | None,
                 wheel_circ_m: float, compat: CompatVerifier | None = None,
                 expect_bpm: float | None = None, channel: int | None = None,
                 device_number: int = 0, dropout_s: float | None = None):
        self.name = name
        self.device_type = device_type
        self.period = period

        # Which receive channel this stream was heard on, and which sensor it
        # came from. Neither is used by the analysis - the whole point of
        # profile_for() is that a stream is judged on its own contents - but
        # both are needed to LABEL the per-channel figures derive() emits, and
        # a multi-channel run whose numbers cannot be attributed to a channel
        # is exactly as useless as no per-channel numbers at all.
        #
        # channel is None on --replay unless the caller pairs a --channel with
        # a --device-number: a capture records the sensor, not the receiver
        # channel (see profile_for()'s docstring), so the only honest source
        # for it there is the operator, not the file.
        self.channel = channel
        self.device_number = device_number
        self.period_s = period / ANT_TICKS_PER_S
        self.expect_watts = expect_watts
        self.expect_rpm = expect_rpm
        self.expect_bpm = expect_bpm
        self.wheel_circ_m = wheel_circ_m

        # --expect-dropout SECONDS: a deliberate, timer'd channel close/reopen
        # was staged during the run (see apps/sim's ANT_SIM_DROPOUT_EVERY_S/_S
        # knobs). None on every ordinary run. Set, it does three things: it is
        # subtracted from the naive expected-packet count in summary() so a
        # dropout is not double-counted as loss, it excludes the reopen's
        # still-counter retransmit from the std_event_still disqualifier in
        # _feed_power_std(), and it is the D in reacquire_from_intervals().
        self.dropout_s = dropout_s

        # Set from outside, by main(), after listen() returns: this analyser
        # has no idea when its channel was opened or when listen() started, so
        # it cannot compute this itself. None on --replay (a capture has no
        # channel-open timestamp) and forced None under --expect-dropout (a
        # staged dropout measures re-acquisition, not initial acquisition -
        # see reacquire_s below).
        self.time_to_first_packet_s: float | None = None

        # None means no key, which is the state that must keep reporting CLEAR.
        self.compat = compat
        self.attest_outcomes = Counter()

        # On device type 0x78 byte 0's high bit is the page-change toggle and
        # not part of the page number, so every page would otherwise be
        # histogrammed under two numbers and matched under neither.
        self.page_mask = (ap.HR_PAGE_NUMBER_MASK
                          if device_type == ap.HRM_DEVICE_TYPE else 0xFF)
        self.hr_baseline: dict | None = None
        self.bpm_samples: list[float] = []

        self.packets = 0
        self.first_t: float | None = None
        self.last_t: float | None = None
        self.intervals: list[float] = []

        # The same two quantities as measured by the radio rather than by the
        # host, when the dongle is willing to report them. Empty on --replay
        # and on any dongle that refuses ENABLE_RSSI / ENABLE_RX_TIMESTAMP.
        self.rssi_samples: list[int] = []
        self.radio_intervals: list[float] = []
        self._last_rx_ticks: int | None = None

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

        # Loss measured against the transmitter's own counter instead of a
        # wall clock, since the headline loss figure's elapsed/period
        # denominator is only an estimate (assumes nominal crystal rate, and
        # rounds - noise-level error at typical packet counts).
        #
        # Only valid where the counter steps once per message rather than per
        # pedal stroke: summary() requires the stream to prove the counter
        # never stood still, since a real crank meter's counter stops when the
        # rider coasts.
        #
        # Even two per-message transmitters disagree about what "a message"
        # means: sdk-ant steps the counter only on page 0x10 and leaves it
        # alone for an interleaved common page, while ant_sim.py steps it on
        # every transmission regardless of page. Which convention is in play
        # has to be inferred from the stream - by counting common pages
        # between page 0x10 packets - rather than assumed.
        self.std_event_pairs = 0
        self.std_event_span = 0
        self.std_event_still = 0
        self.std_votes_per_page = 0
        self.std_votes_per_message = 0
        self.std_common_seen = 0
        self._common_since_std = 0

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

        # Device type 0x60, the RadiANT telemetry envelope.
        #
        # This is the one profile here whose SCHEMA IS NOT KNOWN IN ADVANCE.
        # Everything above decodes a layout compiled into ant_pages.py; this
        # one has to learn the layout from the node's own page 0x00 before it
        # can decode a single field, which is the envelope's whole claim and
        # therefore the thing worth verifying rather than assuming. Until the
        # descriptor set completes, data pages are counted and not decoded -
        # deliberately, because guessing a layout is how a receiver reports
        # confident nonsense.
        self.tlm_rx = ap.TlmDescriptorRx()
        self.tlm_desc: ap.TlmDescriptor | None = None
        self.tlm_schema_changes = 0
        self.tlm_data_before_schema = 0
        self.tlm_data_decoded = 0
        # One baseline per data page, for the same reason torque_baseline is:
        # two data pages of one schema are two independent series and
        # differencing one against the other produces plausible garbage.
        self.tlm_baseline: dict[int, dict] = {}

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

    def _offgrid(self, intervals: list[float]) -> float | None:
        """How far packets land from the slot grid, ignoring how many slots.

        The plain stddev of the intervals is not a timing measurement, because
        one lost packet turns a 250 ms interval into a 500 ms one and that
        single outlier dwarfs everything the clock is actually doing: six
        losses in 1200 packets put the stddev at 17 ms all by themselves, which
        is why the jitter figure has always moved in lockstep with the loss
        figure and told nobody anything the loss figure had not.

        Subtracting the nearest whole number of periods removes the count and
        leaves the error. The point of measuring it on both clocks is that the
        difference between them is entirely this host: the radio's own answer
        here is under a tick.
        """
        if len(intervals) < 2 or self.period_s <= 0:
            return None
        return statistics.pstdev(
            d - round(d / self.period_s) * self.period_s for d in intervals)

    def _feed_radio(self, radio: dict | None) -> None:
        """Record what the radio said about a packet, if it said anything.

        The receive timestamp is 16 bits of a 32768 Hz counter, so it rolls
        every two seconds. Nothing here reconstructs the high bits: a masked
        subtraction is right whenever the real gap is under two seconds, and
        the host clock - crude as it is - is easily good enough to say when it
        is not. A gap that long is eight consecutive lost slots, which belongs
        in the loss figure and not in a jitter sample taken across a counter
        that wrapped an unknown number of times.
        """
        if not radio:
            self._last_rx_ticks = None
            return

        if "rssi_dbm" in radio:
            self.rssi_samples.append(radio["rssi_dbm"])

        ticks = radio.get("rx_ticks")
        if ticks is None:
            self._last_rx_ticks = None
            return
        if (self._last_rx_ticks is not None and self.intervals
                and self.intervals[-1] < 0.95 * TICK_WRAP_S):
            self.radio_intervals.append(
                ((ticks - self._last_rx_ticks) & 0xFFFF) / ANT_TICKS_PER_S)
        self._last_rx_ticks = ticks

    def feed(self, t: float, payload: bytes, radio: dict | None = None) -> None:
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

        self._feed_radio(radio)

        if self.device_type == ap.BSC_COMBINED_DEVICE_TYPE:
            # No page number in this page at all, so there is nothing to
            # histogram and no common pages to expect.
            self._feed_csc(payload)
            return

        page = payload[0] & self.page_mask
        self.pages[page] += 1

        if self.compat is not None:
            outcome = self.compat.feed(payload)
            if outcome is not None:
                self.attest_outcomes[outcome] += 1

        if page in (ap.PAGE_COMMON_MANUFACTURER, ap.PAGE_COMMON_PRODUCT):
            self.common_pages_seen[page] += 1
            self._common_since_std += 1
            if self.messages_since_common is not None:
                self.common_gaps.append(self.messages_since_common)
            self.messages_since_common = 0
            return

        if page in ap.COMPAT_PAGES:
            # Interleaved rather than data. It lands between two data pages
            # exactly as a common page does, and the exact-loss heuristic below
            # has to be told so: a transmitter whose event counter steps once
            # per MESSAGE steps it here too, and a receiver that did not count
            # this message would read the step as a lost data page. That is a
            # 1.2 % phantom loss on an otherwise perfect stream - the compat
            # layer's own airtime cost, reported as a fault.
            #
            # It does not reset the common-page cycle, because it is not a
            # common page and the profile's 121-message requirement is
            # unaffected by it.
            self._common_since_std += 1

        if self.messages_since_common is not None:
            self.messages_since_common += 1
        elif self.packets > COMMON_PAGE_LIMIT:
            # Nothing common has arrived yet and the limit has already gone
            # past. Start counting from here so the gap is reported once the
            # first one does show up.
            self.messages_since_common = 0

        if self.device_type == ap.HRM_DEVICE_TYPE:
            self._feed_hr(payload)
        elif self.device_type == ap.RADIANT_TLM_DEVICE_TYPE:
            self._feed_telemetry(t, payload)
        elif page == ap.PAGE_POWER_STANDARD:
            self._feed_power_std(payload)
        elif page in (ap.PAGE_POWER_WHEEL_TORQUE, ap.PAGE_POWER_CRANK_TORQUE):
            self._feed_power_torque(payload)
        elif page == ap.PAGE_POWER_TORQUE_FREQ:
            self._feed_power_torque_freq(payload)

    # -- the RadiANT telemetry envelope, device type 0x60 -----------------

    def _feed_telemetry(self, t: float, payload: bytes) -> None:
        """Learn the schema from the node, then check its accumulators.

        Nothing about this node is known in advance - not its fields, not its
        widths, not which pages it uses. Everything below is decoded against a
        descriptor that arrived on the air, which is why the first thing this
        does is assemble one and the second is decline to decode until it has.
        """
        page = payload[0]

        if page == ap.PAGE_TLM_DESCRIPTOR:
            try:
                complete = self.tlm_rx.feed(payload)
            except ValueError as error:
                self._violation(f"descriptor frame: {error}")
                return
            if not complete:
                return
            try:
                schema = self.tlm_rx.take()
            except ValueError as error:
                self._violation(f"descriptor set: {error}")
                return
            if (self.tlm_desc is not None
                    and schema.schema_id != self.tlm_desc.schema_id):
                # A new schema renumbers every field offset, so every baseline
                # held against the old one is now a comparison between two
                # different layouts. Dropping them is the only correct move.
                self.tlm_schema_changes += 1
                self.tlm_baseline = {}
            self.tlm_desc = schema
            return

        if not ap.PAGE_TLM_DATA_FIRST <= page <= ap.PAGE_TLM_DATA_LAST:
            # A command page, a reserved page, or a vendor-private one. A
            # receiver ignores what it does not recognise; it does not guess.
            return

        if self.tlm_desc is None:
            # Counted rather than decoded. A receiver that joined mid-stream
            # is entitled to see data before it sees a schema, and inventing
            # one is the failure this whole envelope exists to prevent.
            self.tlm_data_before_schema += 1
            return

        if not self.tlm_desc.may_decode_data:
            # Fail closed: a transform bit this build does not implement.
            self._violation("the node announced a transform this receiver "
                            "does not implement; its data pages are not "
                            "decodable and must not be decoded")
            return

        try:
            got = ap.decode_tlm_data(self.tlm_desc, payload)
        except ValueError as error:
            self._violation(f"data page 0x{page:02X}: {error}")
            return
        self.tlm_data_decoded += 1

        before = self.tlm_baseline.get(page)
        self.tlm_baseline[page] = {"t": t, "counter": got["counter"],
                                   "values": got["values"]}
        if before is None:
            return

        d_counter = ap.delta_u8(got["counter"], before["counter"])
        self._note_wrap(f"counter(0x{page:02X})", got["counter"],
                        before["counter"])
        self._note_event(d_counter != 0)

        if d_counter == 0:
            # The event counter counts TRANSMISSIONS, so two packets carrying
            # the same counter are the same transmission - a sparse node's
            # repeat, which the counter exists to let a receiver deduplicate.
            # The bodies must therefore be identical.
            if got["values"] != before["values"]:
                self._violation(
                    f"page 0x{page:02X}: two packets share counter "
                    f"{got['counter']} and carry different values")
            return

        for field in self.tlm_desc.fields:
            if field.page != page or not field.accumulate:
                continue
            name = f"field {field.id} (0x{field.type:02X})"
            now = got["values"][field.id]
            was = before["values"][field.id]
            span = 1 << field.width
            delta = (now - was) % span
            self._note_wrap(name, now, was)

            # An accumulator differenced IN ITS OWN WIDTH can never be
            # negative, so the only thing a single delta can be wrong about is
            # its size - and a delta past half the field's range is what a
            # reset, a restart, or a subtraction done after widening to int
            # looks like. That last one is the failure section 5 warns about:
            # correct for hours, then one absurd sample per wrap, easy to
            # dismiss as radio noise.
            if delta > span // 2:
                self._violation(
                    f"page 0x{page:02X} {name}: accumulator advanced by "
                    f"{delta} in {field.width} bits over {d_counter} event(s), "
                    "which is more than half its range")

        self._check_tlm_integral(t, page, got, before, d_counter)

    def _check_tlm_integral(self, t: float, page: int, got: dict, before: dict,
                            d_counter: int) -> None:
        """Check an accumulator against its instantaneous partner.

        Section 5's rule - "anything integrable is published as an accumulating
        field ... the accumulator is authoritative" - is what makes this
        possible without knowing anything about the node: section 7 fixes which
        accumulating type is which instantaneous type's integral, so a receiver
        can look the pair up in the vocabulary and check one against the other.
        That is the generic form of what _feed_power_std does for ANT+ page
        0x10's acc_power against inst_power.

        Only pairs on the SAME page are checked, because only those share a
        timestamp; and only the pairs where the accumulator is literally the
        time integral in canonical SI units, because the rest need a constant
        this receiver would have to invent.
        """
        dt = t - before["t"]
        if dt <= 0.0:
            return

        for inst in self.tlm_desc.fields:
            partner_type = ap.TLM_TIME_INTEGRAL_PARTNER.get(inst.type)
            if partner_type is None or inst.page != page or inst.accumulate:
                continue
            for acc in self.tlm_desc.fields:
                if (acc.page != page or acc.type != partner_type
                        or not acc.accumulate):
                    continue
                span = 1 << acc.width
                delta = (got["values"][acc.id] - before["values"][acc.id]) % span
                implied = (delta * (10.0 ** acc.exponent)) / dt
                actual = ap.tlm_value_si(inst, got["values"][inst.id])
                tolerance = max(15.0, 0.35 * abs(actual))
                if abs(implied - actual) > tolerance:
                    self._violation(
                        f"page 0x{page:02X}: field {acc.id} accumulated "
                        f"{implied:.1f} per second over {dt:.3f} s, while "
                        f"field {inst.id} reports {actual:.1f} - the "
                        "accumulator and its instantaneous partner disagree")

    def _feed_power_std(self, payload: bytes) -> None:
        got = ap.decode_power_std(payload)
        if got["inst_power"] != ap.INVALID_U16:
            self.power_samples.append(float(got["inst_power"]))
        if got["cadence"] is not None:
            self.cadence_samples.append(float(got["cadence"]))

        before = self.std_baseline
        self.std_baseline = got
        if before is None:
            # Common pages that arrived before the first page 0x10 sit outside
            # every pair, so they are not evidence about the counter.
            self._common_since_std = 0
            return

        d_event = ap.delta_u8(got["event_count"], before["event_count"])
        d_acc = ap.delta_u16(got["acc_power"], before["acc_power"])
        self._note_wrap("event_count", got["event_count"],
                        before["event_count"])
        self._note_wrap("acc_power", got["acc_power"], before["acc_power"])
        self._note_event(d_event != 0)

        # Which convention the transmitter follows is voted pair by pair: a
        # clean pair (nothing lost) with c common pages between its two page
        # 0x10 packets shows d_event == 1 if the counter ignores common pages,
        # or 1 + c if it counts every message; a pair with loss shows neither
        # and abstains. Comparing run totals instead is wrong - the excess of
        # counter over packets received conflates common pages with lost ones,
        # and at a few hundred packets they're the same size (a 300 s run here
        # flipped its verdict once loss crossed 20 packets).
        c = self._common_since_std
        if c > 0:
            if d_event == 1:
                self.std_votes_per_page += 1
            elif d_event == 1 + c:
                self.std_votes_per_message += 1

        self.std_event_pairs += 1
        self.std_event_span += d_event
        self.std_common_seen += c
        self._common_since_std = 0
        if d_event == 0:
            # A reopen after a staged dropout retransmits the held state - the
            # counter genuinely did not step, but for a reason this run staged
            # on purpose rather than a stuck sensor. The gap immediately
            # before this pair is the tell: an ordinary still packet arrives a
            # period apart, and a reopen's does not arrive until the dropout
            # has elapsed. Excluding it here keeps one staged dropout from
            # disqualifying "loss (exact)" for the whole run.
            gap = self.intervals[-1] if self.intervals else 0.0
            reopen_retransmit = (self.dropout_s is not None
                                 and gap >= 0.5 * self.dropout_s)
            if not reopen_retransmit:
                self.std_event_still += 1

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

    def _feed_hr(self, payload: bytes) -> None:
        """Heart rate, device type 0x78.

        Bytes [4..7] are the same on every page, so the accumulator pair is
        checked without caring which page arrived - which is the property that
        lets a background rotation and an inserted RadiANT page cost a receiver
        nothing. A page number this decoder does not know is skipped here
        exactly as a legacy receiver skips one, which is the whole additive-page
        mechanism observed from the receiving end.
        """
        page = payload[0] & ap.HR_PAGE_NUMBER_MASK
        if page not in ap._HR_DECODERS:
            return
        got = ap.decode_hr(payload)

        before = self.hr_baseline
        self.hr_baseline = got
        if before is None:
            return

        d_beats = ap.delta_u8(got["beat_count"], before["beat_count"])
        d_time = ap.delta_u16(got["event_time"], before["event_time"])
        self._note_wrap("beat_count", got["beat_count"], before["beat_count"])
        self._note_wrap("hr_event_time", got["event_time"],
                        before["event_time"])
        self._note_event(d_beats != 0)

        if d_beats == 0:
            if d_time != 0:
                self._violation(f"0x78: event time advanced by {d_time} with "
                                "no new heartbeat")
            return
        if d_time == 0:
            self._violation(f"0x78: {d_beats} heartbeat(s) with no elapsed "
                            "event time")
            return
        self.bpm_samples.append(ap.heart_rate_bpm_from(d_beats, d_time))

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
        if self.dropout_s is not None and expected > 0 and self.period_s > 0:
            # A staged dropout could never have delivered a packet, so its
            # slots do not belong in the denominator - counting them as
            # expected-but-missing is exactly the inflation loss_accounting()
            # exists to catch, applied to a hole this run dug on purpose.
            expected = max(0.0, expected - self.dropout_s / self.period_s)
        loss_pct = (100.0 * (1.0 - self.packets / expected)
                    if expected > 0 else float("nan"))

        reacquire_s = (reacquire_from_intervals(self.intervals, self.dropout_s)
                       if self.dropout_s is not None else None)

        jitter = {
            "n": len(self.intervals),
            "mean_s": statistics.fmean(self.intervals) if self.intervals else None,
            "stdev_s": (statistics.pstdev(self.intervals)
                        if len(self.intervals) > 1 else None),
            "max_s": max(self.intervals) if self.intervals else None,
            "period_s": self.period_s,
            "radio_n": len(self.radio_intervals),
            "offgrid_host_s": self._offgrid(self.intervals),
            "offgrid_radio_s": self._offgrid(self.radio_intervals),
        }

        signal = ({"n": len(self.rssi_samples),
                   "mean_dbm": statistics.fmean(self.rssi_samples),
                   "min_dbm": min(self.rssi_samples),
                   "max_dbm": max(self.rssi_samples)}
                  if self.rssi_samples else None)

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
        heart_rate = accuracy(self.bpm_samples, self.expect_bpm)

        # CLEAR means "no key here" and nothing else. A channel with a key
        # installed never reports it, however clean the stream looks.
        attestation = ({"verdict": ATTEST_CLEAR} if self.compat is None
                       else dict(self.compat.summary(),
                                 outcomes=dict(self.attest_outcomes)))

        checks = []

        def check(name: str, ok: bool, detail: str) -> None:
            checks.append({"name": name, "pass": bool(ok), "detail": detail})

        # A SPARSE NODE HAS NO PACKET RATE, and measuring one against a period
        # is the exact mistake docs/radiant-telemetry.md section 8 warns about:
        # "the receiver will report it as terrible link quality rather than as
        # a configuration mismatch". A node that transmits forty messages a day
        # while keeping a 4 Hz period configured scores 99.9 % loss and
        # enormous jitter while working perfectly. The flag is in the
        # descriptor precisely so this is a decision made from data, and this
        # is the receiver making it.
        sparse_node = (self.tlm_desc is not None
                       and bool(self.tlm_desc.flags & ap.TLM_FLAG_SPARSE))

        check("packets", self.packets > 0,
              f"{self.packets} received, {expected:.0f} expected over "
              f"{elapsed:.1f} s")
        if sparse_node:
            check("sparse cadence", True,
                  f"{self.packets} transmission(s) in {elapsed:.1f} s from a "
                  f"node announcing sparse mode with a "
                  f"{self.tlm_desc.heartbeat_s} s heartbeat - loss and jitter "
                  "are not measured against a period this node never claimed "
                  "to fill")
        elif self.packets and expected > 0:
            # The resolution matters more than the number on a short run. At
            # ~4 Hz a minute is only 240 packets, so a single dropped one is
            # 0.4 % and two put an otherwise perfect link over a 1 % limit.
            # Reporting it stops a short run from being read as a regression.
            # Judged on the rounded figure, which is the one printed. Deciding
            # on more precision than is shown produces a line that reads
            # "2.50 % (limit 2.50 %)" next to the word FAIL, and a report that
            # appears to contradict itself gets treated as a broken tool.
            check("loss", round(loss_pct, 2) <= max_loss_pct,
                  f"{loss_pct:.2f} % (limit {max_loss_pct:.2f} %, "
                  f"one packet = {100.0 / expected:.2f} %)")
        if jitter["stdev_s"] is not None and self.period_s > 0 and not sparse_node:
            # Left exactly as it was, on the host clock, because the USB path
            # is part of what a dongle is and this check has always covered it.
            # What it is not is a measurement of the link's timing - see
            # _offgrid, and the "timing" line the report prints underneath.
            limit = jitter_factor * self.period_s
            check("jitter", jitter["stdev_s"] <= limit,
                  f"stddev {jitter['stdev_s'] * 1000:.1f} ms, max "
                  f"{jitter['max_s'] * 1000:.1f} ms "
                  f"(limit {limit * 1000:.1f} ms, period "
                  f"{self.period_s * 1000:.1f} ms)")

        check("accumulator continuity", self.violation_count == 0,
              f"{self.violation_count} violation(s)")

        # Exact loss, where the transmitter's counter allows it. Requiring the
        # counter to have never once stood still is what proves it is stepped
        # per message rather than per pedal stroke; a real power meter fails
        # that on its first coast and is reported on the wall clock alone.
        exact = None
        if self.std_event_pairs >= 30 and self.std_event_still == 0:
            # The counter stepped std_event_span times over the run, and every
            # step that did not arrive as a packet was lost. What "a packet"
            # means is the vote above; with no votes either way there were no
            # common pages to judge by, and the two readings coincide.
            per_message = self.std_votes_per_message > self.std_votes_per_page
            received = (self.std_event_pairs + self.std_common_seen
                        if per_message else self.std_event_pairs)
            missed = max(0, self.std_event_span - received)
            sent = self.std_event_span
            scope = "message" if per_message else "page 0x10"
            exact = {
                "scope": scope,
                "missed": missed,
                "sent": sent,
                "loss_pct": 100.0 * missed / sent if sent else 0.0,
            }
            check("loss (exact)", round(exact["loss_pct"], 2) <= max_loss_pct,
                  f"{exact['loss_pct']:.2f} % - {missed} of {sent} "
                  f"{scope} message(s) missing, counted from the "
                  f"transmitter's own event counter rather than a clock")

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

        if self.device_type == ap.RADIANT_TLM_DEVICE_TYPE:
            # The envelope's own check, and the one that says whether this
            # node is decodable at all: a receiver that never assembled a
            # descriptor set has learned nothing, however clean the stream.
            check("descriptor decoded", self.tlm_desc is not None,
                  "no descriptor set assembled" if self.tlm_desc is None else
                  f"schema 0x{self.tlm_desc.schema_id:02X}, "
                  f"{len(self.tlm_desc.fields)} field(s), period "
                  f"{self.tlm_desc.period}, "
                  f"{self.tlm_data_decoded} data page(s) decoded against it "
                  f"({self.tlm_data_before_schema} seen before it arrived)")

        if self.compat is not None:
            tier1 = attestation["tier_i"]
            tier2 = attestation["tier_ii"]
            check("attestation", attestation["verdict"] == ATTEST_VERIFIED,
                  f"{attestation['verdict']}; Tier I "
                  f"{tier1['verified']} verified, {tier1['unverified']} "
                  f"unverified, {tier1['lost']} lost, {tier1['replays']} "
                  f"replay(s); Tier II ({tier2['verdict']}) "
                  f"{tier2['verified']} verified, {tier2['unverified']} "
                  f"unverified, {tier2['lost']} lost; beacon "
                  f"{attestation['beacon_frames']} frame(s), "
                  f"{attestation['key_group_hint_matches']} hint match(es)")

        # A sparse node has no 121-message cycle to hang a common page on
        # (section 8 replaces the interleave outright), so the common-page
        # cadence is checked only where the profile actually requires it. 0x79
        # has no common pages defined for it either.
        if self.device_type != ap.BSC_COMBINED_DEVICE_TYPE and not sparse_node:
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
            # Where this stream came from, for derive()'s per-channel block.
            # Both may be absent from the sensor's point of view - a wildcard
            # pairing has no device number until one is heard, and a replayed
            # capture has no channel at all - so both are nullable and neither
            # is ever invented.
            "channel": self.channel,
            "device_number": self.device_number or None,
            "device_type": self.device_type,
            "period_ticks": self.period,
            "packets": self.packets,
            "elapsed_s": elapsed,
            "expected_packets": expected,
            "loss_pct": loss_pct,
            # T1: stamped by main() after listen() returns, from t_open/
            # t_listen and this analyser's own first_t - None on --replay
            # (no channel-open timestamp exists) and forced None under
            # --expect-dropout (see reacquire_s just below instead).
            "time_to_first_packet_s": self.time_to_first_packet_s,
            # T2: only under --expect-dropout SECONDS, else None.
            "reacquire_s": reacquire_s,
            "jitter": jitter,
            "signal": signal,
            "pages": {f"0x{page:02X}": count
                      for page, count in sorted(self.pages.items())},
            "common_page_gaps": {
                "count": len(self.common_gaps),
                "max": max(self.common_gaps) if self.common_gaps else None,
            },
            "power": power,
            "cadence": cadence,
            "speed_mps": speed,
            "heart_rate_bpm": heart_rate,
            "attestation": attestation,
            "accumulator_wraps": dict(self.wraps),
            # What was learned from the node's own descriptor, and nothing
            # else. None for every device type but 0x60.
            "telemetry": None if self.tlm_desc is None else {
                "schema_id": self.tlm_desc.schema_id,
                "period_ticks": self.tlm_desc.period,
                "rf_index": self.tlm_desc.rf_index,
                "flags": self.tlm_desc.flags,
                "sparse": bool(self.tlm_desc.flags & ap.TLM_FLAG_SPARSE),
                "heartbeat_s": self.tlm_desc.heartbeat_s,
                "descriptor_frames_heard": self.tlm_rx.frames_fed,
                "schema_changes": self.tlm_schema_changes,
                "data_pages_decoded": self.tlm_data_decoded,
                "data_pages_before_schema": self.tlm_data_before_schema,
                "fields": [
                    {
                        "id": f.id,
                        "type": f"0x{f.type:02X}",
                        "quantity": ap.TLM_TYPES.get(f.type, ("unknown", "?"))[0],
                        "unit": ap.TLM_TYPES.get(f.type, ("unknown", "?"))[1],
                        "bits": f.width,
                        "exponent": f.exponent,
                        "accumulate": f.accumulate,
                        "signed": f.signed,
                        "page": f"0x{f.page:02X}",
                        "bit_offset": f.bit_offset,
                    }
                    for f in self.tlm_desc.fields
                ],
            },
            "event_advances": self.event_advances,
            "event_comparisons": self.event_comparisons,
            "exact_loss": exact,
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
    if device_type == ap.HRM_DEVICE_TYPE:
        return "heart-rate"
    if device_type == ap.RADIANT_TLM_DEVICE_TYPE:
        # Both 0x60 profiles are the same envelope with the same decoder, and
        # the difference between them is announced in the descriptor rather
        # than guessable from the page histogram - a telemetry node in a short
        # capture and an asset tag in a long one look alike from outside. A
        # node with data pages is publishing something; one with nothing but
        # page 0x00 is a tag.
        data = sum(count for page, count in pages.items()
                   if ap.PAGE_TLM_DATA_FIRST <= page <= ap.PAGE_TLM_DATA_LAST)
        return "telemetry" if data else "asset-tag"
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
               device_number: int, trans_type: int,
               rf_freq: int = ANT_PLUS_FREQ) -> bool:
    """Assign, configure and open one slave channel on the ANT+ network."""
    steps = [
        (MESG_ASSIGN_CHANNEL_ID,
         bytes([channel, CHANNEL_TYPE_SLAVE, ap.ANT_PLUS_NETWORK_NUM]),
         "assign slave"),
        (MESG_CHANNEL_ID_ID,
         bytes([channel, device_number & 0xFF, (device_number >> 8) & 0xFF,
                device_type, trans_type]), "channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([channel, rf_freq]),
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


def extended_fields(body: bytes) -> dict:
    """Decode whatever the flag byte says was appended to a broadcast.

    The fields come in a fixed order - channel id, RSSI, receive timestamp -
    but each is present only if its flag bit is set, so an offset is only
    correct relative to which of the earlier ones turned up. Reading RSSI at a
    fixed offset works right up until a run is made without the channel id and
    then quietly reports the device number as a signal strength.
    """
    fields: dict = {}
    if len(body) < 10:
        return fields
    flags = body[9]
    at = 10

    if flags & EXT_FLAG_CHANNEL_ID:
        if len(body) < at + 4:
            return fields
        fields["device_number"] = body[at] | (body[at + 1] << 8)
        fields["device_type"] = body[at + 2] & 0x7F
        at += 4

    if flags & EXT_FLAG_RSSI:
        if len(body) < at + 3:
            return fields
        # The one RSSI measurement type that carries dBm. Anything else is a
        # proprietary scale, and a number on an unknown scale is worse than no
        # number.
        if body[at] == RSSI_MEASUREMENT_TYPE_DBM:
            fields["rssi_dbm"] = body[at + 1] - 256 if body[at + 1] > 127 \
                else body[at + 1]
        at += 3

    if flags & EXT_FLAG_RX_TIMESTAMP:
        if len(body) < at + 2:
            return fields
        fields["rx_ticks"] = body[at] | (body[at + 1] << 8)

    return fields


def listen(dev, reader, analyzers: dict, seconds: float, verbose: bool,
           records: list) -> dict:
    """Collect broadcasts for `seconds`, timestamping each on arrival.

    Arrival time is the host's, so the jitter figure includes the USB path -
    honest, since that path is part of what's validated, but not the whole
    story: with ENABLE_RX_TIMESTAMP the radio also stamps each packet on its
    own 32 kHz clock. On this bench host-clock jitter is 3.3 ms vs. 0.01 ms on
    the radio clock, i.e. most of what this tool used to print was Windows'
    USB scheduling, not the link. Both are measured now.
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
            fields = extended_fields(body)
            analyzer.feed(now, payload, fields)
            # The device number is filled in afterwards from the identity the
            # extended messages carry - it is not known when the first packets
            # arrive, and a capture with the sensor left anonymous cannot be
            # split back into per-sensor streams on replay.
            records.append([now, analyzer.device_type, channel, payload])
            # Extended messages carry the channel id after the payload, which
            # is the only way to know which sensor was actually heard rather
            # than which one was asked for.
            if "device_number" in fields:
                identities[channel] = (fields["device_number"],
                                       fields["device_type"])
        elif msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
            if body[1] == MESG_EVENT_ID:
                events[body[2]] += 1

        if verbose and time.monotonic() - last_report >= 5.0:
            last_report = time.monotonic()
            print("  " + ", ".join(
                f"ch{ch} {a.packets} pkts" for ch, a in analyzers.items()))

    return {"events": {EVENT_CODES_BY_VALUE.get(code, str(code)): count
                       for code, count in sorted(events.items())},
            "identities": {str(ch): {"device_number": ident[0],
                                     "device_type": ident[1]}
                           for ch, ident in identities.items()}}


# The two events by which the radio owns up to an empty slot, in both spellings
# this tool has ever written into a result document. The names now come from
# tools/ant_wire.py and carry the protocol's EVENT_ prefix; results recorded
# before that - the baselines in archive/benchmarks/ that ant_ab.py diffs
# against - carry the bare form. Reading an old document and concluding the
# radio reported nothing would blame the host for loss the air ate, which is
# exactly the misdiagnosis this function exists to prevent.
RADIO_FAIL_EVENTS = frozenset((
    EVENT_CODES_BY_VALUE[EVENT_RX_FAIL],
    EVENT_CODES_BY_VALUE[EVENT_RX_FAIL_GO_TO_SEARCH],
    "RX_FAIL",
    "RX_FAIL_GO_TO_SEARCH",
))


def loss_accounting(result: dict) -> dict | None:
    """Decide whether the radio can account for the packets that went missing.

    Loss on its own does not say whose fault it is. RX_FAIL is the stack
    reporting that a message was expected in a timeslot and did not arrive
    intact, so loss it accounts for happened on the air and is the room's
    business. Loss it does not account for happened *after* the radio had the
    packet - on the USB path, in the bridge, or in this tool - and is somebody's
    bug. That is the difference between "move the boards apart" and "we are
    dropping packets we already received".

    It is worth failing on rather than only printing, because that is exactly
    how a 250 ms libusb read timeout against a 249.7 ms channel period hid here
    for as long as it did: it invented packet loss that looked identical to the
    on-air kind, and the only thing that gave it away was the radio's silence
    about it.

    Returns None on --replay: a capture file records payloads, not channel
    events, so there are no RX_FAILs to find and this would blame the host for
    every packet the radio itself dropped.
    """
    events = result.get("events")
    if events is None:
        return None

    radio_fails = sum(count for name, count in events.items()
                      if name in RADIO_FAIL_EVENTS)
    missing = sum(max(0, round(c["expected_packets"]) - c["packets"])
                  for c in result["channels"])
    if missing <= 0:
        return None

    unaccounted = max(0, missing - radio_fails)

    # Not zero tolerance. `missing` is the wall-clock estimate, so it carries a
    # packet of rounding either way, and an event landing after the listening
    # window closes is counted in one column and not the other. A quarter of the
    # loss, or two packets, is slack for that and nothing like the half that the
    # read-timeout bug produced.
    allowed = max(2, round(0.25 * missing))

    if unaccounted == 0:
        verdict = ("all of it reported by the radio as RX_FAIL, so it "
                   "happened on the air")
    elif radio_fails > 0:
        verdict = (f"{radio_fails} of it reported as RX_FAIL; the other "
                   f"{unaccounted} went missing without the radio noticing")
    else:
        verdict = ("none of it reported as RX_FAIL - the radio thinks it "
                   "delivered everything, so look at the USB path")

    return {
        "missing": missing,
        "radio_fails": radio_fails,
        "unaccounted": unaccounted,
        "allowed_unaccounted": allowed,
        "verdict": verdict,
        "pass": unaccounted <= allowed,
    }


def _number(value) -> float | None:
    """A JSON number, or None where the tool has no answer.

    `loss_pct` is a genuine NaN when there is no denominator to divide by (an
    unknown device type has no period, so there is no packet count to expect).
    NaN is not JSON, and a reader that gets one has to guess whether it means
    zero or unknown - so the derived block says None, which the schema's
    ["number", "null"] already allows and every consumer already handles.
    """
    if value is None:
        return None
    if isinstance(value, float) and math.isnan(value):
        return None
    return value


def _ms(seconds) -> float | None:
    """Seconds to milliseconds, keeping None as None.

    The derived block is milliseconds throughout because that is the unit the
    gate thresholds in tools/ab_gates.toml are written in, and a unit
    conversion between the two files is a place for a factor of 1000 to hide.
    """
    value = _number(seconds)
    return None if value is None else value * 1000.0


def derive(result: dict) -> dict:
    """Lift out the handful of numbers `tools/ab_gates.toml`'s gates read.

    archive/benchmarks/README.md keeps a `derived` block beside every recorded
    run so that "a gate runner does not need to know the tool's object shape".
    Until now that block was typed by hand out of the report, which is a
    transcription step between the measurement and the gate - and the one
    number a transcription step is most likely to pick up is `loss_pct`, the
    wall-clock figure the gates specifically must not read. Emitting the block
    from the tool that made the measurement removes the step.

    THE AGGREGATE FIELDS KEEP THEIR SINGLE-CHANNEL MEANING EXACTLY. Every
    baseline in archive/benchmarks/ is a one-channel run, and
    `ant_ab.py`'s gate_loss() reads `derived.loss_exact_pct` from those files;
    with one channel each expression below reduces, term for term, to the
    single channel's own figure. Multi-channel is where they have to choose,
    and they choose the way the underlying quantity composes:

      * loss_exact_pct - one ratio over the pooled counters
        (sum missed / sum sent), not a mean of ratios. A mean of ratios gives
        a channel that heard forty packets the same weight as one that heard
        four hundred.
      * loss_pct - the same pooling on the wall-clock estimate.
      * accumulator_violations - a sum. Any violation anywhere is a decoder or
        wrap-handling bug, and burying one channel's in an average would be
        the whole point of the gate lost.
      * timing_offgrid_*_ms - the WORST channel, not the mean, for the reason
        Baseline.worst() gives: averaging away one bad run is how a regression
        ships.

    per_channel is the block this exists for. The defect it was written to
    measure - a scheduler exclusion radius that drops the later of two windows
    that land too close together - is invisible in any aggregate: it costs
    whole slots on one channel of a pair while the other is untouched, so the
    pooled figure moves by half of an already small number. Per channel it is
    a step.

    Not emitted here, deliberately: `sched_missed`. That counter lives in the
    dongle (MESG_RADIANT_HEALTH, tools/ant_health.py) and is not on this
    tool's wire at all. It belongs in the same `derived` block - ant_ab.py
    gates on it - but a run has to read it out of the firmware before and
    after, and inventing a zero here would be worse than leaving it absent.
    """
    channels = result.get("channels", [])

    per_channel: dict[str, dict] = {}
    missed_total = 0
    sent_total = 0
    packets_total = 0
    expected_total = 0.0
    violations_total = 0
    offgrid_host: list[float] = []
    offgrid_radio: list[float] = []
    rssi_weighted = 0.0
    rssi_n = 0
    ttfp_values: list[float] = []
    reacquire_values: list[float] = []

    for index, channel in enumerate(channels):
        exact = channel.get("exact_loss")
        jitter = channel.get("jitter", {})
        signal = channel.get("signal")

        # The key is the receive channel where one is known, and the stream's
        # own name where it is not - a replayed capture carries no channel
        # number, and a key invented from the list position would read like a
        # channel and not be one. `channel` inside the entry is the machine-
        # readable form of the same fact, and is null in that case.
        number = channel.get("channel")
        key = str(number) if number is not None else channel.get(
            "profile", f"stream {index}")

        entry = {
            "channel": number,
            "profile": channel.get("profile"),
            "device_number": channel.get("device_number"),
            "packets": channel.get("packets", 0),
            "loss_pct": _number(channel.get("loss_pct")),
            "loss_exact_pct": None,
            "exact_missed": None,
            "exact_sent": None,
            "exact_scope": None,
            "accumulator_violations": channel.get("violations", {}).get(
                "count", 0),
            "timing_offgrid_host_ms": _ms(jitter.get("offgrid_host_s")),
            "timing_offgrid_radio_ms": _ms(jitter.get("offgrid_radio_s")),
            "rssi_dbm_mean": signal["mean_dbm"] if signal else None,
            # T1/T2. Both None on most runs - see ChannelAnalyzer's own
            # docstrings for time_to_first_packet_s and reacquire_s.
            "time_to_first_packet_s": _number(
                channel.get("time_to_first_packet_s")),
            "reacquire_s": _number(channel.get("reacquire_s")),
        }

        # A wildcard channel was configured with no device number, so the only
        # device number it has is the one it actually heard - which is the
        # answer to "which sensor is this figure about" anyway, and is the
        # reason lib config 0xE0 asks for the channel id at all.
        if entry["device_number"] is None and number is not None:
            heard = result.get("identities", {}).get(str(number))
            if heard:
                entry["device_number"] = heard.get("device_number")

        if exact is not None:
            entry["loss_exact_pct"] = exact["loss_pct"]
            entry["exact_missed"] = exact["missed"]
            entry["exact_sent"] = exact["sent"]
            entry["exact_scope"] = exact["scope"]
            missed_total += exact["missed"]
            sent_total += exact["sent"]

        per_channel[key] = entry

        packets_total += channel.get("packets", 0)
        expected = _number(channel.get("expected_packets"))
        if expected:
            expected_total += expected
        violations_total += entry["accumulator_violations"]
        if entry["timing_offgrid_host_ms"] is not None:
            offgrid_host.append(entry["timing_offgrid_host_ms"])
        if entry["timing_offgrid_radio_ms"] is not None:
            offgrid_radio.append(entry["timing_offgrid_radio_ms"])
        if signal:
            rssi_weighted += signal["mean_dbm"] * signal["n"]
            rssi_n += signal["n"]
        if entry["time_to_first_packet_s"] is not None:
            ttfp_values.append(entry["time_to_first_packet_s"])
        if entry["reacquire_s"] is not None:
            reacquire_values.append(entry["reacquire_s"])

    accounting = result.get("accounting")

    return {
        # The wall-clock figure. Recorded because the baselines record it, and
        # never the one to gate on - it assumes the transmitter's crystal is
        # exact and rounds its denominator.
        "loss_pct": (100.0 * (1.0 - packets_total / expected_total)
                     if expected_total > 0 else None),
        # THE gate. None rather than 0.0 when no channel could produce one:
        # "the counter could not be trusted" and "nothing was lost" are
        # different answers, and ant_ab.py already fails a gate whose field is
        # missing. A zero here would pass it instead.
        "loss_exact_pct": (100.0 * missed_total / sent_total
                           if sent_total else None),
        # None on --replay, where loss_accounting() correctly declines to
        # judge: a capture records payloads and not channel events, so there
        # are no RX_FAILs to account with.
        "unexplained_loss": (accounting["unaccounted"]
                             if accounting is not None else None),
        "accumulator_violations": violations_total,
        "timing_offgrid_host_ms": max(offgrid_host) if offgrid_host else None,
        "timing_offgrid_radio_ms": (max(offgrid_radio) if offgrid_radio
                                    else None),
        "rssi_dbm_mean": rssi_weighted / rssi_n if rssi_n else None,
        # T1/T2. Run-level is the worst (slowest) channel, the same reasoning
        # as timing_offgrid_*_ms above. None whenever no channel produced a
        # figure - every run on --replay, and every ordinary run for
        # reacquire_s.
        "time_to_first_packet_s": max(ttfp_values) if ttfp_values else None,
        "reacquire_s": max(reacquire_values) if reacquire_values else None,
        "per_channel": per_channel,
    }


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
        jitter = channel["jitter"]
        if jitter.get("offgrid_host_s") is not None:
            # The jitter check above is dominated by the gap each lost packet
            # leaves. This is the same packets with the slot count taken out,
            # so it moves when the timing moves and not when the loss does.
            line = (f"  timing: {jitter['offgrid_host_s'] * 1000:.2f} ms off "
                    f"the {jitter['period_s'] * 1000:.1f} ms slot grid by the "
                    f"host clock")
            if jitter.get("offgrid_radio_s") is not None:
                line += (f", {jitter['offgrid_radio_s'] * 1000:.3f} ms by the "
                         f"radio's")
            print(line)

        signal = channel.get("signal")
        if signal:
            # Printed rather than judged. It answers a question no threshold
            # can: an ANT slave tracks down to about -90 dBm, so a desk pair
            # sitting near -30 has some 60 dB in hand, and loss at 60 dB of
            # margin is not the link running out of signal. What a limit here
            # would actually encode is how far apart the boards happen to be.
            print(f"  signal: mean {signal['mean_dbm']:.1f} dBm "
                  f"(min {signal['min_dbm']}, max {signal['max_dbm']}, "
                  f"n={signal['n']}) - ANT tracks to about -90")
        if channel["accumulator_wraps"]:
            # A run that never wrapped has not tested the wrap. Say so, rather
            # than letting a clean pass imply coverage it does not have.
            print("  wraps observed: " + ", ".join(
                f"{field} x{count}"
                for field, count in sorted(channel["accumulator_wraps"].items())))
        else:
            print("  wraps observed: none - this run did not reach a 16-bit "
                  "accumulator wrap")

        attestation = channel.get("attestation")
        if attestation and attestation["verdict"] != ATTEST_CLEAR:
            tier1 = attestation["tier_i"]
            rate = tier1["verified_of_delivered"]
            # Printed as "of delivered" rather than "of expected" on purpose:
            # Tier I's claim is that its verification rate equals its page
            # DELIVERY rate, independently of the interval. Mixing loss into
            # this number would hide exactly the property being asserted.
            print(f"  attestation: {attestation['verdict']}"
                  + (f" - Tier I {rate * 100:.1f}% of delivered pages verified"
                     if rate is not None else " - no Tier I page delivered")
                  + f", {tier1['lost']} never arrived")
            if attestation["tier_ii"]["verdict"] != ATTEST_CLEAR:
                tier2 = attestation["tier_ii"]
                print(f"  Tier II (N={tier2['window']}): "
                      f"{tier2['verified']} window(s) verified, "
                      f"{tier2['unverified']} failed the tag, "
                      f"{tier2['lost']} unverifiable through loss")
        elif attestation:
            print("  attestation: clear - no key here, which is not the same "
                  "as unprotected")

        for label in ("power", "cadence", "speed_mps", "heart_rate_bpm"):
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

    accounting = result.get("accounting")
    if accounting:
        print(f"{accounting['missing']} packet(s) missing, "
              f"{accounting['verdict']}.")
        if not accounting["pass"]:
            print("  FAIL loss accounting - loss the radio never reported is "
                  "loss that happened after the radio, on this host or in the "
                  "dongle, and it is not a property of the link")
    for channel, ident in result.get("identities", {}).items():
        print(f"heard on channel {channel}: device #{ident['device_number']}, "
              f"type 0x{ident['device_type']:02X}")

    # The per-channel exact-loss table, printed only when there is more than
    # one channel to compare. On one channel it would restate the `loss
    # (exact)` check line above it, and a report that says the same number
    # twice invites the reader to wonder which one is authoritative.
    per_channel = result.get("derived", {}).get("per_channel", {})
    if len(per_channel) > 1:
        print("\nper channel (exact loss, from the transmitters' own "
              "counters):")
        for key, entry in per_channel.items():
            exact = entry["loss_exact_pct"]
            figure = ("no exact figure - the event counter stood still, so it "
                      "is not stepped per message"
                      if exact is None else
                      f"{exact:.2f} % ({entry['exact_missed']} of "
                      f"{entry['exact_sent']} {entry['exact_scope']} "
                      f"message(s) missing)")
            label = f"channel {key}" if entry["channel"] is not None else key
            print(f"  {label:12} {entry['profile']:24} {figure}")
        # Spread, not mean: the whole reason for measuring per channel is that
        # a scheduler that drops the later of two nearby windows costs one
        # channel of a pair and leaves the other alone. That is a difference
        # between channels, and no aggregate can show it.
        values = [entry["loss_exact_pct"] for entry in per_channel.values()
                  if entry["loss_exact_pct"] is not None]
        if len(values) > 1:
            print(f"  spread: {min(values):.2f} % to {max(values):.2f} % "
                  f"across {len(values)} channel(s) - a per-channel spread on "
                  f"one link is a scheduling effect, not a room effect")


def per_channel_args(values: list[int] | None, count: int, name: str, *,
                     default: int, ramp: bool = False,
                     wildcard: int | None = None) -> list[int]:
    """Expand one repeatable per-channel argument to one value per channel.

    Three forms, and the first two are the ones that already existed:

      * absent      - the default, on every channel.
      * given once  - one value for every channel, except that `ramp` ones
                      count up from it (channel N, N+1, ... and device #N,
                      #N+1, ...). That ramp is not new and is not decoration:
                      several bench scripts pass a single --channel with
                      several --profile flags and rely on the channels not
                      colliding.
      * repeated    - one value per channel, positionally. This is the form
                      that exists to place two receive windows deliberately
                      close together, which is how a scheduler's exclusion
                      radius is measured.

    `wildcard` is the value that means "any", and is never ramped: 0 pairs
    with any device, and ramping it to 1 would silently pin channel 1 to a
    real device number that nobody asked for.

    A wrong count is refused rather than zipped short. Positional pairing that
    silently truncates would apply channel 2's frequency to channel 3 and
    report the result as a measurement.
    """
    if not values:
        return [default] * count
    if len(values) == 1:
        one = values[0]
        if ramp and one != wildcard:
            return [one + index for index in range(count)]
        return [one] * count
    if len(values) != count:
        raise SystemExit(
            f"{name} was given {len(values)} times but this run has {count} "
            f"channel(s). Give it once (for all of them) or exactly {count} "
            f"times, in channel order - a partial pairing would attribute one "
            f"channel's setting to another and report the result as a "
            f"measurement.")
    return list(values)


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
    parser.add_argument("--expect-bpm", type=float,
                        help="heart rate the transmitter was told to produce")
    parser.add_argument("--compat-key", metavar="HEX",
                        help="16-byte root key as hex. WITHOUT IT EVERY CHANNEL "
                             "REPORTS 'clear', which means 'no key here' and "
                             "not 'unprotected'. With it, a channel that "
                             "delivers no attestation at all reports "
                             "'unverified' rather than falling back to clear - "
                             "which is the whole of receiver-side downgrade "
                             "protection")
    parser.add_argument("--epoch", type=int, default=1,
                        help="epoch the keys and nonces are derived under "
                             "(default: 1). It is not on the air; a receiver "
                             "recovers it by searching forward against the "
                             "beacon's key-group hint")
    parser.add_argument("--attest-window", type=int, metavar="N",
                        choices=ap.COMPAT_WINDOW_SIZES, default=None,
                        help="expect Tier II data attestation with this "
                             "window. Announced in the beacon; give it here to "
                             "check a stream before a beacon has arrived")
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
    parser.add_argument("--expect-dropout", type=float, metavar="SECONDS",
                        help="a deliberate dropout of this many seconds was "
                             "staged during the run - e.g. apps/sim's "
                             "ANT_SIM_DROPOUT_EVERY_S/ANT_SIM_DROPOUT_S knobs. "
                             "Produces reacquire_s per channel (the worst gap "
                             "minus this duration), subtracts the dropout "
                             "from expected_packets so loss_accounting() "
                             "stays honest, excludes the reopen's "
                             "still-counter retransmit from the exact-loss "
                             "disqualifier, and forces "
                             "time_to_first_packet_s to None - acquisition "
                             "timing and a staged dropout are not the same "
                             "measurement")
    # ── The four per-channel arguments ──────────────────────────────────────
    #
    # All four repeat, and repeating any of them switches this tool from "one
    # sensor, measured well" to "several sensors, measured separately". That
    # is the measurement the multi-channel scheduling defect needs: it is
    # invisible at one channel by construction, so a tool that can only be
    # pointed at one channel can never see it.
    #
    # Given once, each keeps exactly the meaning it had - including the two
    # ramps below, which several bench scripts depend on - so every existing
    # invocation and every recorded baseline still means what it meant.
    parser.add_argument("--device-number", type=int, action="append",
                        metavar="N",
                        help="device number to pair with; 0 is a wildcard, "
                             "which is what a fitness app uses (default: 0). "
                             "Given once with several profiles it ramps "
                             "(N, N+1, ...); repeat it to pair each channel "
                             "with a device of your own choosing")
    parser.add_argument("--trans-type", type=int, action="append",
                        metavar="N",
                        help="transmission type to pair with; 0 is a wildcard "
                             "(default: 0). Give it once for every channel, "
                             "or repeat it per channel")
    parser.add_argument("--channel", type=int, action="append", metavar="N",
                        help="ANT channel to use (default: 0). Given once it "
                             "is the FIRST channel and the rest follow it "
                             "(N, N+1, ...); repeat it to place each channel "
                             "explicitly, which is what puts two receive "
                             "windows where you want them")
    parser.add_argument("--rf-freq", type=int, action="append",
                        metavar="N",
                        help="RF frequency as MHz above 2400; 57 is the ANT+ "
                             "public network and the only value a real sensor "
                             "uses. Set it, and CONFIG_ANT_SIM_RF_FREQ to "
                             "match, only to find out whether bench loss is "
                             "the room rather than the dongle: 2457 MHz sits "
                             "inside Wi-Fi channel 11 and carries every other "
                             "ANT+ device in range, and somewhere quiet like "
                             "2 or 78 does not (default: 57)")
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

    # How many channels this run has: as many as the most-repeated of the two
    # arguments that can name one. --profile alone (several sensors of
    # different kinds, the Zwift shape) and --channel alone (several sensors
    # of the SAME kind on channels of the operator's choosing, the shape the
    # scheduling defect needs) both work, and one profile is spread across
    # every channel because that is the multi-master bench: eight identical
    # power masters, deliberately phased.
    profiles = args.profile or ["power"]
    count = max(len(profiles), len(args.channel or []), 1)
    if len(profiles) not in (1, count):
        raise SystemExit(
            f"--profile was given {len(profiles)} times and --channel "
            f"{len(args.channel or [])}; give one profile for all channels or "
            f"one per channel.")
    if len(profiles) == 1:
        profiles = profiles * count

    channels = per_channel_args(args.channel, count, "--channel", default=0,
                                ramp=True)
    device_numbers = per_channel_args(args.device_number, count,
                                      "--device-number", default=0, ramp=True,
                                      wildcard=0)
    trans_types = per_channel_args(args.trans_type, count, "--trans-type",
                                   default=0)
    rf_freqs = per_channel_args(args.rf_freq, count, "--rf-freq",
                                default=ANT_PLUS_FREQ)
    if len(set(channels)) != len(channels):
        raise SystemExit(
            f"--channel repeats a channel number: {channels}. Two streams on "
            f"one channel cannot be told apart, and the per-channel figures "
            f"would silently be a merge of both.")

    json_to_stdout = args.json == "-"
    verbose = not args.quiet and not json_to_stdout

    # Everything up to the result is narration, and with --json - the caller
    # wants a parseable document on stdout and nothing else. Redirecting rather
    # than guarding each print catches the bring-up chatter from ant_scan's
    # command() too, which this file does not own.
    stack = contextlib.ExitStack()
    if json_to_stdout:
        stack.enter_context(contextlib.redirect_stdout(sys.stderr))

    def make_verifier(device_number: int) -> CompatVerifier | None:
        """A verifier per stream, or None - which is what CLEAR means.

        The device number is inside the nonce, so a wildcard cannot be used to
        check a tag: 0 pairs with anything, and a tag is computed against
        exactly one sensor.
        """
        if not args.compat_key:
            return None
        if not device_number:
            print("  note: --compat-key needs a --device-number; a wildcard "
                  "cannot check a tag, so this channel stays clear")
            return None
        return CompatVerifier(bytes.fromhex(args.compat_key), args.epoch,
                              device_number, window=args.attest_window)

    def make_analyzer(name: str, channel: int,
                      device_number: int) -> ChannelAnalyzer:
        """One analyser per channel.

        Every piece of per-stream state this tool keeps - the accumulator
        baselines, the event-counter pairs the exact-loss figure is counted
        from, the Tier II window - is already an attribute of the analyser
        rather than a global, so several channels need no new bookkeeping:
        one object each, and listen() dispatches on the channel byte the
        broadcast arrives with. What was missing was only the ability to say
        which channel and which device each one is.
        """
        spec = ap.PROFILES[name]
        return ChannelAnalyzer(name, spec["device_type"], spec["period"],
                               args.expect_watts, args.expect_rpm,
                               args.wheel_circ,
                               compat=make_verifier(device_number),
                               expect_bpm=args.expect_bpm,
                               channel=channel, device_number=device_number,
                               dropout_s=args.expect_dropout)

    extra: dict = {}
    records: list = []

    if args.replay:
        # WHICH CHANNEL A REPLAYED STREAM WAS HEARD ON IS NOT IN THE FILE.
        # A capture records the sensor - time, device type, device number,
        # payload - because the channel a packet arrived on is a fact about
        # the receiver and not about the sensor (profile_for()'s docstring
        # says so, and the .antcap line format has no column for it). So the
        # only honest way to label a replayed stream with a channel is for the
        # operator to say which device was on which channel, which is exactly
        # what the paired --channel/--device-number arguments already express:
        #
        #     --replay f.antcap --channel 0 --device-number 4660 \
        #                       --channel 1 --device-number 4661
        #
        # A single ramped --device-number reproduces the same pairing a live
        # multi-channel run used, so a capture recorded by one replays with
        # its channel labels intact. Unpaired streams keep a null channel and
        # are keyed by name; nothing is guessed from the order of the file.
        # strict=True on both zips in this file: per_channel_args() has
        # already refused a mismatched count, so a short zip here could only
        # mean that guarantee was broken, and silently dropping a channel is
        # how a run reports three sensors' loss as two.
        channel_of_device = {number: channel
                             for number, channel in zip(device_numbers,
                                                        channels, strict=True)
                             if number}

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
            mask = (ap.HR_PAGE_NUMBER_MASK
                    if device_type == ap.HRM_DEVICE_TYPE else 0xFF)
            pages = Counter(payload[0] & mask for _, payload in packets)
            name = profile_for(device_type, pages)
            analyzer = ChannelAnalyzer(
                f"{name} #{device_number}", device_type,
                period_for(device_type, name), args.expect_watts,
                args.expect_rpm, args.wheel_circ,
                compat=make_verifier(device_number),
                expect_bpm=args.expect_bpm,
                channel=channel_of_device.get(device_number),
                device_number=device_number)
            for t, payload in packets:
                analyzer.feed(t, payload)
            ordered.append(analyzer)

        if verbose:
            print(f"replayed {args.replay}: {len(ordered)} sensor stream(s)")
    else:
        dev = open_device(verbose, serial=args.serial, port=args.port,
                          baud=args.baud)
        reader = FrameReader(dev)

        print("\nOpening ANT+ receive channels")
        if not reset_stack(dev, reader):
            print("  FAIL: no startup message after reset")
            return 1

        # 0x80 ENABLE_CHANNEL_ID, 0x40 ENABLE_RSSI, 0x20 ENABLE_RX_TIMESTAMP.
        #
        # Without the channel id the broadcasts arrive anonymous, so a run
        # cannot say which sensor it measured. The other two were free for the
        # asking and were not being asked for, which cost this project a lot:
        # RSSI is the difference between a packet lost to a collision and one
        # lost to a fade, and the receive timestamp is the radio's own 32 kHz
        # clock, which is the only clock here that is not Windows.
        #
        # Worth having, not worth aborting over - every measurement that
        # existed before these has a host-clock fallback.
        command(dev, reader, MESG_ANTLIB_CONFIG_ID,
                bytes([0x00, LIB_CONFIG_ALL_EXT_FIELDS]), "extended messages")
        if not command(dev, reader, MESG_NETWORK_KEY_ID,
                       bytes([ap.ANT_PLUS_NETWORK_NUM]) + ANT_PLUS_KEY,
                       "ANT+ network key"):
            return 1

        analyzers = {}
        # T1: when each channel's OPEN command completed - the moment a real
        # receiver would start expecting packets. Only meaningful relative to
        # t_listen just below, since the two are read off the same clock.
        t_open: dict[int, float] = {}
        for name, channel, device_number, trans_type, rf_freq in zip(
                profiles, channels, device_numbers, trans_types, rf_freqs,
                strict=True):
            spec = ap.PROFILES[name]
            if not open_slave(dev, reader, channel, spec["device_type"],
                              spec["period"], device_number, trans_type,
                              rf_freq):
                print(f"  FAIL: channel {channel} ({name}) did not open")
                return 1
            t_open[channel] = time.monotonic()
            pinned = (f", pinned to #{device_number}" if device_number
                      else ", wildcard")
            print(f"  OK: channel {channel} - {spec['label']}{pinned}")
            analyzers[channel] = make_analyzer(name, channel, device_number)

        print(f"\nListening for {args.seconds:.0f} s")
        t_listen = time.monotonic()
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

        # T1: (t_listen - t_open[ch]) + the analyser's own first_t, which is
        # relative to t_listen (listen() timestamps from its own start). Left
        # at the __init__ default of None under --expect-dropout: that mode
        # measures re-acquisition after a staged dropout, not the initial
        # acquisition this figure is about.
        if args.expect_dropout is None:
            for analyzer in ordered:
                if (analyzer.first_t is not None
                        and analyzer.channel in t_open):
                    analyzer.time_to_first_packet_s = (
                        (t_listen - t_open[analyzer.channel])
                        + analyzer.first_t)

    if args.record and not args.replay:
        # The device number is what splits a capture back into per-sensor
        # streams on replay, and a capture whose sensors are all #0 cannot be
        # split at all - two channels would merge into one analysis and the
        # merge would be reported as loss. So: the identity actually heard
        # first, and the device number the channel was PINNED to as the
        # fallback, which is a fact about the run rather than a guess. Zero
        # only survives where the channel was a wildcard and nothing
        # identified itself.
        identities = extra.get("identities", {})
        pinned = dict(zip(channels, device_numbers, strict=True))
        resolved = [
            (t, device_type,
             identities.get(str(channel), {}).get("device_number", 0)
             or pinned.get(channel, 0), payload)
            for t, device_type, channel, payload in records
        ]
        ap.write_capture(args.record, resolved, comments=[
            f"ant_verify.py, {args.seconds:.0f} s, profiles: "
            f"{', '.join(profiles)}",
            "channels: " + ", ".join(
                f"ch{channel}=#{number}" if number else f"ch{channel}=wildcard"
                for channel, number in zip(channels, device_numbers,
                                           strict=True)),
            f"expected {args.expect_watts} W, {args.expect_rpm} rpm",
        ])
        print(f"\nwrote {args.record} ({len(records)} packets)")

    result = {
        "channels": [a.summary(args.max_loss, args.jitter_factor,
                               args.max_error) for a in ordered],
    }
    result.update(extra)
    accounting = loss_accounting(result)
    if accounting is not None:
        result["accounting"] = accounting
    # After the accounting, because unexplained_loss is one of the numbers it
    # lifts out, and before the report, which prints the per-channel table
    # from it.
    result["derived"] = derive(result)
    result["pass"] = (all(c["pass"] for c in result["channels"])
                      and (accounting is None or accounting["pass"]))

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
