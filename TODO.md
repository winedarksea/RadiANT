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
  on the bench against the redacting shim (sec review 2026-11-20).
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
- [ ] **A device-number change keeps queued ciphertext** (latent; reviewer,
  2026-11-22). `radiant_sec_try_devnum()` (`radiant_sec.c`) changes the device
  number, which is part of the nonce, and resets the verify state, but does not
  purge the receive queue. A page queued under the old number would be
  decrypted with the new one and delivered as garbage. Nothing in production
  calls it yet. Fix: `q_purge(ch)` there, with a red test like the 2026-11-21
  epoch/policy purge tests.
- [ ] **`rx_process()` returns silently when `advance_epoch()` fails** (headroom
  or ceiling; low; sec review 2026-11-17 item 3). Nothing is counted.
- [ ] **Host Tier I announcement check** (low, host tool; sec review 2026-11-17
  item 6). The host checks an announcement at the last verified Tier I counter,
  but the firmware tags at the clock's interval index. An announcement whose
  Tier I page was lost, or sent before it, fails on the host.
- [ ] **Recovering a node whose K_dev record is corrupt** needs a partition erase
  or a separate delete API, because provisioning refuses by design (low; sec
  review 2026-11-17 item 7).
- [ ] **Escape binding labels in MQTT discovery JSON before any path sets one**
  (deferred 2026-11-20: not reachable today, since the only production bind
  passes a NULL label). Whoever adds editable labels must first add a bounded
  JSON string encoder in `compose_discovery()` (`mqtt_sink.c`) and a round-trip
  test through a JSON parser.
- [ ] **Bind the Zwift shim to the intended serial device** (deferred; sec review
  2026-11-21). With no explicit port, the shim takes the first CP210x registry
  entry and falls back to COM15. It then forwards host writes there, key-setting
  messages included. Remove the COM15 fallback, remember a user-selected device
  instance or serial number, and report no device when the selection is absent
  or ambiguous (`tools/zwift_shim/shim_util.c`, `dsi_siusbxp.c`).

# Matter bridge

- [ ] **"Absent vs unreadable" high-water mark is only partly closed**
  (reviewer, 2026-11-22). Zephyr's `settings_load_subtree_direct()` drops
  backend read errors, so a fault that hides the stored `br/brd_ep_hw` entry
  still reads as Absent, and the max(restored)+1 fallback can reuse an
  endpoint id. Find a way to tell the two apart (a backend that reports
  iteration errors, or a second marker key). Until then, correct the comment
  in `bridge_storage_manager.cpp` (`LoadEndpointHighWater()`, ~:414-417), which
  claims `HasEntry()` separates a missing key from a read error; it cannot
  when the fault hides the key.

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
- [ ] **Public headers for profiles and the bridge under `include/radiant/`.**
  Since 2026-11-22 every exported name is `radiant_*` and the files are
  `radiant_profile_*.h`, `radiant_node_*.h` and `radiant_bridge*.h`, but they
  are still reached through `src/profiles`, `src/node` and `src/bridge` on the
  include path (`#include "radiant_profile_hr.h"`), not as
  `<radiant/...>`. Decide whether they become `<radiant/...>` headers.
- [ ] **`radiant/src/radiant_ti_rf_hwattrs.c:231` reads the app's
  `CONFIG_ANT_DONGLE_TRANSPORT_*`** to refuse
  `CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE` in a dongle image. That is module
  code testing application Kconfig, the same seam problem the health counters
  had until 2026-11-22. Replace it with a module symbol the dongle selects.
- [ ] **Delete `scripts/migrate_radiant_names.py` after the first release that
  ships `radiant_ant_*`** (the one after v1.2.0). It exists for code that
  tracked master before the 2026-11-22 rename. Also remove its CHANGELOG
  pointer from the next release's notes.

# Reviews to run

- [ ] Firmware size: smaller code without loss of functionality.
- [ ] Performance: minimal packet loss, lowest latency.
- [ ] Security, with a broad remit: look for fundamental changes in code shape
  or policy.
