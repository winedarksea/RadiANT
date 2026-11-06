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

- **Acknowledged data works on MPSL builds.** Until `c3bc9e6` acked transfers
  never completed on a Thread/BLE-coexisting image: the scheduler lead was
  wrong, a grant was released before its callback ran, and master reserves
  nested. Five further defects stacked behind it (including a ~600/s flood
  where the nRF backend replayed its last frame into every empty RX window)
  and are fixed, as is a broadcast refusal that dropped one send in ~280.
  `tools/ant_ack_pair.py` is the A/B harness.
- **Bridge product, `apps/dongle_thread`.** Self channels 6 and 7 are pinned to
  paired sensors on the Zwift model (pinned / idle / evict only when a window
  opens) and are re-established after a host `SYSTEM_RESET`. Discovery skips
  devices already pinned on another self channel. MQTT and Matter sinks
  publish what the bridge binds; the Matter bridge fits an nRF54L15 and an
  nRF52840, and a dying endpoint row is no longer reused for a new sensor.
- **Treadmill reference design, `apps/treadmill`.** Broadcasts FE-C, RSC and
  FTMS from a simulated belt, accepts FE-C/FTMS control, and reads a heart-rate
  strap as an ANT+ slave. Only one control source owns the belt at a time
  ([ADR 0017](docs/decisions/0017-fec-treadmill-control.md)).
- **`apps/trainer`, a trainer simulator with ACK fault injection,** and
  `tools/fec_ack_sweep.py`, built to find why a real trainer drops the return
  ACK. `CONFIG_RADIANT_ACK_FAULTS` is never in a dongle image; CI asserts it.
- **nRF5340 network core is an ANT+ endpoint.** The DK enumerates and tracks
  on air through the net core (`nrf5340dk_nrf5340_cpunet`). Its TX power tops
  out near 8 dB below the other boards, which skews loss comparisons.
- **TI CC2652P dongle (`apps/dongle_ti`, `boards/ti/cc2652p_dongle`).** A working
  ANT+ dongle over its own USB, with the antenna switch on DIO28 (worth ~8 dB).
- **`apps/hrm_ble` is a reference design** (BLE heart-rate strap that also
  transmits ANT+), and a BLE node now advertises again after a disconnect, so
  it can be paired more than once per boot. `apps/treadmill` got the same fix.
- **`MESG_RADIANT_HEALTH` (`0xF6`).** Request-only counters for what the dongle
  drops silently (USB queue overflows, missed windows, host-parser resyncs);
  `tools/ant_health.py` reads them. Behind `CONFIG_ANT_DONGLE_HEALTH_COUNTERS`.
- **Opt-in RadiANT security (`security.conf`, off by default, in no release
  image).** Per-channel `X_AUTH`/`X_CONF` payload transforms, X25519 pairing and
  the `0xF1`–`0xF4` host messages ([docs/radiant-security.md](docs/radiant-security.md)).
  A secured channel does not interoperate with ANT+ profiles.
- `scripts/check_license.py`, `scripts/check_links.py` and
  `scripts/version_int.py`, wired into the no-secret `host-tests` CI job, and
  `SECURITY.md`, which states the signing posture.
- `tools/ant_search_contention.py` measures a tracked channel's loss while a
  background scan runs (A/B/A, needs `--ext 0x01`), and two tests guard that
  case: `radiant_sched.test_a_tracked_channel_keeps_every_slot_under_a_search`
  and `api.test_a_channel_that_drops_to_search_does_not_starve_one_still_tracking`.

### Changed

- **A pending acknowledged command waits longer for a master slot.** A slave's
  queued ACK command now gets three master slots (`RADIANT_API_ACK_OPENER_PERIODS`),
  so it waits up to max(500 ms, 3 channel periods) — 750 ms on FE-C — before
  failing with no opener. Against a trainer losing ~53 % of packets, 26 % of
  grade commands expired with two slots; three is the ceiling, a fourth would
  land on Zwift's next command.
- **A named device is revisited during search.** An acquiring channel with a
  known set is re-steered after 1, 2, 4 … round-robin sets, so one lost packet
  costs 0.25–2 s to recover instead of a whole 8 s sweep. Scan channels get no
  revisits. A search may also turn a device down (a `permit` callback at offer
  time) without losing its place, so a second wildcard channel no longer
  re-steers to a device pinned elsewhere.
- **`CAPABILITIES` max channels is capped below the self channels (6 on the
  bridge).** Bridge images only; the plain dongle is unaffected. A host that
  allocates below the max it is told can no longer collide with a self channel.
- **The MQTT sink does no network I/O on the self thread.** Releases and binds
  are recorded in an owed-offline ledger, and the keepalive thread delivers them,
  connecting if needed (backoff 5 s to 5 min). Broker sends time out at 3 s, so a
  wedged broker costs one stall, not ~100 s of every channel's polling.
- **Every application defaults to a real radio.** `apps/dongle` and
  `apps/dongle_thread` default to `nrf`, `apps/dongle_ti` to `cc26xx`; the inert
  `null` backend used to enumerate, answer OK and find no sensors, which looks
  like a dead antenna.
- **The legacy USB write is bounded at 1 s.** A host that stays configured but
  stops polling bulk-IN no longer parks the bridge and TX threads for good; the
  timeout is counted in the health reply.
- **The event ring keeps 16 slots for owed-once events** (`NEXT_DATA_BLOCK`,
  `TX_COMPLETED`/`FAILED`, `CHANNEL_CLOSED`), so a data flood cannot swallow
  them. Aborting our own acknowledgement raises no `TX_FAILED`.
- **The `led0` heartbeat is a 30 ms flash every 4 s, scaled by receive activity
  (4 s idle → 150 ms with ten sensors).** `CONFIG_ANT_DONGLE_HEARTBEAT_LED=n`
  removes it entirely. The loop is one file, `apps/common/ant_heartbeat_led.c`;
  the activity counter is `ant_activity.h` and exactly one indicator may read
  it per image (`CONFIG_ANT_DONGLE_ACTIVITY_CLAIMED`).
- **Release artifacts are cut from the clean-room backend and sdk-ant is gone
  from CI.** The 30-minute Tier 3 Zwift acceptance passed, resolving the clause
  in [ADR 0001](docs/decisions/0001-backend-selection-and-release-default.md).
  `build-core` packages and attaches the four shipping images; no job needs a
  secret. sdk-ant stays as the opt-in A/B reference
  (`build_all.ps1 -Backend sdk_ant -NcsVersion v3.2.4`), but `apps/sim` is no
  longer compiled by any job.
- **`ANT_RADIO` defaults to `core`, not whatever is on disk.** A bare
  `west build` used to produce a proprietary image on any bench that could
  build sdk-ant; `-DANT_RADIO=sdk_ant` must now be typed. `build_all.ps1`
  defaults to `-Backend core` and NCS v3.4.0, and only `core` writes `dist\`.
- **One Feather image carries the RGB activity indicator.** The shipping
  `radiant_dongle.uf2` applies `rgb.conf` + `rgb_feather.overlay`; the separate
  `feather_rgb` build row is gone.
- **The coded (LE Coded S=8) PHY is behind `CONFIG_RADIANT_PHY_LR_CODED`,
  `default n`.** Its 336 µs preamble set `min_arm_lead_us` for every PHY: 456 µs
  on nRF54L15 and 616 µs on nRF52840, now **168** and **328**
  ([ADR 0007](docs/decisions/0007-long-range-phy.md) amendment).
- `encryption.conf` moved into `build_all.ps1`'s `core` block, and
  `apps/dongle/sample.yaml` passes `ANT_RADIO=core` so twister configures
  without a private checkout.
- `archive/captures/coex/` keeps only the three P4 arms that
  `docs/radiant-bridge.md` §7.4.2 cites, with SHA-256s.

### Fixed

- **Matter pressure was 10× and 1000× too small.** `MeasuredValue` is now
  0.1 kPa per step (Matter 1.5.1 §2.4.5.1), so it reads 10× larger than before
  (101325 Pa: `101` → `1013`), and `Scale` changes from `2` to `-1` because
  `ScaledValue` is in pascals (§2.4.5.5), not kPa; `ScaledValue` itself is
  unchanged. `Min`/`MaxScaledValue`, mandatory under the EXT feature, are now
  served. A controller that cached the old values sees a step change.
- **Page 84's minimum/maximum operating temperature are no longer posted.**
  Subpages 7 and 8 are operating limits, not the present temperature, but each
  became a Matter Temperature endpoint (against the cap of 16) and an MQTT/Home
  Assistant `device_class: temperature` entity. They are still
  decoded; field ids `0x27`/`0x28` stay reserved. **Existing installs keep the
  retained HA discovery for these two entities**, and nothing in the bridge
  removes it: delete them by hand (clear the retained
  `homeassistant/sensor/<uuid>_<key>/config` topics, or HA re-creates them on
  restart). Likewise a Matter endpoint a previous boot persisted for them is
  still restored at boot, unreachable, and is not reclaimed automatically.
- **A searching channel cost a tracking one every other packet.** `arm_next()`
  truncated a scan chunk to end one arm lead before the next tracked window, but
  a receive stays open for the frame body after `t_close`, so the tracked window
  armed with 23 µs of 254 µs left. A/B/A on an nRF54L15: 0 % / 50 % / 0 % before,
  0 / 0 / 0 after. The gap is now `lead + phy_gap + rx_tail()`; the CC26x2 also
  declares 508 µs of `rx_close_hold_us`. `fake_radio` ends a window at `t_close`,
  so no ztest could see it; `test_a_truncated_scan_leaves_room_for_the_chunks_own_tail`
  now asserts the schedule.
- **Zwift sessions.** A search timeout mid-chunk no longer re-binds a search on
  a closed channel; closing with a transfer queued finishes it; an inbound
  burst numbers 0,1,2,3,1,2,3 with no 256-packet wrap; CRC repair admits only
  broadcasts; `SET_NETWORK_KEY(0)` defers the search re-init while a chunk is
  live; per-channel bridge tables are sized 32; and `usb_ant_write` reports
  failures instead of returning success after a host stall.
- **An assign to a keyless network stopped Zwift discovering anything after the
  first sensor.** The refusal (`ANTW_INVALID_NETWORK_NUMBER`) fired every few
  seconds as Zwift cycled its wildcard channel over networks 0/1/2: 5385
  `CLOSE_CHANNEL`s in 127 s and one device found. It is now warned and
  accepted; the sweep matches on network 0's address.
- **Capabilities no longer advertise a serial number the dongle cannot produce.**
  `ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED` was set while `0x61` answers
  `INVALID_MESSAGE`, and Zwift asks for it at startup. The reply changes
  `a40954200300ba2600d5000192` → `a40954200300b22600d500019a` (byte 3 `0xba` →
  `0xb2`). Both conformance transcripts are re-recorded and hashed;
  `conformance-nrf-radiant.antser` (nRF54L15 DK, 2026-11-06) is now
  byte-identical to `conformance-cc26xx.antser`.
- **Two tracked channels within one arm lead of each other lost the later one
  every period.** Merge reach is now `arm_lead()` itself, about 1 % of slots per
  channel at eight sensors; pairs are rescued, a three-way pile-up still drops
  the third ([ADR 0016](docs/decisions/0016-merge-reach-is-the-arm-lead.md)).
  A tracked window that never armed is re-posted at once instead of waiting for
  the 50 ms pump (eight in a row was a ~2 s dropout).
- **2026-10-31 review fixes.** Security and crash-proofing: the secure RX pump
  runs under `api_lock`, a pairing retry can no longer install a publicly
  computable root key, a counter wrap stops secured TX instead of reusing,
  tag compares are constant-time, and the Feather enables
  `CONFIG_HW_STACK_PROTECTION`. Bridge: a just-bound self channel is no longer
  rotated away, a re-acquired stranger is unbound, MQTT availability is
  republished after STALE and every reconnect, and Matter restores adopted
  endpoints as dirty. Broadcasters: the treadmill HR slave reopens after a
  search timeout (it used to never acquire again), RSC distance is in 0.1 m,
  FTMS Stop/Pause is range-checked.
- **A host reset mid-poll no longer strands a re-established self channel,**
  and a Matter endpoint row that was dying is not reused: `needRemove` is
  cleared only by the memset, sinks learn of a bind before traffic is
  attributed, and a forget is sticky until admit.
- **The nRF backend did not build without the MPSL gate** (three statics
  declared inside `#if CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL`); the plain ANT+
  node was affected. `build-node` now builds that combination.
- **DFU packages carry the real application version** from `VERSION`, not a
  hardcoded `1`, so the bootloader's downgrade check works. See `SECURITY.md`
  for packages published earlier.
- **`LICENSE` is Apache-2.0,** as `NOTICE`, ADR 0004 and the SPDX headers
  already said; it had been MPL-2.0.
- **`tools/decode_pcap.py` spliced the bulk streams of different USB devices**
  in an `-All` capture; buffers are now keyed by `(device, direction)` and
  `--device` filters. `scripts/cap_zwift.ps1` and `decode_pcap.py` gained SPDX
  headers; two API tests no longer depend on test order.

### Security

- **The drop policy delivered forgeries as verified (review D1-1).** Under
  `DROP_UNVER` a closing window was released on the verdict judged at that
  moment, which is its predecessor's, so a packet injected into any window
  went out marked `VERIFIED` whenever the window before it was genuine. No
  key was needed. Each window is now held until its own verdict exists, so
  the drop policy's delivery latency is W+1 to 2W packet periods (0.75–1.0 s
  at W=2, 4 Hz), and the first window of an epoch is no longer dropped. The
  default deliver-marked policy was not affected.

### Known issues

- MPSL builds' search is deaf for ~3 arm leads per tracked period, so finding a
  new sensor while another channel tracks misses roughly 30 % of phases
  ([ADR 0018](docs/decisions/0018-search-listens-through-the-arm-lead.md),
  proposed; the fix is not built).
- The MQTT sink's connect and publish paths have never run against a real broker.
- No image has a watchdog.
- An MPSL assert halts the Thread, bridge and Matter images; they do not reboot.
- MPSL builds cannot run multi-packet bursts.
