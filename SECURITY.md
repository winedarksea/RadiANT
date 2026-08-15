# Security policy

## Reporting a vulnerability

Open a GitHub security advisory on this repository, or email the address in
`NOTICE`. Please do not open a public issue for anything that affects a shipped
image.

There is no bounty and no SLA. This is a small project and it is better to say
so than to imply a response time nobody is on call to meet.

## What is and is not protected

**Firmware images are published with SHA-256 sums and are not signed.** That is
a deliberate position rather than an omission, and it is worth stating exactly
why, because "unsigned" reads as carelessness when here it is a constraint:

- **UF2 targets cannot be signed at all.** The bootloader on those boards
  accepts any image dropped on the mass-storage drive. There is no signature
  check to satisfy. On these boards **physical access is the trust boundary**,
  and no firmware-side measure changes that.
- **The nRF52840 Dongle's DFU cannot *usefully* be signed.** `nrfutil pkg
  generate --key-file` will happily produce a signed package, but the stock
  Nordic bootloader validates against a public key compiled into *itself* — so
  a package signed with our key is rejected by every dongle in the field.
  Making it work means replacing the bootloader, which costs the
  works-out-of-the-box property that is most of why that board is a supported
  target at all.

So the honest guarantee is integrity-on-download, not authenticity-at-flash:

- Every release artifact is published with a `.sha256` sidecar, generated in CI
  immediately before the release attachment (`.github/workflows/build.yml`).
- Compare the sum before flashing. If it does not match, do not flash it.
- The build is reproducible from the tagged source with the toolchain the
  release workflow pins ([`.github/workflows/build.yml`](.github/workflows/build.yml)).

**What this does not protect against:** an attacker who can write to the
release, or who has physical access to the board. Neither is addressed by
anything in this repository today.

## Version and downgrade

DFU packages carry an application version derived from the root `VERSION` file
(`scripts/version_int.py`). Until 2026-08-14 both packaging paths passed a
hardcoded `1`, so every package ever published declared the same version and
the bootloader's downgrade check compared 1 against 1 — inert, with no symptom.
Packages built after that date carry a real version; **packages published
before it do not, and no downgrade protection should be assumed for them.**

## On-air security

RadiANT's own link encryption is specified in `docs/radiant-security.md` and is
scoped there. Two things that are easy to assume and are not true:

- **ANT+ traffic is not confidential.** The ANT+ network key is public, and any
  receiver can decode any standard profile broadcast. This is a property of
  ANT+, not of this implementation.
- **`CONFIG_ANT_DONGLE_ENCRYPTION` defaults to off** and that default is
  evidence-based, not cautious: `ANT_DLL.dll` exports 154 functions and not one
  of them keys a channel, so `MESG_ENCRYPT_ENABLE` and its two companions are
  unreachable from any Windows ANT application that exists. See
  `archive/host-api/README.md` for the export table that establishes it.

## Supported versions

Only the most recent release. There is no long-term-support branch.
