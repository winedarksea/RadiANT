# Changelog

Notable changes only, newest first. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

**This file has a length budget, like `README.md` does.** If it grows past
roughly 200 lines, drop the oldest entries rather than letting it become a
second copy of `git log` — a changelog nobody finishes reading is one nobody
reads.

## [Unreleased]

### Fixed

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
