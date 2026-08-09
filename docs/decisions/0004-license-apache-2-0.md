# 0004 — Licence: Apache-2.0

Checked by: the presence of `SPDX-License-Identifier: Apache-2.0` at the top of
every source file, which is grep-able and should become a CI check when there
is a job to hang it on. The reasoning below is checked by nothing.

- **Status:** Accepted
- **Date:** 2026-08-08
- **Related:** [`../../LICENSE`](../../LICENSE), [`../../NOTICE`](../../NOTICE),
  [0002 clean-room policy](0002-clean-room-policy.md),
  [0003 naming and trademark](0003-naming-trademark-and-usb-identity.md),
  [`../third-party.md`](../third-party.md)

## Context

This repository needs a licence, and the realistic candidates for permissively
licensed embedded firmware are MIT, BSD-2/3-Clause, and Apache-2.0. All three
permit commercial use, modification and redistribution; all three are short
enough to read; all three are universally accepted. On the copyright axis there
is nothing to choose between them.

What distinguishes this project is **patents**.

- ANT is a patented radio protocol. Garmin (via Dynastream) holds the ANT
  patent portfolio.
- Core ANT dates to **2003**, so the foundational filings are very likely
  expired — US8855246B2, which is one of the sources this project reads
  precisely because it has expired, is a case in point.
- "Very likely expired" is not "cleared". Nobody has done a freedom-to-operate
  analysis. Continuations, later filings covering specific mechanisms, and
  non-US families have not been enumerated.
- **There is no patent pledge.** Garmin has never issued one for ANT. The
  shutdown of the ANT+ program on 30 June 2025 removed the certification body;
  it did not grant anybody anything.

So this project is building an implementation of a patented protocol, in
public, with the patent position unknown and no prospect of it being formally
resolved. That is not a reason not to build it — the expiry timeline is
strongly in our favour and the alternative is that the ecosystem simply dies —
but it is a reason to care which licence the code carries, because the licences
differ on exactly this point.

MIT and BSD say nothing about patents at all. Their grants are copyright
grants. A contributor to an MIT-licensed project can contribute code reading on
a patent they hold and then assert it against users of that very code, and
nothing in the licence prevents it. In most projects that is a theoretical
concern. In a project whose entire subject matter is a patented radio protocol,
and which will accept contributions from people whose employers hold radio
patents, it is not.

## Decision

**RadiANT is licensed under the Apache License, Version 2.0.**

`LICENSE` carries the full, verbatim Apache-2.0 text. `NOTICE` carries project
attribution and the trademark statement required by
[0003](0003-naming-trademark-and-usb-identity.md). Every source file carries
`SPDX-License-Identifier: Apache-2.0`; every `ant_core/*.c` additionally
carries a `Provenance:` line, per
[0002](0002-clean-room-policy.md).

### Why, in order of weight

**1. §3 is an express patent grant, with a retaliation clause.** This is the
decisive reason and the only one that MIT and BSD cannot match at any price.
Section 3 grants, from every contributor, a perpetual, worldwide, no-charge,
irrevocable patent licence covering the claims that contributor's contribution
necessarily infringes — and terminates that licence for anyone who initiates
patent litigation alleging the Work infringes. In a project reimplementing a
patented protocol, that does two things: it means contributions come with
patent peace attached rather than as a copyright grant with an unexploded
attachment, and it makes the project a materially worse target for a
contributor-turned-plaintiff. Apache-2.0 is the only permissive licence that
addresses patents at all. Given the subject matter, that settles it.

**2. It matches the neighbours.** `github.com/ant-wireless` — Garmin's own
Apache-2.0 host glue — is Apache-2.0. Zephyr is Apache-2.0. sdk-nrf is
predominantly Nordic 5-clause BSD with Apache-2.0 portions and mixes with it
without friction. Choosing the licence that everything this code sits next to
already uses removes an entire class of question from every future
integration, and makes upstreaming a fix trivially uncomplicated.

**3. `NOTICE` is a first-class part of the licence.** Apache-2.0 §4(d) gives
the `NOTICE` file defined redistribution semantics: downstream distributors
must carry its attribution notices. That is exactly the vehicle the trademark
statement needs — the disclaimer that ANT and ANT+ are Garmin's marks, that
this project is not endorsed, and that the USB identifiers exist for driver
matching. Under MIT there is no equivalent mechanism; the statement would live
in a README that nobody is obliged to carry. §6 additionally declines to grant
trademark rights, which is precisely the right default here.

**4. It is explicit about contributions.** §5 states that contributions are
under the same terms unless separately agreed. For a project where the
*provenance* of code is a first-order concern, having the inbound licence
stated in the outbound licence removes the need for a separate CLA to answer a
question the licence already answers.

### What was rejected, and why

- **MIT / BSD-2 / BSD-3.** Shorter and more familiar, and their permissiveness
  is genuinely appealing for embedded code where a two-line header is the whole
  licence. Rejected solely on patents. Nothing else separated them.
- **GPL / LGPL.** Wrong for firmware intended to be embedded in commercial
  products, and it would make the rtl_433 boundary moot in the worst possible
  way — by capitulating to it rather than deciding it. See
  [0002](0002-clean-room-policy.md).
- **MPL-2.0.** File-level copyleft with a patent grant, which does address the
  patent axis. Rejected because per-file copyleft on firmware that gets
  statically linked into a customer image creates ongoing compliance questions
  for downstream users, buying a protection this project does not want in
  exchange for adoption friction it cannot afford.
- **Dual licensing.** No commercial model here to justify the overhead.

### Two files put on this licence now, deliberately

**`tools/ant_pages.py`** and the sibling **`zephyr_aerosense`** project go onto
Apache-2.0 at the same time as this decision, not later.

`tools/ant_pages.py` is the ANT+ page encode/decode contract, and it is
*shared* between this repository and `zephyr_aerosense` — it is what lets the
aero sensor and the dongle agree on what a page means, and it is the module the
future `src/profiles/` implementation will mirror. A header shared between two
projects under two different licences only gets more expensive with time: every
future change has to be checked against both, contributions become ambiguous
about which project they were made to, and the eventual reconciliation happens
when someone wants to do something urgent. Doing it now costs one commit in
each repository.

The same reasoning applies to `zephyr_aerosense` as a whole: it is the second
consumer of `src/profiles/` and the second user of the page envelope, so it is
going to keep sharing code with this repository indefinitely. Two projects that
share code should share a licence.

*(Note for whoever executes this: `zephyr_aerosense` is a separate repository
and is outside this repository's write scope. This ADR records the decision;
the change itself is a task in that repository, and it uses a different NCS
toolchain bundle.)*

## Consequences

**Good.**

- Contributions carry a patent grant, and a contributor who sues over the code
  they contributed loses their own licence to it.
- No licence-compatibility question with Zephyr, sdk-nrf, or Garmin's own
  Apache-2.0 host code. Upstreaming is uncomplicated.
- `NOTICE` gives the trademark statement real redistribution semantics rather
  than being a README section that travels only by courtesy.
- The two projects sharing `tools/ant_pages.py` share a licence, permanently,
  as of now rather than as of whenever it becomes a problem.

**Costs, accepted.**

- Apache-2.0 is long. Some embedded developers prefer a licence they can read
  in thirty seconds, and a few downstream users have blanket policies that
  treat anything longer than BSD as requiring review.
- The `NOTICE` obligation is real: downstream redistributors must carry it, and
  we must keep it accurate as third-party components change.
- The per-file SPDX header is a small, permanent tax on creating new files, and
  it is only worth anything if it is applied consistently — which is why it is
  being applied to every existing file in one pass rather than opportunistically.
- Apache-2.0 says nothing about *third-party* patents. It protects against
  contributors, not against Garmin. Nothing in this decision changes the
  underlying patent exposure described in the context above; it only ensures
  the project does not add to it from the inside.
