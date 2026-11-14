MPSL bursts: multi-packet bursts still can't run on MPSL builds. That's a scope gap, not a regression; Zwift uses acknowledged messages.

Discovery short match (ADR 0022), left open 2026-11-11:
- The two waste fixes (ADR 0022 "Not done here"): closed. The short match is on by default on TI (2026-11-12) and nRF (2026-11-13), and one set makes them moot; they matter only for `long_match.conf` images. No scheduler "window is live" query is built.
- Zwift acceptance of the nRF default flip (user-run, not a precondition any more): every sensor listed within ~1 s while HR and trainer are tracked, 0x61/0xC6 answered, reset -> NETWORK_KEY against 42-94 s (RadiANT history) and 9.2 s (Garmin). If it passes, mark ADR 0022 accepted; if it fails, rebuild with `apps/dongle/long_match.conf` and investigate.

Performance review 2026-11-13 (`docs/performance-review-2026-11-13.md`), open after the drain timing (`archive/benchmarks/2026-11-13-drain-timing/`, 0xF7 page 8):
- Callback/pass split: TI's worst drain (~450 us, one frame) is ~346 us core rx callback, which includes a full scheduler pass (~214 us) arming from inside the drain. Deferring that re-arm is the largest lever.
- Split `pass_step()` timing (slot-table scan vs the backend arm call) before choosing an active-slot bitmap: `pass_steps_max` is 1 on both backends and ~215 us on both CPUs, which points at the arm, not the scan.
- TI CRC table: the bitwise CRC costs ~60 us per frame (not 15-20 us).
- CRC repair dB gain (finding 3) still unmeasured; TI repair now works on air (`archive/benchmarks/2026-11-13-ti-crc-repair/`).

## TI CC2652R, open after the 2026-11-12 bench (evidence `archive/benchmarks/2026-11-12-ti-findings/` and `2026-11-12-ti-lead/`; items 1-3 closed 2026-11-13, `archive/benchmarks/2026-11-13-ti-todo/README.md`):
- [x] TI cannot receive the Pro Micro clone: FIXED. The cause was TI's RX intermediate frequency (0.5 MHz put the signal across DC, so loss depended on the transmitter's exact carrier offset), not the clone or the RX bandwidth. `CONFIG_RADIANT_CC26XX_INT_FREQ` now defaults to 1.0 MHz: clone and L15 0.0-0.6 % on TI, flat across ±200 kHz, below the same-window Garmin. The clone is a valid TI peer again.
- [x] Decoy sync word `0x59C5`: replaced by `syncWord1 = 0` (second correlator off). 6 x 300 s alternating legs: CRC fails halved, tracked loss unchanged against the Garmin.
- [x] `tools/ab_gates.toml`'s acquisition gate for the TI short match: closed as superseded, not run. The gate is a ratio against sdk-ant, which cannot run on TI, and it is `required = false`; ADR 0022's TI section already compared first-sighting medians against the Garmin in the same window (0.30/0.39 s, gate 2 pass).
- [x] The burst ACK-window reserve (e042a10) on MPSL: measured 2026-11-14, and it was a live regression. The data packet's grant ends about where the ACK window closes, and the gate required the window's 1550 us follow-on to fit in it too, so every acknowledged message an MPSL build originated as a slave (Zwift's FE-C path) failed: L15 `gate.conf` against a Garmin 0/60 and 0/60, Garmin receiving 57 and 59. Fixed (8650411): an in-grant arm is served by its own air and its follow-on is clipped at the grant's end; 58/60 and 59/60, every received packet acknowledged. Retries still cannot run in-grant on MPSL (item 4 below). Master-originated acked data was not affected (60/60 on HEAD). `archive/benchmarks/2026-11-14-mpsl-ack-window/`. Under cc26xx coex the in-grant lead stays 600 us, so retries still cannot fit there either.

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
4. Multi-packet bursts on MPSL builds (the "MPSL bursts" line at the top of this file). Retries share the problem: a retry at +3110 us is outside the data packet's grant, and on MPSL it fails the transfer with NO_ACK as before. Inbound too, measured 2026-11-13: Garmin -> L15 `gate.conf`, 100/100 bursts failed with only packet 0 delivered (`archive/benchmarks/2026-11-13-inbound-burst-gaps/` section 1). Giving each ACK its own reserve does NOT fix it: the gate serves an in-grant arm only if that arm's own reserve also ends inside the current grant, and fixed-class grants never grow, so the whole chain must nest inside the first grant (tracked window + 4150 us). The fix, for both directions, is on-demand extension of fixed-class grants in `radiant_radio_nrf_gate_mpsl.c`, ending the burst when an extension is refused.
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
- Done: every epoch is reserved before use. N (next unused) is persisted as E+2 before E is used, the ceiling is E+1, and a wrap past it waits for the persist (`radiant_sec_epoch_wanted()` hook, compat twin). Every arm after the first in a boot takes a fresh reserved epoch. A host 0xF3 on a node-followed channel is refused unless the root is replaced. Persist retries back off up to 30 s.
- Contract limit, not enforced: two channels transmitting at once under the same root and devnum can still meet on an epoch (node_ident.h says they must differ). No app does this today.
- `advance_epoch()` (radiant/src/radiant_sec.c:333) moves a wrap to epoch+1, and a hostless node's epoch is its boot counter (radiant/src/node/node_ident.c:210). A node booted at epoch B that runs for 65 536 slots (~4.5 h at 4 Hz) transmits under B+1. Its next boot is issued B+1 again, with the counter back at 0: the same K_enc and the same nonces. That is the two-time pad D14 and D17 exist to stop.
- A host issuing ordinal epochs has the same trap: section 3.5 says "persist the last epoch issued", but it has to be the last epoch *used* (the 0xF4 epoch). With a wall clock, two boots in the same minute return the same epoch, and `node_ident_arm_sec()` always passes `us_into_epoch = 0` (:478). No app overrides `node_ident_wall_seconds()` today.
- Compat has the same shape through `apply_wraps()`.
- Fix: give each boot a block of epochs (for example `boot_counter << 12`, with a headroom check), or persist the epoch on every wrap. Fix the section 3.5 wording too. Missing test: wrap, reboot, assert the epoch is not reused.

## 2. [DONE 2026-11-13, merge 9bfb0c0: 6106ca6] One injected frame desynchronises a secured master for good
- Done: a secured master refuses secured-range broadcasts in `radiant_sec_rx_ingest()` (counted at 0xF4 [26..27]), and `rx_process()` refuses latched-TX contexts, so reflected windows no longer verify.
- A master runs `radiant_sec_rx_ingest()` on broadcasts heard in its reply window (radiant/src/antr/ant_radio_radiant.c:2486), and RX shares `epoch` and the keys with TX. In the last ~128 slots before a wrap, a forged broadcast with a chosen counter byte makes `rx_process()` call `advance_epoch()` (radiant_sec.c:1201). TX still counts wraps from `tx_wraps`, so it advances again at the real wrap: the master ends on epoch+2 and its receivers on epoch+1, until the host sends a new 0xF3. Section 7.6 accepts the early advance because a receiver heals within 32 s; a master does not.
- The same gap means nothing binds direction: a master's own windows reflected back to it verify.
- Fix: refuse secured-range pages on a master channel (legitimate secured slaves never send them; ant_radio_radiant.c:1374 keeps a slave's broadcast off a secured channel), or put a direction bit in the nonce. Missing test: a master receives a secured page.

## 3. [DONE 2026-11-13, merge 1874e29, then 927cb75..d4b7bdd] The per-packet verdict never reaches the host
- Done: opt-in 0xF8 window-verdict event (0xF1 bit 4, needs X_AUTH): `[ch, verdict, window_start u16, W, seq u16]`. seq is never reset and is spent on every discard. VERIFIED is given only when the closing window is after the judged one and under 256 counters on, and rx state restarts on open/close. The host rule (first covering verdict binds; a gap, overflow or deadline settles unverified; the first verdict after a flush is a baseline only) is in section 3.2 and `tools/ant_sec.py` `VerdictResolver`. Ring refusals are counted at 0xF4 [28..29]. Unsecured and non-opted-in streams are unchanged.
- `radiant_sec_deliver()` discards the verdict (ARG_UNUSED, ant_radio_radiant.c:904), and nothing outside the tests overrides `radiant_sec_window_verdict()`. Under the default deliver policy, a forged secured-range broadcast reaches the host as an ordinary BROADCAST_DATA. The host only has the aggregate 0xF4 counters.
- Section 3.2 says "every secured RX delivery carries verified / unverified / clear". As built, X_AUTH without DROP_UNVER detects forgery in aggregate but cannot reject a forged packet.
- Fix: carry the verdict to the host (an extended-data flag or a window-verdict event), or make DROP_UNVER the default whenever X_AUTH is on.

## 4. [DONE 2026-11-13 for core pairing and 0xF5, merge 1a0031f: 164ff11..80b303c; over-air enrolment is documented only, see item 6] A computing MITM can make the six-digit fingerprints match
- Done: LESC-style commit/reveal (0xF5 sub-commands 0x04-0x06): the responder commits to Nb before it sees Na, and the fingerprint covers both nonces. A reveal that fails its commitment answers response code 0xF5. A failure after the commitment wipes the exchange, and all-zero nonces are refused. The bound is 10^-6 per user-initiated attempt. Host obligations (reveal order, removing the key on mismatch via close, EVENT_CHANNEL_CLOSED, then unassign) are in section 7.4.
- `pair_derive()` (radiant/src/radiant_sec_pair.c:138) has no commitment step. A MITM sees both real public keys and can grind its own key pair offline until the two six-digit fingerprints match. That is about 10^6 X25519 operations, seconds to a minute on a laptop, which fits inside the 60 s window. This applies to 0xF5 and to over-air enrolment.
- The radiant_sec.h pairing block and section 7.4 say two different shared secrets "cannot be made to match". `test_a_man_in_the_middle_cannot_match_both_fingerprints` tests only a MITM that does not grind.
- Fix: a commitment, as in BLE numeric comparison: one side sends H(pub) before it sees the peer's key. Or correct the documented claim.

## 5. [DONE 2026-11-13: 4d5d156, 55bf4c3, 4c12ebf, db761cc] A keyed but unarmed channel fails open in both directions
- Done: a keyed channel with no epoch (pending) withholds secured-range TX (RADIANT_SEC_EUNARMED) and drops secured-range RX (counted in dropped_policy); `is_secured` means keyed, so a pending master is posted by the pump and sends no plaintext ack. 0xF4 [30] holds state flags (bit 0 keyed, bit 1 armed), length 31, version 1. 4c12ebf: a withheld master skips the slots it let pass (it used to lag one period per pump, even for out-of-range pages and after 0xF3). db761cc (review): the skip runs only when `sec_pay_valid` is false (last post withheld, or a keyed master's slot missed/denied), so a master transmitting normally pays one field compare, not a clock read. Security text 6896 B, ceiling 6656 -> 7168.
- When `ctx_armed()` is false, `radiant_sec_tx_transform()` returns OK without transforming and `rx_ingest()` returns PLAIN (radiant_sec.c:840, :970). `radiant_sec_set_key()` clears `epoch_set`, so re-keying or completing a pairing on a live channel sends secured-range pages in plaintext, and accepts them unauthenticated, until the next 0xF3. 0xF4 does not report the armed flag, so a host cannot see it.
- Fix: once a context exists with switches set, withhold TX and drop RX of secured-range pages until the epoch is set. Report "armed" in 0xF4 by appending a field. Per the beta rule, ANTR_SEC_STATUS_VERSION stays at 1.

## 6. [DONE 2026-11-13: f330469, 4544e5c, 9be01e0] Over-air enrolment replaces the group root instead of delivering it
- Done: the sensor wraps its existing root (compat first, else sec) under the pairwise DH key, CTR then trunc64 CMAC (domain 0x06), and sends it as 4 beacon frames for 3 passes after the window closes; its root is untouched. Receiver API `radiant_sec_pair_join()` + `radiant_sec_pair_accept_root()` (no app wiring yet). A rootless sensor installs the enrolment root and delivers that. The compat-only enrolment counter is `radiant_sec_compat_stats.enrolments` (not in 0xF4). Pairing text 3972 B / 4096.
- Consequence to confirm: whoever wins a window (a MITM included) now gets the group root, which also opens traffic recorded under it before the window, and enrolled receivers see nothing (section 11.7 says so). Replacing the root bounded that to future traffic at the cost of a lockout.
- `radiant_sec_pair_peer()` installs the DH-derived key with `radiant_sec_set_key()` on the sensor's own channel (radiant_sec_pair.c:405). Section 11.7 says enrolment is additive and gives the new receiver the existing root. No code does that: test_profile_enrol.c:1325 keys the peer with `compat_root` by hand.
- As written, anyone who answers an open window locks out every existing receiver, and item 5 applies to the live channel at the same moment.
- Fix: wrap the existing root under the DH key and send it, leaving the sensor's root untouched.

## 7. [DONE 2026-11-13: c4cfe94] Compat epoch recovery allows a rollback
- Done: `adopt_epoch()` refuses an epoch below the held one unless `RADIANT_SEC_COMPAT_ADOPT_REPROVISION`; the held epoch is a no-op that keeps the Tier I lock. Without the flag `recover_*` run only the forward phase from max(last_seen, held epoch) (65,537 probes, was 131,090). Scan bounds kept; comments now count epochs (each boot spends at least two).
- `epoch_search()` probes 16 epochs backward and scans 0..65 536 absolute. `radiant_sec_compat_adopt_epoch()` (radiant/src/radiant_sec_compat.c:1468) has no monotonic check and clears the Tier I lock. The 24-bit hint is fixed per epoch, and boot-counter epochs always fall inside the scan. So a recorded beacon, plus recorded Tier I pages from any old session, rolls a receiver back and verifies.
- Section 7.6 says the descriptor search runs forward only; the compat scan contradicts that for a receiver that already knows a newer epoch.
- Fix: `adopt_epoch()` refuses `epoch < base_epoch` unless the caller passes an explicit re-provision flag, and the backward and scan paths require that flag.

## 8. [DONE 2026-11-13: e111889, e7024ce] Tier II windows have no replay floor
- Done: the full 32-bit window index reaches the nonce ([10..11] carry the high half), and `rx_tier2()` refuses `window <= rx_t2_last` before the CMAC (`tier2_replays` stat); the floor rises only on VERIFIED and is cleared with the Tier I state. Compat object 4604 B with item 7, 4 B under the 4608 ceiling: the next compat change moves it. Left open: Tier II recovery works only from windows 0..65535; a sender that reconfigures mid-epoch can reuse a window index.
- `rx_tier2()` resolves the 8-bit window index to the nearest of ±128 windows and keeps no high-water mark (radiant_sec_compat.c:948). A recorded window verifies again, any number of times, for about 4 minutes at N=8 and 4 Hz. The 16-bit index also rolls over inside an epoch (~36 h at N=8), when the same window verifies again.
- Fix: a floor like Tier I's `rx_t1_last`, and either drive the epoch from the window index or carry more of it.

## 9. [DONE 2026-11-13: 4d5d156, 583a981] 0xF2 resets the epoch monotonicity gate
- Done: `radiant_sec_set_key()` compares a key check value (CMAC(root, 0^16), cold path, not stored); the same root and devnum is a no-op that keeps the epoch, gate and ceiling, a different root clears them (pending, fail closed) and purges the queue. This also closes a re-send-the-node-root bypass of item 1's host-0xF3 refusal. 583a981: `radiant_sec_compat_set_key()` follows the same rule through the shared `radiant_sec_key_same()`; compat object 4656 B, ceiling 4608 -> 5120.
- Not closed, by design: a host that swaps to another root (or devnum) and back resets the gate, since nothing remembers old roots across a re-key. A same-root 0xF2 still resets the on-air devnum to the base (`sec_key_set_locked()` calls `set_devnum`), which costs a Tier 2 receiver its trial devnum.
- `radiant_sec_set_key()` clears `epoch_set` (radiant_sec.c:441), so a host can re-send the same key and then an older epoch, and both are accepted. D3 is only as strong as the host.
- Fix: keep the epoch high-water mark across `set_key`, at least when the root is unchanged.

## 10. [LOW] A go-private/return command is replayable within its interval
- `cmd_tag()` uses the Tier I interval index as its counter, so a recorded command verifies again until the interval ends: 20 s by default, up to 3600 s. The 60 s rate limit does not cover a long T. Section 11.6 says a replay "into a later interval fails", which is true, but replay within the same interval is not stated.
- Fix: keep a last-accepted (interval, op) floor in profile_private.c, or state the limit.

## 11. [LOW] Non-broadcast traffic on a secured channel
- The D15 drop examines only the first packet of an inbound transfer. Continuation packets go to the engine in WAIT_NEXT (ant_radio_radiant.c:2629) and reach the host through `api_xfer_rx_data()`, with no check (`radiant_burst.c` has no security code). They carry no page number, so the secured-range test cannot apply to them anyway.
- Outbound acknowledged and burst data from a secured master (`antr_acknowledge_message_tx()`, `antr_burst_tx()`) goes on air in plaintext, untransformed.
- Fix: refuse outbound acked or burst data on a secured channel unless the host opts in, and document that inbound non-broadcast data on a secured channel is unauthenticated.

## 12. [LOW] Hardening and documentation
- Dead-store wipes: `memset()` on secret locals that are never read again can be elided at -Os. node_ident.c:560 (half the pairing scalar in `tag`), :677 (the whole scalar), and the K_dev copies at :138 and :167. Use `radiant_sec_wipe()`.
- radiant/src/radiant_sec_rng_psa.c:45 returns without zero-filling when `psa_crypto_init()` fails, while the entropy backend and the node_ident comments promise a zero-fill on failure. The one caller checks the return code, so this is a contract mismatch, not a live bug.
- [x] Pairing expiry is only checked lazily, on the next call, so an abandoned scalar stays in RAM until a pairing call or a reset. Done 2026-11-13 (33328d8): `radiant_sec_pair_service()` frees expired contexts.
- Found while fixing items 1-4, open:
  - [x] The main ztest image no longer fit the nRF5340 DK (41,756 B over at 5bc61b7; verbose assert text is ~590 KB of it). Done 2026-11-13 (4bab0ca): assert messages shortened, 970,084 B (92.5 %), 1076/1076 pass on the DK. Tests grew ~40 KB in one afternoon, so expect it to fill again; the agreed fallback is a DK-only `boards/nrf5340dk_nrf5340_cpuapp.conf` with `CONFIG_ZTEST_ASSERT_VERBOSE=0` (498 KB). New tests: keep assert messages to runtime values.
  - `scripts/run_ztest_hw.ps1` can capture two boots: the image runs between flash and go. A fixed copy, which waits 1 s and throws away that partial output before go, is in the 2026-11-13 session scratchpad and not committed.
  - [x] `test_matter.c` had a `-Wformat-extra-args` warning. Fixed in 4bab0ca.
- Section 7.5 says ~2.3 h to one verified forgery at W=2 and 4 Hz. One attempt per 2-packet window is 2^16 x 0.5 s, about 9.1 h. Recheck the figure.

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
- MPSL assert 69:108 - NOW REPRODUCIBLE IN MINUTES, OPEN, 2026-11-14
  (`archive/benchmarks/2026-11-14-mpsl-soak/`). L15 `thread.conf` beside an
  attached OpenThread leader, one tracked + one searching channel: it fired in
  every leg, 1.5-6.3 min after boot, on HEAD dd38b26 (3/3) and on the fix image
  (4/4) alike, at 6-1143 failed extensions (2026-11-09: once in ~111,000). With
  `long_match.conf` it still fired (14.6 and 7.6 min, against a 3.2 min mean with it on). Traces end on
  TIMER0 returning END after a run of 10 ms first-bite scan grants with
  extensions skipped; one ends EXTEND_FAILED -> END. What changed since 11-09:
  short match on by default (2026-11-13) and the Pro Micro as master. Each one
  halts the image for good (C5-3), so Thread/Matter MPSL images hang within
  minutes. Next: bisect with the short match and the ext skip, try a no-Thread
  `gate.conf` soak, and look up 69:108 with Nordic.
- [x] A14-1: an in-grant ETIME now releases the grant at once (546be4d);
  FAILED is still reported from thread context. Backend ztest
  (radiant/tests/backend, which does compile radiant_radio_nrf.c) red then
  green. Soak counter `etime_rel` 0 in every leg.
- [x] A pending `gate_failed` is cleared by `radiant_radio_abort()` and at every
  claim (546be4d). Backend ztests red then green. `abort_clr` 0 in every leg.
- [x] `program_tx()` now uses program_rx()'s order (park, clear event, enable,
  write, capture-then-check) (546be4d). Backend ztest red then green. On air:
  L15 direct and `gate.conf` masters against the Garmin, loss unchanged
  (0.82/-0.01 % direct, 0.82/0.20 % gate, HEAD/fix), 0xF7 page 7
  `tx_start_missed` 0. Direct builds +32 B flash.
  `archive/benchmarks/2026-11-14-mpsl-tx-compare/`.

## Device Testing Open Items

Code size gates: DK ztest image fixed 2026-11-13 (4bab0ca, see the security follow-ups above). Comments cost no flash; a readability trim of the five most-commented sources is e441d80.

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
  captured too: an `MPSL ASSERT` line there means the board halted (69:108
  above), not that Zwift lost the sensors.
- Confirm: FE-C acknowledged control completes (page 0x33 about every 1 s, 0x37
  every 30 s, TRANSFER_TX_COMPLETED rather than FAILED; the 2026-11-04 RadiANT USB
  capture had 23 % "master frame heard, no ACK"); every sensor listed within
  ~1 s while HR and trainer are tracked; time from reset to NETWORK_KEY against
  42-94 s (RadiANT history) and 9.2 s (Garmin).
Zwift pcap using a TI build
Zwift pcap using erg mode in Zwifts
Zwift confirm the nRF wildcard matching (faster scan) works
