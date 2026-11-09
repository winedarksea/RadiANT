TI coexistence builds: the reviewer thinks TI's scheduler may refuse the command re-armed right after an early-stopped receive window, because the cancelled window still holds its reserved time. That needs a CC2652 on the bench.
MPSL air use: after a received frame or an ACK reply, the grant is now held to its natural end (up to about 4.5 ms) instead of being handed back early. That costs the other stack some air but isn't a correctness problem.
MPSL bursts: multi-packet bursts still can't run on MPSL builds. That's a scope gap, not a regression; Zwift uses acknowledged messages.
Weak tests: the scheduler half is fixed (`reserve_from_done_case()` in radiant/tests/src/test_sched.c, with cancel and reset recorded there as equivalent mutants); the gate-test half is MPSL and still open.
Tool footgun: fixed 2026-11-08. ant_ack_gate.py exits non-zero on any quality failure with or without --gate; the 300 s / 300 command sufficiency reason fails only under --gate.
cc26xx conformance transcript: owed. Release review item 2 changed advanced_options_3 from 0xD5 to 0x05, so `conformance-cc26xx.antser` is stale until re-recorded on a CC2652.

# Module packaging follow-ups

Remaining steps to make `radiant/` an installable, listed Zephyr module. The
usability work (samples, `antr_*` in the module, stock Zephyr support) is done.

- [ ] **Root-level `zephyr/module.yml`, so a west project entry just works.**
  West only finds a module at a project's root, and ours is at
  `radiant/zephyr/module.yml`, so today a consumer adds the repository as a project
  and then also appends `radiant/` to `ZEPHYR_EXTRA_MODULES`. Add a root
  `zephyr/module.yml` with `build: cmake: radiant`, `kconfig: radiant/Kconfig`, and
  `samples`/`tests` paths under `radiant/`.
  To resolve first: this repo's own west workspace would then discover the module
  automatically while every app, test and sample also appends it by path under the
  same name, so check what `zephyr_module.py` does with the duplicate and remove
  the appends if needed (the Windows `C:\ncs` workflow is not a west workspace for
  this repo, so it still needs them).
  Not a blocker: clone size. The whole history packs to 6.7 MiB and `archive/` is
  5.2 MB, so cleaning up `archive/` is optional, and `.gitignore` would not shrink
  history anyway. Consumers can also set `clone-depth: 1`.

- [ ] **Rename `antr_*`, `ANTW_*` and `ant_radio.h` to RadiANT-specific names.**
  They are public API now, and the names describe the ANT serial protocol rather
  than this project. Keep compatibility aliases for a release. Do this before the
  listing below, while few people depend on them. `ant_wire.h` is generated, so the
  prefix changes in `scripts/gen_ant_wire.py`; the sdk-ant comparison backend
  relies on the prefix not colliding with sdk-ant's own names.

- [ ] **List RadiANT in Zephyr's External Modules docs.** Blocked on the root
  `module.yml`. Open a "New External Source Code" issue for TSC approval, then a PR
  adding `doc/develop/manifest/external/radiant.rst` from the template in that
  directory (`external.rst.tmpl`). Apache-2.0 skips the licence review. Use the
  "compatible, not certified" wording from ADR 0003. The default manifest is not
  the target: it is limited to code a Zephyr subsystem, platform, driver or tool
  uses.

# Discovery issues from the 2026-11-09 review of the last 35 commits

Reviewed through `86233e8`; both issues affect the discovery changes in
`2079575`. Verified by code inspection; C tests and hardware validation pending.

- [ ] **P2: Restore the configured discovery frequency when the last declared search ends.**
  A keyed off-57 search retunes the shared sweep. If that channel acquires or
  closes while a keyless scan remains active, `retune_at_boundary()`'s
  no-declared-frequency branch leaves the sweep on the off-57 frequency
  indefinitely, so the keyless scan misses normal ANT+ traffic on RF 57.
  Preserve the configured default separately from the current sweep frequency
  and restore it when no declared searches remain. Add a regression test for
  an off-57 keyed search ending while a keyless scan continues.
  Code: `radiant/src/radiant_search.c`, `retune_at_boundary()` (around line 606).

- [ ] **P2: Apply a first nonzero-network key installed during an active search.**
  If searches are active when the first valid key is installed on network 1
  or higher, `antr_network_address_set()` skips sweep reinitialization without
  recording pending work. Discovery keeps using the zero address. Closing all
  searches and retrying the same key still fails because `first_key` is now
  false; recovery requires a network-0 key or reset. Record the pending address
  change independently of active search membership and apply it safely between
  windows. Add regression coverage for installation during an active search
  and for closing searches and retrying the key. The analogous late-key
  limitation on network 0 predates this review range.
  Code: `radiant/src/antr/ant_radio_radiant.c`, `antr_network_address_set()` (around line 3712).

# Open issues from the 2026-11-07 RadiANT vs Garmin ANT USB-m comparison

Evidence: archive/benchmarks/2026-11-07-garmin-compare/ (README there).
Raw per-window logs are gitignored but still on disk on the bench PC.

## Before you start
- Bench rules:
  - Python: C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe
  - Run `. .\scripts\env.ps1 -NcsVersion v3.4.0` before git or west.
  - Flash DKs only with scripts/flash_sim_jlink.ps1. Never hand-write a
    J-Link CommanderScript.
  - Never kill ant_sim.
  - Feather flashes are rationed. Reflash the nRF52840 Dongle over DFU.
- Only one agent may use the bench at a time.

## 1. [DONE 2026-11-07, one open follow-up] Receive loss that depends on the device number
**Fixed** by `CONFIG_RADIANT_NRF_SLIP_GUARD` (ADR 0019). Cause: the nRF demodulator
drops one bit in an alternating run of 9 or more bits in or just after the
hardware-matched address. A/B/A on #54100 with the Garmin receiving: 54L15
2.08 → 0.17 → 2.33 %, Garmin 0.17/0.17/0.33 %. That passes the Done gate below.
Evidence: archive/benchmarks/2026-11-07-devnum-rx-loss/README.md.
Follow-up: at -50..-60 dBm guarded boards still lose 0.2-1.25 % on runs of
8-12, including the unguarded run-8 #54058 on the 5340. Measure at a controlled
level, and consider `SLIP_GUARD_RUN=8`. Tried 2026-11-08 (archive/benchmarks/2026-11-08-todo-clearance/README.md):
loss was bimodal, 0.3 % to 48 % for the same image and level, so run 8 could
not be separated from run 9. Default stays 9; this needs shielding or an
attenuator.
Feather (755972D7183A6183) still runs the stock image, so reflash it with the
fix when convenient.

(Original notes follow.)
RadiANT receivers lose 1.5-3 % of packets from device #54100 and 0 % from
#54900. The Garmin stick loses 0-0.2 % throughout. The loss follows the number,
not the transmitter. 120 s windows at -31..-48 dBm, one pinned power channel:

| step | TX    | dev #  | Feather | Dongle | DK RX         | Garmin |
|------|-------|--------|---------|--------|---------------|--------|
| 1    | 54L15 | 54100  | 1.65    | 1.85   | 2.50 (5340)   | 0.00   |
| 2    | 54L15 | 54900  | 0.00    | 0.00   | 0.00 (5340)   | 0.20   |
| 3    | 54L15 | 54100  | 2.92    | 2.71   | 3.12 (5340)   | 0.21   |
| 4    | 5340  | 54100  | 2.29    | 1.46   | 2.49 (54L15)  | 0.00   |

- The radio reports every lost packet as EVENT_RX_FAIL. Two chip families
  (52840, 5340 net core, 54L15) show the same loss.
- Numbers that were clean: 53000 (0xCF08), 53030 (0xCF26), 54130 (0xD372),
  54900 (0xD674). The bad one is 54100 (0xD354). 54130 shares the high byte
  and is clean, so suspect the low byte (0x54 = 01010100).
- **Status 2026-11-07: mechanism found, fix built, result not yet recorded.**
  - Mechanism (measured): the nRF `Nrf_1Mbit` demodulator drops one bit in an
    alternating run of about 9+ bits inside the hardware-matched address or
    just after it. Evidence and rig: archive/benchmarks/2026-11-07-devnum-rx-loss/.
  - Fix: docs/decisions/0019-tracking-matches-a-short-address.md. At-risk
    numbers get a 2-byte hardware match plus a CRC-checked one-bit repair
    (`CONFIG_RADIANT_NRF_SLIP_GUARD`); every other number keeps the full match.
  - All of it is uncommitted: ADR 0019, radiant_addr_split.[ch] and its ztest,
    apps/dongle/rxdiag.conf, and edits to radiant_radio_nrf.c, radiant/Kconfig
    and both CMakeLists.
  - ADR 0019 says "measured on air", but its Result section is empty and the
    archive README stops at Phase 4.
- Task: run and record Phase 5 (A/B/A on #54100 against the Garmin), fill in
  ADR 0019's Result, then commit.
- Done when RadiANT loss on #54100 is within the Wilson CI of the Garmin's,
  with a 0.5 pp margin.

## 3. [CLASSIFIED 2026-11-07: PROBE side, not firmware; trigger unknown] 54L15 receiver's VCOM died mid-window
**Soak:** 1 drop in 10 × 300 s windows (window 3, 204 s), with the same
error. All three checks say it is not the firmware:
- no reboot STARTUP arrived;
- the console kept running with continuous uptime and every channel still
  tracking;
- the console VCOM on the same probe (MI_02) never failed.

The probe did not re-enumerate (LastArrivalDate unchanged), and the port came
back by itself within about 2 minutes.
**Suspect ruled out (as far as the data goes):** the error comes from
SetCommState, and `SerialDevice.read` reconfigured the port on every read. A
bare reconfigure flood died once in 46 s, but then ran 180 s and 600 s clean.
That is 1 death in about 17 minutes, which fits the soak's own rate of about
one per 50 minutes. The trigger is unknown and looks random in time.
`SerialDevice.read` now sets the timeout only on change, as hygiene, not as a
fix. Partial JSON keeps a dead window's data. Low priority unless it starts
costing runs; if it does, try another USB port or hub first.
Evidence: archive/benchmarks/2026-11-07-vcom-soak/README.md.

(Original notes follow.)
- In the rerun's round A multi window (rerun/A_multi_1_54L15.log), pyserial
  raised "PermissionError(13, 'A device attached to the system is not
  functioning')" on the J-Link OB VCOM partway through.
- This could be the probe's VCOM or a firmware fault or reset on the 54L15.
  Nothing yet tells them apart.
- Task: repeat 300 s four-channel windows with the 54L15 receiving, and
  capture its log VCOM (it needs DTR) to see whether the firmware faulted.

## 4. [DONE 2026-11-08] ant_sens reports spurious loss on short event_counter rungs

## 6. [DONE 2026-11-08] Re-run ant_features against the Garmin
**Done:** sections 1-4 and 6 pass. Section 5 reads the event filter back as
`0000f1` after setting 0x0006, not investigated (archive/benchmarks/2026-11-08-todo-clearance/README.md). The run also
leaves the Garmin configured: reset it before using it as a reference.
- The 7-byte capabilities fix landed 2026-11-07 (`capabilities_usable()`,
  tested with the Garmin's real reply `080800ba3600df`). The tool itself has
  not been re-run on the Garmin; do it when the stick is plugged in.

# Open issues from the 2026-11-07 extensions and security review

Items 1-9 need no bench; items 10 and 11 do. `security.conf` and the compat
layer are in no release image, so every protocol change below breaks no
deployed host or node. Findings were verified by reading the code; nothing was
run.

## 1. [CLOSED 2026-11-08, accepted limits] What the Tier I phase lock does not cover

## 2. [DONE 2026-11-07] Implement the descriptor authentication frame

## 3. [DONE 2026-11-07] Version the 0xF4 status reply

## 4. [DECIDED 2026-11-07, implemented with item 2] The epoch in descriptor frame 1
**Decided (user):** the third option. With a transform on, frame 1 bytes
[4..7] carry the epoch only when it is coarse real time (0xF3 flag bit 0), zero
otherwise. The authentication frame's MAC always binds the real epoch, so a
keyed receiver recovers it by trial-verifying forward from the last epoch it
knew (`PROFILE_TLM_AUTH_EPOCH_SPAN` = 16); the 48-bit tag is the check, so no
hint field. Removes the boot-counter fingerprint from sealed hostless nodes.
**Left open, recorded:** a command-only node (command page, no transform) has
no auth frame and keeps broadcasting a non-zero epoch (radiant-security.md
7.1). Evidence: the two shared vectors differ only in frame 1 and the tag;
test_the_epoch_search_is_forward_only_and_bounded in both suites.

(Original notes follow.)
- radiant-telemetry.md section 6 frame 1 bytes [4..7] broadcast the epoch in
  the clear (profile_telemetry.c:391). It is zero on air today only because
  no transform can be announced; item 2 makes it non-zero.
- On a hostless node the epoch is the boot counter (ADR 0009). ADR 0008
  removed the epoch from the compat beacon and the handoff page for exactly
  that reason: a slowly incrementing number defeats identity Tier 2. The same
  field here does the same thing to a 0x60 node.
- Options:
  - keep it, and accept the fingerprint for 0x60 (document it in 7.1);
  - replace it with the compat key-group hint, trunc24(CMAC(K_id, epoch)),
    and recover the epoch by forward search as section 11.3 does;
  - carry it only when the epoch is coarse real time (0xF3 already has that
    flag), zero otherwise.

## 5. [DONE 2026-11-07] Provisioning so identity Tier 0 actually runs

## 7a. [DESIGN, deferred by decision 2026-11-07] Align clear runs to MAC windows
- With the counter now the slot index (item 7, done 2026-11-07), every window that holds a
  clear slot (descriptor, page 80/81) is unverified, and so is the window
  before it. Measured cost: docs/radiant-security.md 3.2.
- The fix would be profile_sched placing each cycle's clear run on whole
  windows. Deferred because it is not a clear long-term choice with no major
  trade-offs:
  - it changes the 121-message cadence copied from certified sensors (a
    secured channel would need a cycle that is a multiple of W, such as 120);
  - it spends up to W-1 padding slots per cycle (about 6 % of slots at W=8);
  - it couples profile_sched to radiant_sec's slot index;
  - an MPSL-denied slot shifts a page already written into the next slot,
    which breaks the alignment for that cycle on MPSL builds;
  - it cannot be enforced for host-driven masters at all.
- Today's behaviour fails safe: nothing is ever wrongly VERIFIED.

## 8. [DONE 2026-11-08] The 0xF1-0xF5 host commands run without api_lock

## 9. [DONE 2026-11-08] No informational bits left in the descriptor

## 10. [FIRMWARE, HIGH, bench] Search is deaf through the MPSL arm lead (ADR 0018)
- ADR 0018 is proposed and nothing is built. On MPSL builds a search is deaf
  for 3 x min_arm_lead_us (24.4 ms) per tracked period, about 30 % of phases.
  Measured: 5 of 17 offsets failed, all inside the band; 5 of 5 were found
  with nothing tracked.
- Consequence on the bridge: a new sensor's bind misses 50-70 % of attempts.
- The same exclusion radius (open decision R1 of the 2026-10-31 review)
  predicts about 8.7 % loss per master on the treadmill BLE build, against
  9.5 % measured.
- Not the shared-radio tradeoff: measured with the other stack silent and
  MPSL refusing nothing. It is one 24 ms placement notice per grant boundary,
  and the boundaries are phase-locked to the tracked slot.
- ADR revised 2026-11-07: MPSL caps a grant at 100 ms, so a 246 ms period
  needs three grants whatever is done. Candidate (a) (hold the elastic grant
  through the tracked boundary, with a bridging extension decided at the
  placement deadline) takes the deaf fraction from ~30 % locked to ~21 %
  drifting, and makes it independent of how many channels are tracked. It
  does not reach zero; that needs the next request pipelined behind the live
  grant (candidate d, a follow-on with its own mpsl_arb spike first).
- Task: build (a) per the ADR, adding the per-set coverage counter first.
  A/B/A with the stepped-offset harness. That harness (disc_phase.py) lived
  in a session scratchpad and may be gone; if so, rebuild it into tools/.
- Done when all 17 offsets bind, the median within about one sweep of the
  control and none beyond about four, and the chunk mask moves between
  reports instead of repeating. Not done-when "as fast as the control at
  every offset": with one session that is (d)'s criterion, not (a)'s.

## 11. [DONE 2026-11-08] review/2026-10-31-timing has waited a week
**Done:** A8-4, A9-3, A14-1 and A14-2 ported and on master. The crossover showed
no regression (archive/benchmarks/2026-11-08-todo-clearance/README.md). The branch is deleted; its commit is kept as
archive/review-2026-10-31-timing/6df53e7-timing-fixes.patch for A13-1 and C5-1,
both in the list at the end of this file.
- One commit, 6df53e7, "timing fixes (UNVERIFIED on air - do not merge without
  A/B)". It holds A14-1, A13-1, A9-3, A14-2, A8-4 and C5-1.
- Task: a crossover A/B (old and new firmware swapped across two identical
  receivers, using the multi-channel recipe); A13-1 and C5-1 also need an MPSL
  coexistence soak. Or delete the branch: it drifts further from master daily.

## 12. [DONE 2026-11-08] Tier II loses windows at the HR seam with no packet loss

## 13. [DONE 2026-11-08] 0x60 exact loss over-counts on host-driven masters

## 14. [DONE 2026-11-08] ant_verify's liveness line is wrong for FE-C

## 15. [DONE 2026-11-08] Telemetry section 6 overstates when unsecured byte [1] advances

# Open issues from the 2026-11-08 release review (HEAD b89a112)

Items 1-4 were confirmed by reading the code at this HEAD. The reviewer also ran
compiled probes for 2 and 4. Nothing here has been run on hardware. The
review's other two findings are already fixed (9ab6bc9, 971e38c); what is left
of the second is item 1 of the section above.

## 1. [DONE 2026-11-08] Discovery assumes network 0 and frequency 57

## 2. [DONE 2026-11-08] Capabilities advertise features that only store configuration

## 3. [DONE 2026-11-08] A reachability or label change can be lost until the next transition

## 4. [DONE 2026-11-08] A pending offline is lost when a source is reused during an outage

## 5. [DONE 2026-11-08] ant_verify decodes sealed data pages it has not processed

## 6. [DONE 2026-11-08] X_CONF alone leaves byte [7] in plaintext

## 7. [DONE 2026-11-08] First-boot provisioning can pick a number that is hard to receive

# Beta cleanup from the 2026-11-08 reviewer pass

No production devices exist yet, so these only remove code nothing needs. All
were checked by reading the code at 1a6c8fa; nothing here has been built or run.
Considered and left out (decided 2026-11-08): renaming the ANTW_* aliases to
RADIANT_WIRE_* (about 1,300 changed lines to remove one generated header), and
making tools/ant_health.py reject other versions (see item 2).

## 1. [FIRMWARE, Matter, LOW] Turn off the version-1 bridge storage migration
`CONFIG_BRIDGE_MIGRATE_VERSION_1` defaults to y
(apps/dongle_thread/src/matter/Kconfig:86), but this application has only ever
stored version 2 (`kCurrentVersion`), so the code looks for data that cannot
exist. Pre-2.7 migration is already off, and a fresh boot takes the same path.
- Task: drop the `default y` and rewrite the help text, which explains why it
  was left on. Update the table at docs/matter-bringup.md:595.
- Leave the vendored bridge_storage_manager.cpp alone. Its #ifdefs already
  exclude the code, and that keeps vendor updates simple.
- Done when a Matter build's .config has it unset, nm shows no
  `MigrateDataVersion1`, and a bound sensor's row survives a reboot on hardware.

## 2. [DOCS, LOW] Version comments contradict the pinned-at-1 rule
During beta every structure version stays 1, even when a field is added
(decided 2026-11-08). Readers tell which fields exist from the body length.
These still say to bump it:
- protocol/ant_wire.yaml:1890 (0xF4) and :1949 (0xF6);
- apps/common/ant_health.c:116;
- tools/ant_health.py:66, and the "older firmware" reasoning in `decode()` at :102.

Task: correct the text and run scripts/gen_ant_wire.py. Keep decoding regardless
of version: firmware from 2026-08-16 to 2026-11-07 (before 920eddb) sends version
2 with the same 30-byte layout, and a version check would reject those boards.
Post-beta versioning is a separate decision.

## 3. [FIRMWARE, LOW] Make backends state max_addr_groups explicitly
`max_addr_groups == 0` means "no constraint" for backward compatibility
(radiant_radio_hal.h:644). The nRF, cc26xx and null backends and both fake_radio
presets all set it, so the fallback only hides a backend that forgot to.
- Task: have `radiant_sched` init (radiant_sched.c:2125) refuse 0, the way it
  already refuses NULL caps. Remove the fallback at radiant_sched.c:300 and its
  copy at tests/fake_radio.c:1987, and fix the header comment.
- Done when a ztest shows init failing on a 0 cap instead of the backend
  returning `RADIANT_RADIO_ENOTSUP` during reception.

## 4. [FIRMWARE, bench, decide before the first production release] Move nRF52/53 to USB_NEXT
Two USB device classes are maintained because the legacy one has hours on it
against Zwift (apps/dongle/next.conf:11). The legacy stack is on borrowed time:
`USB_DEVICE_STACK` does `select DEPRECATED` in NCS v3.4.0
(zephyr/subsys/usb/device/Kconfig:10). nRF54 boards already have no choice.

Measured on a Feather (docs/backends.md:784): USB_NEXT costs 9.8 KB more flash
and saves 1.6 KB RAM, with 27 % lower throughput that is still 75x what eight
channels need.
- Before switching, on the Pro Micro, compare USB_NEXT with legacy:
  - a real Zwift session (no Zwift testing of USB_NEXT is recorded);
  - a byte compare of the descriptors, including endpoint order, because
    positional hosts depend on it;
  - ant_probe, ant_scan and bench_usb_stall.py with `next.conf;health.conf`;
  - USB reboot to bootloader.
- Then remove apps/common/usb_ant_class.c (745 lines), the USB_LEGACY Kconfig
  option in apps/dongle and apps/dongle_thread, and next.conf. Update the
  transports in release.yml, build_all.ps1 and sample.yaml, the board confs, and
  docs/backends.md.
- Also remove the two USB transport options and their stack defaults from
  apps/dongle_ti/Kconfig (:25-31, :44-48). That silicon has no USB, and they
  exist only to match the shape of the other apps' Kconfig. This part can go first.
- Check that the 9.8 KB still fits the Matter builds on nRF52840.
- Done when the checks above match legacy and no release image selects
  USB_DEVICE_STACK.

## 5. [TOOLS + FIRMWARE, LOW] Delete compatibility shims with no remaining user
- `RADIANT_BACKEND_MPSL` (radiant/Kconfig:808): no code or config uses it, and
  it exists only for "an out-of-tree configuration naming the old symbol".
- `ant_sens.run_ladder()` (tools/ant_sens.py:677): "kept for callers of the old
  signature", and it has no callers.
- `EVENT_MARKER` (tools/ant_session.py:94): its own comment says to delete it
  once ant_sim imports `MESG_EVENT_ID` directly. Switch ant_sim.py:71 and :1137
  and ant_sens.py:513, then delete it and the note at ant_session.py:57. Change
  the `source:` text at protocol/ant_wire.yaml:215 and regenerate.
- scripts/run_ztest_hw.ps1:92 still defaults to NCS v3.2.4, and its help at :70
  warns that every v3.4.0 run must override it. Default to v3.4.0 and drop the
  warning.
- Done when ruff and the Python tests pass, `gen_ant_wire.py --check` is clean,
  and a ztest run works with no `-NcsVersion`.

## 6. [FIRMWARE, LOW] Remove the finished bisection switches
`RADIANT_NRF_NO_RADIO0_IRQ`, `RADIANT_NRF_NO_GPPI`, `RADIANT_NRF_NO_HFXO_HOLD`
and `RADIANT_GATE_MPSL_NO_SESSION` (radiant/Kconfig:524-577, :670-689) came from
the SDC-assert bisection, which is closed. All are marked diagnostic, and no
.conf, script or CI job enables any of them. A NO_GPPI build cannot transmit or
receive at all.
- Task: delete the symbols and their branches (radiant_radio_nrf.c:2330, :2407,
  :2706; radiant_radio_nrf_gate_mpsl.c:1598). Git history keeps them. The
  NO_SESSION help argues for keeping that switch, but the result it found is
  already written down, so it goes too.
- Update docs/p4-coexistence-bugs.md:441 so it reads as history.
- Keep `RADIANT_NRF_RX_DIAG` (the device-number follow-up may need it) and
  `ANT_DONGLE_PAIRING_WINDOW_AT_BOOT` (in the bench runbook).
- Done when no reference to the four symbols remains outside history, and the
  nRF and MPSL gate images still build.

## 7. [REPO, LOW] Move radiant/spike out of the source tree
Six directories, about 7,300 lines, built by no CI job and mostly documented for
NCS v3.2.4, so nothing notices if they stop compiling. They do not ship; the gain
is a source tree holding only maintained code.
- First promote what is still live: the nRF5340 net-core dongle needs
  radiant/spike/promisc/appcore programmed alongside it
  (apps/dongle/boards/nrf5340dk_nrf5340_cpunet.conf:18, .overlay:13), and
  net_rtt.conf is referenced at :47 of that .conf. Give them a supported path.
- Check whether radiant/Kconfig:254-257 still needs radiant/spike/ti_phy. It tells
  readers to rerun that spike before turning on TI COEX.
- Move the rest to archive/spikes/ and repoint the roughly 20 files that cite
  them. They are the provenance for measured numbers, so keep them, not delete.
- Done when scripts/check_links.py passes, the 5340 net-core build still works,
  and only archive/ and history mention radiant/spike.


# Left open by the 2026-11-08 clearance

MPSL (needs a soak beside BLE or Thread, which that clearance excluded):
- **A13-1.** Replace `irq_lock()` around capture-and-fold in `radiant_radio_now()`,
  `clock_absolute()` and `clock_counter_at()` with `CLOCK_LOCK()/CLOCK_UNLOCK()`
  (`radiant_swi_zli_lock()` when `CORE_CB_DEFERRED`, else irq_lock) and add
  `radiant_dbg_clock_back` in `clock_fold()`. On MPSL builds `radio_isr()`,
  `deliver_terminal()` and `program_rx()` fold from the ZLI signal callback that
  irq_lock does not mask; a ZLI fold between capture and fold wraps
  `counter-last` to ~2^32, a permanent +71.6 min step. Soak reading
  `radiant_dbg_clock_back` (expect 0).
- **C5-1.** In `radiant_radio_nrf_gate_mpsl.c` record `placed_extendable` and
  `placed_high_prio` in `request_work_fn` before `mpsl_timeslot_request()`, and
  have the BLOCKED/CANCELLED handling read those. MPSL can answer BLOCKED inside
  the request, before `commit_next_grant()`, so the first refusal after a class
  change is attributed to the previous class and the elastic chunk is not
  shortened (ADR 0013). The test (`test_an_inline_block_backs_off_the_class_that_was_refused`,
  `gate_probe_reset_elastic()`) and A13-1 are in
  archive/review-2026-10-31-timing/6df53e7-timing-fixes.patch.
- A14-1: on an MPSL grant, an ETIME from `program_rx()` is reported through
  `gate_failed` only after the grant ends, so the grant is not released early and
  its remaining air is wasted. The comment at gate_mpsl:1160 is wrong.
- A pending `gate_failed` is not cleared by `radiant_radio_abort()`, so an abort
  before `denied_work_fn` runs delivers FAILED to the next operation. Self-heals;
  found by reading only.
- `program_tx()` writes CC_TXSTART before enabling `conn_start_tx` and re-checks
  with `t_start <= now`, the ordering A14-1 replaced for receive. A compare just
  after the enable, before `now` is read, gives a false ETIME.

Security and pairing:
- X25519 pairing holds api_lock 366-376 ms. A multiply outside the lock on a
  copied scalar would remove the stall.
- `node_ident_arm_sec` and `node_ident_pair_*` call lock-checked entry points
  without api_lock. No app calls them today; the first one that does on a build
  linking `ant_radio_radiant.c` must take it.
- The ISR half of `antr_api_lock_held()` is untested (`irq_offload()` could reach
  it). A ztest `after()` failure under CONFIG_ASSERT skips the rest of the suite
  and reports pass.

Discovery (ztest only, not on air; bench checks in the release review):
- Retune restarts the cursor, so a host cycling keyed frequencies faster than a
  pass truncates each pass and coverage becomes probabilistic.
- In a mixed-frequency session a steer or revisit queued while the sweep is on the
  other frequency wastes a dwell and doubles that channel's backoff.
- When only undeclared channels remain, the sweep stays on the last declared
  frequency: a keyless scan opened after a keyed RF 66 slave sits on 66.
- The ENOENT clear of `pass_done` is unreachable through the API, so a declared
  channel acquired in the last set of a pass delays a later mixed session by up to
  a pass.
- A chunk reclaimed by A9-3 is credited as a full listen in the sweep accounting.
- Never verified on air: a slave on a non-57 frequency acquiring its master; mixed
  57/66 passes; a real Zwift session on these images.

Bridge and tools:
- Filtered TX_COMPLETED / TX_FAILED are dropped by an early return in
  `antr_on_message()` after the burst buffer is released. The bridge is in no ztest
  app; order is release first, then drop.
- Matter adoption renames and re-persists each adopted row's NodeLabel (one flash
  write each); each row can be renamed up to 2 x the number of distinct adoption
  passes for its source, bounded per boot and not recurring. Left unchanged.
- The Matter bridge fixes and the MQTT owed list have not run on air or against a
  real broker.
- `ant_compare.py:640` and `ant_sens.py:561` still show unlabelled 0x60 upper bounds.
- `radiant_sched.h` still says "Zero-cost on both nRF builds".
