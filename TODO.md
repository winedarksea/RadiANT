MPSL bursts: multi-packet bursts still can't run on MPSL builds. That's a scope gap, not a regression; Zwift uses acknowledged messages.

Discovery short match (ADR 0022), left open 2026-11-11:
- The two waste fixes (ADR 0022 "Not done here"): closed. The short match is on by default on TI (2026-11-12) and nRF (2026-11-13), and one set makes them moot; they matter only for `long_match.conf` images. No scheduler "window is live" query is built.
- Zwift acceptance of the nRF default flip (user-run, not a precondition any more): every sensor listed within ~1 s while HR and trainer are tracked, 0x61/0xC6 answered, reset -> NETWORK_KEY against 42-94 s (RadiANT history) and 9.2 s (Garmin). If it passes, mark ADR 0022 accepted; if it fails, rebuild with `apps/dongle/long_match.conf` and investigate.
- Decided 2026-11-14 (owner): short match stays default y on EVERY nRF image (MPSL, Matter, nRF5340 net core, L15), none of which had its own gate run; no CI job for `long_match.conf` (declined).

## TI CC2652R, open after the 2026-11-12 bench (evidence `archive/benchmarks/2026-11-12-ti-findings/` and `2026-11-12-ti-lead/`; items closed 2026-11-13, `archive/benchmarks/2026-11-13-ti-todo/README.md`):
- Rebuild the TI image before it ships: the one staged in `build\final_perf_6078d88\` predates e66ae02 (a one-packet transfer's ACK window now reserves no follow-on; host-tested only). Check acknowledged transfers on air with the rebuilt image (`tools/ant_ack_pair.py`, both roles) first.

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

# Open issues from the 2026-11-13 security review (HEAD 1df743a)

Found by reading the code and checked a second time; nothing was built or run. The primitives (AES, CMAC, CTR, KDF, X25519, ct_equal, wipe) read clean. `security.conf` and compat are in no release image, so protocol changes here break no deployed device. "Latent" means no app calls the path yet (`node_ident_arm_sec`, `profile_enrol_*`, compat `recover_*`/`adopt_epoch` are reached only from tests). The plan file holding D1-D17 (`~/.claude/plans/make-a-plan-to-async-gem.md`) does not exist; D14-D17 were checked against the spec and code comments.

Items 1-4 DONE 2026-11-13, each through an independent review and re-review; verified on the combined tree (DK main 1076, api sec_host 131 / sec 99 / plain 93, native_sim 5969/5969 in 16 scenarios, Python 1058). Size ceilings moved with sign-off: security text 6144 -> 6656 (6512 measured), pairing 3584 -> 4096 (3936); the security-off image is byte-identical in size to before.

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
- `cmd_tag()` uses the Tier I interval index as its counter, so a recorded command verifies again until the interval ends: 20 s by default, up to 3600 s. The 60 s rate limit does not cover a long T. Section 11.6 says a replay "into a later interval fails", which is true, but replay within the same interval is not stated.
- Fix: keep a last-accepted (interval, op) floor in profile_private.c, or state the limit.
- Done: profile_private keeps the last accepted tag per op and refuses a byte-identical command (`-EALREADY`, `refused_replay`); the idempotent retransmission ack still comes first. Cost, stated in section 11.6: the same op cannot be issued twice in one interval.

## 11. [DONE 2026-11-14: 24cd0e3] Non-broadcast traffic on a secured channel
- The D15 drop examines only the first packet of an inbound transfer. Continuation packets go to the engine in WAIT_NEXT (ant_radio_radiant.c:2629) and reach the host through `api_xfer_rx_data()`, with no check (`radiant_burst.c` has no security code). They carry no page number, so the secured-range test cannot apply to them anyway.
- Outbound acknowledged and burst data from a secured master (`antr_acknowledge_message_tx()`, `antr_burst_tx()`) goes on air in plaintext, untransformed.
- Fix: refuse outbound acked or burst data on a secured channel unless the host opts in, and document that inbound non-broadcast data on a secured channel is unauthenticated.
- Done: a keyed master refuses both calls with `CHANNEL_IN_WRONG_STATE` and posts nothing; slaves are untouched. No host opt-in (nothing uses one; add it if a user appears). Inbound is documented in section 3.2, "Non-broadcast traffic".

## 12. [DONE 2026-11-14: 3b8e309, dff88e6, 501370a] Hardening and documentation
- [x] Dead-store wipes: `memset()` on secret locals that are never read again can be elided at -Os. node_ident.c:560 (half the pairing scalar in `tag`), :677 (the whole scalar), and the K_dev copies at :138 and :167. Use `radiant_sec_wipe()`. Done 2026-11-14 (3b8e309), plus the read-back on the load-failure return.
- [x] radiant/src/radiant_sec_rng_psa.c:45 returns without zero-filling when `psa_crypto_init()` fails, while the entropy backend and the node_ident comments promise a zero-fill on failure. The one caller checks the return code, so this is a contract mismatch, not a live bug. Done 2026-11-14 (3b8e309).
- [x] Pairing expiry is only checked lazily, on the next call, so an abandoned scalar stays in RAM until a pairing call or a reset. Done 2026-11-13 (33328d8): `radiant_sec_pair_service()` frees expired contexts.
- Found while fixing items 1-4, open:
  - [x] The main ztest image no longer fit the nRF5340 DK (41,756 B over at 5bc61b7; verbose assert text is ~590 KB of it). Done 2026-11-13 (4bab0ca): assert messages shortened, 970,084 B (92.5 %), 1076/1076 pass on the DK. Tests grew ~40 KB in one afternoon, so expect it to fill again; the agreed fallback is a DK-only `boards/nrf5340dk_nrf5340_cpuapp.conf` with `CONFIG_ZTEST_ASSERT_VERBOSE=0` (498 KB). New tests: keep assert messages to runtime values.
  - [x] `scripts/run_ztest_hw.ps1` can capture two boots: the image runs between flash and go. A fixed copy, which waits 1 s and throws away that partial output before go, is in the 2026-11-13 session scratchpad and not committed. Done 2026-11-14 (dff88e6): one boot line on the DK.
  - [x] `test_matter.c` had a `-Wformat-extra-args` warning. Fixed in 4bab0ca.
- [x] Section 7.5 says ~2.3 h to one verified forgery at W=2 and 4 Hz. One attempt per 2-packet window is 2^16 x 0.5 s, about 9.1 h. Recheck the figure. Done 2026-11-14 (501370a): ~9.1 h at 4 Hz, ~4.5 h at 8 Hz.

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

Zwift pcap using an MPSL build dongle (image ready 2026-11-14, user-run):
- Image: L15 DK `apps/dongle_thread` + `gate.conf;uart_57600.conf` +
  `uart_57600.overlay` (Zwift's ANT_DLL opens the port at 57600); hex at
  `build\zwift_mpsl\zephyr.hex` (gitignored, copied there 2026-11-14). Flash with
  `scripts\flash_sim_jlink.ps1 -HexPath <hex> -Device nRF54L15_M33 -Serial 1057737173`.
- Host: `scripts\zwift_ant_shim.ps1 -Port COM4` (the L15's ANT VCOM), set
  `RADIANT_SHIM_LOG` to keep the shim's call log - it is the capture here,
  since USBPcap sees only the J-Link's CDC. Keep the L15 log VCOM (COM3, DTR)
  captured too. The 69:108 cause is fixed (63b2a02, "MPSL assert 69:108"
  above), so the image must be rebuilt from a tree that contains it. `gate.conf`
  images reboot rather than halt on any MPSL assert (`CONFIG_RESET_ON_FATAL_ERROR=y`
  from prj.conf), so a second boot banner on the log VCOM is the sign to look
  for.
- Confirm: FE-C acknowledged control completes (page 0x33 about every 1 s, 0x37
  every 30 s, TRANSFER_TX_COMPLETED rather than FAILED; the 2026-11-04 RadiANT USB
  capture had 23 % "master frame heard, no ACK"); every sensor listed within
  ~1 s while HR and trainer are tracked; time from reset to NETWORK_KEY against
  42-94 s (RadiANT history) and 9.2 s (Garmin).
Zwift pcap using a TI build
Zwift pcap using erg mode in Zwifts
Zwift confirm the nRF wildcard matching (faster scan) works
