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

- **ANT+ Bike Radar (device type 0x28).** Codecs and a radar master
  (`radiant_profile_radar.c`), a bridge adapter publishing nearest-target
  range, closing speed, side, threat, target count and availability with the
  profile's 2 s target timeout (opt-in, `CONFIG_ANT_DONGLE_RADAR`), the Python
  mirror, and `ant_sim.py --profile radar` scenarios. Cross-checked on air
  against a Varia RTL510, RTL515 and RearVue 820; a Garmin Edge paired with the
  simulated radar and showed its vehicles. Telemetry only, not a certified
  radar display.
- **Capture tools.** `tools/ant_record.py` (scan or pinned channels, message
  kind, RSSI, receiver timestamps, scheduled acked sends, walk-test marks),
  `.antcap` v2 (kind and RSSI columns; v1 files still read), `ant_sim.py
  --replay`, and `tools/promisc_log.py` for the promiscuous sniffer, which
  now takes its device numbers from the build line.

### Fixed

- **Acknowledged messages to Garmin radars were always reported failed.** The
  radars acknowledge with control byte `0xF3`, and RadiANT's frame decoder
  required bits 2:0 to be `010`. Receiving now ignores bit 0; what RadiANT
  transmits is unchanged. Before: 0 of 24 completed; after: 6 of 6.

## [1.3.0] - 2026-11-25

### Added

- **Inbound multi-packet bursts.** A burst sent to RadiANT used to stop at
  packet 0. The receiver now listens for each next packet, drops duplicates,
  repeats its ACK into an empty window (`CONFIG_RADIANT_INBOUND_ACK_REPEATS`)
  and raises `EVENT_TRANSFER_RX_FAILED` (0x04); outbound bursts raise
  `EVENT_TRANSFER_TX_START` (0x0A). Garmin to RadiANT: 96-98 of 100 bursts of
  8 complete. Not on MPSL builds.
- **The dongle reports a serial number.** `0x61` and `0xC6` are answered in a
  Garmin stick's shapes (nRF: from the USB iSerial; TI: from the factory IEEE
  MAC) and capabilities advertise it again. Zwift asks for both: its reset to
  first `NETWORK_KEY` went from 42-94 s to 11-15 s.
- **A quiet Feather image, `radiant_dongle_feather_quiet.uf2`,** with no LED
  activity. A tag now attaches five images.
- **Pro Micro images reboot into their UF2 bootloader over USB**
  (`tools/ant_probe.py --reboot-to-bootloader`). Any host software that can
  open the device can now put these boards into a writable bootloader.
- **RadiANT is usable from other firmware:** the `radiant_ant_*` API
  (`<radiant/radiant_ant.h>`, `CONFIG_RADIANT_ANT_API`) lives in the `radiant`
  module with heart-rate samples, and builds on stock Zephyr. Every public name
  is `radiant_*`/`RADIANT_*`, and a west project entry is all a project needs:
  the repository root is a Zephyr module ([radiant/README.md](radiant/README.md)).
  Every header is `<radiant/...>`, profiles and the bridge included.
  **Renamed since v1.2.0** (and since master before 2026-11-22): `antr_` is
  `radiant_ant_`, `ANTW_` is `RADIANT_WIRE_`, exported `profile_`/`node_`
  names are `radiant_profile_`/`radiant_node_`, the bridge's `MATTER_*`
  macros are `RADIANT_MATTER_*`, and `#include "profile_*.h"` or
  `"radiant_bridge.h"` is `#include <radiant/radiant_profile_hr.h>` or
  `<radiant/radiant_bridge.h>`. Run `scripts/migrate_radiant_names.py <your
  dirs>` to rewrite your code.
- **Low-power options for battery nodes, default n**
  ([ADR 0025](docs/decisions/0025-power-diag-struct-and-lp-timebase.md)):
  `CONFIG_RADIANT_NRF_LOW_POWER` releases the HFXO and radio timer between
  slots (needs an LF crystal); `CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE` is the
  TI equivalent, refused in dongle images. `lowpower.conf` in
  `radiant/samples/hr_transmitter` and `apps/hrm_ble` turns them on. Estimated
  nRF54L15 idle ~811 to ~36-41 µA (software proxy; no meter on the bench).

### Changed

- **Treadmill machine range is transport-neutral.** Removed
  `CONFIG_TREADMILL_BLE_MAX_SPEED_CENTI_KPH`,
  `CONFIG_TREADMILL_BLE_MIN_INCLINE_DECI` and
  `CONFIG_TREADMILL_BLE_MAX_INCLINE_DECI`; set
  `CONFIG_TREADMILL_MAX_SPEED_CENTI_KPH`, `CONFIG_TREADMILL_MIN_INCLINE_DECI`
  and `CONFIG_TREADMILL_MAX_INCLINE_DECI` instead (same defaults, no BLE
  dependency). With BLE on they still feed FTMS's Supported Range values.
- **The nRF backend no longer needs a board overlay.** Without a
  `radiant,radio-timer` chosen node it uses TIMER4 (nRF52), TIMER20 (nRF54L)
  or TIMER2 (nRF5340 network core), and stops the build if the board already
  enables that TIMER. A chosen node still wins; existing images are unchanged.
- **Wildcard search is on by default on nRF and TI**
  ([ADR 0022](docs/decisions/0022-search-matches-a-wildcard-device-number.md)):
  the sweep is one set, and discovery went from about 0.5-1 to 4-12
  sightings/s (Garmin parity). In Zwift every untracked sensor listed in
  0.09-0.54 s with three channels tracked. `long_match.conf` /
  `ti_long_match.conf` build the old 24-bit match.
- **MPSL builds book tracked slots a period ahead in a second timeslot
  session** ([ADR 0021](docs/decisions/0021-tracked-slots-are-booked-a-period-ahead.md),
  resolving ADR 0018). Two tracked sensors 5 ms apart lose 0.7 % / 4.0 % of
  slots on `gate.conf` instead of 36 % / 68 %, and a search no longer starves
  a tracked channel.
- **A contended tracked slot alternates and raises `EVENT_CHANNEL_COLLISION`
  (0x09), not `RX_FAIL`**
  ([ADR 0024](docs/decisions/0024-contended-slots-alternate-and-are-collisions.md)),
  so self-collision cannot send a channel to search. Since 2026-11-21 a slot
  the arbiter lends to another stack (MPSL beside BLE or Thread) raises 0x09
  too instead of passing silently. A denied master transmit stays silent.
- **CRC repair is on in the shipping dongles,** now that it works on cc26xx
  and repaired broadcasts reach the host (until 2026-11-18 they were dropped
  at the last gate). 0 mis-repairs on the bench; `crc_repair_off.conf` builds
  without it. The radio hot path is also compiled at -O2 (+4.9 KB flash).
- **Damaged host frames:** LEN 39-255 is dropped silently and counted in 0xF6
  `host_desync` instead of being answered truncated, and a failed checksum
  byte of `0xA4` starts the next frame
  ([docs/ant-serial-protocol.md](docs/ant-serial-protocol.md)).
- **0xF6 is 34 bytes** (`uart_rx_overruns`); an unmeasurable field reads
  `0xFFFF`. v1.2.0's `ant_health.py` shows that as
  `sets_advanced_uncovered = 65535`, meaning "not measured".
- **A held acknowledged command no longer outlives the channel's search:** it
  fails right after `RX_FAIL_GO_TO_SEARCH`, as on a Garmin, and a command sent
  while searching goes out on the reacquiring frame.
- **The heart-rate node transmits ANT+ and BLE together by default;**
  suppression is opt-in with `ble.conf;ble_suppress.conf` (was
  `ble_nosuppress.conf`). Not yet run on air.
- **Nodes generate their identity key on first boot**
  ([ADR 0020](docs/decisions/0020-k-dev-is-generated-on-first-boot.md)): each
  unit gets its own device number instead of 4242/4243; receivers re-pair once.
- **Transmit power:** an explicit level 3 is a true 0 dBm; the boost goes to
  +8 dBm on nRF54L15 and +5 on CC26xx. UART builds send STARTUP with the reset
  cause at every boot.
- **Capabilities claim less** (search sharing, selective data update,
  encrypted channel, advanced-burst hopping); the commands are still accepted.
  The sweep follows the ANT+ key's network and keyed searchers' RF.

### Fixed

- **A slave that acquires on acknowledged data or a burst delivers it as
  that.** The search posted a master's `0xAA`/`0x8A` opener to the host as
  `BROADCAST_DATA`, unacknowledged. It now goes up as `ACKNOWLEDGED_DATA` or
  `BURST_DATA` sequence 0, and a bidirectional channel ACKs it at +2190 µs;
  when that slot can no longer be reached the message is dropped
  unacknowledged, so the master resends it. A background scan reports
  acknowledged and burst messages as such (Rev 5.1 9.5.4.5) and no longer
  posts ACKs; its burst sequence comes from one bit, so it reads 0,1,2,1,2.
  `0x2A`/`0x4A`/`0x6A` no longer claim or re-anchor a channel.
- **A tracked frame that does not decode is a miss,** not the sensor heard:
  one with a matching address and CRC but an unknown control byte used to
  keep a lost sensor tracked. It now raises `RX_FAIL` and, repeated, sends
  the channel to search.
- **Treadmill: FTMS Stop and Reset report a deck that refused.** Stop answers
  failure and keeps Training Status when the belt refuses speed 0; Reset
  releases control and notifies only when speed and incline both reset.
- **Treadmill: any ANT+ transmitter could command the deck past the
  machine's range.** FE-C page 51 was bounded only by the wire (±100 %) on
  every build, and FE-C-only builds had no range at all. Every command path
  is now held to the machine range: a simulation grade (page 51, the BLE grade
  seam) clamps and answers PASS, and an explicit FTMS incline or speed outside
  the range is refused. The FE-C grade decoder also stops at +200.00 %
  instead of reading out-of-spec raw values as up to +455 %.

- **A Matter sensor is no longer dropped when its endpoint id is taken.**
  The bridge retried with the next id but had already discarded the device,
  so the retry failed. Inherited from NCS's `matter_bridge`.

- **MPSL builds no longer assert `69:108`.** MPSL took a RADIO interrupt left
  pending after our timeslot ended. Beside a Thread leader it had fired within
  1.5-6.3 minutes; a 2 h soak and the Matter, gate and BLE legs had none.
- **A RadiANT master's bursts and acknowledged messages complete on air:** it
  listens for the reply at +2190 µs, keeps its period across a burst, no longer
  skips the slot after an acknowledged message, and retries burst packets up
  to five times. A power change after open applies to transfers too.
- **As a slave, RadiANT answers a master's `0xAA` and `0x8A` at +2190 µs**
  (at +1560 a Garmin master saw every message fail), and a slave's
  `MESG_BROADCAST_DATA` now reaches the master instead of being dropped.
- **`SYSTEM_RESET` is answered in 2.6-5.4 ms, not ~52,** off libusb-win32's
  short-read race (5 of 1000 STARTUPs lost at a 50 ms read; now 0).
- **TI:** the UART FIFO is on, so the dongle no longer stops answering host
  commands under load; every channel listened on 2457 MHz whatever its RF,
  and now retunes; receiver IF 1.0 MHz (loss had depended on the
  transmitter's carrier offset, up to 87 %); transmits start 100 µs later
  (slave acknowledged data failed for whole sessions); merged windows keep
  separable sync words; transmit power rounds to the nearest entry.
- **A slave that acquires its sensor hears the next frame too** (~2 % of
  acquisitions lost frame 1). nRF: a late RADIO interrupt could pair one
  frame's CRC status with the next frame's bytes.
- **BLE heart-rate and treadmill builds lost every settings write** (MPSL
  timeslot pool one short) and fell back to device number 4242.
- **Host commands follow Rev 5.1:** `0x6E` replaces instead of OR-ing; `0x79`
  takes the filler byte and filters; `0x5B` keeps channel 0's setup; a
  receive-only slave never transmits; only a burst's first packet carries
  extended data.
- **Matter: an endpoint id is never reused for a different sensor** (persisted
  high-water mark; a failed reservation returns its provider slot; an
  unreadable mark refuses new endpoints). Stored lengths are bounds-checked.
- **Scheduler, backends, bridges:** a never-terminating operation is
  reclaimed, a late compare is refused rather than hanging, an ETIME retry uses
  fresh time, an MPSL staged operation survives `EXTEND_FAILED`; Matter label
  changes and owed MQTT offlines survive a flush or outage.
- **The Linux udev rule grants access** (`host/linux/70-radiant-ant.rules`;
  remove any `99-ant-usb.rules`), unbinds `usb_serial_simple` from
  `0x1008`/`0x1004` sticks too, and tells ModemManager to leave them alone.
  The bridge and Matter builds link again.
- **`ant_verify.py` fails a replay with no sensor streams** instead of
  passing it.

### Security

Except the last two bullets, these apply only to builds with the opt-in
`security.conf`; none of the five release images has it.

- **A secured channel stopped verifying within minutes when it carried
  anything else;** the sender's counter is now its slot index.
- **No plaintext or unjudged leaks:** acquiring and scan frames pass the
  receive hook; a secured master never acknowledges in plaintext; a `0xF1`
  enabling `X_CONF` withdraws the slot already built; `X_CONF` alone encrypts
  byte [7]; a failed key derivation stops `X_AUTH` transmits; `0x5B` keeps keys.
- **Replay and clock:** a signed epoch anchor; replay floors that survive
  configuration retries and cover Tier II; queued ciphertext dropped on an
  epoch or policy change; compat rekeys cannot roll back; over-air command
  tags bind a 16-bit sequence.
- **Pairing and provisioning:** nonces committed before the fingerprint;
  X25519 fits the 2 KB serial stack, with a constant-time swap; unassign ends an
  exchange; enrolment delivers the group root; provisioning is safe to retry.
- **Protocol:** opt-in `0xF8` window-verdict event; `0xF3` is write-only
  ([docs/radiant-security.md](docs/radiant-security.md)).
- **Key material is redacted from diagnostic traces** in any build with
  tracing on, and from the Zwift shim's level-2 log.
- **Release trust order:** a read-only `verify-tag` job checks that the tagged
  commit is on `master` and is the commit the run built before any repository
  script runs, and `publish` re-checks the tag on GitHub before attesting. A
  tag run uses the tagged commit's own workflow, so the `v*` tag ruleset and
  the `release` environment reviewer are the real control. The documented
  `gh attestation verify` now pins the signer workflow and source ref
  ([SECURITY.md](SECURITY.md)).

### Tooling and CI

- Releases come from one tag-only `publish` job that checks every image name
  and `.sha256` sidecar against the matrix; actions, nrfutil and the Matter
  gate's inputs are pinned by digest. Also: 0xF7 pages 8-13 (ADR 0023),
  `ant_scan_rate.py`, `bench_midframe_reopen.py`, `decode_pcap.py` slot loss, Zwift shim logging and a TI
  backend ztest harness.

### Known issues

- MPSL builds cannot run multi-packet bursts or burst retries.
- On MPSL a scan beside several tracked channels can be slow (one Zwift
  observation: 3 frames in 3.66 s with three pinned channels); not measured.
- The MQTT sink has never run against a real broker.
- No image has a watchdog.
- On Windows, a host tool that dies mid-frame can still have the next tool's
  first command swallowed when the stale frame had not reached its checksum
  byte (0 of 10 answered, as on v1.2.0); a resend, bus reset or replug clears
  it. A frame cut at its checksum byte is fixed (10 of 10, v1.2.0 0 of 10).

## [1.2.0] - 2026-11-06

Bridge product (`apps/dongle_thread`, MQTT and Matter sinks), the treadmill,
BLE heart-rate and trainer reference designs, the nRF5340 network core and the
TI CC2652P dongle, acknowledged data on MPSL builds, `0xF6` health counters and
opt-in RadiANT security; release images cut from the clean-room backend. Full
entry: [`CHANGELOG.md` at v1.2.0](https://github.com/winedarksea/RadiANT/blob/v1.2.0/CHANGELOG.md).
