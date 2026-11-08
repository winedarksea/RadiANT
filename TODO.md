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
- Only one agent may use the bench at a time. Items 5 and 6 need no hardware
  and can run in parallel with anything.

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

## 2. [DONE 2026-11-07: HOST TOOL, not firmware] Dongle did not answer SYSTEM_RESET
**Cause:** a host read race. ant_ack_gate.py read USB through a 50 ms
FrameReader; STARTUP arrives 52.2 ms after SYSTEM_RESET; a libusb-win32 read
timeout that lands as a frame arrives loses it whole.
`tools/ant_reset_loop.py` on the Dongle, idle:
- 50 ms read: 11/1000 lost (1.1 %).
- 52 ms read: 10/1000 lost.
- 1000 ms read: 0/1000 lost.
With a channel tracking: 0/500 at 50 ms (mechanism not established).
**Fix:** `ant_ack_gate.reader_timeout_ms()`: USB gets `READ_TIMEOUT_MS`, and
only serial endpoints keep 50 ms. Verified 0/1000 through open-then-reset.
Evidence: archive/benchmarks/2026-11-07-reset-and-power/.

(Original notes follow.)
- Board: nRF52840 Dongle, serial 3D55F77818BE772A.
- In round B acked pass 1 (B_acked_p1_Dongle.log), ant_probe.reset_stack sent
  SYSTEM_RESET and got no STARTUP message within 3 s. ant_ack_gate then
  raised "endpoint ... did not reset".
- It happened once in about 94 endpoint opens across the run, and the next
  pass was fine.
- Task: loop reset_stack on the Dongle (500 or more times, with and without a
  channel open and traffic flowing) to get a rate. If it reproduces, find
  where STARTUP is dropped or delayed.

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

## 4. [DONE 2026-11-07: decided "boost the unset default and level 4 only"]
**Change:** the boost raises only the unset default and LVL_4; an explicit
LVL_3 is a true 0 dBm. It asks for LVL_5 on every radiant build and lets the
backend round, so the 54L15 goes to +8 (was +4) and CC26xx to +5 (was +4);
the 52840 (+8) and the 5340 net core (0) are unchanged.
**On air, 54L15 transmitting, Dongle RSSI, A/B/A:**
- unset default: -21.0 / -21.0 dBm, the same as explicit LVL_5 (-20.96).
- LVL_3: -27.9 dBm.
- Levels 3, 2, 1 and 0: a slope of 1.05 dB per dB.
- Fine ladder: dial check PASS (0.99 dB per dB, worst rung 1.1 dB off).
ant_sens's fine ladder now sends every rung raw, because its +4 rung sent as
LVL_4 would now measure +8.
Evidence: archive/benchmarks/2026-11-07-reset-and-power/.
**Open (found doing this):** ant_sens reports a spurious 17.5 % loss on short
rungs whose basis is `event_counter` (0 RX_FAIL, every packet received), which
produced a fake knee. See that README.

(Original notes follow.)
- CONFIG_ANT_DONGLE_TX_POWER_BOOST defaults to y. It maps ANT power levels
  3 and 4 to the part's ceiling: +8 dBm on a 52840, +4 on the 54L15, 0 on the
  5340.
- In ANT, level 3 means 0 dBm and is the default, so a host that never sets
  the power transmits at +8 dBm on a 52840.
- It also skewed the comparison: the 54L15 ladder's "0 dBm" step was really
  +4 dBm and failed its dial check.
- Options:
  - keep the boost and document it;
  - boost level 4 only;
  - default the boost to n.

## 5. [DONE 2026-11-07] ant_verify decodes FE-C pages as bike-power pages
**Fixed:** `ChannelAnalyzer.feed()` routes on device type first; the
bike-power decoders run only on 0x0B, FE-C gets `_feed_fec()` (speed from
0x10, power/cadence from 0x19 via the new `ap.decode_fec_trainer()`), and SDM
and unknown types stop at the page histogram. Worse than noted below: the
misrouting also produced two **false FAILs** on every FE-C channel
("accumulator continuity: 74 violations", "sensor liveness: advanced on 0 of
681 pairs") from the constant equipment-type byte read as the event count, so
any run with an FE-C channel reported FAILED. Cadence n=672 was page 0x10's
distance byte (460) plus page 0x11's cycle-length byte (212).
Test: tools/test_ant_verify.py `FitnessEquipmentIsNotBicyclePower`.

(Original notes follow.)
- ant_verify.py:625-634 routes by page number alone. On an FE-C channel
  (device type 17), page 0x10 is General FE Data, but it goes to
  _feed_power_std.
- The result is a "power" of 9471-17663 W (mean 13843) on every receiver,
  including the Garmin. Those values fit speed in mm/s. The cadence figures
  (up to 254 rpm) are probably wrong for the same reason.
- Fix: send pages to the bike-power decoders only on device type 11. Either
  give FE-C its own decoder (power comes from page 0x19) or report nothing.
- Add a test that replays FE-C pages and expects no bike-power figures.

## 6. [DONE 2026-11-07] ant_features rejects 7-byte capabilities
**Fixed:** the step-1 gate is now `ant_features.capabilities_usable()`, which
needs `MIN_CAPABILITIES_LEN = 7` bytes (the only byte read is [6], advanced
options 3); a reply shorter than 9 logs "standard 7 bytes; extended bytes
absent". **Correction to the note below:** only line 646 was the capabilities
check. Line 373 checks the SDU mask readback (1 index byte + 8 mask bytes = 9),
which is correct and unchanged. Test: tools/test_ant_features.py with the
Garmin's real reply `080800ba3600df`. The Garmin re-run of the tool itself
waits for the stick to be plugged in.

(Original notes follow.)
- ant_features.py:373 and :646 require at least 9 bytes. The Garmin USB-m
  returns 7, so the tool exits with rc 1.
- Fix: decode the standard bytes and treat the extended ones as absent.
- Add a test using the Garmin's real 7-byte reply.

## Not bugs (rig follow-ups, for context only)
- The sensitivity ladder never reached the Garmin's knee. The boards need
  more distance; fill in [placement] in
  tools/compare_rigs/2026-11-07-fixed.toml.
- The 54L15's lowest custom TX power is -28 dBm; the 5340's is -40 dBm.
- Acked transfers at n=120 can never show Equal under the 0.5 pp rule.
  That needs about 770 transfers per receiver.

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

## 2. [FIRMWARE, MEDIUM] Implement the descriptor authentication frame
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
**Done:** 0xF4 is now 26 bytes with a structure version (1) at [1] and
`enrolments` (u16 LE, saturating) at [24..25]; readable by a host on the
enrolling node, not over the air (docs/radiant-security.md 11.7.2).

(Original notes follow.)
- protocol/ant_wire.yaml MESG_RADIANT_SEC_STATUS_ID is a fixed 23 bytes with
  no version byte. 0xF6 learned this and carries one.
- docs/radiant-security.md 11.7.2 already defers the `enrolments` counter
  because 0xF4 has no room. That counter is the only on-air-visible sign of
  an enrolment the owner did not perform.
- Task: add a structure version and `enrolments`, regenerate with
  scripts/gen_ant_wire.py, update tools/ant_sec.py STATUS_LEN and the api
  tests. Cheapest now, while no release image answers 0xF4.

## 4. [DESIGN DECISION - ask the user, do not just change it] The epoch in descriptor frame 1
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

## 5. [DESIGN DECISION - needs an ADR] Provisioning so identity Tier 0 actually runs
- Nothing in apps/ calls node_ident_provision() (hrm-reference-design.md 5.1,
  and the comments in apps/hrm_ble/src/main.c and apps/treadmill/src/main.c).
- Consequence: Tier 0, the only privacy feature that is on by default, never
  runs on any board. The Kconfig device number ships, and with no K_dev a
  hostless node has no epoch source and no pairing path.
- Task: write the ADR choosing among the three shapes in
  hrm-reference-design.md 5.1, then add the one caller it chooses.

## 6. [DONE 2026-11-07] README calls it "three independent switches"
- README.md:389. X_PRIV was withdrawn (ADR 0006); there are two switches.
- Now reads "the two independent payload switches and the identity rule",
  matching the title of docs/radiant-security.md.

## 7. [DONE 2026-11-07] The secured counter counts pages; the receiver counts periods
**Fixed:** the counter is the sender's slot index, derived from the slot's
t_sync (radiant_sec_tx_transform() gained the argument; ant_radio_radiant.c
passes radiant_channel_next_slot()). The first secured transform after 0xF3
latches the phase; slots are spaced with radiant_channel_counts_to_us(), the
channel's own rounded spacing, so it cannot creep. A wrap of more than one
epoch is applied at once; a slot posted twice returns its cached bytes; an
earlier slot is refused. profile_sched now advances byte [1] every slot, and
tools/ant_pages.py's TlmPageScheduler mirrors it; ant_verify reports 0x60
exact loss as delta - 1 - heard-between (scope "slot"), withheld when a pair
hears more than its delta allows (a pre-rule node).
Evidence (radiant/tests on the nRF5340 DK, 20 000-slot streams):
- Before (master radiant_sec.c, same tests): a 12-slot clear run per cycle
  verified 816 / 353 / 162 windows at W = 2 / 4 / 8 against 8838 / 4253 / 1941
  the slot index says should, and nothing in the last cycle; skipping 1 slot
  in 40 at W = 8 verified 900, 1538 unverified; slot-aligned sparse X_CONF sent
  counter 3 at slot 1003; a re-posted slot built a second ciphertext.
  dropped_replay stayed 0 throughout: nearest-rollover resolution keeps every
  counter within 128 of expected, so the failure shows as windows that never
  verify (wrong nonce, wrong window), not as replays.
- After: every window the slot index says should verify does, none other does,
  dropped_replay 0, in all ten stream runs; radiant/tests 914/914,
  radiant/tests/api (SEC + host messages) 66/66.
- Cost of a clear slot in a window: docs/radiant-security.md 3.2 (2-14 % of
  secured pages unverified, W = 2..8). Aligning the runs is item 7a.
- Size: security text +5948 B (ceiling 6144), bss +6493 B (ceiling 7168),
  i.e. +228 / +192 against the same-day baseline.

(Original notes follow.)
- radiant_sec_tx_transform() (radiant/src/radiant_sec.c) advances the counter
  only for pages inside the secured range. The receiver's expected_index() is
  elapsed time divided by the channel period: one count per slot.
- Every 0x60 node also sends descriptor frames and common pages 80/81 in each
  121-message cycle (radiant-telemetry.md section 6). Those slots do not
  advance the counter, so it falls behind the receiver's clock every cycle.
- Since 4f5cf4f (2026-11-06) the receiver corrects its anchor by at most
  period/16 per verified window, which is slower than the gap grows. With 5 to
  12 unsecured slots per cycle the gap passes the 128-period slack in roughly
  8-45 min at 4 Hz; a larger W and a longer descriptor are faster. After that
  every packet is counted as dropped_replay. The old per-packet re-anchor hid
  this.
- Sparse mode cannot work on a secured channel at all: the first packet after
  a silence longer than 128 periods is outside the slack. Section 3.1 of
  docs/radiant-security.md lists sparse mode as a case the design handles.
- No test sends unsecured pages inside a secured stream.
- Task: write the failing test first (a descriptor set plus 0x50/0x51 in every
  121 slots, 20 000 slots, verified windows expected throughout). Then define
  the counter as the slot index: advance it on every transmitted slot, and
  derive it from time on a sparse node. Amend radiant-telemetry.md section 6
  ("counts data pages only") and its loss-detection rule to match.
- Do this before item 2: it is the counter's meaning on the wire.

## 7a. [DESIGN, deferred by decision 2026-11-07] Align clear runs to MAC windows
- With the counter now the slot index (item 7), every window that holds a
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
- Task: build ADR 0018's recommendation (hold the elastic grant through each
  boundary, decided at the placement deadline), adding the per-set coverage
  counter first. A/B/A with the stepped-offset harness. That harness
  (disc_phase.py) lived in a session scratchpad and may be gone; if so,
  rebuild it into tools/.
- Done when the offsets that failed bind as fast as the control with no
  tracked channel.

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

## Reviewed and not moved here
- X_CONF without X_AUTH is legal. Its verdict is CLEAR, so a host is told it
  is unauthenticated, and its integrity is no worse than a clear channel.
  The real fault is wording: docs/radiant-security.md section 2 calls
  unauthenticated encryption worse than none, while section 3 allows it.
- The deterministic pairing scalar on an RNG-less node ties every past pairing
  to K_dev. Recovering K_dev needs physical capture, which ADR 0009 puts out
  of scope and which already exposes the current root key.
- The coded-PHY 6 dB gate, nRF52840 errata 191, the unused ED-scan caller and
  the heart-rate toggle-bit bench are already recorded as owed in ADR 0007,
  ADR 0012 and ADR 0008. None affects a default build, since the coded PHY is
  default n and no app enables the compat layer.
- radiant_sec_configure() keeps the last-slot re-post cache. A re-post of the
  same slot right after a W or switch change resends bytes built under the
  old configuration. That is harmless: the nonce is the same, and the next
  slot uses the new configuration.
