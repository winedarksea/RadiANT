# Does the Matter arm fit on an nRF52840?

**Date:** 2026-08-15
**Verdict: YES, with room to spare - and the nRF52840 image is SMALLER in both
flash and RAM than the nRF54L15 image the project already ships.**
**797 684 B flash of 1 MB (76.07 %) and 233 816 B RAM of 256 KB (89.19 %),
against a same-day, same-source nRF54L15 control at 804 756 B flash and
242 428 B RAM (92.48 %). The nRF52840 has 28 328 B of static RAM free where the
nRF54L15 has 19 716 B.**
**BUT the image does not build from the repository as it stands.** One
pre-existing two-line `#if` gap in `radiant/src/radiant_radio_nrf.c` makes
`nrf52840 + RADIANT_BACKEND_NRF_GATE_MPSL` six hard compile errors. That file is
owned by another agent right now and was NOT edited; the measurement was taken
on a patched copy of the tree and the patch is quoted in full below.
**Checked by:** the two builds in "Reproduce". `.config` read-back and `nm`
symbol survival both passed on both. **No board was flashed and no serial port
was opened**; there is no nRF52840 DK on this bench in any case.

`docs/matter-ram-budget.md` is the predecessor and every one of its caveats
carries over unchanged, because the nRF52840 has exactly the same 256 KB of RAM.

---

## 1. NCS still supports Matter on nRF52840, and that is measured not assumed

The first thing to rule out was Nordic having dropped the part. It has not, in
NCS v3.4.0:

```
nrf/samples/matter/template/README.rst:3
  |matter_dks_thread| replace:: ``nrf52840dk/nrf52840``, ``nrf5340dk/nrf5340/cpuapp``,
  ``nrf54l15dk/nrf54l15/cpuapp``, ``nrf54l15tag/nrf54l15/cpuapp``, ...
```

`nrf52840dk/nrf52840` appears in the `platform_allow` / `integration_platforms`
lists of `light_bulb`, `light_switch`, `temperature_sensor`, `contact_sensor`,
`smoke_co_alarm` and `window_covering`'s `sample.yaml`, and seven samples carry
a `boards/nrf52840dk_nrf52840.overlay` plus an MCUboot sysbuild overlay for it.
`nrf/dts/samples/matter/nrf52840_partitions.dtsi` exists and is current. No
Matter Kconfig in `chip-module/` gates on the SoC series in a way that excludes
nRF52; the only SoC-conditional line found anywhere in that tree is a stack-size
default (see section 6).

So the SoC is permitted, and the question really is a budget question.

## 2. Reproduce

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
$ErrorActionPreference = 'Continue'      # west's stderr progress is not an error
Push-Location C:\ncs\v3.4.0

west -z C:\ncs\v3.4.0\zephyr build -s <repo>\apps\dongle_thread `
     -d <repo>\build\n52840_fit2 -b nrf52840dk/nrf52840 -p always -- `
     "-DANT_RADIO=core" "-DRADIANT_BACKEND=nrf" `
     "-DEXTRA_CONF_FILE=thread.conf;matter.conf" `
     "-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf"
```

Quote every `-D`; `docs/matter-e3-vendoring.md` explains why (PowerShell 5.1
splits `-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf` at the dot).

Two new files make the board target work and they are the only additions to the
repository this document required:

- `apps/dongle_thread/boards/nrf52840dk_nrf52840.overlay` - the `radiant,radio-timer`
  chosen node (`timer4`; TIMER0/1/2 have only four CC channels and the backend
  needs five, and TIMER0 is MPSL's on this family anyway), the `ant-uart` alias
  on `uart1`, and four unused peripherals turned off.
- `apps/dongle_thread/boards/nrf52840dk_nrf52840.conf` - UART transport, console
  and log on the J-Link VCOM instead of RTT.

Nothing in `matter.conf`, `matter_sysbuild.conf`, `thread.conf`, `prj.conf` or
`radiant/Kconfig` was changed. **`radiant/Kconfig` needed no change at all**:
its nrf arm already reads `depends on SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX`,
and `CONFIG_SOC_COMPATIBLE_NRF52X=y` is in the read-back below.

The control, for the same source tree on the same day:

```powershell
west ... -d <repo>\build\n52840_ctrl_l15 -b nrf54l15dk/nrf54l15/cpuapp ...   # same -D flags
```

## 3. The numbers

No MCUboot, no factory data, no OTA - `matter_sysbuild.conf` says n to all
three, so on the nRF52840 the linker gets the whole 1 MB
(`CONFIG_USE_DT_CODE_PARTITION` is n, `CONFIG_FLASH_LOAD_OFFSET=0`,
`CONFIG_FLASH_LOAD_SIZE=0` - all read back from the generated `.config`).

| | FLASH used | region | % | RAM used | region | % | RAM free |
|---|---:|---:|---:|---:|---:|---:|---:|
| **`nrf52840dk/nrf52840`** | **797 684 B** | 1 MB | **76.07 %** | **233 816 B** | 256 KB | **89.19 %** | **28 328 B** |
| `nrf54l15dk/nrf54l15/cpuapp` (control) | 804 756 B | 1524 KB | 51.57 % | 242 428 B | 256 KB | 92.48 % | 19 716 B |
| Δ (nRF52840 − nRF54L15) | **−7 072 B** | | | **−8 612 B** | | | **+8 612 B** |

Both figures are the linker's own `Memory region / Used Size` line, which is
what the region check actually enforces - not `size` on the ELF.
`arm-zephyr-eabi-size` on the nRF52840 ELF for cross-reference:
`text 791 268 / data 6 402 / bss 227 610`.

**The nRF52840 is the smaller image on both axes, and that is the headline.**
It is not intuition-friendly, so the reason is worth stating: the nRF54L15
carries CRACEN/PSA driver material and an RRAM flash-sync path that the
nRF52840 does not, and the nRF52840's SoftDevice Controller variant selected
here is the peripheral-only `s112` build (36 662 B of text). The nRF52840 gives
that back and more. The parts have **identical 256 KB of RAM**, so every
RAM caveat in `docs/matter-ram-budget.md` applies verbatim and none of it gets
easier.

`docs/matter-ram-budget.md`'s headline 228 532 B is now 242 428 B on the same
board - packages E4-E6 have landed since it was written. That is the number the
comparison above uses, not the document's.

### Where the flash goes

`text` only, attributed from `build/n52840_fit2/dongle_thread/zephyr/zephyr.map`.
The parse totals **711 988 B against the section's 711 996 B - 8 bytes, or
99.999 %**, so this table is trustworthy. (`rodata`, 77 540 B, is not included;
the same parser over-counts it and its ranking is not quoted here.)

| Archive | text | note |
|---|---:|---|
| `libCHIP.a` | **213 770** | 30.0 % of all text |
| `libopenthread-mtd.a` | 85 426 | |
| `libmatter-data-model.a` | 79 550 | the ZAP output + ember |
| `libsoftdevice_controller_peripheral.a` | 36 662 | one object, `s112` |
| `libapp.a` | 31 730 | `apps/common` + this app's `src/` + NCS sample-common |
| `libsubsys__bluetooth__host.a` | 27 674 | commissioning BLE |
| `libnrf-802154-driver.a` | 26 320 | |
| `libsubsys__net__ip.a` | 24 306 | present because of E1's sockets finding |
| **`libradiant.a`** | **22 808** | **3.2 % of text. RadiANT is not the problem.** |
| `libzephyr.a` | 22 158 | |
| `libkernel.a` | 17 862 | |
| `liboberon_3.0.19.a` | 17 440 | |
| `libmpsl.a` | 16 096 | |
| `libpsa_core.a` | 13 892 | |
| everything else | ~76 000 | ~20 archives, none over 8 KB |

Largest single objects: the SDC blob (36 662), the MPSL blob (16 096),
`GroupDataProviderImpl.cpp` (15 840), `CASESession.cpp` (15 060),
`mle.cpp` (11 726), `psa_crypto.c` (10 768).

### Where the RAM goes

Same method, `bss` + `noinit` + `datas` + `device_states` + `k_heap_area`;
**231 638 B of 233 816 B attributed, 99.1 %.**

| Archive | RAM | | Top symbols | bytes |
|---|---:|---|---|---:|
| `libCHIP.a` | 60 684 | | `chip::System::PacketBuffer::sBufferPool` | 19 500 |
| `libapp.a` | 38 610 | | `chip::Server::sServer` | 13 840 |
| `libmatter-data-model.a` | 23 038 | | `ot::gInstanceRaw` | 11 736 |
| `libkernel.a` | 18 906 | | `chip::app::sInteractionModelEngine` | 10 096 |
| `libsubsys__net__ip.a` | 12 544 | | `sHeapMemory` (CHIP malloc arena) | 10 240 |
| `libopenthread-mtd.a` | 11 742 | | `kheap__system_heap` | 8 276 |
| `libsubsys__bluetooth__host.a` | 10 853 | | `sChipThreadStack` | 8 256 |
| **`libradiant.a`** | **9 747** | | `mbedtls_heap` | 8 192 |
| `libmbedtls_zephyr.a` | 8 192 | | `api_xfer` (ours) | 6 144 |
| `libzephyr.a` | 8 057 | | `ot_stack_area` | 4 160 |

Byte for byte the same shape as `docs/matter-ram-budget.md`'s nRF54L15 table,
which is the expected result and a useful cross-check on both.

## 4. The read-backs, which are the point

From `build/n52840_fit2/dongle_thread/zephyr/.config`, never from the log
(trap 1: sysbuild rewrites `CONFIG_CHIP` after the fragments are read):

| Symbol | Value |
|---|---|
| `CONFIG_CHIP` | **y** |
| `CONFIG_BT` | **y** (still never written by hand - it arrives from CHIP's `Kconfig.defaults`) |
| `CONFIG_SOC_COMPATIBLE_NRF52X` | **y** - this is what `radiant/Kconfig`'s nrf arm depends on |
| `CONFIG_RADIANT_BACKEND_NRF` | **y** |
| `CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL` | **y** - trap 3 |
| `CONFIG_RADIANT_BACKEND_NULL` | **not set** - trap 3's failure mode, absent |
| `CONFIG_MPSL_TIMESLOT_SESSION_COUNT` | 3 - trap 4, untouched |
| `CONFIG_ANT_DONGLE_MATTER` / `_MATTER_MAP` | y / y |
| `CONFIG_HEAP_MEM_POOL_SIZE` | 8192 - E9's line, inherited |
| `CONFIG_OPENTHREAD` / `CONFIG_NET_L2_OPENTHREAD` | y / y |
| `CONFIG_FLASH_LOAD_OFFSET` / `_SIZE` | 0 / 0 - the app owns the whole 1 MB |

`nm` on the final ELF - trap 3's other half, the one a `.config` cannot see:

```
00034fc4 T radiant_radio_init          000c2bb0 D radiant_liveness_sink
00032f94 T radiant_sched_tick          000c2bc0 D radiant_matter_glue_sink
0001297c T radiant_matter_row          000c2bd0 D radiant_matter_sink
00078388 T radiant_matter_convert      000c2be0 D radiant_rules_sink
000129dc T radiant_matter_endpoint_for
0001528c T ant_matter_start
```

plus **4 700 demangled `chip::` symbols** in the final ELF, and the sink section
placed (`_radiant_sink_list_start` 0x000c2bb0, `_radiant_sink_list_end`
0x000c2bf0 - four entries, 64 bytes). The flash spike's failure - a green Matter
build with the whole radiant archive garbage-collected - is ruled out.

E9's kernel-heap check re-run on this image: `nm` finds no `k_malloc`,
`k_calloc`, `k_heap_alloc`, `k_queue_alloc_append` or `k_object_alloc`, so
`CONFIG_HEAP_MEM_POOL_SIZE=8192` is still justified here for the same reason it
is on the nRF54L15.

## 5. The one thing that does not build, and the two-line fix that was NOT applied

**As the repository stands, this build fails.** `radiant/src/radiant_radio_nrf.c`:

```
:3474: error: 'NRF_RADIO_Type' has no member named 'PUBLISH_ADDRESS'
:3475: error: 'NRF_RADIO_Type' has no member named 'PUBLISH_RXREADY'
:3476: error: 'NRF_RADIO_Type' has no member named 'SUBSCRIBE_RSSISTART'
:3477: error: 'NRF_RADIO_Type' has no member named 'SUBSCRIBE_RXEN'
:3478: error: 'NRF_RADIO_Type' has no member named 'SUBSCRIBE_TXEN'
:3479: error: 'NRF_RADIO_Type' has no member named 'SUBSCRIBE_DISABLE'
:3581: error: implicit declaration of function 'radio_ep_reg'
```

**This is not a Matter problem and not a budget problem.** It is the exact
failure the file's own comment at line 1510 describes and half-fixes:

> IT IS STILL GATED ON THE SILICON, AND MUST BE. [...] RADIO's PUBLISH_/SUBSCRIBE_
> registers do not exist on a part with the old PPI: on nRF52840 the six lines of
> `radio_ep_reg()` are six hard compile errors [...]

The `RADIANT_NRF_RADIO_EP` discriminator introduced there (`#if defined(DPPIC_PRESENT)`,
line 1532) was applied to `radio_ep_reg()` and to
`radio_endpoints_attach_off_at_init()`, and **was not applied to
`radio_endpoints_save()` and `radio_endpoints_attach()`**, which are guarded by
`#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)` alone (line 3443). So the
direct nRF52840 build compiles and the arbitrated one does not - and every build
in this project that exercises arbitration is on an nRF54L or nRF53, which have
DPPIC, so nothing noticed. It has nothing to do with Matter; **any**
nRF52840 `GATE_MPSL` build fails the same way, including the coexistence arms.

`radiant/src/**` is out of scope for this document's author and was **not
edited**. The measurement was taken on a copy of the tree at
`C:\Users\Colin\ant_dongle_52840_probe`, with this and only this change - the
bodies are empty because on a PPI part there is no endpoint register for another
stack to hold, which is exactly what the comment at
`radio_endpoints_attach_off_at_init()` already says:

```c
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) && !RADIANT_NRF_RADIO_EP
static void radio_endpoints_save(void)
{
	radio_ep.saved = false;
	radio_ep_mine_mask = 0U;
}

static void radio_endpoints_attach(bool on)
{
	ARG_UNUSED(on);
}
#endif

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) && RADIANT_NRF_RADIO_EP
   ... the existing two functions, unchanged ...
#endif
```

**Whoever owns that file should apply it.** It costs nothing on any DPPIC part
(the `#if` selects the existing code byte for byte) and it is the difference
between the nRF52840 being a supported target and being an unbuildable one.

## 6. Two stack-size defaults that do not land, and one of them only on nRF52840

Found while reading the `.config` back, reported because `radiant/Kconfig`'s own
help calls both of them requirements rather than tuning knobs, and because the
failure mode it describes is a silent reboot loop that looks like a wedged
board.

| Symbol | `radiant/Kconfig` wants | nRF52840 got | nRF54L15 got |
|---|---:|---:|---:|
| `ISR_STACK_SIZE` | 4096 | **4096** | 4096 |
| `SYSTEM_WORKQUEUE_STACK_SIZE` | 4096 | **1120** | **1120** |
| `MPSL_WORK_STACK_SIZE` | 2048 | **1024** | 2048 |

`radiant/Kconfig`'s `default N if RADIANT_BACKEND_NRF_GATE_MPSL` wins for
`ISR_STACK_SIZE` (nothing else supplies an earlier default) and **loses for the
other two**, because an earlier definition in the tree already supplies one.
For `MPSL_WORK_STACK_SIZE` the earlier definition is CHIP's own:

```
modules/lib/matter/config/nrfconnect/chip-module/Kconfig.defaults:80
    config MPSL_WORK_STACK_SIZE
        default 2048 if SOC_SERIES_NRF54L   # nRF54L requires more memory due to crypto backend
```

So **the nRF54L15 gets 2048 by coincidence** - CHIP asking for it for an
unrelated reason - and the nRF52840 gets 1024, which is the value
`radiant/Kconfig` explicitly says is not enough. `SYSTEM_WORKQUEUE_STACK_SIZE`
comes out 1120 on **both** boards against the 4096 the module asks for, so that
half is a pre-existing condition of the shipping nRF54L15 image and not
something the nRF52840 introduces.

Neither was changed here: both would have to be restated in a `.conf` the
application names, and every `.conf` in `apps/dongle_thread` is owned by other
agents right now. **Costed: raising both to what `radiant/Kconfig` asks for is
2 976 + 1 024 = 4 000 B of RAM, which the nRF52840's 28 328 B of headroom
absorbs comfortably.** Do it before anyone boots this image.

## 7. What this does and does not settle

**Settled.**

1. **Flash is not the wall on an nRF52840, and neither is RAM.** 76.07 % and
   89.19 %, with 250 892 B and 28 328 B free respectively, on an image that
   really contains both stacks (4 700 `chip::` symbols and six surviving
   `radiant_*` entry points, checked with `nm`).
2. **The nRF52840 is the *cheaper* of the two parts for this workload**, by
   7 072 B of flash and 8 612 B of RAM against a same-day nRF54L15 control.
   Nobody should assume the nRF54L15 is required on budget grounds.
3. **NCS v3.4.0 still supports Matter on nRF52840.** Section 1.
4. **`radiant/Kconfig` needed no widening.** `SOC_COMPATIBLE_NRF52X` was already
   an accepted arm and reads back `y`.
5. **`docs/radiant-bridge.md:1853`'s "Fits. Very tight." is right about the
   first word and pessimistic about the second**, and its section 11 "Matterless
   escape hatch" is not needed on flash or static-RAM grounds.

**Not settled, and two of these are serious.**

6. **It does not build from the repository.** Section 5. Until that `#if` lands,
   every number here is from a patched copy.
7. **DFU is the real nRF52840 constraint, and it is untested here.** This image
   has no MCUboot, no factory data and no OTA, exactly like the nRF54L15 arm it
   is compared against. NCS's own nRF52840 Matter layout
   (`nrf/dts/samples/matter/nrf52840_partitions.dtsi`) is 28 KB MCUboot +
   **960 KB slot0** + 4 KB factory data + 32 KB storage, with **slot1 on an
   external QSPI part** (`mx25r64`) because two 960 KB slots do not fit in 1 MB.
   797 684 B fits in the 983 040 B slot0 with 185 356 B (18.9 %) spare - before
   factory data and the OTA requestor are added, which the nRF54L15's 1524 KB
   makes a non-question. **A shipping nRF52840 bridge needs an external flash
   chip on the board.** That is a BOM decision, not a code one, and it is the
   one place where the nRF52840 is genuinely the worse part.
8. **Peak RAM is still unmeasured**, on either part. Every figure here is
   static. Matter allocates hardest during commissioning out of the 10 KB CHIP
   heap that OpenThread also uses, and `docs/matter-ram-budget.md`'s caveat 1
   applies unchanged - the parts have the same 256 KB.
9. **Nothing here has run.** No board was flashed, no serial port opened, and
   there is no nRF52840 DK on this bench (the attached nRF52840 devices are a
   dongle and a Feather). The board files in section 2 have never booted, the
   `timer4` choice has never been exercised, and the `uart1` pinout is whatever
   the DK's own DTS routes.
10. **The nRF52840's radio backend has never been on the air.**
    `radiant/Kconfig`'s own nrf arm says so: "The address arithmetic and packet
    configuration are measured on nRF54L15 (Spike A). The nRF52840 is predicted
    to behave identically and has not been confirmed." A build that fits is not
    a radio that works, and section 5's fix removes a whole per-grant
    endpoint-swap layer on this part - correctly, since the registers do not
    exist, but that reasoning has never been checked against a running
    nRF52840 beside a second stack.
