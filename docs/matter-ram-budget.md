# E9: where the Matter arm's RAM goes, and what was reclaimed

**Date:** 2026-08-15
**Verdict: the Matter arm went from 253 156 B (96.57 %, 8 988 B free) to
228 532 B (87.18 %, 33 612 B free) on `nrf54l15dk/nrf54l15/cpuapp`. Two lines
did it, and one of them - `CONFIG_HEAP_MEM_POOL_SIZE` - is worth 24 584 B on its
own, because it sizes a kernel heap that this image has NO reachable caller
for.**
**The other two things E3 flagged as untaken headroom are worth almost nothing.
NFC is 40 bytes of RAM today. The On/Off Light endpoint is ZERO bytes of RAM
today, and it is not a placeholder - it is the carrier for the bridged-device
cluster set, so it must not simply be deleted. Both findings are below.**
**Checked by:** the builds in "Reproduce", all on `build/m_ram`. Every number
in this document is a linker figure or a `.map`/`nm` read-back. No board was
flashed and no serial port was opened.

This is the RAM half of the Matter plan's package E. `docs/matter-e3-vendoring.md`
is the predecessor and the baseline; `docs/matter-e1-readback.md` and
`docs/matter-flash-spike.md` are behind that. The spike's verdict was "RAM is the
number to watch, not flash", and it was right.

---

## Reproduce

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
$ErrorActionPreference = 'Continue'
Push-Location C:\ncs\v3.4.0

west -z C:\ncs\v3.4.0\zephyr build -s <repo>\apps\dongle_thread `
     -d <repo>\build\m_ram -b nrf54l15dk/nrf54l15/cpuapp -p always -- `
     "-DANT_RADIO=core" "-DRADIANT_BACKEND=nrf" `
     "-DEXTRA_CONF_FILE=thread.conf;matter.conf" `
     "-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf" > $log 2>&1
```

Quote every `-D`; E3's document explains why (PowerShell 5.1 splits
`-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf` at the dot).

The three measurement tools, in the order they were useful:

```powershell
# 1. Zephyr's own tree. NOTE the target lives in the APPLICATION build dir and
#    `west build -t dongle_thread:ram_report` does NOT work - west resolves the
#    domain back to the top-level dir and ninja reports "unknown target".
ninja -C <repo>\build\m_ram\dongle_thread ram_report

# 2. The .map, which is the only source that attributes a byte to an OBJECT.
#    scripts are throwaway; the parse is "sum every input section landing in the
#    bss / noinit / datas / device_states / k_heap_area output sections, grouped
#    by the archive member on the right of the line".

# 3. nm, for what actually survived --gc-sections.
arm-zephyr-eabi-nm -C --print-size --size-sort zephyr\zephyr.elf
```

`ram_report` is the wrong tool for this job on its own and it is worth saying
why: it groups by **source path**, so 42 484 B land in `(hidden)` and 36 950 B
in `(no paths)` - 31 % of the image in two buckets with no owner, and the CHIP
statics that matter are all in them. The `.map` attributes 249 628 B of the
253 156 B total (98.6 %) to a named object. Use the map.

---

## The ranked table

Baseline, `build/m_e3_l15` / `build/m_ram` before any change. Region is 256 KB
= 262 144 B. Section `noinit` is where Zephyr puts thread stacks and the kernel
heap; `bss` and `datas` are the rest.

### Top consumers, by symbol

| # | Bytes | % of RAM | Symbol | Sect | Object | Sized by |
|---:|---:|---:|---|---|---|---|
| 1 | **32 860** | 12.54 % | `kheap_buf__system_heap` | noinit | `libkernel.a(mempool.c)` | `CONFIG_HEAP_MEM_POOL_SIZE` = 32768 |
| 2 | **19 500** | 7.44 % | `chip::System::PacketBuffer::sBufferPool` | bss | `libCHIP.a(SystemPacketBuffer)` | `CONFIG_CHIP_SYSTEM_PACKETBUFFER_POOL_SIZE` = 15 × 1300 B |
| 3 | **15 304** | 5.84 % | `chip::Server::sServer` | bss | `libmatter-data-model.a(Server.cpp)` | `CHIP_MAX_FABRICS`, `CHIP_MAX_ACTIVE_CASE_CLIENTS`, `CHIP_MAX_ACTIVE_DEVICES` |
| 4 | **11 736** | 4.48 % | `ot::gInstanceRaw` | bss | `libopenthread-mtd.a(instance.cpp)` | OpenThread message/child/router tables |
| 5 | **10 240** | 3.91 % | `sHeapMemory` (CHIP's malloc arena) | bss | `libCHIP.a(SysHeapMalloc)` | `CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` - **trap 10** |
| 6 | **10 096** | 3.85 % | `chip::app::sInteractionModelEngine` | bss | `libCHIP.a(InteractionModelEngine)` | `CHIP_IM_MAX_NUM_*` - **not Kconfig-exposed** |
| 7 | **9 216** | 3.52 % | CHIP event-loop thread stack | noinit | `libCHIP.a(PlatformManagerImpl)` | `CONFIG_CHIP_TASK_STACK_SIZE` (9216 because CRACEN) |
| 8 | **8 192** | 3.13 % | `mbedtls_heap` | bss | `libmbedtls_zephyr.a` | `CONFIG_MBEDTLS_HEAP_SIZE` |
| 9 | **6 144** | 2.34 % | OpenThread thread stack | noinit | `libopenthread_utils.a(openthread.c)` | `CONFIG_OPENTHREAD_THREAD_STACK_SIZE` |
| 10 | **6 144** | 2.34 % | `api_xfer` | bss | `libapp.a(ant_radio_radiant.c)` | ours - the ANT host transfer buffers |
| 11 | 4 192 | 1.60 % | `default_settings_zms` | bss | `libzephyr.a(settings_zms.c)` | ZMS lookup cache |
| 12 | 4 096 | 1.56 % | main thread stack | noinit | `libkernel.a(init.c)` | `CONFIG_MAIN_STACK_SIZE` (thread.conf) |
| 13 | 4 096 | 1.56 % | ISR stack | noinit | `libkernel.a(init.c)` | `CONFIG_ISR_STACK_SIZE` |
| 14 | 4 048 | 1.54 % | `buf32` (log message buffer) | bss | `libzephyr.a(log_core.c)` | `CONFIG_LOG_BUFFER_SIZE` |
| 15 | 3 896 | 1.49 % | `ThreadStackManagerImpl::sInstance` | bss | `libCHIP.a` | CHIP's OT glue |
| 16 | 3 840 | 1.46 % | BT ACL RX pool | noinit | `bluetooth/host(buf.c)` | `BT_BUF_ACL_RX_SIZE` 251 × `(BT_MAX_CONN+1+BT_BUF_ACL_RX_COUNT_EXTRA)` |
| 17 | 3 840 | 1.46 % | `api_ch` | bss | `libapp.a(ant_radio_radiant.c)` | ours |
| 18 | 3 144 | 1.20 % | `g` | bss | `libradiant.a(radiant_event.c)` | ours |
| 19 | 2 848 | 1.09 % | `PlatformManagerImpl::sInstance` | bss | `libCHIP.a` | CHIP |
| 20 | 2 828 | 1.08 % | `sdc_mempool` | bss | `bluetooth/controller(hci_driver.c)` | SoftDevice Controller |
| 21 | 2 820 | 1.08 % | `global_data` | bss | `libpsa_core.a(slot_management)` | PSA key slots |
| 22 | 2 580 | 0.98 % | `m_nrf_802154_rx_buffers` | bss | `libnrf-802154-driver.a` | 802.15.4 |
| 23 | 2 304 | 0.88 % | ANT host thread stack | noinit | `libapp.a(ant_radio_radiant.c)` | ours |
| 24 | 2 264 | 0.86 % | `s` | bss | `libradiant.a(radiant_sched.c)` | ours |
| 25 | 2 112 | 0.81 % | UART transport stack | noinit | `libapp.a(ant_uart_transport.c)` | ours |
| 26 | 2 080 | 0.79 % | `gGroupPeerTable` | bss | `libCHIP.a(SessionManager)` | `CHIP_MAX_FABRICS` |
| 27 | 2 048 | 0.78 % | `channels` | bss | `libradiant.a(radiant_channel.c)` | ours |
| 28 | 2 048 ×6 | 4.69 % | net_tc / net_pkt ×2 / log_core / mpsl_init / bridge_pump / ant_serial_bridge stacks | noinit | various | per-subsystem thread stacks |

### The same image, by archive

| Archive | RAM | Note |
|---|---:|---|
| `libCHIP.a` | 61 363 | packet pool + IM engine + platform + thread stack + CHIP heap |
| `libkernel.a` | 43 234 | 32 860 of it is the system heap (line 1) |
| `libapp.a` | 30 253 | `apps/common` + this app's `src/` + NCS sample-common |
| `libmatter-data-model.a` | 20 428 | 15 304 of it is `sServer` |
| `net/ip` | 12 504 | Zephyr IP stack - present because of E1's sockets finding |
| `libopenthread-mtd.a` | 11 742 | |
| `libzephyr.a` | 10 649 | log buffer, ZMS cache |
| `bluetooth/host` | 9 822 | commissioning BLE |
| **`libradiant.a`** | **9 768** | **3.7 % of the image. RadiANT is not the problem.** |
| `libmbedtls_zephyr.a` | 8 192 | |
| `libopenthread_utils.a` | 6 340 | |
| `libnrf-802154-driver.a` | 5 922 | |
| everything else | ~19 400 | 20 archives, none over 3.2 KB |

**Read line 1 and line 5 of the symbol table together.** The image carries two
general-purpose heaps of 32 KB and 10 KB, plus an 8 KB mbedTLS heap - 51 KB,
20 % of the part - and the 32 KB one has no caller. That is the whole finding of
this package.

---

## What was changed, and what each change measured

Both lines are in `apps/dongle_thread/matter.conf` and nowhere else.
`matter_sysbuild.conf` needed no change.

| Step | Line | RAM | Δ | FLASH | Δ |
|---|---|---:|---:|---:|---:|
| baseline (E3) | - | 253 156 | - | 681 400 | - |
| 1 | `CONFIG_CHIP_NFC_ONBOARDING_PAYLOAD=n` | 253 116 | **−40** | 680 256 | **−1 144** |
| 2 | `CONFIG_HEAP_MEM_POOL_SIZE=8192` | **228 532** | **−24 584** | 680 256 | 0 |
| | **total** | **228 532** | **−24 624** | **680 256** | **−1 144** |

**96.57 % → 87.18 %. 8 988 B free → 33 612 B free.**

Each step is a separate incremental build in `build/m_ram`, and each figure is
the linker's own `RAM: ... B  256 KB  ...%` line.

### Step 1 - NFC: 40 bytes, and that is the point

E1 called `CONFIG_CHIP_NFC_ONBOARDING_PAYLOAD=y` "headroom available for the
asking" and E3 repeated it. It is not, in this image. `--gc-sections` has
already discarded the whole `nfc_t2t` / NDEF chain, because nothing calls
`NFCOnboardingPayloadManagerImpl` until `main()` exists. The measured RAM saving
is 40 B and the flash saving is 1 144 B.

The line is kept regardless, and for a forward reason rather than a present
one: at E6 `main()` reaches `Server::Init()`, NCS's `matter_event_handler.cpp`
(compiled unconditionally by `source_common.cmake`) calls
`ShareQRCodeOverNFC()`, and `libnfc_t2t.a`'s buffers plus the NFCT driver become
reachable. This line means they never arrive. Note also that the symbol's own
`default n` in `chip-module/Kconfig` is overridden by
`chip-module/Kconfig.defaults:232`'s `default y` - reading only the first file
would tell you it was already off.

### Step 2 - the kernel heap: 24 584 bytes, and it has no caller

`thread.conf:111` sets `CONFIG_HEAP_MEM_POOL_SIZE=32768` and calls it "the heap
OpenThread wants". On the **Matter** arm that is not what it is, for a reason
that is trap 10 pointing the other way:

```
zephyr/modules/openthread/platform/memory.c:13-15
    void *otPlatCAlloc(size_t aNum, size_t aSize) { return calloc(aNum, aSize); }
```

OpenThread allocates through **libc**, and `CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE=y`
redirects libc `malloc`/`calloc` globally into CHIP's own 10 KB `sys_heap`
(`__wrap_malloc` is at `0x00037ca4` in this image's symbol table). So on this
arm OpenThread's dynamic allocation comes out of `CHIP_MALLOC_SYS_HEAP_SIZE`,
and `HEAP_MEM_POOL_SIZE` - a different heap with a different API - does not
serve it. **Trap 10 is worse than E1 recorded: it is not only `apps/common` and
the ANT stack sharing CHIP's 10 KB, it is OpenThread too.**

What *can* reach `_system_heap` was checked, not assumed. Disassembling the
final ELF, the object at `0x200013c0` is referenced from exactly four places:

```
statics_init                  init_mem_slab_obj_core_list
k_thread_system_pool_assign   z_thread_alloc_helper
```

`z_thread_alloc_helper`'s only caller is `z_thread_malloc`, whose only caller is
`queue_insert`'s alloc path - `k_queue_alloc_append`/`_prepend`. And `nm` on the
final ELF finds **none** of `k_malloc`, `k_calloc`, `k_realloc`, `k_heap_alloc`,
`k_queue_alloc_append`, `k_object_alloc` or `k_thread_stack_alloc`. The only
archive the linker pulled in *because of* a heap reference is `net_buf`'s
`buf.c` (`k_heap_alloc`), and `CONFIG_NET_BUF_FIXED_DATA_SIZE=y` means its pools
are static. Zephyr's own required-minimum machinery agrees: **no subsystem in
this build declares a `CONFIG_HEAP_MEM_POOL_ADD_SIZE_*` symbol at all**, so the
computed requirement is zero.

8192 rather than 0 is margin for E4/E5/E6, not a measurement. **The check must
be re-run after E6**, and the command is in `matter.conf` beside the line. A
kernel heap that is too small fails at runtime in an image that configures,
compiles, links and boots - the same shape as trap 4, which is why the evidence
above is recorded in full rather than summarised.

It is **restated** in `matter.conf` rather than edited in `thread.conf`, so the
five non-Matter arms keep the images their section 7.4 numbers were taken on.
E2's diff of those arms is unaffected: `matter.conf` is named on the command
line and by no other arm.

### The read-back on the final image

From `build/m_ram/dongle_thread/zephyr/.config`, never from the log:

| Symbol | Value |
|---|---|
| `CONFIG_CHIP` | y |
| `CONFIG_BT` | y (still never written by hand) |
| `CONFIG_MPSL_TIMESLOT_SESSION_COUNT` | **3** - trap 4, untouched |
| `CONFIG_RADIANT_BACKEND_NRF` | y |
| `CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL` | **y** - trap 3, untouched |
| `CONFIG_RADIANT_BACKEND_NULL` | not set |
| `CONFIG_HEAP_MEM_POOL_SIZE` | **8192** (was 32768) |
| `CONFIG_CHIP_NFC_ONBOARDING_PAYLOAD` | **not set** (was y) |
| `CONFIG_NFC_T2T_NRFXLIB` | not set |
| `CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` | 10240 - untouched |
| `CONFIG_CHIP_SYSTEM_PACKETBUFFER_POOL_SIZE` | 15 - untouched |
| `CONFIG_CHIP_MAX_FABRICS` / `_ACTIVE_CASE_CLIENTS` / `_ACTIVE_DEVICES` | 5 / 5 / 5 - untouched |

`nm` on the final ELF, the two checks that matter:

```
0002fcd4 T radiant_radio_init      # E8's check: radiant survived --gc-sections
0002dc94 T radiant_sched_tick
000a611c D radiant_matter_sink      # the second sink is still registered
(no k_malloc, k_queue_alloc_append or k_heap_alloc)   # step 2 still holds
```

`arm-zephyr-eabi-size -t` on `libradiant.a`: **26 921 / 140 / 9 684** - byte for
byte what E1 and E3 measured. Nothing in this package leaked into the module.

**The five non-Matter arms cannot see either change.** Both lines are in
`matter.conf`, which no other arm names, and neither symbol is written by
`thread.conf`, `gate.conf`, `thread_sed.conf`, `coex.conf` or `bridge.conf`. The
prediction E2 should diff against is unchanged from E3's: exactly one comment
line in `bridge.conf`'s `.config`, and no image change.

---

## What was deliberately NOT touched

### Measured, quantified, and left alone

Both of these were built and measured in a probe (`build/m_ram`, both lines
plus step 2, RAM 216 028 B / 82.41 %) and then **reverted**. The numbers are
recorded so that E4/E6 can take them deliberately, with bench evidence, instead
of discovering them under pressure.

| Lever | From → to | Measured saving | Why not taken |
|---|---|---:|---|
| `CONFIG_CHIP_SYSTEM_PACKETBUFFER_POOL_SIZE` | 15 → 8 | **9 100 B** (`sBufferPool` 19 500 → 10 400; 1 300 B/buffer, exactly) | This is CHIP's only message buffer supply, shared by BTP commissioning and every Thread exchange. Exhaustion is a dropped message, not a crash - a runtime failure a `.config` read-back cannot see, on the one code path (commissioning) that allocates hardest. 15 is what Nordic ships and tests in every Matter sample including `matter_bridge`. Taking this needs plan step G1 to have passed first, then a re-run. |
| `CONFIG_CHIP_MAX_ACTIVE_CASE_CLIENTS` / `CONFIG_CHIP_MAX_ACTIVE_DEVICES` | 5 → 2 | **3 408 B** (`sServer` 15 304 → 11 896) | Both default to `CHIP_MAX_FABRICS` *because* `CHIP_PERSISTENT_SUBSCRIPTIONS=y`: they size the outgoing-CASE pools that resume subscriptions after a reboot. At 2, a device joined to 5 fabrics cannot resume all of them. That is a correctness regression against a documented Matter behaviour, for 1.3 % of the part. |

### Not touched, and not measured, with the reason

- **`CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` (10 240 B).** Trap 10, and step 2 above
  made it *more* load-bearing rather than less: `apps/common`, the ANT stack,
  **and now demonstrably OpenThread** all allocate out of it. Shrinking it
  blindly was forbidden by the task and would have been wrong anyway. If
  anything this is the number that may have to go **up** at E6, and that is a
  measurement (`CONFIG_SYS_HEAP_RUNTIME_STATS` plus a bench run), not a guess.
- **`CONFIG_MPSL_TIMESLOT_SESSION_COUNT=3`** - trap 4. Untouched, read back as
  3. 96 bytes.
- **`CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y`** - trap 3. Untouched, read back
  as `y`, with `RADIANT_BACKEND_NRF=y` and `RADIANT_BACKEND_NULL` not set.
- **Storage partition sizing** - trap 12, 36 KB, untouched. Note that
  `CONFIG_CHIP_MAX_FABRICS` is the symbol that couples RAM to that partition,
  and it was left at 5 for both reasons at once.
- **`CONFIG_CHIP_MAX_FABRICS=5`.** Reducing it would take both `sServer` and
  `gGroupPeerTable` down and would ease trap 12 as well, and it is still not
  taken: 5 is the Matter specification's minimum for a commissionable device.
- **`CONFIG_CHIP_TASK_STACK_SIZE` (9 216 B) and every other thread stack.** The
  task's rule was "only where a real high-water mark supports it". There is no
  high-water mark, because nothing here may flash a board.
  `CONFIG_INIT_STACKS` + `kernel stacks` on a bench run is how this gets taken,
  and it is worth up to ~4 KB across the CHIP, OpenThread and BT threads.
- **`CONFIG_MBEDTLS_HEAP_SIZE` (8 192 B).** Serves OpenThread's DTLS. Same
  argument as the CHIP heap: a runtime number with no static bound.
- **`CONFIG_LOG_BUFFER_SIZE` (4 048 B), `CONFIG_MAIN_STACK_SIZE`,
  `CONFIG_NET_*_STACK_SIZE`, the 802.15.4 and net_pkt pools.** All of these are
  either `thread.conf`'s or the board's, and all of them are inside what the
  P3/P4 coexistence arms measure. Out of scope by construction.
- **`CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA` (3 840 B pool).** Nordic sizes it as
  `6 - (BT_MAX_CONN + 1)` and documents the formula in `Kconfig.defaults:198`.
  Worth ~1.3 KB at `EXTRA=2`. Left alone: it changes HCI flow-control credits on
  the one link commissioning runs over, and 1.3 KB is not worth that with
  33.6 KB free.
- **`CONFIG_ZVFS_OPEN_MAX=30` (1 440 B `fdtable`).** CHIP's `Kconfig.defaults:48`
  sets it; the declared `ZVFS_OPEN_ADD_SIZE_*` contributions in this build total
  8. Worth ~670 B at 16. Not worth the risk of a socket exhaustion that only
  appears under load.

---

## The On/Off Light endpoint: it is not a placeholder, and it is not RAM

The task listed the vendored ZAP's On/Off Light endpoint as untaken headroom.
**It is worth zero bytes of RAM in this image, and it must not simply be
deleted.** Both halves matter.

**Zero bytes, today.** The whole ember attribute store is discarded.
`attribute-storage.cpp.obj` in `libmatter-data-model.a` carries 8 213 B text /
684 B data / 823 B bss in the archive and contributes **0 B to the final image**
- it does not appear in the map's RAM attribution at all, because
`emberAfEndpointConfigure()` is called from `Server::Init()` and nothing calls
that yet. E3 said the same thing in flash terms ("zero `emberAf*` symbols in the
final ELF"); it is equally true of RAM.

**What it would be worth at E6**, read off
`src/matter/default_zap/zap-generated/endpoint_config.h` rather than guessed:

```
GENERATED_ENDPOINT_TYPES  { ZAP_CLUSTER_INDEX(0), 14,  19 },   <- endpoint 0
                          { ZAP_CLUSTER_INDEX(14), 2,   0 },   <- endpoint 1, aggregator
                          { ZAP_CLUSTER_INDEX(16), 11, 702 }   <- endpoint 2
ATTRIBUTE_MAX_SIZE (721)             ZAP_FIXED_ENDPOINT_DATA_VERSION_COUNT 24
```

Endpoint 2 owns 702 of the 721 bytes of `attributeData[]` and 11 of the 24
`DataVersion` slots. Deleting it outright would therefore be worth about
702 + 44 = **746 B of bss**, plus ~112 B across the three
`CodegenIntegration.cpp` `gServers[]` arrays (they are sized
`FIXED_ENDPOINT_COUNT + dynamic` = 3 + 16, at 25-44 B a slot), plus
`on-off-server.cpp`'s 148 B of bss. Call it **~1 KB of RAM and ~4.2 KB of
flash** (`on-off-server.cpp` 2 621 B + `groups-server.cpp` 1 560 B of text) -
and none of it before E6.

**And it must not be deleted, because it is the bridged-device template.**
`bridge.matter:2394` onwards, endpoint 2's actual cluster list is:

```
binding: Identify, OnOff
server:  Identify, Groups, OnOff, Descriptor, Binding,
         BridgedDeviceBasicInformation, Switch,
         TemperatureMeasurement, RelativeHumidityMeasurement
```

`BridgedDeviceBasicInformation`, `Descriptor`, `Binding`, `Identify`,
`TemperatureMeasurement` and `RelativeHumidityMeasurement` are exactly the
clusters E4/E5's dynamic endpoints will expose, and they are compiled into the
image **because this endpoint declares them** - the generated `Accessors.cpp`
(126 141 B) and `cluster-objects.cpp` (130 572 B) are keyed on the ZAP's cluster
set, not on which endpoint uses them. Device type `0x0100` On/Off Light is
upstream's way of getting ZAP to accept that palette on one endpoint. Delete the
endpoint and the bridge loses the cluster support it exists to provide.

Also note the 702 bytes are not On/Off's. They are dominated by
`BridgedDeviceBasicInformation`'s string attributes (`NodeLabel`, `VendorName`,
`ProductName`, `SerialNumber`...), which E4 needs. `OnOff` + `Groups` + `Switch`
together are a few tens of bytes of attribute storage.

### What a future ZAP session must do

Not "remove endpoint 2". Precisely:

1. **Open `src/matter/default_zap/bridge.zap` in the ZAP GUI**, which is not in
   the `dcbdc366a1` toolchain bundle - trap 14. `zap_download.py` /
   `zap_bootstrap.sh` under `modules/lib/matter/scripts/tools/zap/` fetch it;
   there is no `west zap` in NCS v3.4.0. This is a one-off setup on whichever
   machine edits the data model.
2. **On endpoint 2, remove only `OnOff`, `Groups` and `Switch`** (server and
   binding). Keep `Identify`, `Descriptor`, `Binding`,
   `BridgedDeviceBasicInformation`, `TemperatureMeasurement` and
   `RelativeHumidityMeasurement`.
3. **Do NOT add `OccupancySensing` to the ZAP** - trap 19. If it is declared,
   `occupancy-sensor-server.cpp` compiles and its init callback runs on every
   dynamic endpoint, calling `emberAfWriteAttribute`, which routes straight back
   into the application's own external-storage write callback. E5 serves
   `Occupancy`, `OccupancySensorType` and `OccupancySensorTypeBitmap` from the
   shadow instead.
4. **The device type on endpoint 2 will have to change** once `OnOff` is gone -
   `0x0100` On/Off Light will no longer validate. Upstream's own answer is to
   leave the endpoint as a template; if it is retyped, the `FIXED_DEVICE_TYPES`
   read-back in `docs/matter-e3-vendoring.md` is what to diff against.
5. **Regenerate `.zap`, `.matter` AND `zap-generated/` together** - trap 13 -
   and re-read `FIXED_ENDPOINT_ARRAY`, `FIXED_DEVICE_TYPES`,
   `ATTRIBUTE_MAX_SIZE` and `ZAP_FIXED_ENDPOINT_DATA_VERSION_COUNT` out of the
   new `endpoint_config.h`. Expect `ATTRIBUTE_MAX_SIZE` to fall by a few tens of
   bytes, not by 702.
6. **Nothing was hand-edited by this package.** `.zap`, `.matter` and
   `zap-generated/` are byte-identical to E3's vendored copies.

**Budget honestly: this is a flash job worth ~4 KB, not a RAM job.** It should
be done, and it should not be counted on for E4/E5/E6's RAM.

---

## `source_common.cmake`'s statics: `board.cpp` is 444 bytes, `matter_init.cpp` is the root

E3 named these as the structural change that pulled 3 696 `chip::` symbols in.
That is right about `matter_init.cpp` and wrong about `board.cpp`.

`nm -C --print-size` on the final ELF, every surviving `Nrf::` symbol:

```
00066176   2  T  Nrf::LEDWidget::Set(bool)
00066178   2  T  Nrf::LEDWidget::Blink(unsigned int, unsigned int)
00010998  68  T  Nrf::Board::UpdateStatusLED()
20005f20 376  B  Nrf::Board::sInstance
200003b0   8  D  Nrf::Matter::InitData::sDeviceInfoProviderDefault
200003b8  16  D  Nrf::Matter::InitData::sOperationalKeystoreDefault
```

**`board.cpp` costs 376 B of bss and 68 B of text.** Its LED paths really are
all inside `#ifdef CONFIG_DK_LIBRARY`, which is absent, and `led_widget.cpp` has
been reduced to two 2-byte stubs. There is nothing to reclaim there, and
removing it would need an edit to `apps/dongle_thread/CMakeLists.txt` (it is an
unconditional `target_sources` in `source_common.cmake`, with no Kconfig guard
upstream) for 444 bytes. Not worth it. `matter_init.cpp` adds 528 B directly,
504 of it `sThreadNetworkDriver`.

The 61 363 B of `libCHIP.a` RAM is **not** these six symbols. It is CHIP's
singletons - `sBufferPool`, `sInteractionModelEngine`,
`PlatformManagerImpl::sInstance`, `ThreadStackManagerImpl::sInstance`,
`sHeapMemory` - retained through the reference graph that those two `.data`
statics in `matter_init.cpp` root. **The root is 24 bytes and the tree is
61 KB**, which is why "delete the roots" is not a strategy and "size the
singletons" is; the table above is sorted for exactly that.

---

## How much room do E4, E5 and E6 actually have?

**33 612 B of static RAM, and that is probably enough, with two caveats that
are not small.**

The demand, as far as it can be bounded from source:

| E-package | What lands in RAM | Estimate |
|---|---|---|
| E4 | `BridgeManager`, `BridgedDeviceDataProvider`, up to 16 `MatterBridgedDevice` instances (each with its `DataVersion[]`, its Descriptor entry and its dynamic attribute declarations) | ~6-10 KB |
| E5 | one more bridged device type; per-instance, not a fixed cost | <1 KB |
| E6 | `Server::Init()` **retains** `attribute-storage.cpp`'s `attributeData[721]` + endpoint arrays, the `CodegenIntegration` `gServers[]` arrays that are currently partly gc'd, the ember attribute metadata, plus the plan's per-endpoint shadow row under a mutex and the `ScheduleWork` flush state | ~4-8 KB |

That is roughly 11-19 KB against 33.6 KB. It closes, with margin, and the two
measured-but-unclaimed levers above (12.5 KB more) are in reserve behind it.

**Caveat 1: this is still not a peak, and it never was.** Every figure in this
document is a static link-time figure. Matter allocates hardest during
commissioning, out of the 10 KB CHIP heap that OpenThread also uses, and no
number here bounds that. `CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` may need to go up
at E6, and that comes straight back out of the 33.6 KB.

**Caveat 2: the kernel-heap saving is conditional on a fact that E6 can
invalidate.** It is 24 584 of the 24 624 bytes reclaimed, and it rests on
`k_malloc` having no caller. If E4/E5/E6 links anything that uses the kernel
heap, the `nm` check in `matter.conf` will show it and the number has to be
re-argued. **Run that check as part of E6, not after it.**

So: the honest answer to "is there room" is **yes, but the margin is one
subsystem wide, not two.** Anyone who adds a second general-purpose heap, raises
`BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER` above 16, or turns MCUboot on (tier 1,
deferred, and a shipping requirement) is spending this package's whole result.

---

## Things found wrong on the way

1. **`docs/matter-e3-vendoring.md`'s "8 636 B free" is an arithmetic slip.**
   256 KB is 262 144 B; 262 144 − 253 156 = **8 988**. The percentage in that
   document (96.57 %) is right and is consistent with 262 144, so it is the
   subtraction that is wrong, not the measurement. It does not change any
   conclusion - both numbers say "nearly nothing left" - but E4 should not
   inherit the wrong one.
2. **`west build -t dongle_thread:ram_report` does not work**, and neither does
   pointing `-d` at the application sub-directory: west resolves the domain back
   to the top-level build directory either way and ninja reports
   `unknown target 'dongle_thread:ram_report'`. Run `ninja` directly in
   `build/<dir>/dongle_thread`. Both E1 and E3 recommend build read-backs and
   neither hits this because neither ran `ram_report`.
3. **`ram_report` cannot answer this question on its own.** 79 434 B - 31 % of
   the image - lands in its `(hidden)` and `(no paths)` buckets, and every CHIP
   static that matters is in them. This is not a criticism of the tool, which
   groups by source path and is excellent for application code; it is a warning
   against quoting its tree for a library-dominated image. The `.map` is the
   source of truth and it attributes 98.6 %.
4. **E1's and E3's "NFC is headroom available for the asking" is wrong for the
   current image** - 40 bytes - and right for the E6 image. The distinction is
   `--gc-sections`, and it is the same distinction E1 drew about `libCHIP.a`
   itself and then did not apply here.
5. **The task's premise that the On/Off Light endpoint is removable headroom is
   wrong twice**: it is 0 B of RAM today, and endpoint 2 is the bridged-device
   cluster template rather than a placeholder. See above.
6. **`thread.conf:108-111`'s "the heap OpenThread wants" is not accurate on any
   arm that overrides libc `malloc`,** and is at best unproven on the others -
   OpenThread's `otPlatCAlloc` is `calloc`, not `k_malloc`. This package did not
   change `thread.conf` (out of scope, and the five arms' baselines depend on
   it), but the comment there should be revisited by whoever owns those arms.

## Risks retired, and risks not

**Retired.**

1. **Where the RAM goes is now known and attributed**, to the object, for 98.6 %
   of the image, with a reproducible method.
2. **The single largest consumer was a heap with no caller**, and it is gone.
   87.18 %.
3. **The two "known untaken headroom" items from E3 are quantified**: 40 B and
   0 B respectively. Neither is the answer, and nobody should spend time on
   either expecting RAM back.
4. **The next 12.5 KB is measured and costed**, so E4/E6 do not have to
   rediscover it.

**Not retired.**

5. **Peak RAM. Still unmeasured, still needs a board.** Plan step G1.
6. **The kernel-heap check must be re-run after E6.** See caveat 2.
7. **`CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` is now serving three subsystems** -
   `apps/common`, the ANT stack and OpenThread - on 10 KB, and no number here
   bounds the demand.
8. **The ZAP session has not happened**, and the flash it would save (~4 KB) is
   not this package's to claim.
