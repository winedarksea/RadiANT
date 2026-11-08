# 2026-11-07: the 54L15 DK's VCOM death (TODO.md item 3)

Rig: `rig.toml` (copy of `tools/compare_rigs/2026-11-07-vcom-soak.toml`). The
Feather ran ant_sim with four profiles (power, HR, CSC, FE-C). The Garmin
(reference), the nRF52840 Dongle and the 54L15 DK received. There were ten
300 s multi windows, 17:05-17:58. The 54L15 ran this change set (boot
STARTUP, reset-cause log), and its console (COM3) was captured with
`log_port`. The plan had the nRF5340 DK transmitting; it was in use by
another agent, so the Feather did.

## Result: one drop in ten windows, and it is not the firmware

Window 3 (`S_multi_3_54L15_console.log` holds the console capture, and
`summary.json` the window record):

- **What happened:** the 54L15's ANT port (COM4, J-Link OB interface MI_00)
  died at 204 s with `SerialException: Cannot configure port ...
  PermissionError(13, 'A device attached to the system is not functioning.',
  None, 31)`. That is the same error as the original. ant_verify wrote
  partial JSON with `port_death`, and the per-receiver `wall_s` (205 s)
  tripped the dropped-port check. Under the old in-order wait that check was
  disarmed.
- **The firmware did not reboot:** no unsolicited STARTUP arrived
  (`receiver_reboots: []`). This image announces every boot on that port.
- **The firmware kept working:** the console ran on through the death with
  continuous uptime (00:30:56 → 00:35:26), no boot banner and no fault.
  `radiant_api: rxpath` kept counting about 480 tracked and posted messages
  per 30 s, which is 4 channels × 4 Hz, until the window closed.
- **The console VCOM on the same probe never failed:** COM3 is MI_02 on the
  same J-Link OB.
- **The probe did not re-enumerate:** `DEVPKEY_Device_LastArrivalDate` on
  the probe and on both VCOM interfaces still read 00:01:12 that day. As a
  check that this property moves, the Dongle's moved with its DFU
  re-enumeration. Kernel-PnP/Configuration is NOT usable for this: it logs
  only first-time device configuration and shows no event for any
  re-enumeration that afternoon.
- **It recovered by itself:** window 4 reopened COM4 about 2 minutes later,
  and the other nine windows ran their full 301 s.

**Classification:** a probe-side fault, confined to the J-Link OB's ANT CDC
interface (or the driver above it). The device stays on the bus and the
interface briefly fails requests with ERROR_GEN_FAILURE.

## A suspected trigger that the data does not support

"Cannot configure port" is raised from `SetCommState`, not from a read.
`tools/ant_probe.SerialDevice.read` assigned `port.timeout` on every call,
and in pyserial 3.5 each assignment is SetCommTimeouts + GetCommState +
SetCommState, which are control requests to the probe. ant_verify did that
about 20 times a second. The console port, configured once, never failed.

A bare stress test on COM4 with the firmware idle (no traffic, 1-byte reads
with a 1 ms timeout) gave:

| arm | what each read does | ran | operations | died |
|---|---|---|---|---|
| reconfigure | reassigns the timeout | 46 s | 1472 | **yes, the identical error** |
| steady | timeout set once | 180 s | 11726 | no |
| reconfigure | reassigns the timeout | 180 s | 5766 | no |
| reconfigure, 5340 DK probe (COM5) | reassigns the timeout | 180 s | 5787 | no |
| reconfigure | reassigns the timeout | 600 s | 19242 | no |
| steady | timeout set once | 600 s | 38497 | no |

**This does not support the reconfigure trigger.** The reconfigure arm ran
about 17 minutes and about 27,000 operations in total and died once. At the
soak's own rate, one death per ~50 minutes of use, about 0.3 deaths would be
expected by chance in that time, so the one fast death fits coincidence. The
trigger is unidentified; on this evidence the death looks random in time, at
roughly one per 20-50 minutes of use.

`SerialDevice.read` now assigns the timeout only when it changes. It is kept
because it removes thousands of pointless control requests per window, not
because it is a fix. Do not read a later clean soak as proof that it fixed
anything without an A/B.

Mitigations if a run must not lose a window: a different USB port or hub for
the DK, or a shorter window. Partial JSON already keeps a dead window's data
up to the moment of death. Never attempt the J-Link OB firmware update (see
the J-Link notes in docs/bench-runbook.md and the hardware-bench memory).
