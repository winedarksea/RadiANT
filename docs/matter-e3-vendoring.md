# E3/E7: the Matter bridge core vendored, built and read back

**Date:** 2026-08-15
**Verdict: NCS's Matter bridge core and its committed ZAP data model configure,
compile and link inside this application on `nrf54l15dk/nrf54l15/cpuapp`, with
`CONFIG_CHIP=y` read back from the generated `.config`. The ZAP directory is
proven to be the one the build consumes. `radiant_matter.c` is now compiled into
the Matter arm and into no other. The trap-15 `static_assert` is in place and was
observed to fire.**
**But the headline number is RAM: this image is at 96.57 % of 256 KB, up from
E1's 68.80 %, and packages E4-E6 have about 8.6 KB of static RAM to work in.
That is the finding of this package, and it is not a small one.**
**Checked by:** the builds below, reproducible from this document. Narrative
where it says so.

This is E3 and E7 of the Matter plan:

> **E3** Bring `src/core/` and `src/default_zap/` into
> `apps/dongle_thread/src/matter/`, leaving the BLE provider and unused device
> types behind.
> **E7** The Matter block goes after `project()`. Delete the exclusion at
> `CMakeLists.txt:102-106` and guard `radiant_matter.c` on its own symbol.

`docs/matter-e1-readback.md` is the predecessor and its numbers are the baseline
below. Everything E1 recorded about `CONFIG_BT` arriving as a default, about
CHIP being on its Zephyr-sockets path here, and about `CONFIG_STD_CPP17` and
`CONFIG_CHIP_FACTORY_DATA_NONE` being mandatory is unchanged and was not
re-derived.

---

## What was vendored

`apps/dongle_thread/src/matter/`. Its `README.md` is the provenance record and
the omission list; this section is the summary.

| Path | Source | Verbatim? |
|---|---|---|
| `core/bridge_manager.{h,cpp}` | `nrf/applications/matter_bridge/src/core/` | **yes** |
| `core/matter_bridged_device.{h,cpp}` | same | **yes** |
| `core/bridged_device_data_provider.{h,cpp}` | same | **yes** |
| `core/bridge_storage_manager.{h,cpp}` | same | **yes** |
| `core/util/bridge_util.h` | `src/core/util/` | **yes** |
| `default_zap/bridge.zap` | `src/default_zap/` | **yes** |
| `default_zap/bridge.matter` | same | **yes** |
| `default_zap/zap-generated/` (8 files) | `src/default_zap/zap-generated/` | **yes** |
| `Kconfig` | derived from `src/core/Kconfig` | no — BT menu removed |
| `chip_project_config.h` | ours | no |
| `bridge_core_asserts.cpp` | ours | no |
| `README.md` | ours | no |

NCS v3.4.0, toolchain bundle `dcbdc366a1`. Every upstream licence header is
untouched.

`nrf/samples/matter/common/src/binding/binding_handler.cpp` is **referenced in
place, not copied** — `BridgeManager::Init()` calls it and there is no reason to
own a copy of an NCS file that is on the include path anyway. Upstream's own
`CMakeLists.txt` does the same.

## What was left behind

- **The entire BLE-central provider.** `src/ble/` — `ble_connectivity_manager.cpp`,
  `ble_bridged_device_factory.cpp`, both BLE data providers, and the eleven
  `BRIDGE_BT_*` Kconfig symbols. Research finding 5 says to build with
  `CONFIG_BRIDGED_DEVICE_BT=n`; here it is not present to be enabled. That
  subsystem bridges *BLE* sensors and this product bridges *ANT+*.
- **`src/simulated_providers/`** — the shell-driven fakes.
- **`src/bridged_device_types/`** — all five. E5 writes occupancy on
  `humidity_sensor.cpp`'s pattern; package D decides which sensor types this
  product instantiates at all.
- **`app_task.cpp`, `main.cpp`, `bridge_shell.cpp`, `zcl_callbacks.cpp`** — E6
  owns `main()`, and `zcl_callbacks.cpp` is entirely inside
  `#ifdef CONFIG_BRIDGE_SMART_PLUG_SUPPORT`.
- **`src/onoff_plug_zap/`** — a second data model nothing selects is a second
  thing to keep in sync (trap 13).

**Matter's commissioning BLE is untouched and is still on.** `CONFIG_BT=y` in
the read-back below. Dropping the BLE *provider* is not dropping `CONFIG_BT`,
and `bridge_storage_manager.h` still includes `<zephyr/bluetooth/addr.h>`.

## Reproduce

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
$ErrorActionPreference = 'Continue'
Push-Location C:\ncs\v3.4.0

west -z C:\ncs\v3.4.0\zephyr build -s <repo>\apps\dongle_thread `
     -d <repo>\build\m_e3_l15 -b nrf54l15dk/nrf54l15/cpuapp -p always -- `
     -DANT_RADIO=core -DRADIANT_BACKEND=nrf `
     "-DEXTRA_CONF_FILE=thread.conf;matter.conf" `
     "-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf" > $log 2>&1
```

The control arm — same application, same board, same backend, the bus and the
tap present, **no Matter**:

```powershell
west ... -d <repo>\build\m_e3_bridge -b nrf54l15dk/nrf54l15/cpuapp -p always -- `
     -DANT_RADIO=core -DRADIANT_BACKEND=nrf `
     "-DEXTRA_CONF_FILE=thread.conf;bridge.conf" > $log 2>&1
```

**Quote every `-D` argument.** Windows PowerShell 5.1 splits
`-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf` into two arguments at the dot, and
what reaches CMake is `-DSB_EXTRA_CONF_FILE=matter_sysbuild` followed by a
stray `.conf` — which fails with `File not found: .../matter_sysbuild`, a
message that names a file nobody wrote. Measured on the first attempt at this
package; `-DEXTRA_CONF_FILE=` survives unquoted only because the semicolon
already forces quoting. This is not in E1's document because E1's reproduce
block happens to quote it.

## The read-back

**From `build/<dir>/dongle_thread/zephyr/.config`, never from the log.** Trap 1
did not fire, and did not fire for the same reason it did not fire in E1:
`matter_sysbuild.conf` was named on the command line from the first attempt.

| Symbol | Matter arm | Control arm (`bridge.conf`) |
|---|---|---|
| `CONFIG_CHIP` | **y** | not set |
| `CONFIG_ANT_DONGLE_MATTER` | y | ABSENT (`depends on CHIP`) |
| **`CONFIG_ANT_DONGLE_MATTER_MAP`** | **y** | **not set** |
| `CONFIG_CHIP_PROJECT_CONFIG` | `"src/matter/chip_project_config.h"` | ABSENT |
| `CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH` | `"${APPLICATION_CONFIG_DIR}/src/matter/default_zap/bridge.zap"` | ABSENT |
| `CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER` | **16** | ABSENT |
| `CONFIG_BRIDGE_MAX_BRIDGED_DEVICES_NUMBER` | 16 | ABSENT |
| `CONFIG_BRIDGE_MAX_BRIDGED_DEVICES_NUMBER_PER_PROVIDER` | 2 | ABSENT |
| `CONFIG_BRIDGE_AGGREGATOR_ENDPOINT_ID` | **1** | ABSENT |
| `CONFIG_BRIDGE_MIGRATE_VERSION_1` | y | ABSENT |
| `CONFIG_BRIDGED_DEVICE_BT` | **ABSENT** (never defined — the menu was not vendored) | ABSENT |
| `CONFIG_NCS_SAMPLE_MATTER_PERSISTENT_STORAGE` | y | ABSENT |
| `CONFIG_CHIP_ENABLE_READ_CLIENT` | y | ABSENT |
| `CONFIG_DK_LIBRARY` | **ABSENT** | ABSENT |
| `CONFIG_STD_CPP17` | y | — |
| `CONFIG_EVENTFD` | y | — |
| `CONFIG_BT` | **y** (still never written by hand) | — |
| `CONFIG_MPSL_TIMESLOT_SESSION_COUNT` | **3** | 2 |
| `CONFIG_RADIANT_BACKEND_NRF` | **y** | y |
| `CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL` | **y** | y |
| `CONFIG_RADIANT_BACKEND_NULL` | not set | not set |
| `CONFIG_RADIANT_BRIDGE` | y | y |
| `CONFIG_CHIP_FACTORY_DATA_NONE` | y | ABSENT |

`ABSENT` means the symbol is not defined at all in that build's Kconfig tree.

**The five coexistence arms gain exactly one comment line and no image change.**
The control arm's `.config` contains `# CONFIG_ANT_DONGLE_MATTER_MAP is not set`
and nothing else new — every Matter symbol is inside `if CHIP`, and
`ANT_DONGLE_MATTER_MAP` is inside `if RADIANT_BRIDGE`, which only `bridge.conf`
sets. `gate.conf`, `thread.conf`, `thread_sed.conf` and `coex.conf` never see
the symbol at all. **This is the prediction E2 should diff against**, alongside
E1's own paragraph on the subject.

## The vendored core is in the build, and mostly not in the image

This is the part E1 could not claim and this package can only half claim. Say it
plainly rather than quoting a flash delta.

**What is proven.** Every vendored source compiled, as its own object:

```
CMakeFiles/app.dir/src/matter/core/bridge_manager.cpp.obj
CMakeFiles/app.dir/src/matter/core/matter_bridged_device.cpp.obj
CMakeFiles/app.dir/src/matter/core/bridge_storage_manager.cpp.obj
CMakeFiles/app.dir/src/matter/core/bridged_device_data_provider.cpp.obj
CMakeFiles/app.dir/src/matter/bridge_core_asserts.cpp.obj
CMakeFiles/app.dir/C_/.../samples/matter/common/src/binding/binding_handler.cpp.obj
CMakeFiles/app.dir/C_/Users/Colin/ant_dongle/radiant/src/bridge/radiant_matter.c.obj
```

and the whole image linked, so every symbol those objects reference resolved —
`emberAfSetDynamicEndpoint`, `emberAfClearDynamicEndpoint`,
`MatterReportingAttributeChangeCallback`, `Nrf::PersistentStorage`,
`Nrf::Matter::BindingHandler`, `CodegenDataModelProvider::Registry()`, the
`IdentifyCluster` code-driven registration in `matter_bridged_device.h`. That is
the E3 question and the answer is yes.

**What is NOT true, and would be dishonest to imply.** `nm -C` on the final ELF
finds **zero `Nrf::Bridge*` symbols**. The six `Nrf::` symbols that survive are
`Board::UpdateStatusLED`, `Board::sInstance`, two `LEDWidget` methods and two
`Nrf::Matter::InitData` statics — all from NCS's sample-common, none from the
vendored core. `--gc-sections` discarded `BridgeManager`, `MatterBridgedDevice`,
`BridgeStorageManager` and `BridgedDeviceDataProvider` in their entirety,
**exactly as E1 predicted it would**, because nothing in this repository calls
them yet. E4 and E6 are what change that.

The same is true of the ember data model itself: **zero `emberAf*` symbols in
the final ELF.** `libmatter-data-model.a` is built (374 612 B text) and then
almost all of it is discarded, because `emberAfEndpointConfigure()` is called
from `Server::Init()`, which nothing calls. So the aggregator endpoint **exists
in the data model and does not exist in the running image**, and will not until
E6.

## Is `zap-generated/` genuinely consumed? Yes, two independent ways

**1. The three committed `.cpp` files are compiled, from this repository's
path, into the `matter-data-model` target:**

```
modules/connectedhomeip/CMakeFiles/matter-data-model.dir/
  C_/Users/Colin/ant_dongle/apps/dongle_thread/src/matter/default_zap/zap-generated/callback-stub.cpp.obj
  .../zap-generated/CodeDrivenInitShutdown.cpp.obj
  .../zap-generated/IMClusterCommandHandler.cpp.obj
```

**2. The committed `endpoint_config.h` is the one CHIP's own
`attribute-storage.cpp` includes.** That file does
`#include <zap-generated/endpoint_config.h>`, and its compile line in
`build.ninja` carries

```
-IC:/Users/Colin/ant_dongle/apps/dongle_thread/src/matter/default_zap
-IC:/Users/Colin/ant_dongle/apps/dongle_thread/src/matter/default_zap/zap-generated
```

ahead of everything in `modules/lib/matter`. There is no other
`endpoint_config.h` it could resolve to.

**3. Corroborating: the build compiles clusters only this ZAP asks for** —
`bridged-device-basic-information-server.cpp` is in `matter-data-model`, and it
is in no NCS build that is not a bridge.

The data model the committed ZAP describes, read back from
`zap-generated/endpoint_config.h` rather than from the `.zap`:

```
FIXED_ENDPOINT_ARRAY  { 0x0000, 0x0001, 0x0002 }
FIXED_DEVICE_TYPES    { 0x12, 1 }, { 0x16, 4 }, { 0x0E, 2 }, { 0x0100, 3 }
```

Endpoint 0 = OTA Requestor + Root Node, **endpoint 1 = Aggregator (`0x000E`)**,
endpoint 2 = On/Off Light placeholder that `BridgeManager::Init()` disables.

**`chip_zapgen` did not run**, which is the whole point of `BYPASS_IDL`:
`ncs_configure_data_model()` passes it, and the committed directory is therefore
the artefact rather than a cache of one. Nothing in this package regenerated,
reformatted or edited it.

## Trap 15: the `static_assert`, and proof that it fires

`apps/dongle_thread/src/matter/bridge_core_asserts.cpp` is a translation unit
containing nothing but assertions. It exists because the two numbers live on
opposite sides of a language boundary: `chip_project_config.h` is pulled into
CHIP's headers before anything of RadiANT's exists, and `radiant_matter.h` is a
C header this package must not edit.

```c++
static_assert(CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT == RADIANT_MATTER_MAX_ENDPOINTS, ...);
static_assert(Nrf::BridgeManager::kMaxBridgedDevices    == RADIANT_MATTER_MAX_ENDPOINTS, ...);
static_assert(Nrf::BridgeManager::kAggregatorEndpointId != 0, ...);
```

**Verified by breaking it on purpose.** With the first assertion temporarily
written `== RADIANT_MATTER_MAX_ENDPOINTS - 1` and only that object rebuilt:

```
bridge_core_asserts.cpp:51:57: error: static assertion failed:
  CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT ... and RADIANT_MATTER_MAX_ENDPOINTS ... disagree.
  Trap 15: ...
note: the comparison reduces to '(16 == 15)'
```

The compiler's note is the read-back that matters: **both sides really are 16**,
not two undefined macros comparing equal to each other as zero. The edit was
reverted and the image relinked.

`radiant_matter.h:163-169` asked for exactly this and named the trap; the ask is
now satisfied.

## The size reports, against E1's

`nrf54l15dk/nrf54l15/cpuapp`, no MCUboot, so FLASH is the whole
`slot0_partition`.

| Build | FLASH used | % of 1524 KB | RAM used | % of 256 KB |
|---|---|---|---|---|
| E1 `thread.conf` baseline | 282 768 B | 18.12 % | 147 632 B | 56.32 % |
| E1 `thread.conf;matter.conf` | 397 168 B | 25.45 % | 180 368 B | 68.80 % |
| **E3 `thread.conf;matter.conf`** | **681 400 B** | **43.66 %** | **253 156 B** | **96.57 %** |
| E3 control, `thread.conf;bridge.conf` | 310 060 B | 19.87 % | 187 144 B | 71.39 % |
| **E3 delta vs E1's Matter arm** | **+284 232 B** | +18.21 pp | **+72 788 B** | **+27.77 pp** |

`arm-zephyr-eabi-size` on the ELF: `text 674 884 / data 6 508 / bss 246 826`.

Archives, `arm-zephyr-eabi-size -t`:

| Archive | text | data | bss | E1's figure |
|---|---|---|---|---|
| `libmatter-data-model.a` | 374 612 | 956 | 23 875 | did not exist |
| `libCHIP.a` | 520 851 | 618 | 62 095 | 495 865 / 618 / 62 079 |
| `libapp.a` | 38 395 | 725 | 32 139 | 20 709 / 309 / 29 002 |
| `libradiant.a` | 26 921 | 140 | 9 684 | **26 921 / 140 / 9 684 — identical** |

RadiANT's own contribution is byte-identical to E1's, which is the check that
nothing in this package leaked into the module.

### What the numbers are, and what they are not

**They are not the cost of the vendored bridge core.** That core is entirely
`--gc-sections`'d out, as shown above. `libapp.a` grew by ~17.7 KB of text and
~3.1 KB of bss, and that is where the four vendored objects, `binding_handler`,
`radiant_matter.c` and NCS's six sample-common sources live *before* garbage
collection.

**The +284 KB of flash and +72.8 KB of RAM are dominated by two things that
arrived together with the plumbing, not by the bridge:**

1. **`source_common.cmake`.** It compiles `matter_init.cpp` and `board.cpp`
   unconditionally, and both define *statically initialised objects* —
   `Nrf::Matter::InitData::sDeviceInfoProviderDefault`,
   `sOperationalKeystoreDefault`, `Nrf::Board::sInstance`. Static objects with
   vtables are roots that `--gc-sections` cannot discard, and each one drags a
   chain of CHIP behind it. **This is why `nm -C` finds 3 696 `chip::` symbols
   in this image where E1 found zero**, and it is the single biggest structural
   change between the two builds.
2. **The ZAP data model**, `libmatter-data-model.a`, which did not exist in E1
   because E1 called no `ncs_configure_data_model()`.

**So the honest framing is the mirror of E1's.** E1 measured the cost of CHIP's
Kconfig-driven *dependencies* with CHIP itself thrown away. E3 measures the cost
of CHIP's dependencies plus the *root set* that NCS's sample-common statics
create — still with the bridge core and the ember data model thrown away. The
number will move again at E6, in both directions: `Server::Init()` will retain
the ember tables (up), and by then the arm may have shed the NFC stack and the
unwanted On/Off Light endpoint (down).

### RAM is now the live problem, and this package did not solve it

**96.57 % of 256 KB. 8 636 B of static RAM left.** E1 listed RAM as the one
un-retired risk and said "this remains the binding constraint and E1 has not
measured it". E3 has now measured a much worse number and has not solved it
either. Three things follow:

- **E4/E5/E6 cannot assume any static RAM.** Sixteen bridged endpoints, each
  with a `DataVersion[]` array, a Descriptor instance and a shadow row, plus
  E6's per-endpoint shadow under a mutex, do not obviously fit in 8.6 KB.
- **This figure is still not a peak.** Matter allocates hardest during
  commissioning. And trap 10 is unchanged: `CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` is
  10 240 B and every `malloc` in `apps/common` and the ANT stack comes out of it.
- **There is known headroom that nobody has taken.** Off the top of E1's own
  loose-ends list: `CONFIG_CHIP_NFC_ONBOARDING_PAYLOAD=y` is still on (a dongle
  with no NFC antenna), and the ZAP still carries an On/Off Light endpoint this
  product does not have, which drags `on-off-server.cpp` and `groups-server.cpp`
  in. Neither is E3's to change — the second needs the ZAP GUI (trap 14) — but
  both should be on the table before anyone concludes the part is too small.

## `radiant_radio_init` survived `--gc-sections`

E8's check, run now because a Matter image with no ANT radio would otherwise
look like a success. `nm` on the Matter arm's final ELF:

```
000300ec T radiant_radio_init
0002e0ac T radiant_sched_tick
00010750 T radiant_matter_row
00010788 T radiant_matter_endpoint_for
00066134 T radiant_matter_convert
000a6594 D radiant_matter_sink
00066174 W radiant_matter_attr_write
```

All present. `radiant_matter_sink` in `.data` is the `RADIANT_SINK_DEFINE()`
iterable-section entry, so the second sink really is registered — which is what
`bridge_pump.c`'s "bridge: 2 sinks" boot line will report once a board runs this
image. `radiant_matter_attr_write` is still `W`: the `__weak` no-op, unoverridden,
because overriding it is E4/E6's job.

**No board was flashed and no serial port was opened for this package.** The
sink count above is a prediction from the symbol table, not an observation.

## Build plumbing (E7), and the exclusion that was deleted

`apps/dongle_thread/CMakeLists.txt`:

- The preamble is **untouched**. `radiant_backend.cmake` and
  `ant_radio_backend.cmake` still run before `find_package(Zephyr)` and still
  only generate `.conf` fragments.
- A new `if(CONFIG_CHIP)` block **after `project()`**, laid out line for line
  like `nrf/applications/matter_bridge/CMakeLists.txt`: `enable-gnu-std.cmake`,
  `source_common.cmake`, `data_model.cmake`, `zap_helpers.cmake`,
  `ncs_get_zap_parent_dir()`, the sources, `ncs_configure_data_model()`.
- **The application did not become C++.** `enable-gnu-std.cmake` appends
  `-std=gnu++17` to the C++ flags only; `src/main.c` and all of `apps/common`
  compiled as C, unchanged.
- **The exclusion was deleted and replaced, not simply deleted.** The old
  comment excluded `radiant_matter.c` *and* `radiant_rd_adapter.c` for two
  different reasons in one sentence. `radiant_rd_adapter.c` **stays excluded** —
  its reason (no bench evidence) has not expired — and its half of the comment
  is intact. `radiant_matter.c` is now compiled under
  `CONFIG_ANT_DONGLE_MATTER_MAP`.
- **`CONFIG_ANT_DONGLE_MATTER_MAP` rather than `CONFIG_CHIP`**, because
  `radiant_matter.c` contains no CHIP call and no CHIP include. Gating a pure-C
  lookup table on connectedhomeip would make it unbuildable on the Matterless
  nRF52840 escape hatch for no reason. It defaults to y only when
  `ANT_DONGLE_MATTER` is set, so `bridge.conf` does not gain a second sink.

Four lines were added to `matter.conf`, each with its reasoning in the file:
`CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH`, `CONFIG_CHIP_PROJECT_CONFIG`,
`CONFIG_NCS_SAMPLE_MATTER_PERSISTENT_STORAGE=y` (without which the vendored
storage manager compiles and fails to link) and `CONFIG_CHIP_ENABLE_READ_CLIENT=y`.
`matter_sysbuild.conf` needed **no change at all**.

## One file outside the package's own scope was edited

`scripts/check_license.py`. It asserts an Apache-2.0 SPDX tag in the first 15
lines of every `.c`, `.h`, `.py`, `.ps1`, `.cmake` and `CMakeLists.txt` in the
tree, and ten vendored headers fail it:

- five `core/*.h` carry `LicenseRef-Nordic-5-Clause`;
- five `zap-generated/*.h` **are** Apache-2.0, but ZAP's templates emit the long
  licence text with **no SPDX line**, which the checker cannot see.

Retagging somebody else's file to satisfy a checker would be a false licence
claim — the exact failure this script was written to catch, pointed the other
way. So all ten are on the script's own `ALLOWLIST`, by name, each with the
licence it actually carries. `check_license: OK ... 300 source files tagged`.

Note what the exemption did *not* have to cover: **every vendored `.cpp`**,
because `SOURCE_SUFFIXES` contains `.c` and not `.cpp`. That is a pre-existing
gap in the checker rather than a decision about those files, it is recorded in a
comment there, and widening the suffix set is a change to what the whole tree is
asserted about and belongs in its own commit.

**`NOTICE` and `docs/third-party.md` were NOT updated**, because this package's
scope forbids touching `docs/**` beyond this file. This repository now
redistributes Nordic-licensed source under `LicenseRef-Nordic-5-Clause`, which
is a real third-party obligation and the second such entry after
`radiant/coex154_ti/`. **Someone must add it.**

## Things the plan or E1's document got wrong

1. **The plan's "delete the exclusion at `CMakeLists.txt:102-106`" is right
   about the lines and wrong about what to do with them.** Deleting the whole
   comment would have deleted the still-valid reason `radiant_rd_adapter.c` is
   excluded. The task brief already caught this; recording it because the plan
   text does not.
2. **The plan says E3 brings in `src/core/` including `bridge_util`, without
   saying it is `src/core/util/bridge_util.h`** — a header-only device factory
   template, no `.cpp`, and it is `#include "bridge_util.h"` from
   `bridge_manager.h`, so `core/util/` has to be on the include path in its own
   right.
3. **The plan does not mention `binding_handler.cpp`**, which
   `BridgeManager::Init()` and `HandleCommand()` both need, and which is NCS's
   file rather than the bridge's. Nor `CONFIG_CHIP_ENABLE_READ_CLIENT`, which it
   needs, nor `CONFIG_NCS_SAMPLE_MATTER_PERSISTENT_STORAGE`, which
   `bridge_storage_manager.cpp` needs. All three are link-time failures, which
   is the good case.
4. **`CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH` is not named anywhere in the
   plan**, and it is the single line that decides which data model the image
   contains. Unset, `ncs_get_zap_parent_dir()` returns an empty parent and the
   build reaches for whatever `zap-generated/endpoint_config.h` the include path
   finds first.
5. **E1's "`CONFIG_NCS_SAMPLE_MATTER_LEDS=n` is currently a no-op" is still
   true, and now for a second reason.** E1 said it is invisible because
   `NCS_SAMPLE_MATTER_LEDS depends on DK_LIBRARY`. E3 compiles `board.cpp` for
   real and `CONFIG_DK_LIBRARY` is still absent, so *both* of `board.cpp`'s LED
   guards are false and the two-owners-of-`led0` collision cannot occur. The `=n`
   line stays as a record of the decision; it is not what is preventing the
   collision.
6. **E1 said E2's expected diff is "comment lines rather than `=y` lines".**
   Still true after E3, and now the diff is exactly one line — see the read-back
   section. E2 should check against that.
7. **The plan's trap 15 says "set it to match `BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER`"
   as though it were a value to choose.** Upstream already defines it *as* that
   symbol (`#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT
   CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER`), so those two cannot drift. The
   pair that can drift, and that the `static_assert` therefore has to guard, is
   the CHIP side against **`RADIANT_MATTER_MAX_ENDPOINTS`**.

## Risks retired, and risks not

**Retired.**

1. **The bridge core compiles and links inside this application**, with its ZAP
   data model, on the nRF54L15, beside RadiANT's arbitrated backend, and
   `radiant_radio_init` survives.
2. **The committed `zap-generated/` is the artefact the build consumes**, proven
   from the object list and the include path rather than assumed from
   `BYPASS_IDL`.
3. **Trap 15 cannot be re-introduced silently.** The assertion is compiled in
   this arm and was observed to fire.
4. **Trap 13's drift risk is bounded for now**, because nothing in this package
   touched `.zap`, `.matter` or `zap-generated/`. It is not *retired* — see
   below.
5. **The five coexistence arms are unchanged.** One comment line in
   `bridge.conf`'s `.config`; no image change; `libradiant.a` byte-identical.

**Not retired.**

6. **RAM, and it is now urgent.** 96.57 %, 8 636 B free, before E4, E5 or E6
   writes a byte.
7. **Trap 13 has no automated check.** The plan suggests wiring NCS's
   `check_zap.py` into CI. Nothing here does, and nothing in this repository
   would notice a hand-edit of `zap-generated/`.
8. **Trap 5 and trap 19 are untested claims.** Every dynamic-endpoint attribute
   being external storage, and declaring `OccupancySensing` in the ZAP being a
   trap rather than a help, are both still read off source rather than observed.
   Neither can be observed until E4/E5.
9. **Trap 6, the endpoint 1 collision, is now confirmed as a fact** — the
   aggregator really is endpoint 1 in this ZAP and `radiant_matter.c`'s
   `MATTER_ENDPOINT_FIRST` really is `1u` — and is **not fixed**. Package D
   owns it.
10. **The On/Off Light placeholder endpoint** is in this product's data model and
    is not this product's device. Removing it needs the ZAP GUI (trap 14), which
    is not in the toolchain bundle.
11. **Everything E1 left open** — the MPSL runtime half, PICOLIBC's replaced
    `malloc`, factory reset, storage partition sizing, and the fact that this arm
    is on CHIP's Zephyr-sockets path rather than the one Nordic tests — is
    untouched by this package.
