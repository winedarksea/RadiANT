# `archive/specs/` — pointers and facts, not PDFs

Checked by: [`../../.github/workflows/linkcheck.yml`](../../.github/workflows/linkcheck.yml).
The weekly run fails, and opens an issue, when any URL below stops resolving.
The hashes and quotes were taken by hand on the dates given and nothing
re-checks them.

**No specification document is committed to this repository, and none will
be.** Free to download is not the same as ours to republish: Garmin retains
copyright in every document below, and none of them arrives with a licence.

What is committed is the part that actually survives — the document number, the
URL, a SHA-256 so a copy found elsewhere can be *proved* to be the copy this
project was written against, the retrieval date, and a snapshot URL. Plus, and
this is the part that matters most, **our own derived prose in
[`docs/`](../../docs/)**. If thisisant.com goes dark tomorrow,
`docs/ant-serial-protocol.md` and `docs/ant-radio-link.md` still describe the
protocol, still carry their provenance tags, and are still in git.

---

## 1. ANT Message Protocol and Usage, Rev 5.1

|  |  |
|---|---|
| **Document number** | D00000652 |
| **Title** | *ANT Message Protocol and Usage*, Rev 5.1 |
| **URL** | <https://www.thisisant.com/assets/resources/ANT%20Protocol/D00000652_ANT_Message_Protocol_and_Usage_Rev_5.1.pdf> |
| **Access** | Free download. **No login, no adopter agreement.** |
| **Retrieved** | 2026-08-08 |
| **Size** | 4,124,380 bytes |
| **SHA-256** | `66dd7133d9a2799ed2e6f9375a35ccdddc5a884d87e46ce7d16826bdd8b5b9f1` |
| **Snapshot** | <https://web.archive.org/web/20240716203926/https://www.thisisant.com/assets/resources/ANT%20Protocol/D00000652_ANT_Message_Protocol_and_Usage_Rev_5.1.pdf> (captured 2024-07-16) |
| **Redistribution** | **No.** Pointer, hash and derived prose only. |

**Why this one matters more than everything else here.** It specifies the
host&harr;dongle *serial* protocol — the `0xA4`-framed messages
`src/ant_serial_bridge.c` implements and `protocol/ant_wire.yaml` encodes. It
is the exact layer this project must be byte-compatible at, and it is free.
Every constant in `protocol/ant_wire.yaml` tagged `rev5.1 sec ...` came from
here.

It is emphatically **not** a description of the radio link. There is no on-air
frame format in it, no channel scheduling, nothing about the link layer
`ant_core` has to rebuild. That gap is why `docs/ant-radio-link.md` exists and
why every line of it carries a provenance tag.

**The snapshot is not ours and is two years old.** Anonymous Wayback "Save Page
Now" submission was attempted on 2026-08-08 and refused. The 2024 capture was
verified to exist and to have returned HTTP 200, but **its content was not
downloaded and not hashed**, so it is not known to be byte-identical to the
copy hashed above. Treat the snapshot as "a copy of some revision of this file
exists off-site", not as a verified mirror.

Verify a copy you already have:

```powershell
Get-FileHash D00000652_ANT_Message_Protocol_and_Usage_Rev_5.1.pdf -Algorithm SHA256
```

Submit a fresh snapshot (needs an archive.org account):

```powershell
Invoke-WebRequest -UseBasicParsing -TimeoutSec 300 `
  -Uri "https://web.archive.org/save/https://www.thisisant.com/assets/resources/ANT%20Protocol/D00000652_ANT_Message_Protocol_and_Usage_Rev_5.1.pdf"
```

## 2. The programme shutdown announcement

|  |  |
|---|---|
| **Title** | *ANT+ Changes* |
| **Document number** | None. It is a web page, not a numbered tech bulletin — see the note below |
| **URL** | <https://www.thisisant.com/business/go-ant/ant-brand> |
| **Companion page** | <https://www.thisisant.com/developer/ant-plus/certification> |
| **Retrieved** | 2026-08-08, HTTP 200, both pages live |
| **SHA-256** | **Not recorded.** See below |
| **Snapshot** | <https://web.archive.org/web/20260428102401/https://www.thisisant.com/business/go-ant/ant-brand/> (captured 2026-04-28) |
| **Redistribution** | **No.** The facts below are recorded; the page is not mirrored |

**No hash, deliberately.** These are server-rendered HTML pages carrying
navigation, session-varying markup and a footer year. Hashing one produces a
number that changes for reasons unrelated to the announcement, which is worse
than no number at all because it looks authoritative. The durable artifact for
a web page is the **verbatim quotation**, and the quotations are below.

**No numbered tech bulletin was found.** Garmin's tech bulletins are numbered
`D000xxxxx` like the specifications; a search on 2026-08-08 surfaced no such
document for the shutdown, only these pages. If a numbered bulletin exists,
add it here with its number and hash rather than replacing this entry — a page
and a PDF rot independently.

### The facts, quoted

> The ANT+ membership program and ANT+ product certification programs, along
> with associated engineering support will discontinue on June 30, 2025.

> ANT+ device profiles and documentation will continue to be available to
> developers after June 30, 2025.

> there is currently a changing regulatory landscape that would require
> substantial ANT+ redevelopment which would also break compatibility across
> the established ecosystem of products

And, from the same source, three things that are easy to get wrong and that
this project depends on being right:

- **New certification applications closed on 31 March 2025**, three months
  before the programme itself.
- **ANT licensing and manufacturer IDs are separate** from the ANT+
  certification programme and are *unaffected*. The stack is still licensed;
  the `$0.08` per-unit royalty on `libant.a` did not go away because the
  certification programme did.
- **The adopter agreement was modified to remove the certification
  requirement**, while still requiring products to meet the interoperability
  minimums the ANT+ documentation defines. This is why adopter signup has been
  free since January 2025 and why the device profile documents remain
  obtainable — a fact the clean-room policy in
  [`docs/decisions/0002`](../../docs/decisions/0002-clean-room-policy.md)
  depends on.

**What this announcement does not say**, and what no reading of it supports:
that Garmin open-sourced ANT. It did not. The stack remains closed and
proprietary, and sdk-ant's licence still forbids reverse-engineering the
binary.

## 3. `github.com/ant-wireless` — the host repositories

|  |  |
|---|---|
| **URL** | <https://github.com/ant-wireless> |
| **Retrieved** | 2026-08-08, via `api.github.com/orgs/ant-wireless/repos` |
| **Snapshot** | <https://web.archive.org/web/20250704031437/https://github.com/ant-wireless> (captured 2025-07-04, four days after the programme closed) |
| **Redistribution** | **Not vendored.** Publicly cloneable from GitHub; there is nothing perishable to rescue except the licence facts |

Sixteen repositories, as reported by the GitHub API on 2026-08-08 with the
licence GitHub detects for each:

| Repository | Licence detected |
|---|---|
| `Android_ANTHALService` | Apache-2.0 |
| `Linux_ant-hal` | Apache-2.0 |
| `GenericChannelHeartRateBarrel` | Apache-2.0 |
| `GenericAntPlusHeartRateField` | Apache-2.0 |
| `platform_frameworks_base` | (AOSP fork; GitHub reports `NOASSERTION`) |
| `platform_packages_apps_settings` | (AOSP fork; `NOASSERTION`) |
| `platform_hardware_libhardware` | (AOSP fork; `NOASSERTION`) |
| `platform_external_bluetooth_bluedroid` | (AOSP fork; `NOASSERTION`) |
| `platform_system_bluetooth` | none detected |
| `platform_packages_apps_Bluetooth` | none detected |
| `Android_ANTRadioService` | none detected |
| `Android_antradio-library` | none detected |
| `Android_ANTPlusPluginsService` | none detected |
| `Android_build` | none detected |
| `ANT_in_Android` | none detected |
| `ANT-Android-SDKs` | none detected |

**Read the table before repeating the headline.** "Garmin open-sourced ANT" is
the claim this organisation gets cited for, and it is wrong twice over: only
four of the sixteen repositories carry a detected Apache-2.0 licence, and every
one of the sixteen is Android or Linux **host glue** — HAL services, an
Android radio service, AOSP forks. There is no radio code and no link layer in
any of them. The correction is stated at the top of
[`docs/decisions/0002`](../../docs/decisions/0002-clean-room-policy.md) for
exactly this reason.

The last commits across the organisation predate the shutdown by years. The
2025-07-04 snapshot is worth keeping because an unmaintained GitHub
organisation is one policy change away from disappearing, and the licence
facts above are the only part this project relies on.

## 4. ANT+ device profile documents

|  |  |
|---|---|
| **Access** | Adopter-gated. Signup free since January 2025, no certification required |
| **Retrieved** | Not archived here, by policy |
| **Redistribution** | **No — and no mirror is committed either** |

The per-device-type specifications: heart rate (`0x78`), bicycle power
(`0x0B`), speed and cadence (`0x79`), fitness equipment (`0x11`), and the
common pages. They define the 8-byte page layouts that make a sensor
interoperable, and this project uses them — under the graded policy in
[`docs/decisions/0002`](../../docs/decisions/0002-clean-room-policy.md) — for
`src/profiles/`, `tools/ant_pages.py` and the profile documentation, and
**nowhere in `ant_core/**`**.

Obtain them the intended way: an adopter login at
<https://www.thisisant.com/developer/ant-plus/>. It is free and takes minutes.

### Unofficial mirrors: references only, deliberately not committed

Re-hosted copies of these PDFs are easy to find. **None is committed here and
none is linked from this file**, for two reasons that compound:

1. **Redistribution risk.** They are Garmin's documents, mirrored without
   permission. Committing one puts an unlicensed copy of a third party's
   copyrighted work in a public repository.
2. **Clean-room contamination.** Worse, and less obvious. Putting
   adopter-gated material *in this tree* undermines the one structural defence
   `ant_core` has — that the agent or person who wrote the link layer could not
   have read it. A file in the repository is available to everybody who clones
   it. Doing both in one commit would be the single most damaging thing anyone
   could do to this project's legal position, and it would look like tidiness.

Not linking them is a deliberate editorial choice, not an oversight: a link
here would be checked weekly by `linkcheck.yml`, which would amount to this
project actively maintaining a directory of mirrors.

What *is* worth citing, because it is code rather than a copied document, and
because it is what people actually reach for:

- **`openant`** — <https://github.com/Tigge/openant>. A Python ANT/ANT+ host
  library. Its device classes encode page semantics as working code.
- **GoldenCheetah** — <https://github.com/GoldenCheetah/GoldenCheetah>. Its ANT
  support has decoded real sensors for well over a decade.
- **rtl_433** — <https://github.com/merbanan/rtl_433>, specifically
  `src/devices/ant_antplus.c`. **GPL.** Facts only, never expression; the
  boundary and the reason are in
  [`docs/third-party.md`](../../docs/third-party.md).

## The point of all this

**The durable artifact is our derived prose**, not this file. Pointers rot;
this directory is the acknowledgement of that, and `linkcheck.yml` is what
turns a rotted pointer into an issue somebody sees instead of a 404 somebody
meets in two years.

| Source | What it became |
|---|---|
| Rev 5.1 | `protocol/ant_wire.yaml`, `docs/ant-serial-protocol.md` |
| Rev 5.1 + rtl_433 facts + our own measurements | `docs/ant-radio-link.md`, every line provenance-tagged |
| ANT+ profile documents | `src/profiles/`, `tools/ant_pages.py`, `docs/ant-plus-profiles.md` |
| The shutdown announcement | The framing in `docs/preservation.md` and `docs/decisions/0002` |

Those are in git. They build, they are tested, and they do not depend on
anybody else's web server.
