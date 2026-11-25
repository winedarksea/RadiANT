# TODO

Open work only. Finished items are removed once they land; what changed is in
[CHANGELOG.md](CHANGELOG.md), and how is in git history and the evidence
READMEs under `archive/`. Each item says where it came from, so the original
finding can be found with `git log -S` on an older TODO.md.

Bench rules: one agent on the bench at a time; Python is
`C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe`; run
`. .\scripts\env.ps1 -NcsVersion v3.4.0` before git or west; flash DKs only
with `scripts/flash_sim_jlink.ps1` (never a hand-written J-Link
CommanderScript); never kill ant_sim; Feather flashes are rationed, reflash the
nRF52840 Dongle over DFU.

# Owner steps

Checks and decisions left over from work that has otherwise landed.

- [ ] **Release workflow.** One `workflow_dispatch` run of `release.yml` before
  the next tag. It is the first real Linux run of the pinned nrfutil bootstrap
  and of the hash-locked Matter gate install (sec review 2026-11-20 item 8,
  2026-11-21). Only a tag exercises the `publish` job and
  `scripts/check_release_artifacts.py`. Known residual: a compromised build row
  can still upload an image with a matching sum.
- [ ] **Release settings in the GitHub UI** (2026-11-24, with the
  `publish` hardening below). `publish` now runs in a `release` environment,
  which the first tag run creates *unprotected*: add a required reviewer and a
  `v*`-tags-only deployment rule. Also turn on immutable releases and add a tag
  ruleset for `v*` (restrict creation and deletion). After the next tag, run
  `gh attestation verify <image> -R winedarksea/RadiANT` on one downloaded
  image.
- [ ] **Matter endpoint ids, on an L15 Matter image** (sec reviews 2026-11-20,
  2026-11-21):
  1. Bind 3 sensors, unbind the newest, reboot, bind one: it must get a new id.
     Also one upgrade boot from a store without `br/brd_ep_hw`.
  2. Provider slots: on a debug image whose `StoreEndpointHighWater()` fails for
     the first ~30 s, leave one sensor pending through more than
     `CONFIG_BRIDGE_MAX_BRIDGED_DEVICES_NUMBER` failed retries, then let writes
     succeed. That sensor binds, and as many more as the quota allows.
  3. Storage-fault boot: write a 1-byte value to `br/brd_ep_hw`, reboot. Expect
     "Endpoint high-water mark unreadable", stored endpoints restored, and no
     endpoint for a new sensor. Restore a valid 2-byte value: the next bind
     retry (within 1 s) gets an id at or above it.
- [ ] **Zwift shim key redaction end to end.** Run `ant_zwift_visible.py --dll`
  on the bench against the redacting shim (sec review 2026-11-20). The
  2026-11-24 Zwift rides do not cover it: their log ran at level 1, which
  logs message ids only (none of the 33 MB holds key bytes). The masking
  is level 2.
- [ ] **Nordic report on MPSL assert 69:108.** A draft is in
  `archive/benchmarks/2026-11-15-assert-69108/nordic-report-draft.md`; posting
  it is your call. DevZone thread 128550 reports the same `69, 108` from
  Nordic's ESB timeslot code.
- [ ] **Hardware test round on a Mac.** Most host testing runs on Windows here
  and Linux (Bluefin); repeat the device tests from macOS.
- [ ] **Windows: a host tool that dies mid-frame swallowing the next tool's
  first command** (low). Probably fixed by the 2026-11-16 host-frame parser
  change (LEN 39-255 dropped, `0xA4` resync), but never re-tested. Re-run the
  reopen-without-bus-reset test (0 of 10 before) and drop the CHANGELOG known
  issue if it passes.

# MPSL

- [ ] **Multi-packet bursts on MPSL builds.** Not built, a scope gap rather than
  a regression: Zwift uses acknowledged messages. This covers retries too. A
  retry at +3110 us falls outside the data packet's grant, so on MPSL the
  transfer fails with NO_ACK. Inbound fails the same way: Garmin -> L15
  `gate.conf`, 100/100 bursts failed with only packet 0 delivered
  (`archive/benchmarks/2026-11-13-inbound-burst-gaps/` section 1).
  - Giving each ACK its own reserve does not help. The gate serves an in-grant
    arm only if that arm's reserve ends inside the current grant, and
    fixed-class grants never grow. So the whole chain must nest inside the first
    grant (tracked window + 4150 us).
  - The fix, for both directions: on-demand extension of fixed-class grants in
    `radiant_radio_nrf_gate_mpsl.c`, ending the burst when an extension is
    refused.
  - The same work should settle attribution (owner, 2026-11-14). After an
    in-grant ACK window, `in_reserve()` (radiant_sched.c) sets `reserve_end`
    whether or not the gate could hold it. A retry or next packet is then armed
    at the in-grant lead, and the gate refuses it EDENIED (`arm_denied`,
    `refused_too_near`) where the scheduler should report MISSED. The fix needs
    a capability saying follow-ons do not chain (cc26xx extends its endTime, so
    clipping in the scheduler would be wrong there). Since e66ae02, only
    multi-packet bursts reach this path.
- [ ] **Scan airtime under tracking, and the MPSL Dongle's FE-C collapse.** One
  bench session covers both (Zwift captures 2026-11-15, item 4; ADR 0022 follow-up).
  - With 3 pinned channels tracked, the MPSL gate's scan heard 3 frames in
    3.66 s, then every sensor within 1 s once Zwift closed the pinned channels
    (`zwift-20261115-214722` at 271.8 s). The likely cause is the ADR 0018/0021
    arm lead, not the short match.
  - On that ride the nRF52840 Dongle completed 20 of 222 FE-C commands; the Pro
    Micro completed 181/213 and 174/200. The Dongle heard the Kicker about 8 dB
    weaker (-75 against -67 dBm). When the master frame was heard, 57 % of
    commands got no ACK (11-13 % on the Pro Micro). At equal RSSI power lost 17 %
    against 9 %, and the scan ran at 57 % of the Pro Micro's rate. Board and
    placement are confounded.
  - Method: put the PCA10059 on the Pro Micro's port, in the same position and
    orientation, and gate on Kicker RSSI near -67 dBm. Then A/B the direct image
    against `gate.conf` there: `ant_scan_rate.py --track`, power loss, FE-C
    reacquire time, "heard, no ACK" rate.
- [ ] **Leftovers from the 69:108 acceptance (2026-11-14/20).**
  - Master-originated acked data on the gate image: 58/60 twice, against 60/60
    on 2026-11-14. Not attributed: there was no same-sitting run of the earlier
    image.
  - 2 h Thread leg: DUT ch0 loss 1.87 % against the Garmin's 1.43 % (the DUT was
    lower than the Garmin in the other three legs). The child's age read 210 s
    of a 240 s timeout at the end.
- [ ] **Gate hazards found by review, not observed.**
  - In-grant `gate_acquire()` returns GATE_GRANTED unmasked, and the caller then
    programs the radio from SWI or a thread. A TIMER0 backstop END between the
    two leaves `program_ed()` enabling RXREADY (only with
    `CONFIG_RADIANT_ED_SCAN`), or `program_rx()` writing SHORTS/connections on a
    radio that is no longer ours. Improbable (end margin).
  - A TIMER0 COMPARE0 that fires while a RADIO signal returns END can leave
    TIMER10 pending after END. Not cleared on purpose, since MPSL uses other CCs
    on that timer. No TIMER-context assert has been seen.
  - Latched RADIO EVENTS_* (END, ADDRESS, READY) are left set at hand-back.
    Harmless beside the 802.15.4 driver, which resets the radio. Unknown beside
    the SoftDevice Controller.
- [ ] **No retained record of an MPSL assert across the reboot.** The
  reset-cause line reads `0x00000002` after both a flash and an assert reboot.
  `apps/common/diag_flash_log.c` is not built into `apps/dongle_thread`, and
  flash writes from the assert context are unsafe under MPSL. Proposed: a
  `__noinit` RAM record read at the next boot. Also, a bare `thread.conf` still
  sets `CONFIG_RESET_ON_FATAL_ERROR=n`, so it halts rather than reboots.

# Bursts and acknowledged transfers

Multi-packet bursts are a lower-priority feature. Handling them must not
noticeably degrade primary functionality, such as receiving a heart rate strap.

- [ ] **An 0x8A from a Garmin master sometimes gets no answer** (on air,
  2026-11-13). Seen 1/20 at normal power, and three in a row in the lossy leg,
  while the sniffer heard it clearly. Also, a Garmin master does not retry a
  slot-opening 0x8A when its ACK is lost, so our ACK repeats after a lost
  opener ACK are always wasted (5 x 3.1 ms).
- [ ] **0xAA from a master.** No non-RadiANT slave's 0xAA reply offset has been
  recorded. In the fixed leg (442d4f7) our slave also counted rx_fail 8 and one
  go_to_search on broadcasts (control: 2 and 0). Not investigated.
- [ ] **Advanced burst.** True 16/24-byte packets on air (today a 24-byte block
  goes out as three 8-byte packets), the retry-extension byte
  (`api_adv_burst[10]`, stored and never read), and the stall count and retry
  defaults (sdk-ant reads back stall 3210).
- [ ] **A burst on an open slave channel is untested.**
- [ ] **Slave burst opener uses the nominal period** (low priority, not
  planned). The START goes at last t_sync + nominal period + 2190 us
  (`radiant_channel.c`). The opener is therefore shifted by the master's clock
  error: +7.6 us against the L15 with `RADIANT_NRF_LOW_POWER`, well inside the
  window, and every burst completed. Fix if it ever matters: predict with the
  drift-tracked period.

# Scheduler and core timing

- [ ] **`pass()` masks interrupts for its whole body, on every backend**
  (radiant_sched.c). TI's 9-channel worst case is 492 us, with 19-26 us of arm
  slack. Candidates: run done callbacks and event posting after the unlock, and
  take `RF_scheduleCmd` out of the lock
  (`archive/benchmarks/2026-11-16-ti-cmd-stall/isr_blocker/`).
- [ ] **nRF: `radiant_frame.c` is outside the O2 hot set.** Its
  `radiant_addr_alt_run()` bit loop costs ~17 us per filter on a filter-memo
  miss. `rx_sw_addr_len()` is already in the set.
- [ ] **A carrier that acquires while other channels search loses frame 1** (low;
  sec review 2026-11-14 item 9). The sweep chunk is cut at acquisition only when
  no other channel is searching. Otherwise the carrier keeps its slot until the
  chunk ends (about one period + 10 ms), so frame 1 can drop, and that window is
  UNVERIFIED under X_AUTH. Candidate fix: hand the sweep to another searching
  channel's slot without a gap; needs the scan and sweep regression checks.
  Background-scan rigs (the Zwift model) never hit this.

# TI (CC26xx)

- [ ] **Possible: the first Zwift probe of the XDS110 auxiliary port stalls
  the LaunchXL** (low; a hypothesis from two failed rides, 2026-11-23 and
  2026-11-24 16:38). Both failed rides ran on a heavily loaded host, so this
  is unconfirmed. The clean ride after a restart (16:49) passed.
  - Zwift probes every COM port: it opens the XDS110 Auxiliary Data Port
    (COM10), sets 115200 and writes `8a9a0800 "RideOn" 0204`, every 1.5-2 s,
    starting at the same moment as the HR/power ASSIGN burst on COM11.
  - In both failures COM10 first read 115190 baud, and that first
    SET_LINE_CODING completed about 100 ms late in USBPcap, while the ANT
    port's traffic stalled and the ch1 responses were lost. In both passes it
    already read 115200. Host load can skew USBPcap timestamps and starve IN
    URBs, so the two causes are not separated.
  - Cheap test if it recurs: reset the XDS110, then ride on an idle host.
    Workaround: open COM10 once at 115200 before Zwift, or disable COM10.
- [ ] **The Pro Micro -> TI link loses 8-13 %; other links lose 0.25-4 %.**
  Unexplained. It is not RX buffering and not the masked pass
  (`archive/benchmarks/2026-11-16-b4-pair/gate_extra/`).
- [ ] **`FS_SETTLE_US` (250) is unmeasured** (`radiant_radio_cc26xx.c`).
- [ ] **A frame in flight when `0xF1` lands** (low; sec review 2026-11-17 item 2).
  Sniff whether it completes on the CC2652 (L15 promiscuous rig).
- [ ] **DENIED vs ABORTED is judged at callback time, not abort time.** A known
  limit of backend drift item 4.

# Receiver sensitivity against the Garmin

- [ ] **Does a RadiANT receiver need more signal than the Garmin?** Compare the
  two with `tools/ant_sens.py` at equal input (opened 2026-11-12). The goal is
  as sensitive or better, not an exact match. Related: the weak-signal residual
  after SLIP_GUARD (ADR 0019). The Garmin's `0x60` level 4 is its weakest
  level, not +4 dBm; use level 3 for 0 dBm.

# FE-C coverage

- [ ] **Simulator gaps** (`radiant_profile_fec_tx.c`, `apps/trainer`). Page 55 is refused
  on request (`radiant_profile_fec_tx_request()` has no USER_CONFIG case). The page-70
  "reply acknowledged" bit is decoded but never acted on. A page 71 is sent in
  reply to a page 70; check that against the spec.
- [ ] **Never run on air:** 0x31 ERG, 0x30 and 0x32; page 70 -> 54/55/71 round
  trips; acked replies from the master; Zwift against the sim; a commercial
  trainer's page 71.

# Security

- [ ] **Two live channels under one root and one device number** (v2, protocol
  change; sec review 2026-11-14 item 5). Refused today. Allowing it needs the
  device type in the key derivation or the nonce. Related, and judged safe: a
  spread-MAC channel and a compat channel under the same root and number are
  not checked against each other (different MAC domains, and compat has no
  `K_enc`). Re-check if either MAC input format changes.
- [ ] **Align clear runs to MAC windows** (design, deferred 2026-11-07). Every
  window that holds a clear slot (descriptor, page 80/81) is unverified, and so
  is the window before it (cost: `docs/radiant-security.md` 3.2). The fix would
  have radiant_profile_sched place each cycle's clear run on whole windows. Costs:
  - it breaks the certified 121-message cadence (it would need a multiple of W,
    such as 120);
  - it spends up to W-1 padding slots per cycle (~6 % at W=8);
  - it couples radiant_profile_sched to radiant_sec;
  - MPSL-denied slots break the alignment;
  - it cannot be enforced on host-driven masters.

  Today's behaviour fails safe: nothing is ever wrongly VERIFIED.
- [ ] **Compat `set_key` releases the context when a re-import fails after the
  old root is destroyed** (sec review 2026-11-14 item 7b). Fail-closed by
  reading. Test it when an opaque (PSA) backend lands; the portable backend
  cannot reach the branch.
- [x] **A device-number change keeps queued ciphertext** (latent; reviewer,
  2026-11-22). `radiant_sec_try_devnum()` (`radiant_sec.c`) changes the device
  number, which is part of the nonce, and resets the verify state, but does not
  purge the receive queue. A page queued under the old number would be
  decrypted with the new one and delivered as garbage. Nothing in production
  calls it yet. Fix: `q_purge(ch)` there, with a red test like the 2026-11-21
  epoch/policy purge tests.
  *Done 2026-11-24 (wip/s24-sec-host-api):* `radiant_sec_try_devnum()` purges the queue;
  red test `test_a_new_devnum_candidate_drops_ciphertext_still_queued`. The
  2026-11-21 "deliberately not purged" had no recorded reason (that item named
  epoch and policy only, with 32 B of text left); try_devnum already dropped
  the held windows, so the queue was the inconsistency. Costs 0 B in the
  dongle: nothing in production calls it.
- [x] **`rx_process()` returns silently when `advance_epoch()` fails** (headroom
  or ceiling; low; sec review 2026-11-17 item 3). Nothing is counted.
  *Done 2026-11-24 (wip/s24-sec-host-api):* counted in `dropped_replay`, the
  "time-inconsistent" half of 0xF4 [18..19] (+4 B in `radiant_sec_pump`); test
  `test_a_wrap_the_receiver_cannot_take_is_counted`.
- [ ] **Host Tier I announcement check** (low, host tool; sec review 2026-11-17
  item 6). The host checks an announcement at the last verified Tier I counter,
  but the firmware tags at the clock's interval index. An announcement whose
  Tier I page was lost, or sent before it, fails on the host.
- [x] **Recovering a node whose K_dev record is corrupt** needs a partition erase
  or a separate delete API, because provisioning refuses by design (low; sec
  review 2026-11-17 item 7).
  *Done 2026-11-24 (wip/s24-node-ident):* `radiant_node_ident_wipe()` deletes
  every identity record (K_dev first, so a wipe cut short is still refused at
  boot and a second wipe finishes) through the new seam call
  `radiant_node_nvm_delete()`, which clears a record that does not load too;
  the next boot is a first boot with a new K_dev. It is also the Tier 1
  factory reset ADR 0020 now names. No application wires a trigger to it yet
  (a product decision: button, shell command). Tests:
  `test_a_wipe_clears_a_corrupt_k_dev` and four more in `test_node_ident.c`.
- [ ] **Escape binding labels in MQTT discovery JSON before any path sets one**
  (deferred 2026-11-20: not reachable today, since the only production bind
  passes a NULL label). Whoever adds editable labels must first add a bounded
  JSON string encoder in `compose_discovery()` (`mqtt_sink.c`) and a round-trip
  test through a JSON parser.
- [x] **Bind the Zwift shim to the intended serial device** (deferred; sec review
  2026-11-21). With no explicit port, the shim takes the first CP210x registry
  entry and falls back to COM15. It then forwards host writes there, key-setting
  messages included. Remove the COM15 fallback, remember a user-selected device
  instance or serial number, and report no device when the selection is absent
  or ambiguous (`tools/zwift_shim/shim_util.c`, `dsi_siusbxp.c`).
  *Done 2026-11-24 (wip/s24-zwift-shim):* no registry guess and no COM15
  fallback. `shim_select_port()` (new `shim_port.c`) takes `RADIANT_ANT_COM`
  (COM1..COM999 only) or `RADIANT_ANT_DEVICE` (a USB serial number or device
  instance id of the port or a device above it, resolved against present
  ports with cfgmgr32 each enumeration); none, unusable, ambiguous (an
  XDS110's serial names both its ports) or the two disagreeing reports no
  device, with the reason and the candidates logged once per change.
  Installer `-Device` and `-ListDevices`; `-Port` and the user's pinned
  `RADIANT_ANT_COM=COM11` work unchanged (bench PASS on the TI, also by
  instance id). Tests: `ShimSelectTest`.

# Matter bridge

- [x] **"Absent vs unreadable" high-water mark is only partly closed**
  (reviewer, 2026-11-22). Zephyr's `settings_load_subtree_direct()` drops
  backend read errors, so a fault that hides the stored `br/brd_ep_hw` entry
  still reads as Absent, and the max(restored)+1 fallback can reuse an
  endpoint id. Find a way to tell the two apart (a backend that reports
  iteration errors, or a second marker key). Until then, correct the comment
  in `bridge_storage_manager.cpp` (`LoadEndpointHighWater()`, ~:414-417), which
  claims `HasEntry()` separates a missing key from a read error; it cannot
  when the fault hides the key.
  *Done 2026-11-24 (wip/s24-node-ident):* a second marker key, the same
  pattern as the node-identity format marker. `br/brd_ep_mk` (one byte) is
  written only once the mark is in storage (`EndpointHighWater::NeedsMarker()`:
  after a successful `StoreEndpointHighWater()`, or by a load that finds the
  mark Valid on an unmarked store), and `Classify()` takes whether it exists:
  a missing mark beside the marker is Unreadable (new ids deferred), and only
  neither key is the legacy fallback. The comment is corrected (HasEntry()
  iterates the same backend, so a hidden key reads as missing there too).
  `matter_storage` +5 (hidden mark on a marked store does not reuse, one
  marker write per store, upgraded store marked at boot, lost marker
  rewritten, failed marker write retried) plus marker rows in the classify
  test; red on the codec with the marker ignored. `bridge_storage_manager.cpp`
  and `bridge_manager.cpp` compile-checked against an nRF54L15 Matter build's
  flags only (no Matter image built or run). Open, owner call: a lost mark
  never comes back, and nothing in this image clears the bridge store short of
  a settings-partition erase (CHIP's factory reset here deletes only `mt/`,
  `CONFIG_CHIP_FACTORY_RESET_ERASE_SETTINGS` is off, and
  `BridgeStorageManager::FactoryReset()` has no caller); wiring that call into
  the Matter factory reset would give the Unreadable states a recovery.

# Backends: shared HAL contract layer

From the 2026-11-18 TI vs nRF drift audit (item 6, refactor, ~200-300 lines).
Small pure helpers that both backends call; register access stays in the
backends.

- [ ] Request validation with one set of error codes
  (`radiant_hal_validate_rx/tx()`), and `op_id_next()`. nRF `rx_arm` skips the
  `radiant_rx_filters_separable()` check that TI makes. That is dead code on nRF
  today (`min_filter_hamming_bits` is 0). Owner call: if wanted, add a HAL
  error-table row (ENOTSUP), then the nRF call, ahead of `gate_acquire()`.
- [ ] Terminal and RX event fill: the window gate on `t_sync`, the `wild_value`
  policy, CRC-fail gating, and the terminal `t_sync`/`exact`/noise. TI
  `rx_drain_entries` vs the nRF `radio_isr` END branch. `wild_value` differs:
  TI sets it only under `CONFIG_RADIANT_SEARCH_SHORT_MATCH`, nRF whenever
  `filter.wild_last` is set.
- [ ] Window priority rule `(t_close - t_open) >= 10000 ? NORMAL : HIGH`, which
  is written three times (nRF `rx_gate_prio`, inline in nRF `rx_arm`, TI).
- [ ] TX start formula with a named calibration term. The calibration sign is
  opposite: nRF subtracts `T_SYNC_CAL_TX_US`, TI adds
  `CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US`.
- [ ] Early-arm diagnostic (`nrf_diag_early` / `cc_diag_early`) and
  `term_ok`/`term_empty`/`term_error`, which are near copies. Move them to
  `radiant_diag.c`.
- [ ] Smaller: TI's CRC16 could use `radiant_crc16` for the CCITT case, and the
  two backends could share one nearest-entry TX power helper (they already use
  the same rule).

Leave apart: TI arbitration vs the MPSL gate, `radiant_rx_torn`, power and idle
code, RSSI conversion, filter and address hardware programming.

# Low power for battery-powered nodes

Apply only where it doesn't harm performance, or make it configurable: a heart
rate sensor should save power, a dongle need not. Much power saving is
application-specific, so add only general items here (or in one example such as
the HR sample). Every current so far is an estimate from the power-diag proxy
(ADR 0025); there is no meter on the bench. TI standby (`CONFIG_PM`) is deferred
(owner, 2026-11-19).

- [ ] **nRF: finish the HFXO/timer release.** `RADIANT_NRF_LOW_POWER` covers
  nRF54L15 (GRTC), nRF52840 (RTC1) and MPSL. Left:
  - the nRF5340 net core builds but is unmeasured;
  - register retention across HFXO-off is unverified on nRF54L and the 5340 net
    core (`radiant_radio_nrf.c`), so the register shadow stays nRF52-only.
- [ ] **CC26xx: let the RF core power down between distant slots.**
  `radiant_radio_cc26xx.c` still sets `nInactivityTimeout = UINT32_MAX`.
  `RADIANT_CC26XX_XOSC_HF_RELEASE` only releases the 48 MHz crystal while the
  core is off.
  - Budget RF startup in the scheduler, and let the driver power down across
    long gaps while keeping the core for closely spaced operations.
  - Check the startup cost against the 250 us in-grant arm budget, and how far
    ahead commands are posted.
  - Standalone TI image only: on coex154 builds, 802.15.4 shares the core.
  - Verify the first TX and tracked RX after wake, and acknowledged transfers.
- [ ] **Measure and tighten the RX window per tracked message.** The power-diag
  struct has only totals (`radio_rx_us`, `radio_rx_ops`). Expose window length
  per tracked message. Check the lead, and the widening after missed messages,
  for anything wider than timing accuracy needs. Re-run tracked loss when
  changing either.
- [ ] **Receivers tracking at a fraction of the master's rate.** The HRM profile
  allows 16140 or 32280 against an 8070 master. Add a test that RadiANT tracks
  reliably at 2x and 4x the period, since window drift grows with the gap. Then
  document it alongside bounded search, with the acquisition-latency tradeoff,
  which `radiant/samples/hr_transmitter/README.md` gives one sentence today.
- [ ] **Wake the API thread for work or a deadline.**
  `radiant_ant.c` wakes every `RADIANT_API_HOUSEKEEP_MS` (50 ms) even when
  idle. Block on events when there is no timed work, and arm a wakeup for the
  next retry, search timeout or transfer watchdog. The tick does more than
  housekeeping:
  - `radiant_sched.c` has no timer, so the tick is the only retry path for arms
    refused with EBUSY or ESTATE. Give that retry an explicit deadline.
  - Each denied search chunk extends the search and ACQUIRE deadlines by exactly
    `RADIANT_API_HOUSEKEEP_MS` (`radiant_search_note_denied()`,
    `radiant_channel_search_note_denied()`). Credit measured elapsed time
    instead, or search timeouts will come out wrong.

# Repository, packaging and docs

- [ ] **Move nRF52/53 to USB_NEXT** (firmware, bench, deferred). Nordic USBD
  parts default to `ANT_DONGLE_TRANSPORT_USB_LEGACY`; USB_NEXT is opt-in
  (`apps/dongle/next.conf`).
- [ ] **Move `radiant/spike` out of the source tree** (low). There are seven
  directories, built by no CI job and mostly documented for NCS v3.2.4.
  - First give a supported path to what is still live: the nRF5340 net-core
    dongle needs `radiant/spike/promisc/appcore` and `net_rtt.conf`
    (`apps/dongle/boards/nrf5340dk_nrf5340_cpunet.conf`, `.overlay`).
  - Check whether `radiant/Kconfig`'s TI COEX note still needs
    `radiant/spike/ti_phy`.
  - Move the rest to `archive/spikes/` and repoint the ~20 citing files. They
    are the provenance for measured numbers, so keep them.
  - Done when `scripts/check_links.py` passes, the 5340 net-core build works,
    and only `archive/` and history mention `radiant/spike`.
- [ ] **Clean up the ADRs and the long docs.** Users should be able to
  understand RadiANT as it works today without reconstructing its history. The
  ADRs mix architecture, investigations, measurements and proposals. Write one
  short current architecture and design-principles document, supported by the
  integration, security and backend references. The same applies to:
  - `docs/radiant-bridge.md` (2,205 lines), which mixes architecture with
    successive measurements and fixes; don't move more ADR content into it;
  - `docs/treadmill-reference-design.md` 7.4a-7.9: replace the hypothesis and
    bench chronology with verification procedures, supported behaviour and
    limits;
  - `docs/bench-runbook.md`: replace the old project phases with repeatable
    procedures and equipment needs;
  - `docs/radiant-telemetry.md`: state the current format, without references
    to earlier drafts or fixed TODO items.
- [ ] **List RadiANT in Zephyr's External Modules docs. Deferred until the
  project matures** (owner 2026-11-22); still the eventual aim, and nothing
  blocks it now that the root `zephyr/module.yml` exists. Open a "New External
  Source Code" issue for TSC approval, then a PR adding
  `doc/develop/manifest/external/radiant.rst` from `external.rst.tmpl`.
  Apache-2.0 skips the licence review. Use the "compatible, not certified"
  wording from ADR 0003. The default manifest is not the target.
- [ ] **Delete `scripts/migrate_radiant_names.py` after the first release that
  ships `radiant_ant_*`** (the one after v1.2.0). It exists for code written
  against v1.2.0's or master's old names and header paths. Also remove its CHANGELOG
  pointer from the next release's notes.

# Reviews to run

- [ ] Firmware size: smaller code without loss of functionality.
- [ ] Performance: minimal packet loss, lowest latency.
- [ ] Security, with a broad remit: look for fundamental changes in code shape
  or policy.

# Security review 2026-11-22

Findings from a review by reading at `8b7e0c3`: eight reviewers, one per area,
each main claim then re-read in the source. Nothing was built, run or tested on
hardware. No memory-safety bug was found in any area. "Library" means no
shipped app calls the code yet. Items are ordered by severity within each
group.

## Medium

- [x] **A keyed receiver accepts a wholly plain descriptor set** (library;
  radio peer without the key). `radiant_profile_desc_rx_take_auth()`
  (`radiant_profile_telemetry.c` ~:994) returns success without verifying anything when
  frame 0 has no transform bit, and `radiant_profile_desc_may_decode_data()` (~:113)
  then allows decoding. An attacker who sends a complete plain set with the
  same schema id makes the receiver read genuine, correctly MAC'd data pages
  through a forged schema, which is the case `docs/radiant-security.md` 5.3
  exists to stop; 7.6 (~:1600) says a changed transform bit fails the tag.
  `tools/ant_verify.py` (~:803) has the same gap. Fix: when a key is supplied,
  or a sealed set was authenticated before on this channel id, refuse a set
  without transform bits (`-EBADMSG`, data not decoded). Add a red test, mirror
  it in the host tools and correct 7.6.
  *Done 2026-11-23 (merge 09edcff, wip/s22-desc-plain):* `take_auth()`
  returns `-EBADMSG` for a set with neither X_CONF nor X_AUTH (reserved-bit
  only included); a receiver-side `keyed` field (never encoded) makes
  `may_decode_data()` refuse a keyed-but-unauthenticated descriptor even if
  the caller ignores the return code. The "sealed before on this channel"
  trigger was dropped: a key is mandatory in `take_auth()`. Residual,
  documented: `keyed` is per descriptor, so a keyed receiver must never fall
  back to the keyless take for that node. Mirrored in ant_pages/ant_verify
  (`--compat-key` applies to every channel); radiant-security 5.3/7.6 fixed.
- [x] **Treadmill: FE-C page 51 incline is clamped to the wire range, not the
  machine's** (shipped app, physical safety; any ANT+ transmitter in range).
  `treadmill_state_request_incline()` (`apps/treadmill/src/treadmill_state.c`)
  clamps only to +/-100.00 %, and `apps/treadmill/src/main.c` (~:343) passes
  the result to `treadmill_source_set_incline()`. The bonded BLE FTMS path
  refuses anything outside `CONFIG_TREADMILL_BLE_MIN/MAX_INCLINE_DECI`
  (`treadmill_ble.c` ~:540), and the porting guide tells integrators to set the
  machine range through those BLE symbols, which an FE-C-only build does not
  have. Fix: transport-neutral machine-range symbols for incline and speed,
  enforced in `treadmill_state_request_*()`, with page 71 answering
  REJECTED/FAIL out of range. Also bound `radiant_profile_fec_grade_to_centi_pct()`
  (`radiant_profile_fec_tx.c` ~:62) at raw 40000; it accepts up to +455 % today.
  *Done 2026-11-23 (merge 88507c6, wip/s22-incline-range):* owner call - sim
  grade (page 51, BLE grade seam) clamps to the machine range and page 71
  answers PASS; explicit FTMS set incline/speed refuse (`-ERANGE` ->
  INVALID_PARAM). New `TREADMILL_MIN/MAX_INCLINE_DECI` and
  `TREADMILL_MAX_SPEED_CENTI_KPH` (no BLE dependency) replace the removed
  `TREADMILL_BLE_*` range symbols; FE-C-only builds now narrow too. Raw grade
  40001..65534 clamps to +200.00 %. **Owner step:** a real deck.
- [x] **Matter: a spoofed FE-C equipment type drives one flash write per second
  per endpoint, with no bound** (Matter image; radio transmitter).
  `ant_matter_note_sample()` (`ant_bridge.cpp` ~:1175) sets `labelDirty` on
  every equipment-type change, and the 1 Hz flush (~:838) then calls
  `PersistRowLabel()` -> `StoreBridgedDevice()` for every row of the source,
  with no compare against the stored label and no rate limit. A transmitter
  using a bound trainer's channel ID and alternating page 16 between two types
  wears the settings partition, which also holds fabrics and the Thread
  dataset. `docs/radiant-bridge.md` (~:1463) calls the rename cost bounded per
  boot. Fix: persist only when the composed label differs from the stored one,
  accept a subtype only after several identical page 16s, and cap persisted
  renames per source. Correct the doc. `mqtt_sink.c` (~:1066) re-arms retained
  discovery on the same flip (only with `PROFILES_EXTRA` on the MQTT image).
  *Done 2026-11-23 (merge 3d10312 + 6cb4e1d, wip/s22-fec-type-wear):* the
  power adapter posts the first page 16 after bind at once and a type change
  only after 4 identical page 16s; Matter persists a label only when its
  FNV-1a hash differs from the stored record's, and at most 4 renames per
  source per boot (`LabelPersist`, every rename trigger shares the budget);
  MQTT caps subtype re-arms the same way. Matter image +732 B flash, +328 B
  RAM (98.1 %). Accepted: a spoofer whose page 16 arrives first after a bind
  can set the name (name integrity, not wear). The MQTT cap has no test (no
  mqtt_sink suite). **Owner step:** L15 Matter bench of the ant_bridge.cpp
  wiring (creation/adoption seeding, flush decision), untestable on native_sim.
- [x] **Release: shipping rows restore compiler, ccache and pip caches written
  by ordinary master CI runs.** `release.yml` (~:374) restores
  `ccache-core-<index>-` from any earlier run, and
  `action-zephyr-setup@cc223fe` caches the Zephyr SDK under a key with no
  content hash plus a pip cache. Code execution in any default-branch job at
  any earlier time (an unhashed PyPI package, the lychee binary in the weekly
  linkcheck) can therefore plant a compiler or object files that the tag build
  ships with a matching `.sha256`. Fork PRs cannot (their caches are scoped).
  Fix: no cache restore in the five shipping rows (`enable-ccache: false`, skip
  "Cache ccache" on tags, `-DUSE_CCACHE=0`), and install the SDK there with
  explicit pinned-URL-plus-sha256 steps.
  *Status (wip/s22-release-pins):* all of `build-core` is now cache-free (no
  `actions/cache`, no setup action, `-DUSE_CCACHE=0` plus `CCACHE_DISABLE=1`,
  because the -D does not reach the sysbuild image), linted in
  `tools/test_ci_actions.py`. Left: the non-shipping release jobs still use
  `action-zephyr-setup` and their caches; an owner `workflow_dispatch`.
  *Merged 2026-11-23 (f45a4eb).*
- [x] **Release: the shipping rows' toolchain is not pinned.** The same action
  downloads the Zephyr SDK with `wget` and no digest, installs the latest
  `west`, and installs Zephyr's open-range requirements unhashed. `west.yml`
  pins sdk-nrf by tag (`revision: v3.4.0`, `import: true`), and the imported
  manifest pins by tag too. The hash lock from 2026-11-21 covers only the
  Matter rows, which do not ship. `SECURITY.md` says the workflow pins the
  toolchain. Fix: pin `west.yml` to the v3.4.0 commit SHA (or build tags from a
  committed `west manifest --freeze`), set `west-version`, and use a
  hash-locked requirements file and a sha256-checked SDK in the shipping rows.
  *Status (wip/s22-release-pins):* `west.yml` pins nrf by SHA, `build-core`
  builds from `.github/west-release-frozen.yml`, installs
  `.github/release-build-requirements.txt` with `--require-hashes`, and checks
  the SDK and toolchain digests; `SECURITY.md` lists what is not pinned (runner
  image, setup-python's interpreter and pip). Left: the same for the
  non-shipping jobs if wanted; an owner `workflow_dispatch`.
  *Merged 2026-11-23 (f45a4eb).* Dry-run on Bluefin: Feather RGB, Pro Micro,
  nRF52840 Dongle (with DFU package) and nrf54l15dk rows build with the
  locked set. actionlint (first run) flags two older findings outside
  `build-core` near release.yml :1601 (`matrix.extra_cmake` undefined; SC2206).
- [x] **New USB stack: each suspend/resume leaves one more bulk-OUT buffer
  armed** (USB_NEXT images only, none shipped; any host that suspends the bus).
  `usb_ant_class_next.c`: `USBD_MSG_SUSPEND` (~:454) clears `rx_armed` without
  dequeuing the buffer `arm_rx()` queued, and `USBD_MSG_RESUME` (~:446) arms
  another. The extra buffer defeats the NAK back-pressure, so `ant_request()`
  (~:296) discards a host packet when the ring is full, and after about 30
  cycles the 32-buffer pool is empty and every reply fails until a bus reset.
  The legacy transport cancels transfers on suspend. Fix: leave `rx_armed`
  alone on suspend, or `usbd_ep_dequeue()` the OUT endpoint there. Add a
  suspend case to `tools/bench_usb_stall.py`. Fix before the USB_NEXT move.
  *Done 2026-11-23 (merge 5a29753, wip/s22-usb-suspend):* suspend/resume no
  longer touch `rx_armed`/`rx_backpressured` (udc_nrf keeps queued buffers
  across suspend; only endpoint disable dequeues). On Linux the leak never
  lost a reply: the kernel resets the device after the failed post-resume
  GET_STATUS, which dequeues the leak, so `bench_usb_stall.py
  --suspend-cycles` (root, Linux) fails on host resets. Feather A/B/A: unfixed
  reset at cycles 17 / 29+58, fixed 0 resets in 60. The 0x57 UF2 reboot
  request now also works on USB_NEXT (vendor-to-device only).
- [ ] **USB_NEXT: the first request after an open is usually lost** (found
  2026-11-23 doing item 6 above). USBD ignores a repeat SET_CONFIGURATION
  (`usbd_ch9.c` ~:142), so the device keeps its data toggles, while Linux
  turns libusb `set_configuration()` into `usb_reset_configuration()` and
  resets the host's. `ant_probe.open_device` sends it on every open: 9 of 10
  one-request opens lost the request on the Feather, 0 of 10 without it.
  Legacy stack unaffected; Windows untested. Fix: USBD patch (reset toggles /
  re-run the configuration) or skip the redundant SET_CONFIGURATION in the
  tools. Fix before the USB_NEXT move.
- [ ] **USB_NEXT: `arm_rx()` check-then-set race** (reviewer 2026-11-23,
  older than item 6). RESUME's `arm_rx()` (system workqueue) and
  `usb_ant_resume_rx()` (bridge thread) both test then set `rx_armed`;
  `usbd_ep_enqueue` can yield between, leaving one extra buffer that defeats
  NAK back-pressure. A plain CAS with rollback can leave OUT unarmed for good
  (claim held, ring-space check fails, the bridge's call already returned).
  Fix: move all arming onto the USBD thread (class `.resumed` callback, same
  thread as `ant_request`), or CAS plus a retry after rollback.
- [ ] **USB_NEXT: a deferred RESET handled after `.enable` leaves the device
  deaf** (reviewer 2026-11-23, older). With `USBD_MSG_DEFERRED_MODE` the
  message callback runs on the system workqueue; if it is stalled for a whole
  re-enumeration (~50 ms, e.g. a flash erase on that queue), a late RESET or
  SUSPEND clears `configured` after the USBD thread already ran `.enable`,
  until the next SET_CONFIGURATION.
- [ ] **Security size headroom is nearly gone** (measured 2026-11-23 on
  6cb4e1d). Security text 7828/7856 B (+4 since 7824), compat object
  4768/5120 B (+112 since 4656), compat linked 188/288 B. Not traced; the
  next security change should measure before and after.
  2026-11-24 (wip/s24-sec-host-api): the review 2026-11-22 host-API fixes
  measured 7828 -> 8036 B text (+208, breakdown in release.yml); bss
  6979/7168; compat object unchanged. 2026-11-24 (wip/s24-scan-xauth): 7884 B
  alone after the scan guess budget (+56 in `rx_close_window()`, +2 in
  `radiant_sec_pump()`). Both merged to master: 8060 B measured, ceiling
  raised 7856 -> 8092 (32 B left).

## Low

- [x] **Keyed scan channel: X_AUTH tags can be guessed at air rate.** A scan
  offers every CRC-valid frame to `radiant_sec_rx_ingest()`
  (`radiant_search.c` ~:1028, `radiant_ant.c` ~:1506), `rx_floor` is 0
  until something verifies, and a closed window can be reopened and judged
  again without limit (`radiant_sec.c` ~:1654-1671). An injector sends a chain
  of 2-frame windows, each a 16-bit guess for the one before, so the expected
  cost at W=2 is about 131,000 frames rather than the 9.1 hours of
  `docs/radiant-security.md` 7.5, which assumes one attempt per slot. 3.2
  (~:484) says nothing sent to a keyed scan verifies. The frame rate of a scan
  was not measured. Fix: refuse X_AUTH with W=2 on a scan (or force W>=4), or
  budget failed windows per period; correct 3.2 and 7.5.
  *Done 2026-11-24 (wip/s24-scan-xauth):* a per-channel budget of failed
  judgements in `rx_close_window()`: a comparison that ran and failed moves
  `rx_judge_tat` W channel periods on from max(itself, the closing packet's
  t_sync), a pass moves it to that max (keeps the 32-bit value fresh), and no
  comparison runs while it is more than W periods ahead (one spare); a refused
  window is `unverified`. That is the one-guess-per-W-periods
  rate a tracked channel's one frame per slot already imposes, so 7.5's 9.1 h
  now holds on every channel kind and the rule needs no scan-mode knowledge in
  radiant_sec. Refusing W=2 on a scan was rejected: radiant_sec does not know a
  channel is a scan, and W>=4 at air rate is still far below its documented
  bound. A tracked channel (rx_floor 0 or not) was never exposed: it hears one
  frame per slot, so the budget never binds there; the reopen itself still
  works after a verify, above the floor, so rx_floor was not the enabler. A
  genuine stream only fails a comparison when a whole window is lost (at most
  once per 2W periods), so it never meets the budget: test at 5-20 % loss
  with whole-window losses and +/-200 us jitter, W=2/4/8, verifies exactly
  the windows it did before. Reachable on the dongle only in a `security.conf`
  build (not a released image): 0x42 with extended assignment 0x01, then
  0xF1/0xF2/0xF3. 3.2 and 7.5 corrected; no host tool models the window
  judgement (`tools/ant_verify.py` judges telemetry auth frames, not this).
  Scan frame rate still not measured: with the budget the bound no longer
  depends on it.
- [ ] **Private-mode receiver accepts a replayed SWITCH/RETURN announcement for
  the rest of the Tier I interval** (library). `rx_take()`
  (`radiant_profile_private.c` ~:768) has no floor on the last verified announcement,
  and `radiant_sec_compat_announce_verify()` checks only the interval index.
  With a long interval (up to 3600 s) a recorded pair sends every keyed
  receiver to the empty locator again; at any interval a late replay moves
  `act_at` later. An injected frame A between a genuine A and B also discards
  the genuine pair. On the node, a verified but refused command (-EBUSY,
  -EAGAIN) is not recorded in the replay floor (~:505). Fix: refuse a repeated
  frame A within an interval, let `act_at` only decrease once armed, hold two
  candidate frame A's, and record refused-but-verified command tags.
- [ ] **Reliable-command backoff lets a peer without the key lock out the
  owner** (library). `radiant_profile_command.c` (~:366) drops every command
  unverified while `now < blocked_until`, genuine ones included, and only an
  accepted fresh command clears the ladder. One forged frame after each expiry
  keeps the node deaf, at up to 819 s per frame. Fix: cap the ladder near 1 s
  when the tag is 64-bit (the backoff buys nothing there), and document the
  trade for 16-bit tags.
- [ ] **A replayed command keeps the response windows and acks running**
  (library). A duplicate (~:406) and a 0x02 answer (~:422) each top
  `expect_left` back up and send a tagged ack, so one replay per window keeps a
  coin-cell receiver armed indefinitely. The 2026-11-20 fix bounded banking,
  not this. Fix: top up on a fresh accept and on the first one or two
  duplicates of a sequence only.
- [x] **"Period before epoch" is documented as a refusal and is not enforced.**
  `radiant_ant_sec_epoch_set()` (`radiant_ant_sec.c` ~:193) only checks
  `radiant_channel_period_get()`, which cannot fail for a valid channel; there
  is no period-set flag, so an unset period binds as the default 8182.
  `radiant_sec_set_epoch()` caches the period (`radiant_sec.c` ~:811) and
  `radiant_ant_channel_period_set()` has no security hook, so a later 0x43 leaves the
  slot counter running at the old rate: a master re-sends old ciphertext or
  skips counters (no nonce reuse; the `tx_last_idx` guard holds), and a
  receiver's counter drifts. `docs/radiant-security.md` 9, `radiant_ant.h`
  (~:1144) and the header of `test_api_sec_host.c` all claim the refusal. Fix:
  a `CHF_PERIOD_SET` flag checked by 0xF3, and refuse a 0x43 that changes the
  period while the channel has an epoch. Add both tests.
  *Done 2026-11-24 (wip/s24-sec-host-api):* `struct chan`'s flags byte is full, so the flag
  is `radiant_ant_sec_period_mask` (bit per channel, set by an accepted 0x43,
  cleared on assign). 0xF3 without it is CHANNEL_IN_WRONG_STATE; a 0x43 that
  changes the period under an epoch is refused the same way (same period, a new
  root or an unassign lifts it), in `radiant_ant_sec_rebind()` under api_lock.
  The only host tool sending 0xF3 (`ant_features.py` keyed scan check) now sends
  0x43 first. Docs 9, `radiant_ant.h`, the test header and the 0xF3 wire text
  updated. Tests `test_sec_host_an_epoch_needs_the_period_set_first`,
  `test_sec_host_a_period_change_is_refused_under_an_epoch`.
- [x] **Re-numbering a keyed master keeps the old device number in the keys and
  nonce.** `radiant_sec_set_key()` stores `base_devnum` once (`radiant_sec.c`
  ~:647); `radiant_ant_channel_id_set()` is a bare forward, and 0xF4 reports neither
  number. After close, 0x51, open, the master is on air under the new number
  with keys for the old one, and the host may believe (root, old number) is
  free for another channel. Fix: on a master with a security context, refuse a
  0x51 whose device number differs from `base_devnum`, or report the bound
  number in 0xF4. (A receiver re-wildcard keeping its key is intended and
  tested.)
  *Done 2026-11-24 (wip/s24-sec-host-api):* the refusal - a keyed master's 0x51 to any
  other number (0 included) is CHANNEL_IN_WRONG_STATE; the same number with new
  device/transmission types is accepted; receivers are not checked. Test
  `test_sec_host_a_keyed_master_refuses_a_new_device_number`.
- [x] **The 0xF3 monotonic gate is cleared by SYSTEM_RESET, unassign, and a
  0xF2 with a different root or number.** Root A -> B -> A, or number X -> Y ->
  X, then accepts an epoch already used under A. The comment at
  `radiant_sec.c` (~:653) says any epoch is safe after a new root "because no
  nonce under the new keys was ever used", which is false for that sequence,
  and the docs say the gate holds "within a power cycle". The host's duty to
  persist its last epoch covers it. Fix: correct the comments and 3.5;
  optionally keep a small RAM table of {key check value, number, highest
  epoch} across those three events.
  *Done 2026-11-24 (wip/s24-sec-host-api):* comments (`radiant_sec.c` set_key and
  set_epoch, `radiant_sec.h`, `radiant_ant.c` reset) and docs 3.5 now say the
  gate holds only while a channel keeps its root and number, and that the
  authority's persisted floor is the guarantee. No RAM table (not wanted this
  round).
- [ ] **Compat Tier I, locked receiver: an unauthenticated page moves `wraps`,
  the epoch and `K_auth` before its MAC is checked** (library).
  `rx_tier1()` (`radiant_sec_compat.c` ~:1053) calls `apply_wraps()` on the
  counter from the page's own bytes when `rx_t1_any` is set; the comment below
  it (~:1078) is true on first contact only. A forged page in the upper half
  of an epoch shifts wraps by one until the next call that re-applies them from
  time, which can be a whole interval on a Tier I-only receiver. No forgery
  follows. Fix: use the trial-key path for the locked branch too, and commit
  wraps only on a matching tag. Extend
  `test_a_forged_first_page_fails_once_and_moves_nothing` to a locked receiver.
- [x] **Node identity: a record that reads as absent is taken as 0** (library;
  storage fault or local flash write). `radiant_node_ident_boot()` ignores `present`
  from `load_u32()` (`radiant_node_ident.c` ~:447, :465, :488). If `radiant/epoch`
  alone is lost, the epoch falls back to the boot counter, which is about half
  of N, so an epoch already used is used again with the counter at 0 (keystream
  reuse under X_CONF). Zephyr's `settings_nvs.c` (~:157-191) deletes a setting
  whose name or value entry fails to read and never calls the handler, so a
  fault reads as absent, and a lost K_dev is silently re-provisioned, which ADR
  0020 meant to exclude. Same root cause as the Matter high-water item. Fix:
  cross-check the records at boot (EIO when K_dev is absent but a counter is
  present, or when N is absent with a non-zero boot counter), and gate the
  "older image has no N" migration on a format marker.
  *Done 2026-11-24 (wip/s24-node-ident):* a fifth record, the format marker
  `fmt` (u32 = 1), is written after every other record (by provisioning, and
  by the boot that completes an older image's store). Boot and the same-key
  provisioning retry load all five and check the present set against a table
  (comment above `records_consistent()` in `radiant_node_ident.c`): K_dev
  absent is a first boot only beside an empty store; with the marker every
  record is mandatory; without it the older-image and interrupted-write sets
  still boot (the boot creates a missing pairing counter, N, then the
  marker), except a pairing counter or N without the boot counter. Every
  refused set is EIO, writes nothing and draws nothing, on every boot until
  wiped. Recovery: new `radiant_node_ident_wipe()` (deletes K_dev first, the
  marker last, each confirmed; a wipe cut short still boots EIO and a second
  wipe finishes) over a new seam call `radiant_node_nvm_delete()`.
  `test_node_ident.c` +16: one per lost record, the marker cases, all 32
  present/absent combinations in an explicit `record_table[]`, and five wipe
  cases (every cut point, a corrupt K_dev). Red on the old code: 18 failures
  per unit scenario (the 16 plus two updated cases). ADR 0020 amended (trigger
  rule, Tier 1 reset), radiant-security.md 3.5 and section 4 updated. A double
  fault (the marker and a counter both lost) still reads as an older image's
  store; the NVS backend can also roll a record back to an older value when
  its newest ATE is corrupt, which this does not detect (not a new exposure).
- [ ] **`radiant_node_ident_wipe()` deletes K_dev but does not erase it**
  (review 2026-11-24; owner call). On NVS a delete appends a deletion entry,
  so the old key stays in flash until garbage collection erases its sector,
  and a flash dump after a Tier 1 "I am selling this" reset recovers it.
  Overwriting with zeros first does not help: NVS is append-only. The fix is
  to erase the storage partition (`nvs_clear()` on the settings backend's
  file system, or `flash_area_erase()`), which also drops Bluetooth bonds and
  Matter fabrics. Decide whether the product's factory reset does that.
  Nothing calls the wipe yet, so nothing is exposed today. The header, ADR
  0020 and radiant-security.md Tier 1 now say "deleted, not erased".
- [x] **A search record goes live before its filter is written.**
  `begin_common()` (`radiant_search.c` ~:582) sets `active` first and `want`
  last, in thread context with no critical section, and `handle_ok()` (~:1043)
  reads them from the radio interrupt. A zeroed record is a full-wildcard
  ACQUIRE, so a frame landing in that window makes a pinned channel acquire an
  arbitrary device, or turns a scan channel into a tracker. The window is a few
  instructions per search start. Fix: build the record in a local and publish
  it inside `radiant_event_crit_enter()` with `active` last; guard the memset
  in `radiant_search_end()` the same way.
  *Done 2026-11-24 (wip/s24-search-races):* as proposed; `seen_find()` takes
  the frequency as an argument because begin now asks before the record is
  published. The torn instant is two plain stores apart, so no single-threaded
  test reaches it: `test_event.c`'s crit hook now runs a probe at each edge of
  the section, and `test_begin_publishes_the_record_whole` /
  `test_end_clears_the_record_whole` offer a frame there and check the record
  is absent or whole (red before: the probe never ran).
- [x] **The sweep can offer a frame to a channel that has left SEARCHING.**
  Membership is removed after the state change (`radiant_ant.c` ~:3787
  vs :3797; timeout ~:3239 vs the later pump), and `api_search_acquired()` has
  no state check, so a broadcast can follow CHANNEL_CLOSED and the frame's one
  ACQUIRE claim is spent on a dead channel. Fix: `api_search_permit()` returns
  false unless the channel is SEARCHING.
  *Done 2026-11-24 (wip/s24-search-races):* as proposed (a scan stays
  SEARCHING). `test_a_channel_that_left_searching_is_not_offered_a_frame`: a
  timed-out channel still in the sweep took the frame and posted a broadcast
  after its timeout while the live searcher behind it stayed SEARCHING.
- [ ] **Matter: one spoofed common page creates persistent endpoints, and
  restored endpoints that are never claimed are never removed.**
  `endpoint_get_or_add()` (`radiant_matter.c` ~:487) instantiates on first
  sight, and page 84 is decoded on every bound channel, so one frame on a
  heart-rate strap's ID adds Temp/Humidity/Pressure endpoints with stored
  records. Removal exists only for rows of a source at unbind
  (`ant_bridge.cpp` ~:1220). Without an attacker, replacing a sensor after a
  power cycle orphans its endpoints, and 16 fill the table. Fix: require a
  field to be seen several times before creating an endpoint, and collect
  unclaimed restored endpoints (oldest first when the table is full).
  *Orphan half done 2026-11-24 (wip/s24-matter-ident):* `CreateEndpointForRow()`
  calls `CollectOrphanIfFull()` before it creates: only when BridgeManager has
  no free device slot, it removes the oldest unclaimed restored endpoint
  (`RemoveBridgedDevice` → storage record → `PersistIndexes()`) and creates in
  the freed slot. A row that failed to create keeps `needCreate`, so
  `AnythingDirty()` arms the 1 Hz flush and the collector runs every second
  while a row waits. The device-type-mismatch adoption path now removes the
  stale record at once instead of creating a second endpoint with the same
  UniqueID. Review follow-up: the collector skips an entry whose UniqueID a
  current row would adopt (`RowWouldAdopt()`; none left means nothing is
  collected and the create retries), and adopting an entry, or creating after
  the mismatched ones are retired, retires every other unclaimed record with
  the same UniqueID (`Orphans::NextDuplicate`), which older builds stored on
  the mismatch path. Policy (`Orphans::Needed`/`PickOldest`/`NextDuplicate`)
  is in `bridge_storage_codec.h`, tested in `dongle_thread.matter_storage`.
  **Still open:** the "seen N times" debounce (the spoof half). The CHIP-side
  wiring in `ant_bridge.cpp` (`RetireRestored()`, the collector and the
  duplicate retirement against BridgeManager) has no native_sim test
  (compile-checked only); an on-air Matter check (fill 16, replace a sensor,
  see the oldest orphan go) is an owner step. A sensor that is merely slow to
  come back after a reboot can lose its endpoint to a new row once the bridge
  is full. An upgrade re-identifies every measured endpoint (env temperature
  and page-84 temperature/humidity/pressure): their old records stay as
  unreachable orphans until the bridge fills. Owner call whether to add a
  legacy-ID migration.
- [x] **Matter: `(source, field_id)` is not unique for Environment sensors.**
  The env temperature is field 0 (`radiant_env_adapter.h`), and the env event
  count drives the WORN rule, which posts field 0 as OCCUPANCY
  (`radiant_rules.c` ~:233; `RADIANT_RULE_FIELD_WORN` is 0).
  `radiant_matter_endpoint_for()` keys on source and field id only, so WORN is
  written to the temperature endpoint and refused, or, if WORN arrives first,
  the endpoint becomes an occupancy sensor and temperature never appears. The
  comment in `mqtt_sink.c` (~:138) says the ids cannot collide. Fix: key
  endpoints and the deadband on the field type as well, or exclude device type
  0x19 from the WORN rule. Add a test that combines the env adapter, the rules
  and the Matter sink.
  *Done 2026-11-24 (wip/s24-matter-ident):* both. `radiant_matter.c` keys
  endpoints and the deadband on (source, field_id, field_type)
  (`radiant_matter_endpoint_for()` takes the type; same symbol), and
  `struct radiant_matter_endpoint` records `derived`. `eval_activity()` skips
  the WORN slot for a source bound with device type 0x19 (pairing bit masked);
  unbound sources keep it. The Matter UniqueID follows the spec's "unique per
  bridged device, persistent, at most 32 characters": derived rows keep
  `%08x%08x%02x` byte for byte, measured rows add `t%02x` (field type), e.g.
  `3f2a91c40e77b21200t10` (21 characters), in `bridge_storage_codec`'s
  `UniqueId::Format()`. So every measured endpoint stored by an older build
  (env temperature, page-84 temperature/humidity/pressure) is re-identified
  once and its old record becomes an orphan (see the item above). Comments in
  `mqtt_sink.c` and `docs/radiant-bridge.md` §9.1 corrected. Tests:
  `test_matter.c` (both arrival orders, independent deadbands, `derived`, and
  the `radiant_matter_combo` suite: env adapter → rules → Matter gives one
  temperature endpoint and no WORN for a bound 0x19 source, while a bound HR
  strap still gets WORN), `test_rules.c` (WORN skipped for bound 0x19 and
  0x99, kept unbound and for 0x78), `matter_storage` (formatter). Red on
  master first. Residual: on-air Matter check is an owner step.
- [x] **Bridge pairing window binds the first device heard, and a transmitter
  that keeps sending cannot be evicted.** `try_bind()` (`self_channels.c`
  ~:758) has no RSSI or proximity threshold, and `radiant_self_pick_evict()`
  skips tracking channels. Recovery is a power cycle, and on the Matter image
  the endpoints persist. Fix: a proximity threshold for discovery, and a
  deliberate "evict even if tracking" gesture such as a long press.
  *Done in code 2026-11-24 (wip/s24-bridge-prox); BENCH PENDING (plan B4).*
  radiant now enforces `0x71` proximity search at the ACQUIRE claim
  (`radiant_search_id_filter.min_rssi_dbm`, `stats.prox_rejected`; no RSSI
  fails closed; SCAN and tracking ignore it), one-shot as in ANT (cleared on
  acquisition, kept on a search timeout). Bins 1..10, 1 closest, at Nordic's
  approximate nRF51 levels -44..-80 dBm in 4 dB steps; 11+ refused; the
  C-API custom form `0x80` + magnitude is `[inferred]`, reachable through
  the optional third byte of `0x71` (kept, as since build 1). The capability bit
  and the setter follow `caps.has_rssi`. The RSSI field's byte 2 reports the
  threshold in force. Self channels: every discovery open sets
  `CONFIG_ANT_DONGLE_PAIRING_PROX_BIN` (provisional default 3, about -52 dBm)
  or `_PROX_DBM`; a pinned open sets 0. A long press
  (`CONFIG_ANT_DONGLE_PAIRING_LONG_PRESS_MS`, 3000) evicts the most recently
  bound owned sensor even while it tracks (`radiant_self_pick_evict_forced()`),
  keeps it off the exclusion list and out of `try_bind()` until the window
  closes, and restarts the window; repeated presses walk back. The treadmill
  HR floor read the bins backwards (10 nearest) and now relaxes 3 -> 10 -> off.
  Tests: test_self_policy, test_channel, test_search, test_event, test_api,
  test_bridge_parser, treadmill hr_rx. Twister (cassandra) 9589/9589 on
  d3f39f7. Host 0x71 on air PASS 2026-11-25
  (`archive/benchmarks/2026-11-25-prox-search/`): 0/140 too-weak acquired,
  140/140 strong acquired, one-shot and timeout-keeps 6/6, cap bit set, and a
  far real HR strap never taken under bin 3 (30/30 went to the near one).
  Default bin 3 kept; distance calibration was waived by the owner. Not on air:
  the bridge's long press (needs a hand on the button) and fail-closed with no
  RSSI (the TI always reports it).
- [x] **Owner decision: the RSSI field's byte 2 with proximity search off.**
  radiant sent `0x00`; AN16 RSSI Extended Information section 4.3.3 (and ANT
  MP Rev 5.1 section 7.1.1.2.3) give -128 (`0x80`) for "off".
  *Done 2026-11-25:* the owner ruled to follow the ANT documents unless there
  is a clearly better way, and there isn't one: 0x00 reads as a 0 dBm
  threshold. `RSSI_THRESHOLD_CFG_NONE` is now 0x80 (`radiant_event.c`), and
  test_event asserts it. No host in the repo reads the byte. A Garmin ANTUSB-m
  can't confirm it: its RSSI field is measurement type 0x10 in 4 bytes, not
  the AN16 type 0x20 in 3, so it has no threshold byte
  (`archive/benchmarks/2026-11-25-garmin-rssi-byte2/`). Confirming on the
  wire would need a receiver that reports type 0x20.
- [x] **Zwift shim installer cannot replace an older shim, and the installed
  one predates key masking.** `scripts/zwift_ant_shim.ps1` (~:143) treats "same
  file length" as "ours" and refuses anything else, and `-Uninstall` (~:81)
  deletes by name with no check. On this machine the DLL in the Zwift directory
  is byte-identical to `tools/zwift_shim/out/` from 2026-11-15, which logs the
  first byte of every write and has none of the 2026-11-20 masking; the source
  is newer than both. `test_shim_log.py` loads `out/`, so it tests the stale
  build until a rebuild. Fix: put a version marker in both DLLs (VERSIONINFO or
  an exported symbol), identify "ours" by it for install and uninstall, then
  rebuild and reinstall. Do this before the "Zwift shim key redaction end to
  end" owner step.
  *Done 2026-11-24 (wip/s24-zwift-shim):* both DLLs carry a VERSIONINFO
  resource (`shim_version.rc`/`.h`, ProductName "RadiANT Zwift ANT shim",
  2026.11.24.1, also in the log banner); the installer calls a file ours by
  that resource, or by the exact SHA-256 of the installed 903718a pair, for
  install (both checked before either is copied) and `-Uninstall` (foreign
  files kept and reported). `test_shim_log.py` refuses to run when a source
  is newer than `out/` or a DLL lacks the resource; `RADIANT_SHIM_OUT` points
  it elsewhere. Rebuilt and reinstalled 2026-11-24 (2026.11.24.2 in the
  Zwift ride logs).
- [x] **Zwift shim: the level-2 read dump is not masked.** `SI_Read`
  (`dsi_siusbxp.c` ~:318) logs raw hex, so a device that echoes (a console on
  another CP210x, which is what the unbound-port item allows) puts a
  key-setting write into the log. Also check 0x76 (128-bit network key) and
  0x83 (store encryption key) against the ANT message spec: neither
  `key_keep()` nor the firmware's `key_msg_keep()` lists them. Fix: run the
  frame walk and mask over reads as well; add a read-side case to
  `test_shim_log.py`.
  *Done 2026-11-24 (wip/s24-zwift-shim):* ANT_DLL reads ONE byte per
  `SI_Read` (60432 of them on 11-23), so reads are reassembled per reading
  thread and the read that completes a frame logs it, masked like a write;
  a frame that fails its checksum drops its whole claimed length unseen, so
  key bytes cannot be re-read as a lucky frame. Rev 5.1 sec 9.5.2.25 (0x76,
  [net, key 16]) and 9.5.2.36 (0x83, [op, NVM index, key 16 on a store]) carry
  keys: added to `key_keep()`, firmware `key_msg_keep()` and
  `protocol/ant_wire.yaml` (unbridged; excluded from the conformance run).
  Tests: `ShimReadDumpTest`, `bridge_trace{,_sec}`.
- [x] **Zwift shim installer runs `python.exe` from a directory other local
  users can write.** `zwift_ant_shim.ps1` (~:168) hard-codes
  `C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe`; `C:\ncs` grants
  Authenticated Users modify here. Fix: take the interpreter from a parameter
  or `$env:RADIANT_PYTHON`, or print the verify command instead of running it.
  *Done 2026-11-24 (wip/s24-zwift-shim):* `-Python` (default
  `$env:RADIANT_PYTHON`), run with `-I`; with neither the verify command is
  printed. No PATH lookup either.
- [x] **Zwift shim: the global port handle is unsynchronised.** `g_port`
  (`dsi_siusbxp.c`) is read and written by every export with no lock, and
  ANT_DLL reads and writes on different threads. A close between the test and
  `WriteFile` can send a write to a reused handle; a second `SI_Open` that
  fails overwrites the first caller's valid handle and leaks it. Fix: one
  SRWLOCK (shared for I/O, exclusive for open and close), and open into a
  local before publishing.
  *Done 2026-11-24 (wip/s24-zwift-shim):* `g_port_lock`: every export that
  uses the handle holds it shared across the whole call, I/O included
  (shared holders never block each other, so a pending read still does not
  hold up a write); open configures a local handle and publishes it under
  the exclusive lock; close cancels pending I/O first, then unpublishes
  under the exclusive lock and closes; while it runs a `g_closing` flag
  makes `port_get()` refuse new I/O and the cancel repeats until the
  exclusive lock is taken, so a read thread that re-enters at once cannot
  make the close or the writes wait out a read. DllMain no longer resolves the port
  and closes on FreeLibrary only via a try-lock. Red on the 903718a build:
  8 racing `SI_Open`s on a one-instance pipe clobbered the winner's handle
  from round 1 (`ShimLockTest`).
- [x] **The checksum-error log line bypasses the key redaction rule.**
  `ant_serial_bridge.c` (~:1912) logs `got` and `exp` for every failed frame
  in any build with logging. For a key message `exp` is the XOR of the key
  bytes with the known header, which the trace code withholds for that reason.
  Fix: log the message id only, or nothing when `key_msg_keep()` says the
  frame may carry a key. Add the case to `test_bridge_trace_sec.c`.
  *Done 2026-11-24 (wip/s24-zwift-shim):* the line is now `Checksum err: id
  0x%02X len %u`, never a checksum, for every frame (a frame that lost its
  id byte names no key, so the id test alone could not decide). Routed
  through `ANT_BRIDGE_CSUM_WRN` so `bridge_trace{,_sec}` capture it.
- [ ] **Release: any job in the tag run can replace a `release-*` artifact**
  (widens the known residual under Owner steps). Artifacts are matched by name
  and checked only against the sidecar inside the same artifact, and 35 of the
  40 build jobs do not ship (the Matter and TI rows have the largest
  third-party surface). Fix: each shipping row exports its sha256 or
  `artifact-id` as a job output, `publish` compares against
  `needs.<job>.outputs`, and the five shipping rows move into a job of their
  own.
- [ ] **Release: a published release can be replaced, and nothing ties a tag to
  master.** `softprops/action-gh-release` is used without
  `overwrite_files: false` (its default overwrites), `publish` has no
  `environment:`, and the only gate is `version_int.py --check-tag`. A re-run
  of an old tag run rebuilds with whatever the unpinned inputs resolve to and
  replaces assets and sums under the same version. Fix: immutable releases,
  `overwrite_files: false`, a tag ruleset for `v*`, a protected `release`
  environment, and `git merge-base --is-ancestor "$GITHUB_SHA" origin/master`
  in `publish`. Repository rulesets were not checked.
  *2026-11-24:* the workflow half is done: `overwrite_files: false`,
  `environment: release`, and a "Tagged commit is on master" step
  (checkout now `fetch-depth: 0` so `origin/master` is there). Not run yet:
  only a tag exercises `publish`. The settings half (environment protection,
  immutable releases, `v*` ruleset) is an owner step above.
- [x] **`SECURITY.md` integrity claims do not match what exists.** Line 45 says
  the build is reproducible with the toolchain the workflow pins: nothing is
  pinned (above), no job rebuilds and compares, and the DFU zip embeds
  wall-clock entry times. "Every release artifact has a `.sha256` sidecar": the
  reviewer's read of the public releases found v1.0 and v1.1 with hand-uploaded
  assets and no sidecars, and v1.2.0 with no assets (not re-checked). Fix:
  state the claim for the UF2 or inner `zephyr.bin` only after one
  rebuild-and-compare has passed, and say which releases predate sidecars.
  *Done 2026-11-24:* the reproducibility claim was already corrected (902dbf6:
  "not shown to be reproducible", pinned inputs listed). Now the sidecar claim
  is limited to workflow-published releases, and SECURITY.md says v1.0 and
  v1.1 were hand-uploaded with no sidecars and v1.2.0 has no assets
  (re-checked against the GitHub API today). It also documents the attestation
  check.

## Hardening

- [x] **Tracked channels do not check the transmission type of a clean frame.**
  `api_tracked_frame()` (`radiant_ant.c` ~:2455) compares body byte 0
  only for CRC-repaired frames, so a frame with another transmission type
  re-anchors the slot, is ACKed, and goes to the host under the channel's ID.
  ANT+ uses the upper nibble as device-number bits 16-19, so two sensors can
  merge in a dense venue. The behaviour is deliberate (`test_api.c` ~:4526).
  Fix: drop a mismatching frame before `radiant_channel_on_slot()` when the
  configured or acquired type is non-zero; at minimum post the frame's own ID.
  *Assessment 2026-11-24 (before the fix):* the clean-frame exemption came in
  with CRC repair (0fbbba4, 2026-08-10). Its comment gave two reasons: "a
  shared channel, or a sensor whose transmission type this host guessed".
  Neither holds, and no path was found where a tracking channel with a
  non-zero type legitimately hears another byte 0:
  - A guessed type never acquires. Search filters the type at acquisition
    (`radiant_search.c` `id_match()`). A channel that loses its sensor
    searches again with `radiant_channel_id_get()`, which gives the acquired
    ID (`api_search_begin()`). A real ANT receiver matches the whole channel
    ID, so it never hears such a frame either.
  - Shared channels: bits 0-1 of the type only say how long the shared
    address is, and the address itself is in payload bytes 1-2. Master and
    slaves all transmit the one channel ID.
  - Master channels hearing replies: `radiant_channel_id_get()` gives the
    configured ID. A slave replies with the channel ID it acquired: ours
    encodes `id.trans_type` (`radiant_frame.c` encode, asserted by
    `fec_exchange()`), and so does an ANT slave.
  - Profile handoff (`radiant_profile_handoff.c`) takes its type from a
    TRACKING channel's ID, so the receiver applies the type the master
    actually sends.
  - Bridge self channels discover with a wildcard and re-pin with the
    binding's type, which came from the acquired ID.
  - Keyed channels: XOR and CMAC cover the payload only. Byte 0 stays the
    plain type (`test_api_sec.c` "the header moved").
  - Zwift's FE-C slave names the device and wildcards the type. It acquires
    at the sensor's type, and every later frame from that sensor carries it.
  A wildcard (0) still checks nothing. The configured ID is never narrowed,
  so close and reopen searches wide again.
  *Done 2026-11-24 (wip/s24-ttype):* the check now covers clean frames too.
  A mismatching clean frame is dropped before `radiant_channel_on_slot()`
  and counted in the new `api_stats.rx_ttype_mismatch` (not in 0xF6, and
  0xF7 page 10 is full). A repaired one still counts `crc_repair_refuted`.
  `api_sched_rx()` still marks `slot_heard` before the frame is processed,
  and puts back the window's previous value when `api_tracked_frame()`
  reports the frame was not ours. So a channel whose sensor left while a
  neighbour stayed keeps missing, and returns to search after eight misses.
  Nine api cases, in every api scenario: clean broadcast and acknowledged
  data of type 6 on a channel acquired at 5 (long and short match), back to
  search on foreign frames only, and a named device with a configured type
  of 0 or 5 (long and short).
  Residuals: (1) a frame that reaches a non-idle transfer
  (`api_sched_rx()`'s non-idle branch, and burst continuations in
  WAIT_NEXT) goes to the transfer engine without a type check. It still
  marks the slot heard. A foreign packet in the middle of our own burst
  could be taken as ours. WAIT_NEXT's broadcast fallback does get the check.
  (2) The search seen-table and acquisition paths were not changed. They
  already filter the type. (3) Two channels on ONE dongle tracking two
  sensors that differ only in transmission type have byte-identical RX
  filters; a merged window routes by filter index, so the leader now drops
  the other's frames and the other channel misses those periods (it used to
  take them wrongly; not worse, not fixed).
  On-air PASS 2026-11-24 (`archive/benchmarks/2026-11-24-ttype-onair/`): TI
  wildcard HR receiver, Pro Micro HR #12345 type 5 as the genuine sensor, L15
  `ant_sim` HR #12345 type 0x15 at -600 ppm as the impostor, 480 s each. Ship
  430562b posted 8 impostor frames (2 s of following it); ship d94d07e posted
  0 of 2049. A +3000 ppm drift jumps the ~150 us guard and shows nothing.
  Zwift on d94d07e (`captures/zwift-20261124-164925.pcap`): FE-C, named with a
  wildcard type, tracked #52233 type 5 for the whole capture with no
  GO_TO_SEARCH. Its 42 % loss came from the link: the Kicker was heard about
  5 dB weaker than on 11-23 (-74 against -69 dBm) on the scan channel as well,
  and its other device types never pass the address match.
- [x] **Acquisition and re-anchor accept any control byte.** `handle_ok()` and
  `api_search_acquired()` anchor on slave replies, ACKs and non-opener burst
  packets. A channel entering search during someone's exchange can acquire off
  the slot grid and lose about 2 s, and the acquiring ACK's echoed payload
  reaches the host as BROADCAST_DATA. Fix: require
  `radiant_ctrl_is_slot_opener()` for an ACQUIRE claim and for
  `radiant_channel_on_slot()` on a slave. The tracked path setting `slot_heard`
  on a reply or ACK is correct and stays: a reply or ACK in the window proves
  the master transmitted that period.
  *Done 2026-11-24 (wip/s24-opener-gate):* `handle_ok()` skips an ACQUIRE
  channel on a non-opener before the claim and the permit call
  (`stats.non_opener_skipped`), which covers every backend and the seen-cache
  acquires. SCAN still gets every frame and the seen cache still notes it.
  `api_tracked_frame()` re-anchors and notes RF only on an opener; on a
  slave's non-opener it steps the clock one period with
  `radiant_channel_catch_up()` (guard charged, phase kept), because a heard
  window does not advance the clock by itself and the next window would land
  on the slot already gone and count a miss. A 0x02 heard by the search no
  longer reacquires the FE-C channel (`fec_reacquire_does_not_send` now
  expects SEARCHING for a non-opener). Residual,
  not fixed: a repeated 0x8A/0xAA (packet-0 retry, +3.7 ms) still carries the
  opener bit and cannot be told apart from the frame alone; it needs a master
  retry inside our search dwell, so it is rare. Tests: `test_search.c`
  (opener gate cases), `test_api.c` (tracked re-anchor, search on an ACK).
  On-air A/B/A PASS 2026-11-24 (`archive/benchmarks/2026-11-24-opener-gate/`,
  TI DUT, L15 acked master, Garmin ACKer, 200 reopens per arm): old image
  13 and 13 acquisitions on the ACK (off-grid, ACK payload posted as the
  first BROADCAST_DATA), new image 0. The Garmin's ACK carries its own
  buffer, not an echo of the acked data.
- [ ] **UART transport can emit a truncated frame.** `ant_uart_write()`
  (`ant_uart_transport.c` ~:311) queues the head of a frame, and on a 1 s
  stall returns with it queued, so the next frame follows a fragment and a
  host parser may resynchronise on a `0xA4` inside air payload. Needs a wedged
  UART today (no overlay sets flow control). Fix: all-or-nothing writes.
- [x] **0xF1, 0xF2, 0xF3 and 0x46 accept over-long bodies.** `dispatch()`
  checks minimum lengths only; `ant_wire.yaml` declares exact ones and 0xF5
  already enforces them. Fix: `len == N`.
  *Done 2026-11-24 (wip/s24-search-races):* `body_len_ok()` in
  `ant_serial_bridge.c` gates `dispatch()` (INVALID_MESSAGE, no arm runs);
  it sits outside `ANT_BRIDGE_PARSER_ONLY` so `test_bridge_parser.c` tests it.
  ant_wire.yaml's INVALID_MESSAGE text says so. The host tools already send
  exact lengths. L15 direct diag A/B/A: a 10-byte 0x46 answers NO_ERROR on
  wd_dir_0fce666 and INVALID_MESSAGE on the fix. Security text 7828 -> 7788 B
  (release.yml recipe, Feather core, base 92239e9 measured in the same
  sitting; -40 B, not traced), bss delta unchanged.
- [x] **The channel type byte is stored unvalidated and compared two ways.**
  Some paths mask it and others compare raw (`radiant_ant.c` ~:1135,
  :1984, :2058, :4470, :4550), so type 0x41 reads back as RX-only and refuses
  broadcasts yet acknowledges and accepts acked data and bursts. Fix:
  normalise once at assign.
  *Done 2026-11-24 (wip/s24-search-races):* masked, not refused:
  `radiant_channel_assign()` stores `type & 0xF0`. Refusing is out of
  contract (assign's permitted error set has no code for a bad type,
  `radiant_channel.h`), and the status byte never carried the low nibble, so
  the mask is invisible on the wire. Bits 7:4 are kept as given (0x70 is
  still accepted, as `test_an_unrecognised_channel_type_is_accepted` pins).
  0x41 now never transmits, 0x51 never listens (api tests). L15 A/B/A: type
  0x41 status 0x42 on both, acknowledged data accepted on wd_dir_0fce666 and
  refused (0x15) on the fix; scan and 8-channel session unchanged.
- [x] **A failed re-import during 0xF2 on a secured master would fail open**
  (latent; needs an opaque key backend). After the old root is destroyed, a
  failing import releases the context (`radiant_sec.c` ~:621-640), and an
  unkeyed channel transmits untouched (~:1209), so secured-range pages would
  go out in clear after a 0xF2 that only returned an error. The compat twin
  above is fail-closed; this one is not. Fix: import into a temporary before
  destroying the old root.
  *Done 2026-11-24 (wip/s24-sec-host-api):* `radiant_sec_set_key()` imports into a stack
  temporary, then takes back, destroys the old root and moves the handle in
  (wiped, not destroyed); a failure leaves the old root, epoch and windows. Test
  `test_a_failed_rekey_import_keeps_the_old_root` via a CONFIG_ZTEST-only
  import-failure seam in `radiant_sec_aes.c`. The compat twin is unchanged.
- [ ] **Lock assertions miss most users of the shared AES schedule.**
  `RADIANT_SEC_ASSERT_LOCKED` is absent from `radiant_sec_compat.c` and from
  the primitives that `radiant_profile_command.c`, `radiant_profile_telemetry.c`,
  `radiant_profile_enrol.c` and `radiant_node_ident.c` call, some of which key the schedule
  with the public all-zero key. Fix: assert in `select_key()`
  (`radiant_sec_aes.c` ~:161).
- [ ] **`epoch_search()` has no rate limit** (library). One forged beacon or
  Tier I page can cost 65,537 CMACs or KDF+CMACs synchronously
  (`radiant_sec_compat.c` ~:1590). The first caller must rate-limit it or tie
  it to a user action; say so in 11.3. Likewise cap refused small-order peer
  keys per enrolment window (`radiant_sec_pair.c` ~:455; each costs an X25519
  under the port lock), and throttle failures in
  `radiant_profile_private_on_command()` as `radiant_profile_command` does.
- [ ] **Bench Thread and Matter credentials compile in with no guard.**
  `thread.conf` sets the well-known network key with
  `OPENTHREAD_MANUAL_START=n`, and `matter.conf` sets `CHIP_FACTORY_DATA_NONE`
  (test DAC, default passcode). These images are built but not published. Fix:
  a `CONFIG_ANT_DONGLE_BENCH_CREDENTIALS` symbol the fragments set, a boot
  warning, and a release check that refuses an artifact whose `.config` has
  either.
- [ ] **ADR 0011 says the leaf role is enforced with `BUILD_ASSERT`; no such
  assert exists.** The role rests on `CONFIG_OPENTHREAD_MTD=y` alone. Add
  `BUILD_ASSERT(!IS_ENABLED(CONFIG_OPENTHREAD_FTD))` and a `.config` check in
  the Thread, MQTT and Matter CI rows.
- [ ] **State the bridge's adversary in the docs.** Neither
  `docs/radiant-security.md` 6 nor `docs/radiant-bridge.md` 13 says that the
  Matter occupancy, temperature, pressure and humidity endpoints and the MQTT
  values are driven by unauthenticated ANT+, and that a pinned channel
  acquires a spoofer by itself when the real sensor is off. Add a paragraph:
  do not key security-relevant automations on these entities.
- [ ] **MQTT backoff resets on every CONNACK.** `clear_backoff()`
  (`mqtt_sink.c` ~:675) runs before the session has done anything, so a peer
  that accepts and then stops reading costs the drain thread a 3 s blocked
  send every few seconds. Fix: clear only after a minimum session time or N
  successful publishes.
- [x] **Zwift shim build and inputs.** Add `/guard:cf /sdl /W4` and a version
  resource (the marker above). Validate the port name from the registry and
  `RADIANT_ANT_COM` as `COM[0-9]{1,3}` (`shim_util.c` ~:189, :208); a 60+
  character name aborts Zwift through `sprintf_s`. Clamp a zero read timeout
  in `SI_SetTimeouts`. State in `SECURITY.md` that the shim is always built
  locally and is not a release artifact.
  *Done 2026-11-24 (wip/s24-zwift-shim):* `/W4 /WX /O2 /sdl /guard:cf`,
  linked `/guard:cf /DYNAMICBASE /NXCOMPAT` (dumpbin: CF instrumented), no
  `_CRT_SECURE_NO_WARNINGS`; port names `COM[0-9]{1,3}` from every source,
  invalid values not echoed; `SI_SetTimeouts` clamps the read timeout to
  1..60000 ms (0 used to mean forever); `SECURITY.md` "The Zwift shim" section. A `SHIM_TEST_PIPES` build in
  `out/test/` (never installed) keeps the named-pipe tests.
- [ ] **udev rule.** The driver-unbind line in `host/linux/70-radiant-ant.rules`
  matches vendor 0fcf with no product filter, wider than the three access
  lines; add the product ids and `ENV{ID_MM_DEVICE_IGNORE}="1"`.
- [ ] **Release provenance.** Add `actions/attest-build-provenance` in
  `publish` only, which covers the "attacker who can write to the release"
  case `SECURITY.md` lists as unaddressed, and a `github-actions` dependabot
  entry so the SHA pins move. `linkcheck.yml` downloads the lychee binary with
  no checksum in a weekly default-branch job.
  *2026-11-24:* attestation (v4.2.2, SHA-pinned, before the attach step) and
  `.github/dependabot.yml` (github-actions, monthly, grouped) added. Open:
  the lychee binary checksum.
