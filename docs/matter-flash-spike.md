# The Matter flash-budget spike

**Date:** 2026-08-14
**Verdict: the flash budget does not kill Phase 2.5. RAM is the constraint, and it is tight rather than fatal.**
**Checked by:** the builds below, reproducible from this document. Narrative where it says so.

`docs/radiant-bridge.md:1654-1656` is unambiguous about doing this first:

> Establish the flash budget with a linked image before writing the Matter data
> model, not after — this is a wall you hit at 95 % complete.

The §8.1 type map, the unit conversion, the endpoint model and
`radiant_matter_attr_write()` already exist and are compiled and tested. What is
missing is connectedhomeip. So the question this spike answers is narrow and it
is the only one worth answering before any more Matter code is written: **is
there room on an nRF54L15 for Matter and RadiANT in one image?**

## What was built

connectedhomeip is present in the workspace at `C:\ncs\v3.4.0\modules\lib\matter`,
so no fetch was needed.

```
. .\scripts\env.ps1 -Bundle dcbdc366a1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0
west -z C:\ncs\v3.4.0\zephyr build `
     -s C:\ncs\v3.4.0\nrf\samples\matter\template `
     -d <repo>\build\m_spike -b nrf54l15dk/nrf54l15/cpuapp -p always
Pop-Location
```

It builds clean, including MCUboot, the factory-data image, `dfu_multi_image.bin`
and `matter.ota`. The C++ toolchain, the ZAP-generated sources and PSA CryptoPAL
all work on this bench as shipped — none of those is a blocker.

## The numbers

Matter template, `nrf54l15dk/nrf54l15/cpuapp`, with MCUboot:

| Image | FLASH used | of | % | RAM used | of | % |
|---|---|---|---|---|---|---|
| MCUboot | 43 208 B | 52 KB | 81.14 % | — | — | — |
| `template` (application slot) | 716 400 B | 1 437 552 B | **49.83 %** | 168 476 B | 256 KB | **64.27 %** |

RadiANT's **marginal** cost is the right figure to add, not the size of a whole
RadiANT image — the Matter template already carries the Zephyr kernel,
OpenThread, MPSL, the 802.15.4 driver and logging, and RadiANT would share every
one of them. Measured directly off the two archives in the `p4med` build
(`arm-zephyr-eabi-size -t`), which is the ANT+ + OpenThread + MPSL-gate arm:

| Archive | text | data | bss |
|---|---|---|---|
| `libapp.a` (apps/common: antr adapter, serial bridge, transport, boot) | 14 129 | 245 | 20 512 |
| `libradiant.a` (link layer, scheduler, search, profiles, bridge) | 30 697 | 188 | 9 847 |
| **total** | **44 826** | **433** | **30 359** |

Projected combined image:

| | Matter alone | + RadiANT | headroom left |
|---|---|---|---|
| FLASH | 716 400 B (49.8 %) | **761 226 B (53.0 %)** | **676 326 B** |
| RAM | 168 476 B (64.3 %) | **199 268 B (76.0 %)** | **62 876 B** |

## Reading it

**Flash was the feared wall and it is not one.** 676 KB spare in the application
slot after both stacks. Nothing about the flash budget argues for dropping
Matter, for a bigger part, or for cutting the data model down.

**RAM is the real constraint, and 76 % is a number to respect rather than to
celebrate.** Two things make it less comfortable than it looks:

- Matter allocates heavily *during commissioning* — the CASE/PASE session
  establishment, the BLE transport and the CHIP heap all peak at once, and the
  64.27 % baseline above is an idle, un-commissioned node. The spike does not
  measure the peak.
- `CONFIG_BT=y` for BLE commissioning is not yet in this figure. It brings the
  SoftDevice Controller, and with it a third radio client — see below.

**So the honest verdict is: proceed, and re-measure with a genuinely linked
image before relying on the margin.** This spike bounds the static cost; it does
not bound the dynamic one. What it rules out is the specific failure
`radiant-bridge.md` warned about — discovering at 95 % complete that the image
cannot be linked at all.

## What this spike does NOT answer

Stated explicitly, because a green flash number is exactly the kind of result
that gets read as a general go-ahead.

1. **The three-client interlock is untouched by this.** `CONFIG_BT=y` re-opens
   `radiant/Kconfig`'s `depends on !BT || ..._GATE_MPSL`, so a Matter build
   *must* carry `RADIANT_BACKEND_NRF_GATE_MPSL` or the radio silently becomes
   NULL. And during commissioning there are **three** clients — our ANT windows,
   the 802.15.4 driver, and SDC advertising. **P4 measured two.**
   `radiant-bridge.md:1092-1095` flags that bug 22's `elastic_skew_us` removal
   *"has only been measured against OpenThread. Re-measure the BLE arm before
   treating the advertiser case as settled."* `apps/dongle_thread/coex.conf` is
   that arm and it exists. Run it early — this remains the most likely place
   Matter discovers the gate needs more work, and it is a bigger risk than flash
   ever was.

2. **CHIP owns OpenThread.** `ThreadStackManager` starts, stops and reconfigures
   it. A commissioning cycle that tears the stack down and brings it back is a
   new class of grant exit path, and the residual risk stands: `on_grant_end()`
   is not the only exit from a grant, and any path that skips it leaves our
   channel in the 802.15.4 driver's write-once `SUBSCRIBE_RXEN` — one missed
   restore is permanent. Instrument `radio_ep_contended` restores with a counter
   and assert grants == restores across a commissioning cycle.

3. **The schedule estimate is unchanged.** Bench commissioning to a Home
   Assistant controller with the four §6 booleans visible is still realistically
   **3–6 weeks** of focused work for someone who has not shipped a Matter
   device, dominated by ZAP, factory data and risk 2 above. The pre-built type
   map saves perhaps a week of that, not more. This item alone remains larger
   than Phases 1 and 3 combined.

4. **No combined image was linked.** The two halves were measured separately and
   added. That is a bound, not a build, and the arithmetic assumes RadiANT adds
   nothing to the shared components it links against — which is very nearly true
   and is not exactly true.
