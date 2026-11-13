MPSL bursts: multi-packet bursts still can't run on MPSL builds. That's a scope gap, not a regression; Zwift uses acknowledged messages.

Discovery short match (ADR 0022), left open 2026-11-11:
- [x] Gates 3 and 4 pass on nRF (2026-11-12, `archive/benchmarks/2026-11-12-gate3/`). Tracked loss B - A = -0.23 pp against the same-SoC dongle control (bound 2.16 pp), and page 7's hand-off counters (fields 3-5) stayed at zero. The "slot after the search heard the device number" counter was removed, not split: it only counts when the search hears a frame, so it grows with discovery and can't measure a cost (it read 100 % missed with the short match off too). Gate 4: about 10/s more noise matches on B, all dropped by the backend CRC, with no false devices and no new host events. All four gates pass. The nRF default flips to y after the Zwift capture below.
- [x] `radiant_search_stats` over the host: done by ADR 0023 (c5320f4), 0xF7 pages 5-6. Read them after a window only: `radiant_search_init()` zeroes them on every stack reset and key set.
- The two waste fixes (ADR 0022 "Not done here"): closed for TI, where the short match is now on by default and one set makes them moot. On nRF they matter only while the short match is off, which ends when the default flips after the Zwift capture; no scheduler "window is live" query is built.
- Deferred to the Zwift follow-up testing: the final Zwift capture with both workstreams flashed (user-run): every sensor listed within ~1 s, 0x61/0xC6 answered, reset -> NETWORK_KEY against 42-94 s (RadiANT history) and 9.2 s (Garmin).

Discovery (ztest only, not on air; bench checks in the release review):
- [x] Retune restarts the cursor: fixed on fix/discovery-open-items (B1), each
  frequency keeps its own cursor (`RADIANT_SEARCH_RF_CURSORS`).
- [x] A steer or revisit queued for the other frequency wasted a dwell and doubled
  the backoff: fixed (B2), steer entries carry their frequency.
- [x] When only undeclared channels remain, the sweep stays on the last declared
  frequency: fixed in afe3be2 (`home_rf`), test
  `test_the_sweep_returns_home_when_the_last_declared_search_ends`.
- [x] The ENOENT clear of `pass_done` was unreachable through the API: fixed
  (B4), a new session clears it in `begin_common()`.
- [x] A chunk reclaimed by A9-3 was credited as a full listen: fixed (B5), it
  credits nothing and counts `reclaimed_chunks`.
- [x] On air 2026-11-12 (`archive/benchmarks/2026-11-12-gate3/` step 7): a keyed
  slave on RF 66 acquires a Garmin master (0.06 s), and a mixed 57/66 session
  acquires both (0.06-0.09 s and 0.30-0.34 s), on both short-match images. Only
  named-device channels were tried. A real Zwift session on these images is
  deferred to the Zwift follow-up testing.


## TI CC2652R, open after the 2026-11-12 bench (evidence `archive/benchmarks/2026-11-12-ti-findings/` and `2026-11-12-ti-lead/`):
- TI cannot receive the Pro Micro clone `EE6986854FD72803`: as a simulated sensor TI lost 87.5 % of its frames, the Garmin 0.34 %, and TI lost 5.4 % of the nRF54L15 DK's. Guess: the clone's carrier offset against TI's RX bandwidth. Measure the offset, then decide whether TI's RX bandwidth should be wider. Until then, never use the clone as a TI radio peer.
- Decoy sync word `0x59C5`: 0xF7 page 4 counts its hits (`alt_sync_hits`, 1-5 per leg against hundreds of windows). Not decided; the `syncWord1 = 0` A/B is 6 x 300 s alternating tracked-loss legs.
- Not run: `tools/ab_gates.toml`'s acquisition gate for the TI short match (needs `ant_verify` T1 sittings; deferred until the decoy is decided).
- The burst ACK-window reserve (e042a10) is unmeasured on MPSL: the window now holds 1550 us more arbiter air and retries become armable in-grant. Under cc26xx coex the in-grant lead stays 600 us, so retries still cannot fit there.

## FE-C coverage (deferred from the 2026-11-11 Zwift-capture plan, one item):
- `tools/ant_fec_control.py`: the page-70 count byte is shifted (:152-156), and the incline is read at [3..4] instead of [4..5] (:171).
- Sim (`profile_fec_tx.c`, `apps/trainer`): refuses page 55 on request; ignores the page-70 "reply acknowledged" bit and the transmission count; sends page 71 in reply to a page 70, which needs checking against the spec.
- Never run on air: 0x31 ERG, 0x30 and 0x32; page 70 -> 54/55/71 round trips; acked replies from the master; Zwift against the sim; a commercial trainer's page 71. (Master-originated bursts DO run on air since 2026-11-11, against a Garmin slave - they never had before, see archive/captures/radio/README.md, "The master-burst captures". Master acked data was run on air 2026-11-12 with `ant_ack_pair.py --master-acked`: 0/100 and 2/100 failed with both ends at 0 dBm or more, see archive/benchmarks/2026-11-12-master-acked-power/.)
- Burst on an open slave channel is untested; MPSL doesn't support multi-packet burst.
- Stale docs: `docs/testing.md:296` has an ERG label on a 0x33 test; `docs/device-profiles.md:1459` says `ant_pages` has no FE-C; `docs/decisions/0001...:19-25` records a Tier 3 pass with no FE-C trace in the repo; `docs/zwift-fec-validation.md` is still "Hardware acceptance: FAIL" at 98.33 %.

## RadiANT vs Garmin as receivers at equal input (opened 2026-11-12):
The master's acknowledgement misses (closed 2026-11-12) were the link budget: 10-12 % missed with the Garmin at -20 dBm, 0 % at 0 dBm, in alternating legs. Still not known: whether a RadiANT receiver needs more signal than the Garmin's. Compare the two with `tools/ant_sens.py` at equal input. Related: the weak-signal residual after SLIP_GUARD (ADR 0019). Bench note: the Garmin's `0x60` level 4 is its weakest level, not +4 dBm; use level 3 for 0 dBm. (We don't need an exact match, that's not our goal, ours should be as sensitive or better)

## Inbound and multi-packet burst work (deferred from the Rev 5.1 review, fix/spec-rev51-gaps, 2026-11-11). Outbound retries landed there (`RADIANT_TRANSFER_TX_RETRIES_MAX`, radiant/src/radiant_burst.c `retry_packet()`). Items 1, 2, 3 and 6 landed 2026-11-12 on `feat/inbound-bursts`:
1. [x] Inbound continuation window: `RADIANT_TRANSFER_STATE_WAIT_NEXT`, a window at ack + 1550 +/- 250 us after each ACK of an open burst (and once after LAST). Empty windows repeat the ACK at +3143 us, 21 attempts. On air, Garmin -> RadiANT: 96/100 and 98/100 bursts of 8 complete against a 97/100 control the other way (archive/benchmarks/2026-11-12-inbound-burst/). Needed a second fix found on air: a master's 0x8A is answered at +2190 (slot-reply turnaround), not +1560 - with +1560 every burst failed.
2. [x] Duplicate filtering: same sequence bit as the last delivered packet -> re-ACK with the saved reply, not delivered (`stats.rx_duplicates`). A slave's packet-0 retry one period later is not deduplicated.
3. [x] EVENT_TRANSFER_RX_FAILED (0x04): from the engine (peer stopped, broadcast mid-burst, ACK repeats exhausted), on close (before CHANNEL_CLOSED), on drop to search, and the watchdog as backstop.
4. Multi-packet bursts on MPSL builds (the "MPSL bursts" line at the top of this file). Retries share the problem: a retry at +3110 us is outside the data packet's grant, and on MPSL it fails the transfer with NO_ACK as before. Inbound too: a denied continuation window ends the burst with RX_FAILED.
5. Advanced burst: true 16/24-byte packets on air (today a 24-byte block goes out as three 8-byte packets), the retry-extension byte (`api_adv_burst[10]`, stored and never read), and the stall count / retry defaults (sdk-ant reads back stall 3210).
6. [x] EVENT_TRANSFER_TX_START (0x0A): once per outbound multi-packet burst, not for acknowledged data or a one-packet burst.
7. Open: 0xAA from a master (acknowledged data, master -> our slave) is still answered at +1560; whether a Garmin master hears that is untested on air. Not changed, by the "standalone acked data stays as it is" rule.
8. Open: the no-burst tracked-loss regression leg (new image vs old, same-window control) was not run. Those paths are only reached after a non-LAST packet, so no change is expected.
Note: multi packet burst mode is a lower priority feature, and handling it should not significantly degrade the performance of primary functionality (ie reading and listening to a heart rate strap)

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

## 3. [CLASSIFIED 2026-11-07: PROBE side, not firmware; trigger unknown] 54L15 receiver's VCOM died mid-window. Evidence: archive/benchmarks/2026-11-07-vcom-soak/README.md

## 4. [DONE 2026-11-08] ant_sens reports spurious loss on short event_counter rungs

## 6. [DONE 2026-11-08] Re-run ant_features against the Garmin

# Open issues from the 2026-11-07 extensions and security review

Items 1-9 need no bench; items 10 and 11 do. `security.conf` and the compat
layer are in no release image, so every protocol change below breaks no
deployed host or node. Findings were verified by reading the code; nothing was
run.

## 1. [CLOSED 2026-11-08, accepted limits] What the Tier I phase lock does not cover

## 2. [DONE 2026-11-07] Implement the descriptor authentication frame

## 3. [DONE 2026-11-07] Version the 0xF4 status reply

## 4. [DECIDED 2026-11-07, implemented with item 2] The epoch in descriptor frame 1

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

## 10. [DONE 2026-11-11] Search is deaf through the MPSL arm lead (ADR 0018)

## 11. [DONE 2026-11-08] review/2026-10-31-timing has waited a week

## 12. [DONE 2026-11-08] Tier II loses windows at the HR seam with no packet loss

## 13. [DONE 2026-11-08] 0x60 exact loss over-counts on host-driven masters

## 14. [DONE 2026-11-08] ant_verify's liveness line is wrong for FE-C

## 15. [DONE 2026-11-08] Telemetry section 6 overstates when unsecured byte [1] advances

# Open issues from the 2026-11-08 release review (HEAD b89a112)

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

## 1. [DONE 2026-11-11] Turn off the version-1 bridge storage migration

## 2. [DONE before 2026-11-11] Version comments contradict the pinned-at-1 rule

## 3. [DONE 2026-11-11] Make backends state max_addr_groups explicitly

## 4. [FIRMWARE, bench, DEFERRED] Move nRF52/53 to USB_NEXT
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
- (Done 2026-11-11: the USB transport options and stack defaults are gone from
  apps/dongle_ti/Kconfig; the TI image still builds.)
- Check that the 9.8 KB still fits the Matter builds on nRF52840.
- Done when the checks above match legacy and no release image selects
  USB_DEVICE_STACK.

## 5. [DONE 2026-11-11] Delete compatibility shims with no remaining user

## 6. [DONE 2026-11-11] Remove the finished bisection switches

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

## 8. Cleanup the ADRs. Stop maintaining resolved troubleshooting chronology in active documentation
Users should be able to understand RadiANT as it works today without reconstructing its development history. The ADRs currently mix enduring architecture, bug investigations, measurements and proposed work, notes we don't need as we combine this into a single production release.
We should probably create one short current architecture and design principles document, supported by the existing integration, security and backend references.

There is also a broader issue: radiant-bridge.md already exceeds 2,000 lines and mixes architecture with successive measurements and fixes. It would benefit from the same editing approach; simply moving more ADR content there would compound the problem.

[treadmill-reference-design.md (line 538)](docs/treadmill-reference-design.md)	Sections 7.4a–7.9 follow successive hypotheses, bench sessions and resolved defects—including faults in the dongle rather than the treadmill. Replace that chronology with current verification procedures, supported behavior and outstanding limitations.

[bench-runbook.md (line 38)](docs/bench-runbook.md)	Organized around old project phases and the backend-transition acceptance exercise. Refine it into repeatable procedures and equipment requirements; reconcile old prerequisites against the completed transition.

[radiant-telemetry.md (line 402)](docs/radiant-telemetry.md)	A protocol reference should state the current format directly. References to contradictions in earlier drafts, original wording and fixed TODO items can disappear once the resulting rule is stated clearly.

# Left open by the 2026-11-08 clearance

MPSL (needs a soak beside BLE or Thread, which that clearance excluded):
- MPSL assert 69:108, once in about 111,000 failed extensions on the L15 beside
  an attached Thread peer (2026-11-09, docs/adr18-eval/phase-0a.md). It fired on
  the CLOCK_LOCK arm about 5 min into a run and did not repeat in a further
  30 min on the same image; the irq_lock arms saw none in 65,000. One event, so
  neither arm is implicated. Last signal EXTEND_FAILED, returned END.
- A14-1: on an MPSL grant, an ETIME from `program_rx()` is reported through
  `gate_failed` only after the grant ends, so the grant is not released early and
  its remaining air is wasted. Not fixed: no ztest reaches radiant_radio_nrf.c,
  and a soak counter saw 0 on-grant ETIMEs in 87 min beside a Thread peer.
- A pending `gate_failed` is not cleared by `radiant_radio_abort()`, so an abort
  before `denied_work_fn` runs delivers FAILED to the next operation. Self-heals;
  found by reading only. Not fixed, for the same two reasons (0 in the same soak).
- `program_tx()` writes CC_TXSTART before enabling `conn_start_tx` and re-checks
  with `t_start <= now`, the ordering A14-1 replaced for receive. A compare just
  after the enable, before `now` is read, gives a false ETIME.

Bridge and tools:
- Filtered TX_COMPLETED / TX_FAILED are dropped by an early return in
  `antr_on_message()` after the burst buffer is released. The bridge is in no ztest
  app; order is release first, then drop.
- Matter adoption renames and re-persists each adopted row's NodeLabel (one flash
  write each); each row can be renamed up to 2 x the number of distinct adoption
  passes for its source, bounded per boot and not recurring. Left unchanged.
- `ant_compare.py:640` and `ant_sens.py:561` still show unlabelled 0x60 upper bounds.
- `radiant_sched.h` still says "Zero-cost on both nRF builds".

## Device Testing Open Items

Make continuous dual transmission the HR default (CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED=n). 

Apple testing round 2
