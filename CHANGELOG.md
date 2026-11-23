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
  remove any `99-ant-usb.rules`). The bridge and Matter builds link again.

### Security

Except the last bullet, these apply only to builds with the opt-in
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

### Tooling and CI

- Releases come from one tag-only `publish` job that checks every image name
  and `.sha256` sidecar against the matrix; actions, nrfutil and the Matter
  gate's inputs are pinned by digest. Also: 0xF7 pages 8-13 (ADR 0023),
  `ant_scan_rate.py`, `decode_pcap.py` slot loss, Zwift shim logging and a TI
  backend ztest harness.

### Known issues

- MPSL builds cannot run multi-packet bursts or burst retries.
- On MPSL a scan beside several tracked channels can be slow (one Zwift
  observation: 3 frames in 3.66 s with three pinned channels); not measured.
- The MQTT sink has never run against a real broker.
- No image has a watchdog.
- On Windows, a host tool that dies mid-frame may still have the next tool's
  first command swallowed; a bus reset or replug clears it. Probably fixed by
  the host-frame parser change above, but not re-tested.

## [1.2.0] - 2026-11-06

### Added

- **Bridge product, `apps/dongle_thread`:** self channels 6 and 7 pinned to
  paired sensors on the Zwift model, restored after `SYSTEM_RESET`; MQTT and
  Matter sinks; the Matter bridge fits an nRF54L15 and an nRF52840.
- **Reference designs:** `apps/treadmill` (FE-C, RSC, FTMS;
  [ADR 0017](docs/decisions/0017-fec-treadmill-control.md)), `apps/hrm_ble`
  (BLE heart-rate strap that also transmits ANT+) and `apps/trainer`, a
  trainer simulator with ACK fault injection.
- **More hardware:** the nRF5340 network core as an ANT+ endpoint, and the TI
  CC2652P dongle (`apps/dongle_ti`) over its own USB.
- **Acknowledged data works on MPSL builds.**
- **`MESG_RADIANT_HEALTH` (0xF6)** counters for silent drops.
- **Opt-in RadiANT security (`security.conf`, in no release image):**
  `X_AUTH`/`X_CONF`, X25519 pairing and `0xF1`-`0xF4`. A secured channel does
  not interoperate with ANT+ profiles.

### Changed

- **Release artifacts are cut from the clean-room backend;** sdk-ant is out of
  CI ([ADR 0001](docs/decisions/0001-backend-selection-and-release-default.md))
  and every app defaults to a real radio backend.
- **A pending acknowledged command gets three master slots** (750 ms on FE-C).
- **A named device is revisited during search,** so one lost packet costs
  0.25-2 s instead of an 8 s sweep.
- **The MQTT sink does no network I/O on the self or sample path.**
- **The coded PHY is opt-in,** cutting the arm lead to 168 µs (nRF54L15) and
  328 µs (nRF52840) ([ADR 0007](docs/decisions/0007-long-range-phy.md)).
- Bridge max channels sit below the self channels; the USB write is bounded at
  1 s; one Feather image carries the RGB indicator.

### Fixed

- **Matter pressure was 10× and 1000× too small.** Page 84's
  operating-temperature limits are no longer posted; existing Home Assistant
  installs must delete the two retained discovery topics by hand.
- **A USB bus reset mid-frame swallowed the host's next command.**
- **An MPSL assert on bridge and Matter images reboots** instead of halting.
- **A search cost a tracked channel every other packet.**
- **Zwift:** an assign to a keyless network no longer stops discovery after
  the first sensor; two tracked channels within one arm lead no longer lose
  the later one ([ADR 0016](docs/decisions/0016-merge-reach-is-the-arm-lead.md)).
- DFU packages carry the real version; `LICENSE` is Apache-2.0.

### Security

Both apply only to `security.conf` builds.

- A keyless injector could replay or desynchronise a secured channel.
- Under `DROP_UNVER`, forgeries were delivered as verified.
