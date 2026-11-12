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

- **The dongle reports a serial number.** `MESG_REQUEST` for `0x61` answers
  with a 4-byte little-endian serial, and `0xC6` answers with the same serial
  plus one `0x00`. Both shapes are copied from a real Garmin stick's replies
  (`captures/zwift-20261111-193805.pcap`). Zwift asks for both right after its
  reset, and RadiANT used to refuse them. On nRF the serial is the last eight
  hex digits of the USB iSerial. `ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED` is
  advertised again, but only when the backend really answers, so the
  capabilities reply goes from `...b2...` back to `...ba...`. The sdk-ant
  backend still refuses both. `tools/ant_probe.py` checks the replies.
- **Opt-in wildcard search on nRF (`CONFIG_RADIANT_SEARCH_SHORT_MATCH`, default
  n).** A search window matches only `[A6 C5]` in hardware and reads
  `devnum_lo` from the frame. The sweep becomes one set rather than 32, and a
  background scan reports devices at the Garmin stick's rate (12.2 against
  12.6 sightings/s on a Pro Micro, against 1.0/s without it). It stays off by
  default until its effect on tracked loss is settled. See ADR 0022.
- **`tools/ant_scan_rate.py`** measures discovery speed: sightings/s, time to
  first sighting after each scan open (with Zwift's reopen cadence), and
  tracked-channel loss, on several dongles in one window.
- **RadiANT is usable from other firmware.** The `antr_*` API and its runtime
  moved into the `radiant` module behind `CONFIG_RADIANT_ANTR_API`, with
  heart-rate receiver and transmitter samples in `radiant/samples/` and two
  component switches, `CONFIG_RADIANT_PROFILES` and `CONFIG_RADIANT_BRIDGE`. See
  `radiant/README.md`.
- **The module builds on stock Zephyr.** It used to abort Kconfig for every build
  in a workspace without the Nordic Connect SDK. CI now builds the samples on
  stock Zephyr v4.4.0.
- **The event filter filters.** `MESG_EVENT_FILTER_CONFIG` (0x79) now suppresses
  the events it names at delivery, after internal accounting. The default of 0
  passes everything.
- **Discovery follows the key and the searchers.** The sweep uses the ANT+ key's
  network even when it is not network 0, and the frequency of searching slaves on
  keyed networks. Channels on keyless networks, such as Zwift's scans on RF 71 and
  53, never move it. A keyed slave that sets no frequency defaults to 66 and moves
  the sweep there.
- **Pro Micro images reboot into their UF2 bootloader over USB.**
  `python tools/ant_probe.py --reboot-to-bootloader` mounts the drive with no
  RST-to-GND short. On in both Pro Micro release images and asserted at build
  time; other boards opt in with `apps/dongle/uf2_reboot.conf`. Any software on
  the host that can open the device can now put these boards into a writable
  bootloader with no physical access.
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

- **The capabilities reply claims less.** Search sharing, selective data update and
  encrypted channel are no longer advertised (`advanced_options_3` 0xD5 to 0x05).
  Their commands are still accepted.
- **Host tools.** `ant_ack_gate` exits non-zero on any quality failure with or
  without `--gate`. `ant_sens` drains every receiver through the settle.
  `ant_verify` counts sealed 0x60 pages without decoding them, labels slot loss an
  upper bound when the counter outruns the clock, and prints "no exact-loss source
  for this profile".
- **The heart-rate and treadmill nodes change device number once.** They now
  generate their identity key from the chip's own TRNG on the first boot that
  finds none ([ADR 0020](docs/decisions/0020-k-dev-is-generated-on-first-boot.md)),
  so each unit gets its own derived device number instead of the shared
  Kconfig 4242/4243. Receivers paired to the old number re-pair once, and a
  full flash erase re-rolls it. If identity boot fails the node falls back to
  the Kconfig number and logs an error every boot. `apps/trainer` keeps its
  fixed 4244.
- **Transmit power: an explicit 0 dBm is now 0 dBm, and the nRF54L15 and
  CC26xx boost higher.** `CONFIG_ANT_DONGLE_TX_POWER_BOOST` raises only the
  unset default and level 4; an explicit level 3 is a true 0 dBm. The boost
  now asks for level 5 on every radiant build and lets the backend round:
  +8 dBm on the nRF54L15 (was +4) and +5 on CC26xx (was +4); the nRF52840
  (+8) and nRF5340 net core (0) are unchanged. Zwift never sets power, so it
  still gets the maximum.
- **UART builds announce every boot.** They send an unsolicited STARTUP with
  the hardware reset cause (0x00 power-on, 0x01 pin, 0x02 watchdog, lockup or
  software reset, which includes a crash), so a host can tell that the board
  rebooted. That matters most on the CC2652P, where the CP2102N keeps the
  USB handle open across a reboot. Every build now logs its reset cause at
  boot. USB builds are unchanged.
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
  wedged broker costs one stall, not ~100 s of every channel's polling. The
  keepalive thread is also the only one that connects: the sample path used to
  hold the sink lock across the TCP connect and CONNACK wait (up to ~8 s),
  stalling the drain thread that also feeds the Matter sink and the rule engine
  (A/B on an nRF54L15 with no broker: worst publish 3001 ms → 1 ms, bus drops
  ~27 → 0 per minute). The bridge's `online` status now counts as published
  only if its send succeeded, so a reconnect no longer skips it under a
  retained `offline` LWT.
- **The Feather names itself in its USB product string**
  (`RadiANT ANT+ compatible USB stick (Feather)`), so it can be told apart from
  an nRF52840 Dongle on the same host. VID/PID and `bcdDevice` are unchanged,
  and no host matches on the product string.
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

- **A receive-only slave (channel type `0x40`) never transmits.** It used to
  acknowledge inbound acknowledged and burst data like a normal slave. It now
  delivers the data and leaves it unacknowledged, and `antr_burst_tx()` refuses
  `0x40` and `0x50` channels with `CHANNEL_IN_WRONG_STATE`, as acknowledged
  data already did. Zwift's discovery channel is `0x40`; its paired sensor
  channels are `0x00` and are unaffected.
- **Lib Config (`0x6E`) replaces the previous value.** It used to OR the new
  bits in, so a host that went from `0xE0` to `0x80` kept receiving RSSI and
  timestamps it had not asked for, and mis-parsed every message. Zero still
  clears everything; an unknown bit is still refused with no side effect.
- **Event Filter (`0x79`) takes the spec's filler byte.** The command is
  `[filler, lo, hi]` (Rev 5.1 §9.5.2.28); RadiANT read a 3-byte command as
  `[lo, hi, ...]` and set the wrong filter. The legacy 2-byte `[lo, hi]` form
  is still accepted. `tools/ant_features.py` now sends the 3-byte form, which
  also explains the Garmin's `0000f1` readback.
- **Open Rx Scan Mode (`0x5B`) keeps the host's channel 0 setup.** It used to
  reassign channel 0 as a wildcard slave on network 0 at RF 66, so a host that
  had set RF 57 and the ANT+ key scanned the wrong band. Channel 0's type,
  network, channel ID, RF and period now survive (Rev 5.1 §9.5.4.5); its ID
  list, low-priority timeout and search priority still reset. A master
  channel 0 is refused with `CHANNEL_IN_WRONG_STATE`.
- **The Linux udev rule grants access.** It was named `99-`, which sorts after
  the systemd rule that applies `uaccess`, so the device stayed root-only. It is
  now `host/linux/70-radiant-ant.rules`, also gives the `dialout` group access for SSH
  and headless use, and releases the kernel's `usb_serial_simple` binding so
  libusb apps can claim the stick. Remove any installed `99-ant-usb.rules`.
- **The bridge and Matter builds link again.** `apps/dongle_thread` listed the
  bridge and profile sources that the module had started compiling under
  `CONFIG_RADIANT_BRIDGE`, so `bridge.conf` and `matter.conf` failed with duplicate
  definitions. The module now owns all of `src/bridge/`, with its three sinks
  behind `CONFIG_RADIANT_BRIDGE_RULES`, `_LIVENESS` and `_MATTER_MAP`, and the
  application lists none of them.
- **Scheduler and nRF backend.** An armed operation that never terminates is
  reclaimed (A9-3), a receive window inside its PHY switch is reported MISSED
  (A8-4), a window whose start compare was written too late is refused rather
  than hanging (A14-1), and the deferred callback queue carries the HAL's longest
  body (A14-2).
- **Bridges.** A Matter reachability or label change is no longer lost during a
  flush, and every owed MQTT offline survives a broker outage.
- **Security.** The 0xF1-0xF5 host commands and secured-master posts run under
  `api_lock`; a Tier II tag due on HR messages 119/120 is no longer lost at the
  seam; first-boot provisioning avoids hard-to-receive device numbers.
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
- **A USB bus reset mid-frame swallowed the host's next command.** The host
  frame parser kept its state across a new USB configuration, so a host that
  reset part way through a frame had its first `RESET_SYSTEM` eaten as the old
  frame's tail. The parser now resets exactly at the new session's first byte
  (a byte-count boundary published by the USB class), and a SET_KEY/pairing
  frame that fails its checksum is now wiped too. A complete frame the old
  session left queued still executes. On the `next` USB stack a repeated
  same-value SET_CONFIGURATION is not a new session (the stack never reports
  it); a bus reset is, on both stacks.
- **An MPSL assert on the bridge and Matter images now reboots instead of
  halting forever.** `mpsl_assert_handle()` called `k_fatal_halt()`, which
  bypasses the fatal hook, so no setting could make it reboot; and
  `bridge.conf`/`matter.conf` inherited `thread.conf`'s bench
  `RESET_ON_FATAL_ERROR=n` anyway. The handler now reboots under that symbol
  (after the synchronous dump), and both shipping fragments set it `y`. The
  `gate.conf`/`coex.conf` arms already carried `y` and now reboot on an assert
  too; `thread.conf` alone still halts.
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

- X_CONF without X_AUTH now encrypts byte [7]; it previously went out in clear.
  Sealed-frame layouts accept versions 1-7 as compatible and reject 8-15.
These fixes apply only to builds with the opt-in `security.conf`; no release
image includes it.

- **A secured channel stopped verifying within minutes when it carried
  anything else.** The sender's counter advanced once per secured transmission,
  while the receiver counts time, one per slot. Each clear slot (a 0x60 node's
  descriptor and common pages, 5–12 per 121-slot cycle), each MPSL-denied slot
  and each sparse silence put the sender one count behind, faster than the
  receiver's `period / 16` re-anchor follows; within 8–45 minutes nothing
  verified, and a sparse `X_CONF` node decrypted under the wrong nonce after
  any silence over 128 slots. The counter is now the sender's slot index,
  derived from the slot's time (`radiant_sec_tx_transform()` takes `t_sync`),
  and a slot posted twice resends identical bytes. Unsecured 0x60 nodes carry
  the slot index in byte `[1]` too, and `tools/ant_verify.py` reports their
  exact loss with scope `slot`. Still a cost, by decision: a window holding a
  clear slot is unverified, as is the one before it (2–14 % of secured pages,
  W = 2…8; [docs/radiant-security.md](docs/radiant-security.md) §3.2). Sparse
  plus `X_AUTH` is refused in v1. +228 B text, +192 B bss.

- **A keyless injector could replay or desynchronise a secured channel.** The
  receiver re-anchored its clock on every packet that passed the replay check,
  before any MAC was checked, and that check accepts counters up to 128 periods
  behind. Forged counters could drag the clock back, so recorded genuine
  windows verified again, or push it forward into the epoch advance, which is
  a permanent desync. Now only a `VERIFIED` window moves the clock (in full on
  the first lock after a reset, then by at most `period / 16` per window), and
  each window verifies at most once per epoch. Window 0 of every new epoch,
  which was always refused as a replay, now verifies. See
  [docs/radiant-security.md](docs/radiant-security.md) §3.1; the remaining
  residuals are in §7.6.
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
- On Windows, a host tool that dies mid-frame can still have the next tool's
  first command swallowed. Reopening the device without a bus reset did not
  reset the host-frame parser in bench testing (0 of 10); a bus reset or replug
  does.
- MPSL builds cannot run multi-packet bursts.
