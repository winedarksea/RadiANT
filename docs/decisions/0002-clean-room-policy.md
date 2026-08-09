# 0002 — Clean-room policy for `radiant_core`

Checked by: the `Provenance:` line required on every `radiant_core/*.c` — a file
without one is not reviewable and should not be merged. The read-scope rules
are checked by the structure of the work itself (see *Enforcement*), not by a
script. Nothing else here is machine-verifiable, which is precisely why it is
written down before there is any code to argue about.

- **Status:** Accepted. **This ADR must exist and be merged before the first
  line of `radiant_core` is written.** That ordering is the whole point: a policy
  adopted after the fact proves nothing.
- **Date:** 2026-08-08
- **Related:** [0001 backend selection](0001-backend-selection-and-release-default.md),
  [0004 licence](0004-license-apache-2-0.md),
  [`../third-party.md`](../third-party.md)

## Context

`radiant_core` is a from-scratch implementation of a link layer whose only existing
implementation is a proprietary, obfuscated binary — `libant.a` — distributed
under a licence that explicitly forbids reverse-engineering it. The protocol is
patented (Garmin holds the ANT patents; core ANT dates to 2003), the ANT+
certification program was shut down on 30 June 2025, and there is no
specification of the link layer in public.

That is a situation where the *process* by which the code was written matters
as much as the code. A clean-room defence is not a claim you can make
retroactively. **One person disassembling `libant.a` once contaminates the
rebuild permanently**, and there is no way to prove afterwards that it did not
happen — which means the only useful time to write the rules down is before
anyone starts, and the only useful form for them is one that constrains what
people are able to look at rather than what they promise to forget.

The good news is that this is not a rebuild in the dark. A great deal is
genuinely public:

- **The ANT Message Protocol and Usage Rev 5.1** (D00000652) is free to
  download with no login and no adopter agreement. It specifies the host↔dongle
  serial protocol the bridge already implements.
- **rtl_433's ANT+ decoder** and expired patent **US8855246B2** together give
  the complete on-air frame: preamble, the `A6 C5` ANT+ network address, device
  number, device type, transmission type, a control byte (rtl_433 calls it a
  length; Spike B part 2 measured that it is not — see
  `docs/spike-b-part2-results.md`), 8-byte payload, and
  CRC-16/CCITT-FALSE (`0x1021`, init `0xFFFF`) over the 17 bytes after the
  preamble, with no whitening.
- **Our own hardware transmits.** Anything observable off the air from a device
  we own is ours to record and reason about.
- **ANT+ device profile documents** are adopter-gated, but adopter signup has
  been free since January 2025, no longer requires certification, and this
  project already holds a login.

So the interesting question is not "may we build this" — it is *which sources
may touch which files*, and that answer is not uniform.

## Decision

### Permitted sources

For **any** file in this repository:

- The **ANT Message Protocol and Usage Rev 5.1** (D00000652), and the archived
  ANT technical bulletins. Free, public, no login.
- **On-air observation using hardware this project owns.** Captures, timings,
  register experiments, `.antcap` recordings, spike output.
- **Facts** learned from rtl_433 — see the GPL boundary below.
- **Expired patents**, notably US8855246B2. An expired patent is a published
  teaching with no enforceable claim left; that is what the patent bargain is.
- Public datasheets and reference manuals: the nRF52840/nRF54L product
  specifications, Silicon Labs' RAIL documentation, the nRF24L01+ datasheet
  (the nRF24AP2 was an ANT MCU bonded to an nRF24L01+ core, which is why the
  ANT frame maps onto ShockBurst at all).
- Anything under `github.com/ant-wireless`, which is Apache-2.0 — but note that
  it is Android and Linux *host* glue and contains no radio or link-layer code,
  so it is nearly useless for this purpose. Do not let its existence be
  mistaken for "Garmin open-sourced ANT". Garmin did not.

For **`src/profiles/`, `tools/ant_pages.py`, and the profile documentation
only**:

- **ANT+ device profile documents** obtained under our adopter login. Use the
  page-layout tables and the field semantics. Copy no prose.

### Forbidden sources

- **Any output of disassembling, decompiling or instrumenting `libant.a`.**
  This one is absolute, is not graded, and does not expire with the ANT+
  program. Not `objdump`, not Ghidra, not a debugger single-stepping through
  it, not "just to check one constant". Public symbol *names* from `nm` and
  archive membership from `ar t` are inventory facts about a file and have
  already been recorded; the *contents* of the code are off limits.
- **Adopter-gated documents anywhere under `radiant_core/**`.** The link layer
  stays strictly clean-room. If a page-layout question arises while writing
  core code, the answer is that it belongs in `src/profiles/`, not that the PDF
  may be opened.
- **sdk-ant's source tree, for anyone writing `radiant_core/**`.** Not the headers,
  not the samples, not the documentation. See *Enforcement*.
- **Verbatim or near-verbatim rtl_433 code**, and verbatim profile-PDF prose,
  anywhere at all.

### The posture is graded, and here is why

It would be simpler to say "no gated document is ever opened by anyone" and be
done. That rule is rejected deliberately, and the reasoning is written here so
that it is not re-argued file by file for the next four months.

**Strict where the legal risk is structural.** The link layer and the binary
are where the exposure actually lives. `libant.a` is a licensed proprietary
work whose licence names reverse-engineering as prohibited; the link layer is
what the patents read on; and the link layer is the thing a plaintiff would
allege was copied. There, the defence has to be procedural and airtight —
demonstrable read-scope separation, provenance on every file, and no route by
which contaminated knowledge could have reached the author. Nothing about that
posture is softened by the sunset.

**Pragmatic where the sunset has left an interoperability spec with no program
behind it.** An ANT+ page layout is a *fact about a target you must
interoperate with*. Byte 4 of page `0x10` holds accumulated distance, or the
sensor does not work with a Garmin head unit. There is no design freedom to
infringe. The documents are obtainable for free by anyone who signs up, the
certification program they existed to serve no longer exists, and no plausible
harm follows from an implementer reading a layout table and writing an encoder.
Refusing to read them would not make `src/profiles/` any more original; it
would make it slower to write and more likely to be wrong, which serves nobody.

**The line between the two is a directory boundary, not a judgement call.**
`radiant_core/**` is strict. `src/profiles/`, `tools/ant_pages.py` and the profile
docs are pragmatic. That is checkable by looking at a path, which is the only
kind of rule that survives contact with a deadline.

### The rtl_433 GPL boundary

rtl_433 is **GPL-3.0-or-later** and this project is Apache-2.0. Those do not
mix in a shipped binary.

**Facts are fine.** Centre frequency, network address bytes, CRC polynomial and
seed, absence of whitening, the preamble derivation rule, field order and field
widths are facts about a radio protocol. Facts are not copyrightable. They go
into `docs/ant-radio-link.md`, each carrying an inline provenance tag —
`[rev5.1 §x]`, `[rtl_433]`, `[measured]`, `[inferred]` — so that a reader can
see, per fact, where it came from and how much to trust it.

**Expression is not fine.** rtl_433's loop structure, bit-extraction
expressions, variable naming and comments are copyrightable. None of it may
appear in an Apache-2.0 file, verbatim or paraphrased. The practical rule for
an implementer: read rtl_433, write the fact down in `docs/ant-radio-link.md`
in your own words, then write the code from the document rather than from the
source. If you find yourself with rtl_433 open in one window and
`radiant_frame.c` in the other, stop.

**The escape hatch, specified in advance.** If some file genuinely must derive
from rtl_433's expression, it does **not** get relicensed, and it does not get
edited until it looks different enough. It lives in a separate directory with a
GPL header, is excluded from the shipped image and from anything linked into
it, and is used only as a host-side analysis tool. This is written down now
because the alternative — improvising the answer at the moment someone has
already written the file — reliably produces the wrong one, and because there
is no cheap way to unwind a GPL derivation once it is inside an Apache-2.0
firmware image.

### Enforcement: read scope, expressed as who does the work

The mechanism is the useful part. A policy that says "do not look at sdk-ant"
is a promise. **A policy that says "the person writing `radiant_core/radiant_frame.c`
is not the person who has sdk-ant checked out" is a control.**

Work on this project is done by agents and contributors with explicitly stated
read scopes, and the scopes are assigned so that the boundary is a property of
the work assignment rather than of anyone's self-discipline:

| Role | May read | May not read |
|---|---|---|
| `radiant_core/**` authors | Rev 5.1, `docs/ant-radio-link.md`, spike outputs, this repository, public datasheets | sdk-ant (at all), profile PDFs, anything derived from `libant.a` |
| `sdk-ant-shim` author | sdk-ant headers — the **only** role that may | `libant.a` internals |
| `profiles-c`, `profiles-py` | additionally, the adopter-gated profile PDFs | sdk-ant, `libant.a` internals |
| everyone | `libant.a` internals — **nobody**, ever | |

This is the reason the parallel structure of the project is worth more here
than raw throughput would justify: splitting the work is what makes the
boundary auditable. Each work item's brief states its read scope in writing,
and the record of what was assigned is the evidence that the separation existed
at the time the code was written — which is the only evidence a clean-room
defence ever consists of.

Two consequences follow that are easy to get wrong:

- **The shim is written from sdk-ant's headers and that is fine.**
  `src/ant_radio_sdk_ant.c` is glue against a licensed API we are entitled to
  call. It is not part of `radiant_core` and must not be used as a route to inform
  it. An `radiant_core` author who wants to know what a shim forwarder does reads
  `src/ant_radio.h`, our own contract.
- **`ant_network_address_set()` stays permanently limited.** The key→address
  function is unknown to anyone outside Garmin and is not needed: `ant_net.c`
  holds a small table seeded with ANT+ (`B9A521FBBD72C345` → `A6 C5`) and
  returns `INVALID_PARAMETER_PROVIDED` for anything else. Document that as a
  limitation, not a to-do. The table may be extended by *observing RF
  emissions* from a shipping master, licence position confirmed first. Do
  **not** attempt to fit the function from samples: that is the single activity
  in this project most likely to be characterised as reverse-engineering, and
  it buys a feature nobody has asked for.

### `Provenance:` on every `radiant_core/*.c`

Every file under `radiant_core/` carries, in its header comment, both:

```c
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * <what this file is>
 *
 * Provenance: <where the knowledge in this file came from>
 */
```

The `Provenance:` line names the permitted sources this file was actually
written from. It is one line and it is not boilerplate — "written from the
sources in ADR 0002" is a non-answer. Useful examples:

```
 * Provenance: ANT Message Protocol Rev 5.1 §5.2 (message framing); frame
 *   field order and CRC parameters from docs/ant-radio-link.md, which tags
 *   each fact [rev5.1], [rtl_433], [measured] or [inferred].
```

```
 * Provenance: nRF52840 Product Specification v1.8 §6.20 (RADIO); register
 *   values measured in radiant_core/spike/rx_raw and recorded in
 *   docs/ant-radio-link.md. No sdk-ant source consulted.
```

Why per file rather than once per project: the question a clean-room defence
has to answer is not "was the project clean" but "where did *this* come from",
asked about whichever file is under dispute. Answering that four months later
from memory is not possible. Answering it from a line the author wrote the day
they wrote the code is trivial. One line per file, written when the knowledge
is fresh, is the cheapest insurance this project buys.

## Consequences

**Good.**

- The clean-room boundary is a property of how the work is divided, so it
  produces evidence as a side effect of getting the work done.
- Provenance is answerable per file, immediately, for the life of the project.
- The GPL question is settled before any code exists to be tainted by it, and
  the escape hatch is specified rather than improvised.
- `src/profiles/` is not artificially handicapped by a rule written for a
  different risk.

**Costs, accepted.**

- Some work is slower. An `radiant_core` author who could resolve a question in
  thirty seconds by opening a header instead spends a bench session measuring
  it. That cost is the deliverable — the measurement is what makes the answer
  ours — and it is the reason Spike A and Spike B exist and are scheduled
  before `radiant_core` starts rather than alongside it.
- Facts must be laundered through `docs/ant-radio-link.md` rather than going
  straight from a source into code. The document is a real artifact with real
  value, but it is extra writing.
- The graded posture will be questioned. It is written out above so the answer
  is a link rather than an argument.
- Nothing here is enforced by a test. The `Provenance:` line is enforced by
  review; the read scopes are enforced by how the work is handed out. Both are
  weaker than a CI check and both are the strongest available mechanisms for
  what they cover — which is worth stating plainly rather than implying more
  rigour than exists.
