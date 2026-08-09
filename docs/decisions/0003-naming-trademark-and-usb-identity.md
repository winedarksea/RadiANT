# 0003 — The RadiANT name, the trademark posture, and the USB identity

Checked by: nothing — treat as narrative, with one exception. The USB
identifiers this ADR decides to keep are asserted on the wire by
`tools/ant_probe.py`, which will not find a device that stopped presenting
them; and the descriptor strings live in `src/usb_ant_class.c` and
`src/usb_ant_class_next.c`. The trademark position and the open risk below are
judgements, and no test can hold them.

- **Status:** Accepted for the name and the qualifier. **The USB identity is
  accepted as a decision and recorded as an open risk** — accepted is not the
  same as resolved, and this file must not be read as claiming otherwise.
  The driver-free host access experiment is **Proposed**, gated, not shipped.
- **Date:** 2026-08-08
- **Related:** [`../../NOTICE`](../../NOTICE),
  [0002 clean-room policy](0002-clean-room-policy.md),
  [0004 licence](0004-license-apache-2-0.md)

## Context

Three separate questions get conflated whenever this subject comes up, and the
main value of this ADR is keeping them apart:

1. **What is the project called**, given that any name for an ANT-compatible
   thing tends to contain the ANT mark.
2. **How is it described**, so that "compatible with" is never heard as
   "endorsed by".
3. **What does it claim to be on the USB bus**, which is a much harder question
   than the first two and does not become easier by answering them.

ANT and ANT+ are trademarks of Garmin Canada Inc. (formerly Dynastream
Innovations Inc.). The ANT+ Alliance's membership and certification programs
closed on 30 June 2025, which removes the body that used to grant permission
but removes none of the marks.

## Decision

### The name is RadiANT, and the qualifier is mandatory

RadiANT is the public name for the whole effort and for the extension family —
the optional security switches, the generic telemetry profile, the new device
types. `ant_core` remains the C module name.

The name contains the mark, so it carries the same posture as the descriptor
strings: **always described as compatible, never as endorsed.** On first use in
any user-facing text — README, release notes, the project description, a
website, a talk — it appears with its qualifier:

> RadiANT, an open-source, clean-room implementation compatible with the ANT+
> specifications.

Not "an ANT+ dongle". Not "ANT+ compatible" as a badge. Not anything that could
be read as a certification claim, because the certification program that would
have issued one no longer exists and never issued one to this project.

The use is **nominative**: naming the specification an implementation
interoperates with is the only way to describe an interoperable implementation
at all, and Apache-2.0 §6 explicitly declines to grant trademark rights while
carving out "reasonable and customary use in describing the origin of the
Work". The qualifier is what keeps the use inside that carve-out. `NOTICE`
carries the full statement and is the authoritative text.

### `0FCF:1009` and the Dynastream descriptor strings are kept

Firmware from this repository enumerates as USB vendor `0x0FCF`, product
`0x1009` — the Garmin/Dynastream ANT USB-m stick — and reports Dynastream's
descriptor strings.

**This stays, and the reason is that driver matching is the only thing making
any of it work.**

- On Windows, libusb-win32 binds by VID/PID. The `.inf` names the identifiers
  and nothing else. A device presenting different ones binds to nothing.
- Every host application that exists — Zwift, openant, Garmin's own utilities,
  every third-party trainer app — locates an ANT stick by scanning for exactly
  these identifiers. None of them has a discovery mechanism that could find a
  device using a different pair, because there was never a reason to build one.
- Windows caches its verdict about a device per VID/PID/`bcdDevice` in
  `usbflags`. Changing the identity does not merely fail to be found; it can
  produce a host that has cached a stale answer about a device that no longer
  exists.

So changing the PID is not a cosmetic concession to a trademark concern. **It
breaks the product**, comprehensively, on every host, with no migration path
that does not require every application vendor to ship a change. The
identifiers exist here for host driver-matching compatibility and for no other
purpose.

### And that is recorded as an open risk, not a resolved question

The temptation is to treat the naming answer as covering this too. It does not.
A qualifier on a project name is a description; a USB descriptor is an
assertion made by a device to a host, and it asserts Dynastream's identity.
Those are different acts and the second is meaningfully harder to defend.

Recording it honestly:

- **What we rely on:** that presenting these identifiers is a functional
  interoperability measure rather than an attempt to pass the device off as
  Garmin's; that no packaging, marketing, documentation or user-visible text
  claims Garmin origin; that `NOTICE` states plainly whose marks these are and
  why the identifiers are present; and that no unit is sold as a Garmin
  product.
- **What is unresolved:** whether that is *sufficient*. Nobody has cleared it,
  no counsel has opined on it, and the ANT+ Alliance that might once have been
  asked is gone. The honest state is "we have a reason, we have written it
  down, and we have not been told it is enough."
- **What would change the answer:** a request from Garmin; a decision to sell
  hardware commercially at any volume; or a host ecosystem that stops requiring
  the identifiers — which is what makes the experiment below interesting for
  more than convenience.
- **What we will not do:** pretend the trademark statement in `NOTICE`
  discharges this. It discharges the naming question. This one stays open, in
  writing, until something actually resolves it.

One related fact, stated so it is not mistaken for a motivation: removing
`libant.a` removes the **$0.08 per unit royalty**. That is a consequence of the
rebuild. It is not a reason to change the USB identity and does not bear on
this decision.

### Driver-free host access: a composite device (Proposed, gated)

The most annoying part of using this dongle on Windows is the driver ceremony —
Zadig, choose libusb-win32 specifically and not WinUSB or libusbK, then work
around the `usbflags` cache when Windows declines to re-query. Every new user
pays it, and every new tool inherits it.

There is a way out that does not touch the legacy identity at all: make the
device **composite**.

- **Interface 0 stays byte-identical** to what ships today: the same two bulk
  endpoints, the same descriptors, the same class/subclass/protocol, binding
  the same libusb0 driver for Zwift and every existing tool. Byte-identical is
  the requirement, not "equivalent" — the whole value of the approach is that
  the shipping path is provably unchanged.
- **A second vendor interface** is added, carrying **MS OS 2.0 descriptors**
  (the BOS descriptor, the platform capability GUID, the compatible-ID
  `WINUSB`). Windows reads those during enumeration and binds WinUSB to that
  interface automatically, with no INF and no Zadig. New tools open the WinUSB
  interface and need no driver ceremony whatsoever.

Three feasibility gates, all of which must pass before this ships:

1. **The legacy interface still binds libusb0.** Verified against a machine
   with the existing driver already installed *and* a clean machine with the
   INF from `archive/drivers/`. A composite device changes the hardware ID
   Windows matches against (`USB\VID_0FCF&PID_1009&MI_00` rather than
   `USB\VID_0FCF&PID_1009`), which is exactly the kind of detail that turns
   this from a one-day change into a regression.
2. **Zwift still enumerates and pairs.** Not "opens the device" — pairs a
   sensor and holds it. Zwift is the acceptance criterion for anything touching
   USB, because it is the host nobody here can debug or patch.
3. **The `usbflags` cache is accounted for.** Windows caches descriptor
   verdicts per VID/PID/**`bcdDevice`**. An experimental composite build
   presenting the same `bcdDevice` as a shipped dongle can poison the cache on
   any machine it is plugged into — including for the shipped dongles that
   machine sees later, which is a failure the user cannot diagnose and we
   cannot see. **The experiment bumps `bcdDevice`**, so its cache entry is
   distinct from the shipping one. This is not optional and is not a detail:
   `scripts/reset_usb_cache.ps1` exists because this cache has already cost
   this project time once.

**Any regression reverts it.** Not "we investigate" — reverts. The shipping
configuration is the one Zwift works with, and a driver convenience for new
tools is not worth a nonzero probability of breaking it. The experiment is
worth running because if it works it removes the single worst part of the user
experience; it is worth reverting instantly because the thing it risks is the
only thing that matters.

## Consequences

**Good.**

- The name is usable, defensible in its nominative framing, and carries its own
  disclaimer wherever it appears.
- The product keeps working: driver binding, Zwift, openant, Garmin utilities,
  and everything else that scans for `0FCF:1009`.
- The hard question is written down as open rather than quietly assumed
  resolved, so a future decision has the reasoning available instead of having
  to reconstruct it.
- If the composite experiment passes its gates, new tooling needs no driver
  install on Windows at all, and the legacy path is provably untouched.

**Costs, accepted.**

- The qualifier is verbose and must appear on first use in every user-facing
  text. It will feel like boilerplate. It is the cheapest part of the whole
  posture.
- The USB identity remains a real, unquantified risk for as long as the project
  exists in this form. Recording it does not reduce it.
- The composite experiment costs bench time on machines in more than one state
  (clean, driver-installed, cache-poisoned), and gate 3 in particular can only
  be checked by deliberately reproducing a failure mode that is unpleasant to
  reproduce.
