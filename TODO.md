MPSL bursts: multi-packet bursts still can't run on MPSL builds. That's a scope gap, not a regression; Zwift uses acknowledged messages.

Discovery short match (ADR 0022), left open 2026-11-11:
- The two waste fixes (ADR 0022 "Not done here"): closed. The short match is on by default on TI (2026-11-12) and nRF (2026-11-13), and one set makes them moot; they matter only for `long_match.conf` images. No scheduler "window is live" query is built.
- [DONE 2026-11-20, ADR 0022 accepted from existing captures] Zwift acceptance of the nRF default flip. Pro Micro (3 captures) and TI: every untracked sensor listed in 0.09-0.54 s with 3 channels tracked, cold opens 0.17-0.66 s; 0x61/0xC6 answered; reset -> NETWORK_KEY 11-15 s. Table in the ADR's "Zwift acceptance" section.
- [OPEN, MPSL] With 3 pinned channels tracked, the MPSL gate's scan (nRF52840 Dongle, `zwift-20261115-214722` at 271.8 s) heard 3 frames in 3.66 s, then every sensor within 1 s once Zwift closed the pinned channels. One open, on the stick with the weak Kicker link. Scan airtime under tracking (ADR 0018/0021 arm lead), not the short match, which a long match would make worse. Measure with `ant_scan_rate.py --track` on the gate image beside the direct image at the same position, together with item 4 of "From the 2026-11-15 Zwift captures".
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

- [x] [DONE 2026-11-20, merge ac908e2: 9dd5316 fix + 0126452/72e3c74/254084d/8e972c6 check: `fe_cswap()` builds its mask behind an asm value barrier and is `noinline`;
- [x] [DONE 2026-11-20, d1d93b6 + 289f9a1 + 5ae405e: a pure decoder (`apps/dongle_thread/src/matter/bridge_storage_codec.{h,cpp}`) checks every length by subtraction against the bytes left and against `sizeof(dst) - 1`, and writes the NUL; V1 and both migrations bounded;
- [x] [DONE 2026-11-20, d0bfe65 red + 8889073 + 316801b + 6c32621 (symmetric split after review: a sender 65..127 ahead was locked out) + 3d9fafb: the tag and the ack tag bind a 16-bit extended sequence the node infers from the wire byte (ext = seq on the first lap, so old tags are unchanged);
- [x] [DONE 2026-11-20, merge ac82971: 0632f69/cad9880 red + 4252265 + c510c73 + f6dcf95 + 340a154: a persisted endpoint high-water mark (`br/brd_ep_hw`, u16, absent = max(restored)+1, so no version bump). `Init` seeds max(first, stored); the restore still raises it and writes back once if it did; each allocation writes the next id BEFORE `CreateEndpoint()` (a failed write refuses the bind), so a power loss can only burn an unexposed id; 0xFFFF wraps to the first dynamic id and is never stored. Vendored files patched: bridge_manager.cpp, bridge_storage_manager.{h,cpp} (src/matter/README.md). matter_storage 26 -> 37 cases (codec arithmetic plus a model of bind/unbind/reboot; the manager wiring is covered by review and the build). Matter images +488 B flash, RAM +24 (L15) / +0 (52840); nm/.config checks pass. **Owner step:** on an L15 Matter image bind 3, unbind the newest, reboot, bind one: it must get a new id; also one upgrade boot from a store without the key. Left alone (upstream): on a SENTINEL from `CreateEndpoint` the caller erases the device, so the skip loop never retries] **(Follow-up to the Matter item, pre-existing, LOW) Reserve a skipped record's endpoint id.** `RestoreBridgedDevices()` (`ant_bridge.cpp`) skips an undecodable record, and `mCurrentDynamicEndpointId` is rebuilt as max(restored)+1 (`bridge_manager.cpp`), not persisted. If the corrupted record held the highest endpoint id, the next new sensor reuses it as a different device, which Matter forbids. Upstream has the same behaviour on any decode failure. Keep the id when the fixed header decodes but the strings fail, or persist the endpoint high-water mark. **Re-assessed 2026-11-20 (reviewer P3):** the skipped record is the narrow case. A normal unbind (`RemoveEndpointForRow()`) of the highest endpoint followed by a reboot reuses its id the same way, with no corruption involved, and so do the restore loop's other skips (heap, type not served by this build, AddBridgedDevices failure). A failed settings load usually yields no header at all, so the header-only option covers almost nothing. Only a persisted high-water mark (a new key, absent = max(restored)+1, so no version bump) fixes all of them; it needs a Matter build and an on-device bind/unbind/reboot check.
- [x] [DONE 2026-11-20, merge af153ce: fbf6784/22b1e72 red + 4568597 + 77a96be: `backoff_clear()` runs only once a fresh sequence is accepted (after the duplicate, window and exhausted checks);
- [x] [DONE 2026-11-20, merge dd630da: eba9070 red + 0d7b0f9 + 98a2f95 + 07feb0c + 6d9eb5f + 3782e84: a weak `radiant_sec_key_replacing()` hook fires in `radiant_sec_set_key()` only on a real rekey (after the same-root no-op); 
- [x] [DONE 2026-11-20, merge fc9ed18: fd47ca6/fde6981 red + ca32545 + 84d04ea + 6a3d0bc + c93e1d1 (bridge), 4a61a78/5391281 red + 7adb779 + 3031cc5 (shim): `key_msg_keep()` (no `#ifdef`s) keeps the 0x46 net number, the 0x7E key number, the 0xF2 channel and key length, the 0x7F type byte of an RNG seed, and nothing of 0xF5;
Open: end-to-end `ant_zwift_visible.py --dll` on the bench; SI_Read dump stays unmasked (no device reply carries a key)] **Redact key material from diagnostic traces across configurations.** In `apps/common/ant_serial_bridge.c`, tracing precedes dispatch, the root-key and pairing masks depend on feature flags, and the legacy encryption-key message has no mask. A trace-enabled dongle can therefore log a key even when it rejects the command. The Windows shim also hex-dumps host writes at log level 2 in `tools/zwift_shim/dsi_siusbxp.c`. Classify sensitive wire IDs independently of feature flags, omit their checksums as well as their bodies, and keep fragmented or coalesced shim writes from exposing key bytes. Check trace output with the security handlers both enabled and disabled.
- [x] [DONE 2026-11-20, merge 9a72bfd: 6dd0be3 + 9ce3f37 + 9048cd1 + 35a4707: workflow default `contents: read`; build-core uploads each image with its `.sha256` (bare file name now, so `sha256sum -c` works beside a download) and, on tags, a `release-*` copy; a tag-only `publish` job (needs every build job + zero-cost, the only `contents: write`) re-checks the tag against VERSION, requires one artifact per `release: true` row, verifies every sum and attaches with `fail_on_unmatched_files`. All 47 `uses:` in release/ci/linkcheck pinned to commit SHAs (each equal to its moving tag today). DFU: nrfutil launcher 1.4.2, core 8.2.0 (bootstrapped from a pinned tarball into a fresh `NRFUTIL_HOME`) and nrf5sdk-tools 1.1.0 (`install --tarball`), all sha256-pinned, versions checked without a `head` pipe; `package_dfu.ps1` refuses any other core or nrf5sdk-tools version. linkcheck `issues: write` only in the job that files issues. **Owner steps:** one `workflow_dispatch` run of release.yml before the next tag (first real Linux run of the nrfutil bootstrap); only a tag exercises `publish`. Residual: a compromised build row can still upload an image with a matching sum (the sum check catches transport corruption only)] **Isolate release publication from builds and pin packaging inputs.** `.github/workflows/release.yml` grants `contents: write` to every matrix job. Its DFU step executes a downloaded `nrfutil` with no pinned digest and installs an unversioned `nrf5sdk-tools` before the package is checksummed. Make build jobs read-only and give write permission to a separate tag-only publisher consuming the expected artifacts after the gates pass. Pin release actions by commit and the DFU tools by version and verified digest, and use the same versions in `scripts/package_dfu.ps1`.
- [ ] **(Deferred 2026-11-20: not reachable today) Before any path sets a binding label, escape it in MQTT discovery JSON.** Checked against 906b949: the only production bind passes a NULL label (`self_channels.c`) and no host, ANT or Matter path writes one. Whoever adds editable labels must first add a bounded JSON string encoder in `compose_discovery()` and a round-trip test through a JSON parser. Original finding: **Escape binding labels in MQTT discovery JSON before exposing editable labels.** `compose_discovery()` in `apps/dongle_thread/src/mqtt_sink.c` interpolates the display name into a JSON string. A quote, backslash, or control character in a binding label can make the retained configuration invalid. The current production binding call in `self_channels.c` supplies a null label. Use a bounded JSON string encoder and check that a label round-trips through a JSON parser.

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
  - [x] The coex leg had no BLE load (`coex.conf` never calls `bt_enable`), so
    the fix beside live BLE traffic is untested. Done 2026-11-20 on c028e53
    (`acc_coex_ble`, `ble_load.conf`, SoftDevice advertising every 1 s): 1800 s,
    0 asserts, 0 reboots, no END found RADIO pending; DUT ch0 loss 1.65 %
    against the Garmin's 0.54 % in the same window, 1 468 timeslots blocked.
    Evidence README "Coex leg with live BLE load".
  - [ ] Under BLE load 92 of the DUT's 119 misses in `acc_coex_ble` had no
    RX_FAIL (ant_verify fails its loss accounting). Probably slots MPSL gave the
    SoftDevice; confirm, and decide whether a blocked tracking slot should
    surface as an event. CONFIRMED 2026-11-20 from the leg's 5 s gate
    counters: tracked requests acquired/pending/granted 7209/7121/7117, so
    92 never granted = 88 `book_denied` (refused at `gate_acquire()`) + 4
    pending but never started (mechanism not logged); 27 RX_FAIL + 92 = 119.
    Refusals come in 45 bursts 40.1 s apart, the beat of 1005 ms advertising
    against four ch0 periods. A denied slot is counted in `slots_denied`
    (`antr/ant_radio_radiant.c:3008`) and posts no event, so ant_verify's
    "unaccounted" always holds them. Granted windows missed 27 against the
    Garmin's 39. OWNER: surface a denied tracking slot as an event, or have
    ant_verify credit gate denials. Cheapest diag for the 4: put
    `slots_denied`/`slots_collided` on the `rxpath` log line (:3279).
  - [ ] Master-originated acked data on the gate image: 58/60 twice, against
    60/60 on 2026-11-14 (two missed acks per run). No same-sitting run of the
    image before the fix, so not attributed.
  - [ ] 2 h Thread leg: DUT ch0 loss 1.87 % against the Garmin's 1.43 % in the
    same window (DUT below the Garmin in the other three legs), and the child's
    age read 210 s of a 240 s timeout at the end of the leg.
  Open items found on the way, not fixed here:
  - [x] DPPI hand-back order: closed by a8ad20d, pinned by
    `test_arm_handback.c` `test_handback_disables_connections_before_abort`
    (mpsl/staged-abort). The 2026-11-14 entry: beside OpenThread
    SUBSCRIBE_DISABLE/TXEN/RSSISTART stay wired to radiant's DPPI channels
    between grants (the hand-back restores each register of
    `radio_ep_mine_mask` to its grant-entry value, which is ours wherever the
    other stack has not written it), and EXTEND_FAILED, the ext-skip END and a
    TIMER0 END that cuts a live operation handed back before the abort or the
    DENIED disabled the operation's connections. a8ad20d (2026-11-18) disables
    conn_start/start_tx/close/rssi inside `radiant_nrf_gate_on_grant_end()`,
    so they are off at the hand-back on every END path. The test snapshots
    them inside the hand-back, before any abort, for plain END and
    hand-back-then-abort on a live TX and RX; with those four lines reverted
    it fails. The stale "Known gap" comment at the hand-back is rewritten.
  - [ ] In-grant `gate_acquire()` returns GATE_GRANTED unmasked and the caller
    then programs the radio from SWI or a thread. A TIMER0 backstop END between
    the two leaves `program_ed()` enabling RXREADY in INTENSET (only with
    `CONFIG_RADIANT_ED_SCAN`, default n) or `program_rx()` writing
    SHORTS/connections on a radio no longer ours. Improbable (end margin);
    found by review, not observed.
  - [x] EXTEND_FAILED (or the ext-skip END) can abort a successor staged
    behind a held grant (review 2026-11-19). Fixed in ba16a6a
    (mpsl/staged-abort): `radiant_radio_abort()` from the signal also skips
    an operation staged behind its own still-placed request
    (`radiant_staged.pending && gate_request_pending()`), so a staged
    successor keeps its request and gets its own grant or denial. Chosen over
    holding `arm_marked` until `d2_settle_now()`: without D2 the owed release
    is paid by `grant_release_settle()` after the dispatch, so the window
    outlives the arm. Red test first (ef58f29):
    `test_arm_handback.c` `test_staged_successor_survives_extend_failed`
    sweeps every preemption point of the staged TX/RX/ED arm plus the point
    after it returns, in `radiant.backend` and the new `radiant.backend.d2`;
    the gate's half is `test_gate_d2.c`
    `test_d2_extend_failed_leaves_a_staged_request_pending`.
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

## 2. [DONE 2026-11-17, 90c7f49 on wip/sec-f1-policy] Stop old plaintext transmissions when a master enables confidentiality

## 3. [DONE 2026-11-17, 3d554a6 on wip/sec-epoch-anchor] Preserve epoch phase when a receiver joins after boot

- [OPEN, LOW] `rx_process()` returns silently when `advance_epoch()` fails (headroom or ceiling): nothing is counted.
- [DONE 2026-11-19 with item 6, 5bd9674, merge c443b07] `tools/radiant_crypto.py` `resolve_counter()` breaks the exact 128-period tie toward the lower rollover; the C keeps `base + low`.
- [OPEN, LOW, owner call] `protocol/ant_wire.yaml` says `0xF3` can be read, but `ant_sec_host.c` has no read handler. Checked 2026-11-20: nothing sends a MESG_REQUEST for it, but `docs/radiant-security.md` (~:1967) states 0xF3/0xF4 readability as intended and `tools/ant_sec.py` ships `decode_epoch()`. Either make it write-only (yaml h2d, security doc ~:1841/~:1968, regenerate) or add the read handler (costs security text, 7804/7856).

## 4. [DONE 2026-11-19, merge ded8169 (de4b895, fcdd2a4, 7408f28, 901fe3a)] Preserve the verified-window floor across configuration retries

## 5. [DONE 2026-11-19, merge 6277248 (42a7b27, 09c5975, 6fcb584)] Clear compat verdicts when the security identity changes

## 6. [DONE 2026-11-19, merge c443b07 (10ddbe7, 8c329fd, 5bd9674, 036f062)] Advance the host Tier I replay floor only after MAC verification

- [OPEN, LOW, host tool] The host checks an announcement at the last verified Tier I counter, but the firmware tags at the clock's interval index; an announcement whose Tier I page was lost (or sent before it) fails on the host. Was true before item 6 too.
- [DONE 2026-11-20, c57d494] `ant_verify.py` did not handle a 16-bit Tier I counter wrap (no modular compare, no next-epoch K_auth);

## 7. [DONE 2026-11-19, merge b79b524 (dc314af, 2636422, a9bd295)] Make factory provisioning safe to retry with the installed key


- [OPEN, LOW] Recovering a node whose K_dev record is corrupt now needs a partition erase or a separate delete API; provisioning refuses (by design of item 7).

## 8. [DONE 2026-11-20, 0cfd7fc + 76ad282, review 8e919eb, on master] Honor the RNG selection in ADR 0009

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

# Backend drift: TI vs nRF (audit 2026-11-18, HEAD 40d6961)
Read-only audit of the nRF backend (direct and MPSL gate) and the CC26xx backend against `radiant/include/radiant/radiant_radio_hal.h`. The HAL boundary itself is clean: radiant_sched/channel/ack/burst/search have no `#if` on a backend or SoC symbol, and core timing goes through caps. The drift is in how each backend keeps the contract (error codes, terminal events, timing anchors). The arbitration code (TI RF-scheduler hook vs MPSL timeslot gate), power code and register/RF-command programming are genuinely hardware-specific and should stay apart. Cause: nRF changes get red ztests (`radiant/tests/backend`, `radiant/tests/gate`), TI changes only get bench sessions.

Suggested order: items 1-4 (bugs), then 5 (settle the contract text), then 6 and 7 together.

## 1. [DONE 2026-11-20, d4d46c0] The TI backend ignores `rf_index`

## 2. [DONE 2026-11-20, f06a07e] A BUILD_ASSERT is disarmed by a deprecated SoC symbol

## 3. [DONE 2026-11-20, 8459e4c + 21b727e] Abort delivers the terminal without a lock

## 4. [DONE 2026-11-20, 8459e4c] A preempted window reports DENIED even after it heard frames

## 5. [DONE 2026-11-20, 59ae9db + a23674a, review 3e41790 + dcedfd7; nRF window-edge bench PASS 2026-11-20 (846def7)] Settle the HAL text where the backends picked different readings

## 6. [OPEN, refactor] Shared HAL contract layer (~200-300 lines)
Small pure helpers both backends call; register access stays in the backends. Do after item 5 so the helpers encode the settled contract:
- Request validation with one set of error codes (`radiant_hal_validate_rx/tx()`), and `op_id_next()`. TI `arm_common`/`rx_arm`/`radiant_radio_tx`/`fmt_supported` (`:976`, `:1637-1760`, `:1882-1950`) vs nRF `rx_arm`/`radiant_radio_tx`/`format_prepare` (`:3221`, `:2738`, `:~1087`). nRF `rx_arm` also skips the `radiant_rx_filters_separable()` check that TI makes (`:1726`). [CHECKED 2026-11-20, no change, owner call: not a behaviour difference today. nRF's caps leave `min_filter_hamming_bits` 0, so the helper returns true at once ("free where it does not apply", HAL) and the call would be dead code; no test can go red on it. The HAL error table has no row for a non-separable set (TI and the fake both answer ENOTSUP, under "a format ... this backend cannot express" at best). If wanted: a HAL table row (ENOTSUP), then the nRF call for symmetry, ahead of `gate_acquire()`.]
- Terminal and RX event fill (window gate on `t_sync`, `wild_value` policy, CRC-fail gating, terminal `t_sync`/`exact`/noise). TI `rx_drain_entries` (`:1200-1409`) vs nRF `radio_isr` END branch (`:~4500-4690`). `wild_value` also differs: TI sets it only under `CONFIG_RADIANT_SEARCH_SHORT_MATCH`, nRF whenever `filter.wild_last` is set (`:4636`).
- Window priority rule `(t_close - t_open) >= 10000 ? NORMAL : HIGH`, written three times: nRF `rx_gate_prio` (`:3132`), inline in nRF `rx_arm` (`:~3278`), TI (`:1849-1853`).
- TX start formula `t_sync - (PREAMBLE + addr_len) * 8 - RAMP_UP` with a named calibration term. Same shape on both (TI `:1955-1959`, nRF `:2905-2910`), but the calibration sign is opposite: nRF subtracts `T_SYNC_CAL_TX_US`, TI adds `CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US`.
- Early-arm diagnostic and terminal classification: `nrf_diag_early` (`radiant_radio_nrf.c:128-155`) and `cc_diag_early` (`radiant_radio_cc26xx.c:422-445`) are near copies; `term_ok`/`term_empty`/`term_error` too (nRF `:4187-4194`, TI `:1008-1030`). Move to `radiant_diag.c`.
- Smaller: TI's CRC16 (`:663-720`) could use `radiant_crc16` for the CCITT case. TX power: nRF rounds to the nearest table entry, TI keeps the previous power with a warning for an off-table dBm (`:874-881`), so an in-range request can leave TI at a stale power; share a nearest-entry helper. [DONE 2026-11-20, 848035b: TI `nearest_txp()` applies nRF's rule (nearest entry, tie to the higher power, clamp at both ends) instead of `RF_TxPowerTable_findValue()`, which actually rounded down in range (-6 went out at -10) and kept the previous power only below -20; HAL text names the tie and the clamp; `test_tx_power_rounds_to_nearest_entry` in both cc26xx scenarios. A shared helper is still item 6's refactor.]
Leave apart: TI arb vs MPSL gate, `radiant_rx_torn` (nRF single-buffer only), power/idle code, RSSI conversion, filter/address hardware programming.

## 7. [DONE 2026-11-20, f430309..3082ca9] A TI backend test harness, and a fake that matches the hardware
