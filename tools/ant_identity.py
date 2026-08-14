#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Device-number provisioning: the three identity tiers, and a re-roll command.

    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe tools\\ant_identity.py show
    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe tools\\ant_identity.py provision --tier 0
    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe tools\\ant_identity.py reroll

An ANT+ sensor broadcasts a fixed 16-bit device number forever, and that number
is typically a factory serial - so it ties the node to a purchase record, a
warranty, and a previous owner, and anyone in range can follow it across
sessions and locations. Encryption never touched this: the device number is
outside the payload because the receiver's hardware filter has to match on it.

`X_PRIV`, the continuous per-epoch rotation this project first specified, is
withdrawn - see docs/decisions/0006-security-v1-scope-and-x-priv-withdrawal.md,
which records why so that it is not re-proposed. What replaces it is
provisioning, and the whole of the cost argument is that A ROTATION NEVER
HAPPENS WHILE A CHANNEL IS OPEN:

    Tier 0  random at first provisioning, stable thereafter.  ON BY DEFAULT.
            Zero UX change - no receiver ever loses a sensor - which is the
            entire reason it can be the default. Not a security feature: a
            correction. Applies to ANT+ compatible device types too, which is
            the population with a real anonymity set.

    Tier 1  re-roll on explicit user action. Opt-in. Receivers must re-pair,
            but the user just asked for a new identity. This is the tier that
            covers "I am selling this".

    Tier 2  re-roll at every power-up. Opt-in, RadiANT device types only. It
            answers the stalking case and it SILENTLY LOSES STANDARD
            RECEIVERS - a Garmin head unit re-pairs every session. A keyed
            RadiANT receiver does not, because it resolves identity by whether
            X_AUTH verifies rather than by a number.

This module is the provisioning half. It has no radio in it: a device number is
set by the host (MESG_SET_CHANNEL_ID) or by the node application, so the tiers
are a rule about how that number is CHOSEN, not a protocol feature. Nothing in
radiant implements them and nothing needs to.

Standard library only, and `secrets` rather than `random`, because a device
number generated from a seeded PRNG in a shipped provisioning script is a
device number an attacker can enumerate.
"""

from __future__ import annotations

import argparse
import datetime
import json
import secrets
import sys
from pathlib import Path

#: 0 is the wildcard on an ANT channel ID, so it is not a device number. The
#: range is 1..65535 and that is the whole of it.
DEVICE_NUMBER_MIN = 1
DEVICE_NUMBER_MAX = 0xFFFF

#: Birthday bound on a 16-bit field: collisions become likely around 300
#: devices in range. A constraint ANT+ already lives with (device type and
#: transmission type have to match too).
COLLISION_LIKELY_AROUND = 300

TIERS = (0, 1, 2)

TIER_DESCRIPTIONS = {
    0: "random at first provisioning, stable thereafter (default)",
    1: "re-roll on explicit user action (opt-in)",
    2: "re-roll at every power-up (opt-in, RadiANT device types only)",
}

#: docs/radiant-security.md section 5.4 and docs/radiant-telemetry.md.
#: Independent of every switch and tier, and the cheapest privacy rule here.
PRIVACY_SERIAL_NOT_SUPPLIED = 0xFFFFFFFF
PRIVACY_MANUFACTURER_ID = 0x00FF   # the "development" id, not somebody else's
PRIVACY_MODEL_NUMBER = 0x0001
PRIVACY_HW_REVISION = 1
PRIVACY_SW_REVISION = 1

DEFAULT_PROVISIONING_FILE = Path.home() / ".radiant" / "identity.json"


def random_device_number() -> int:
    """A uniformly random device number in 1..65535."""
    return secrets.randbelow(DEVICE_NUMBER_MAX) + DEVICE_NUMBER_MIN


class Identity:
    """One node's provisioned identity.

    `base_device_number` is FIXED AT PROVISIONING TIME, distinct from
    `device_number`: on a Tier 2 node the on-air number re-rolls every
    power-up but the base one doesn't, since it's bound into key derivation
    (`kdf_block`) - giving same-type sensors under one household root their
    own keys. A receiver learns it at pairing, never from the air.
    """

    def __init__(self, device_number: int, base_device_number: int, tier: int,
                 created: str, privacy_pages: bool = True):
        if not DEVICE_NUMBER_MIN <= device_number <= DEVICE_NUMBER_MAX:
            raise ValueError(f"device number {device_number!r} is outside "
                             f"1..65535 (0 is the wildcard)")
        if not DEVICE_NUMBER_MIN <= base_device_number <= DEVICE_NUMBER_MAX:
            raise ValueError(f"base device number {base_device_number!r} is "
                             f"outside 1..65535")
        if tier not in TIERS:
            raise ValueError(f"tier {tier!r} is not one of {list(TIERS)}")
        self.device_number = device_number
        self.base_device_number = base_device_number
        self.tier = tier
        self.created = created
        self.privacy_pages = privacy_pages

    @classmethod
    def new(cls, tier: int = 0, privacy_pages: bool = True,
            now: str | None = None) -> Identity:
        number = random_device_number()
        stamp = now or datetime.datetime.now(
            datetime.UTC).replace(microsecond=0).isoformat()
        return cls(number, number, tier, stamp, privacy_pages)

    def to_dict(self) -> dict:
        return {
            "device_number": self.device_number,
            "base_device_number": self.base_device_number,
            "tier": self.tier,
            "created": self.created,
            "privacy_pages": self.privacy_pages,
        }

    @classmethod
    def from_dict(cls, data: dict) -> Identity:
        return cls(
            int(data["device_number"]),
            int(data.get("base_device_number", data["device_number"])),
            int(data.get("tier", 0)),
            str(data.get("created", "")),
            bool(data.get("privacy_pages", True)),
        )

    def reroll(self) -> Identity:
        """A new on-air number. THE BASE NUMBER DOES NOT MOVE.

        Tier 1 and Tier 2 both land here; only who calls it (user vs. power
        cycle) differs. The expensive part of rotation was never the
        rolling, it was rolling WHILE A CHANNEL WAS OPEN.
        """
        self.device_number = random_device_number()
        return self

    def on_boot(self) -> Identity:
        """Apply the tier's power-up rule. Tier 2 re-rolls; 0 and 1 do not."""
        if self.tier == 2:
            self.reroll()
        return self

    def __repr__(self) -> str:
        return (f"Identity(device_number={self.device_number}, "
                f"base={self.base_device_number}, tier={self.tier}, "
                f"privacy_pages={self.privacy_pages!r})")


def load(path: Path) -> Identity | None:
    if not path.exists():
        return None
    return Identity.from_dict(json.loads(path.read_text(encoding="utf-8")))


def save(path: Path, identity: Identity) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(identity.to_dict(), indent=2) + "\n",
                    encoding="utf-8")


def provision(path: Path, tier: int = 0, privacy_pages: bool = True) -> Identity:
    """Tier 0, as a function: create once, then keep.

    Idempotent on purpose. A provisioning step that generates a new number
    every time it runs is Tier 2 wearing Tier 0's name, and the difference is
    whether every receiver in the house keeps working.
    """
    existing = load(path)
    if existing is not None:
        return existing
    identity = Identity.new(tier=tier, privacy_pages=privacy_pages)
    save(path, identity)
    return identity


def _cmd_show(args) -> int:
    identity = load(args.file)
    if identity is None:
        print(f"no identity provisioned at {args.file}")
        print(f"run: {Path(sys.argv[0]).name} provision")
        return 1
    print(f"device number      : {identity.device_number} "
          f"(0x{identity.device_number:04X})")
    print(f"base device number : {identity.base_device_number} "
          f"(0x{identity.base_device_number:04X})   <- bound into the key "
          f"derivation, never re-rolled")
    print(f"tier               : {identity.tier} - "
          f"{TIER_DESCRIPTIONS[identity.tier]}")
    print("privacy pages      : %s"
          % ("yes - page 81 serial 0xFFFFFFFF, page 82 suppressed, page 80 "
             "generic" if identity.privacy_pages else "NO"))
    print("provisioned        : %s" % (identity.created or "unknown"))
    return 0


def _cmd_provision(args) -> int:
    before = load(args.file)
    identity = provision(args.file, tier=args.tier,
                         privacy_pages=not args.no_privacy_pages)
    if before is not None:
        print("already provisioned; leaving it alone (use `reroll` to change "
              "it)")
    if identity.tier != args.tier:
        identity.tier = args.tier
        save(args.file, identity)
    print(f"device number {identity.device_number} "
          f"(0x{identity.device_number:04X}), tier {identity.tier}")
    return 0


def _cmd_reroll(args) -> int:
    identity = load(args.file)
    if identity is None:
        print(f"nothing to re-roll: no identity at {args.file}")
        return 1
    was = identity.device_number
    identity.reroll()
    save(args.file, identity)
    print(f"device number {was} (0x{was:04X}) -> "
          f"{identity.device_number} (0x{identity.device_number:04X})")
    print("")
    print("EVERY RECEIVER PAIRED TO THIS NODE MUST RE-PAIR. That is Tier 1's")
    print("whole cost, and it is acceptable only because you just asked for")
    print("it. A keyed RadiANT receiver is the exception: it re-acquires by")
    print("checking whether X_AUTH verifies, not by device number.")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--file", type=Path, default=DEFAULT_PROVISIONING_FILE,
                        help="provisioning record (default: %(default)s)")
    sub = parser.add_subparsers(dest="command", required=True)

    show = sub.add_parser("show", help="print the provisioned identity")
    show.set_defaults(func=_cmd_show)

    prov = sub.add_parser("provision",
                          help="create an identity if there is not one already")
    prov.add_argument("--tier", type=int, choices=TIERS, default=0,
                      help="; ".join(f"{tier} = {TIER_DESCRIPTIONS[tier]}"
                                     for tier in TIERS))
    prov.add_argument("--no-privacy-pages", action="store_true",
                      help="keep broadcasting a real serial number in page 81. "
                           "A 32-bit serial in the clear defeats tiers 1 and 2 "
                           "outright, so this is here to be explicit rather "
                           "than to be used")
    prov.set_defaults(func=_cmd_provision)

    reroll = sub.add_parser("reroll",
                            help="Tier 1: take a new on-air device number")
    reroll.set_defaults(func=_cmd_reroll)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
