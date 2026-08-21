# apps/dongle_ti

The ANT+ dongle firmware on a second vendor's silicon: a TI CC13x2/CC26x2
LaunchXL (`cc26x2r1_launchxl`), running radiant's `cc26xx` HAL backend
(`radiant/src/radiant_radio_cc26xx.c`). It exists to prove that everything
above `radiant_radio_hal.h` - the frame codec, the schedule, the channel
state machines, the search policy, the event queue, the transfer engine and
the whole serial bridge - is genuinely vendor-neutral, by running unchanged
on a radio from a different company.

**UNVERIFIED PENDING HARDWARE**, beyond a single bring-up bench, in one
specific respect: the coexistence arm (`ti_coex.conf`,
`CONFIG_RADIANT_COEX154_CC26XX_FORK`) builds and links but has never run
against a real 802.15.4 peer. There is no second stack on the bench that
built it. Read `ti_coex.conf`'s own header before recording anything off
that arm, and see `docs/decisions/0014` and `0015` for the port's design.

Neither board has a USB device peripheral, so neither enumerates as a dongle
the way `apps/dongle`'s boards do - the ANT serial protocol goes out a UART
instead, at 57600 baud (not the 115200 every other board in this repository
uses; see `boards/cc26x2r1_launchxl.overlay`). On the dongle a CP2102N carries
that UART to the host, which gets to the same place by a different road: the
host sees a COM port rather than an ANT stick, and `--port COMxx --baud 57600`
is how every tool in `tools/` reaches it.

**WORKING ON REAL HARDWARE as of 2026-08-20**, on the CC2652P dongle, over its
own USB: `ant_probe.py` passes all three checks, and `ant_scan.py` hears real
ANT+ sensors - a heart rate strap and a trainer - and decodes their device
numbers and profiles.

    [1/3] reset          OK: startup, reason 0x00 (STARTUP_POWER_ON_RESET)
    [2/3] capabilities   OK: max channels: 32, max networks: 3
    [3/3] version        OK: 'RADIANT0.01B00'

## The two boards

| | `cc26x2r1_launchxl` | `cc2652p_dongle` |
|---|---|---|
| what | LAUNCHXL-CC26X2R1 | the CC2652P + CP2102N USB stick |
| die | CC2652R1F | CC2652P1F (**runtime-different**, see below) |
| ANT UART | uart0, DIO2/DIO3, out the XDS110 backchannel | uart0, DIO12/DIO13, out the CP2102N |
| log | uart1 on BoosterPack pins | RTT, read over JTAG - there is nowhere else |
| flashing | XDS110 backchannel, or the ROM bootloader | XDS110 rig **only**, forever |

The LaunchXL is the development board; the dongle is the deployment target this
application was written for from the start. Board definition:
`boards/ti/cc2652p_dongle/` in this repository, so builds for it need
`-DBOARD_ROOT=<repo>`.

**THE DONGLE CANNOT BE FIELD-UPDATED OVER ITS OWN USB PORT.** The CC13x2/CC26x2
ROM serial bootloader listens on DIO2/DIO3, fixed in silicon (SWCU185 Table
10-2), and this module's CP2102N is soldered to DIO12/DIO13. Its CCFG backdoor
is enabled and on the right button - it simply cannot hear the host. Every
reflash needs the XDS110 wired to it.

**AND THE TWO DIE ARE NOT INTERCHANGEABLE, in a way nothing in the build
catches.** TI's RF driver picks its setup-command layout from
`ChipInfo_GetChipType()` at runtime, so the identical binary is correct on the
LaunchXL's CC2652R and bus-faults inside `RF_open()` on the dongle's CC2652P.
See the comment on `setup_cmd` in `radiant/src/radiant_radio_cc26xx.c`. A green
LaunchXL matrix says nothing about the dongle; build and boot both.

## This application has its own main(), and that has already cost once

`apps/common/ant_dongle_main.c` is the shared boot sequence for `apps/dongle`
and `apps/dongle_thread`. This application does not use it (see the header of
`apps/common/Kconfig.dongle_main` for why) and carries a hand-written `main()`
in `src/main.c` instead. That copy was missing `usb_ant_class_init()`, and had
been since the port was written - which is why the TI port had never actually
passed a byte in either direction, on either board.

It was invisible because the transport's two ring buffers are file-scope, so a
missing `ring_buf_init()` leaves them as well-formed buffers of size **zero**
rather than as anything that faults. Nothing crashed. Every received byte was
accepted and discarded, every transmitted byte waited a second for room that
could not appear, and the board logged `transport up` and blinked its LED
throughout. From the host it looked exactly like a wrong pin map, a wrong baud
or a dead image.

`ant_transport_enable()` now refuses to open a UART whose buffers are
unallocated, so that particular omission fails loudly. **Anything else added to
the shared boot sequence still has to be mirrored here by hand** — which is why
the LED loop stopped being one of them: it is `ant_heartbeat_led_run()` in
`apps/common/ant_heartbeat_led.c`, called from both `main()`s, rather than the
two copies that used to have to agree.

Note the pattern changed with it: 30 ms every 4 s, shortening with receive
activity, rather than the 1 Hz square wave DIO6 was identified by eye from.
`ti_bringup.conf` puts that square wave back, which is what to build when
hunting for a pin.

## Building

```
. .\scripts\env.ps1 -NcsVersion v3.4.0
.\scripts\fetch_hal_ti.ps1                    # once - NCS's manifest filters hal_ti out
west build -s apps/dongle_ti -b cc26x2r1_launchxl -- `
    -DANT_RADIO=core -DRADIANT_BACKEND=cc26xx `
    -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti
```

`v3.4.0` specifically: the ANT stack does not need it (this board has no
sdk-ant path at all), but `radiant`'s own `-DRADIANT_BACKEND=nrf` build has a
recorded trap on `v3.4.0` from a Zephyr SoC-symbol rename - unrelated to this
backend, but it is why the rest of this repository pins `v3.2.4` for the sdk_ant
matrix and why this app is built under whichever NCS version has hal_ti
fetched into it, independently.

For the dongle:

```
west build -s apps/dongle_ti -b cc2652p_dongle -- `
    -DBOARD_ROOT=<repo> -DANT_RADIO=core -DRADIANT_BACKEND=cc26xx `
    -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti
.\scripts\flash_ti.ps1 -Ccxml scripts\ti\cc2652p_dongle.ccxml
.\scripts\ti\rtt_dump.ps1                      # the log, over JTAG, no USB

python tools\ant_probe.py --port COM15 --baud 57600     # does it answer
python tools\ant_scan.py  --port COM15 --baud 57600     # does it hear the air
```

**Do NOT pass `-NoBackchannelReset` when the dongle's USB is in use.** On this
board the XDS110's reset line goes to the target, so that switch skips the
dongle's own reset, and `dslite -u` resuming the core is not a clean boot: the
board comes up silent on its ANT UART and `ant_probe.py` fails all three checks
exactly as if the image were broken. That flag's help in `scripts\flash_ti.ps1`
has the measurements.

If a build picks up `C:\Users\Colin\zephyr` instead of the NCS tree - the
give-away is `Zephyr version: 4.2.99` and a `Zephyr-sdk ... version 0.16`
mismatch - set `ZEPHYR_BASE` and build from inside the workspace, as
`scripts\env.ps1` prints on every invocation.

Add `-DEXTRA_CONF_FILE=ti_bringup.conf` when anything is wrong: it stops a
fault from resetting the board, which on this board is what erases the fault
message before it can be read. That fragment's header explains why.

`scripts/build_all.ps1 -NcsVersion v3.4.0 -Backend core` builds all four TI
arms (base, patch-only, gate-only, full coexistence) and skips them silently
if hal_ti has not been fetched. It does **not** yet carry a `cc2652p_dongle`
row, so the dongle board can still rot unnoticed; adding one needs `BOARD_ROOT`
plumbed through that script. `scripts/flash_ti.ps1` covers flashing over
the XDS110 (`dslite`, not OpenOCD - see that script's header for
why OpenOCD cannot reach this board's JTAG at all).
