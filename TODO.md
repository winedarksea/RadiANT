MPSL bursts: multi-packet bursts still can't run on MPSL builds. That's a scope gap, not a regression; Zwift uses acknowledged messages.

Discovery short match (ADR 0022), left open 2026-11-11:
- The two waste fixes (ADR 0022 "Not done here"): closed. The short match is on by default on TI (2026-11-12) and nRF (2026-11-13), and one set makes them moot; they matter only for `long_match.conf` images. No scheduler "window is live" query is built.
- Zwift acceptance of the nRF default flip (user-run, not a precondition any more): every sensor listed within ~1 s while HR and trainer are tracked, 0x61/0xC6 answered, reset -> NETWORK_KEY against 42-94 s (RadiANT history) and 9.2 s (Garmin). If it passes, mark ADR 0022 accepted; if it fails, rebuild with `apps/dongle/long_match.conf` and investigate.
- Decided 2026-11-14 (owner): short match stays default y on EVERY nRF image (MPSL, Matter, nRF5340 net core, L15), none of which had its own gate run; no CI job for `long_match.conf` (declined).

## Open after the 2026-11-16 benches (filed on fix/ti-cmd-stall):
- nRF: re-read `now` on STEP_RETRY after an early-ETIME refusal; the stale `now` makes the refusals cascade (`archive/benchmarks/2026-11-16-b4-pair/`).
- nRF: `radiant_frame.c` (`rx_sw_addr_len` / `radiant_addr_alt_run` bit loop) is outside the O2 hot set, ~17 us per filter on a filter-memo miss.
- TI and every backend: the scheduler's `pass()` masks interrupts for its whole body. TI 9-channel worst case 492 us, arm slack 19-26 us. Candidates: run done callbacks and event posting after the unlock; take `RF_scheduleCmd` out of the lock (`archive/benchmarks/2026-11-16-ti-cmd-stall/isr_blocker/`).
- TI: Pro Micro -> TI link loses 8-13 % (other links 0.25-4 %), unexplained; not RX buffering and not the masked pass (`archive/benchmarks/2026-11-16-b4-pair/gate_extra/`).

## FE-C coverage (deferred from the 2026-11-11 Zwift-capture plan, one item):
- `tools/ant_fec_control.py`: the page-70 count byte is shifted (:152-156), and the incline is read at [3..4] instead of [4..5] (:171).
- Sim (`profile_fec_tx.c`, `apps/trainer`): refuses page 55 on request; ignores the page-70 "reply acknowledged" bit and the transmission count; sends page 71 in reply to a page 70, which needs checking against the spec.
- Never run on air: 0x31 ERG, 0x30 and 0x32; page 70 -> 54/55/71 round trips; acked replies from the master; Zwift against the sim; a commercial trainer's page 71. (Master-originated bursts DO run on air since 2026-11-11, against a Garmin slave - they never had before, see archive/captures/radio/README.md, "The master-burst captures". Master acked data was run on air 2026-11-12 with `ant_ack_pair.py --master-acked`: 0/100 and 2/100 failed with both ends at 0 dBm or more, see archive/benchmarks/2026-11-12-master-acked-power/.)
- Burst on an open slave channel is untested; MPSL doesn't support multi-packet burst.
- Stale docs: `docs/testing.md:296` has an ERG label on a 0x33 test; `docs/device-profiles.md:1459` says `ant_pages` has no FE-C; `docs/decisions/0001...:19-25` records a Tier 3 pass with no FE-C trace in the repo; `docs/zwift-fec-validation.md` is still "Hardware acceptance: FAIL" at 98.33 %.

## RadiANT vs Garmin as receivers at equal input (opened 2026-11-12):
The master's acknowledgement misses (closed 2026-11-12) were the link budget: 10-12 % missed with the Garmin at -20 dBm, 0 % at 0 dBm, in alternating legs. Still not known: whether a RadiANT receiver needs more signal than the Garmin's. Compare the two with `tools/ant_sens.py` at equal input. Related: the weak-signal residual after SLIP_GUARD (ADR 0019). Bench note: the Garmin's `0x60` level 4 is its weakest level, not +4 dBm; use level 3 for 0 dBm. (We don't need an exact match, that's not our goal, ours should be as sensitive or better)

## Inbound and multi-packet burst work (deferred from the Rev 5.1 review, fix/spec-rev51-gaps, 2026-11-11). Outbound retries landed there (`RADIANT_TRANSFER_TX_RETRIES_MAX`, radiant/src/radiant_burst.c `retry_packet()`). Items 1, 2, 3 and 6 landed 2026-11-12 on `feat/inbound-bursts`:
1. [x] Inbound continuation window: `RADIANT_TRANSFER_STATE_WAIT_NEXT`, a window at ack + 1550 +/- 250 us after each ACK of an open burst (and once after LAST). Empty windows repeat the ACK at +3143 us, 5 times by default since 2026-11-13 (`CONFIG_RADIANT_INBOUND_ACK_REPEATS`, 0..20; it was 21 attempts). On air, Garmin -> RadiANT: 96/100 and 98/100 bursts of 8 complete against a 97/100 control the other way (archive/benchmarks/2026-11-12-inbound-burst/). Needed a second fix found on air: a master's 0x8A is answered at +2190 (slot-reply turnaround), not +1560 - with +1560 every burst failed.
2. [x] Duplicate filtering: same sequence bit as the last delivered packet -> re-ACK with the saved reply, not delivered (`stats.rx_duplicates`). A slave's packet-0 retry one period later is not deduplicated.
3. [x] EVENT_TRANSFER_RX_FAILED (0x04): from the engine (peer stopped, broadcast mid-burst, ACK repeats exhausted), on close (before CHANNEL_CLOSED), on drop to search, and the watchdog as backstop.
4. Multi-packet bursts on MPSL builds (the "MPSL bursts" line at the top of this file). Retries share the problem: a retry at +3110 us is outside the data packet's grant, and on MPSL it fails the transfer with NO_ACK as before. Inbound too, measured 2026-11-13: Garmin -> L15 `gate.conf`, 100/100 bursts failed with only packet 0 delivered (`archive/benchmarks/2026-11-13-inbound-burst-gaps/` section 1). Giving each ACK its own reserve does NOT fix it: the gate serves an in-grant arm only if that arm's own reserve also ends inside the current grant, and fixed-class grants never grow, so the whole chain must nest inside the first grant (tracked window + 4150 us). The fix, for both directions, is on-demand extension of fixed-class grants in `radiant_radio_nrf_gate_mpsl.c`, ending the burst when an extension is refused. Also belongs here, by the owner's decision of 2026-11-14: the scheduler/gate attribution of a retry or next packet refused after an in-grant ACK window (EDENIED from the gate instead of MISSED from the scheduler; "Left open by the 2026-11-08 clearance" below). MPSL assert 69:108 was not the EXTEND machinery: it was a RADIO interrupt left pending at END (fixed 2026-11-14, 63b2a02; "Left open by the 2026-11-08 clearance" below), so `ACTION_EXTEND` is not implicated.
5. Advanced burst: true 16/24-byte packets on air (today a 24-byte block goes out as three 8-byte packets), the retry-extension byte (`api_adv_burst[10]`, stored and never read), and the stall count / retry defaults (sdk-ant reads back stall 3210).
6. [x] EVENT_TRANSFER_TX_START (0x0A): once per outbound multi-packet burst, not for acknowledged data or a one-packet burst.
7. [x] 0xAA from a master is answered on the slot-reply turnaround (+2190) since 2026-11-13 (442d4f7). Sniffed first: at +1560 a Garmin master was told FAILED for 50/50 messages our slave had received; at +2190, 96/100 against a 99/100 control. Open: no non-RadiANT slave's 0xAA reply offset has been recorded; in the fixed leg the slave also counted rx_fail 8 and one go_to_search on broadcasts (control 2, 0), not investigated.
8. [x] No-burst regression leg run 2026-11-13: B inside the A windows' range against both the same-SoC dongle and the Garmin (`archive/benchmarks/2026-11-13-inbound-burst-gaps/` section 4).
9. [x] Landed 2026-11-13 (d507ae0): the guard widens for periods a burst stepped over (sniffed: a Garmin master keeps its grid across a burst, so this is margin); ACK-fault MUTE/DEAF behave as lost-ACK / not-heard inside a burst; the second-channel test overlaps the burst by construction; a 64-packet burst completes 20/20 with the event queue at high-water 1 of 64.
10. Open, seen on air 2026-11-13: an 0x8A from a Garmin master sometimes gets no answer at all (1/20 at normal power, three in a row in the lossy leg) while the sniffer hears it clearly. Also: a Garmin master does not retry a slot-opening 0x8A in the slot when its ACK is lost, so our ACK repeats after a lost opener ACK are always wasted (5 x 3.1 ms).
Note: multi packet burst mode is a lower priority feature, and handling it should not significantly degrade the performance of primary functionality (ie reading and listening to a heart rate strap)

# Open issues from the 2026-11-14 security review (HEAD 360e12f)

## 1. [DONE 2026-11-14: fecac17, 66bc189, 175a495] The search-acquisition frame skips the security hook

## 2. [DONE 2026-11-14: 1a784e6, b4e0450] Two live channels under the node root can meet on one epoch

## 3. [DONE 2026-11-14: e8633a0] Plaintext can reach an acknowledgement through a two-statement race

## 4. [DONE 2026-11-14] Hardening

## 5. [OPEN, protocol, v2] Two live channels under one root and one device number
- Item 2 refuses this (for example two device types on one Tier 0 number). Allowing it needs the device type in the key derivation or the nonce, which is a protocol change and not v1. Not built.
- Related, judged safe in review: a spread-MAC channel and a compat channel under the same root and base number are not checked against each other. They share `K_auth` at one epoch, but the MAC domains differ (`DOM_COMPAT_MAC` vs `DOM_SPREAD_MAC`/`DOM_CTR`) and compat has no `K_enc`. Re-check if either MAC input format changes.

## 6. [DONE 2026-11-16: 2cf4a2a] The search chunk keeps listening after its only channel acquires

## 7. [CLOSED 2026-11-16: 7a fixed (6bb8a1d), 7b deferred, 7c accepted] Gaps left by the 2026-11-14 fixes
- 7a [DONE]: A fresh arm whose `radiant_sec_set_epoch()` fails inside `derive_keys()` is covered by item 2's rule (`epoch_set` counts), but untested: forcing it needs a KDF fault seam. Done: `radiant_sec_test_kdf_fault()`, a `CONFIG_ZTEST`-only weak seam in `derive_keys()` (the compat `kid_fault` pattern). The new node_ident tests found a real gap: the arm failed and the twin stayed refused, but under X_AUTH alone the channel kept sending secured pages with no MAC behind them (X_CONF already refused at the empty `K_enc`). A failed derive now sets `tx_spent`, which `set_epoch()` clears on the next epoch. The auth variant failed before the fix. Sizes: security text 7308 -> 7316 of 7680; the security-off image is unchanged (91296).
- 7b [DEFERRED]: Compat `set_key` releases the context when a re-import fails after the old root is destroyed. The portable backend cannot reach that branch; an opaque backend can. Fail-closed by reading (the context is released, so nothing is keyed); test it when an opaque (PSA) backend lands.
- 7c [ACCEPTED]: A keyed background scan under X_AUTH rarely completes a window, and all senders share one window state (spec 3.2). Accepted with the owner's decision to allow keyed scans. A per-sender window state would fix it, at RAM cost.

## 8. [CLOSED 2026-11-16, owner: host trusted, documented (6bb8a1d)] The twin guard runs only on a node arm (reviewer, 2026-11-15)

## 9. [OPEN, LOW, core search, follow-up to item 6] A carrier that acquires while other channels search loses frame 1
- Item 6 cuts the sweep chunk only when nobody else searches. When the carrier acquires while another channel is still searching, the chunk keeps the carrier's slot until it ends (about one period plus 10 ms), so the carrier's frame 1 can still be dropped (when acquired early in a dwell, item 6's bench) and that first window is then UNVERIFIED under X_AUTH. Cutting there would cost the other searchers a re-arm gap. The fix candidate is moving the sweep to another searching channel's slot (a carrier handover) without a gap. It needs the scan/sweep regression checks. Background-scan rigs (the Zwift model) never hit this, because a scan channel never acquires and pinned channels are never the carrier.

# Open issues from the 2026-11-13 security review (HEAD 1df743a)

## 1. [DONE 2026-11-13, merge 9bfb0c0: d463c01, caabace, 802ecc2] A counter wrap uses the epoch the next boot will be issued

## 2. [DONE 2026-11-13, merge 9bfb0c0: 6106ca6] One injected frame desynchronises a secured master for good

## 3. [DONE 2026-11-13, merge 1874e29, then 927cb75..d4b7bdd] The per-packet verdict never reaches the host

## 4. [DONE 2026-11-13 for core pairing and 0xF5, merge 1a0031f: 164ff11..80b303c; over-air enrolment is documented only, see item 6] A computing MITM can make the six-digit fingerprints match

## 5. [DONE 2026-11-13: 4d5d156, 55bf4c3, 4c12ebf, db761cc] A keyed but unarmed channel fails open in both directions

## 6. [DONE 2026-11-13: f330469, 4544e5c, 9be01e0; additive design and its section 11.7 trade-off ACCEPTED by the owner 2026-11-14] Over-air enrolment replaces the group root instead of delivering it

## 7. [DONE 2026-11-13: c4cfe94] Compat epoch recovery allows a rollback

## 8. [DONE 2026-11-13: e111889, e7024ce] Tier II windows have no replay floor

## 9. [DONE 2026-11-13: 4d5d156, 583a981; same-root 0xF2 no longer resets a Tier 2 trial devnum, 2026-11-14: b657458] 0xF2 resets the epoch monotonicity gate

## 10. [DONE 2026-11-14: de2df63] A go-private/return command is replayable within its interval

## 11. [DONE 2026-11-14: 24cd0e3] Non-broadcast traffic on a secured channel

## 12. [DONE 2026-11-14: 3b8e309, dff88e6, 501370a] Hardening and documentation

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

## 1. [DONE 2026-11-07] Receive loss that depends on the device number

## 3. [CLASSIFIED 2026-11-07: PROBE side, not firmware; trigger unknown] 54L15 receiver's VCOM died mid-window. Evidence: archive/benchmarks/2026-11-07-vcom-soak/README.md

## 4. [DONE 2026-11-08] ant_sens reports spurious loss on short event_counter rungs

## 6. [DONE 2026-11-08] Re-run ant_features against the Garmin

# Open issues from the 2026-11-07 extensions and security review

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
- The scheduler and the MPSL gate still disagree about a clipped follow-on
  (review 2026-11-14 of 8650411). `in_reserve()` (radiant_sched.c) sets
  `reserve_end` to the last operation's close + follow_on_us whether or not the
  gate could hold it, so after an in-grant ACK window a retry or a burst's next
  packet is armed at the in-grant lead and the gate refuses it EDENIED (counted
  as `arm_denied`, `refused_too_near`, "gate: too near" printk x3) instead of the
  scheduler reporting MISSED. The outcome (transfer fails) is the same as before
  e042a10; only the attribution blames the other stack. A fix needs a capability
  saying follow-ons do not chain (cc26xx extends its endTime, so clipping in the
  scheduler would be wrong there) and belongs with grant extension (item 4).
  Deferred there by the owner, 2026-11-14. Since e66ae02 a one-packet
  transfer's ACK window reserves nothing, so acknowledged data no longer
  reaches this path; only multi-packet bursts do.
- [x] MPSL assert 69:108 - RESOLVED 2026-11-14 (63b2a02; hardening 1c8967b;
  evidence `archive/benchmarks/2026-11-15-assert-69108/`). It had been
  reproducible in minutes (`archive/benchmarks/2026-11-14-mpsl-soak/`): L15
  `thread.conf` beside an attached OpenThread leader, one tracked and one
  searching channel, 1.5-6.3 min after boot in every leg. Cause: MPSL took a
  RADIO interrupt after our timeslot had ended. A RADIO END or DISABLED event
  of the live receiver that fires while a TIMER0 (or RADIO) signal handler runs,
  at the same priority, only pends RADIO_0 in the NVIC.
  `radiant_nrf_gate_on_grant_end()` cleared our INTENSET bits and disabled the
  radio but left the NVIC pending bit set; its comment assumed our own
  `radio_isr()` would drop it. Under MPSL the vector is MPSL's, so after
  `ACTION_END` closed the timeslot the latched interrupt was taken with no
  timeslot open and MPSL asserted. A scan chunk ended by the ext-skip compare
  cuts a live receiver every 10 ms, which is why the scan workload reproduced it;
  the short match (more RADIO END events) raised the rate. It does not depend on
  the booking design (ADR 0021) or on the ext skip.
  Stage 1 dump (legs `base1`, `base2`, `ctrl3`; image built from abd6be7):
  `ipsr=154 ctx=RADIO`, 18-19 us after the END returned from the ext-skip TIMER0
  path, `ts_live=0`, radio disabled, SHORTS 0, every RADIO DPPI endpoint 0,
  TIMER0 INTENSET 0, no MPSL API call in flight, HFXO tuned (`xo_stat=0x10001`,
  no tune error). Asserts at 185 s, 224 s and 102 s after boot.
  Fix: every high-priority `ACTION_END` in `on_signal()` now ends by calling
  `radiant_nrf_gate_radio_irq_unpend()`, which clears the RADIO vector's NVIC
  pending bit if this timeslot took the radio. It is not called on the
  low-priority hand-backs (BLOCKED/CANCELLED, SESSION_IDLE, SESSION_CLOSED,
  `gate_shutdown()`), where the radio may be another stack's. Nordic's 802.15.4
  driver does the same at its hand-over (`nrf_802154_trx.c`). Test:
  `radiant/tests/gate/src/test_gate_radio_unpend.c`, 10 cases, 6 of which fail
  against the previous gate.
  Confirmation (legs `fix1`-`fix3`, image `fix_63b2a02`, 30 min each, same
  sitting as control `ctrl3`, which asserted at 102 s): 0 asserts and 0 reboots;
  an END found the RADIO vector pending 14, 18 and 11 times per leg (UNPEND
  counters: t0skip 13/14/11, extfail 1/4/0) out of about 65,560 grants per leg;
  ch0 tracked loss DUT 0.357 / 0.358 / 0.288 % against Garmin 0.856 / 0.385 /
  0.468 % in the same windows.
  Hardening from the independent review (1c8967b; test
  `radiant/tests/gate/src/test_gate_hardening.c`, 6 cases): low-priority
  signals return a dedicated always-NONE struct instead of the per-session one
  the zero-latency invocation writes; the "this slot took the radio" flag is
  cleared at every SIGNAL_START; `timer0_disarm()` outside a high-priority
  signal writes MPSL_TIMER0 only while a COMPARE0 interrupt we enabled is still
  armed. The STATS-only `skip_left_min` capture, which wrote TIMER0 CC1 after the
  hand-back, was removed (90ba241), and so were the bench arm switches added in
  abd6be7. The Stage 1 instrumentation stays under
  `CONFIG_RADIANT_GATE_MPSL_STATS`. Cost with STATS off: +56 B text for the fix,
  +88 B for the hardening. Bluefin native_sim at 1c8967b: 16 scenarios, 6124
  cases passed, `check_test_counts` OK, `radiant.gate` 93 cases.
  Ruled out by the Stage 1 dump: HFXO/clock, API concurrency or locking, the
  60 s calibration tick, the DPPI hand-back order, the STATS-only TIMER0 CC1
  capture. The four earlier explanations (extension refusals, the skip, the
  early release, the short match) were symptoms of the rate, not causes. Stage 2
  of the plan (the arm table) was not run: Stage 1 named the mechanism.
  Containment (leg `contain1`, `thread.conf;bridge.conf`, image before the fix,
  `CONFIG_RESET_ON_FATAL_ERROR=y`): the assert fired once at 497 s and the board
  cold-rebooted within about 1 s, no halt, and was attached to the Thread leader
  again as the same child. Not shown: the time to re-attach, and ANT traffic
  after the reboot (a host-driven dongle's channels are the host's to reopen,
  and the runner did not reopen them). `thread.conf` alone still sets
  `CONFIG_RESET_ON_FATAL_ERROR=n`, so a bare `thread.conf` build halts.
  Nordic: a report draft is in the evidence directory
  (`nordic-report-draft.md`); posting it is the owner's decision. DevZone thread
  128550 reports the same `69, 108` from Nordic's ESB timeslot code.
  Stage 4 acceptance (2026-11-14, images at 1c8967b): 2 h `thread.conf` soak, 1 h
  gate-only, 1 h Matter, 30 min coex, all 0 asserts and 0 reboots; the 2 h leg
  cleared 45 pending RADIO vectors and stayed attached. The image before the fix
  asserted at 105 s in the same sitting. Table and caveats in the evidence
  README. The Zwift image (`build\zwift_mpsl\zephyr.hex`) is on the L15.
  Left from acceptance:
  - [ ] The coex leg had no BLE load (`coex.conf` never calls `bt_enable`), so
    the fix beside live BLE traffic is untested. Rebuild with `ble_load.conf`.
  - [ ] Master-originated acked data on the gate image: 58/60 twice, against
    60/60 on 2026-11-14 (two missed acks per run). No same-sitting run of the
    image before the fix, so not attributed.
  - [ ] 2 h Thread leg: DUT ch0 loss 1.87 % against the Garmin's 1.43 % in the
    same window (DUT below the Garmin in the other three legs), and the child's
    age read 210 s of a 240 s timeout at the end of the leg.
  Open items found on the way, not fixed here:
  - [ ] DPPI hand-back order. Beside OpenThread the grant-boundary endpoint swap
    moves only PUBLISH_ADDRESS and SUBSCRIBE_RXEN, so RADIO
    SUBSCRIBE_DISABLE/TXEN/RSSISTART stay wired to radiant's DPPI channels
    between grants. EXTEND_FAILED and the ext-skip TIMER0 path call
    `timer0_disarm()` (hand-back) before `radiant_radio_abort()` disables the
    operation's connections, and a TIMER0 END that cuts a live operation
    (extendable grant whose last step is below the extension minimum, or a
    fixed backstop) leaves them enabled until `denied_work_fn()` delivers the
    terminal. A CC_CLOSE or CC_TXSTART compare landing in that gap would
    trigger DISABLE/TXEN on the 802.15.4 driver's radio. Ruled out as the cause
    of 69:108 (every RADIO DPPI endpoint read 0 at the assert); still a latent
    hazard to the other stack's radio on the plain TIMER0 END that cuts a live
    operation. Untested. A test: disable the four connections before the
    hand-back on every TIMER0/EXTEND_FAILED END.
  - [ ] In-grant `gate_acquire()` returns GATE_GRANTED unmasked and the caller
    then programs the radio from SWI or a thread. A TIMER0 backstop END between
    the two leaves `program_ed()` enabling RXREADY in INTENSET (only with
    `CONFIG_RADIANT_ED_SCAN`, default n) or `program_rx()` writing
    SHORTS/connections on a radio no longer ours. Improbable (end margin);
    found by review, not observed.
  - [ ] The symmetric case: a TIMER0 COMPARE0 that fires while a RADIO signal
    returns END can leave TIMER10 pending after END. Not cleared on purpose
    (MPSL uses other CCs on that timer). No TIMER-context assert has been seen.
  - [ ] Latched RADIO EVENTS_* (END, ADDRESS, READY) are left set at hand-back.
    Harmless beside the 802.15.4 driver (it resets the radio); unknown beside
    the SoftDevice Controller.
  - [ ] No retained record of an MPSL assert across the reboot. The reset-cause
    line reads `0x00000002` after both a flash and the assert reboot, so it does
    not identify an assert; `apps/common/diag_flash_log.c` is not built into
    `apps/dongle_thread`, and flash writes from the assert context are unsafe
    under MPSL. A `__noinit` RAM record read at the next boot was proposed and
    not implemented.

## Device Testing Open Items

Make continuous dual transmission the HR default (CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED=n). 

Apple testing round 2

# From the 2026-11-15 Zwift captures (firmware 3b486c3)

Captures `captures/zwift-20261115-*.pcap`, plus the shim log `captures/zwift-shim-ti-20261115.log`:
- `214118`: TI over the shim.
- `214722`: nRF52840 Dongle, MPSL gate.
- `215313`: Pro Micro, including Lock Watts.
- `215820`: Pro Micro, workout.
- `220629`: Garmin USB-m, ERG, capture started mid-session.

The trainer is a Wahoo Kicker #52233 on 2017 firmware; it never sends page 0x47. None of the faults below was visible in Zwift.

## 1. [PARTLY DONE; simulated-master bench 2026-11-17 clean, real-master run (plan step 3) NEXT] The FE-C channel goes deaf when its slot crosses the power meter's
Done (ADR 0024): two slots that cannot both be heard now take turns losing (`turn_candidate()` in radiant_sched.c), and a slot the scheduler dropped is an EVENT_CHANNEL_COLLISION, not an RX_FAIL, and never counts toward search. So a crossing can no longer send FE-C to search on its own, whatever its cause. 0xF7 page 11 (scheduler contention) and `decode_pcap.py --phase A B` are in.

Open: what makes the 0.57-2.67 ms band unservable. `test_two_masters_drifting_through_each_other` (radiant/tests/api) does not reproduce it on the nRF-direct or TI preset, with commands, frame latency, jitter or scattered loss; FE-C never loses two in a row there, before or after the change. So it is backend timing the fake does not model, or the real masters. Next: the two-master bench below, on the Pro Micro then the TI, reading page 11 and `--phase` across >= 4 crossings; then one Zwift ride (`decode_pcap.py --phase 3 2`, no 8-miss run, 0x33 completion at crossings equal to the rest of the ride). The booking path (MPSL) still books earliest-first.

2026-11-17, simulated masters (plan step 4, `archive/benchmarks/2026-11-17-fec-crossing/`, tool `tools/ant_crossing.py`): apps/trainer at 8192 (#52235, because the real Kicker was on the air) plus ant_sim power #20329 at 8182, received at once by the Pro Micro (diag, master 27a2a15), the TI (diag a63489c5) and the Garmin (FE-C only), 2 x 1200 s, 12 crossings. **The dongle serves the crossing:** FE-C lost 6-8 of 117 in-band slots per receiver, every one within +-0.2 ms of power's frame. That is on-air overlap: the FE-C-only controls lose the same slot. No run was longer than 2, and nothing was lost in the 0.57-2.67 ms band. ADR 0024 on hardware: `slots_collided` 1-3 per crossing and 0 elsewhere, `turn_yields`/`arm_rejected` 0. So the Zwift loss needs the real masters.
- NEXT (owner pedals ~10 min, Zwift closed): plan step 3, recipe in `C:\Users\Colin\.claude\plans\fec-crossing-handoff-2026-11-17.md`. The Pro Micro still carries the diag image for it.

Original report:
Power #20329 has period 8182 and FE-C has 8192, so their slots line up every ~204 s. Each crossing costs FE-C most of its frames while power barely loses:
- TI, 130–138 s: 82 % FE-C loss inside ±5 ms, and GO_TO_SEARCH.
- Pro Micro (215313), 208–213 s: 13 of 21 FE-C slots lost, 8 of them in a row, then GO_TO_SEARCH and two failed commands.
- Pro Micro (215820), 106–119 s: 33 % loss, against 18 % for the rest of the ride.

FE-C lost every slot whenever power was 0.6–2.7 ms ahead of it. At 211.236 s (215313) both frames arrived 0.27 ms apart, so one window can catch both.

The Garmin loses here too, and reports EVENT_CHANNEL_COLLISION. But the channel that carries commands should not always be the one that loses. The suspect is the merge, priority and preempt code in `arm_rx_window()` (radiant_sched.c, rules 3–5, `RADIANT_SCHED_MERGE_SPAN_MAX_US`). The MPSL Dongle shows a smaller version of the same thing: FE-C slots within 3 ms of another channel were heard 55–60 % of the time, against 82 % otherwise.

- Reproduce with two simulated masters at 8182 and 8192 (apps/trainer). Run it on the Pro Micro, and on the TI built with `ti_diag.conf` so the 0xF7 counters are available.

## 2. [DONE 2026-11-16, branch fix/ti-sync-separation] The TI dual-sync merge ignores min_filter_hamming_bits

## 3. [FIXED 2026-11-17, bench PASS on Pro Micro and TI; Zwift acceptance open] A pending slave acked command outlives the channel's search

## 4. [BENCH] The MPSL Dongle's FE-C collapse: confirm it is RF, then measure the MPSL cost
On the Dongle only 20 of 222 FE-C commands completed; the Pro Micro managed 181 of 213 and 174 of 200. The Dongle heard the Kicker about 8 dB weaker (-75 against -67 dBm), on all of the Kicker's channels, while the other sensors were equal on both sticks. FE-C spent 61 % of the ride searching, and 174 of the 202 failures were opener timeouts.

Two things remain open:
- When the master frame was heard, 57 % of commands got no ACK back (26 of 46), against 11–13 % on the Pro Micro.
- At equal RSSI, power lost 17 % on the Dongle against 9 % on the Pro Micro, and the scan heard other sensors at 57 % of the Pro Micro's rate. That fits the MPSL arm-lead deafness, but board and placement are confounded.

There was no MPSL assert and no re-enumeration.
- Put the PCA10059 on the Pro Micro's port, in the same position and orientation. Gate on Kicker RSSI near -67 dBm.
- Then A/B the direct image against `gate.conf` in that position. Compare power loss, scan rate, FE-C reacquire time and the "heard, no ACK" rate.

## 5. [FIRMWARE, LOW] STARTUP_MESG takes 53–59 ms after SYSTEM_RESET
The Garmin answers in 1 ms. 53–59 ms sits right at the libusb0 short-read race, where a read near reply latency loses the frame (1.1 % at 50 ms). Find what delays the reply and shorten it.

## 6. [DONE 2026-11-16, branch tools/shim-log-and-decode-pcap] Zwift shim logging

## 7. [DONE 2026-11-16, branch tools/shim-log-and-decode-pcap] decode_pcap.py gaps (slot loss from the RX timestamp: DONE 2026-11-16, `--phase A B`)

## 8. [DONE 2026-11-16, ADR 0024] No EVENT_CHANNEL_COLLISION

Settled by these captures, nothing to do:
- The 0x61 and 0xC6 replies work: reset to first NETWORK_KEY now takes 11–15 s, against 42–94 s before.
- Keep accept-newest with the 3-period expiry. The Garmin refuses newer commands with `4f 1f`: 49 % of ERG targets were discarded, and its ACK success per attempt was 32 % against RadiANT's 89 %.
- The trainer followed targets that were reported as TX_FAILED, on both the Garmin and the Pro Micro. Most failures are lost return ACKs.
- Lock Watts is a short ERG window: page 0x31 for 5–25 s, then back to 0x33.
- Kicker quirks:
  - It never sends page 0x47.
  - In ERG it reports modelled power.
  - Cadence is always 0xFF.
  - It reports 0 W on leaving ERG.
  - It also broadcasts as power (type 0x0B, transmission type 0xA5) and speed (type 0x7B).
- Power meter #20329 reads about 35 % below the trainer. It is probably not on the same bike (actually it is, the trainer is really that inaccurate).

Make sure there is an RGB activity version of the feather, and the other version then is no LEDs at all (quiet mode)
