# `apps/dongle_thread/src/matter/` — the vendored Matter bridge core

**Package E3 of the Matter plan.** This directory is NCS's Matter bridge core,
copied into this repository, plus the two small files this application has to
own itself. It is what turns `CONFIG_CHIP=y` (package E1, which linked the CHIP
stack and called none of it) into a build that carries a bridge data model, an
Aggregator endpoint and the dynamic-endpoint machinery.

## Provenance

| | |
|---|---|
| Source | `C:\ncs\v3.4.0\nrf\applications\matter_bridge\src\` |
| NCS version | **v3.4.0** (toolchain bundle `dcbdc366a1`) |
| Copied on | 2026-08-15 |
| Upstream licence | `LicenseRef-Nordic-5-Clause` (the `core/` files) and Apache-2.0 (the ZAP output, which is Project CHIP's) |

**Every upstream file below is byte-for-byte identical to its source.** Nothing
was reformatted, retagged or "tidied". If a future reader needs to diff against
a newer NCS, the copy is a straight `Copy-Item`:

```
core/bridge_manager.{h,cpp}                <- src/core/
core/matter_bridged_device.{h,cpp}         <- src/core/
core/bridged_device_data_provider.{h,cpp}  <- src/core/
core/bridge_storage_manager.{h,cpp}        <- src/core/
core/util/bridge_util.h                    <- src/core/util/
default_zap/bridge.zap                     <- src/default_zap/
default_zap/bridge.matter                  <- src/default_zap/
default_zap/zap-generated/*                <- src/default_zap/zap-generated/
```

Two files in this directory are **ours**, carry this repository's Apache-2.0
tag, and are not upstream copies:

- `chip_project_config.h` — the CHIP project-config header. Upstream's has one
  load-bearing line in it (see trap 15 below) and a set of log-verbosity
  overrides; ours restates the load-bearing line and explains it.
- `bridge_core_asserts.cpp` — a translation unit whose entire content is
  `static_assert`s. See trap 15.

`Kconfig` is also ours: it is upstream's `src/core/Kconfig` with the whole
`if BRIDGED_DEVICE_BT` menu removed, so it is a derivative rather than a copy
and is written in this repository's register.

## What was deliberately LEFT BEHIND, and why

Upstream `matter_bridge` is an *application*. This is not an application; it is
a library that RadiANT will drive. So:

**The entire BLE-central provider.** `src/ble/` — `ble_connectivity_manager.cpp`,
`ble_bridged_device_factory.cpp`, the two BLE data providers, the scan/GATT/
recovery machinery and every `BRIDGE_BT_*` Kconfig symbol. That subsystem exists
to bridge *Bluetooth LE* sensors, and this product bridges *ANT+*. The plan's
research finding 5 says to build with `CONFIG_BRIDGED_DEVICE_BT=n`, which is
exactly what drops it; here it is not even present to be enabled.

**BE CAREFUL WITH THE WORD "BLUETOOTH" IN THIS DIRECTORY.** Dropping the BLE
provider does NOT drop `CONFIG_BT`. Matter's *commissioning* transport is BLE
and arrives as a Kconfig `default` from connectedhomeip's `Kconfig.defaults`
(see `matter.conf`'s long note, and trap 3 of the plan). Two different
Bluetooths; only one of them is gone. `bridge_storage_manager.h` still
`#include`s `<zephyr/bluetooth/addr.h>` for that reason and it still compiles,
because `CONFIG_BT=y` in this arm whether anybody asked for it or not.

**The simulated providers.** `src/simulated_providers/` — the shell-driven fake
sensors upstream uses when there is no BLE. The ANT provider (package E4) is the
real answer to the same question.

**Every bridged device type.** `src/bridged_device_types/` — `onoff_light`,
`onoff_light_switch`, `generic_switch`, `temperature_sensor`, `humidity_sensor`.
Package E5 writes an occupancy device type on `humidity_sensor.cpp`'s pattern,
and the plan's package D decides which of the sensor types this product actually
instantiates. Vendoring types nobody instantiates would put untested code in the
image and would make the E5 review a diff against a copy rather than a design.

**The application shell.** `app_task.cpp`, `main.cpp`, `bridge_shell.cpp`,
`zcl_callbacks.cpp`. `main()` is package E6's, and it does not move — see the
plan. `zcl_callbacks.cpp` is entirely inside `#ifdef CONFIG_BRIDGE_SMART_PLUG_SUPPORT`
upstream and is empty without it.

**`src/onoff_plug_zap/`** — the alternative data model for the smart-plug build.
One ZAP is enough; a second one that nothing selects is a second thing to keep
in sync (trap 13).

## The ZAP is committed OUTPUT, and it is build-critical

`default_zap/zap-generated/` is not a cache and must not be regenerated,
reformatted or hand-edited.

`ncs_configure_data_model()` (`nrf/samples/matter/common/cmake/data_model.cmake`)
calls `chip_configure_data_model()` with **`BYPASS_IDL`** and
`GEN_DIR <here>/zap-generated`. `BYPASS_IDL` means `chip_zapgen` never runs at
build time: the checked-in directory *is* what the build compiles. Meanwhile
`chip_codegen` DOES run, every build, on `bridge.matter`. So there are two
descriptions of one data model in this directory and only one of them is
regenerated automatically.

**They can disagree, and the failure is silent** (trap 13): a cluster that is in
`bridge.matter` but not in `zap-generated/endpoint_config.h` gets metadata with
no init callback — enumerable, wrong, and no error anywhere.

Therefore: `bridge.zap`, `bridge.matter` and `zap-generated/` move **together
and unmodified**, which is how they arrived here. If the data model has to
change, that is a ZAP-GUI job, and **the ZAP GUI is not in the `dcbdc366a1`
toolchain bundle** (trap 14) — `modules/lib/matter/scripts/tools/zap/zap_download.py`
fetches it and there is no `west zap` in NCS v3.4.0. Stop and set that up; do
not hand-edit generated output.

## The data model this ZAP describes

Endpoint 0 is the Matter root node. **Endpoint 1 is the Aggregator (device type
`0x000E`)** — `CONFIG_BRIDGE_AGGREGATOR_ENDPOINT_ID` names it and
`BridgeManager::CreateEndpoint()` passes it as every dynamic endpoint's parent.
Endpoint 2 is a placeholder carrying the **On/Off Light** device type
(`0x0100`), which `BridgeManager::Init()` immediately
`emberAfEndpointEnableDisable(..., false)`s; its only job is to tell
`emberAfFixedEndpointCount()` where the dynamic range starts. Read back from
`zap-generated/endpoint_config.h`:

```
FIXED_ENDPOINT_ARRAY  { 0x0000, 0x0001, 0x0002 }
FIXED_DEVICE_TYPES    { 0x12, 1 }, { 0x16, 4 }, { 0x0E, 2 }, { 0x0100, 3 }
```

endpoint 0 = OTA Requestor (`0x12`) + Root Node (`0x16`), endpoint 1 =
Aggregator (`0x0E`), endpoint 2 = On/Off Light (`0x0100`). **The On/Off Light
is not a device this product has** — it is scaffolding, and its presence in the
data model drags `on-off-server.cpp` and `groups-server.cpp` into the build. It
is left alone because removing it needs the ZAP GUI (trap 14) and because
`BridgeManager::Init()`'s "disable the last fixed endpoint" logic is written
against exactly this shape.

**This is trap 6 in the plan, and it is now a fact rather than a prediction:**
`radiant_matter.c`'s `MATTER_ENDPOINT_FIRST` is `1u`, and CHIP endpoint 1 is the
aggregator. RadiANT's endpoint ids are an opaque key and the glue maps them to
CHIP ids — package D/E4's problem, recorded here because this directory is where
the collision becomes visible.

## Trap 15, and the one number three files have to agree on

`CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT` does not only size the dynamic
endpoint table its own header comment describes. Descriptor is a code-driven
cluster with one server instance per endpoint, and
`modules/lib/matter/src/app/clusters/descriptor/CodegenIntegration.cpp` sizes its
`gServers` array as *fixed + dynamic*. Set it too small and the Nth bridged
endpoint registers **with no Descriptor at all**: enumerable, no cluster list,
no warning, no error.

Three numbers must be equal:

| Number | Where |
|---|---|
| `CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT` | `chip_project_config.h`, here |
| `CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER` | `Kconfig`, here |
| `RADIANT_MATTER_MAX_ENDPOINTS` | `radiant/src/bridge/radiant_matter.h:179` |

`radiant_matter.h`'s own comment already says all three must agree and that "the
glue static_asserts the three together". `bridge_core_asserts.cpp` is that
static_assert, and it exists at E3 rather than at E4 because it costs nothing now
and because the failure it catches is invisible.

## What is NOT here yet

Nothing in this repository *calls* any of this. There is no data provider
(package E4), no occupancy device type (E5) and no `main()` handoff (E6). The
consequence is measurable and is stated plainly in `docs/matter-e3-vendoring.md`:
these objects link, and then `--gc-sections` discards almost all of them, exactly
as E1 found for `libCHIP.a`. A flash delta is therefore **not** evidence that
this vendoring worked, and the readback document does not use one as such.
