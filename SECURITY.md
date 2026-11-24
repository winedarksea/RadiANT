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

- **UF2 updates depend on who can enter the bootloader and write to its drive.**
  The stock bootloader accepts an unsigned UF2 image copied to its mass-storage
  drive. The standard Feather release images require a physical RESET double-tap
  to enter that mode.

  Both Pro Micro release images deliberately enable
  `CONFIG_ANT_DONGLE_UF2_REBOOT_REQUEST` because many of these boards have no
  reset button. Software with access to the USB device can request bootloader
  mode. A process with write access to the resulting drive can then install
  firmware without anyone touching the board. **Keep this option disabled in
  production and commercial builds.**
- **The nRF52840 Dongle's DFU cannot *usefully* be signed.** `nrfutil pkg
  generate --key-file` will happily produce a signed package, but the stock
  Nordic bootloader validates against a public key compiled into *itself* — so
  a package signed with our key is rejected by every dongle in the field.
  Making it work means replacing the bootloader, which costs the
  works-out-of-the-box property that is most of why that board is a supported
  target at all.

So the honest guarantee is integrity-on-download, not authenticity-at-flash:

- Every release artifact published by the release workflow has a `.sha256`
  sidecar, generated in CI by the job that built the image and re-checked by
  the separate `publish` job before it attaches anything
  (`.github/workflows/release.yml`). Only `publish` can write to the release,
  it runs only after every build job passes, it releases only a tag whose
  commit is on master, and it never replaces an asset already on a release.
  **v1.0 and v1.1 predate this:** their assets were uploaded by hand and have
  no sidecars and no attestation. v1.2.0 has no assets.
- The same job attaches a GitHub build-provenance attestation to every file it
  publishes. `gh attestation verify <file> -R winedarksea/RadiANT` checks that
  the file was built by this repository's release workflow, and from which
  commit. Unlike the sidecar, this does not depend on trusting whoever can
  write to the release.
- Compare the sum before flashing (`sha256sum -c <artifact>.sha256` beside the
  downloaded file). If it does not match, do not flash it.
- The job that builds the shipping images (`build-core` in
  [`.github/workflows/release.yml`](.github/workflows/release.yml)) restores
  no cache and pins its inputs:
  - **sources:** every west project by commit SHA, from
    [`.github/west-release-frozen.yml`](.github/west-release-frozen.yml), a
    `west manifest --freeze` of the sdk-nrf v3.4.0 workspace (`west.yml`
    itself pins sdk-nrf by commit; sdk-nrf's own manifest still names its
    imports by tag, which is why the frozen file exists);
  - **compiler:** Zephyr SDK 1.0.1 (minimal bundle plus `arm-zephyr-eabi`),
    fetched from versioned release URLs and checked against SHA-256 digests in
    the workflow;
  - **Python:** `west` and the packages a Zephyr build imports, installed with
    `--require-hashes` from
    [`.github/release-build-requirements.txt`](.github/release-build-requirements.txt);
  - **DFU packaging:** nrfutil, its core and `nrf5sdk-tools`, by version and
    SHA-256.

  **Not pinned:** the GitHub-hosted runner image (`ubuntu-24.04`: CMake,
  Ninja, git, the system libraries), the Python interpreter and pip that
  `actions/setup-python` provides, and the third-party actions beyond their
  commit SHAs. The build is **not** shown to be reproducible: no job rebuilds
  and compares, and the DFU zip carries entry timestamps. The other jobs in
  the release workflow (the Matter, TI and other non-shipping rows) still use
  `action-zephyr-setup` and its caches. Nothing they build is attached to a
  release, but they run in the same workflow run, and `publish` picks
  artifacts by name; tying each artifact to the job that built it is still
  open.

**What this does not protect against:** physical access to the board, and an
attacker who can write to the release, for anyone who checks only the
sidecar. Such an attacker can replace an image and its sum together; the
attestation check above detects that, the sidecar alone does not.

## Version and downgrade

DFU packages carry an application version derived from the root `VERSION` file
(`scripts/version_int.py`). Until 2026-08-14 both packaging paths passed a
hardcoded `1`, so every package ever published declared the same version and
the bootloader's downgrade check compared 1 against 1 — inert, with no symptom.
Packages built after that date carry a real version; **packages published
before it do not, and no downgrade protection should be assumed for them.**

## The Zwift shim

`tools/zwift_shim/` (two DLLs that let Zwift's `ANT_DLL.dll` reach a dongle on
a COM port) is **always built locally**, from your checkout, with your own
MSVC, by `scripts/zwift_ant_shim.ps1`. It is **not a release artifact**: no
release carries it, no CI job builds it for distribution, and a copy from
anywhere else should not be trusted. It runs inside Zwift's process and
forwards every host write, Zwift's ANT+ network key included, so:

- it drives only the port you select (`-Port` / `-Device`, kept as
  `RADIANT_ANT_COM` / `RADIANT_ANT_DEVICE`) and reports no device when there
  is no selection or it is ambiguous; it never guesses a port;
- it is built with `/W4 /WX /sdl /guard:cf` and accepts only `COM1`..`COM999`
  as a port name;
- its log masks key material in reads and writes (`tools/zwift_shim/README.md`);
- the installer replaces or deletes a file in the Zwift directory only if it
  carries the shim's version resource (or is the one earlier build it knows by
  hash), and runs no Python unless you name the interpreter.

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
