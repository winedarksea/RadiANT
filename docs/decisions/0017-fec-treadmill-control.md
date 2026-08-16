# ADR 0017 — FE-C page 51 is the treadmill incline command, and no private page is invented

Status: accepted, 2026-08-15
Supersedes: nothing
Amends: [ADR 0003](0003-naming-trademark-and-usb-identity.md) (the device-name
clause at the bottom of this record)

Checked by: `radiant/tests/src/test_profile_fec_tx.c` for the grade encoding and
the command-status obligation; `.github/workflows/build-treadmill` for the
configuration; nothing for the certification note, which is a process fact.

---

## Context

`apps/treadmill` needs a way for a controller to set the deck's incline. Three
questions had to be answered before the first line of it was written, and each
had a plausible wrong answer that would have shipped.

## Amendment to ADR 0003: the device name

[ADR 0003](0003-naming-trademark-and-usb-identity.md) is the existing home for
naming and trademark questions, and this is one, so it is amended here rather
than given a record of its own.

**Some fitness apps key compatibility off a BLE device name.** That makes a
plausible-looking default name an invitation to impersonate somebody's product,
and it makes a *neutral* default a real interoperability cost — an app that only
recognises names it knows will not recognise this machine.

The decision is the one ADR 0003 already makes for the USB identity, applied to
a new surface:

- **The application ships no trademarked default name.** The default is
  `"RadiANT Treadmill"`.
- **`CONFIG_BT_DEVICE_NAME` is exposed** so an end user can set whatever they
  like on their own initiative. What a user does to their own machine is their
  decision; what this project ships as a default is ours.
- **No product name of anybody else's appears in a `default` string, in a
  comment as an example, or in a worked example in `docs/`.** That last clause
  is the one most easily broken by accident, which is why it is written down.

---

## Consequences

- A conforming FE-C controller can set a treadmill's grade with no
  RadiANT-specific knowledge, and reads the result back on a mandatory page.
- An FTMS client can do the same, and the two cannot disagree, because the state
  is one struct member and the conversion is one tested function.
- A machine with a fixed deck is expressible: `-ENOTSUP`, both capability bits
  clear, and two well-formed refusals on the air.
- The certification note is discoverable by anybody who goes looking for why a
  treadmill is doing something the spec reserves for trainers.
- If a future revision of the FE-C profile allocates a treadmill-specific
  control page, this record is what says the current choice was deliberate and
  what would have to change: the page number in `profile_fec_tx.h`, the decode
  branch in `apps/treadmill/src/main.c`, and nothing else — the state, the
  actuator call and the FTMS side are all downstream of the decode.
