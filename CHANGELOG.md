# Changelog

Notable changes only, newest first. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

**This file has a length budget, like `README.md` does.** If it grows past
roughly 200 lines, drop the oldest entries rather than letting it become a
second copy of `git log` — a changelog nobody finishes reading is one nobody
reads.

## [Unreleased]

### Added

- **Two tests for a defect the suite could not see: a channel dropping to
  search starving one still tracking.** Measured in
  `captures/zwift-20260818-160751.pcap` — a power meter on channel 1 held 10.6%
  loss while a heart-rate strap tracked on channel 2, and 44.4% (perfect
  `OK FAIL OK FAIL` alternation) from the instant channel 2 went to search,
  though the meter was on air at full signal throughout. The step follows
  channel 2's change of STATE, not its going quiet 2.3 s earlier, so it is not
  RF. The nearest existing test,
  `test_the_scan_keeps_finding_devices_while_another_channel_tracks`, guards
  the *sweep* against starvation and asserts only `count_bcast_on(1u) > 0u`
  about the tracked channel — a channel losing half its packets passes it.
  `radiant_sched.test_a_tracked_channel_keeps_every_slot_under_a_search` and
  `api.test_a_channel_that_drops_to_search_does_not_starve_one_still_tracking`
  now assert the loss rate. **Both pass**, and are proven sensitive rather than
  vacuous (the second reports 20 of 20 slots served, 0 RX_FAIL), including with
  the scan's `chunk_us` at the 260 ms dwell that exceeds the 249.7 ms slot
  period. So the arbitration logic is correct and the fault is in what
  `fake_radio` replaces — arming latency, the SEARCH→TRACKING reconfiguration,
  or timeslot arbitration in `radiant_radio_nrf.c`. The tests stay as the
  regression guard for whatever the fix turns out to be.
- **`tools/ant_search_contention.py`, and the correction it forced.** The entry
  above used to end "a background scan does **not** cause this: 8.3% against
  8.0%". That was one weak measurement and it is wrong. A third Zwift capture,
  `captures/zwift-20260818-225148.pcap`, has the step twice: 49.5% loss on the
  tracked power meter while Zwift's own ch0 wildcard discovery channel cycled,
  10.8% for the same channel with two sensors tracking and nothing searching,
  and 49.2% (perfect alternation) after the heart-rate strap was switched off
  and its channel fell back to search. The boundary is exact — ch0's search
  closes at t=143.840 and the meter returns seven consecutive good slots from
  144.044. It is mutual, not a priority inversion: ch0's 5 s search windows
  yielded 4, 3 and 10 broadcasts before the tracked channel opened and 0, 0, 2,
  3, 0 after. The new tool measures it as an A/B/A with no Zwift in the loop and
  reproduces it first try — 36.6% / **63.0%** / 35.5%, controls agreeing to 1.1
  points, with the tracked channel raising `RX_FAIL_GO_TO_SEARCH` during the
  arm. It reports an alternation percentage beside the loss because losing
  exactly every other slot is the one signature interference cannot fake.
  Still not reproduced in ztest, so the search for the fault stays in
  `radiant_radio_nrf.c`.

### Fixed

- **`scripts/cap_zwift.ps1` and `tools/decode_pcap.py` had no SPDX header**, so
  the `host-tests` CI job's `check_license.py` was failing on both from the day
  they were added.

- **Two API tests asserted a fact about test ORDER.** `api_stats` is zeroed in
  `antr_init()`, which runs once per suite, and not in `antr_stack_reset()`,
  which runs between tests — so `zassert_equal(0u, ...->slots_missed)` in
  `test_a_constant_transmit_offset_moves_the_period_by_exactly_that` and
  `test_a_denied_master_transmit_does_not_wedge` really asserted "no
  earlier-ordered test has ever missed a slot". Adding any test that misses
  slots broke both. Both now baseline the counter at test start.

- **An assign to a keyless network stopped Zwift discovering anything after the
  first sensor.** `antr_channel_assign()` refused an in-range network with no
  address installed (`ANTW_INVALID_NETWORK_NUMBER`), which was added to turn a
  silent hole into a diagnosable error and instead turned a survivable
  degradation into a dead session. Zwift installs three network keys, only the
  ANT+ one is accepted, and it then cycles its wildcard scan channel across
  networks 0/1/2 — so the refusal hit every few seconds. Measured against real
  Zwift: assign refused at t=26.05 s, then `CLOSE_CHANNEL` **5385 times** over
  the remaining 127 s with no further `ASSIGN`/`OPEN`, and exactly one ANT+
  device discovered for the whole session. Zwift has no recovery path for a
  failed assign. It is now warned about and accepted; the sweep matches on
  network 0's address regardless, so a background scan on a keyless network
  works normally. `ANTW_INVALID_NETWORK_NUMBER` goes back to meaning what
  `docs/ant-serial-protocol.md` says it means — a number above the advertised
  maximum — which `radiant_channel_assign()` still enforces.
- **Capabilities no longer advertise a readable serial number the dongle cannot
  produce.** `ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED` was set while
  `MESG_GET_SERIAL_NUM` (`0x61`) is unbridged and answers `INVALID_MESSAGE` —
  the advertised-and-unimplemented trap `docs/gotchas.md` names, and a real host
  does ask: Zwift requests `0x61` during startup. The `sdk_ant` backend this one
  is A/B'd against never claimed the bit (advanced-options `0xb2` vs radiant's
  `0xba`). Dropped from the `radiant` and `stub` backends. **Two conformance
  transcripts need regenerating on hardware**, both carrying the same frame:
  `conformance-nrf-radiant.antser` and `conformance-cc26xx.antser` (the TI
  dongle runs the same radiant backend). The capabilities reply changes
  `a40954200300ba2600d5000192` → `a40954200300b22600d500019a` — byte 3
  `0xba` → `0xb2`, checksum `0x92` → `0x9a`. `conformance-sdk-ant.antser` is
  unaffected; it never set the bit. Until they are regenerated, `ab_gates`
  will flag both as a diff, and that diff is expected.
- **`tools/decode_pcap.py` spliced together the bulk streams of different USB
  devices.** Reassembly buffers were keyed by direction alone, so a capture
  holding more than one device (any `-All` capture) concatenated unrelated bytes
  into one stream and the reframer resynchronised through the wreckage, emitting
  messages nobody sent. Now keyed by `(device, direction)`, with a `--device`
  filter to restrict decoding to one address.

- **Two tracked channels landing within one arm lead of each other lost the
  later one, every period.** `min_arm_lead_us` is the scheduler's exclusion
  radius — `pass_step()` reports a window `DONE_MISSED` once
  `t_end < now + arm_lead` — but `arm_rx_window()`'s merge rule 3 required
  literal overlap and so reached only twice the 100 µs tracked guard. Between
  the two was a band in which a pair could neither merge nor be armed in
  sequence. The reach is now `arm_lead()` itself, mirrored in
  `could_join_armed()`. Invisible at one channel, which is why every loss figure
  this project has recorded missed it; worth roughly 1 % of slots per channel at
  eight tracked sensors. `max_addr_groups == 2` on nRF means this rescues
  **pairs** — a three-way pile-up inside one lead still drops the third. See
  `docs/decisions/0016-merge-reach-is-the-arm-lead.md`.
- **A tracked window that never armed no longer waits for the 50 ms housekeeping
  pump.** `api_sched_done()`'s TRACK_RX miss/deny branch sets `track_repost`, so
  the next period's window is posted immediately instead of arriving after the
  radio has committed ~250 ms out. Eight of those in a row was
  `RX_FAIL_GO_TO_SEARCH` — a ~2 s dropout that looked like RF and was not.
- **`want_preempt()` lets an armed bounded receive that has not opened yet give
  way to a pending bounded receive that genuinely starts earlier.** Tearing down
  before the first bit of preamble costs nothing, and without it the re-posted
  window above still lost to `arm_next()`'s deliberate early commitment.

- **The nRF backend could not be built without the MPSL gate.**
  `radiant_nrf_gate_on_grant()` is defined unconditionally, but the three
  statics it writes (`grant_short_rx`, `grant_scored`, `GRANT_SHORT_WINDOW_US`)
  were declared inside `#if CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL`, so
  `radiant_radio_nrf.c` failed to compile in any ungated nRF build — including
  the plain ANT+ node, which is the configuration a manufacturer starts from.
  It survived because no CI job had ever built that combination: every in-tree
  nRF build turns the gate on, either to satisfy `radiant/Kconfig`'s
  `depends on !BT || ..._GATE_MPSL` interlock or because it is a P4 gate arm.
  The new `build-node` job is what surfaced it, on its first run.
- **Channels assigned to a network with no address are now refused.** A host
  that installed a non-ANT+ key (which is rejected — the key→address table is
  the published pair only) left that network with an all-zero address while
  still *in range*, so an assign to it succeeded, joined the sweep, acquired,
  and then tracked on a zero address and heard nothing. Silent, with no error
  anywhere. Zwift cycles channel 0 across networks 0, 1 and 2, so two thirds of
  its discovery attempts went into that hole — the mechanism behind ANT+
  discovery appearing roughly three times slower than it is.
  Network 0 keeps its all-zero default: that is ANT's public network, and
  refusing it would break both conformance and real hardware.
- **DFU packages carry a real application version.** Both packaging paths —
  `scripts/package_dfu.ps1` and the release workflow — passed a hardcoded `1`,
  so every package ever published declared version 1 and the bootloader's
  downgrade check compared 1 against 1. Both now derive it from the root
  `VERSION` file and fail rather than fall back. See `SECURITY.md` for what
  this does and does not mean for packages published before this change.
- **`LICENSE` is Apache-2.0.** It had been the Mozilla Public License 2.0 while
  `NOTICE`, ADR 0004 and every SPDX header said Apache-2.0 — and ADR 0004
  rejects MPL-2.0 *by name*, because per-file copyleft on firmware that gets
  statically linked into a customer image creates ongoing compliance questions.
- **17 broken documentation links**, all fallout from the `apps/` reorganisation.

### Added

- `scripts/check_license.py`, `scripts/check_links.py` and
  `scripts/version_int.py`, all wired into the no-secret `host-tests` CI job so
  they run on forks and on every pull request. Each exists because the thing it
  checks had already gone wrong once with nothing watching.
- Release artifacts are published with `.sha256` sidecars, and a tagged release
  now fails if the tag disagrees with `VERSION`.
- `SECURITY.md`, stating the signing posture rather than leaving it implied.
- `scripts/clean.ps1`, which refuses to delete anything under `bench-logs/` or
  `archive/`.

### Changed

- **The coded (LE Coded S=8) PHY is behind `CONFIG_RADIANT_PHY_LR_CODED`,
  `default n`.** It used to be compiled into every nRF build, and its 336 µs
  preamble+access address set the advertised `min_arm_lead_us` for every window
  of every PHY: 456 µs on nRF54L15 and 616 µs on nRF52840, where **168** and
  **328** now do. ADR 0007 accepted that as "scheduling slack ... costing no
  airtime or current", which is true at one channel and false at several — see
  its amendment. An image that wants S=8 sets the symbol and pays the lead.

- `archive/captures/coex/` now holds the three P4 coexistence arms that
  `docs/radiant-bridge.md` §7.4.2 actually cites, with recorded SHA-256s. The
  other 140 files from that sitting were intermediate debugging runs and were
  dropped; the README there says so explicitly rather than leaving the
  directory an unaccountable pile.
