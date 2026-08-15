# E1: the CHIP stack configured, linked and read back

**Date:** 2026-08-15
**Verdict: `CONFIG_CHIP=y` configures, compiles and links beside RadiANT on both
DKs, and the symbols are verified from the generated `.config` rather than from a
build log. Four things had to be added that the plan did not name, one of them an
architectural difference rather than a missing line. The size numbers are a
FLOOR, not the cost of Matter — see "What the numbers are not".**
**Checked by:** the builds below, reproducible from this document. Narrative
where it says so.

This is E1 of the Matter plan:

> Build `matter.conf` with `CONFIG_CHIP=y` and no Matter code, and read back the
> `.config` and the size report on both DKs. This retires the flash, RAM and
> timeslot risks before a line of Matter code is written.

The first half of E1 — `BUILD_ASSERT(CONFIG_MPSL_TIMESLOT_SESSION_COUNT >= 1)`
in `radiant/src/radiant_radio_nrf_gate_mpsl.c` — was already in place and is
untouched. Nothing below trips it.

`docs/matter-flash-spike.md` is the predecessor: it built NCS's Matter
*template* with radiant appended as a module. This is the other direction —
**this repository's own application, with CHIP turned on inside it** — which is
what the shipping arrangement will be.

## What was built

Three files, all new or additive:

| File | What it is |
|---|---|
| `apps/dongle_thread/matter.conf` | the opt-in image fragment, layered on `thread.conf` |
| `apps/dongle_thread/matter_sysbuild.conf` | `SB_CONFIG_MATTER=y` and the tier-1 opt-outs, passed by name |
| `apps/dongle_thread/Kconfig` | `CONFIG_ANT_DONGLE_MATTER`, plus connectedhomeip's `Kconfig.features` / `Kconfig.defaults` and NCS's `samples/matter/common/src/Kconfig`, sourced inside `if CHIP` |

Nothing under `src/`, nothing in `CMakeLists.txt`, no `radiant/` change, and
**no `apps/dongle_thread/sysbuild.conf`** — that file would apply to all five
existing coexistence arms, whose section 7.4 baselines have to stay comparable.

## Reproduce

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0
```

**nRF54L15 DK** — the full arm, radiant's arbitrated backend included:

```powershell
west -z C:\ncs\v3.4.0\zephyr build -s <repo>\apps\dongle_thread `
     -d <repo>\build\m_e1_l15 -b nrf54l15dk/nrf54l15/cpuapp -p always -- `
     -DANT_RADIO=core -DRADIANT_BACKEND=nrf `
     "-DEXTRA_CONF_FILE=thread.conf;matter.conf" `
     -DSB_EXTRA_CONF_FILE=matter_sysbuild.conf
```

**nRF5340 DK** — CHIP only, no radiant backend; see "The nRF5340" for why:

```powershell
west -z C:\ncs\v3.4.0\zephyr build -s <repo>\apps\dongle_thread `
     -d <repo>\build\m_e1_5340 -b nrf5340dk/nrf5340/cpuapp -p always -- `
     -DANT_RADIO=core -DRADIANT_BACKEND=null `
     -DSB_CONFIG_NETCORE_IPC_RADIO=y `
     -DSB_CONFIG_NETCORE_IPC_RADIO_IEEE802154=y `
     -DSB_CONFIG_NETCORE_IPC_RADIO_BT_HCI_IPC=y `
     "-DEXTRA_CONF_FILE=<scratch>\matter_5340_probe.conf" `
     -DSB_EXTRA_CONF_FILE=matter_sysbuild.conf
```

Redirect west to a log file (`> $log 2>&1`) and read `$LASTEXITCODE`. Windows
PowerShell 5.1 wraps a native command's stderr in ErrorRecords, and west writes
its ordinary progress there; `scripts/build_p4.ps1` says the same thing at
length.

The baseline the deltas below are measured against is the same application, same
board, same backend, **without** `matter.conf`:

```powershell
west ... -d <repo>\build\m_e1_base_l15 -b nrf54l15dk/nrf54l15/cpuapp -p always -- `
     -DANT_RADIO=core -DRADIANT_BACKEND=nrf -DEXTRA_CONF_FILE=thread.conf
```

## The read-back

**From `build/<dir>/dongle_thread/zephyr/.config`, never from the log.** That is
the whole point of the exercise: `nrf/sysbuild/CMakeLists.txt:890-903` writes
`CONFIG_CHIP` into the image from `SB_CONFIG_MATTER` *after* the application's
own fragments have been read, so an unset `SB_CONFIG_MATTER` does not conflict
with `CONFIG_CHIP=y`, it silently overrides it.

`ABSENT` below means the symbol is not defined at all in that build's Kconfig
tree — a different and more informative answer than `n`.

| Symbol | nRF54L15 DK | nRF5340 DK (cpuapp) |
|---|---|---|
| `CONFIG_CHIP` | **y** | **y** |
| `CONFIG_ANT_DONGLE_MATTER` | y | y |
| `CONFIG_BT` | **y** (never written by hand — see trap 3) | **y** |
| `CONFIG_MPSL` | y | ABSENT (cpunet only on this part) |
| `CONFIG_MPSL_TIMESLOT_SESSION_COUNT` | **3** | 0 (symbol invisible) |
| `CONFIG_RADIANT_BACKEND_NRF` | **y** | ABSENT |
| `CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL` | **y** | not set |
| `CONFIG_RADIANT_BACKEND_NULL` | not set | y |
| `CONFIG_RADIANT_BRIDGE` | y | y |
| `CONFIG_ANT_DONGLE_RX_TAP` | y | y |
| `CONFIG_ANT_DONGLE_SELF_CHANNELS` | 2 | 2 |
| `CONFIG_ANT_DONGLE_PAIRING_WINDOW_S` | 60 | 60 |
| `CONFIG_ANT_DONGLE_MQTT_SINK` | not set (correct — this arm has no MQTT) | not set |
| `CONFIG_STD_CPP17` | y | y |
| `CONFIG_EVENTFD` | y | y |
| `CONFIG_CHIP_USE_OPENTHREAD_ENDPOINT` | **ABSENT** | **ABSENT** |
| `CONFIG_PICOLIBC` | y | y |
| `CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE` | y | y |
| `CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` | 10240 | 10240 |
| `CONFIG_HEAP_MEM_POOL_SIZE` | 32768 | 32768 |
| `CONFIG_CHIP_OTA_REQUESTOR` | not set | not set |
| `CONFIG_CHIP_FACTORY_DATA` | not set | not set |
| `CONFIG_BOOTLOADER_MCUBOOT` | not set | not set |
| `CONFIG_NCS_SAMPLE_MATTER_LEDS` | n (invisible: `depends on DK_LIBRARY`) | n |
| `CONFIG_CHIP_NFC_ONBOARDING_PAYLOAD` | **y** — unasked-for; see findings | y |
| settings backend | `SETTINGS_ZMS=y` (RRAM) | `SETTINGS_NVS=y` |

**Trap 1 did not fire, because the fragment that prevents it was written first.**
`SB_CONFIG_MATTER=y` is in `matter_sysbuild.conf` and the merge is visible in the
sysbuild log line `Merged configuration '.../matter_sysbuild.conf'`. There was
never a build of this arm without it, so this is a trap avoided rather than a
trap observed — do not read the green row above as evidence that sysbuild would
have left `CONFIG_CHIP=y` alone.

**Trap 3 fired exactly as described, and the mitigation held.** Nothing in
`matter.conf`, `thread.conf` or `prj.conf` says `CONFIG_BT=y`; it is
`connectedhomeip/config/nrfconnect/chip-module/Kconfig.defaults:152` —
`config BT / default y` inside `if CHIP`. `CONFIG_BT=y` is in the read-back
above on both boards. On the nRF54L15 the `!BT` half of `radiant/Kconfig:84` is
therefore unmet and only `RADIANT_BACKEND_NRF_GATE_MPSL=y` keeps the backend off
NULL — and the read-back confirms `RADIANT_BACKEND_NRF=y` with
`RADIANT_BACKEND_NULL` not set. The silent fallback this project has shipped
three times did not happen here.

**`nm` on the final ELF finds `radiant_radio_init` (`0002d3d0 T`) and
`radiant_sched_tick` (`0002b390 T`).** The spike's 596-byte image had neither —
its whole radiant archive was discarded by `--gc-sections`. This one links
radiant for real, because this application calls it.

## The size reports

`nrf54l15dk/nrf54l15/cpuapp`, no MCUboot (`SB_CONFIG_BOOTLOADER_MCUBOOT=n`), so
FLASH is the whole `slot0_partition`:

| Build | FLASH used | of | % | RAM used | of | % |
|---|---|---|---|---|---|---|
| `thread.conf` baseline | 282 768 B | 1524 KB | 18.12 % | 147 632 B | 256 KB | 56.32 % |
| `thread.conf;matter.conf` | **397 168 B** | 1524 KB | **25.45 %** | **180 368 B** | 256 KB | **68.80 %** |
| delta | **+114 400 B** | | +7.33 pp | **+32 736 B** | | +12.48 pp |

`nrf5340dk/nrf5340/cpuapp` — two images, because this part is dual-core and
sysbuild builds `ipc_radio` for the network core:

| Image | FLASH used | of | % | RAM used | of | % |
|---|---|---|---|---|---|---|
| `dongle_thread` (cpuapp) | 333 416 B | 1 MB | 31.80 % | 177 528 B | 448 KB | 38.70 % |
| `ipc_radio` (cpunet) | 189 876 B | 256 KB | 72.43 % | 54 064 B | 64 KB | 82.50 % |

**The nRF5340 row is WITHOUT radiant's real backend** (`RADIANT_BACKEND_NULL=y`)
and without the MPSL gate. The nRF54L15 rows are WITH both.

RadiANT's own contribution to the nRF54L15 image, measured off the two archives
with `arm-zephyr-eabi-size -t`:

| Archive | text | data | bss |
|---|---|---|---|
| `libradiant.a` | 26 921 | 140 | 9 684 |
| `libapp.a` (apps/common + this app's own src/) | 20 709 | 309 | 29 002 |

## What the numbers are not

**The CHIP library is built and then thrown away, and the flash figure does not
contain it.** `nm -C` on the final ELF of *both* boards finds **zero** symbols
in namespace `chip::`. Nothing in this application calls a CHIP API yet — that
is what "and no Matter code" means — so with `-ffunction-sections
-fdata-sections` and `--gc-sections` the linker discards essentially all of
`libCHIP.a`. The archive that was built and discarded:

| Board | libCHIP.a text | data | bss |
|---|---|---|---|
| nRF54L15 | 495 865 | 618 | 62 079 |
| nRF5340 | 495 983 | 618 | 61 055 |

This is the spike's 596-byte trap in a different costume, and it has to be said
plainly because a 25 % flash figure is exactly the kind of result that gets
quoted as headroom. **The +114 400 B / +32 736 B delta above is the cost of
CHIP's Kconfig-driven DEPENDENCIES** — the SoftDevice Controller and the whole
BLE host, PSA/CRACEN crypto, the NFC onboarding stack, the ZMS lookup cache,
Zephyr sockets and `eventfd`, and the enlarged net buffer counts — **not the
cost of CHIP.**

The useful bound on the finished thing is still the one the flash spike
measured: a real Matter application on this part, plus radiant's marginal cost,
was 761 226 B flash (53.0 %) and 199 268 B RAM (76.0 %). Nothing here contradicts
that and nothing here refines it.

## Risks retired, and risks not

**Retired.**

1. **The image links.** `CONFIG_CHIP`, `CONFIG_BT`, `CONFIG_MPSL`,
   `CONFIG_MPSL_TIMESLOT_SESSION_COUNT` and both radiant backend symbols resolve
   together, in *this application*, on the nRF54L15 — and radiant survives
   `--gc-sections` this time. There is no Kconfig loop, no devicetree conflict,
   and no silent backend fallback.
2. **The timeslot count cannot be zero any more.** The BUILD_ASSERT in
   `radiant_radio_nrf_gate_mpsl.c` is compiled in this arm (`CONFIG_MPSL=y`,
   `GATE_MPSL=y`) and the value read back is 3.
3. **Flash is not a wall on either part**, on any reading. Even adding
   `libCHIP.a`'s whole 495 865 B text to the nRF54L15 figure gives ~893 KB of
   1524 KB.

**Not retired.**

4. **RAM.** 68.80 % at idle with the CHIP library gc'd out. Add the archive's
   62 079 B of bss naively and it does not close; the spike's 76 % measured
   figure says it does close for a real application, but neither number is a
   *peak* — Matter allocates hardest during commissioning, when CASE/PASE, the
   BLE transport and the CHIP heap all want memory at once. **This remains the
   binding constraint and E1 has not measured it.**
5. **Trap 4's runtime half.** `COUNT=3` is a `.config` fact. Whether the gate
   actually gets air with the SoftDevice Controller, the 802.15.4 Service Layer
   and the RRAM flash-sync driver all live is a bench question (plan G2), and a
   `.config` read-back structurally cannot see it.
6. **Trap 10, PICOLIBC and a replaced `malloc` — CONFIRMED, not retired.**
   `CONFIG_PICOLIBC=y` and `CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE=y` with
   `CHIP_MALLOC_SYS_HEAP_SIZE=10240` are all in the read-back, on both boards.
   So every `malloc` in `apps/common`, in the ANT stack and in anything else
   this image links now comes out of a 10 KB `sys_heap` that Matter sized for
   itself. `CONFIG_HEAP_MEM_POOL_SIZE=32768` is a different heap and does not
   help. Nothing here measures the actual demand.
7. **Trap 11, factory reset.** Untouched by this work. Both boards put
   OpenThread's dataset, Matter's fabrics and (if
   `CONFIG_RADIANT_NODE_NVM_SETTINGS` is ever on) radiant's node identity in one
   settings partition.
8. **Trap 12, storage partition sizing — MEASURED, and it is tighter than the
   plan says.** From the generated `zephyr.dts`:

   | Board | `storage_partition` | |
   |---|---|---|
   | nRF54L15 DK | `0x174000 + 0x9000` | **36 KB** (ZMS) |
   | nRF5340 DK cpuapp | `0x0f8000 + 0x8000` | **32 KB** (NVS) |

   The plan says "board default is 36 KB; NCS's Matter layout uses 40 KB plus a
   separate factory-data partition". That is right for the nRF54L15 and
   **understates the nRF5340, which ships 32 KB**. Neither board has a
   `factory_data` partition at all. Fabrics + ACLs + the OT dataset + radiant's
   settings in 32 KB is a partition-layout job that is still entirely ahead.

## Four things the plan did not name

Each of these is a line in `matter.conf` that had to be discovered by building.
Each failed the build rather than passing silently, which is the good case — but
none of them is in the plan.

1. **`CONFIG_STD_CPP17=y`.** connectedhomeip's GN build passes `-std=gnu++17`;
   Zephyr's flags are appended after it and carry `-std=gnu++11` from the
   default arm of the `STD_CPP` choice, and the last `-std` wins. The failure is
   ~40 lines of `'underlying_type_t' in namespace 'std' does not name a template
   type` in `ErrorStr.cpp`, which reads like a broken toolchain. Every NCS Matter
   sample sets this in its own `prj.conf`; nothing sets it for you.

2. **`CONFIG_CHIP_FACTORY_DATA_NONE=y` — trap 2 has a second half.**
   `SB_CONFIG_MATTER_FACTORY_DATA_GENERATE=n` only stops the factory data being
   *generated*. Which provider the image *consumes* is a separate Kconfig choice
   that defaults to `CHIP_FACTORY_DATA_NRFCONNECT_BACKEND`, which
   `select CHIP_FACTORY_DATA`, which needs a `factory_data` devicetree
   partition. Measured: `CONFIG_CHIP_FACTORY_DATA=y` on the first attempt and a
   build failure in `FactoryDataProvider.h:40-41` on
   `DT_N_NODELABEL_factory_data_partition_REG_IDX_0_VAL_SIZEU`. A devicetree
   error message for a Kconfig problem.

3. **`CONFIG_EVENTFD=y`, and the reason behind it is architectural — this is the
   one real disagreement with the plan.** See below.

4. **The nRF5340 needs its own sysbuild netcore selection.**
   `SB_CONFIG_NETCORE_IPC_RADIO` and its two transports do not default on, and
   the symbols do not exist on the nRF54L15 (`SUPPORT_NETCORE` is nRF53/nRF54H
   only), so they cannot live in the shared `matter_sysbuild.conf`. They are
   command-line flags in the reproduce block above.

## The architectural finding: CHIP is on its Zephyr-sockets path here

`connectedhomeip/config/nrfconnect/chip-module/Kconfig:475-478`:

```
config CHIP_USE_OPENTHREAD_ENDPOINT
	bool "Use OpenThread TCP/UDP stack directly"
	default y
	depends on OPENTHREAD && !WIFI && !NETWORKING
```

`thread.conf` sets `CONFIG_NETWORKING=y`, and it has to: `thread_coex_load.c`
sends its UDP through Zephyr sockets and `mqtt_sink.c` needs the same stack. So
`!NETWORKING` is unmet, `CHIP_USE_OPENTHREAD_ENDPOINT` is **ABSENT** in both
read-backs, and `chip_system_config_use_sockets = true` appears in the generated
`args.gn`. **Every NCS Matter sample builds the other way** — no Zephyr IP stack
at all, CHIP talking to OpenThread's own UDP directly. The Matter flash spike's
template build did too.

The immediate consequence is `CONFIG_EVENTFD=y`: `SystemConfig.h:819-824` only
reaches for `<zephyr/posix/sys/eventfd.h>` when `CONFIG_EVENTFD` is defined and
reaches for a nonexistent bare `<sys/eventfd.h>` otherwise, so
`src/system/WakeEvent.cpp:44` fails to compile without it.

The larger consequence is that **this arm is not the configuration Nordic tests
Matter in.** It carries a whole Zephyr IP stack CHIP does not need, and its
UDP path through `UDPEndPointImplSockets` is a different code path from the
samples'. The plan's decision to layer `matter.conf` on `thread.conf` is what
produces this, and the decision is still right for E1 — this arm has to stay
comparable to the section 7.4 coexistence arms, and dropping `NETWORKING` would
take `thread_coex_load.c` with it. But it is a decision that should be made
again, explicitly, when the Matter arm stops needing to be a coexistence arm:
dropping Zephyr networking would put CHIP back on its tested path and take the
Zephyr IP stack out of the image.

## The nRF5340

The first attempt used `matter.conf` unchanged with `-DRADIANT_BACKEND=nrf`, and
it failed at Kconfig, for exactly the reason package F exists:

```
warning: The choice symbol RADIANT_BACKEND_NRF (radiant/Kconfig:57) was selected
(set =y), but RADIANT_BACKEND_NULL ended up as the choice selection.

warning: MPSL has direct dependencies SOC_SERIES_BSIM_NRFXX || SOC_SERIES_NRF52
|| SOC_NRF5340_CPUNET || SOC_NRF54H20_CPURAD || SOC_SERIES_NRF54L ||
SOC_SERIES_NRF71 with value n, but is currently being y-selected by
RADIANT_BACKEND_NRF_GATE_MPSL

error: Aborting due to Kconfig warnings
```

Two separate blockers, and the second is the one the plan does not mention:

- `radiant/Kconfig:76` admits `SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX`
  only, so the backend falls to NULL — package F1, known.
- **`RADIANT_BACKEND_NRF_GATE_MPSL` (`radiant/Kconfig:343`) `select MPSL`
  unconditionally, and MPSL does not exist on `nrf5340/cpuapp` at all.** The
  nRF5340's radio *and its arbiter* are on cpunet. So package F is not only
  "add an NRF53 branch to the address arithmetic" — the gate itself has to move
  to cpunet with the backend, which is what F2 says, but the Kconfig
  consequence is that **no `apps/dongle_thread` fragment that names the gate can
  ever configure for `nrf5340/cpuapp`**. That includes `thread.conf`,
  `gate.conf`, `coex.conf` and `bridge.conf`, i.e. every arm this application
  has.

A third, unrelated blocker turned up on the way and is worth recording because
it means **`apps/dongle_thread` has never been configured for this board**:
`prj.conf` selects `CONFIG_SEGGER_RTT_MODE_NO_BLOCK_SKIP` and
`CONFIG_LOG_BACKEND_RTT_MODE_DROP` while `boards/nrf5340dk_nrf5340_cpuapp.conf`
turns RTT off without unsetting them, leaving two orphaned choice selections;
and that board file's own header still tells the reader to build it with
`-DEXTRA_CONF_FILE=stub.conf`, a file this application does not have. Both are
Kconfig-warning-level and therefore fatal. Neither is E1's to fix.

So the nRF5340 numbers in the table above were taken with a scratch fragment
(`<scratch>/matter_5340_probe.conf`, not committed) that is `thread.conf` +
`matter.conf` with every MPSL and backend line removed, plus the three
board-hygiene overrides, and `-DRADIANT_BACKEND=null`. **They answer the
CHIP-side question — does connectedhomeip configure, compile and link inside
this application on an nRF5340, and what does it cost — and they answer nothing
about RadiANT on that part.**

## Loose ends this raised

- **An NFC stack nobody asked for.** `CONFIG_CHIP_NFC_ONBOARDING_PAYLOAD=y` on
  both boards, from `Kconfig.defaults:232`. NCS's own Matter template turns it
  off in its `prj.conf`; this arm does not, so the measured flash figures carry
  `nfc_t2t` and the NDEF libraries. That is headroom available for the asking,
  and the numbers above are an honest upper bound rather than a tuned one. A
  dongle with no NFC antenna has no use for it.
- **`CONFIG_NCS_SAMPLE_MATTER_LEDS=n` is currently a no-op**, because
  `NCS_SAMPLE_MATTER_LEDS depends on DK_LIBRARY` and this arm does not enable
  `DK_LIBRARY`. The line stays: E2 brings NCS's sample-common sources in, and
  the collision it prevents (NCS's `board.cpp` driving DK LEDs 0-3 against
  `ant_dongle_main.c:173-183`'s `led0`) has to be decided before that, not after.
- **E2's expected diff is smaller than the plan predicts.** The plan says
  rebuilding the five existing arms will show `NCS_SAMPLE_MATTER_LEDS` /
  `NCS_SAMPLE_MATTER_TEST_EVENT_TRIGGERS` appearing as `=y` everywhere, because
  `samples/matter/common/src/Kconfig` is not guarded by `if CHIP`. This work
  wrapped that `osource` in `if CHIP` in `apps/dongle_thread/Kconfig` — see the
  comment there for why — so on the five non-Matter arms those symbols are
  defined but unselectable, and the diff is comment lines rather than `=y`
  lines. E2 should still run the diff; the prediction to check against is this
  paragraph, not the plan's.
- **`bridge.conf:365-376`, cited by the plan as the precedent for restating a
  symbol, does not exist** — `bridge.conf` is 96 lines. The passage meant is
  `bridge.conf:63-85`, the `NET_CONFIG_INIT_TIMEOUT` note, and that is the voice
  `matter.conf` follows.
