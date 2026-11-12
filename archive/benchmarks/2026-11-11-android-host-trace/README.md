# Android ANT stack host trace, 2026-11-11

An Android 17 tablet running Zwift, against two sticks.

## Rig

- **Command trace:** nRF5340 DK, application core, `apps/dongle` built with
  `-DANT_RADIO=core -DRADIANT_BACKEND=null "-DEXTRA_CONF_FILE=host_trace.conf"`.
  The tablet is on the DK's nRF5340 USB port and the console is on the J-Link
  VCOM (MI_02, DTR asserted). `H>` is a frame from the host and `<D` a frame
  from the dongle. Network-key payloads are masked. **Null radio:** this image
  answers every command and never hears a packet.
- **End to end:** Pro Micro (`EE6986854FD72803`), release image
  `radiant_dongle_promicro.uf2` built from the working tree with the fix below.
  The tablet is on its USB. It has no trace.
- **Reference:** Garmin ANT USB-m (`usb:006`, version `AJK2.05RAF`).
  `replay_android.py` sends it one cycle of the tablet's sequence.

## What the tablet sends (`before_fix_trace.txt`)

Nothing until Zwift asks for ANT. The ANT stack then repeats a ~12 s cycle,
starting about 2 min 35 s after USB configuration:

1. Reset (twice), network key 1 (the ANT+ key, accepted), network key 2 (an
   unknown key, refused `0x33`), the version request and both keys again,
   then capabilities, `0x78` advanced burst, and `0x74` event buffering on and
   then off.
2. Channel 0: slave on network 1, an empty exclusion list (`5A 00 00 01`),
   lib config `0x40` (RSSI), wildcard ID, RF 57, period 8192, LP search
   timeout 4, HP 0, priority 7, open.
3. About 5 s later: two version requests, then `5A 00 00 00` with the
   channel still searching, a status request, close, unassign, lib config 0
   and event buffering off.
4. Reset and both keys. The tablet then stops reading, so the key-2 reply
   sits in the IN endpoint and the dongle logs `USB TX timeout` 1 s later.
   About 4.5 s after that the cycle starts again.

## Differences from the retail stick (`replay_*.txt`)

| Command | Garmin USB-m | RadiANT before |
|---|---|---|
| `5A`/`59` ID list, channel open | `00` | `15` CHANNEL_IN_WRONG_STATE |
| `74` configure event buffer | `00` | `28` INVALID_MESSAGE |
| `4D 74` request event buffer config | `74 00 00 00 00 00 00` | `28` |
| `46` network key 2 (unknown key) | not replayed | `33` (by policy) |

**Fixed:** `5A`/`59` on an open channel (`antr_id_list_config/_add`, ztest
`test_the_id_list_is_configurable_on_an_open_channel`).

**Not the cause of the cycle.** `after_fix_trace.txt` shows the same ~12 s
cycle, but now the close follows an accepted `5A`. With a null radio the cycle
looks like the Android stack's own behaviour when a search finds nothing.
That is not proven, because this rig cannot trace a stick that does find
something.

**Left as is:** the `0x74` refusal (the tablet carries on past it) and the
network-key-2 refusal (clean-room policy; the tablet retries once and carries
on).

## End to end

With the Pro Micro in the tablet, Zwift listed the ANT+ power, HR and cadence
sensors and paired them. The connection held for the first minute. A 30-minute
ride was left for later.
