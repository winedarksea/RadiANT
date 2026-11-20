MPSL bursts: multi-packet bursts still can't run on MPSL builds. That's a scope gap, not a regression; Zwift uses acknowledged messages.

Discovery short match (ADR 0022), left open 2026-11-11:
- The two waste fixes (ADR 0022 "Not done here"): closed. The short match is on by default on TI (2026-11-12) and nRF (2026-11-13), and one set makes them moot; they matter only for `long_match.conf` images. No scheduler "window is live" query is built.
- Zwift acceptance of the nRF default flip (user-run, not a precondition any more): every sensor listed within ~1 s while HR and trainer are tracked, 0x61/0xC6 answered, reset -> NETWORK_KEY against 42-94 s (RadiANT history) and 9.2 s (Garmin). If it passes, mark ADR 0022 accepted; if it fails, rebuild with `apps/dongle/long_match.conf` and investigate.
- Decided 2026-11-14 (owner): short match stays default y on EVERY nRF image (MPSL, Matter, nRF5340 net core, L15), none of which had its own gate run; no CI job for `long_match.conf` (declined).

## Open after the 2026-11-16 benches (filed on fix/ti-cmd-stall):
- [DONE 2026-11-20] Re-read `now` on STEP_RETRY after an early-ETIME refusal; the stale `now` made the refusals cascade (`archive/benchmarks/2026-11-16-b4-pair/`). Fixed in the core scheduler, so every backend (14bd0f5 red case, 0397411 fix): `arm_failed()` sets `s.renow`, `pass()` re-reads before the next step and before `book_ahead()`; `pb.now` stays the first read. Gate is the ztest `test_an_etime_retry_plans_from_the_time_after_the_refusal`; no bench A/B (about 1 refusal per 900 s).
- nRF: `radiant_frame.c` (`rx_sw_addr_len` / `radiant_addr_alt_run` bit loop) is outside the O2 hot set, ~17 us per filter on a filter-memo miss.
- TI and every backend: the scheduler's `pass()` masks interrupts for its whole body. TI 9-channel worst case 492 us, arm slack 19-26 us. Candidates: run done callbacks and event posting after the unlock; take `RF_scheduleCmd` out of the lock (`archive/benchmarks/2026-11-16-ti-cmd-stall/isr_blocker/`).
- TI: Pro Micro -> TI link loses 8-13 % (other links 0.25-4 %), unexplained; not RX buffering and not the masked pass (`archive/benchmarks/2026-11-16-b4-pair/gate_extra/`).

## FE-C coverage (deferred from the 2026-11-11 Zwift-capture plan, one item):
- [DONE 2026-11-20] `tools/ant_fec_control.py`: page-70 count byte no longer shifted (bits 0-6 of byte 5), page-17 incline read at [4..5]; both match `profile_fec_tx.c`.
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

# Open issues from the 2026-11-20 security review (HEAD c2ec3a1)

- [ ] **Make the X25519 conditional swap constant-time in the compiled pairing build.** The swap in `radiant/src/ext/radiant_x25519.c` uses a mask, but Zephyr SDK 1.0.1 GCC 14.3.0, compiling this revision with the Feather nRF52840 pairing flags (`-mcpu=cortex-m4 -Os`), emits a conditional branch on the swap argument inside the limb loop. The ladder derives that argument from the private scalar. This establishes secret-dependent control flow on that target. Practical key recovery was not assessed. Use a vetted constant-time swap or backend and check the generated code for each supported pairing toolchain and optimization level alongside the existing X25519 vectors.
- [ ] **Validate saved Matter IDs and labels before copying them.** The V2 loader in `apps/dongle_thread/src/matter/core/bridge_storage_manager.cpp` checks the source record length but copies stored lengths into fixed-size destination arrays. A malformed local settings record can overwrite the stack during `RestoreBridgedDevices()` at boot. The optional V1 migration has the same label copy. Check each length against the remaining record using subtraction and against the destination capacity, reserve and write the string terminator, and enforce the same limits on store. Cover valid boundary lengths and rejection of malformed records while other endpoints still restore. Direct control of those records from ANT or MQTT was not established.
- [x] [DONE 2026-11-20, d0bfe65 red + 8889073 + 316801b: the tag and the ack tag bind a 16-bit extended sequence the node infers from the wire byte (ext = seq on the first lap, so old tags are unchanged); the window is 1..127 ahead, 1..128 behind is an authenticated 0x02; ext above 65408 is refused (0x02, `seq_exhausted`); an equal epoch keeps replay state, an older one is -EINVAL; `profile_cmd_tag_ext()` for senders; spec section 9 rules 3-5] **Prevent command replay within one epoch.** In `radiant/src/profiles/profile_command.c`, the MAC binds an eight-bit sequence and the receiver accepts a forward distance of 1–64 modulo 256. As a busy stream advances, a previously executed command can become eligible again with its original valid tag. Reapplying the current epoch through `profile_cmd_set_epoch()` also clears the duplicate history. This affects library users wiring `profile_cmd_on_command()` to an actuator. The in-tree callers are tests. Authenticate an extended counter or enforce an epoch change before any sequence can become eligible twice. Preserve replay state on an equal epoch, reject rollback, and cover both wrap and setter retries with an execute-once regression.
- [ ] **Keep stale commands from resetting the bad-tag backoff.** In the same command handler, `backoff_clear()` runs after MAC verification and before the duplicate and stale-sequence checks. A previously authenticated command therefore clears the accumulated penalty even when the receiver rejects its sequence. This weakens the rate limit relied on by the 16-bit standard-frame tag. Clear the penalty only on a fresh accepted sequence or decay it with time. Cover duplicate and stale traffic between failed verifications.
- [ ] **Discard staged master traffic when replacing the root key.** A real `0xF2` rekey makes the channel pending and purges queued RX data in `radiant/src/radiant_sec.c`. The host adapter in `radiant/src/antr/ant_sec_host.c` leaves the master's already scheduled broadcast and cached acknowledgement payload in `ant_radio_radiant.c` intact. Bytes prepared under the retired key can still leave after the host receives success. Apply the staged-slot cancellation and acknowledgement-cache invalidation used for a changed `0xF1` policy to a real rekey as well. Preserve the identical-key retry and test the interval before the next pump.
- [ ] **Redact key material from diagnostic traces across configurations.** In `apps/common/ant_serial_bridge.c`, tracing precedes dispatch, the root-key and pairing masks depend on feature flags, and the legacy encryption-key message has no mask. A trace-enabled dongle can therefore log a key even when it rejects the command. The Windows shim also hex-dumps host writes at log level 2 in `tools/zwift_shim/dsi_siusbxp.c`. Classify sensitive wire IDs independently of feature flags, omit their checksums as well as their bodies, and keep fragmented or coalesced shim writes from exposing key bytes. Check trace output with the security handlers both enabled and disabled.
- [ ] **Isolate release publication from builds and pin packaging inputs.** `.github/workflows/release.yml` grants `contents: write` to every matrix job. Its DFU step executes a downloaded `nrfutil` with no pinned digest and installs an unversioned `nrf5sdk-tools` before the package is checksummed. Make build jobs read-only and give write permission to a separate tag-only publisher consuming the expected artifacts after the gates pass. Pin release actions by commit and the DFU tools by version and verified digest, and use the same versions in `scripts/package_dfu.ps1`.
- [ ] **Escape binding labels in MQTT discovery JSON before exposing editable labels.** `compose_discovery()` in `apps/dongle_thread/src/mqtt_sink.c` interpolates the display name into a JSON string. A quote, backslash, or control character in a binding label can make the retained configuration invalid. The current production binding call in `self_channels.c` supplies a null label. Use a bounded JSON string encoder and check that a label round-trips through a JSON parser.

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
  than this project. Probably don't need to keep compatibility aliases for a release. Do this before the
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
  - [ ] EXTEND_FAILED (or the ext-skip END) can abort a successor staged
    behind a held grant (review 2026-11-19, source inspection, not observed).
    The old op ends early in a growing elastic grant with the release owed
    (`grant_release_after_core()`); the next op, armed from the dispatch,
    misses the held air and stages (GATE_PENDING); `arm_settle_staged()`
    clears `arm_marked` before `d2_settle_now()` pays the release. A TIMER0
    compare in that gap (or one pended under settle's ZLI mask) asks EXTEND;
    on refusal `radiant_radio_abort()` (radiant_radio_nrf_gate_mpsl.c ~1736)
    passes the `arm_marked` guard (radiant_radio_nrf.c ~4445) and aborts the
    staged successor: ABORTED for air it never had, its request forgotten to
    a booking, one slot lost; nothing wedges. Window ~1-3 us with D2 (default
    y). No test covers it: test_arm_handback.c sweeps PRE_ABORT_END only with
    the grant held and GRANTED. Red test: backend harness, grant held but arm
    answered PENDING, abort injected after `arm_settle_staged()`; assert no
    ABORTED and the successor still staged.
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

## 1. [DONE 2026-11-17: trainer-internal (the Kicker), not the dongle] The FE-C channel goes deaf when its slot crosses the power meter's
Done (ADR 0024): two slots that cannot both be heard now take turns losing (`turn_candidate()` in radiant_sched.c), and a slot the scheduler dropped is an EVENT_CHANNEL_COLLISION, not an RX_FAIL, and never counts toward search. So a crossing can no longer send FE-C to search on its own, whatever its cause. 0xF7 page 11 (scheduler contention) and `decode_pcap.py --phase A B` are in.

Open: what makes the 0.57-2.67 ms band unservable. `test_two_masters_drifting_through_each_other` (radiant/tests/api) does not reproduce it on the nRF-direct or TI preset, with commands, frame latency, jitter or scattered loss; FE-C never loses two in a row there, before or after the change. So it is backend timing the fake does not model, or the real masters. Next: the two-master bench below, on the Pro Micro then the TI, reading page 11 and `--phase` across >= 4 crossings; then one Zwift ride (`decode_pcap.py --phase 3 2`, no 8-miss run, 0x33 completion at crossings equal to the rest of the ride). The booking path (MPSL) still books earliest-first.

2026-11-17, simulated masters (plan step 4, `archive/benchmarks/2026-11-17-fec-crossing/`, tool `tools/ant_crossing.py`): apps/trainer at 8192 (#52235, because the real Kicker was on the air) plus ant_sim power #20329 at 8182, received at once by the Pro Micro (diag, master 27a2a15), the TI (diag a63489c5) and the Garmin (FE-C only), 2 x 1200 s, 12 crossings. **The dongle serves the crossing:** FE-C lost 6-8 of 117 in-band slots per receiver, every one within +-0.2 ms of power's frame. That is on-air overlap: the FE-C-only controls lose the same slot. No run was longer than 2, and nothing was lost in the 0.57-2.67 ms band. ADR 0024 on hardware: `slots_collided` 1-3 per crossing and 0 elsewhere, `turn_yields`/`arm_rejected` 0. So the Zwift loss needs the real masters.
2026-11-17, real masters (plan step 3, `archive/benchmarks/2026-11-17-fec-crossing/real/`): Kicker #52233 + #20329 pedalled, Pro Micro DUT (diag acdd9d2, power + FE-C, commands), TI FE-C-only control, 2 crossings. **On the air:** in the band the TI loses the same slots (58 % of in-band slots missed by both, against 24 % elsewhere). The DUT's loss on frames the TI heard is 25 % in band and 23 % outside. Scheduler counters flat (`slots_collided` 1/0, `turn_yields`/`arm_rejected` 0). The Garmin was dropped (69 % background). Caveats: only 2 crossings, and 29-40 % background loss today. Crossing 2 still reached GO_TO_SEARCH from 8 air misses. The held-command path for that is item 3.
2026-11-17, Zwift acceptance (`captures/zwift-20261117-192749.pcap`, Pro Micro `pm_ship_acdd9d2`, 3 crossings): **PASS, closed as trainer behaviour.** No command lost to the reacquire: one GO_TO_SEARCH (138.066 s); the pending 0x33 failed there (Garmin order) and FE-C was back one period later. FE-C got no COLLISION events, and power yielded 2 slots to FE-C, so none of the band loss is the scheduler. Band loss was 59-76 % from -4 to +8 ms against 33.5 % background. Inside the band FE-C was heard on about every third slot (750 ms), which points to a cycle inside the Kicker. #20329 is on the same bike (owner confirmed; the Kicker reads high), consistent with a PowerMatch pairing. Not pursued further (owner, 2026-11-17). The Kicker re-pair ride stays optional: it would only prove the mechanism.

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

## 5. [DONE 2026-11-17, branch fix/startup-latency] STARTUP_MESG takes 53–59 ms after SYSTEM_RESET

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

# Open issues from the 2026-11-17 RadiANT security review (HEAD acdd9d2)

## 1. [DONE 2026-11-17, HIGH] Give serial pairing enough stack for X25519

**Done** (`68f3545` tests, `dd7ade4` fix, branch `wip/sec-x25519-stack`): the ladder works in two temporaries plus the dead `x2, z2, x3, z3`, in one static 896 B workspace (pairing builds only; not re-entrant, now lock-checked). Feather pairing recipe, NCS v3.4.0: curve frame 2,248 -> 72 B, bridge-to-curve chain 2,852 -> 668 B of 2,048 (exception frame included), deepest pairing chain now `PEER_NONCE`'s key install at 760 B; pairing text 4,044 -> 3,852 B, pairing bss +896 B. `RADIANT_SEC_PAIR_STACK_BYTES` (768) is checked by a bridge `BUILD_ASSERT`; `scripts/check_stack_budget.py` runs on the CI pairing build. Hardware: the bridge-stack ztest faulted before the fix (USAGE FAULT, stack overflow) and passes after on the nRF5340 DK (M33, high-water 728 of 2,048 B; `radiant.api.sec_host` 172/172); twister on Bluefin 6,425/6,425 in 16 scenarios; on the Pro Micro (Cortex-M4) `tools/ant_pair_bench.py` ran the full serial exchange (RFC 7748 public keys, fingerprint 362706 on both ends, no reset) with a bridge high-water of 680 of 2,048 B. Caveats: the chains in CI are named by hand (not a call-graph analysis; a per-function 512 B frame limit is the net); the bridge's deepest non-pairing path (channel open, ~1.2 KB by objdump call graph) is not gated. **Independent review** (`cff1de5`): fold, `fe_invert`/`fe_pack` contracts, wipe and lock coverage confirmed; added the RFC 7748 section 5.2 iterated vectors (1 round everywhere, 1,000 on native_sim) and made the frame limit see same-named frames separately. Revert check: old curve + new tests fails `sec_lock_hook` on native_sim (iterated vectors still pass, so the fold computes the same) and on the DK faults in `radiant_sec_x25519` under `radiant_sec_pair_set_scalar` (USAGE FAULT, stack overflow, one boot); fixed tree on the DK (NCS v3.2.4) 172/172, high-water 752 of 2,048 B; twister 6,433/6,433. CI step run locally over the pairing build: all six chains ok; objdump call graph agrees the chains are the deepest under `antr_sec_pair` (584 B).

The serial bridge calls pairing on its 2,048-byte thread stack (`apps/common/ant_serial_bridge.c`, `BRIDGE_STACK_SIZE`). ARM GCC at `-Os -mcpu=cortex-m4 -mthumb -fstack-usage` reports 2,248 bytes for `radiant_sec_x25519` alone and another 296 for its nested `fe_mul`. Size the pairing-enabled bridge stack or reduce the curve workspace. Check the complete bridge-to-curve call chain against the target stack budget and exercise ordinary serial pairing on that stack.

## 2. [DONE 2026-11-17, 90c7f49 on wip/sec-f1-policy] Stop old plaintext transmissions when a master enables confidentiality

In `radiant/src/antr/ant_sec_host.c`, key installation checks for an active master transfer, but the enabling `0xF1` configuration call does not. The radio path can also retain a scheduled TX body, a same-slot output, and an automatic ACK payload built under the previous policy. Make the policy transition take effect before reporting success. Cover queued ACK and burst transfers, a scheduled broadcast, and cached replies when enabling X_CONF or expanding the secured range. After success, affected pages must satisfy the new policy and the nonce high-water mark must survive.

Fixed, unconditionally (owner, 2026-11-17: no Kconfig or wire option). `radiant_sec_configure()` asks a weak port hook, `radiant_sec_policy_changing()`, before it writes a changed tuple, and marks the slot last built stale: its re-post is withheld, never resent or rebuilt, so no slot index is used twice. The antr `0xF1` path (`antr_sec_policy_set()`) runs configure inside the event crit section. Its hook refuses a master whose transfer engine is busy (`CHANNEL_IN_WRONG_STATE`, nothing changed: acked, burst, or answering a slave's burst). Otherwise it cancels the master's scheduled slot and drops the ack payload in that same section, then pumps. An identical `0xF1` and a slave's are untouched. 4 `radiant_sec` tests and 10 sec_host api tests; the policy-change unit test and the 7 refusal and take-back api tests fail on c273bef. Twister native_sim 6500 cases in 16 scenarios pass (security on and off, check_test_counts OK); nRF5340 DK radiant/tests 1152/1152 and api sec_host 182/182, one boot each. Security text 7452 → 7664 B (ceiling 7680, 16 B left), bss 6974 → 6975. Deviations from the plan: a hook instead of `radiant_sec_config_is()`, both antr functions `cold`, and the engine's `reply_ctrl` left alone, all for the size gate. The plan's version measured 7764. An idle engine's saved reply is reachable only through `radiant_transfer_reply_retransmit()`, which antr never calls, and every other use is inside a refused burst. Caveats: the boundary is a frame already on the air. nRF cuts it short; on TI it is unmeasured (`RF_flushCmd` is asynchronous and the frame may complete). A change costs one silent slot, or two below a 50 ms period. Independent review 2026-11-17: weak/strong hook linkage, crit atomicity, the stale flag, both deviations and the revert check confirmed (the 2 unit and 7 api tests fail with the fix neutered); `ant_wire.yaml`'s `0xF1` description now states the refusal and take-back (665c077).

- [DONE 2026-11-19 with item 4, merge ded8169] An identical `0xF1` still resets the TX MAC window (`tx_win_open`, `tx_prev_valid`), so the window in progress goes out without a tag and the receiver reports it unverified. Item 4's idempotent configure should cover TX as well as `rx_floor`.
- [OPEN, LOW] The TI boundary is unmeasured: sniff whether a frame in flight when `0xF1` lands completes on the CC2652 (L15 promiscuous rig).

## 3. [DONE 2026-11-17, 3d554a6 on wip/sec-epoch-anchor] Preserve epoch phase when a receiver joins after boot

`radiant_sec_set_epoch()` in `radiant/src/radiant_sec.c` clamped the epoch origin to zero when the supplied epoch age exceeded local uptime, and so did the re-anchor and the master's slot latch. The receiver then resolved counters against the wrong point in the epoch, invalidating the host's freshness boundary (§7.6). Fixed: `epoch_anchor` is signed, all three clamps are gone, and `0xF3` refuses a phase of more than `INT32_MAX` periods or whose whole wraps pass the epoch headroom. A newly booted receiver verifies current windows and rejects windows outside the fixed 128-period slack around the supplied phase (9 new `radiant_sec` tests, 1 adapter test, 1 Python test, all failing on 876711c). Security text 7340 → 7452 B (ceiling 7680), bss 6974 unchanged. Caveat: an absurd phase that used to be accepted silently is now refused.

- [OPEN, LOW] `rx_process()` returns silently when `advance_epoch()` fails (headroom or ceiling): nothing is counted.
- [DONE 2026-11-19 with item 6, 5bd9674, merge c443b07] `tools/radiant_crypto.py` `resolve_counter()` breaks the exact 128-period tie toward the lower rollover; the C keeps `base + low`.
- [OPEN, LOW] `protocol/ant_wire.yaml` says `0xF3` can be read, but `ant_sec_host.c` has no read handler.

## 4. [DONE 2026-11-19, merge ded8169 (de4b895, fcdd2a4, 7408f28, 901fe3a)] Preserve the verified-window floor across configuration retries

`radiant_sec_configure()` unconditionally clears `rx_floor`, even for an identical `0xF1`. A recently verified window inside the time slack can be delivered twice under strict delivery. Make identical configuration idempotent and retain the authenticated counter floor across policy changes within the same identity and epoch. Check at-most-once delivery after a retry and align the configure-reset wording in §7.6 with the fix.

Fixed: an identical `(switches, w, page_lo, page_hi)` returns OK and touches nothing (RX windows, `rx_floor`, TX window, stale flag, and the `radiant_sec_policy_changing()` hook does not fire, so no master flush and no `0xF8` seq spent). A real change still discards in-flight RX windows and restarts the TX window but keeps `rx_floor`; `set_epoch`/`set_key`/`try_devnum` still clear it. Side effect: the first window verified after a real `0xF1` is slew-limited instead of taken whole (§3.1 says so). Docs: §3.1, §3.2, §7.6, `radiant_sec.h`, `ant_wire.yaml` (`0xF1` idempotence, `0xF8` seq rule). 5 `radiant_sec` tests + 1 sec_host api test; with radiant_sec.c from 591174b, 4 unit tests and the api retry test fail (DK, one boot, reviewer-confirmed). The reviewer's LOW (the W-change test does not exercise the floor, since old-W replays cannot verify under a new W) is closed by the page-range-change-with-W-kept test (901fe3a), which delivers window 8 twice on the old code. Security text +8 B.

## 5. [DONE 2026-11-19, merge 6277248 (42a7b27, 09c5975, 6fcb584)] Clear compat verdicts when the security identity changes

`radiant_sec_compat_set_key()` disarms the epoch but leaves the previous identity's verdict and per-tier freshness state. A pinned stream can still report VERIFIED between rekey and rearm. Clear identity-bound verdict and freshness state on replacement. Check both Tier I and Tier II-only receivers immediately after rekey and after rearm. VERIFIED must require fresh evidence under the new identity.

Fixed: `rx_forget()` zeroes the whole receive block of `compat_ctx` (open window, absorbed/overflow, Tier I and Tier II last/any/ok/instant, `last_verdict` → CLEAR) on a real rekey (root or base devnum changed); the same-key no-op keeps it. `set_epoch`/`adopt_epoch` use the same helper. BUILD_ASSERTs pin the cleared range (follows the last TX field, `stats` last). TX state left alone: nothing transmits until `set_epoch`, which clears it. Header and `radiant-security.md` ("Compat follows both rules") updated. 2 `sec_compat_stream` tests (Tier I-only, Tier II-only; Tier II case also covers a devnum-only rekey); both fail at the right-after-rekey assert with the fix reverted (DK, one boot, reviewer-confirmed). Compat object text 4770 → 4768 B (ceiling 5120), linked delta 192 unchanged; not in the security text gate. Accepted (info): an on-air-only `set_devnum` keeps the verdict, as designed.

## 6. [DONE 2026-11-19, merge c443b07 (10ddbe7, 8c329fd, 5bd9674, 036f062)] Advance the host Tier I replay floor only after MAC verification

`CompatVerifier._feed_tier1()` in `tools/ant_verify.py` writes `last_counter` before it checks the tag. A rejected page can therefore cause subsequent genuine attestations to be counted as replays. Follow the firmware's authenticated-counter rule and keep any arrival diagnostics separate. Check that a bad tag leaves the authenticated floor intact and that the next genuine attestation verifies.

Fixed: `t1_floor` (the last VERIFIED counter, as `rx_t1_last`) moves only on a tag match; the replay check runs against it before the tag. `last_counter` is gone; the lost count is taken between VERIFIED pages only (review follow-up 036f062: a forged page had inflated it, e.g. forged 60000 gave lost=59994). `_feed_announce_b()` now checks at `t1_floor`, matching the firmware's anchor that moves only on a verified page. 9 tests; 6 fail on the old code, 3 more on 5bd9674. Tools suite 1227 OK.

- [OPEN, LOW, host tool] The host checks an announcement at the last verified Tier I counter, but the firmware tags at the clock's interval index; an announcement whose Tier I page was lost (or sent before it) fails on the host. Was true before item 6 too.
- [OPEN, LOW, host tool] `ant_verify.py` does not handle a 16-bit Tier I counter wrap (no modular compare, no next-epoch K_auth); after 65536×T every page reads as a replay. Was true before item 6 too.

## 7. [DONE 2026-11-19, merge b79b524 (dc314af, 2636422, a9bd295)] Make factory provisioning safe to retry with the installed key

`node_ident_provision()` in `radiant/src/node/node_ident.c` zeros the boot counter, pairing counter, and epoch reservation even when the supplied K_dev is unchanged. This can repeat deterministic pairing scalars and roll back freshness counters. Preserve all three records on a same-key retry and fail closed on a key-read error. Cover retries after the counters have advanced as well as interrupted first provisioning. Keep factory reset with a genuinely new key working.

Fixed: provisioning reads the stored K_dev first (`kdev_load_eq()`, constant-time, wiped). Any load result other than OK/absent returns `NODE_IDENT_EIO` with nothing written. Same key: the boot, pair and epoch records are kept, an absent one is created at 0 (only safe for a never-written record; same exposure as `node_ident_boot()` reading absent as 0). Absent or different key: the old path. 4 `node_ident` tests; 3 fail on the old code (DK, one boot, reviewer-confirmed; the new-key test is a guard). +96 B text, in no release gate. Documented limits (a9bd295): an interrupted new-key provisioning retried with that key keeps the old counters, so it cannot clear an exhausted boot counter (use a different key); a corrupt K_dev record now fails closed instead of being overwritten.

- [OPEN, LOW] Recovering a node whose K_dev record is corrupt now needs a partition erase or a separate delete API; provisioning refuses (by design of item 7).

**Merged-tree verification 2026-11-19 (47cab88, all unpushed):** twister on Bluefin 7085/7085 in 22 scenarios, check_test_counts OK (floors raised in 2367155: global 6285, unit scenarios +4 for item 7); nRF5340 DK `radiant\tests` 1186 pass + 1 skip and api sec_host 184/184, one boot each; tools 1227 OK; `gen_ant_wire --check` in sync (LF export). Security text 7676 / 7680 (**4 B left**, the next security change must raise the ceiling; owner OK'd raising 2026-11-17), bss 6975 / 7168, compat object 4768 / 5120, compat linked delta 192 / 288. `radiant/tests/boards/nrf5340dk_nrf5340_cpuapp.conf` (47cab88) sets `CONFIG_ZTEST_ASSERT_VERBOSE=0` for the DK only: master overflowed DK flash by 5648 B at the default.

## 8. [OPEN, MEDIUM, hostless pairing API] Honor the RNG selection in ADR 0009

`node_ident_pair_begin()` always calls `node_ident_pair_scalar()`, including when `caps.has_rng` is set. No production caller of this helper was found at the reviewed commit. Before wiring it into a node, use the advertised RNG for the ephemeral scalar and reserve deterministic derivation for builds without one. Exercise the entry point with and without RNG support and on a failed draw. The pairing counter must advance before public-key use, and a failed RNG draw must close the window.

# Low-power operation for battery-powered nodes
Apply only where it doesn't otherwise harm performance (or configurable, ie for a heart rate sensor use power saving, but a dongle doesn't need to save power as much). Note also that much power saving should be end application specific, we only want more generalizable items added here (or perhaps in one example like the HR example)

Suggested order: baseline and sample configuration first, then nRF on MPSL
builds, nRF standalone, CC26xx, and the API thread wake last. Leaving HFXO on
costs a few hundred µA against an idle floor of a few µA, so the nRF clock item
should outweigh the rest. The 50 ms API wake is probably low single µA and can't
be measured until the clock item lands.

**Status 2026-11-19 (power plan, ADR 0025; every current is an ESTIMATE, no
meter on the bench).** Done on local master: power-diag struct + readers
(baseline, `jlink_peek.ps1`, `tools/ant_power_est.py`), `hr_transmitter`
low-power overlays, nRF HFXO/timebase release `RADIANT_NRF_LOW_POWER` (default
n; nRF54L15 GRTC, nRF52840 RTC1, MPSL timeslot variant), TI C1 hooks and
`RADIANT_CC26XX_XOSC_HF_RELEASE` (default n), and the idle-HFXO regression
ztest (item below). Gates: G1 (L15 direct) ~811 -> ~36-41 µA, power and timing
pass, a TI-only receiver loss excess of ~0.3-0.4 points remains (H3: frame's
last bit after each HFXO restart; owner ACCEPTED it 2026-11-19, no FREQ_TRIM
sweep); G4 (MPSL, BLE connected 1 h) PASS on the steady-state reading the
owner chose (steady anchor -4 µs; boot/connect ±33 µs are one-off);
B5 (Pro Micro) PASS at equal TX power, owner 2026-11-19 (spike S3 lp rerun
PASS too, HFXO proven off); G2 (trainer FE-C on L15, option on) PASS on
master-side timing, owner 2026-11-19: ACK rate and bursts unchanged against
TI and Garmin slaves, HFXO released (~2.4 % duty); the literal 2186-2191 µs
band cannot be judged with either slave. All power gates are closed.
TI standby (CONFIG_PM): DEFERRED by owner 2026-11-19 - it needs a PM policy, RTC wake and a
UART rework, and C2's XOSC_HF release already takes the TI's easy step.
Open: nRF5340 net core, receivers at fractional rate, RX window counter, API
thread wake.

- [ ] **Slave burst opener uses the nominal period (low priority, not planned).**
  A RadiANT slave places its burst START at last t_sync + nominal period +
  2190 µs (`api_xfer_t_sync_for()`, `c->t_next = t_sync + period_us(c)` in
  `radiant_channel.c`). A master whose clock is off by a few ppm therefore
  sees the opener shifted: +7.6 µs against the L15 with
  `RADIANT_NRF_LOW_POWER` (LF crystal −30 ppm). It stays well inside the reply
  window and every burst completed (archive `2026-11-19-power-g2-garmin`).
  Fix if it ever matters: predict with the slave's drift-tracked period.

- [ ] **Baseline: document and measure low-power configurations.**
  Do this first, so the items below can be ranked and checked. Add a low-power
  overlay for `radiant/samples/hr_transmitter` (its `prj.conf` has
  `CONFIG_LOG=y`). The overlay turns off logging, serial, and the console, or
  suspends them with PM_DEVICE. An enabled nRF UART keeps the high-frequency
  clock requested and hides everything else. Note how to enable DCDC in
  devicetree. Show how one-way sensors can use the existing `MASTER_TX_ONLY`
  type to skip the post-TX receive window. Document bounded search for
  battery-powered receivers and the acquisition-latency tradeoff. Use a
  matched 1 Hz test link to measure between-slot current and charge per
  broadcast. Also check packet delivery at the standard HR profile cadence.

- [ ] **Receivers tracking at a fraction of the master's rate.**
  The ANT+ HRM profile lets a receiver open at period 16140 or 32280 against
  an 8070 master. That is a low-power receiver configuration that needs no new
  code. Add a test that RadiANT tracks reliably at 2× and 4× the period, since
  window drift grows with the gap, then document it alongside bounded search.

- [ ] **Measure and tighten the RX window per tracked message.**
  Receive current (about 5–6 mA) times window length is what a receiver spends
  per message. Expose window length per tracked message as a counter. Check the
  lead and the widening after missed messages for anything wider than timing
  accuracy needs. Re-run tracked loss when changing either.

- [ ] **nRF: release the high-frequency clock and timer during long idle gaps.**
  `radiant/src/radiant_radio_nrf.c` calls `clock_control_on()` for HFXO at init
  with no guard, and leaves its 1 MHz TIMER running even when there is no radio
  work. Prototype waking from a low-frequency timer far enough ahead of the next
  slot to start HFXO and arm the hardware compare. Preserve monotonic radio
  time, slot phase, and turnaround timing.
  - *MPSL builds first.* The MPSL gate already requests
    `MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED` for each grant
    (`radiant_radio_nrf_gate_mpsl.c`), so the init-time request pins the
    crystal on and overrides MPSL's own clock management on hrm_ble, Matter,
    and Thread builds. MPSL already starts HFXO ahead of each grant, which
    makes this the easier first step.
  - *Time base.* Once HFXO is off, the TIMER falls back to HFINT, which is only
    accurate to about a percent. Keep radio time on RTC (nRF52) or GRTC
    (nRF54L, µs resolution and low power), and re-anchor the TIMER after each
    wake. Pausing the TIMER alone loses time.
  - *Low-frequency clock source.* The `_synth` builds derive their 32 kHz
    clock from HFXO and cannot release it. LFRC at 250–500 ppm drifts 60–125
    µs over a 250 ms gap, about the size of the ACK window. Require LFXO, or
    widen the wake margin and RX windows for LFRC.
  - *Register retention.* The nRF52-only register shadow in the same file
    lists retention across HFXO-off as unverified on nRF54L and the nRF5340
    network core. This item inherits that check.

- [ ] **CC26xx: let the RF core power down between distant slots.**
  `radiant/src/radiant_radio_cc26xx.c` sets `nInactivityTimeout = UINT32_MAX`.
  Budget RF startup in the scheduler and let the driver power down across long
  gaps while keeping the core available for closely spaced operations. Wake
  also restarts XOSC_HF (`radiant_ti_power.c`). Check the full startup cost
  against the 250 µs in-grant arm budget, and how far ahead the scheduler posts
  commands. The driver can only power up early for commands posted with enough
  lead. On coex154 builds 802.15.4 shares the RF core, so this only helps the
  standalone TI image. Verify the first TX and tracked RX after wake, plus
  acknowledged transfers.

- [ ] **Wake the API thread for work or a deadline.**
  `radiant/src/antr/ant_radio_radiant.c` wakes every
  `RADIANT_API_HOUSEKEEP_MS` (50 ms) even when idle. Block on events when there
  is no timed work, and arm a wakeup for the next retry, search timeout, or
  transfer watchdog when one is pending. Verify deadlines with no radio events.
  The tick does more than housekeeping:
  - `radiant_sched.c` has no timer, so the tick is the only retry path for arms
    refused with EBUSY or ESTATE. Give that retry an explicit deadline.
  - Each denied search chunk extends the search and ACQUIRE deadlines by
    exactly `RADIANT_API_HOUSEKEEP_MS` (`radiant_search_note_denied()`,
    `radiant_channel_search_note_denied()`). Credit measured elapsed time
    instead, or search timeouts will quietly come out wrong.

- [x] **Guard against power regressions.** Done 2026-11-19 (power/wrapup):
  `test_idle_without_near_op_requests_no_hfxo` + positive control in
  radiant.backend.lp / lp_rtc.
  After the baseline, add a counter or ztest check such as "HFXO not requested
  while idle with no slot within N ms". A bench-only number won't catch a later
  change that puts the crystal back on.

# Backend drift: TI vs nRF (audit 2026-11-18, HEAD 40d6961)
Read-only audit of the nRF backend (direct and MPSL gate) and the CC26xx backend against `radiant/include/radiant/radiant_radio_hal.h`. The HAL boundary itself is clean: radiant_sched/channel/ack/burst/search have no `#if` on a backend or SoC symbol, and core timing goes through caps. The drift is in how each backend keeps the contract (error codes, terminal events, timing anchors). The arbitration code (TI RF-scheduler hook vs MPSL timeslot gate), power code and register/RF-command programming are genuinely hardware-specific and should stay apart. Cause: nRF changes get red ztests (`radiant/tests/backend`, `radiant/tests/gate`), TI changes only get bench sessions.

Suggested order: items 1-4 (bugs), then 5 (settle the contract text), then 6 and 7 together.

## 1. [DONE 2026-11-20, d4d46c0] The TI backend ignores `rf_index`
Fixed: EINVAL above `RADIANT_RF_INDEX_MAX`; an arm on another channel chains a CMD_FS (`chain_fs()`) ahead of its RX/TX, credited only once the FS is seen to have run, and a refused retune arm forgets the tuned channel (the driver caches and replays the shared CMD_FS). Bench: rf 66 heard (0 before), rf 57 loss unchanged against same-window controls. Open: `FS_SETTLE_US` (250) is unmeasured.
`radiant_radio_cc26xx.c` never reads `req->rf_index`. The synth is fixed at 2400 + `ANT_RF_INDEX_DEFAULT` (`:212` `.centerFreq`, `:257` `.frequency`), and no caps field tells the core. A channel on any other frequency (private network, ANT-FS, adaptive frequency per ADR 0012) arms successfully and silently listens on 2457 MHz. nRF validates (`> 124` gives EINVAL, `radiant_radio_nrf.c:~1088`) and programs `2400 + rf_index`. Fix: retune on change (FS command), or refuse non-default indexes and advertise it in caps. Silent wrong-frequency is the one outcome to rule out. `radiant_rf_index_to_mhz()` in the HAL header is used by neither backend.

## 2. [DONE 2026-11-20, f06a07e] A BUILD_ASSERT is disarmed by a deprecated SoC symbol
Fixed: keys on `CONFIG_SOC_COMPATIBLE_NRF54LX`; nrf54l15dk + synth.conf now fails the build as intended. No other live uses.
`apps/common/ant_dongle_main.c:40-41` refuses a synthesised LF clock on nRF54L by testing `CONFIG_SOC_SERIES_NRF54LX`. On NCS v3.4.0 that symbol is only `select DEPRECATED` and is never set (`zephyr/soc/nordic/nrf54l/Kconfig:12`), so the assert never fires; the failure it guards is a build that boots and hears nothing. Same class as the nrf backend-selection trap. Fix: `CONFIG_SOC_COMPATIBLE_NRF54LX` (see `radiant/Kconfig:73-96`). Grep the tree for other deprecated SoC symbols while there.

## 3. [DONE 2026-11-20, 8459e4c + 21b727e] Abort delivers the terminal without a lock
Fixed: one terminal claim under `irq_lock` (`claim_locked()`), core callback after unlock, owed callbacks tracked per handle; a claim taken while an arm is still inside `post_op()` no longer cancels handle -1 (= `RF_CMDHANDLE_FLUSH_ALL`).
`radiant_radio_abort()` (`radiant_radio_cc26xx.c:~2001`) delivers the terminal from the caller's context, and `deliver_rx_terminal()`/`deliver_tx_terminal()` do an unlocked test-and-set of `st.delivered_terminal` (`:1015-1019`) and clear `st.kind`. TI's RF callback runs in the RF driver SWI (`radiant_radio_cc26xx_arb.c:39-45`). `arm_common()` also checks `st.kind != OP_NONE` with no lock (`:1642`). nRF claims the op slot and aborts under `irq_lock` (`radiant_radio_nrf.c:2759-2766`, `3231-3240`). Risk: a double terminal, or a core callback running concurrently with another, against HAL "callbacks never nest and never run concurrently" (`radiant_radio_hal.h:630`) and the thread re-entrancy clause (`:635-639`). Not yet confirmed whether the core's call contexts can actually preempt the RF SWI on TI; check that first.

## 4. [DONE 2026-11-20, 8459e4c] A preempted window reports DENIED even after it heard frames
Fixed: DENIED only if the op never had the air (cancelled in the pending queue, or aborted before t_open / the TX preamble with no frame drained), else ABORTED. Known limit: judged at callback time, not abort time.
`rf_callback` maps any `RF_EventCmdPreempted` to `RADIANT_RADIO_STATUS_DENIED` (`radiant_radio_cc26xx.c:1497`, `1503`). The comment assumes "a preempted window heard nothing", which is false for a mid-window preemption. The core credits DENIED as zero listening (`radiant_sched.c:~2535-2540`). The nRF MPSL gate ends a window that lost the air mid-way as ABORTED so partial listening is credited (`radiant_radio_nrf_gate_mpsl.c:~1631-1656`, HAL `:829-832`). Fix: DENIED only if the command never started (no frames, start not reached), ABORTED otherwise.

## 5. [OPEN, contract] Settle the HAL text where the backends picked different readings
Decide each, write it into `radiant_radio_hal.h`, then make nRF, TI and `radiant/tests/fake_radio.c` agree:
- [DONE 2026-11-20, 0e07355 + 85f40cb: nested delivery is the contract. The HAL names it as the one exception to "callbacks never nest" (ORDERING, `radiant_radio_abort()`): the ABORTED terminal runs inside the abort call, once, slot free on return, outer event and body still valid. The fake now delivers nested; nRF and TI already did, now pinned by `radiant/tests/backend/src/test_terminal_contract.c` and `test_terminal.c`. The scheduler retires the window before aborting, so the nested terminal is stale (`test_a_nested_abort_terminal_is_stale_and_ends_nothing`)] Abort from inside a callback. nRF (`radiant_radio_nrf.c:~4049-4068`) and TI (`:2016-2019`) deliver the terminal nested inside the calling callback; the fake frees the slot and defers the terminal until the callback returns (`fake_radio.c:2684-2693`). `radiant_burst.c:842-848` documents the fake's behaviour as the contract. The HAL contradicts itself: "never nest" (`:630`) vs "terminal before the slot looks free" (`:998-1002`). The nested path is unreachable in tests today.
- Error codes. `n_filters > max_filters`: nRF EINVAL, TI ENOTSUP, fake EINVAL (the core maps these differently). Abort before init: nRF/fake ESTATE, TI OK. CRC width 0 and `body_len` 0: nRF refuses, TI accepts. Double init / NULL rx/tx callbacks: nRF refuses, TI accepts. Window longer than `max_window_us`: MPSL gate EDENIED, fake ENOTSUP, TI unchecked. Non-coex TI returns EDENIED when `RF_postCmd` fails (`:1859-1862`, `1979-1981`), which the HAL reserves for arbitrated backends.
- [DONE 2026-11-20, TI side: op ids skip 0, `*op` cleared on a failed arm] Op id. nRF skips 0 on wrap and clears `*op` on a failed arm; TI uses a bare `st.next_op_id++` (`:1835`, `:1962`) and leaves `*op`.
- [DONE 2026-11-20, 0e07355 + 85f40cb: t_sync is meaningful only on OK; otherwise every backend reports the requested `t_sync_at`, not exact (nRF keeps it at arm acceptance; fake no longer 0; TI already did). `radiant_burst.c` stamps `reply_t_sync` from `in_ack_at` when the reply did not go out, as the in-burst branch already anchored] Terminal `t_sync` of a TX that did not go out: nRF `now()` (`:~4196`), TI the requested time (`:1041`), fake 0 (`fake_radio.c:807`). `radiant_burst.c:1021` stored it as `reply_t_sync` regardless of status and `radiant_ack.c:473` judges the retransmit by it. The per-backend difference was latent, with no live path: `hal_tx` passes only OK events on to the core, `api_feed_xfer_terminal()` makes up `now()` for failures whatever the backend reported, and `radiant_transfer_reply_retransmit()` is called only from tests.
- [DONE 2026-11-20, 3606e8e: op error statuses (0x08xx, >= 0x38xx) and an op that never ran end FAILED, except RXBUF/RXFULL which stay TIMEOUT; status_err counts 0x08xx; bench: status_err 0 in routine tracking and acked runs] TI never emits FAILED: a command ending with error status `>= 0x3800` is only logged (`:1470-1479`); RX then ends TIMEOUT and TX ends OK if LastCmdDone fired. 0x08xx generic errors (e.g. ERROR_PAST_START) are not even counted.
- [DONE 2026-11-20 for TX, 8d5bc95: TX pastTrig = 0 (ERROR_PAST_START → FAILED, not exact) and ETIME when the TX start is inside the arm floor; RX keeps pastTrig = 1, documented] Late start. TI sets `pastTrig = 1` (`:1819-1828`, `:1949`), so a start time already passed launches immediately, and the TX event still reports the requested `t_sync` with `exact = true`. HAL `:343-346` says a backend never transmits late; nRF returns ETIME or goes out on time. Only `ARM_FLOOR_US` (150 us) guards it at arm time.
- Receive window edges. TI drops frames whose `t_sync` falls outside `[t_open, t_close]` in software (`:1281-1286`); nRF has no equivalent check (hardware close only, `radiant_radio_nrf.c:~3072-3075`).
- RX terminal contents. nRF sets `t_sync = now`, `exact = false` and fills `noise_dbm` on an empty TIMEOUT (`:~4190-4228`); TI leaves `t_sync` 0 and never reports noise.

## 6. [OPEN, refactor] Shared HAL contract layer (~200-300 lines)
Small pure helpers both backends call; register access stays in the backends. Do after item 5 so the helpers encode the settled contract:
- Request validation with one set of error codes (`radiant_hal_validate_rx/tx()`), and `op_id_next()`. TI `arm_common`/`rx_arm`/`radiant_radio_tx`/`fmt_supported` (`:976`, `:1637-1760`, `:1882-1950`) vs nRF `rx_arm`/`radiant_radio_tx`/`format_prepare` (`:3221`, `:2738`, `:~1087`). nRF `rx_arm` also skips the `radiant_rx_filters_separable()` check that TI makes (`:1726`).
- Terminal and RX event fill (window gate on `t_sync`, `wild_value` policy, CRC-fail gating, terminal `t_sync`/`exact`/noise). TI `rx_drain_entries` (`:1200-1409`) vs nRF `radio_isr` END branch (`:~4500-4690`). `wild_value` also differs: TI sets it only under `CONFIG_RADIANT_SEARCH_SHORT_MATCH`, nRF whenever `filter.wild_last` is set (`:4636`).
- Window priority rule `(t_close - t_open) >= 10000 ? NORMAL : HIGH`, written three times: nRF `rx_gate_prio` (`:3132`), inline in nRF `rx_arm` (`:~3278`), TI (`:1849-1853`).
- TX start formula `t_sync - (PREAMBLE + addr_len) * 8 - RAMP_UP` with a named calibration term. Same shape on both (TI `:1955-1959`, nRF `:2905-2910`), but the calibration sign is opposite: nRF subtracts `T_SYNC_CAL_TX_US`, TI adds `CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US`.
- Early-arm diagnostic and terminal classification: `nrf_diag_early` (`radiant_radio_nrf.c:128-155`) and `cc_diag_early` (`radiant_radio_cc26xx.c:422-445`) are near copies; `term_ok`/`term_empty`/`term_error` too (nRF `:4187-4194`, TI `:1008-1030`). Move to `radiant_diag.c`.
- Smaller: TI's CRC16 (`:663-720`) could use `radiant_crc16` for the CCITT case. TX power: nRF rounds to the nearest table entry, TI keeps the previous power with a warning for an off-table dBm (`:874-881`), so an in-range request can leave TI at a stale power; share a nearest-entry helper.
Leave apart: TI arb vs MPSL gate, `radiant_rx_torn` (nRF single-buffer only), power/idle code, RSSI conversion, filter/address hardware programming.

## 7. [OPEN, tests] A TI backend test harness, and a fake that matches the hardware
- [DONE 2026-11-20, 3e5e1f0: `radiant/tests/backend_cc26xx` (plain + coex scenarios, RF driver fake, 43/49 cases). The fake does not model dispatch blocking behind a retired command or the driver's CMD_FS cache/replay] No test compiles `radiant_radio_cc26xx*.c`; `release.yml` only proves it builds. Add a `radiant/tests/backend`-style harness with an RF driver fake (`RF_postCmd`/`RF_scheduleCmd`/`RF_flushCmd`/`RF_cancelCmd`, data-entry queue, callback events). Items 3 and 4 should land as red tests first. Also worth a test: `rx_queue_init()` (`:1830`) memsets `rx_data`/`rx_output` and rewrites the static `rx_cmd` on the next arm while a cancelled window may still be finishing (cancel/flush are asynchronous, see `e82a0ea`); TI benches clean, so likely tiny, but the nRF equivalent was a recurring bug class.
- `fake_radio.c` gaps: no cc26xx preset (hamming 4, `rx_close_hold_us` 508, `addr_len_hw_max` 3, in-grant lead; tests rely on `caps_mut`). ETIME is stricter than either real backend (refuses inside the full `min_arm_lead_us`, `:1837-1845`), so the real `[floor, lead)` band is never exercised. The nRF preset has `filter_wildcard_dev = false` (`:423`) while real nRF defaults it on (ADR 0022). Terminal and frame delivery default to 0 delay while real backends land ~100-150 us later (knobs exist, default 0). Cannot model TI errors as TIMEOUT, TI preemption as DENIED mid-window, or late TX starts.
