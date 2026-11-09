TI coexistence builds: the reviewer thinks TI's scheduler may refuse the command re-armed right after an early-stopped receive window, because the cancelled window still holds its reserved time. That needs a CC2652 on the bench.
MPSL air use: after a received frame or an ACK reply, the grant is now held to its natural end (up to about 4.5 ms) instead of being handed back early. That costs the other stack some air but isn't a correctness problem.
MPSL bursts: multi-packet bursts still can't run on MPSL builds. That's a scope gap, not a regression; Zwift uses acknowledged messages.
Weak tests: the new tests that claim the reserve is cleared on deny, abort, cancel and reset would pass without those clears. The gate tests don't exercise the handoff code at all.
Tool footgun: ant_ack_gate.py exits 0 on a failed verdict unless --gate is passed

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
level, and consider `SLIP_GUARD_RUN=8`.
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

## 4. [HOST TOOL, open] ant_sens reports spurious loss on short event_counter rungs
- Found while settling the TX power boost (done 2026-11-07: the boost now
  raises only the unset default and LVL_4; LVL_3 is a true 0 dBm).
- ant_sens reports a spurious 17.5 % loss on short rungs whose basis is
  `event_counter` (0 RX_FAIL, every packet received), which produced a fake
  knee. Evidence: archive/benchmarks/2026-11-07-reset-and-power/README.md.

## 6. [HOST TOOL, residual] Re-run ant_features against the Garmin
- The 7-byte capabilities fix landed 2026-11-07 (`capabilities_usable()`,
  tested with the Garmin's real reply `080800ba3600df`). The tool itself has
  not been re-run on the Garmin; do it when the stick is plugged in.

# Open issues from the 2026-11-07 extensions and security review

Items 1-9 need no bench; items 10 and 11 do. `security.conf` and the compat
layer are in no release image, so every protocol change below breaks no
deployed host or node. Findings were verified by reading the code; nothing was
run.

## 1. [FIRMWARE, LOW] What the Tier I phase lock does not cover
The phase anchor itself landed 2026-11-07 (rx_tier1 learns the phase from the
first verified page; docs/radiant-security.md 7.6 and 11.4). Still open:
- A **Tier II-only** compat receiver has no phase source: an 8-bit window index
  cannot supply one, so its window resolution still trusts the receiver's own
  anchor.
- A sensor that reboots into epoch + 1 usually reads as floor replays
  (`tier1_replays`) until the host re-runs epoch recovery. That was already the
  behaviour before the fix, which does not change it.

## 2. [DONE 2026-11-07] Implement the descriptor authentication frame
**Done:** `profile_desc_encode_auth()` emits the frame last, under the pinned
construction (K_auth = KDF(K_root, "auth", epoch, base_devnum); CMAC over
nonce_block(epoch, devnum, schema_id, 0x03) || the bodies of every other frame;
radiant-telemetry.md section 6, radiant-security.md 5.3). The receiver half is
`profile_desc_auth_verify()`, `profile_desc_auth_find_epoch()` (forward-only)
and `profile_desc_rx_take_auth()`, which sets `authenticated`;
`profile_desc_may_decode_data()` now requires it for X_CONF/X_AUTH.
`profile_sched_init_auth()` and `profile_sched_reseal()` seal a node's set in
the existing 16-frame buffer. The plain encoder still refuses every transform
bit. The encoder refuses X_AUTH with sparse, reserved bits 5/4, 17 frames, and
a descriptor epoch that disagrees with the seal. tools/ant_pages.py mirrors it
(`TlmDescAuth`, `encode_tlm_descriptor(d, auth)`,
`decode_tlm_descriptor(frames, auth, ...)`, `TlmPageScheduler(auth=)` and
`reseal()`), `radiant_crypto.desc_auth_tag()` is the MAC, and ant_verify
reports `descriptor: authenticated / FAILED / no key`. 0x61 now requires
W >= 4 (device-profiles.md 5.1).
**Evidence:** two shared vectors (0x61-shaped, frame 1 with and without the
epoch) as the same hex in test_profiles.c and test_ant_pages.py, checked by
hand against the layout. Every flipped body byte of frames 0..5 fails, as do a
dropped, duplicated or swapped frame, a frame 1 spliced from the next epoch,
and a wrong devnum, base_devnum or key. radiant/tests on the nRF5340 DK
932/932 (profiles 30/30, 9 new). Python: test_ant_pages, test_ant_verify,
test_radiant_crypto, test_ant_sim, test_compat_capture all pass. Standard
devices: apps/hrm_ble and apps/treadmill (nrf54l15dk, CONFIG_RADIANT_SEC=y)
link none of the new symbols, bss/data +0, text +20 B. Cost on a sealed
node: radiant-security.md 5.6.
**Open, found doing it:** a sealed node's schedule frame is under the MAC, so
profile_sched_set_downlink() refuses a sealed set (-ENOTSUP) rather than
re-phasing a MAC'd body per transmission.

(Original notes follow.)
- Specified byte for byte in docs/radiant-telemetry.md section 6 ("The last
  frame") and docs/radiant-security.md 5.3. Not implemented:
  profile_desc_encode() refuses any transform bit
  (profile_telemetry.c:266, asserted by test_profiles.c:376).
- Consequence: no 0x60 node can announce X_AUTH or X_CONF, the 0x61 CGM
  profile cannot ship (docs/device-profiles.md 5.1 says so), and Layer C
  private mode is specified to land on a 0x60 stream nothing can encode.
- In the same change, give 0x61 a minimum W in docs/device-profiles.md 5.1.
  X_AUTH is mandatory there but W is not stated. W = 2 is a 16-bit tag: at
  0.5 Hz that is about 73 hours of continuous injection per expected forged
  reading. W = 8 costs 32 s of latency on a 5-minute reading.
- Decide item 4 first, because it changes what frame 1 carries.
- The descriptor encoder must refuse X_AUTH together with the sparse flag
  (decided with item 7, 2026-11-07: the tag lags one window, so a sparse burst
  never carries its own tag and never verifies; radiant-telemetry.md section 8
  and radiant-security.md 3.1 say so). Not enforceable before this item,
  because profile_desc_encode() refuses every transform bit today.
- Done when the encoder emits the frame, a receiver rejects a descriptor whose
  frame 1 was altered, and tools/ant_pages.py mirrors it with shared vectors.

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

## 8. [FIRMWARE, MEDIUM] The 0xF1-0xF5 host commands run without api_lock
- apps/common/ant_sec_host.c calls radiant_sec_configure, _set_key,
  _set_epoch, the status read and the pairing calls with no lock. Every other
  antr_* entry point takes api_lock (apps/common/ant_radio_radiant.c:452).
- The RX thread runs radiant_sec_pump() under api_lock, and the parser thread
  outranks it. The software AES keeps one shared key schedule and is safe only
  because api_lock serialises every call (radiant_sec_aes.c:127).
- So a host command that preempts the pump mid-CMAC can re-expand the schedule
  under another key, or clear window state under it. Result: spurious
  UNVERIFIED windows or a wrong keystream.
- The 2026-10-31 review recorded this as not fixed; it still is not.
- Task: take api_lock around those calls. A preemption ztest is hard; at least
  assert the lock is held inside the radiant_sec entry points in a debug build.

## 9. [ENVELOPE, decide before v1 nodes ship] No informational bits left in the descriptor
- Frame 0 byte [7] is full. radiant-telemetry.md section 6 says further
  informational flags come from frame 1 byte [3], but W, the repeat count and
  the clock-accuracy nibble now fill that byte.
- What remains: the version nibble, which makes every v1 receiver reject the
  node; field count 15, "reserved for an extended descriptor" and undefined;
  and schedule-frame bits 42..47, present only when a schedule frame is sent.
- Task: choose and write down the extension rule, for example a minor-version
  rule a v1 receiver accepts, or a definition of field count 15. Fix the stale
  sentence in section 6 in the same edit.

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

## 11. [BRANCH, bench] review/2026-10-31-timing has waited a week
- One commit, 6df53e7, "timing fixes (UNVERIFIED on air - do not merge without
  A/B)". It holds A14-1, A13-1, A9-3, A14-2, A8-4 and C5-1.
- Task: a crossover A/B (old and new firmware swapped across two identical
  receivers, using the multi-channel recipe); A13-1 and C5-1 also need an MPSL
  coexistence soak. Or delete the branch: it drifts further from master daily.

## 12. [FIRMWARE, LOW] Tier II loses windows at the HR seam with no packet loss
- Found while writing the seam test for the Tier I phase anchor (971e38c). It
  existed before that change, which did not cause it.
- When a Tier II tag falls due on message 119 or 120, the window closes
  untagged. The profile_compat seam never offers those two slots
  (profile_compat.c, near the "slots 119/120" comment). The receiver also
  loses the window after it.
- Measured through the real seam: 17 of 998 windows unverified (1.7 %) with
  zero packet loss. Only a comment at test_profile_compat.c:655 records it.
  That test asserts "more verified than not", not every window.
- Related margin from the same test: Tier I slip on the seam is 818 ms
  against the 985 ms JITTER allowance (about 0.7 of a period of headroom).
  A future seam claimant that delays Tier I trips that test first.
- Low because nothing in apps/ links the compat layer yet.
- Task: let a tag due on 119/120 move to the next offered slot (and teach the
  receiver to expect it there), or keep a window from closing over a slot the
  seam never offers. Then tighten test_profile_compat.c to assert every window.

## 13. [HOST TOOL, LOW] 0x60 exact loss over-counts on host-driven masters
- The slot-based 0x60 loss rule (tools/ant_verify.py, about lines 772-790,
  from e1e9660) counts a slot as lost whenever the counter jumps.
- A host-driven master that refills twice, advancing the counter for a slot
  that never goes out, reads as loss. Seen live in the 2026-11-07 final
  review: a two-channel ant_sim run reported 6 lost slots against 3 RX_FAILs
  and 3 by wall clock. It came with an accumulator-continuity violation from
  the same skipped page. A single-channel rerun matched exactly (2/2/2).
- Gaps of 256 slots or more also alias. That is documented, and bike power
  has the same limit.
- Task: cross-check the counter delta against wall-clock slots between the
  pair. Withhold the figure, or call it an upper bound, when the delta is
  larger than the elapsed slots.

## 14. [HOST TOOL, cosmetic] ant_verify's liveness line is wrong for FE-C
- tools/ant_verify.py:1947 prints "the event counter stood still" for any
  channel without an exact-loss figure.
- Since the FE-C routing fix (7ddbb45), FE-C channels deliberately collect no
  liveness evidence, so every FE-C channel prints this false statement.
- Task: say "no exact-loss source for this profile" when the device type has
  none, and keep "stood still" for a channel that really has a frozen counter.

## 15. [DOCS, LOW] Telemetry section 6 overstates when unsecured byte [1] advances
- docs/radiant-telemetry.md section 6 (from 5655901) says byte [1] advances
  on every slot, including MPSL-denied and missed ones.
- That holds on secured channels, where radiant_sec derives the counter from
  t_sync. On an unsecured profile_sched node it holds only if the caller calls
  profile_sched_next_at() for denied slots too. The apps call it once per
  EVENT_TX.
- No 0x60 app exists yet, so nothing is wrong on air.
- Task: either add the caveat to section 6, or have the radio glue call
  next_at() for denied and missed slots so the docs become true. Decide when
  the first 0x60 app lands.

- Look into reducing the code size for fewer bytes in the critical size

# Open issues from the 2026-11-08 release review (HEAD b89a112)

Items 1-4 were confirmed by reading the code at this HEAD. The reviewer also ran
compiled probes for 2 and 4. Nothing here has been run on hardware. The
review's other two findings are already fixed (9ab6bc9, 971e38c); what is left
of the second is item 1 of the section above.

## 1. [FIRMWARE] Discovery assumes network 0 and frequency 57
`api_search_cfg_build()` (apps/common/ant_radio_radiant.c:1182) gives the one
shared sweep network 0's address and frequency 57, whatever the searching
channels asked for. Tracking does honour both (`api_net_addr_for()`,
`api_rf_of()`), so the channel never gets that far.

**a. Network: small and clearly worth fixing.** With the ANT+ key installed
only on network 1, the sweep stays on network 0's all-zero address and finds
nothing, with no error.
- Task: sweep on network 0's address if it is valid, else the lowest valid
  network's. Re-init on the first valid key of any network, not only network 0
  (:3579), keeping the live-carrier deferral. This is complete for now because
  the key table only ever holds the ANT+ address.
- Do not break Zwift: it scans on keyless networks 1 and 2 and relies on the
  sweep still hearing ANT+ there. `test_an_assign_to_a_network_with_no_key_is_accepted`
  must stay green, and do not refuse the assign.
- Done when an api ztest shows the sweep moving to the ANT+ address after a key
  on network 1 only.

**b. Frequency: real, but lower value, and it needs a decision.** A slave on
frequency 66 can never acquire a master on 66, though this library can be that
master. One frequency per sweep is a deliberate design (radiant_search.h:360,
radiant_search.c:306: a frequency axis multiplies sweep time). Only the ANT+ key
is accepted, so private-network hosts are excluded regardless; this reaches only
hosts using the ANT+ key off 57.
- Cheapest useful step: `LOG_WRN` when a slave opens off 57, as the keyless
  assign already does, so the silence becomes readable.
- Full fix: let the sweep follow the searching channels' frequency when they all
  agree, which costs ANT+-only sessions nothing. Alternate passes only when they
  are mixed. Same live-carrier deferral as the key install.

## 2. [FIRMWARE] Capabilities advertise features that only store configuration
ADVANCED_OPTIONS_3 (ant_radio_radiant.c:4070) claims event filtering and
selective data update. The filter is stored and never consulted
(radiant_event.c:250), and SDU masks are only stored and read back. This is the
"advertised and unimplemented" trap in docs/gotchas.md.
- The stated reason not to filter (ant_radio.h:805, loss accounting needs
  RX_FAIL) only matters to a host that sets the filter. It defaults to 0.
- Same byte, same class, not in the review: search sharing round-trips but is
  never consulted outside radiant_channel.c, and the encrypted-channel bit is set
  although AES-CTR is refused (comment at :4075). Audit the whole byte.
- No known host is at risk either way: archive/host-api says Zwift uses neither
  the filter nor SDU.
- Task: implement the event filter at the delivery point to the host
  (`antr_on_message()` call, :559), after any internal accounting. Check the
  bit-to-event mapping against the ANT message protocol document first. Replace
  `test_event_filter_round_trips_and_filters_nothing`.
- For each remaining bit, implement it or clear it. Clearing changes the
  capabilities frame, so the nRF and cc26xx .antser transcripts need regenerating
  on hardware, and the frame stops matching the sdk_ant reference. Keep accepting
  the commands either way: an error to a host with no recovery path is a
  regression (see the comment at :3216).

## 3. [FIRMWARE, Matter] A reachability or label change can be lost until the next transition
The flush reads each source's dirty flags per row with the lock held, drops the
lock for the endpoint work, then clears every source's flags at the end
(apps/dongle_thread/src/matter/ant/ant_bridge.cpp:831). A change landing in
between is erased. `reachableDirty` is only set on a change (:1150), so later
fresh samples do not repair it: Matter can show a live sensor as unreachable, or
a dead one as reachable. A bind (:1184, :1198) races the same way. The window is
the whole pass, which includes `PersistRowLabel()`.
- Task: at the top of the pass, copy `reachable` and both flags for every source
  into a local array and clear them, in one lock hold. Rows read the copy. A
  change during the pass then stays dirty for the next pass.

## 4. [FIRMWARE, MQTT] A pending offline is lost when a source is reused during an outage
`owed[source]` holds one uuid (apps/dongle_thread/src/mqtt_sink.c:1201). Release
A, bind B, release B during a broker outage, and A's "offline" is overwritten. A's
retained status then reads "online" until A is bound again.
`offline_overwritten` only counts it. Derived and rotated fields have no
`expire_after`, so nothing else clears them in Home Assistant.
- Task: replace the per-source uuid with one de-duplicated list of owed uuids,
  2 x RADIANT_BINDING_MAX entries (16, 128 B). The drain clears only the uuid it
  sent. A bind cancels a matching uuid anywhere in the list, not only on its own
  source (:1207). Keep the counter for overflow.
- Done when a ztest or the reviewer's probe shows both A and B offline after
  reconnect.

Items 5-7 come from a second pass over 69ac822..b89a112. 5 and the numbers in 7
were reproduced with the real Python tools; the C side of 6 and 7 was read, not
run.

## 5. [HOST TOOL] ant_verify decodes sealed data pages it has not processed
tools/ant_verify.py:775 hands received bytes to the decoder once the descriptor
authenticates. `may_decode_data` only vouches for the schema, and its docstring
(ant_pages.py:3118) makes the per-packet gate the caller's job. This caller has
no gate, so the tool that produces our evidence reports forged data as clean.
Reproduced through `ChannelAnalyzer`:
- Valid X_AUTH descriptor, then an arbitrary unauthenticated page: decoded 4660,
  counted as decoded, 0 violations.
- X_CONF ciphertext of the value 100: decoded as 6579, 0 violations.
- Task: when the schema has X_CONF or X_AUTH, count the page and do not decode
  it, and report that count in the summary. Keep counting slots, since byte [1]
  is in the clear. Real decryption and window verdicts are a separate, larger
  job, only if someone needs them.
- Done when both cases above report 0 decoded, with a test for each.

## 6. [FIRMWARE, before any X_CONF node ships] X_CONF alone leaves byte [7] in plaintext
CTR covers five bytes whether or not X_AUTH has claimed byte [7]: TX
radiant/src/radiant_sec.c:903, RX :1241, and tools/radiant_crypto.py:307. Without
X_AUTH the field area is 48 bits, so a field at bit offset 40 is sent in the
clear under a schema announcing confidentiality. The sealed encoder accepts that
layout. Reproduced in Python: byte [7] 0xAB went out as 0xAB.
- The specs disagree: radiant-telemetry.md:655 says [2..7] without X_AUTH,
  radiant-security.md:132 says [2..6] always.
- X_CONF alone is not a corner case: it is the only secured mode a sparse node
  may use (radiant-telemetry.md:1035). A host-driven X_CONF-only channel leaks
  byte [7] the same way.
- Task: process six bytes when AUTH is off, in TX, RX and `conf_transform()` and
  its callers. Fix radiant-security.md 3.1 and the "reserved tag space" comment
  at radiant_crypto.py:300, and update the shared vectors.
- Prefer this to making the encoder refuse the layout, which would not help
  host-driven channels. The on-air change is free now: `security.conf` is in no
  release image.

## 7. [FIRMWARE, before nodes ship] First-boot provisioning can pick a number that is hard to receive
`self_provision()` (radiant/src/node/node_ident.c:248) stores the first key it
draws without looking at the device number that key derives.
`ant_identity.random_device_number()` already avoids run >= 8, and firmware only
warns. The number comes from K_dev, so a bad draw is permanent, and the old
defaults 4242/4243 were safe (run 4).
- Computed over 1..65535: 11.1 % of numbers have run >= 8 for one device type,
  11.8 % across the seven ANT+ types, 5.4 % have run >= 9. The reviewer's key
  5d00..00 derives #37976, run 8, as claimed.
- Size of the harm: run 9+ loses 1-5 % on an unguarded receiver. Run 8 sits below
  the guard's default threshold of 9 and loses at most 0.2 % on a strong signal
  (radiant/Kconfig:640), up to 1.25 % when weak (follow-up on item 1 of the
  Garmin section). It is worth fixing because it is cheap and can only be done
  before the first persistence.
- Task: in `self_provision()`, derive the candidate's number before
  `node_ident_provision()`. Redraw if `radiant_channel_id_alt_run()` reaches
  `RADIANT_ADDR_ALT_RUN_AVOID` for any of the seven types `ant_identity.py`
  checks. Bound it (16 draws is ample), and on exhaustion keep the last draw
  rather than fail the boot. An all-zero draw must still fail as an RNG failure,
  not be redrawn.
- Do not change `derive_devnum()` or touch an already-provisioned key: existing
  nodes keep their numbers and paired receivers.
- Done when a test_node_ident case gives 5d00..00 then a good key from
  tests/fake_rng.c and the second key is the one persisted. Note the rule in ADR
  0020.

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
