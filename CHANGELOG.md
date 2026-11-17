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

- **TI: acknowledged data sent as a slave no longer fails for whole
  sessions.** The reply went on air about 105 us earlier than a real ANT slave
  answers, at the early edge of the master's reply window. A Garmin master's
  window wanders with its own timing, so the TI's acknowledged messages failed
  for stretches of seconds to entire sessions and worked the rest of the time.
  Every TI transmit now starts 100 us later
  (`CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US`), measured on air with a sniffer.

- **TI: two tracked channels no longer share a receive window the radio cannot
  hear.** The CC26x2 tells sync words apart with a correlator, and two words
  1-3 bits apart leave it deaf to both (0 of 30 frames at 1 bit). The search
  already kept its pairs apart; merged tracking windows did not. The scheduler
  now gives such a channel its own window. The default TI build was not exposed
  with ANT+ sensors, because its hardware sync word is the ANT+ network address
  every sensor shares. A `ti_long_match.conf` build was exposed for about a
  third of sensor pairs (device numbers whose low bytes are 1-3 bits apart).
- **A slave that acquires its sensor now hears the next frame too.** When the
  only searching channel acquired, the search kept listening for the rest of
  its dwell. A sensor found early in a dwell then had its second frame dropped
  (on the Pro Micro, about 2 % of acquisitions, reported as a channel
  collision). Under `X_AUTH` that first window was UNVERIFIED, and `DROP_UNVER`
  dropped the acquiring frame with it. The search now stops as soon as nobody
  is searching: 0 of 150 acquisitions lost frame 1, against 3 of 150 before
  (`archive/benchmarks/2026-11-16-item6-frame1/`). Discovery with other
  channels still searching, and background scans, are unchanged.
- **An `X_AUTH`-only channel whose key derivation failed kept transmitting.**
  If deriving the epoch's keys failed, secured pages went out with no MAC
  behind them. `X_CONF` already refused. Nothing secured now goes out until a
  new epoch is set. Only an opaque key backend can fail there; the portable
  one cannot.

- **The TI dongle stopped answering host commands under load.** With about 9
  tracked channels, the CC26xx UART lost a host byte every few tens of seconds:
  Zephyr's driver runs it with the FIFO off, so a 1-byte receiver overran
  whenever its ISR was held off for more than about 170 µs. One lost byte could
  stall the host for seconds to minutes, because the parser then swallowed the
  following commands and their resends. Zwift's 4.5 s scan reopen hit it
  within 5-40 s. The ANT UART now runs with the hardware FIFO on (about 4 ms
  of tolerance). On the full C2 workload for 300 s: 0 unanswered commands,
  against a stall at 26 s on the old image
  (`archive/benchmarks/2026-11-16-ti-cmd-stall/`).

### Changed

- **Damaged host frames are handled differently, on every transport.** A frame
  with LEN 39-255 used to be answered: the parser dispatched it truncated,
  usually as INVALID_MESSAGE. It is now dropped silently and counted in 0xF6
  `host_desync`. A failed checksum byte of `0xA4` now starts the next frame. No
  legal host frame is affected. See `docs/ant-serial-protocol.md`, framing.
- **0xF6 is 34 bytes, with `uart_rx_overruns` at [32..33].** An optional field
  this build or transport cannot measure (coverage without
  `CONFIG_RADIANT_SEARCH_COVERAGE`, overruns on USB) now reads `0xFFFF` instead
  of being left out. Measured counts clamp at 65534. The structure version
  stays 1, and `tools/ant_health.py` treats `0xFFFF` as not measured. Owner
  accepted in beta; ADR 0023 carries the amendment. **Mixed versions misread one
  field:**
  - An older reader (released v1.2.0's `ant_health.py` decodes any body of 32
    bytes or more as having coverage) reports `sets_advanced_uncovered = 65535`
    on every non-coverage build (TI, USB, non-MPSL nRF). There it means "not
    measured".
  - A new reader shows an old MPSL image's saturated coverage (65535) as "not
    measured".
- **The radio hot path is compiled at -O2 in the shipping dongles**
  (`CONFIG_RADIANT_HOT_PATH_O2` in `apps/dongle/prj.conf` and
  `apps/dongle_ti/prj.conf`, owner ruling 2026-11-16). `radiant_sched.c`,
  the antr runtime and the active radio backend build at -O2; the rest of the
  image stays at -Os. Under load on the Pro Micro the worst pass-to-arm CPU
  time (0xF7 page 9 [5]) was 289 µs at -Os and 255 µs at -O2, against a
  240 µs budget (`archive/benchmarks/2026-11-16-pass-breakdown/`, one clean
  pair). Cost: +4864 B flash on the Pro Micro, +4468 B on the cc26xx
  LaunchXL, RAM unchanged. The library default stays n. The release matrix
  asserts it on both trainer rows (nRF52840 and nRF54L15), which also keeps
  the simulator at the -Os its ACK window was characterised at, and on the
  cc26xx long-match row (`ti_long_match.conf` sets it to n), the one cc26xx
  build at -Os. Treadmill, hrm_ble and dongle_thread also build at -Os but
  do not assert it. The security text ceiling in
  `release.yml` moved 7168 -> 7680 B, because the security hooks in the antr
  runtime grow at -O2 (7304 B measured); no security code changed.

- **CRC repair is on in the shipping dongles** (`CONFIG_RADIANT_CRC_REPAIR`
  and `CONFIG_RADIANT_PROFILE_SANITY` in `apps/dongle/prj.conf` and
  `apps/dongle_ti/prj.conf`). The owner accepted the nRF bench gate on
  2026-11-15: on the Pro Micro, 22 frames repaired (11 at -28 dBm, 11 at
  -20 dBm), 0 mis-repairs, and the 0xF7 page 10 counters reconcile with the
  truth script's counts within the end-of-run tail (+2 to +8 frames per
  block; `archive/benchmarks/2026-11-15-nrf-crc-repair/`). The
  plan wanted 20 at one rung; the owner accepted the split. Only the Pro Micro
  was gated, but `apps/dongle/prj.conf` covers every nRF dongle image (MPSL,
  nRF5340 net core and nRF54L15 included), following the short-match ruling
  of 2026-11-14. The cc26xx run (2026-11-13) is the "CRC repair works on
  cc26xx" entry under Fixed below. The library
  default stays n; `apps/dongle/crc_repair_off.conf` builds a dongle without
  repair, and the release matrix's repair row now asserts that build. The
  10 s and channel-close counter log lines moved to debug level; page 10 is
  the read path.

- **nRF wildcard search is on by default** (`CONFIG_RADIANT_SEARCH_SHORT_MATCH`,
  ADR 0022). All four nRF gates passed on 2026-11-12 (tracked loss -0.23 pp
  against the same-SoC control, bound 2.16 pp). Discovery goes from about
  0.5-1 to 4-12 sightings/s. The Zwift capture is the acceptance check;
  `apps/dongle/long_match.conf` builds the old 24-bit image. This covers every
  nRF image, MPSL, Matter, nRF5340 net core and nRF54L15 included.

- **Acknowledged data no longer reserves air after its acknowledgement
  window.** Since e042a10 every window reserved 1550 µs for a next packet or
  retry; a one-packet transfer has neither, and under an arbiter (cc26xx coex
  `endTime`, MPSL) that held the other stack's air for nothing. Multi-packet
  bursts keep the reserve. Host-tested only. The TI image staged in
  `build/final_perf_6078d88/` predates this: rebuild it and check acknowledged
  transfers on air before it ships.

- **The Zwift shim's log (`RADIANT_SHIM_LOG`) can be trusted line by line.**
  Each line is one append-only `WriteFile`, so ANT_DLL's read and write
  threads, and the two shim DLLs, no longer glue or blank each other's lines.
  Every line carries a QPC timestamp (1 µs), the thread id and the DLL; the
  load line adds wall-clock time and PID. Reads and writes log how long they
  blocked and the ANT message ids written; runs of empty reads fold into one
  line a second. "COMn absent" now gives the reason (`err=2` not present,
  `err=5` held by another process) and where the port name came from.
  `RADIANT_SHIM_LOG_LEVEL` 0/1/2 (`scripts/zwift_ant_shim.ps1 -LogLevel`)
  trades volume for detail; level 2 adds hex payloads. See
  `tools/zwift_shim/README.md`.

- **`tools/decode_pcap.py` reports transfers and loss that hold on a Garmin
  capture.** The transfer ledger counts the host's own ACKNOWLEDGED_DATA, so a
  Garmin USB-m (which never sends RESPONSE_NO_ERROR for 0x4F) no longer shows
  accepted=0. A capture that starts mid-session keeps the first transfer
  outcome out of the totals (`presync`), and FE-C channels get a per-page line
  (0x33 completion). New `slots=... loss=...` per channel from the receive
  timestamps, beside RX_FAIL, which undercounts (Garmin FE-C: 84.3 % against
  77.7 %). The period comes from `--period CH=N`, CHANNEL_PERIOD or the
  timestamps, and says which. FE-C pages are decoded as a `[FE-C ...]`
  suffix. With `-o`, the header goes to stdout and the file holds only the
  body. Zwift's `00 00` OUT padding is `padding_bytes`, not `discarded_bytes`.
  The summary's `transfers` line changed shape: `unmatched_terminal_events`
  is now `unmatched`, shown only when non-zero.

### Fixed

- **Security (not in any release image): the 2026-11-14 review items 1-4.**
  Acquiring and background-scan frames now go through the receive hook like
  tracked ones, so an acquisition no longer hands the host ciphertext or an
  unjudged forgery. A secured master can no longer acknowledge with plaintext.
  `node_ident` refuses a second live arm under one root and one device number,
  which could otherwise reach the other channel's epoch. Wipes that `-Os` had
  elided are now `radiant_sec_wipe()`. Compat `set_key` resets the on-air
  number and leaves no half-replaced context. A failed enrolment wrap closes
  its window. Size gates unchanged, all under their ceilings (security text
  7144/7168). Details and commits: TODO.md, 2026-11-14 review.

- **MPSL builds (Thread, Matter, BLE coexistence, gate) no longer halt or
  reboot with `MPSL ASSERT 69:108`.** A RADIO event that fired while a TIMER0
  signal handler ran left the RADIO interrupt pending in the NVIC; after the
  timeslot ended MPSL took it with no timeslot open. The gate now clears the
  pending bit at every end of a timeslot that used the radio (63b2a02, plus
  three hardenings in 1c8967b). On the nRF54L15 beside a Thread leader the
  assert had fired within 1.5-6.3 minutes in every leg; three 30 minute legs and
  a 2 h soak on the fixed image had none.

- **A same-root `0xF2` keeps a Tier 2 receiver's trial device number.** The
  re-send was already a no-op for the epoch and gate, but the adapter still
  reset the on-air device number to the provisioning one, so the receiver
  stopped verifying until it tried again. A different root still resets it.

- **CRC repair works on cc26xx.** The backend dropped the body of a
  CRC-failed frame, so `CONFIG_RADIANT_CRC_REPAIR` could never repair on TI.
  On air (300 s, `archive/benchmarks/2026-11-13-ti-crc-repair/`): 28 frames
  repaired, 7 unrepairable, 0 refuted, and every delivered payload matched the
  transmitter. Repair was off by default at the time; see "CRC repair is on
  in the shipping dongles" above.

### Added

- **0xF7 pages 9 and 10** (ADR 0023, diagnostic builds only): page 9 splits
  the scheduler pass (expiry sweep, arm decision, backend arm call, done
  callbacks, `book_ahead()`), the pass-to-backend clock span `ARM_SETUP_US`
  must cover, the smallest slack at the backend's early check, and that
  check's ETIME refusals, which were counted nowhere. Page 10 carries the CRC
  repair counters, the nRF END-interrupt lateness, and cc26xx's per-entry
  `rx_t_sync()` and match times. The short spans use the core's cycle counter
  (DWT); page 8's cc26xx per-entry timings move to it from the RAT. Not yet
  read on air.

- **0xF7 page 8, timing** (ADR 0023): receive-drain parts on cc26xx (frames,
  per-entry work, CRC, core callback, drains over 250 µs) and the scheduler
  pass on both backends (worst µs, steps, passes over 100 µs). First reading:
  TI's ~450 µs drain is one frame, ~346 µs of it the core callback (a full
  scheduler pass included) and ~60 µs the bitwise CRC
  (`archive/benchmarks/2026-11-13-drain-timing/`).

- **cc26xx wildcard search, on by default** (`CONFIG_RADIANT_SEARCH_SHORT_MATCH`,
  ADR 0022). The CC2652R sweeps one set instead of 128: 4.45 sightings/s
  against 0.52, median first sighting 0.30 s against 0.84 s, tracked loss no
  worse. The 16-bit match applies to tracking windows too.
  `apps/dongle_ti/ti_long_match.conf` builds the 24-bit image.
- **`tools/ant_scan_rate.py --port/--baud`** adds a UART board (the TI) as a
  same-window child beside USB dongles.
- **The dongle reports a serial number.** `MESG_REQUEST` for `0x61` answers
  with a 4-byte little-endian serial, and `0xC6` answers with the same serial
  plus one `0x00`. Both shapes are copied from a real Garmin stick's replies
  (`captures/zwift-20261111-193805.pcap`). Zwift asks for both right after its
  reset, and RadiANT used to refuse them. On nRF the serial is the last eight
  hex digits of the USB iSerial. `ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED` is
  advertised again, but only when the backend really answers, so the
  capabilities reply goes from `...b2...` back to `...ba...`. The sdk-ant
  backend still refuses both. `tools/ant_probe.py` checks the replies.
- **Wildcard search on nRF (`CONFIG_RADIANT_SEARCH_SHORT_MATCH`).** A search
  window matches only `[A6 C5]` in hardware and reads
  `devnum_lo` from the frame. The sweep becomes one set rather than 32, and a
  background scan reports devices at the Garmin stick's rate (12.2 against
  12.6 sightings/s on a Pro Micro, against 1.0/s without it). Default y
  since 2026-11-13 (see Changed). See ADR 0022.
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

- **`ant_compare.py` and `ant_sens.py` mark 0x60 upper bounds.** When
  `ant_verify` scopes a 0x60 figure as an upper bound (a host refill counted a
  slot that was never sent), the loss basis now reads `exact_upper_bound` /
  `event_counter (upper bound)` rather than plain exact. The compare report
  shows `ub` in the multi table and adds a caveat. `ant_features.py
  --burst-filter` checks that filtered burst completions still free the
  bridge's burst buffer.
- **`apps/dongle_ti` can build the 0xF6 health counters** (`ti_health.conf`),
  the one way to read `sched_denied` off a LaunchXL whose log pins are unattached.

- **`conformance-cc26xx.antser` re-recorded on a CC2652R LaunchXL (2026-11-12)**
  and compares equal to the nRF transcript again. It was the first hardware
  check of the TI serial number: the reply carries the low four bytes of the
  chip's factory IEEE MAC (FCFG1 `MAC_15_4_0`), confirmed by a JTAG read.
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
- **A pending acknowledged command no longer outlives the channel's search.**
  When a slave channel drops to search, its waiting command fails right away
  (after RX_FAIL_GO_TO_SEARCH, as a Garmin USB-m orders them) instead of
  blocking the host's next command until the deadline. A command submitted
  while the channel searches is sent from the frame that reacquires it. New
  0xF7 page 13 counts both, beside the other transfer outcomes.
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

- **A master skipped its next slot after every acknowledged message.** The
  slave lost one broadcast per `0x4F` (Garmin `rx_fail` 100 per 100 messages
  before, 6 after). Bursts were not affected.
  `archive/benchmarks/2026-11-12-master-slot-skip-fix/`.
- **cc26xx coex arms refused every op chained from an early-stopped window**
  (ACK replies, next burst packet): the cancelled window still held its
  reserved time. One refusal per acked reception before, none after.
- **LaunchXL immediate logging corrupted the ANT UART and lost ACK replies**
  (~250 checksum failures per 120 s, 0/8 ACKs). The board file now logs
  deferred; `ti_bringup.conf` turns immediate back on.

- **A power change on an open channel reaches acknowledged data, bursts and
  acknowledgements.** The transfer engine kept the power the channel opened
  with, so `0x60` after open changed only the broadcasts. A host that set level
  0 sent its acknowledged data ~25 dB hotter (sniffed). That asymmetry made a
  weak return link look like a master that misses acknowledgements: with both
  ends really at 0 dBm or more, master acked data fails 0-2 %
  (`archive/benchmarks/2026-11-12-master-acked-power/`).
- **A slave's broadcast reaches its master.** `MESG_BROADCAST_DATA` on a slave
  channel used to return success and transmit nothing. It now goes out once,
  in the master's next reply window (control byte `0x02`, +2190 µs, both
  measured from a Garmin slave first), and raises `EVENT_TX`. RX-only and
  secured slave channels still do not transmit it.
- **A RadiANT master's bursts and acknowledged messages complete on air.**
  None ever had: a slave answers a master's slot-opening packet on its
  slot-reply turnaround (~2190 µs, a Garmin at +2209 µs), and the master only
  listened until +1810 µs. And every burst packet re-anchored the master's
  period, moving its broadcasts by the burst's length until the slave lost it.
  Both fixed and checked with a sniffer against a Garmin USB-m.
- **Burst packets are retried.** A multi-packet burst used to fail at the
  first missed acknowledgement, so in a room with a few percent loss most long
  bursts failed. Each packet is now retried up to five times with the same
  sequence bit, 3110 µs apart, before `EVENT_TRANSFER_TX_FAILED` (Rev 5.1
  §5.4.3); a slave's first packet waits for the master's next slot. A
  single-packet burst and acknowledged data are not retried, as the spec says.
  Not on MPSL builds, where multi-packet bursts do not run yet.
- **A burst survives the receiver missing a packet.** A receiver that misses a
  burst packet repeats its acknowledgement of the previous one (a Garmin
  answered `0x82` with `0xC2`). RadiANT treated that as a bad acknowledgement
  and failed the burst without retrying. It now retries as it would after an
  empty window. Both failures in the 2026-11-11 retry A/B were this.
- **Advanced burst no longer advertises frequency hopping.** The capabilities
  form of `0x78` claimed it, but nothing in RadiANT hops. The supported-modes
  byte is now `0x00`.
- **After a transfer, a master's next broadcast repeats the last packet it
  sent.** It used to go back to the host's previous broadcast page. Rev 5.1
  p.109 (and our own Spike B captures) show the master repeating the last
  acknowledged or burst packet until the host writes a new page.
- **Only the first packet of a received burst carries extended data.** With
  lib config on, every burst packet carried the channel ID tail; Rev 5.1 §7.1.1
  puts it on the first message only, and a host parsing by the spec misread
  the rest. Single-packet acknowledged data keeps its tail.
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
- After each acknowledged message it sends, a RadiANT master skips its next
  broadcast slot (TODO.md).
