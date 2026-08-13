# Third-party components

Checked by: nothing — treat as narrative. Nothing here is machine-verified.
Two facts in the table below *are* duplicated elsewhere and will rot if they
drift: the sdk-ant repository and revision appear in `west.yml` and as
`SDK_ANT_REVISION` in `.github/workflows/build.yml`, and the URLs are only as
live as the weekly link check makes them. Everything else is a legal and
policy position, which no test can assert.

Every component this project touches, what its licence is, and what this
project's posture toward it is. "Posture" is the load-bearing column: several
of these are things we deliberately read, or deliberately do not read, or
deliberately do not redistribute, and the reason is not inferable from the
licence alone.

The clean-room rules that this table is an inventory *for* live in
[`decisions/0002-clean-room-policy.md`](decisions/0002-clean-room-policy.md).
Where the two disagree, the ADR wins.

---

## Summary

| Component | Licence | Redistributed here? | May we read it? |
|---|---|---|---|
| `ant-nrfconnect/sdk-ant` | Proprietary, not redistributable | No | Only the `sdk-ant-shim` role |
| `libant.a` (inside sdk-ant) | Proprietary binary, $0.08/unit royalty | No | **No.** Never disassembled |
| Zephyr RTOS | Apache-2.0 | No (fetched by west) | Yes, freely |
| nRF Connect SDK (`sdk-nrf`, `nrfxlib`) | Nordic 5-clause BSD / Apache-2.0 / proprietary blobs | No (fetched by west) | Yes, freely |
| rtl_433 | GPL-3.0-or-later | No | Facts only, never expression |
| ANT Message Protocol Rev 5.1 (D00000652) | Free download, Garmin copyright | No — pointers only | Yes, freely |
| ANT+ device profile documents | Not freely redistributable, Garmin copyright | No | Only for `src/profiles/`, `tools/ant_pages.py`, profile docs |
| Garmin libusb-win32 driver package | Garmin package wrapping LGPL libusb0 | **No** | N/A |
| pyusb | BSD-3-Clause | No (pip) | Yes, freely |
| RFC 7748 (X25519) | IETF standard, free | N/A — clean-room, nothing vendored | Yes, freely |

---

## `ant-nrfconnect/sdk-ant`

**What it is.** Nordic Semiconductor's ANT support for the nRF Connect SDK: a
Zephyr module supplying the ANT headers (`include/`, `init/`), the ANT+ profile
libraries that `sim/` is built from, and the prebuilt radio binary `libant.a`.
Pinned at `v2.1.0`.

**Licence.** Proprietary. Access to the repository is granted per GitHub
account under Garmin's terms for the SDK — which is why
`.github/workflows/build.yml` needs a personal access token rather than the
automatic `GITHUB_TOKEN`, and why `west.yml` puts it in a disabled group. The
licence text (`LICENSE.txt` in that repository) explicitly forbids
reverse-engineering the binary.

**Posture.** *Optional backend, never vendored.* sdk-ant remains a first-class
selectable backend — see
[`decisions/0001`](decisions/0001-backend-selection-and-release-default.md) —
because it is the reference the clean-room stack is measured against and the
only configuration that can compile-assert our protocol constants against
Garmin's own header values. What we will not do is copy any of it into this
repository. Vendoring the headers would violate the licence, destroy the
clean-room defence, and convert a public repository into a legal problem; the
`#include "ant_interface.h"` in `src/ant_serial_bridge.c` is meant to end up
*deleted*, not *duplicated*.

**Read scope.** Only the agent or contributor writing
`src/ant_radio_sdk_ant.c` may open this tree. Everyone writing `radiant_core/**`
is barred from it. That split is the clean-room boundary; see
[`decisions/0002`](decisions/0002-clean-room-policy.md).

## `libant.a`

**What it is.** One object, `libant_obfuscated.elf`, inside sdk-ant's archive:
the ANT link layer, 30,768 B of flash and 2,389 B of RAM in the shipping
nRF52840 image. 271 global functions, 210 of them name-stripped to hashes; 59
public `antlib_*` entry points.

**Licence.** Proprietary binary, distributed under the sdk-ant terms, carrying
a **$0.08 per unit royalty** on shipped devices. Removing it removes the
royalty — which is a consequence of the rebuild, not its motivation.

**Posture.** *Disassembly is forbidden absolutely.* Not "discouraged", not
"only if we need to": forbidden, permanently, sunset or no sunset. One person
disassembling it once contaminates the rebuild retroactively and there is no
way to prove afterwards that it did not happen. Public symbol names obtained
from `nm` and archive membership from `ar t` are inventory facts about a file
and have been recorded; the contents of the code have not been examined and
must not be. See [`decisions/0002`](decisions/0002-clean-room-policy.md).

## Zephyr RTOS

**Licence.** Apache-2.0.

**Posture.** Upstream dependency, fetched by `west update`, not redistributed
by this repository. Same licence as this project, so there is no compatibility
question and no notice obligation beyond the one Apache-2.0 already imposes.
Read freely — it is the platform this firmware is written against.

## nRF Connect SDK (`sdk-nrf`, `nrfxlib`, HALs)

**Licence.** Mixed. `sdk-nrf` is predominantly under the Nordic 5-clause BSD
licence (`LicenseRef-Nordic-5-Clause`) with Apache-2.0 portions; `nrfxlib`
ships precompiled binaries (MPSL, the SoftDevice controller) under Nordic's
proprietary terms.

**Posture.** Upstream dependency, fetched by `west update`, not redistributed.
Read freely. One nuance worth recording: the planned MPSL timeslot backend
(`radiant_radio_nrf_mpsl.c`) links a closed Nordic binary — but *only under that
backend*. The default direct-peripheral backend stays free of it, which is one
of the reasons direct is the default rather than a fallback.

## rtl_433

**What it is.** A GPL software-defined-radio decoder suite. Its
`src/devices/ant_antplus.c` decodes ANT+ frames off the air and is, together
with the expired patent US8855246B2, the best public description of the on-air
frame format.

**Licence.** **GPL-3.0-or-later.** Incompatible with shipping Apache-2.0
firmware.

**Posture.** *Facts yes, expression never.* The boundary is the one copyright
law actually draws: the centre frequency, the network address bytes, the CRC
polynomial and initial value, the absence of whitening, the preamble rule and
the field order are **facts about a radio protocol** and are not copyrightable.
They are recorded, with provenance tags, in `docs/ant-radio-link.md`. rtl_433's
*code* — its loop structure, its variable naming, its bit-extraction
expressions, its comments — is copyrightable and must not appear, verbatim or
paraphrased, in any file in this repository.

**The escape hatch, stated in advance so nobody improvises one.** If some file
genuinely must derive from rtl_433's expression rather than from its facts, it
does not get quietly relicensed and it does not get "cleaned up" until it looks
different. It lives in a separate, clearly GPL-headered directory, is excluded
from the shipped image and from anything linked into it, and is used only as a
host-side analysis tool. Deciding that after the fact is not possible: there is
no cheap way to unwind a GPL derivation once it is inside an Apache-2.0 binary.
See [`decisions/0002`](decisions/0002-clean-room-policy.md).

## ANT Message Protocol and Usage, Rev 5.1 (D00000652)

**What it is.** Garmin's specification of the host↔dongle *serial* protocol —
the `0xA4`-framed messages `src/ant_serial_bridge.c` implements. It is the
document that makes this project possible: it describes exactly the layer we
must be byte-compatible at, and it is free.

**Licence / availability.** Free download from thisisant.com, no login and no
agreement to sign. Garmin retains copyright in the document.

**Posture.** *Permitted source, pointers only.* Free to download is not the
same as ours to republish, so the PDF is not committed. What is committed is
`archive/specs/README.md` — title, document number, URL, SHA-256, retrieval
date and a Wayback snapshot we submitted ourselves — plus our own derived prose
in `docs/`. The derived prose is the durable artifact; the link is the thing
most likely to rot, which is what the weekly link check exists for.

## ANT+ device profile documents

**What they are.** The per-device-type specifications: heart rate (`0x78`),
bicycle power (`0x0B`), speed and cadence (`0x79`), fitness equipment (`0x11`),
and the common pages. They define the 8-byte page layouts that make a sensor
interoperable.

**Licence / availability.** Not freely redistributable. Obtaining a copy has
been free since January 2025 and no longer requires certification; this
project holds one from other work. Garmin retains copyright.

**Posture.** *Permitted, but only in three places.* These documents may be used
for `src/profiles/`, for `tools/ant_pages.py`, and for the profile
documentation — and **nowhere in `radiant_core/**`**. The link layer stays strictly
clean-room; page semantics do not need to be, because a page layout is a fact
about an interoperability target with no program left behind it. Use the
tables and the semantics; copy no prose. This is the "graded, not absolute"
posture, and the grading rationale is written into
[`decisions/0002`](decisions/0002-clean-room-policy.md) precisely so it is not
re-argued file by file.

Unofficial mirrors of these documents found on the web are listed as
references where useful but are not committed: doing so would combine
redistribution risk with clean-room contamination in one act.

## Garmin's libusb-win32 driver package

**What it is.** The Windows driver package Garmin ships for ANT USB sticks. It
wraps libusb0 (LGPL) but the package as distributed is Garmin's.

**Posture.** **Not redistributed.** libusb0's LGPL does not make the vendor
package redistributable, and no reading of it does. What this repository
carries instead is more useful anyway: `archive/drivers/README.md` recording
the SHA-256, both provenance paths and a Wayback snapshot; **our own**
`ant_libusb_win32.inf`, written from scratch, binding libusb0 to
`VID_0FCF&PID_1009/1008/1004` — an INF is a table of identifiers, and ours
covers our device by construction; and `install-notes.md` capturing the two
facts that cost real time to learn (choose libusb-win32, not WinUSB or libusbK;
and the Windows `usbflags` cache means the host may never re-query a changed
descriptor).

The driver-free alternative — a second vendor interface carrying MS OS 2.0
descriptors so Windows auto-binds WinUSB with no install step — is an
experiment recorded in
[`decisions/0003`](decisions/0003-naming-trademark-and-usb-identity.md), not a
shipped feature.

## pyusb

**Licence.** BSD-3-Clause.

**Posture.** Host-side test dependency for everything in `tools/`, installed by
`pip`, not vendored. Pure Python: it imports fine with no libusb present, which
is why the `host-tests` CI job can run the page and simulator tests on a runner
with no USB device attached.

## X25519 — the third-party component that is deliberately *not* here

**There isn't one, and that is a decision rather than an omission.**

`radiant_core/src/ext/radiant_x25519.c` sits in an `ext/` directory, which
normally means vendored code. It is not: it is clean-room, written from
[RFC 7748](https://www.rfc-editor.org/rfc/rfc7748) and verified against that
RFC's own published test vectors. The directory name marks it as a *standard
algorithm rather than RadiANT protocol logic* — the thing a hardware PKA
backend replaces wholesale — and not as somebody else's source.

Vendoring is normally the better call for a primitive this well studied: fewer
eyes on new cryptographic code is strictly worse, and a widely-deployed
public-domain implementation carries thousands of audits this one does not. The
plan for the phase that added it said "vendored public-domain X25519 with a
`Provenance:` line", and that was the right instinct.

What made it the wrong outcome here is what a `Provenance:` line is *for*. Its
whole value is that it is a checkable claim about where bytes came from — that
is the load-bearing element of [`decisions/0002`](decisions/0002-clean-room-policy.md),
and the reason the policy is written down before there is code to argue about.
A file reconstructed from memory and labelled "vendored public-domain" would
carry a provenance claim nobody could verify, sitting in the same repository as
claims that must hold up under scrutiny. One unverifiable line devalues every
other one.

So the file says what it is. If a genuine vendored implementation is wanted
later, it drops in behind `radiant_sec_x25519()` and nothing above it changes —
that is what the seam is for, and it is the same seam a CC310 or CRACEN PKE
backend would use.

**Read-scope note.** RFC 7748 is a free, published IETF standard with no
access restriction and no proprietary encumbrance, so it falls under the same
"interoperability specification" reasoning that permits ANT+ page layouts: a
public standard describing a mathematical construction, where there is no design
freedom to infringe.

---

## Our own licence

RadiANT is Apache-2.0. Every source file carries
`SPDX-License-Identifier: Apache-2.0`; every `radiant_core/*.c` additionally
carries a `Provenance:` line naming which of the permitted sources above it was
written from. `tools/ant_pages.py` and the sibling `zephyr_aerosense` project
are on the same licence, because a header shared between two projects under two
licences only gets more expensive. The reasoning is in
[`decisions/0004`](decisions/0004-license-apache-2-0.md); the trademark
statement is in [`NOTICE`](../NOTICE).
