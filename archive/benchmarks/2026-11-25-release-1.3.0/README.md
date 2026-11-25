# 2026-11-25: v1.3.0 release bench (Phase D)

Release candidate: local master **817c301** (unpushed). Baseline: **b28680f**
(master before this release's fixes). Old release: tag **v1.2.0** (69ac822).
Windows 11 host. Device under test: Pro Micro / nice!nano clone
`EE6986854FD72803` (UF2 bootloader 0.6.0, S140 6.1.1, has the 32 kHz crystal).
Same-window control receiver: TI LaunchXL on COM11 at 57600, running its
shipping d94d07e image (receive only, not reflashed). Master/transmitter:
Garmin ANT USB-m `006`. The L15 and 5340 DKs were not attached and not touched.
No Feather flashes.

Figures are in `results_summary.json`. Per-run JSON and transcripts are in
`raw/`, the images and their `.config` files are in `images/`, and the
build_all transcripts are in `buildlogs/`. All of these stay local under the
archive .gitignore rules. They rebuild from the commits named here.

## Images (D1)

Each was built in a clean hand-made worktree with that tree's own
`scripts/build_all.ps1` (NCS v3.4.0, `-Backend core`, nrf HAL), one `-Only
<artifact>` per row.

| image | commit | sha256 |
|---|---|---|
| radiant_dongle.uf2 | 817c301 | 8d5b9e12ff46741eef51c5f8b608dc6cea076244477724202cea9e3abcdae2d0 |
| radiant_dongle_feather_quiet.uf2 | 817c301 | 483f1e3bc684431d0452cbbfab28439b2e8015ebbed0059756def8cca385fef0 |
| radiant_dongle_nrf52840dongle.zip | 817c301 | 94e000ed0da0a175b9e679bf889d757369ddfedfab3d51c8f6cd71ad1dcc10e3 |
| radiant_dongle_promicro.uf2 | 817c301 | f03f2ac8470ccd7ac4642f07ad58aff6b8714c2ed755415480a41997285416b7 |
| radiant_dongle_promicro_synth.uf2 | 817c301 | 3f59df6a37c787a387731677c44d568a91a6fe03c65a8a848ecaf3f773c42194 |
| radiant_dongle_promicro.uf2 (baseline) | b28680f | be54c354a8e2d1a8e67ba3320c78d3139a7fe6c143e6e80128ace07ccf15c3e4 |
| radiant_dongle_promicro.uf2 (control) | v1.2.0 + reboot backport | acc0ac4bf0810690f6fccd69e5cb360b38177234fba9ca54e399057d8834ee4a |

- All five 817c301 release rows passed build_all's own per-row checks: link
  address, transport, radio backend, the dongle zip's 0x1000 start, and
  `assert_config` (the Pro Micro rows assert `CONFIG_ANT_DONGLE_UF2_REBOOT_REQUEST=y`).
- All three Pro Micro `.config`s read `CONFIG_ANT_DONGLE_UF2_REBOOT_REQUEST=y`,
  `CONFIG_ANT_DONGLE_RADIO_CORE=y`, `CONFIG_RADIANT_BACKEND_NRF=y` and
  `CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y`.
- Before each copy, every UF2 was checked for target address 0x26000 and
  family id 0xADA52840.
- Every reflash went through `ant_probe.py --reboot-to-bootloader`. No RST
  double-tap was needed.

### The v1.2.0+reboot control image

v1.2.0 predates the USB reboot feature entirely: 78dd92c added it and 680239e
enabled it for the Pro Micro, both after the tag. So the backport (local
commit 3a3a2ad in a throwaway worktree; `v120_reboot_backport.diff`) carries:

- from **78dd92c**: `apps/common/usb_ant_class.c` (a vendor control-request
  handler on EP0 that writes GPREGRET 0x57 and reboots; the existing MS OS
  descriptor handler is regrouped under `#if` around it), the
  `ANT_DONGLE_UF2_REBOOT_REQUEST` Kconfig symbol, and `apps/dongle/uf2_reboot.conf`;
- from **680239e**: the `CONFIG_ANT_DONGLE_UF2_REBOOT_REQUEST=y` line in
  `apps/dongle/boards/promicro_nrf52840_nrf52840_uf2.conf`, the
  `uf2_reboot.conf` comment change, and the build_all.ps1 `assert_config` row
  field and check.

Nothing in the bulk-OUT path (`ep_out_cb`) or the host-frame parser was
touched. The control keeps v1.2.0's host-frame parsing byte for byte. The
CHANGELOG, README, release.yml, ant_probe and test changes were left out.

## D2: Windows mid-frame retest

Tool: `tools/bench_midframe_reopen.py` from 817c301, `--count 10`, random
0.05–0.75 s delays. A child process writes the first N bytes of a 6-byte
capabilities request (`a4 02 4d 00 54 bf`) and dies with `os._exit`. The
parent then reopens and sends the whole request. Every run's pre-check, a
request with no partial frame in front of it, was answered in 0.5–0.6 ms.

| child writes | v1.2.0+reboot | 817c301 |
|---|---|---|
| 4 bytes `a4024d00` (tool default) | **0/10** answered | **0/10** answered |
| 5 bytes `a4024d0054` | 0/10 | **10/10** |
| 6 bytes, the whole frame (control arm) | 10/10 | 10/10 |

**Verdict: the known issue stays.** The candidate does not pass 10/10 at the
default cut, so the drop rule is not met. The test does reproduce the bug: the
control fails 10/10. The fix covers part of it.

- 817c301's parser change (a failed checksum byte of `0xA4` starts the next
  frame) fixes exactly the case where the new SYNC lands in the checksum
  position (the 5-byte cut: 0/10 to 10/10).
- When the new SYNC lands inside the stale frame's payload (the 4-byte cut,
  LEN 2), the reopened host's first command is still swallowed on both images.
  The parser has no inter-byte timeout, and nothing else drops a stale partial
  frame.

Suggested CHANGELOG rewording: "On Windows, a host tool that dies mid-frame
can still have the next tool's first command swallowed when the stale frame
had not reached its checksum byte; a bus reset, a replug or a resend clears
it. Measured 2026-11-25: 0/10 answered at a 4-byte cut on both v1.2.0 and
1.3.0; at a 5-byte cut 1.3.0 is fixed (10/10, v1.2.0 0/10)."

Not measured: whether the second command after the swallowed one is answered.
Every iteration plants a fresh partial frame, so the tool cannot see recovery.

## D3: acquiring on an exchange opener, on air

Tool: `tools/bench_acquire_opener.py`. It is new, committed on branch
`wip/s25-bench-tools` (5cb28a0, off 817c301), and not merged.

- **Master:** the Garmin, an FE-C type 17 bidirectional master, device 24681,
  0x60 level 3 (0 dBm). It re-queues an acknowledged message, or a 2-packet
  burst, on every terminal event. Each transfer carries a running index in
  data byte 0.
- **Slave:** the Pro Micro, a bidirectional slave pinned to the same channel
  id. It was opened 20 times per arm, with a random 0.3–1.5 s delay between
  close and reopen.
- **Recorded per open:** the first data message for the channel and the
  master's terminal event for that same transfer index.
- **Opener on air every period:** the master emitted 0 plain-broadcast
  `EVENT_TX` slots during the acked runs. In the burst runs it emitted 3, all
  in the first two seconds before the first open, because the first burst was
  refused with CHANNEL_NOT_OPENED right after open and was re-queued.
- **Acquisition phase spread:** first frames arrived 8–480 ms after the open,
  so the acquisition phase was spread across the period.

| image | master opener | first host message / master outcome for that transfer |
|---|---|---|
| b28680f | acked (0xAA) | **BROADCAST_DATA / TX_FAILED: 20/20** |
| 817c301 | acked (0xAA) | **ACKNOWLEDGED_DATA / TX_COMPLETED: 20/20** |
| b28680f | 2-packet burst (0x8A) | **BROADCAST_DATA / TX_FAILED: 20/20** |
| 817c301 | 2-packet burst (0x8A) | **BURST_DATA (seq 0, then seq 1 last) / TX_COMPLETED: 20/20** |

**Verdict: PASS, and the change is confirmed on air.**

- On the baseline the acquiring frame was posted as BROADCAST_DATA and never
  ACKed. In burst mode, packet 2 of that burst was also lost to the host:
  the next message was the following burst's seq 0. Later transfers were
  delivered and ACKed normally.
- On the candidate, the acquiring transfer was delivered with its real kind
  and ACKed 40/40. The master's TX_COMPLETED arrived 2.2–4.1 ms (acked) and
  3.8–5.8 ms (burst) after the slave's host message.
- No dropped openers were observed in 40 candidate opens. A drop would show
  as the first message being the next index; none did.

## D4: regression, candidate vs baseline

**Room.** No real sensors were on the air: a 40 s untracked scan by both
receivers found nothing. The Garmin therefore ran `tools/ant_sim.py` as the
source, at its default power (level 3, 0 dBm):

| channel | device | role in the test |
|---|---|---|
| power | 23571:0x0B | tracked |
| heart rate | 23572:0x78 | tracked |
| CSC | 23573:0x79 | untracked, the scan-rate target |

`ant_scan_rate.py`: Pro Micro and TI in the same 300 s window, `--reopen 4.5`
(66 opens), both `--track`s. One window per Pro Micro image, about 9 minutes
apart. Each window's TI figure is its own control.

| window | receiver | loss 23571:0x0B | loss 23572:0x78 | 23573 scan: per s, heard/opens, median and max first sighting |
|---|---|---|---|---|
| b28680f | Pro Micro | 3.50 % (42/1201) | 3.45 % (42/1218) | 2.60/s, 66/66, 0.26 s, 0.98 s |
| b28680f | TI (control) | 3.66 % | 3.30 % | 2.58/s, 66/66, 0.25 s, 0.72 s |
| 817c301 | Pro Micro | 3.75 % (45/1200) | 3.37 % (41/1218) | 2.61/s, 66/66, 0.23 s, 0.59 s |
| 817c301 | TI (control) | 3.57 % | 3.54 % | 2.58/s, 66/66, 0.24 s, 0.61 s |

**Verdict: no regression.**

- The Pro Micro minus TI loss difference is within ±0.2 pp on every channel
  in both windows (b28680f: -0.16 and +0.15 pp; 817c301: +0.18 and -0.17 pp).
  At ~1200 packets that is about two packets.
- The scan rate and time to first sighting match both the TI control and each
  other across the two windows.
- Caveats:
  - ant_sim reported "EVENT_TX pacing did not hold" in both windows. About 2 %
    of payloads went out on its wall-clock fallback after a slow first
    EVENT_TX. That affects payload freshness only; loss is counted at the
    receivers.
  - 23573:0x79 trips ADR 0019's alternating-run warning. It affects both
    receivers and both windows alike.
  - An unrelated GPU training job was running on the host throughout.
  - An earlier two-profile baseline window is kept in
    `raw/d4_first_try_2profiles/`. It had no untracked device, so it gave no
    scan figure. Pro Micro 2.00/2.30 %, TI 1.79/2.21 %.

**`ant_probe.py` on the candidate Pro Micro: PASS.** It reports RADIANT0.01B00,
32 channels, 3 networks, and serial 0x4FD72803, which matches the USB iSerial
tail.

**`ant_zwift_visible.py --dll`: PASS.**

- **DLL used:** a scratch copy of Zwift's `ANT_DLL.dll` (sha256 db4d99c5…),
  alone in the session scratchpad, with no shim DLLs beside it. ANT_DLL
  therefore took its libusb0 raw-bulk path. `RADIANT_SHIM_LOG` was pointed at
  a scratch file. The installed shim and Zwift's directory were not touched.
- **Which stick it opened:** the Garmin shares the PID, so ANT_DLL device 0
  is ambiguous. The run was repeated with the Garmin held open by a 45 s
  `ant_sim` process:
  - device 0 (the busy Garmin): `ANT_Init -> False`;
  - device 1: `ANT_Init -> True`, `ANT_ResetSystem -> True`, PASS. That is
    the Pro Micro.

## Not run

- D2's recovery after the swallowed command was not measured (see D2).
- The burst opener used 2-packet bursts only. Longer bursts and
  advanced-burst openers were not tried.
- No sniffer capture of the ACK on air. TX_COMPLETED at the Garmin is the
  evidence that the ACK arrived.
- No real-sensor tracked loss: the room had none.

## Board state at the end

- **Pro Micro `EE6986854FD72803`:** 817c301 `radiant_dongle_promicro.uf2`
  (sha256 f03f2ac8…416b7). It carries the USB reboot path. Probe PASS after
  the final flash.
- **TI LaunchXL COM11:** unchanged (shipping d94d07e image). It was used
  receive-only.
- **nRF54L15 DK and nRF5340 DK:** not attached, not touched.
- **Garmin `006`:** attached.
