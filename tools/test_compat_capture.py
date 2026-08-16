#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""compat-C5's gate: the C compat master emits byte-exact ANT+ plus two pages.

`radiant/tests/src/test_profile_compat.c` drives `src/profiles/profile_hr.c`
and `src/profiles/profile_power.c` on a DK and prints every transmitted message
as a `.antcap` line. Those captures are committed to `tools/vectors/`, and this
file decodes them with `tools/ant_pages.py` - written independently, from the
layout tables rather than from the C - and **re-encodes every page, asserting
the bytes come back identical**. The re-encode is the load-bearing half: a
decode that merely succeeds proves the page number was recognised, but a
decode whose fields re-encode to the same eight bytes proves the two
implementations agree about every field, width, byte order and sentinel.

Three claims are checked here, each a sentence from `docs/decisions/0008`:

  * **"byte-exact ANT+ apart from the two added page numbers."** Strip pages
    `0x70`-`0x72` from either capture and what is left decodes as a stock ANT+
    stream, with no page number outside the profile's own set, and re-encodes
    byte for byte.
  * **"2.0% of slots"** - 0.8% beacon plus 1.2% Tier I, against the 1.65% ANT+
    itself spends on common pages 80 and 81. Measured here from the capture,
    which is what turns a calculation in a plan into a number in a fixture.
  * **"a keyed receiver rejects a colliding stranger's sensor
    cryptographically."** `ant_verify.CompatVerifier` holds the root and every
    Tier I tag in the capture verifies under it - so the tags the C wrote are
    tags the Python reads, which is the same cross-check applied to the
    cryptography rather than to the layout.

**Live captures are not run here.** `tools/ant_verify.py --replay` over these
files does the same decoding against a radio-shaped analyser and needs no
hardware either; this suite is the version that runs on every push.
"""

from __future__ import annotations

import os
import unittest

import ant_pages as ap
import ant_verify as av
import radiant_crypto as rc

TOOLS = os.path.dirname(os.path.abspath(__file__))
VECTORS = os.path.join(TOOLS, "vectors")
SRC_PROFILES = os.path.join(os.path.dirname(TOOLS), "radiant", "src", "profiles")

# Pinned in radiant/tests/src/test_profile_compat.c; must agree or
# nothing verifies. Also ant_sim's DEFAULT_COMPAT_ROOT, so a C capture and a
# simulator capture are two streams under one key.
ROOT = bytes(range(16))
EPOCH = 7
DEVNUM = 0x2C41

HR_CAPTURE = os.path.join(VECTORS, "compat-hr.antcap")
POWER_CAPTURE = os.path.join(VECTORS, "compat-power.antcap")

# Every page number a stock sensor of each type may put on the air. The
# compat allocation is deliberately excluded: the test subtracts it and
# asserts what's left is a subset ("these pages are expected", not "nothing
# else got in").
HR_ANT_PLUS_PAGES = {
    ap.PAGE_HR_DEFAULT, ap.PAGE_HR_CUMULATIVE_TIME, ap.PAGE_HR_MANUFACTURER,
    ap.PAGE_HR_PRODUCT, ap.PAGE_HR_PREVIOUS_BEAT,
    ap.PAGE_COMMON_MANUFACTURER, ap.PAGE_COMMON_PRODUCT,
    ap.PAGE_COMMON_BATTERY,
}
POWER_ANT_PLUS_PAGES = {
    ap.PAGE_POWER_STANDARD, ap.PAGE_POWER_WHEEL_TORQUE,
    ap.PAGE_POWER_CRANK_TORQUE, ap.PAGE_POWER_TORQUE_FREQ,
    ap.PAGE_COMMON_MANUFACTURER, ap.PAGE_COMMON_PRODUCT,
    ap.PAGE_COMMON_BATTERY,
}


def page_of(payload: bytes, device_type: int) -> int:
    """The page number, with heart rate's toggle bit taken off first."""
    if device_type == ap.HRM_DEVICE_TYPE:
        return payload[0] & ap.HR_PAGE_NUMBER_MASK
    return payload[0]


def toggle_of(payload: bytes, device_type: int) -> bool:
    if device_type != ap.HRM_DEVICE_TYPE:
        return False
    return bool(payload[0] & ap.HR_PAGE_TOGGLE)


def reencode_hr(payload: bytes) -> bytes:
    """Decode one heart-rate payload and build it again from the fields."""
    page = payload[0] & ap.HR_PAGE_NUMBER_MASK
    toggle = bool(payload[0] & ap.HR_PAGE_TOGGLE)
    got = ap.decode_hr(payload)
    tail = dict(event_time=got.get("event_time"),
                beat_count=got.get("beat_count"),
                computed_hr=got.get("computed_hr"),
                toggle=toggle)

    if page == ap.PAGE_HR_DEFAULT:
        return ap.encode_hr_default(**tail)
    if page == ap.PAGE_HR_CUMULATIVE_TIME:
        return ap.encode_hr_cumulative_time(got["operating_time"], **tail)
    if page == ap.PAGE_HR_MANUFACTURER:
        return ap.encode_hr_manufacturer(got["manufacturer_id"],
                                         got["serial_upper16"], **tail)
    if page == ap.PAGE_HR_PRODUCT:
        return ap.encode_hr_product(got["hw_version"], got["sw_version"],
                                    got["model_number"], **tail)
    if page == ap.PAGE_HR_PREVIOUS_BEAT:
        return ap.encode_hr_previous_beat(
            got["previous_event_time"],
            manufacturer_specific=got["manufacturer_specific"], **tail)

    # A common page on device type 0x78 carries the toggle too, so
    # ant_pages.decode_hr() strips it before dispatching.
    body = reencode_common(bytes([page]) + payload[1:])
    if toggle:
        body = bytes([body[0] | ap.HR_PAGE_TOGGLE]) + body[1:]
    return body


def reencode_common(payload: bytes) -> bytes:
    page = payload[0]
    if page == ap.PAGE_COMMON_MANUFACTURER:
        got = ap.decode_common_80(payload)
        return ap.encode_common_80(got["hw_revision"], got["manufacturer_id"],
                                   got["model_number"])
    if page == ap.PAGE_COMMON_PRODUCT:
        got = ap.decode_common_81(payload)
        return ap.encode_common_81(got["sw_revision_main"],
                                   got["serial_number"],
                                   got["sw_revision_supplemental"])
    if page == ap.PAGE_COMMON_BATTERY:
        got = ap.decode_common_82(payload)
        return ap.encode_common_82(got["fractional_voltage"],
                                   got["coarse_voltage"], got["status"],
                                   got["operating_time"],
                                   got["time_resolution_16s"])
    raise AssertionError(f"page 0x{page:02X} is not a common page")


def reencode_power(payload: bytes) -> bytes:
    page = payload[0]
    if page == ap.PAGE_POWER_STANDARD:
        got = ap.decode_power_std(payload)
        return ap.encode_power_std(got["event_count"], got["cadence"],
                                   got["acc_power"], got["inst_power"],
                                   got["pedal_power"])
    if page in (ap.PAGE_POWER_WHEEL_TORQUE, ap.PAGE_POWER_CRANK_TORQUE):
        got = ap.decode_power_torque(payload)
        return ap.encode_power_torque(page, got["event_count"], got["ticks"],
                                      got["cadence"], got["acc_period"],
                                      got["acc_torque"])
    if page == ap.PAGE_POWER_TORQUE_FREQ:
        got = ap.decode_power_torque_freq(payload)
        return ap.encode_power_torque_freq(got["event_count"],
                                           got["slope_tenth_nm_hz"],
                                           got["time_stamp"],
                                           got["torque_ticks"])
    return reencode_common(payload)


def reencode(payload: bytes, device_type: int) -> bytes:
    """One message, decoded and rebuilt. Compat pages included."""
    page = page_of(payload, device_type)
    toggle = toggle_of(payload, device_type)

    if page == ap.COMPAT_PAGE_ATTEST_TIER_I:
        got = ap.decode_compat_attest_tier1(payload)
        return ap.encode_compat_attest_tier1(got["att_counter"], got["tag"],
                                             toggle)
    if page == ap.COMPAT_PAGE_ATTEST_TIER_II:
        got = ap.decode_compat_attest_tier2(payload)
        return ap.encode_compat_attest_tier2(got["window_index"], got["tag"],
                                             toggle)
    if page == ap.COMPAT_PAGE_BEACON:
        # The beacon is a two-frame set; handled per capture in
        # test_the_beacon_round_trips_as_a_set.
        return payload

    if device_type == ap.HRM_DEVICE_TYPE:
        return reencode_hr(payload)
    return reencode_power(payload)


class TestTheCapturesExist(unittest.TestCase):
    """The fixture is the evidence, so its absence must be a failure - a
    silent skip would report a pass on a tree where the gate was deleted
    (tools/vectors/README.md documents the same failure mode)."""

    def test_both_captures_are_present_and_the_right_length(self):
        for path, device_type in ((HR_CAPTURE, ap.HRM_DEVICE_TYPE),
                                  (POWER_CAPTURE, ap.BPWR_DEVICE_TYPE)):
            records = ap.read_capture(path)
            self.assertEqual(3 * ap.RADIANT_TLM_CYCLE, len(records),
                             f"{path} should be three 121-message cycles")
            for _, dtype, devnum, payload in records:
                self.assertEqual(device_type, dtype)
                self.assertEqual(DEVNUM, devnum)
                self.assertEqual(8, len(payload))


class TestByteExactness(unittest.TestCase):
    """THE GATE. Every page the C emitted re-encodes to the same eight bytes."""

    def check(self, path, device_type, ant_plus_pages):
        records = ap.read_capture(path)
        for index, (_, _, _, payload) in enumerate(records):
            page = page_of(payload, device_type)
            if page == ap.COMPAT_PAGE_BEACON:
                continue
            again = reencode(payload, device_type)
            self.assertEqual(payload.hex(), bytes(again).hex(),
                             f"{os.path.basename(path)} message {index}, page "
                             f"0x{page:02X}: the C bytes and the Python "
                             f"encoder's bytes differ")

    def test_the_heart_rate_stream_is_byte_exact(self):
        self.check(HR_CAPTURE, ap.HRM_DEVICE_TYPE, HR_ANT_PLUS_PAGES)

    def test_the_power_stream_is_byte_exact(self):
        self.check(POWER_CAPTURE, ap.BPWR_DEVICE_TYPE, POWER_ANT_PLUS_PAGES)

    def test_nothing_outside_the_profile_and_the_allocation_is_on_the_air(self):
        for path, device_type, allowed in (
                (HR_CAPTURE, ap.HRM_DEVICE_TYPE, HR_ANT_PLUS_PAGES),
                (POWER_CAPTURE, ap.BPWR_DEVICE_TYPE, POWER_ANT_PLUS_PAGES)):
            pages = {page_of(p, device_type)
                     for _, _, _, p in ap.read_capture(path)}
            extra = pages - allowed
            self.assertEqual(set(ap.COMPAT_PAGES) & extra, extra,
                             f"{os.path.basename(path)} carries page numbers "
                             f"that are neither this profile's nor the compat "
                             f"allocation: "
                             f"{sorted(hex(p) for p in extra - set(ap.COMPAT_PAGES))}")
            # The allocation really is on the air - a capture with no compat
            # page at all would pass every assertion above.
            self.assertIn(ap.COMPAT_PAGE_BEACON, pages)
            self.assertIn(ap.COMPAT_PAGE_ATTEST_TIER_I, pages)
            # Tier II is off by default, and that is a claim about the stream.
            self.assertNotIn(ap.COMPAT_PAGE_ATTEST_TIER_II, pages)

    def test_stripping_the_two_pages_leaves_a_stock_ant_plus_stream(self):
        """The compatibility claim, stated as an operation on the capture."""
        for path, device_type, allowed in (
                (HR_CAPTURE, ap.HRM_DEVICE_TYPE, HR_ANT_PLUS_PAGES),
                (POWER_CAPTURE, ap.BPWR_DEVICE_TYPE, POWER_ANT_PLUS_PAGES)):
            stock = [p for _, _, _, p in ap.read_capture(path)
                     if page_of(p, device_type) not in ap.COMPAT_PAGES]
            self.assertTrue(stock)
            for payload in stock:
                page = page_of(payload, device_type)
                self.assertIn(page, allowed)
                got = ap.decode(payload, device_type)
                self.assertNotIn("raw", got,
                                 f"page 0x{page:02X} did not decode")


class TestTheCadence(unittest.TestCase):
    """119 / 120 / 121, which is what a certified receiver pairs on."""

    def check_gap(self, path, device_type):
        records = ap.read_capture(path)
        last = {ap.PAGE_COMMON_MANUFACTURER: None, ap.PAGE_COMMON_PRODUCT: None}
        worst = 0
        for index, (_, _, _, payload) in enumerate(records):
            page = page_of(payload, device_type)
            if page in last:
                if last[page] is not None:
                    worst = max(worst, index - last[page])
                last[page] = index
        self.assertTrue(0 < worst <= av.COMMON_PAGE_LIMIT,
                        f"{os.path.basename(path)}: worst common-page gap is "
                        f"{worst}, limit {av.COMMON_PAGE_LIMIT}")
        return worst

    def test_the_common_pages_are_never_displaced(self):
        for path, device_type in ((HR_CAPTURE, ap.HRM_DEVICE_TYPE),
                                  (POWER_CAPTURE, ap.BPWR_DEVICE_TYPE)):
            # Exactly 121: inserted pages ride the rotation rather than
            # lengthening it. A grown gap would be the compat layer
            # stretching the cycle - the one thing a receiver's own
            # conformance check would notice.
            self.assertEqual(ap.RADIANT_TLM_CYCLE,
                             self.check_gap(path, device_type))

    def test_the_toggle_steps_every_four_messages_on_heart_rate(self):
        records = ap.read_capture(HR_CAPTURE)
        for index, (_, _, _, payload) in enumerate(records):
            want = bool((index // 4) & 1)
            got = bool(payload[0] & ap.HR_PAGE_TOGGLE)
            self.assertEqual(want, got,
                             f"message {index} carries toggle {got}")

    def test_no_toggle_bit_on_bicycle_power(self):
        for _, _, _, payload in ap.read_capture(POWER_CAPTURE):
            self.assertEqual(0, payload[0] & 0x80,
                             "byte [0] is a whole page number on 0x0B")


class TestTheTwoPercentClaim(unittest.TestCase):
    """0.8% beacon + 1.2% Tier I, against ANT+'s own 1.65%.

    Measured from the capture rather than calculated: the number is what the
    whole compatibility argument rests on, and a calculation in a plan can be
    wrong in a way nobody notices.
    """

    def test_the_compat_pages_cost_about_two_percent_of_slots(self):
        for path, device_type in ((HR_CAPTURE, ap.HRM_DEVICE_TYPE),
                                  (POWER_CAPTURE, ap.BPWR_DEVICE_TYPE)):
            records = ap.read_capture(path)
            pages = [page_of(p, device_type) for _, _, _, p in records]
            total = len(pages)

            beacon = pages.count(ap.COMPAT_PAGE_BEACON) / total
            tier1 = pages.count(ap.COMPAT_PAGE_ATTEST_TIER_I) / total
            common = ((pages.count(ap.PAGE_COMMON_MANUFACTURER)
                       + pages.count(ap.PAGE_COMMON_PRODUCT)) / total)

            self.assertAlmostEqual(0.008, beacon, delta=0.003)
            self.assertAlmostEqual(0.012, tier1, delta=0.005)
            self.assertAlmostEqual(0.0165, common, delta=0.002)
            self.assertLess(beacon + tier1, 0.025,
                            f"{os.path.basename(path)} spends "
                            f"{100 * (beacon + tier1):.1f}% of its slots on "
                            f"RadiANT; the claim is 2.0%")


class TestTheAttestation(unittest.TestCase):
    """The tags the C wrote verify under a Python receiver holding the root."""

    def verify(self, path):
        verifier = av.CompatVerifier(ROOT, EPOCH, DEVNUM)
        for _, _, _, payload in ap.read_capture(path):
            verifier.feed(payload)
        return verifier.summary()

    def test_every_tier_one_page_verifies(self):
        for path in (HR_CAPTURE, POWER_CAPTURE):
            summary = self.verify(path)
            tier1 = summary["tier_i"]
            self.assertGreaterEqual(tier1["verified"], 4,
                                    f"{os.path.basename(path)} carried "
                                    f"{tier1['verified']} verified Tier I "
                                    f"pages")
            self.assertEqual(0, tier1["unverified"])
            self.assertEqual(0, tier1["replays"])
            self.assertEqual(0, tier1["lost"])
            # A file has no loss, so this must be 1.0 or the tag
            # construction is wrong.
            self.assertEqual(1.0, tier1["verified_of_delivered"])
            self.assertEqual(av.ATTEST_VERIFIED, summary["verdict"])

    def test_the_beacon_decodes_and_its_hint_is_the_receivers(self):
        for path in (HR_CAPTURE, POWER_CAPTURE):
            summary = self.verify(path)
            self.assertEqual(0, summary["beacon_malformed"])
            self.assertGreaterEqual(summary["beacon_frames"], 3)
            beacon = summary["beacon"]
            self.assertIsNotNone(beacon,
                                 "no complete beacon set in the capture")
            self.assertEqual(ap.COMPAT_VERSION, beacon["version"])
            self.assertTrue(beacon["attest_available"])
            # policy `never` and no locator must agree, or the beacon is
            # malformed - a `never` node has nowhere to go.
            self.assertEqual(ap.COMPAT_POLICY_NEVER, beacon["policy"])
            self.assertFalse(beacon["private_available"])
            self.assertFalse(beacon["pending_switch"])
            self.assertEqual(0, beacon["target_device_type"])
            self.assertEqual(0, beacon["target_device_number"])
            self.assertEqual(0, beacon["target_period"])
            # The epoch-derived key-group hint matched: "is this one of
            # mine", which a receiver with several roots asks first.
            self.assertGreaterEqual(summary["key_group_hint_matches"], 1)

    def test_the_hint_the_c_emitted_is_the_one_the_python_derives(self):
        keys = rc.derive_keys(ROOT, EPOCH, DEVNUM)
        expected = rc.compat_key_group_hint(keys[rc.LABEL_ID], EPOCH)
        for path, device_type in ((HR_CAPTURE, ap.HRM_DEVICE_TYPE),
                                  (POWER_CAPTURE, ap.BPWR_DEVICE_TYPE)):
            frames = [p for _, _, _, p in ap.read_capture(path)
                      if page_of(p, device_type) == ap.COMPAT_PAGE_BEACON
                      and ap.compat_frame_index(p)[0] == 0]
            self.assertTrue(frames)
            for frame in frames:
                self.assertEqual(expected, bytes(frame[4:7]))

    def test_the_beacon_round_trips_as_a_set(self):
        for path, device_type in ((HR_CAPTURE, ap.HRM_DEVICE_TYPE),
                                  (POWER_CAPTURE, ap.BPWR_DEVICE_TYPE)):
            frames = {}
            for _, _, _, payload in ap.read_capture(path):
                if page_of(payload, device_type) != ap.COMPAT_PAGE_BEACON:
                    continue
                index, count = ap.compat_frame_index(payload)
                self.assertEqual(ap.COMPAT_BEACON_FRAMES, count)
                frames.setdefault(index, payload)
            self.assertEqual({0, 1}, set(frames),
                             "both beacon frames must appear")

            toggle = toggle_of(frames[0], device_type)
            plain = [bytes([f[0] & ap.HR_PAGE_NUMBER_MASK]) + f[1:]
                     for f in (frames[0], frames[1])]
            beacon = ap.decode_compat_beacon(plain)
            again = ap.encode_compat_beacon(beacon, toggle=toggle)
            self.assertEqual(frames[0].hex(), bytes(again[0]).hex())


class TestTheBoundary(unittest.TestCase):
    """profile_hr.c and profile_power.c call radiant_sec nowhere.

    Greps rather than reasoning about the call graph, because the thing being
    protected is a habit: the next profile author reads profile_hr.c, and if
    a key ever appears there the reason it must not be is three documents
    away. The include closure is checked too (the stronger half):
    profile_compat.h names no radiant_sec type, so these files do not reach
    a radiant header even transitively.
    """

    PROFILE_FILES = ("profile_hr.c", "profile_hr.h", "profile_power.c",
                     "profile_power.h", "profile_common.c", "profile_common.h",
                     # apps/treadmill's two masters make the same claim about
                     # themselves, and the claim is only worth making if it is
                     # checked from outside the file that makes it.
                     "profile_fec_tx.c", "profile_fec_tx.h",
                     "profile_sdm.c", "profile_sdm.h")

    def test_no_profile_module_names_radiant_sec_in_code(self):
        for name in self.PROFILE_FILES:
            path = os.path.join(SRC_PROFILES, name)
            with open(path, encoding="utf-8") as handle:
                for lineno, line in enumerate(handle, start=1):
                    stripped = line.strip()
                    # Comments may discuss the boundary; code may not cross it.
                    if stripped.startswith("*") or stripped.startswith("/*"):
                        continue
                    self.assertNotIn("radiant_sec", stripped,
                                     f"{name}:{lineno} names radiant_sec "
                                     f"outside a comment")

    def test_no_profile_module_includes_a_radiant_header(self):
        for name in self.PROFILE_FILES:
            path = os.path.join(SRC_PROFILES, name)
            with open(path, encoding="utf-8") as handle:
                includes = [line for line in handle
                            if line.lstrip().startswith("#include")]
            for line in includes:
                self.assertNotIn("radiant/", line,
                                 f"{name} includes {line.strip()}")

    def test_profile_compat_is_the_one_file_that_does(self):
        """Or the boundary is a wall with nothing behind it."""
        with open(os.path.join(SRC_PROFILES, "profile_compat.c"),
                  encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("#include <radiant/radiant_sec_compat.h>", text)
        self.assertIn("radiant_sec_compat_tx_attest", text)

        with open(os.path.join(SRC_PROFILES, "profile_compat.h"),
                  encoding="utf-8") as handle:
            header = handle.read()
        for line in header.splitlines():
            if line.lstrip().startswith("#include"):
                self.assertNotIn("radiant/", line,
                                 "profile_compat.h must not export a "
                                 "radiant header to its includers")


TREADMILL_CAPTURE = os.path.join(VECTORS, "treadmill-pages.antcap")


class TestTreadmillGoldenPages(unittest.TestCase):
    """The ten hand-computed FE-C and SDM payloads, from both directions.

    Every other case in this file proves the C and the Python agree with each
    other, which is a strong claim about two implementations and a weak one
    about either of them being RIGHT - both were written by the same project
    from the same reading. `tools/vectors/treadmill-pages.antcap` is hand
    computed from the scalings the headers state, and this suite asserts it
    against the Python codecs while radiant/tests/src/test_profile_fec_tx.c and
    test_profile_sdm.c assert the same byte values against the C ones.

    The re-encode is the load-bearing half, as it is for the compat captures: a
    decode that merely succeeds proves the page number was recognised, whereas
    a decode whose fields re-encode to the same eight bytes proves the
    implementation agrees about every field, width, byte order and sentinel.
    """

    def setUp(self):
        self.records = ap.read_capture(TREADMILL_CAPTURE)
        self.by_page = {}
        for _t, device_type, _dev, payload in self.records:
            self.by_page[(device_type, payload[0])] = payload

    def test_the_capture_covers_both_device_types(self):
        self.assertEqual(len(self.records), 11)
        types = {device_type for _t, device_type, _d, _p in self.records}
        self.assertEqual(types, {ap.FEC_DEVICE_TYPE, ap.SDM_DEVICE_TYPE})

    def test_every_payload_decodes_on_its_own_device_type(self):
        # AND ON ITS OWN DEVICE TYPE SPECIFICALLY. FE-C's 0x10, 0x11 and 0x12
        # collide with bicycle power's Standard Power, Wheel Torque and Crank
        # Torque, and SDM's 0x10 collides with both - nothing in any payload
        # says which, so decode() dispatches on the channel's device type
        # first. Decoding these on the wrong type is how a treadmill's speed
        # becomes a power accumulator with no error anywhere.
        for _t, device_type, _dev, payload in self.records:
            got = ap.decode(payload, device_type)
            self.assertEqual(got["page"], payload[0])

    def test_general_fe_data_round_trips(self):
        payload = self.by_page[(ap.FEC_DEVICE_TYPE, ap.PAGE_FEC_GENERAL)]
        got = ap.decode_fec_general(payload)
        self.assertEqual(got["equipment_type"], ap.FEC_TYPE_TREADMILL)
        self.assertEqual(got["elapsed_time_qs"], 40)
        self.assertEqual(got["distance_m"], 100)
        self.assertEqual(got["speed_mm_s"], 3000)
        self.assertEqual(got["heart_rate"], 150)
        self.assertEqual(got["hr_source"], ap.FEC_HR_SRC_ANTPLUS)
        self.assertEqual(got["state"], ap.FEC_STATE_IN_USE)
        self.assertFalse(got["lap_toggle"])

        again = ap.encode_fec_general(
            got["equipment_type"], got["elapsed_time_qs"], got["distance_m"],
            got["speed_mm_s"], got["heart_rate"], got["state"],
            hr_source=got["hr_source"],
            distance_enabled=got["distance_valid"],
            virtual_speed=got["speed_virtual"], lap_toggle=got["lap_toggle"])
        self.assertEqual(again, payload)

    def test_settings_incline_is_the_grade_the_command_asked_for(self):
        settings = self.by_page[(ap.FEC_DEVICE_TYPE, ap.PAGE_FEC_SETTINGS)]
        command = self.by_page[(ap.FEC_DEVICE_TYPE,
                                ap.PAGE_FEC_TRACK_RESISTANCE)]

        # THE LOOP SECTION 8.8.4 ASKS FOR: "the incline reported should match
        # the grade received in this command page". Two different encodings of
        # one physical quantity, and this is the only place in the tree where
        # they are asserted equal from the wire bytes rather than from a
        # variable that happens to hold both.
        grade = ap.decode_fec_track_resistance(command)["grade_centi_pct"]
        incline = ap.decode_fec_settings(settings)["incline_centi_pct"]
        self.assertEqual(grade, 300)
        self.assertEqual(incline, 300)
        # ... and the wire bytes are NOT the same, which is the trap.
        self.assertNotEqual(command[5:7], settings[4:6])

        self.assertEqual(
            ap.encode_fec_settings(120, 300, None, ap.FEC_STATE_IN_USE),
            settings)
        self.assertEqual(ap.encode_fec_track_resistance(300), command)

    def test_command_status_echoes_the_command_in_place(self):
        command = self.by_page[(ap.FEC_DEVICE_TYPE,
                                ap.PAGE_FEC_TRACK_RESISTANCE)]
        status = self.by_page[(ap.FEC_DEVICE_TYPE, ap.PAGE_FEC_CMD_STATUS)]

        got = ap.decode_fec_cmd_status(status)
        self.assertEqual(got["last_command"], ap.PAGE_FEC_TRACK_RESISTANCE)
        self.assertEqual(got["status"], ap.FEC_CMD_STATUS_PASS)
        # Bytes 4..7 of the command, position for position - so the grade is
        # back at bytes 5..6 of the status, where the controller wrote it.
        self.assertEqual(got["data"], command[4:8])
        self.assertEqual(status[5:7], command[5:7])

    def test_treadmill_and_capabilities_pages(self):
        page19 = self.by_page[(ap.FEC_DEVICE_TYPE, ap.PAGE_FEC_TREADMILL)]
        got = ap.decode_fec_treadmill(page19)
        self.assertEqual(got["cadence_strides_min"], 85)
        self.assertEqual(got["neg_vertical_dm"], 12)
        self.assertEqual(got["pos_vertical_dm"], 47)
        self.assertEqual(ap.encode_fec_treadmill(
            85, 12, 47, ap.FEC_STATE_IN_USE,
            capabilities=(ap.FEC_TREADMILL_CAP_NEG_VERTICAL |
                          ap.FEC_TREADMILL_CAP_POS_VERTICAL)), page19)

        page54 = self.by_page[(ap.FEC_DEVICE_TYPE, ap.PAGE_FEC_CAPABILITIES)]
        caps = ap.decode_fec_capabilities(page54)
        self.assertEqual(caps["capabilities"], ap.FEC_CAP_SIMULATION)
        self.assertIsNone(caps["max_resistance_n"])
        self.assertEqual(ap.encode_fec_capabilities(None,
                                                    ap.FEC_CAP_SIMULATION),
                         page54)

    def test_sdm_pages_round_trip(self):
        page1 = self.by_page[(ap.SDM_DEVICE_TYPE, ap.PAGE_SDM_DEFAULT)]
        got = ap.decode_sdm_default(page1)
        self.assertEqual(got["time_s"], 42)
        self.assertEqual(got["time_frac_200"], 100)
        self.assertEqual(got["distance_frac_16"], 8)
        self.assertEqual(got["speed_int_mps"], 3)
        self.assertEqual(got["speed_frac_256"], 128)
        self.assertEqual(got["strides"], 77)
        self.assertEqual(ap.encode_sdm_default(100, 42, 200, 8, 3, 128, 77, 4),
                         page1)

        base = self.by_page[(ap.SDM_DEVICE_TYPE, ap.PAGE_SDM_BASE)]
        cal = self.by_page[(ap.SDM_DEVICE_TYPE, ap.PAGE_SDM_CALORIES)]
        # The only byte that differs between the two pages.
        self.assertEqual(base[1:6], cal[1:6])
        self.assertEqual(base[7], cal[7])
        self.assertEqual(base[6], 0)
        self.assertEqual(cal[6], 123)
        # And the reserved bytes really are zero: there is no 0xFF sentinel
        # anywhere on this profile, which is the reverse of every other one.
        self.assertEqual(base[1], 0)
        self.assertEqual(base[2], 0)

        status = ap.sdm_status_split(base[7])
        self.assertEqual(status["location"], ap.SDM_LOC_OTHER)
        self.assertEqual(status["use_state"], ap.SDM_USE_ACTIVE)

        summary = self.by_page[(ap.SDM_DEVICE_TYPE, ap.PAGE_SDM_SUMMARY)]
        got = ap.decode_sdm_summary(summary)
        self.assertEqual(got["strides"], 0x123456)
        self.assertEqual(got["distance_256"], 0x89ABCDEF)
        self.assertEqual(ap.encode_sdm_summary(0x123456, 0x89ABCDEF), summary)

        caps = self.by_page[(ap.SDM_DEVICE_TYPE, ap.PAGE_SDM_CAPABILITIES)]
        self.assertEqual(ap.decode_sdm_capabilities(caps)["flags"], 0x17)
        self.assertEqual(ap.encode_sdm_capabilities(0x17), caps)


if __name__ == "__main__":
    unittest.main()
