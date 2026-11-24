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

**Every upstream file below is byte-for-byte identical to its source, except
the three marked PATCHED** (see the paragraphs after the list). Nothing was
reformatted, retagged or "tidied". If a future reader needs to diff against a
newer NCS, the copy is a straight `Copy-Item`:

```
core/bridge_manager.h                      <- src/core/
core/bridge_manager.cpp                    <- src/core/, PATCHED (below)
core/matter_bridged_device.{h,cpp}         <- src/core/
core/bridged_device_data_provider.{h,cpp}  <- src/core/
core/bridge_storage_manager.h              <- src/core/, PATCHED (below)
core/bridge_storage_manager.cpp            <- src/core/, PATCHED (below)
core/util/bridge_util.h                    <- src/core/util/
default_zap/bridge.zap                     <- src/default_zap/
default_zap/bridge.matter                  <- src/default_zap/
default_zap/zap-generated/*                <- src/default_zap/zap-generated/
```

**`core/bridge_storage_manager.cpp` is patched** (security review 2026-11-20
item 2). Upstream's record loaders checked each stored string length against
the bytes read, never against the `char[32]` it was copied into, so a corrupted
settings record overran the stack at every boot. The V1 and V2 loaders now load
and then call `bridge_storage_codec.h`'s decoders; `StoreBridgedDevice()`
refuses a length the loader would refuse; the two compiled-out migrations bound
their label copies. Every change carries a `RadiANT:` comment. When re-copying
from a newer NCS, check whether upstream has fixed this, and re-apply the patch
if it has not. The record format is unchanged.

**`core/bridge_manager.cpp` and `core/bridge_storage_manager.{h,cpp}` are
patched for the endpoint high-water mark** (security review 2026-11-20 item B).
Upstream rebuilt the next dynamic endpoint id at every boot as
max(restored ids) + 1, so the id of a device that was unbound before a reboot,
or whose record did not restore, was handed to the next new device - a reuse
Matter forbids. `BridgeStorageManager` gains one key, `br/brd_ep_hw` (a
`uint16_t`, the next id), with `StoreEndpointHighWater()` /
`LoadEndpointHighWater()`. `BridgeManager::Init()` seeds the counter from it,
the restore still raises it past every restored id, and a raised counter is
written back once after the restore. A fresh allocation writes the mark
*before* `CreateEndpoint()` makes the id visible to a controller, so a power
loss between the two cannot leave an exposed id uncovered; if that write fails
the endpoint is not created. An id the stored mark already covers costs no
write, which keeps upstream's ENDPOINT_EXISTS skip loop from writing per
skipped id. 0xFFFF (`chip::kInvalidEndpointId`) is never tried or stored: it
wraps to the first dynamic id like upstream's wrap to 0.

**The same three files are patched again for a missing versus an unreadable
mark** (security review 2026-11-21). `LoadEndpointHighWater()` returns
`EndpointHighWater::MarkLoad` (Absent / Valid / Unreadable) instead of a
bool: the persistent-storage API's `NonSecureLoad()` fails alike for a
missing key, a read error, an oversized value and the settings backend's
empty-value marker, so a failed load asks `NonSecureHasEntry()` whether the
key exists. Only Absent takes the legacy max(restored) + 1 fallback. An
Unreadable mark (present but not loadable, the wrong size, or 0xFFFF)
restores the stored devices, skips the boot write-back (it would replace the
mark with max(restored) + 1) and refuses every new allocation with
`CHIP_ERROR_PERSISTED_STORAGE_FAILED`; each refused attempt re-reads the mark
(the ANT bridge retries a bind every second) and the first Valid or Absent
read lets allocation resume past it. A mark that stays malformed blocks new
sensors until the store is repaired or factory-reset: that is the price of
never guessing an id. `sPersistedEndpointHighWater` became
`sEndpointMark` (`EndpointHighWater::Mark`: the persisted value plus whether
it is known). One limit is below this layer: Zephyr's
`settings_load_subtree_direct()` returns 0 whatever its backend does, and the
NVS settings loader deletes an entry it cannot read without reporting it, so a
fault that hides the key reads as missing to `NonSecureHasEntry()` too.

**`core/bridge_storage_manager.{h,cpp}` and the codec gain a format marker for
the mark** (security review 2026-11-22), which closes that limit for a single
fault. A second key, `br/brd_ep_mk` (one byte, 1), is written only once the
mark is in storage: after a successful `StoreEndpointHighWater()`, or by a
`LoadEndpointHighWater()` that finds the mark Valid on a store from before the
marker. `EndpointHighWater::Classify()` takes whether the marker exists, and a
missing mark beside the marker is Unreadable, not Absent, so a mark lost after
it was written defers new ids instead of falling back to max(restored) + 1. A
store with neither key is still the legacy case. Losing the marker alone is
harmless (the mark loads; the marker is rewritten); losing both is a double
fault and still reads as legacy. A lost mark does not come back, so new
sensors stay refused until the bridge's storage is cleared. Note what clears
it in this build: a Matter factory reset does **not** (this image's `.config`
has `CONFIG_CHIP_FACTORY_RESET_ERASE_SETTINGS` off, so CHIP deletes only its own
`mt` subtree and every `br/` key survives, bridged-device records included),
and `BridgeStorageManager::FactoryReset()`, which deletes the `br` subtree,
has no caller. Today the recovery is erasing the settings partition (a full
chip erase and reflash, which also drops the fabric and the Thread dataset).
`NeedsMarker()` is the ordering rule, tested with the rest in
`apps/dongle_thread/tests/matter_storage`.

**`core/bridge_manager.cpp` is patched for the provider quota** (security
review 2026-11-21). Upstream's `AddSingleDevice()` counted a new data
provider in `mNumberOfProviders` at admission and never gave the slot back
when a later step failed (the high-water write, an insert, or
`CreateEndpoint()`), so repeated storage failures exhausted the quota and kept
new sensors out after storage recovered. The count now follows the map
(`bridge_storage_codec.h`'s `ProviderSlots`): admission only checks the
quota, the count rises after a successful `Insert()`, and every erase of a
pair this call inserted passes `FiniteMap::Erase()`'s result to
`ProviderSlots::Erased()`. `CreateEndpoint()`'s internal-error path already
calls `SafelyRemoveDevice()`, which erases the entry and decrements (upstream,
unchanged); the caller's own `Erase()` then returns false, so the slot is
returned exactly once. `dataProvider->Init()` now runs after the high-water
gate, still before the insert.

**`core/bridge_manager.cpp` is patched for the endpoint-exists retry**
(2026-11-22). In `AddSingleDevice()`'s auto-id loop, `CreateEndpoint()`
returns `CHIP_ERROR_SENTINEL` when `emberAfSetDynamicEndpoint()` reports
`CHIP_ERROR_ENDPOINT_EXISTS`, and the loop is meant to retry with the next id.
Upstream erases the pair from `mDevicesMap` on every non-success, SENTINEL
included, so the retry's `CreateEndpoint()` fails `Contains(index)` with
`CHIP_ERROR_INTERNAL` and the device is dropped. The pair is now erased only
for errors other than SENTINEL, so the retry finds it. The loop still ends:
at most `kMaxBridgedDevices - 1` other dynamic endpoints exist and each try
advances the id once. The explicit-id branch is unchanged (a caller-chosen id
cannot be retried, so erasing there is right). Upstream note drafted in
`archive/upstream/ncs-matter-bridge-endpoint-exists-retry.md`. Re-check on an
NCS re-copy: drop the patch if upstream's loop stops erasing on SENTINEL.

**What the tests cover, and what they do not.** The counter arithmetic
(upstream's raise and wrap guard included), the stored value's codec, the
load classification, the unknown-mark rules and the provider bookkeeping live
in `bridge_storage_codec.h`'s `EndpointHighWater` and `ProviderSlots`, and
`apps/dongle_thread/tests/matter_storage` tests them on native_sim, together
with a *model* of the bind/unbind/reboot sequence written in the test (with
read, write and `CreateEndpoint()` faults). It does NOT test
`bridge_manager.cpp`'s wiring (the calls, their order, the power-loss window):
CHIP does not build on native_sim. That wiring is covered by review, by the
Matter build (nm and `.config` checks) and by an on-device check, which is an
owner step.

A store without the key behaves exactly as
upstream; neither the record format nor `kCurrentVersion` changes. Every
change carries a `RadiANT:` comment; re-apply it on a re-copy unless upstream
has started persisting the counter itself.

Three files in this directory are **ours**, carry this repository's Apache-2.0
tag, and are not upstream copies:

- `chip_project_config.h` — the CHIP project-config header. Upstream's has one
  load-bearing line in it (see trap 15 below) and a set of log-verbosity
  overrides; ours restates the load-bearing line and explains it.
- `bridge_core_asserts.cpp` — a translation unit whose entire content is
  `static_assert`s. See trap 15.
- `bridge_storage_codec.{h,cpp}` — the bounds-checked bridged-device record
  decoder that the patched loader calls, and the endpoint high-water mark's
  codec, counter arithmetic and load classification, and the provider-quota
  bookkeeping (`ProviderSlots`). It includes no CHIP header, so
  `apps/dongle_thread/tests/matter_storage` tests it on native_sim.

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
`radiant_matter.c`'s `RADIANT_MATTER_ENDPOINT_FIRST` is `1u`, and CHIP endpoint 1 is the
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
| `RADIANT_MATTER_MAX_ENDPOINTS` | `radiant/include/radiant/radiant_matter.h:179` |

`radiant_matter.h`'s own comment already says all three must agree and that "the
glue static_asserts the three together". `bridge_core_asserts.cpp` is that
static_assert, and it exists at E3 rather than at E4 because it costs nothing now
and because the failure it catches is invisible.

## What is NOT here yet

Nothing in this repository *calls* any of this. There is no data provider
(package E4), no occupancy device type (E5) and no `main()` handoff (E6). The
consequence is measurable and is stated plainly in `docs/matter-bringup.md`:
these objects link, and then `--gc-sections` discards almost all of them, exactly
as E1 found for `libCHIP.a`. A flash delta is therefore **not** evidence that
this vendoring worked, and the readback document does not use one as such.
