#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Check the optional ANT features against what the dongle claims to support.

Two things nothing else here checks:

1. **Capabilities versus coverage.** `ant_capabilities_get()` reports what the
   ANT *stack* can do; the serial messages `dispatch()` implements are what the
   *bridge* can do, and they are separate lists that nothing keeps in step. A
   bit that is advertised and unimplemented is a trap: a host reads the bit,
   sends the configuration message, gets INVALID_MESSAGE back, and gives up -
   the same failure mode that once cost this project a week over
   `ANT_SetTransmitPower`. This walks every optional bit and probes the message
   behind it with a harmless (usually "off") configuration.

2. **Round trips.** Advanced burst, selective data updates and event filtering
   are all set/get pairs, so what went out can be read back and compared. That
   is the only check that catches a payload offset which is wrong by one - the
   reply for some of these messages leads with an index byte and for others
   does not, and both shapes look perfectly plausible on a scope.

    python tools/ant_features.py
    python tools/ant_features.py --port COM8      # a UART build

Nothing here opens a channel or puts anything on the air. Exit status is 0 when
every implemented feature round-trips and every mismatch is one of the known
ones listed in KNOWN_GAPS.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_probe import (  # noqa: E402
    EP_OUT,
    FrameReader,
    close_device,
    frame,
    open_device,
    reset_stack,
)
# Protocol constants come from the generated module, never from a second copy
# here. See tools/ant_wire.py and protocol/ant_wire.yaml.
#
# Two prose notes that used to sit on definitions in this file are worth
# keeping, because they are the reason two of the round trips below look odd:
#
#   INVALID_SDU_MASK is the selective-data-update value that detaches a channel
#   from every mask, which is how check_sdu() puts the stack back as it found
#   it.
#
#   The encryption set and get info types do not line up: set 0 is the crypto
#   ID and get 0 is the supported mode, so a round trip writes with one number
#   and reads with the next. Both spellings are separate constants precisely so
#   that asymmetry is visible at the call site.
#
# Everything else those comments said - which values advanced burst takes, what
# each id is - is now carried by protocol/ant_wire.yaml's own description
# fields, which is where the generator gets the comments in tools/ant_wire.py
# and src/ant_wire.h from.
import ant_sec  # noqa: E402

from ant_wire import (  # noqa: E402,F401
    ADV_BURST_MODE_DISABLE,
    ADV_BURST_MODE_ENABLE,
    ADV_BURST_MODES_FREQ_HOP,
    ADV_BURST_MODES_SIZE_24_BYTES,
    ENCRYPTION_DISABLED_MODE,
    ENCRYPTION_INFO_GET_CRYPTO_ID,
    ENCRYPTION_INFO_GET_CUSTOM_USER_DATA,
    ENCRYPTION_INFO_GET_SUPPORTED_MODE,
    ENCRYPTION_INFO_SET_CRYPTO_ID,
    ENCRYPTION_INFO_SET_CUSTOM_USER_DATA,
    ENCRYPTION_INFO_SET_RNG_SEED,
    ENCRYPTION_KEY_SIZE,
    ENCRYPTION_USER_DATA_SIZE,
    CHANNEL_IN_WRONG_STATE,
    INVALID_MESSAGE,
    INVALID_PARAMETER_PROVIDED,
    INVALID_SDU_MASK,
    MESG_CAPABILITIES_ID,
    MESG_CONFIG_ADV_BURST_ID,
    MESG_ENCRYPT_ENABLE_ID,
    MESG_EVENT_BUFFERING_CONFIG_ID,
    MESG_EVENT_FILTER_CONFIG_ID,
    MESG_HIGH_DUTY_SEARCH_MODE_ID,
    MESG_REQUEST_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_SDU_CONFIG_ID,
    MESG_SDU_SET_MASK_ID,
    MESG_SET_ENCRYPT_INFO_ID,
    MESG_SET_ENCRYPT_KEY_ID,
    RESPONSE_NO_ERROR,
)

# Capabilities byte 6, "advanced options 3": every feature in the ANT tech
# bulletin this file exists to check, plus search sharing, which predates it.
ADV3_BITS = [
    (0x01, "advanced burst"),
    (0x02, "event buffering"),
    (0x04, "event filtering"),
    (0x08, "high duty search"),
    (0x10, "active search sharing"),
    (0x20, "radio coexistence config"),
    (0x40, "selective data updates"),
    (0x80, "single channel encryption"),
]

# Advertised, not bridged, and staying that way. Each entry says why, because
# the alternative - clearing the capability bit - would make this dongle report
# something a real ANT USB-m does not, and hosts do read these bytes.
KNOWN_GAPS = {
    "high duty search":
        "no ant_interface.h API on the single-chip path; sdk-ant handles 0x77 "
        "only in the nRF5340 network-processor passthrough",
    "single channel encryption":
        "the write side is behind CONFIG_ANT_DONGLE_ENCRYPTION, off by "
        "default because ANT_DLL.dll exports no encryption call for any host "
        "to reach it with (the read side is always bridged)",
    "radio coexistence config":
        "ant_coex_config_set() exists but no host API reaches it",
}

# Bridged messages that no Windows host can send, where that is a recorded fact
# rather than drift. Anything bridged and unreachable that is *not* named here
# fails the run: bridging a message nothing can call is dead code that costs
# dispatch space and, on the encryption path, stack RAM shared with the plain
# channels.
HOST_UNREACHABLE = {
    "single channel encryption":
        "ANT_DLL.dll exports 154 functions and not one of them keys a channel "
        "or enables AES-CTR, so 0x7D-0x7F are unreachable from any Windows "
        "ANT application that exists (archive/host-api/README.md). This is "
        "the argument for CONFIG_ANT_DONGLE_ENCRYPTION defaulting off",
}

# archive/host-api/ant_dll_exports.json, found relative to this file rather
# than to the working directory. These tools get run from the repository root,
# from tools/, and from a copy of tools/ dropped somewhere else entirely, and
# the script's own path is the only one true in all three.
HOST_API_JSON = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), os.pardir,
    "archive", "host-api", "ant_dll_exports.json")


def load_host_api(path: str = HOST_API_JSON):
    """Index ANT_DLL.dll's export table by the message each call puts on the wire.

    On Windows every ANT application reaches the stick through `ANT_DLL.dll`,
    so its export table bounds what any of them can ask a dongle for - not by
    convention, by construction: a function that is not exported cannot be
    called. That turns "can anything actually reach this bridged message" from
    an argument into a lookup.

    Returns `{mesg_id: [export, ...]}` covering the 72 of 154 exports that
    carry a message id, grouped into the 36 distinct ids they name - the list
    per id is not a formality, because `_RTO` (caller-supplied response
    timeout) and `_ext` variants are separate exports with separate ordinals
    sending the same message. Returns None if the archive is not there: a copy
    of tools/ on its own is a supported way to run these, so a missing archive
    is a reduced check and not a failure.

    Rows with a null `mesg_id` are dropped rather than guessed at. Null means
    one of two different things - the call is host-local (`ANT_Init`, the
    `ANTFS_*` family), or the message is real but its id is not yet in
    protocol/ant_wire.yaml - and the six-field JSON cannot tell them apart.
    archive/host-api/README.md writes both groups out by name, and the fix for
    the second is to add the message to the YAML and regenerate, never to edit
    the JSON.
    """
    try:
        with open(path, encoding="utf-8") as handle:
            exports = json.load(handle)
    except OSError as exc:
        print(f"  -- no host API table at {path} ({exc.strerror}); the "
              "ANT_DLL reachability check is skipped")
        return None
    except ValueError as exc:
        print(f"  -- the host API table at {path} is not valid JSON ({exc}); "
              "the ANT_DLL reachability check is skipped")
        return None

    by_message: dict[int, list] = {}
    for export in exports:
        mesg_id = export.get("mesg_id")
        if mesg_id is not None:
            by_message.setdefault(mesg_id, []).append(export)
    return by_message


def check_host_reach(by_message, name: str, msg_id: int,
                     implemented: bool) -> list[str]:
    """Say who on a Windows host can send `msg_id`, and object if nobody can."""
    exports = by_message.get(msg_id, [])
    if exports:
        zwift = [e["name"] for e in exports if e.get("zwift_uses")]
        line = (f"     host: {', '.join(e['name'] for e in exports)} "
                f"({len(exports)} export(s))")
        line += (f"; Zwift resolves {', '.join(zwift)}" if zwift
                 else "; Zwift resolves none of them")
        print(line)
        return []

    if not implemented:
        print(f"     host: no ANT_DLL export sends 0x{msg_id:02X}, and this "
              "build does not bridge it either - consistent")
        return []

    if name in HOST_UNREACHABLE:
        print(f"     host: no ANT_DLL export sends 0x{msg_id:02X} - "
              f"known: {HOST_UNREACHABLE[name]}")
        return []

    print(f"     host: !! bridged, but no ANT_DLL export sends "
          f"0x{msg_id:02X} - no Windows application can reach it")
    return [f"{name}: bridged with no ANT_DLL entry point"]


def command(dev, reader, msg_id: int, payload: bytes, timeout: float = 2.0):
    """Send a configuration message; return its response code, or None."""
    dev.write(EP_OUT, frame(msg_id, payload))

    deadline = time.monotonic() + timeout
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return None
        got_id, body = result
        if got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
            return body[2]


def request(dev, reader, index: int, req_id: int, timeout: float = 2.0):
    """Request a message; return its payload, or an int response code on refusal.

    `index` occupies the channel slot, which for these messages is not a channel
    at all: it is the mask number, the encryption info type, or - for advanced
    burst - 0 for capabilities and 1 for the current configuration.
    """
    dev.write(EP_OUT, frame(MESG_REQUEST_ID, bytes([index, req_id])))

    deadline = time.monotonic() + timeout
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return None
        got_id, body = result
        if got_id == req_id:
            return body
        if (got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3
                and body[1] == MESG_REQUEST_ID):
            return body[2]


def bridged(code) -> bool:
    """Did the message reach the stack, whatever the stack then said?

    A message the dispatcher does not implement is refused by the dispatcher
    itself with INVALID_MESSAGE. Anything else - success, or a complaint about
    the parameters - means the message was decoded and handed on.
    """
    return code is not None and code != INVALID_MESSAGE


def check_coverage(dev, reader, adv3: int, by_message=None) -> list[str]:
    """Probe every advertised optional feature. Returns unexpected mismatches.

    Three lists have to agree for an optional feature to be worth anything: the
    capability bits the stack advertises, the messages `dispatch()` implements,
    and the entry points `ANT_DLL.dll` exports for a host to call them with.
    The first two are asked of the device; the third is read from
    archive/host-api/ant_dll_exports.json, which is generated from the real PE
    export directory. A feature that is advertised and unbridged strands a host
    that believes the bit; a feature that is bridged and unexported is code no
    host can reach.
    """
    # Each probe is the "off" configuration for its feature, so a firmware that
    # does implement the message is left exactly as it was found.
    probes = {
        "advanced burst": (MESG_CONFIG_ADV_BURST_ID,
                           bytes([0x00, ADV_BURST_MODE_DISABLE,
                                  ADV_BURST_MODES_SIZE_24_BYTES,
                                  0, 0, 0, 0, 0, 0])),
        "event buffering": (MESG_EVENT_BUFFERING_CONFIG_ID, bytes(6)),
        "event filtering": (MESG_EVENT_FILTER_CONFIG_ID, bytes([0x00, 0x00])),
        "high duty search": (MESG_HIGH_DUTY_SEARCH_MODE_ID, bytes([0x00, 0x00])),
        "selective data updates": (MESG_SDU_CONFIG_ID,
                                   bytes([0x00, INVALID_SDU_MASK])),
        "single channel encryption": (MESG_ENCRYPT_ENABLE_ID,
                                      bytes([0x00, 0x00, 0x00, 0x00])),
    }

    problems = []
    print(f"\n[2/6] advertised capabilities vs bridged messages "
          f"(advanced options 3 = 0x{adv3:02X})")

    for bit, name in ADV3_BITS:
        advertised = bool(adv3 & bit)
        if name not in probes:
            # Search sharing is configured per channel by a message the bridge
            # already implements, and radio coexistence has no host API to
            # reach it. Neither has a probe that is worth a false alarm.
            note = KNOWN_GAPS.get(name, "not probed")
            print(f"  .  {name}: "
                  f"{'advertised' if advertised else 'not advertised'} ({note})")
            continue

        msg_id, payload = probes[name]
        code = command(dev, reader, msg_id, payload)
        implemented = bridged(code)

        if advertised and implemented:
            verdict, mark = "advertised, bridged", "OK"
        elif not advertised and not implemented:
            verdict, mark = "not advertised, not bridged", "OK"
        elif advertised and not implemented:
            if name in KNOWN_GAPS:
                verdict, mark = f"advertised, NOT bridged - known: {KNOWN_GAPS[name]}", "--"
            else:
                verdict, mark = "advertised but NOT bridged - a host that " \
                                "believes the bit will stall here", "!!"
                problems.append(f"{name}: advertised, unimplemented")
        else:
            verdict, mark = ("bridged but not advertised - harmless, though no "
                             "host will ever send it", "--")

        print(f"  {mark} {name} (0x{msg_id:02X}): {verdict}")

        if by_message is not None:
            problems += check_host_reach(by_message, name, msg_id, implemented)

    return problems


def check_adv_burst(dev, reader) -> list[str]:
    """Set an advanced burst configuration and read it back."""
    print("\n[3/6] advanced burst round trip (0x78)")
    problems = []

    caps = request(dev, reader, 0, MESG_CONFIG_ADV_BURST_ID)
    if isinstance(caps, (int, type(None))):
        print(f"  FAIL: capabilities request refused ({caps})")
        return ["advanced burst: capabilities unreadable"]
    print(f"  OK: supported = {caps.hex()} "
          f"(max packet size code {caps[0]}, modes 0x{caps[1]:02X})")

    # [filler, enable, rf payload size, required modes, 0, 0, optional modes,
    #  0, 0] - the 8-byte required part of the structure, one byte in.
    wanted = bytes([ADV_BURST_MODE_ENABLE, ADV_BURST_MODES_SIZE_24_BYTES,
                    0x00, 0, 0, ADV_BURST_MODES_FREQ_HOP, 0, 0])
    code = command(dev, reader, MESG_CONFIG_ADV_BURST_ID, bytes([0x00]) + wanted)
    if code != RESPONSE_NO_ERROR:
        print(f"  FAIL: configuration refused, code {code}")
        return ["advanced burst: set refused"]
    print("  OK: 24-byte packets enabled, frequency hopping optional")

    got = request(dev, reader, 1, MESG_CONFIG_ADV_BURST_ID)
    if isinstance(got, (int, type(None))):
        print(f"  FAIL: configuration request refused ({got})")
        problems.append("advanced burst: config unreadable")
    else:
        # The reply drops the enable byte the command carried: it is
        # [size, required, 0, 0, optional, 0, 0, stall lsb, stall msb, retry].
        print(f"  .  read back {got.hex()}")
        if got[0] != wanted[1] or got[1] != wanted[2] or got[4] != wanted[5]:
            print(f"  FAIL: expected size={wanted[1]} required={wanted[2]} "
                  f"optional={wanted[5]}, got size={got[0]} required={got[1]} "
                  f"optional={got[4]}")
            problems.append("advanced burst: readback does not match")
        else:
            print("  OK: readback matches what was set")

    # Leave the stack as it was found - advanced burst is off by default.
    command(dev, reader, MESG_CONFIG_ADV_BURST_ID,
            bytes([0x00, ADV_BURST_MODE_DISABLE, ADV_BURST_MODES_SIZE_24_BYTES,
                   0, 0, 0, 0, 0, 0]))
    return problems


def check_sdu(dev, reader) -> list[str]:
    """Set two selective-data-update masks and read both back."""
    print("\n[4/6] selective data update round trip (0x7B, 0x7A)")
    problems = []

    masks = {
        0: bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08]),
        1: bytes([0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7]),
    }

    for index, mask in masks.items():
        code = command(dev, reader, MESG_SDU_SET_MASK_ID, bytes([index]) + mask)
        if code != RESPONSE_NO_ERROR:
            # A stack with fewer mask slots than this is not a bridge fault.
            print(f"  -- mask {index}: stack refused the set, code {code}")
            continue

        got = request(dev, reader, index, MESG_SDU_SET_MASK_ID)
        if isinstance(got, (int, type(None))):
            print(f"  FAIL: mask {index} unreadable ({got})")
            problems.append(f"sdu: mask {index} unreadable")
            continue

        # [mask index, mask[8]]. An off-by-one in either direction shows up
        # here as the index byte carrying mask[0] and the mask sliding down.
        if len(got) < 9:
            print(f"  FAIL: mask {index} reply is {len(got)} bytes, want 9: {got.hex()}")
            problems.append(f"sdu: mask {index} reply truncated")
        elif got[0] != index:
            print(f"  FAIL: mask {index} reply leads with 0x{got[0]:02X}, "
                  f"not the index: {got.hex()}")
            problems.append(f"sdu: mask {index} reply misframed")
        elif got[1:9] != mask:
            print(f"  FAIL: mask {index} read back {got[1:9].hex()}, "
                  f"set {mask.hex()}")
            problems.append(f"sdu: mask {index} readback does not match")
        else:
            print(f"  OK: mask {index} = {mask.hex()}, read back identically")

    # Attach a mask to a channel and detach it again. There is no request for
    # this one, so all it proves is that the message is accepted.
    if command(dev, reader, MESG_SDU_CONFIG_ID, bytes([0x00, 0x01])) == RESPONSE_NO_ERROR:
        print("  OK: channel 0 attached to mask 1")
    else:
        print("  -- channel 0 could not be attached to mask 1")
    if command(dev, reader, MESG_SDU_CONFIG_ID,
               bytes([0x00, INVALID_SDU_MASK])) == RESPONSE_NO_ERROR:
        print("  OK: channel 0 detached again")
    else:
        problems.append("sdu: channel could not be detached")
        print("  FAIL: channel 0 could not be detached")

    return problems


def check_event_filter(dev, reader) -> list[str]:
    """Set an event filter and read it back."""
    print("\n[5/6] event filter round trip (0x79)")

    wanted = 0x0006
    code = command(dev, reader, MESG_EVENT_FILTER_CONFIG_ID,
                   bytes([wanted & 0xFF, wanted >> 8]))
    if code != RESPONSE_NO_ERROR:
        print(f"  FAIL: filter refused, code {code}")
        return ["event filter: set refused"]

    got = request(dev, reader, 0, MESG_EVENT_FILTER_CONFIG_ID)
    problems = []
    if isinstance(got, (int, type(None))):
        print(f"  FAIL: filter unreadable ({got})")
        problems.append("event filter: unreadable")
    elif len(got) < 3 or (got[1] | (got[2] << 8)) != wanted:
        print(f"  FAIL: read back {got.hex()}, expected filter 0x{wanted:04X}")
        problems.append("event filter: readback does not match")
    else:
        print(f"  OK: filter 0x{wanted:04X} set and read back")

    # Filtering nothing is the default; leave it that way.
    command(dev, reader, MESG_EVENT_FILTER_CONFIG_ID, bytes([0x00, 0x00]))
    return problems


def check_encryption(dev, reader) -> list[str]:
    """Read the encryption capability, and round-trip the writes if they are in.

    The read side is always bridged. The writes are behind
    CONFIG_ANT_DONGLE_ENCRYPTION and are off in a shipping image, so finding
    them absent is a result, not a failure.

    The read is worth checking on its own: it is the other message whose reply
    leads with the byte that was requested, and a misframed one looks exactly
    like a stack that supports nothing.
    """
    print("\n[6/6] encryption (0x7D–0x7F)")

    got = request(dev, reader, ENCRYPTION_INFO_GET_SUPPORTED_MODE,
                  MESG_ENCRYPT_ENABLE_ID)
    if isinstance(got, (int, type(None))):
        print(f"  -- supported-mode request refused ({got})")
        return []
    if len(got) < 2 or got[0] != ENCRYPTION_INFO_GET_SUPPORTED_MODE:
        print(f"  FAIL: reply {got.hex()} does not lead with the info type")
        return ["encryption: reply misframed"]
    print(f"  OK: reply {got.hex()} (info type echoed, supported mode 0x{got[1]:02X})")

    # Does this build carry the write side? Setting the crypto ID is the
    # cheapest way to ask, and it is also the one write that can be read back.
    wanted_id = bytes([0xDE, 0xAD, 0xBE, 0xEF])
    code = command(dev, reader, MESG_SET_ENCRYPT_INFO_ID,
                   bytes([ENCRYPTION_INFO_SET_CRYPTO_ID]) + wanted_id)
    if not bridged(code):
        print("  -- writes are not compiled in "
              "(CONFIG_ANT_DONGLE_ENCRYPTION=n, the shipping default)")
        return []
    if code != RESPONSE_NO_ERROR:
        print(f"  FAIL: crypto ID refused, code {code}")
        return ["encryption: crypto ID set refused"]

    problems = []
    got = request(dev, reader, ENCRYPTION_INFO_GET_CRYPTO_ID,
                  MESG_ENCRYPT_ENABLE_ID)
    if isinstance(got, (int, type(None))):
        print(f"  FAIL: crypto ID unreadable ({got})")
        problems.append("encryption: crypto ID unreadable")
    elif len(got) < 5 or got[0] != ENCRYPTION_INFO_GET_CRYPTO_ID:
        print(f"  FAIL: crypto ID reply {got.hex()} is misframed")
        problems.append("encryption: crypto ID reply misframed")
    elif got[1:5] != wanted_id:
        print(f"  FAIL: crypto ID read back {got[1:5].hex()}, set {wanted_id.hex()}")
        problems.append("encryption: crypto ID readback does not match")
    else:
        print(f"  OK: crypto ID {wanted_id.hex()} set and read back")

    # Custom user data is the longest message a *host* sends: one byte of info
    # type plus 19 of payload, and nothing a host can put on the wire is
    # longer.
    #
    # It is not the longest message the parser sees, which is what this comment
    # used to claim. MAX_SIZE_VALUE is 38, not 20 - src/usb_ant_class.c:30-31
    # says so outright and both USB class files size their frame buffer from
    # the resulting 42. An advanced-burst data message reaches LEN 25 on its
    # own, and an extended receive message adds a flag byte and three appended
    # fields on top of a payload. Sizing a body buffer from 20 makes
    # handle_burst() see size = 19, fail its size % 8 check, and silently
    # reject every 24-byte burst packet. See MAX_SIZE_VALUE in tools/ant_wire.py.
    wanted_data = bytes(range(1, ENCRYPTION_USER_DATA_SIZE + 1))
    code = command(dev, reader, MESG_SET_ENCRYPT_INFO_ID,
                   bytes([ENCRYPTION_INFO_SET_CUSTOM_USER_DATA]) + wanted_data)
    if code != RESPONSE_NO_ERROR:
        print(f"  -- custom user data refused, code {code}")
    else:
        got = request(dev, reader, ENCRYPTION_INFO_GET_CUSTOM_USER_DATA,
                      MESG_ENCRYPT_ENABLE_ID)
        if isinstance(got, (int, type(None))) or len(got) < 20:
            print(f"  FAIL: custom user data unreadable ({got})")
            problems.append("encryption: user data unreadable")
        elif got[1:20] != wanted_data:
            print(f"  FAIL: user data read back {got[1:20].hex()}")
            problems.append("encryption: user data readback does not match")
        else:
            print(f"  OK: {ENCRYPTION_USER_DATA_SIZE} bytes of custom user data "
                  "set and read back")

    # A key has no getter, so all this proves is that the message is decoded
    # and the stack takes it.
    code = command(dev, reader, MESG_SET_ENCRYPT_KEY_ID,
                   bytes([0x00]) + bytes(ENCRYPTION_KEY_SIZE))
    print(f"  {'OK' if code == RESPONSE_NO_ERROR else '--'}: 128-bit key at "
          f"index 0, code {code}")

    # An RNG seed is refused on purpose - no size for it is documented.
    code = command(dev, reader, MESG_SET_ENCRYPT_INFO_ID,
                   bytes([ENCRYPTION_INFO_SET_RNG_SEED]) + bytes(4))
    if code == INVALID_MESSAGE:
        print("  OK: RNG seed refused, as intended")
    else:
        print(f"  -- RNG seed answered {code}, expected INVALID_MESSAGE")

    # Leave every channel unencrypted, whatever this build started with.
    for ch in range(8):
        command(dev, reader, MESG_ENCRYPT_ENABLE_ID,
                bytes([ch, ENCRYPTION_DISABLED_MODE, 0x00, 0x00]))
    print("  OK: encryption disabled on all eight channels")
    return problems


def check_radiant_security(dev, reader) -> list[str]:
    """Probe the RadiANT security messages, 0xF1-0xF4.

    NOT ANT protocol. A stock ANT stick answers INVALID_MESSAGE to all four and
    that is the correct answer, so an absent family is a result rather than a
    failure - exactly like the encryption writes above.

    Structured to leave the device as it was found. 0xF1 with no switches at
    all is the last thing this does, because a probe that left a channel
    transforming would make every later test on that channel read as garbage.
    """
    print("\n[7/7] RadiANT security (0xF1-0xF4)")

    ch = 0

    # 0xF4 first: it is the read arm, it is harmless, and its reply tells us
    # whether the family is compiled in without writing anything.
    got = request(dev, reader, ch, ant_sec.MESG_STATUS)
    if isinstance(got, (int, type(None))):
        print(f"  -- not compiled in (CONFIG_RADIANT_SEC_HOST_MESSAGES=n, "
              f"the shipping default); status request answered {got}")
        return []
    if len(got) < ant_sec.STATUS_LEN:
        print(f"  FAIL: status reply is {len(got)} bytes, want "
              f"{ant_sec.STATUS_LEN}: {got.hex()}")
        return ["radiant security: status reply misframed"]

    st = ant_sec.decode_status(got)
    print(f"  OK: channel {st.channel} switches={ant_sec.switches_str(st.switches)} "
          f"W={st.w} pages=0x{st.page_lo:02X}..0x{st.page_hi:02X} "
          f"epoch={st.epoch} verdict={ant_sec.verdict_str(st.verdict)}")

    problems = []

    # A key needs a channel ID, because the provisioning device number is bound
    # into the KDF and antr_sec_key_set() reads it from the channel rather than
    # taking it on the wire. Without one the answer is CHANNEL_IN_WRONG_STATE,
    # which is the honest reply and not a fault - so this reports it and moves
    # on rather than counting it as a problem.
    code = command(dev, reader, ant_sec.MESG_SET_KEY,
                   ant_sec.encode_set_key(ch, bytes(range(16))))
    if code == RESPONSE_NO_ERROR:
        print("  OK: 128-bit root key accepted")
    else:
        print(f"  -- key refused, code {code} (a channel ID must be set first: "
              "the provisioning device number is bound into the KDF)")

    # A key that is not 128 bits must be refused. This is the one write whose
    # rejection is the interesting outcome.
    code = command(dev, reader, ant_sec.MESG_SET_KEY,
                   bytes([ch, 192]) + bytes(16))
    if code in (INVALID_MESSAGE, INVALID_PARAMETER_PROVIDED,
                CHANNEL_IN_WRONG_STATE):
        print(f"  OK: a 192-bit key is refused, code {code}")
    else:
        print(f"  FAIL: a 192-bit key answered {code}")
        problems.append("radiant security: an unsupported key length was accepted")

    # Descriptor encryption is refused in v1: the descriptor has no counter and
    # therefore no nonce. Underspecified rather than merely unbuilt, which is
    # why it must not quietly succeed.
    code = command(dev, reader, ant_sec.MESG_CONFIG,
                   bytes([ch, ant_sec.SW_CONF | ant_sec.SW_DESC_CONF, 2,
                          0x01, 0x0F]))
    if code == RESPONSE_NO_ERROR:
        print("  FAIL: descriptor encryption was accepted; it has no nonce source")
        problems.append("radiant security: descriptor encryption accepted")
    else:
        print(f"  OK: descriptor encryption refused, code {code}")

    # An illegal W must be refused: W has to divide 256 and 65536 or the spread
    # MAC stops resynchronising after a lost packet.
    code = command(dev, reader, ant_sec.MESG_CONFIG,
                   bytes([ch, ant_sec.SW_AUTH, 3, 0x01, 0x0F]))
    if code == RESPONSE_NO_ERROR:
        print("  FAIL: W=3 was accepted")
        problems.append("radiant security: an illegal MAC window was accepted")
    else:
        print(f"  OK: W=3 refused, code {code}")

    # Page 0x00 is the descriptor. Securing it would make the node's own
    # descriptor unreadable and the node undiscoverable, from one host typo.
    code = command(dev, reader, ant_sec.MESG_CONFIG,
                   bytes([ch, ant_sec.SW_AUTH, 2, 0x00, 0x0F]))
    if code == RESPONSE_NO_ERROR:
        print("  FAIL: a page range starting at the descriptor was accepted")
        problems.append("radiant security: page range low bound not enforced")
    else:
        print(f"  OK: a page range including the descriptor is refused, code {code}")

    # There is no read arm for a key anywhere, and a request for one must be
    # answered as though the device had never heard of the message.
    got = request(dev, reader, ch, ant_sec.MESG_SET_KEY)
    if isinstance(got, (int, type(None))):
        print(f"  OK: a key read is refused, code {got}")
    else:
        print(f"  FAIL: 0xF2 answered a read with {got.hex()}")
        problems.append("radiant security: a root key was readable")

    # Leave the channel as it was found: no switches, default range.
    command(dev, reader, ant_sec.MESG_CONFIG,
            ant_sec.encode_config(ch, 0, 2, 0x01, 0x0F))
    print("  OK: channel 0 left with no transforms on")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="match a device whose serial ends with this")
    parser.add_argument("--port", help="talk to a UART build over this serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--radiant-security", action="store_true",
                        help="also probe the RadiANT security messages "
                             "(0xF1-0xF4). Off by default because a stock ANT "
                             "stick answers INVALID_MESSAGE to all four and "
                             "the noise is not informative.")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    dev = open_device(not args.quiet, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)

    print("\n[1/6] reset and capabilities")
    if not reset_stack(dev, reader):
        print("  FAIL: no startup message after reset")
        close_device(dev)
        return 1
    caps = request(dev, reader, 0, MESG_CAPABILITIES_ID)
    if isinstance(caps, (int, type(None))) or len(caps) < 9:
        print(f"  FAIL: capabilities unreadable ({caps})")
        close_device(dev)
        return 1
    print(f"  OK: {caps.hex()}")

    by_message = load_host_api()

    problems = []
    problems += check_coverage(dev, reader, caps[6], by_message)
    problems += check_adv_burst(dev, reader)
    problems += check_sdu(dev, reader)
    problems += check_event_filter(dev, reader)
    problems += check_encryption(dev, reader)
    if args.radiant_security:
        problems += check_radiant_security(dev, reader)

    close_device(dev)

    print()
    if problems:
        print("FAILED:")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print("PASS: advertised features are bridged, and every set/get round-trips")
    return 0


if __name__ == "__main__":
    sys.exit(main())
