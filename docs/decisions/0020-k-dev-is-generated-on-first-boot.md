# 0020 — K_dev is generated on the device at first boot

- **Status:** Accepted. Built, ztest-verified, and checked on an nRF54L15 DK;
  the nRF52840 path is build-verified only (see [Verification](#verification)).
- **Date:** 2026-11-07
- **Amends:** [0009](0009-hostless-node-identity.md) decision 2 ("`K_dev` is a
  per-device secret provisioned at manufacture").
- **Related:** [0006 identity tiers](0006-security-v1-scope-and-x-priv-withdrawal.md),
  `../radiant-security.md` section 4 (Tier 0), `../hrm-reference-design.md`
  section 5.1, TODO item 5 of the 2026-11-07 security review
- **Code:** `radiant/src/node/node_ident.c` (`self_provision()`, the trigger in
  `node_ident_boot()`), `CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION`,
  `radiant/src/radiant_sec_rng_entropy.c` (`CONFIG_RADIANT_SEC_RNG_ENTROPY`),
  `radiant/src/node/node_nvm_settings.c` (a record that fails to load is EIO),
  `radiant/tests/fake_rng.c`, `radiant/tests/src/test_node_ident.c`

> **Provenance.** Clean-room. Written from this project's own source and
> design documents. Nothing here derives from sdk-ant, `libant.a` or any ANT+
> profile document. See [0002](0002-clean-room-policy.md).

---

## Context

ADR 0009 made `K_dev` the root of a hostless node's identity: the Tier 0 device
number is derived from it, and so is the deterministic pairing scalar. It said
`K_dev` is "provisioned at manufacture" and left how to a later decision.

Nothing in `apps/` ever called `node_ident_provision()`. So on every board in
this tree `node_ident_boot()` returned `NODE_IDENT_ENOKEY`, the node fell back
to its Kconfig device number (4242, 4243, 4244), and **Tier 0 — the only
privacy feature on by default — never ran**. Every unit flashed from one build
shared one identity, which for a treadmill in a gym is twenty machines a
receiver cannot tell apart. `../hrm-reference-design.md` section 5.1 recorded
the gap and three shapes for closing it, and called it a manufacturing-process
decision needing its own ADR. This is that ADR.

## Decision

1. **`K_dev` is generated on the device, from its own TRNG, the first time it
   boots with no `K_dev` record.** `node_ident_boot()` draws 16 bytes with
   `radiant_sec_rng()`, hands them to the existing `node_ident_provision()`
   (which refuses an all-zero key, writes the key before the counters and
   confirms each by read-back), wipes its copy, and carries on booting. It
   logs `identity provisioned on first boot` once.
2. **Only "never written" triggers it.** The storage seam already separates
   `NODE_NVM_ENOENT` (record absent: a first boot) from every other failure.
   `ENOENT` provisions; any other `K_dev` load failure returns
   `NODE_IDENT_EIO` and writes nothing. An RNG failure returns
   `NODE_IDENT_ENOKEY` and writes nothing.
3. **Failure falls back loudly.** Any boot failure — no entropy backend, a
   storage error, an exhausted boot counter — leaves the node on its Kconfig
   device number with no transforms, which is exactly what every board did
   before this ADR, and the application logs it at error level on every boot
   with the reason (`node_ident_strerror()`).
4. **The node applications get an entropy backend.**
   `CONFIG_RADIANT_SEC_RNG_ENTROPY` is `radiant_sec_rng()` over Zephyr's
   entropy driver on the board's `zephyr,entropy` node: CRACEN's TRNG
   (`psa_rng`) on nRF54L, and the nRF52840's RNG peripheral (bias correction
   on), which the hrm_ble and treadmill nRF52840 overlays choose instead of the
   SoC default `&cryptocell`. Policy still never names a backend; it reads
   `caps.has_rng` (ADR 0009 decision 5).
5. **`apps/trainer` opts out.** It is the bench FE-C fixture and
   `tools/fec_ack_sweep.py` addresses it as 4244, so its `prj.conf` sets
   `CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION=n` and it always uses
   `CONFIG_TRAINER_DEVICE_NUMBER`, even when another image left an identity
   record on the same DK.

## The four shapes, and why this one

Section 5.1 of the reference design listed three; this is a fourth.

| Shape | Factory tooling | Key leaves the chip | Who must keep a secret |
|---|---|---|---|
| Bench-only `PROVISION_ON_FIRST_BOOT`, refused in release builds | none on the bench, a real step in production | no | the factory, in production |
| A separate one-shot provisioning image | a second image and a flash step per unit | depends on the image | the factory |
| A factory step writing `K_dev` through the programmer | a programmer script and a key source | **yes**, it starts outside | the factory, forever |
| **Generated on the device at first boot** | **none** | **no** | **nobody** |

First-boot generation wins for four reasons.

- **No protocol needs `K_dev` off the device.** It feeds exactly two things,
  the Tier 0 device number and the pairing scalar, and both are computed on
  the node. A receiver learns the device number from the air and a pairing key
  from the pairing exchange; a printed or QR-code key is `K_root`, a different
  key. So the factory never needs to know `K_dev`, and a scheme that makes it
  know one creates a secret to protect for no reader.
- **The key never exists anywhere else.** Nothing to leak from a factory
  database, a programmer log or a provisioning image.
- **It fits an open-source project.** This project's "manufacturer" is often one
  person with a DK. A design that is secure only with factory tooling is a
  design most builds here would ship without; this one is on by default and
  costs them nothing.
- **It matches the security design's own words.** `../radiant-security.md`
  section 4 titles Tier 0 "random at first boot, stable thereafter". Until now
  the firmware could not do what that heading says.

A factory that does want to write its own key still can: `node_ident_provision()`
is unchanged, and a node that finds a record at boot never draws (asserted by
`test_a_factory_key_is_never_replaced`).

## The trigger rule

**Only `NODE_NVM_ENOENT` may lead to a new key.** The tempting version is "no
usable key, so make one". It is wrong: a flash read that fails once, or a record
whose size a firmware update changed, would then re-roll the identity silently.
The device number moves, every receiver paired to it is orphaned, the pairing
counter restarts under a new key, and nothing logs an error, because from the
node's point of view provisioning succeeded. So a read error is `EIO`, the node
falls back for that boot, and the next boot with a working read gets the same
identity back (`test_a_k_dev_read_error_is_eio_and_never_reprovisions`). A stored
all-zero key is not `ENOENT` either and is not repaired; `node_ident_provision()`
can never have written one.

The rule is only as good as the storage backend's honesty, and the shipping one
was not honest about it: `node_nvm_settings.c` left a record it found but could
not load (wrong length, read error) looking *absent*, which `node_nvm.h` already
forbade and which under this ADR would have re-rolled the identity. It now
marks such a record bad and answers `NODE_NVM_EIO`; a zero-length record (the
settings subsystem's deletion) is still absent.

## Accepted costs

- **A full flash erase re-rolls the device number.** The identity is in the
  settings partition, so `nrfjprog --eraseall`, a J-Link `erase` or
  `flash_sim_jlink.ps1 -Erase` gives the node a new number, and receivers
  paired to it re-pair once. A reflash that only programs the image's own
  sectors keeps it (verified on the bench, below).
- **Units already in the field re-roll once.** They ran on the Kconfig number
  with no `K_dev`; their first boot on this firmware provisions and moves them
  to a derived number. That is the point of the change, and it is a one-off.
- **Flash and RAM.** Measured 2026-11-07 on hrm_ble's plain ANT+ build (no
  BLE), `arm-zephyr-eabi-size` on `zephyr.elf`, against master at `920eddb`:

  | Board | master flash / RAM | this ADR flash / RAM | delta |
  |---|---|---|---|
  | nRF52840 DK | 86 956 / 42 666 B | 88 420 / 42 738 B | **+1 464 / +72 B** |
  | nRF54L15 DK | 97 368 / 42 466 B | 98 220 / 45 030 B | **+852 / +2 564 B** |

  (The nRF52840 row was measured before the last `node_nvm_settings.c` change,
  which added 20 B of text on the nRF54L15.)

  On the nRF52840 that is the RNG peripheral's driver (~750 B), the
  provisioning path in node_ident and `main()` (~270 B) and
  `radiant_sec_rng()` (104 B). The board files keep it
  there: left at the SoC default, the CryptoCell entropy driver links its
  platform library and NCS raises `MAIN_STACK_SIZE` to 2048 for it, which
  measured **+8 192 / +2 009 B** instead — so `zephyr,entropy = &rng` in the
  overlay and `CONFIG_ENTROPY_CC3XX=n` in `boards/nrf52840dk_nrf52840.conf`.

  On the nRF54L15, 2 560 B of the RAM is the main stack: NCS raises
  `MAIN_STACK_SIZE` from 1024 to 3584 whenever the CRACEN CTR_DRBG driver is
  linked, and the draw runs on `main()`'s stack. The BLE build already links
  that driver for the SoftDevice Controller and already has the 3584-byte
  stack, so it pays only the ~850 B of flash. The plain ANT+ build pays the
  stack once, on a part with 256 KB of RAM; shrinking it back would leave the
  one call that needs it — the first boot — to overflow.

## Consequences

- **Tier 0 runs on every node build with an entropy backend**, which is now
  every hrm_ble and treadmill build in this tree. Production checklist items
  3 and 4 in `../hrm-reference-design.md` are met by the firmware; what a
  manufacturer still checks is that the entropy backend is on and that the
  boot log shows the identity record.
- **ADR 0009's counter invariant holds.** "The counters are never reset
  independently of `K_dev`": provisioning writes a new key and fresh counters
  together, in that order, as it always did.
- **A Tier 1 factory reset is now "delete the `K_dev` record and reboot".**
  The next boot finds `ENOENT` and provisions a new key with fresh counters.
  Not built: no trigger exists yet (out of scope here).
- **Dongles are unaffected.** Their epoch and pairing scalar come from the host
  (`0xF3`/`0xF5`) and they do not enable `RADIANT_NODE_IDENT`.

## Verification

- ztest, `radiant/tests` on the nRF5340 DK through `scripts/run_ztest_hw.ps1`,
  2026-11-07: the default `prj.conf` (fake RNG on, self-provisioning on) ran
  923/923, `node_ident` 31/31 including the seven first-boot cases; the
  `no_pairing` configuration (`CONFIG_RADIANT_SEC_PAIRING_X25519=n`,
  `CONFIG_RADIANT_TEST_FAKE_RNG=n`) ran 878/878, `node_ident` 23/23 including
  the pre-ADR "unprovisioned node cannot boot" and "the RNG seam compiles out"
  cases.
- nRF54L15 DK, hrm_ble (CRACEN through `psa_rng`), 2026-11-07, log on the
  DK's VCOM:
  1. flashed with a full erase (`flash_sim_jlink.ps1 -Erase`):
     `identity provisioned on first boot`, then `device number 57338 from the
     identity record` — not the Kconfig 4242;
  2. reset (the same image re-flashed without erase, so J-Link skipped the
     program and only reset): `device number 57338`, no provisioned line;
  3. `tools/ant_scan.py` through a second RadiANT stick: `found: #57338 -
     heart rate monitor`, 90 broadcasts in 30 s;
  4. a different image (`sensor_example.conf`) flashed without erase:
     `device number 57338`, no provisioned line.

  Repeated on the final image (with the `node_nvm_settings.c` change): flashed
  without erase over the record above, `device number 57338`; then with a
  full erase, `identity provisioned on first boot` and `device number 30995`
  — the erase re-rolled it, as accepted above; then a reset, `30995` again
  with no provisioned line; `ant_scan.py` heard `#30995`.
- The nRF52840 entropy path (RNG peripheral) is build-verified only; no
  nRF52840 DK was on the bench.
