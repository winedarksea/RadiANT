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

This board has no USB device peripheral at all, so it can never enumerate as
a dongle the way `apps/dongle`'s boards do - the ANT serial protocol goes out
a UART instead, at 57600 baud (not the 115200 every other board in this
repository uses; see `boards/cc26x2r1_launchxl.overlay`).

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

`scripts/build_all.ps1 -NcsVersion v3.4.0 -Backend core` builds all four TI
arms (base, patch-only, gate-only, full coexistence) and skips them silently
if hal_ti has not been fetched. `scripts/flash_ti.ps1` covers flashing over
the XDS110 backchannel (`dslite`, not OpenOCD - see that script's header for
why OpenOCD cannot reach this board's JTAG at all).
