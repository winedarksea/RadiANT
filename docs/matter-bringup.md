# The Matter bridge: what it costs, what it needs, and what runs where

Reference for the `matter.conf` arm of `apps/dongle_thread` — the CHIP stack,
Nordic's vendored bridge core, and the ANT-to-Matter glue — on
`nrf54l15dk/nrf54l15/cpuapp` and `nrf52840dk/nrf52840`.
`docs/matter-flash-spike.md` is the predecessor: it built NCS's Matter
*template* with radiant appended as a module. This is the other direction —
this repository's own application with CHIP turned on inside it, which is the
shipping arrangement.

Every number below is a linker figure, a `.map` attribution, an `nm` read-back
or a generated `.config`. Unless a section says otherwise **no board was flashed
and no serial port was opened**; `docs/g-hardware-bringup.md` is where this image
first ran on real hardware.

---

## 1. The arm

| File | What it is |
|---|---|
| `apps/dongle_thread/matter.conf` | the opt-in image fragment, layered on `thread.conf` |
| `apps/dongle_thread/matter_sysbuild.conf` | `SB_CONFIG_MATTER=y` and the tier-1 opt-outs, passed by name |
| `apps/dongle_thread/Kconfig` | `CONFIG_ANT_DONGLE_MATTER`, plus connectedhomeip's `Kconfig.features` / `Kconfig.defaults` and NCS's `samples/matter/common/src/Kconfig`, sourced inside `if CHIP` |

There is deliberately **no `apps/dongle_thread/sysbuild.conf`** — it would apply
to all the existing coexistence arms, whose section 7.4 baselines have to stay
comparable. `matter_sysbuild.conf` is named on the command line instead.

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
$ErrorActionPreference = 'Continue'   # AFTER env.ps1, never before
Push-Location C:\ncs\v3.4.0

west -z C:\ncs\v3.4.0\zephyr build -s <repo>\apps\dongle_thread `
     -d <repo>\build\m_ram -b nrf54l15dk/nrf54l15/cpuapp -p always -- `
     "-DANT_RADIO=core" "-DRADIANT_BACKEND=nrf" `
     "-DEXTRA_CONF_FILE=thread.conf;matter.conf" `
     "-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf" > $log 2>&1
```

This is the recipe `.github/workflows/release.yml`'s "dongle_thread: Matter
(CHIP + bridge core)" row uses. For the nRF52840, swap
`-b nrf52840dk/nrf52840`; nothing else changes.

**Four mechanics that cost bench time:**

- **Quote every `-D` argument.** PowerShell 5.1 splits
  `-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf` into two arguments at the dot, and
  CMake receives `-DSB_EXTRA_CONF_FILE=matter_sysbuild` plus a stray `.conf` —
  failing with `File not found: .../matter_sysbuild`, a message naming a file
  nobody wrote. `-DEXTRA_CONF_FILE=` survives unquoted only because the semicolon
  already forces quoting.
- **Redirect west to a log file and judge on `$LASTEXITCODE`.** PowerShell 5.1
  wraps a native command's stderr in ErrorRecords and west writes ordinary
  progress there; with `Stop` in force the first progress line aborts the run and
  reads exactly like a broken toolchain. `scripts/build_p4.ps1` says the same at
  length.
- **`-d` cannot be a deep path on this machine.** A build directory under
  `AppData\Local\Temp\claude\...\scratchpad\` overruns `MAX_PATH` twice over:
  first as `ninja: error: mkdir(...): No such file or directory` inside
  `cracen_psa_driver`, then — for the Matter arm specifically, which survives
  that — as `fatal error: .../CHIPConfig.h: Invalid argument` out of
  connectedhomeip's GN sub-build, whose include paths are long relative chains
  computed from the build directory's depth. Neither message names path length.
- **Read `.config` from `build/<dir>/dongle_thread/zephyr/.config`, never from
  the build log.** `nrf/sysbuild/CMakeLists.txt`'s Matter block writes
  `CONFIG_CHIP` into the image from `SB_CONFIG_MATTER` *after* the application's
  own fragments have been read, so an unset `SB_CONFIG_MATTER` does not conflict
  with `CONFIG_CHIP=y` — it silently overrides it (trap 1).

---

## 2. Flash and RAM

### 2.1 The progression on `nrf54l15dk/nrf54l15/cpuapp`

No MCUboot (`SB_CONFIG_BOOTLOADER_MCUBOOT=n`), so FLASH is the whole
`slot0_partition` (1524 KB) and RAM is 256 KB = 262 144 B.

| Stage | FLASH | % | RAM | % | RAM free |
|---|---:|---:|---:|---:|---:|
| `thread.conf` baseline, no Matter | 282 768 B | 18.12 % | 147 632 B | 56.32 % | |
| CHIP configured, no Matter code | 397 168 B | 25.45 % | 180 368 B | 68.80 % | |
| bridge core vendored and linked | 681 400 B | 43.66 % | 253 156 B | 96.57 % | 8 988 B |
| after the two RAM cuts (§2.4) | 680 256 B | 43.59 % | 228 532 B | 87.18 % | 33 612 B |
| **E4-E6, the Matter plane live** | **804 756 B** | **51.57 %** | **242 428 B** | **92.48 %** | **19 716 B** |

Control arm, same source tree, `thread.conf;bridge.conf`, no Matter: 310 060 B
flash (19.87 %) and 187 144 B RAM (71.39 %).

**Neither big step is the cost of the thing it looks like.**

- **+114 400 B flash / +32 736 B RAM (+7.33 pp / +12.48 pp) to turn
  `CONFIG_CHIP=y` on with no Matter code.** `nm -C` on that ELF finds **zero**
  `chip::` symbols: `--gc-sections` discards essentially all of `libCHIP.a`
  (495 865 B text / 618 / 62 079 on the nRF54L15; 495 983 / 618 / 61 055 on the
  nRF5340). The delta is the cost of CHIP's **Kconfig-driven dependencies** — the
  SoftDevice Controller and the whole BLE host, PSA/CRACEN crypto, the NFC
  onboarding stack, the ZMS lookup cache, Zephyr sockets and `eventfd`, and the
  enlarged net buffer counts.
- **+284 232 B flash / +72 788 B RAM (+18.21 pp / +27.77 pp) to vendor the bridge
  core.** Also not the bridge core, which is itself entirely gc'd out at that
  stage (zero `Nrf::Bridge*`, zero `emberAf*`). It is dominated by
  `source_common.cmake`, which compiles `matter_init.cpp` and `board.cpp`
  unconditionally and both define *statically initialised objects* —
  `Nrf::Matter::InitData::sDeviceInfoProviderDefault`,
  `sOperationalKeystoreDefault`, `Nrf::Board::sInstance`. Static objects with
  vtables are roots `--gc-sections` cannot discard, each dragging a chain of CHIP
  behind it: **3 696 `chip::` symbols where there had been zero.** Second
  contributor: `libmatter-data-model.a`, which did not exist before because
  nothing called `ncs_configure_data_model()`.

Archives at the vendored-core stage, `arm-zephyr-eabi-size -t`; the ELF is
`text 674 884 / data 6 508 / bss 246 826`:

| Archive | text | data | bss | at the CHIP-only stage |
|---|---:|---:|---:|---|
| `libmatter-data-model.a` | 374 612 | 956 | 23 875 | did not exist |
| `libCHIP.a` | 520 851 | 618 | 62 095 | 495 865 / 618 / 62 079 |
| `libapp.a` | 38 395 | 725 | 32 139 | 20 709 / 309 / 29 002 |
| `libradiant.a` | 26 921 | 140 | 9 684 | **26 921 / 140 / 9 684 — identical** |

`libradiant.a` byte-identical across every stage is the check that nothing in
this work leaked into the module.

### 2.2 nRF5340 DK — CHIP only, no radiant backend (see §8.3)

Two images, because sysbuild builds `ipc_radio` for the network core.

| Image | FLASH | of | % | RAM | of | % |
|---|---:|---:|---:|---:|---:|---:|
| `dongle_thread` (cpuapp) | 333 416 B | 1 MB | 31.80 % | 177 528 B | 448 KB | 38.70 % |
| `ipc_radio` (cpunet) | 189 876 B | 256 KB | 72.43 % | 54 064 B | 64 KB | 82.50 % |

### 2.3 nRF52840 — the cheaper part on both axes

Same source tree, same day, same `-D` flags, against an nRF54L15 control. **Both
parts have exactly 256 KB of RAM**, so every RAM caveat here applies to both.

| | FLASH | region | % | RAM | region | % | RAM free |
|---|---:|---:|---:|---:|---:|---:|---:|
| **`nrf52840dk/nrf52840`** | **797 684 B** | 1 MB | **76.07 %** | **233 816 B** | 256 KB | **89.19 %** | **28 328 B** |
| `nrf54l15dk/nrf54l15/cpuapp` | 804 756 B | 1524 KB | 51.57 % | 242 428 B | 256 KB | 92.48 % | 19 716 B |
| Δ (nRF52840 − nRF54L15) | **−7 072 B** | | | **−8 612 B** | | | **+8 612 B** |

Both are the linker's own `Memory region / Used Size` line, which is what the
region check enforces — not `size` on the ELF. `arm-zephyr-eabi-size` on the
nRF52840 ELF for cross-reference: `text 791 268 / data 6 402 / bss 227 610`.

The nRF54L15 carries CRACEN/PSA driver material and an RRAM flash-sync path the
nRF52840 does not, and the nRF52840's SoftDevice Controller variant selected here
is the peripheral-only `s112` build (36 662 B of text). **Nobody should assume
the nRF54L15 is required on budget grounds.**

NCS v3.4.0 still supports Matter on this part, checked rather than assumed:
`nrf52840dk/nrf52840` is in `nrf/samples/matter/template/README.rst`'s board list
and in the `platform_allow` / `integration_platforms` lists of `light_bulb`,
`light_switch`, `temperature_sensor`, `contact_sensor`, `smoke_co_alarm` and
`window_covering`; seven samples carry a `boards/nrf52840dk_nrf52840.overlay`;
`nrf/dts/samples/matter/nrf52840_partitions.dtsi` exists and is current. No Matter
Kconfig in `chip-module/` gates on the SoC series in a way that excludes nRF52 —
the only SoC-conditional line in that tree is a stack-size default (§8.2).

Two files make the board target work:
`apps/dongle_thread/boards/nrf52840dk_nrf52840.overlay` (the `radiant,radio-timer`
chosen node on `timer4` — TIMER0/1/2 have only four CC channels and the backend
needs five, and TIMER0 is MPSL's on this family anyway; the `ant-uart` alias on
`uart1`; four unused peripherals off) and
`apps/dongle_thread/boards/nrf52840dk_nrf52840.conf` (UART transport, console and
log on the J-Link VCOM instead of RTT). **`radiant/Kconfig` needed no change** —
its nrf arm already reads
`depends on SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX`.

**Where the flash goes** — `text` only, from `zephyr.map`. The parse totals
711 988 B against the section's 711 996 B: 8 bytes, **99.999 %**. (`rodata`,
77 540 B, is excluded; the same parser over-counts it.)

| Archive | text | note |
|---|---:|---|
| `libCHIP.a` | **213 770** | 30.0 % of all text |
| `libopenthread-mtd.a` | 85 426 | |
| `libmatter-data-model.a` | 79 550 | the ZAP output + ember |
| `libsoftdevice_controller_peripheral.a` | 36 662 | one object, `s112` |
| `libapp.a` | 31 730 | `apps/common` + this app's `src/` + NCS sample-common |
| `libsubsys__bluetooth__host.a` | 27 674 | commissioning BLE |
| `libnrf-802154-driver.a` | 26 320 | |
| `libsubsys__net__ip.a` | 24 306 | present because of the sockets finding, §4.3 |
| **`libradiant.a`** | **22 808** | **3.2 % of text. RadiANT is not the problem.** |
| `libzephyr.a` | 22 158 | |
| `libkernel.a` | 17 862 | |
| `liboberon_3.0.19.a` | 17 440 | |
| `libmpsl.a` | 16 096 | |
| `libpsa_core.a` | 13 892 | |
| everything else | ~76 000 | ~20 archives, none over 8 KB |

Largest single objects: the SDC blob (36 662), the MPSL blob (16 096),
`GroupDataProviderImpl.cpp` (15 840), `CASESession.cpp` (15 060), `mle.cpp`
(11 726), `psa_crypto.c` (10 768).

**Where the RAM goes** — `bss` + `noinit` + `datas` + `device_states` +
`k_heap_area`; **231 638 B of 233 816 B attributed, 99.1 %.** Byte for byte the
same shape as the nRF54L15 table in §2.5, which cross-checks both.

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

### 2.4 `CONFIG_HEAP_MEM_POOL_SIZE`: 24 584 bytes for a heap with no caller

Two lines in `matter.conf` took the vendored-core image from 96.57 % of RAM to
87.18 % — **8 988 B free → 33 612 B free.** Each step is a separate incremental
build and each figure is the linker's own `RAM: ... B  256 KB  ...%` line.

| Step | Line | RAM | Δ | FLASH | Δ |
|---|---|---:|---:|---:|---:|
| baseline | - | 253 156 | - | 681 400 | - |
| 1 | `CONFIG_CHIP_NFC_ONBOARDING_PAYLOAD=n` | 253 116 | **−40** | 680 256 | **−1 144** |
| 2 | `CONFIG_HEAP_MEM_POOL_SIZE=8192` | **228 532** | **−24 584** | 680 256 | 0 |
| | **total** | **228 532** | **−24 624** | **680 256** | **−1 144** |

**Step 2 is the finding.** `thread.conf` sets `CONFIG_HEAP_MEM_POOL_SIZE=32768`
and calls it "the heap OpenThread wants". On the Matter arm that is not what it
is:

```
zephyr/modules/openthread/platform/memory.c:13-15
    void *otPlatCAlloc(size_t aNum, size_t aSize) { return calloc(aNum, aSize); }
```

OpenThread allocates through **libc**, and
`CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE=y` redirects libc `malloc`/`calloc`
globally into CHIP's own 10 KB `sys_heap` (`__wrap_malloc` is at `0x00037ca4`).
So OpenThread's dynamic allocation comes out of `CHIP_MALLOC_SYS_HEAP_SIZE`, and
`HEAP_MEM_POOL_SIZE` — a different heap with a different API — does not serve it.
**Trap 10 is worse than first recorded: it is not only `apps/common` and the ANT
stack sharing CHIP's 10 KB, it is OpenThread too.** `thread.conf`'s own comment
is therefore inaccurate on any arm that overrides libc `malloc`, and at best
unproven on the others.

What *can* reach `_system_heap` was checked, not assumed. Disassembling the ELF,
the object at `0x200013c0` is referenced from exactly four places:

```
statics_init                  init_mem_slab_obj_core_list
k_thread_system_pool_assign   z_thread_alloc_helper
```

`z_thread_alloc_helper`'s only caller is `z_thread_malloc`, whose only caller is
`queue_insert`'s alloc path — `k_queue_alloc_append`/`_prepend`. And `nm` on the
final ELF finds **none** of `k_malloc`, `k_calloc`, `k_realloc`, `k_heap_alloc`,
`k_queue_alloc_append`, `k_object_alloc` or `k_thread_stack_alloc`. The only
archive the linker pulled in *because of* a heap reference is `net_buf`'s
`buf.c` (`k_heap_alloc`), and `CONFIG_NET_BUF_FIXED_DATA_SIZE=y` means its pools
are static. Zephyr's own required-minimum machinery agrees: **no subsystem in
this build declares a `CONFIG_HEAP_MEM_POOL_ADD_SIZE_*` symbol at all**, so the
computed requirement is zero.

8192 rather than 0 is margin, not a measurement. **The `nm` check must be re-run
whenever new code lands** — the command is in `matter.conf` beside the line —
because a kernel heap that is too small fails at runtime in an image that
configures, compiles, links and boots. It was re-run after E4-E6 and on the
nRF52840 build and holds on both. The line is **restated in `matter.conf` rather
than edited in `thread.conf`**, so the non-Matter arms keep the images their
section 7.4 numbers were taken on.

**Step 1, NFC, is 40 bytes today** and is kept for a forward reason:
`--gc-sections` has already discarded the whole `nfc_t2t` / NDEF chain because
nothing calls `NFCOnboardingPayloadManagerImpl` until `main()` exists. Once
`main()` reaches `Server::Init()`, NCS's `matter_event_handler.cpp` (compiled
unconditionally by `source_common.cmake`) calls `ShareQRCodeOverNFC()` and
`libnfc_t2t.a`'s buffers plus the NFCT driver become reachable. This line means
they never arrive. The symbol's own `default n` in `chip-module/Kconfig` is
overridden by `chip-module/Kconfig.defaults:232`'s `default y` — reading only the
first file would tell you it was already off.

### 2.5 Where the nRF54L15's RAM goes, by symbol

Measured at the vendored-core stage (253 156 B). The `.map` attributes 249 628 B
(98.6 %) to a named object. `noinit` is where Zephyr puts thread stacks and the
kernel heap; `bss` and `datas` are the rest.

| # | Bytes | % of RAM | Symbol | Sect | Object | Sized by |
|---:|---:|---:|---|---|---|---|
| 1 | **32 860** | 12.54 % | `kheap_buf__system_heap` | noinit | `libkernel.a(mempool.c)` | `CONFIG_HEAP_MEM_POOL_SIZE` = 32768 |
| 2 | **19 500** | 7.44 % | `chip::System::PacketBuffer::sBufferPool` | bss | `libCHIP.a(SystemPacketBuffer)` | `CONFIG_CHIP_SYSTEM_PACKETBUFFER_POOL_SIZE` = 15 × 1300 B |
| 3 | **15 304** | 5.84 % | `chip::Server::sServer` | bss | `libmatter-data-model.a(Server.cpp)` | `CHIP_MAX_FABRICS`, `CHIP_MAX_ACTIVE_CASE_CLIENTS`, `CHIP_MAX_ACTIVE_DEVICES` |
| 4 | **11 736** | 4.48 % | `ot::gInstanceRaw` | bss | `libopenthread-mtd.a(instance.cpp)` | OpenThread message/child/router tables |
| 5 | **10 240** | 3.91 % | `sHeapMemory` (CHIP's malloc arena) | bss | `libCHIP.a(SysHeapMalloc)` | `CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` — **trap 10** |
| 6 | **10 096** | 3.85 % | `chip::app::sInteractionModelEngine` | bss | `libCHIP.a(InteractionModelEngine)` | `CHIP_IM_MAX_NUM_*` — **not Kconfig-exposed** |
| 7 | **9 216** | 3.52 % | CHIP event-loop thread stack | noinit | `libCHIP.a(PlatformManagerImpl)` | `CONFIG_CHIP_TASK_STACK_SIZE` (9216 because CRACEN) |
| 8 | **8 192** | 3.13 % | `mbedtls_heap` | bss | `libmbedtls_zephyr.a` | `CONFIG_MBEDTLS_HEAP_SIZE` |
| 9 | **6 144** | 2.34 % | OpenThread thread stack | noinit | `libopenthread_utils.a(openthread.c)` | `CONFIG_OPENTHREAD_THREAD_STACK_SIZE` |
| 10 | **6 144** | 2.34 % | `api_xfer` | bss | `libapp.a(ant_radio_radiant.c)` | ours — the ANT host transfer buffers |
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

By archive: `libCHIP.a` 61 363 · `libkernel.a` 43 234 (32 860 of it line 1) ·
`libapp.a` 30 253 · `libmatter-data-model.a` 20 428 (15 304 of it `sServer`) ·
`net/ip` 12 504 · `libopenthread-mtd.a` 11 742 · `libzephyr.a` 10 649 ·
`bluetooth/host` 9 822 · **`libradiant.a` 9 768 — 3.7 % of the image** ·
`libmbedtls_zephyr.a` 8 192 · `libopenthread_utils.a` 6 340 ·
`libnrf-802154-driver.a` 5 922 · everything else ~19 400 across 20 archives, none
over 3.2 KB.

**Read line 1 and line 5 together.** The image carries two general-purpose heaps
of 32 KB and 10 KB plus an 8 KB mbedTLS heap — 51 KB, 20 % of the part — and the
32 KB one has no caller.

**Method.** `ram_report` cannot answer this on its own: it groups by **source
path**, so 42 484 B land in `(hidden)` and 36 950 B in `(no paths)` — 79 434 B,
31 % of the image, in two buckets with no owner, and every CHIP static that
matters is in them. Use the `.map`. Note also that `west build -t
dongle_thread:ram_report` does not work, nor does pointing `-d` at the
application sub-directory: west resolves the domain back to the top-level build
directory either way and ninja reports `unknown target`. Run
`ninja -C <build>\dongle_thread ram_report` directly.

### 2.6 Levers measured and deliberately not taken

Both were built and measured in a probe (RAM 216 028 B / 82.41 %) and reverted,
so they can be taken later with bench evidence rather than under pressure.

| Lever | From → to | Measured saving | Why not taken |
|---|---|---:|---|
| `CONFIG_CHIP_SYSTEM_PACKETBUFFER_POOL_SIZE` | 15 → 8 | **9 100 B** (`sBufferPool` 19 500 → 10 400; 1 300 B/buffer, exactly) | CHIP's only message buffer supply, shared by BTP commissioning and every Thread exchange. Exhaustion is a dropped message, not a crash — a runtime failure a `.config` read-back cannot see, on the one code path that allocates hardest. 15 is what Nordic ships and tests in every Matter sample including `matter_bridge`. |
| `CONFIG_CHIP_MAX_ACTIVE_CASE_CLIENTS` / `_MAX_ACTIVE_DEVICES` | 5 → 2 | **3 408 B** (`sServer` 15 304 → 11 896) | Both default to `CHIP_MAX_FABRICS` *because* `CHIP_PERSISTENT_SUBSCRIPTIONS=y`: they size the outgoing-CASE pools that resume subscriptions after a reboot. At 2, a device joined to 5 fabrics cannot resume all of them — a correctness regression against documented Matter behaviour, for 1.3 % of the part. |

Not touched and not measured:

| Symbol (current value) | Worth | Reason it stays |
|---|---|---|
| `CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` (10 240 B) | — | Trap 10, and §2.4 made it *more* load-bearing. If anything it goes **up**, and that is a measurement (`CONFIG_SYS_HEAP_RUNTIME_STATS` + a bench run). |
| `CONFIG_CHIP_MAX_FABRICS=5` | `sServer` + `gGroupPeerTable`, and eases §4.4 | 5 is the Matter specification's minimum for a commissionable device. |
| `CONFIG_CHIP_TASK_STACK_SIZE` (9 216 B) and every thread stack | up to ~4 KB across CHIP/OpenThread/BT | Needs a real high-water mark (`CONFIG_INIT_STACKS` + `kernel stacks` on a bench run). |
| `CONFIG_MBEDTLS_HEAP_SIZE` (8 192 B) | — | Serves OpenThread's DTLS: a runtime number with no static bound. |
| `CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA` (3 840 B pool) | ~1.3 KB at `EXTRA=2` | Nordic sizes it `6 - (BT_MAX_CONN + 1)` (`Kconfig.defaults:198`); it changes HCI flow-control credits on the one link commissioning runs over. |
| `CONFIG_ZVFS_OPEN_MAX=30` (1 440 B `fdtable`) | ~670 B at 16 | CHIP's `Kconfig.defaults:48` sets it; declared `ZVFS_OPEN_ADD_SIZE_*` contributions total 8. Socket exhaustion only appears under load. |
| `CONFIG_LOG_BUFFER_SIZE` (4 048 B), `CONFIG_MAIN_STACK_SIZE`, `CONFIG_NET_*_STACK_SIZE`, the 802.15.4 and net_pkt pools, `CONFIG_MPSL_TIMESLOT_SESSION_COUNT=3` (96 B) | — | All either `thread.conf`'s or the board's, and inside what the P3/P4 coexistence arms measure. |

`source_common.cmake`'s statics are not a lever either. Every surviving `Nrf::`
symbol, `nm -C --print-size`:

```
00066176   2  T  Nrf::LEDWidget::Set(bool)
00066178   2  T  Nrf::LEDWidget::Blink(unsigned int, unsigned int)
00010998  68  T  Nrf::Board::UpdateStatusLED()
20005f20 376  B  Nrf::Board::sInstance
200003b0   8  D  Nrf::Matter::InitData::sDeviceInfoProviderDefault
200003b8  16  D  Nrf::Matter::InitData::sOperationalKeystoreDefault
```

`board.cpp` costs 376 B of bss and 68 B of text — its LED paths are all inside
`#ifdef CONFIG_DK_LIBRARY`, which is absent, and `led_widget.cpp` reduces to two
2-byte stubs. `matter_init.cpp` adds 528 B directly, 504 of it
`sThreadNetworkDriver`. The 61 363 B of `libCHIP.a` RAM is not these six symbols;
it is CHIP's singletons retained through the reference graph those two `.data`
statics root. **The root is 24 bytes and the tree is 61 KB**, which is why
"delete the roots" is not a strategy and "size the singletons" is.

### 2.7 The On/Off Light endpoint is zero bytes of RAM, and must not be deleted

**Zero bytes today.** The whole ember attribute store is discarded.
`attribute-storage.cpp.obj` in `libmatter-data-model.a` carries 8 213 B text /
684 B data / 823 B bss in the archive and contributes **0 B to the final image**,
because `emberAfEndpointConfigure()` is called from `Server::Init()`.

**What it would be worth once `Server::Init()` runs**, read off
`src/matter/default_zap/zap-generated/endpoint_config.h`:

```
GENERATED_ENDPOINT_TYPES  { ZAP_CLUSTER_INDEX(0), 14,  19 },   <- endpoint 0
                          { ZAP_CLUSTER_INDEX(14), 2,   0 },   <- endpoint 1, aggregator
                          { ZAP_CLUSTER_INDEX(16), 11, 702 }   <- endpoint 2
ATTRIBUTE_MAX_SIZE (721)             ZAP_FIXED_ENDPOINT_DATA_VERSION_COUNT 24
```

Endpoint 2 owns 702 of the 721 bytes of `attributeData[]` and 11 of the 24
`DataVersion` slots, so deleting it would be worth about 702 + 44 = **746 B of
bss**, plus ~112 B across the three `CodegenIntegration.cpp` `gServers[]` arrays
(sized `FIXED_ENDPOINT_COUNT + dynamic` = 3 + 16, at 25-44 B a slot), plus
`on-off-server.cpp`'s 148 B of bss. Call it **~1 KB of RAM and ~4.2 KB of flash**
(`on-off-server.cpp` 2 621 B + `groups-server.cpp` 1 560 B of text).

**And it must not be deleted, because it is the bridged-device template.**
`bridge.matter:2394` onwards, endpoint 2 declares `binding: Identify, OnOff` and
`server: Identify, Groups, OnOff, Descriptor, Binding,
BridgedDeviceBasicInformation, Switch, TemperatureMeasurement,
RelativeHumidityMeasurement`. Six of those are exactly the clusters the dynamic
endpoints expose, and they are compiled into the image **because this endpoint
declares them** — the generated `Accessors.cpp` (126 141 B) and
`cluster-objects.cpp` (130 572 B) are keyed on the ZAP's cluster set, not on
which endpoint uses them. Device type `0x0100` On/Off Light is upstream's way of
getting ZAP to accept that palette on one endpoint. The 702 bytes are not
On/Off's either: they are dominated by `BridgedDeviceBasicInformation`'s string
attributes (`NodeLabel`, `VendorName`, `ProductName`, `SerialNumber`…), while
`OnOff` + `Groups` + `Switch` are a few tens of bytes between them.

**A future ZAP session must therefore do this, and not "remove endpoint 2":**

1. **Open `src/matter/default_zap/bridge.zap` in the ZAP GUI**, which is not in
   the `dcbdc366a1` toolchain bundle (trap 14). `zap_download.py` /
   `zap_bootstrap.sh` under `modules/lib/matter/scripts/tools/zap/` fetch it;
   there is no `west zap` in NCS v3.4.0.
2. **On endpoint 2, remove only `OnOff`, `Groups` and `Switch`** (server and
   binding). Keep the other six. **The device type will then have to change** —
   `0x0100` no longer validates. Upstream's own answer is to leave the endpoint
   as a template.
3. **Do NOT add `OccupancySensing` to the ZAP** (trap 19). If it is declared,
   `occupancy-sensor-server.cpp` compiles and its init callback runs on every
   dynamic endpoint, calling `emberAfWriteAttribute`, which routes straight back
   into the application's own external-storage write callback. `Occupancy`,
   `OccupancySensorType` and `OccupancySensorTypeBitmap` are served from the
   shadow instead.
4. **Regenerate `.zap`, `.matter` AND `zap-generated/` together** (trap 13) and
   re-read `FIXED_ENDPOINT_ARRAY`, `FIXED_DEVICE_TYPES`, `ATTRIBUTE_MAX_SIZE` and
   `ZAP_FIXED_ENDPOINT_DATA_VERSION_COUNT` out of the new `endpoint_config.h`.
   Expect `ATTRIBUTE_MAX_SIZE` to fall by a few tens of bytes, not by 702.

**Budget honestly: a flash job worth ~4 KB, not a RAM job.**

---

## 3. The vendored NCS files — a maintenance liability list

`apps/dongle_thread/src/matter/`, from NCS v3.4.0, toolchain bundle
`dcbdc366a1`. Its `README.md` is the provenance record; every upstream licence
header is untouched.

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

**Non-obvious dependencies, each a link-time or configure-time failure if
forgotten:**

- **`core/util/bridge_util.h`** is a header-only device factory template with no
  `.cpp`, and it is `#include "bridge_util.h"` from `bridge_manager.h` — so
  `core/util/` has to be on the include path in its own right.
- **`nrf/samples/matter/common/src/binding/binding_handler.cpp`** is **referenced
  in place, not copied.** `BridgeManager::Init()` and `HandleCommand()` both need
  it, it is NCS's file rather than the bridge's, it is on the include path
  anyway, and upstream's own `CMakeLists.txt` does the same.
- **`CONFIG_CHIP_ENABLE_READ_CLIENT=y`** — `binding_handler.cpp` needs it.
- **`CONFIG_NCS_SAMPLE_MATTER_PERSISTENT_STORAGE=y`** — without it the vendored
  `bridge_storage_manager.cpp` compiles and fails to link.
- **`CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH`** is the single line that decides
  which data model the image contains. Unset, `ncs_get_zap_parent_dir()` returns
  an empty parent and the build reaches for whatever
  `zap-generated/endpoint_config.h` the include path finds first.
- **`CONFIG_CHIP_PROJECT_CONFIG`** points at `src/matter/chip_project_config.h`.
- **`CONFIG_STD_CPP17=y`.** connectedhomeip's GN build passes `-std=gnu++17`;
  Zephyr's flags are appended after it and carry `-std=gnu++11` from the default
  arm of the `STD_CPP` choice, and the last `-std` wins. The failure is ~40 lines
  of `'underlying_type_t' in namespace 'std' does not name a template type` in
  `ErrorStr.cpp`, which reads like a broken toolchain. Every NCS Matter sample
  sets this in its own `prj.conf`; nothing sets it for you.
- **`CONFIG_CHIP_FACTORY_DATA_NONE=y`.**
  `SB_CONFIG_MATTER_FACTORY_DATA_GENERATE=n` only stops factory data being
  *generated*. Which provider the image *consumes* is a separate Kconfig choice
  defaulting to `CHIP_FACTORY_DATA_NRFCONNECT_BACKEND`, which
  `select CHIP_FACTORY_DATA`, which needs a `factory_data` devicetree partition.
  Without this line the build fails in `FactoryDataProvider.h:40-41` on
  `DT_N_NODELABEL_factory_data_partition_REG_IDX_0_VAL_SIZEU` — a devicetree
  error message for a Kconfig problem.
- **`CONFIG_EVENTFD=y`** — see §4.3.

**Left behind on purpose.** The entire BLE-central provider (`src/ble/` —
`ble_connectivity_manager.cpp`, `ble_bridged_device_factory.cpp`, both BLE data
providers and the eleven `BRIDGE_BT_*` Kconfig symbols), because that subsystem
bridges *BLE* sensors and this product bridges *ANT+*; it is not present to be
enabled, so `CONFIG_BRIDGED_DEVICE_BT` is ABSENT rather than `n`. Also
`src/simulated_providers/` (shell-driven fakes); `src/bridged_device_types/` (all
five upstream types); `app_task.cpp`, `main.cpp`, `bridge_shell.cpp` and
`zcl_callbacks.cpp` (`main()` is ours, and `zcl_callbacks.cpp` is entirely inside
`#ifdef CONFIG_BRIDGE_SMART_PLUG_SUPPORT`); and `src/onoff_plug_zap/`, because a
second data model nothing selects is a second thing to keep in sync (trap 13).
**Matter's commissioning BLE is untouched and still on** — `CONFIG_BT=y`, and
`bridge_storage_manager.h` still includes `<zephyr/bluetooth/addr.h>`.

**Licensing.** Ten vendored headers fail `scripts/check_license.py`'s
Apache-2.0 SPDX assertion: five `core/*.h` carry `LicenseRef-Nordic-5-Clause`,
and five `zap-generated/*.h` **are** Apache-2.0 but ZAP's templates emit the long
licence text with **no SPDX line**. Retagging somebody else's file to satisfy a
checker would be a false licence claim — the exact failure that script exists to
catch, pointed the other way — so all ten are on its `ALLOWLIST` by name, each
with the licence it actually carries. The exemption did not have to cover the
vendored `.cpp` files, because `SOURCE_SUFFIXES` contains `.c` and not `.cpp`;
that is a pre-existing gap, and widening the suffix set changes what the whole
tree is asserted about. `docs/third-party.md` carries the redistribution
obligation — the second `LicenseRef-Nordic-5-Clause` entry after
`radiant/coex154_ti/`.

**The ZAP directory is genuinely the one the build consumes**, proven two
independent ways rather than inferred from `BYPASS_IDL`:

1. The three committed `.cpp` files compile from this repository's path into the
   `matter-data-model` target — `callback-stub.cpp.obj`,
   `CodeDrivenInitShutdown.cpp.obj`, `IMClusterCommandHandler.cpp.obj` under
   `modules/connectedhomeip/CMakeFiles/matter-data-model.dir/C_/Users/Colin/ant_dongle/apps/dongle_thread/src/matter/default_zap/zap-generated/`.
2. CHIP's own `attribute-storage.cpp` does
   `#include <zap-generated/endpoint_config.h>`, and its compile line in
   `build.ninja` carries `-I.../src/matter/default_zap` and
   `-I.../src/matter/default_zap/zap-generated` **ahead of everything in
   `modules/lib/matter`**. There is no other `endpoint_config.h` it could resolve
   to.

Corroborating: the build compiles clusters only this ZAP asks for —
`bridged-device-basic-information-server.cpp` is in `matter-data-model` and is in
no NCS build that is not a bridge. `chip_zapgen` did not run, which is what
`BYPASS_IDL` is for, so the committed directory is the artefact rather than a
cache of one. The model itself, read back from `endpoint_config.h` rather than
from the `.zap`:

```
FIXED_ENDPOINT_ARRAY  { 0x0000, 0x0001, 0x0002 }
FIXED_DEVICE_TYPES    { 0x12, 1 }, { 0x16, 4 }, { 0x0E, 2 }, { 0x0100, 3 }
```

Endpoint 0 = OTA Requestor + Root Node, **endpoint 1 = Aggregator (`0x000E`)**,
endpoint 2 = the bridged-device template of §2.7, which `BridgeManager::Init()`
disables.

---

## 4. Kconfig read-back, and the traps in it

`ABSENT` means the symbol is not defined at all in that build's Kconfig tree — a
different and more informative answer than `n`.

| Symbol | nRF54L15 Matter arm | nRF52840 | nRF5340 cpuapp (CHIP-only) | control (`bridge.conf`) |
|---|---|---|---|---|
| `CONFIG_CHIP` | **y** | **y** | **y** | not set |
| `CONFIG_ANT_DONGLE_MATTER` | y | y | y | ABSENT (`depends on CHIP`) |
| `CONFIG_ANT_DONGLE_MATTER_MAP` | **y** | y | — | **not set** |
| `CONFIG_ANT_DONGLE_PROFILES_EXTRA` | **y** | — | — | **not set** |
| `CONFIG_BT` | **y** (never written by hand) | **y** | **y** | — |
| `CONFIG_MPSL` | y | y | ABSENT (cpunet only) | y |
| `CONFIG_MPSL_TIMESLOT_SESSION_COUNT` | **3** | **3** | 0 (invisible) | 2 |
| `CONFIG_RADIANT_BACKEND_NRF` | **y** | **y** | ABSENT | y |
| `CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL` | **y** | **y** | not set | y |
| `CONFIG_RADIANT_BACKEND_NULL` | not set | **not set** | y | not set |
| `CONFIG_SOC_COMPATIBLE_NRF52X` | — | **y** | — | — |
| `CONFIG_RADIANT_BRIDGE` | y | y | y | y |
| `CONFIG_STD_CPP17` / `CONFIG_EVENTFD` | y / y | — | y / y | — |
| `CONFIG_CHIP_USE_OPENTHREAD_ENDPOINT` | **ABSENT** | — | **ABSENT** | — |
| `CONFIG_PICOLIBC` | y | — | y | — |
| `CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE` / `_SIZE` | y / 10240 | — | y / 10240 | — |
| `CONFIG_HEAP_MEM_POOL_SIZE` | **8192** (was 32768) | **8192** | 32768 | — |
| `CONFIG_CHIP_NFC_ONBOARDING_PAYLOAD` | **not set** (was y) | — | y | — |
| `CONFIG_NFC_T2T_NRFXLIB` | not set | — | — | — |
| `CONFIG_CHIP_SYSTEM_PACKETBUFFER_POOL_SIZE` | 15 | — | — | — |
| `CONFIG_CHIP_MAX_FABRICS` / `_ACTIVE_CASE_CLIENTS` / `_ACTIVE_DEVICES` | 5 / 5 / 5 | — | — | — |
| `CONFIG_CHIP_PROJECT_CONFIG` | `"src/matter/chip_project_config.h"` | — | — | ABSENT |
| `CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH` | `"${APPLICATION_CONFIG_DIR}/src/matter/default_zap/bridge.zap"` | — | — | ABSENT |
| `CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER` | **16** | — | — | ABSENT |
| `CONFIG_BRIDGE_MAX_BRIDGED_DEVICES_NUMBER` / `_PER_PROVIDER` | 16 / 2 | — | — | ABSENT |
| `CONFIG_BRIDGE_AGGREGATOR_ENDPOINT_ID` | **1** | — | — | ABSENT |
| `CONFIG_BRIDGE_MIGRATE_VERSION_1` | **not set** (was y) | — | — | ABSENT |
| `CONFIG_BRIDGED_DEVICE_BT` | **ABSENT** (menu not vendored) | — | — | ABSENT |
| `CONFIG_NCS_SAMPLE_MATTER_PERSISTENT_STORAGE` | y | — | — | ABSENT |
| `CONFIG_CHIP_ENABLE_READ_CLIENT` | y | — | — | ABSENT |
| `CONFIG_CHIP_FACTORY_DATA_NONE` | y | — | — | ABSENT |
| `CONFIG_DK_LIBRARY` | **ABSENT** | — | — | ABSENT |
| `CONFIG_CHIP_OTA_REQUESTOR` / `_FACTORY_DATA` / `BOOTLOADER_MCUBOOT` | not set | not set | not set | — |
| `CONFIG_FLASH_LOAD_OFFSET` / `_SIZE` | — | 0 / 0 (app owns the whole 1 MB) | — | — |
| settings backend | `SETTINGS_ZMS=y` (RRAM) | | `SETTINGS_NVS=y` | |

### 4.1 `CONFIG_BT=y` arrives by itself, and the backend survives it

Nothing in `matter.conf`, `thread.conf` or `prj.conf` says `CONFIG_BT=y`. It is
`connectedhomeip/config/nrfconnect/chip-module/Kconfig.defaults:152` —
`config BT / default y` inside `if CHIP`. That makes the `!BT` half of
`radiant/Kconfig:84` unmet, and only `RADIANT_BACKEND_NRF_GATE_MPSL=y` keeps the
backend off NULL. The read-back confirms `RADIANT_BACKEND_NRF=y` with
`RADIANT_BACKEND_NULL` not set on both the nRF54L15 and the nRF52840, so the
silent fallback this project has shipped three times did not happen (trap 3).

### 4.2 `radiant` survives `--gc-sections`

The flash spike's failure mode was a green Matter build whose entire radiant
archive was discarded (a 596-byte image). `nm` on the final ELFs rules it out.
nRF54L15, E4-E6:

```
radiant_radio_init          KEPT     ant_matter_start             KEPT
radiant_sched_tick          KEPT     ant_matter_note_sample       KEPT
radiant_matter_row          KEPT     ant_matter_binding_changed   KEPT
radiant_matter_convert      KEPT     radiant_matter_sink          KEPT
radiant_matter_endpoint_for KEPT     radiant_matter_glue_sink     KEPT

chip:: symbols            3599       Nrf bridge-core symbols        71
```

nRF52840, plus **4 700 demangled `chip::` symbols** and the sink section placed
(`_radiant_sink_list_start` 0x000c2bb0, `_radiant_sink_list_end` 0x000c2bf0 —
four entries, 64 bytes):

```
00034fc4 T radiant_radio_init          000c2bb0 D radiant_liveness_sink
00032f94 T radiant_sched_tick          000c2bc0 D radiant_matter_glue_sink
0001297c T radiant_matter_row          000c2bd0 D radiant_matter_sink
00078388 T radiant_matter_convert      000c2be0 D radiant_rules_sink
000129dc T radiant_matter_endpoint_for
0001528c T ant_matter_start
```

**3 599 and 71 are the headline.** The CHIP-only stage measured zero `chip::`
symbols and the vendored-core stage zero `Nrf::Bridge*` symbols, and in both
cases the build was green. Only the E4-E6 ELF has the Matter plane *reachable*
rather than merely linkable.

### 4.3 CHIP is on its Zephyr-sockets path here, not the one Nordic tests

`connectedhomeip/config/nrfconnect/chip-module/Kconfig:475-478`:

```
config CHIP_USE_OPENTHREAD_ENDPOINT
	bool "Use OpenThread TCP/UDP stack directly"
	default y
	depends on OPENTHREAD && !WIFI && !NETWORKING
```

`thread.conf` sets `CONFIG_NETWORKING=y` and has to: `mqtt_sink.c` needs
Zephyr sockets on top of OpenThread. So `!NETWORKING` is unmet,
`CHIP_USE_OPENTHREAD_ENDPOINT` is **ABSENT**, and
`chip_system_config_use_sockets = true` appears in the generated `args.gn`.
**Every NCS Matter sample builds the other way** — no Zephyr IP stack at all,
CHIP talking to OpenThread's own UDP directly.

The immediate consequence is `CONFIG_EVENTFD=y`: `SystemConfig.h:819-824` only
reaches for `<zephyr/posix/sys/eventfd.h>` when `CONFIG_EVENTFD` is defined and
reaches for a nonexistent bare `<sys/eventfd.h>` otherwise, so
`src/system/WakeEvent.cpp:44` fails to compile without it. The larger consequence
is that this arm carries a whole Zephyr IP stack CHIP does not need (12 504 B of
RAM, 24 306 B of text) and its UDP path through `UDPEndPointImplSockets` is a
different code path from the samples'. Layering `matter.conf` on `thread.conf` is
what produces this, and it is right while this arm has to stay comparable to the
coexistence arms — but it should be decided again, explicitly, when it stops
having to.

### 4.4 Storage partitions, from the generated `zephyr.dts`

nRF54L15 DK: `storage_partition` at `0x174000 + 0x9000` = **36 KB** (ZMS).
nRF5340 DK cpuapp: `0x0f8000 + 0x8000` = **32 KB** (NVS). Neither board has a
`factory_data` partition at all. Fabrics + ACLs + the OT dataset + radiant's
settings in 32 KB is a partition-layout job that is entirely ahead, and
`CONFIG_CHIP_MAX_FABRICS` is the symbol that couples RAM to it.

### 4.5 `CONFIG_NCS_SAMPLE_MATTER_LEDS=n` is a no-op, twice over

`NCS_SAMPLE_MATTER_LEDS depends on DK_LIBRARY` and this arm does not enable
`DK_LIBRARY`, so the symbol is invisible; and `board.cpp` is compiled for real
with *both* of its LED guards false, so the collision the line was written to
prevent (NCS's `board.cpp` driving DK LEDs 0-3 against
`apps/common/ant_heartbeat_led.c`'s `led0`) cannot occur. The line stays as a
record of the decision; it is not what prevents the collision. A second owner of
`led0` would show up as a flash pattern that does not match
`CONFIG_ANT_DONGLE_HEARTBEAT_LED_ON_MS`/`_OFF_MS`, rather than as nothing.

### 4.6 CHIP's Kconfig zeroes radiant's own log level

`chip-module/Kconfig.defaults` supplies a `LOG_DEFAULT_LEVEL` that lands ahead of
radiant's, so every Matter build to date read back `CONFIG_RADIANT_LOG_LEVEL=1`
— errors only — from every `LOG_MODULE_REGISTER(..., CONFIG_RADIANT_LOG_LEVEL)`
in `radiant/`. `docs/g-hardware-bringup.md` is the canonical write-up and carries
the fix.

---

## 5. The `static_assert` pin

`apps/dongle_thread/src/matter/bridge_core_asserts.cpp` is a translation unit
containing nothing but assertions, because the two numbers live on opposite sides
of a language boundary: `chip_project_config.h` is pulled into CHIP's headers
before anything of RadiANT's exists, and `radiant_matter.h` is a C header the
vendoring must not edit.

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
not two undefined macros comparing equal to each other as zero.

The pair being guarded is the CHIP side against `RADIANT_MATTER_MAX_ENDPOINTS`.
`CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT` and
`CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER` cannot drift — upstream defines the
first *as* the second. Not pinned, and confirmed as a fact rather than fixed:
**the aggregator really is endpoint 1 in this ZAP and `radiant_matter.c`'s
`MATTER_ENDPOINT_FIRST` really is `1u`** (trap 6).

---

## 6. The `.config` regression gate

### 6.1 The arms, and why there are six

`.github/workflows/release.yml`'s `build-new-apps` job builds six configurations
of this application: the five coexistence arms `apps/dongle_thread/Kconfig` names
— `gate.conf`, `thread.conf`, `thread_sed.conf`, `coex.conf`, `bridge.conf` —
plus a `plain` row with no `EXTRA_CONF_FILE` and `-DRADIANT_BACKEND=null`.
`scripts/build_all.ps1` builds none of them (its matrices are `apps/dongle` /
`apps/dongle_ti` and `apps/hrm_ble`); `scripts/build_p4.ps1` builds three
(`p4ctrl`/`gate.conf`, `p4med`/`thread.conf`,
`p4sed`/`thread.conf;thread_sed.conf`, each with
`-DCONFIG_RADIANT_SWEEP_DEBUG=y`, which is a property of that bench sitting
rather than of the images). `plain` is not a coexistence baseline, but it is an
existing CI arm and the only one on the null backend; excluding it on the grounds
of a naming convention would be exactly the kind of silent narrowing that has
disarmed checks in this repository before. All six are gated.

| Arm | `-DANT_RADIO` | `-DRADIANT_BACKEND` | `EXTRA_CONF_FILE` |
|---|---|---|---|
| `plain` | `core` | `null` | *(none)* |
| `gate` | `core` | `nrf` | `gate.conf` |
| `med` | `core` | `nrf` | `thread.conf` |
| `sed` | `core` | `nrf` | `thread.conf;thread_sed.conf` |
| `coex` | `core` | `nrf` | `coex.conf` |
| `bridge` | `core` | `nrf` | `thread.conf;bridge.conf` |

All on `nrf54l15dk/nrf54l15/cpuapp`, all `-p always`, no `-DSB_EXTRA_CONF_FILE`
on any. The file compared is `<out>/<arm>/dongle_thread/zephyr/.config` — the
image's, under the sysbuild layout, not the sysbuild one. One normalisation is
required when diffing across a `git worktree`: Kconfig writes the module's own
path into `.config` as a comment
(`# radiant (<tree>/apps/dongle_thread/../../radiant)`), which is the worktree's
location and not a change.

### 6.2 No symbol changed value, but the diff is not empty

Against baseline `244857e`:

| Arm | base lines | head lines | added | removed | net new content | symbols relocated |
|---|---:|---:|---:|---:|---|---:|
| `plain` | 2607 | 2608 | 29 | 28 | one blank line | 28 |
| `gate` | 2617 | 2618 | 29 | 28 | one blank line | 28 |
| `med` | 3544 | 3545 | 51 | 50 | one blank line | 50 |
| `sed` | 3545 | 3546 | 51 | 50 | one blank line | 50 |
| `coex` | 3135 | 3136 | 47 | 46 | one blank line | 46 |
| `bridge` | 3588 | 3590 | 52 | 50 | + `# CONFIG_ANT_DONGLE_MATTER_MAP is not set` | 51 |

Compared as *sets* of lines, five arms differ by one blank line and nothing else.
**No symbol changed value; no symbol was added or removed except
`ANT_DONGLE_MATTER_MAP` on `bridge`**, which is a comment line predicted by the
`if RADIANT_BRIDGE` wrap. `# CONFIG_CHIP is not set` is present in all six, which
is what makes that sentence mean something.

**`CONFIG_NCS_SAMPLE_MATTER_LEDS` and
`CONFIG_NCS_SAMPLE_MATTER_TEST_EVENT_TRIGGERS` appear in NONE of the twelve
`.config` files, at any value, in any form** — zero occurrences of the string
`NCS_SAMPLE_MATTER`. Kconfig omits a symbol from `.config` when its dependencies
are unmet rather than writing the commented form, and `if CHIP` with
`CONFIG_CHIP` not set is exactly that.

**But 28 to 51 symbols moved position in every arm**, and that is a real property
of the change rather than generator noise. `apps/dongle_thread/Kconfig`
`osource`s three files — connectedhomeip's `Kconfig.features` and
`Kconfig.defaults` and NCS's `samples/matter/common/src/Kconfig` — **before**
`source "Kconfig.zephyr"`. Kconfig's writer emits each symbol once, at the FIRST
node in the menu tree that defines it, and those three files are full of
`config X / default …` overrides for symbols Zephyr and NCS define elsewhere:
`BT` at `Kconfig.defaults:152`, `OPENTHREAD` at `:278`, `SHELL` at `:507`,
`MBEDTLS_HEAP_SIZE` at `:378`, `ZVFS_OPEN_MAX` at `:48`,
`SPI_NOR_FLASH_LAYOUT_PAGE_SIZE` at `Kconfig.features:31`. Each one's first
definition node is now inside this application's Kconfig, so its line moves to
the top of the file. `TRUSTED_STORAGE_BACKEND_AEAD_MAX_DATA_SIZE` moves too and
is not an `NCS_SAMPLE_MATTER_*` symbol at all — it comes from
`nrf/samples/matter/common/src/persistent_storage/Kconfig:42`, reached through
that file's `rsource`; its value (256) is Zephyr's own and did not change.

**`if CHIP` does not prevent this and cannot**: Kconfig's `if` adds a dependency,
it does not hide a definition, and a symbol's *value* is computed globally from
all its nodes — which is why the values are unchanged. **So `diff
zephyr/.config` is not a usable regression gate for this application unless the
comparison is order-insensitive**, and any future baseline comparison has to say
so. Sourcing the three files after `source "Kconfig.zephyr"` would restore
positional stability.

### 6.3 The image gate, which a `.config` diff cannot see

`plain`, `gate`, `med`, `sed`, `coex`: **allocated sections byte-identical and
the defined-symbol set identical**, base against head, on
`arm-zephyr-eabi-size -A` and `arm-zephyr-eabi-nm --defined-only`. Only
`.debug_line` and `.debug_str` differ, by ~250-550 B, because the baseline
worktree lives at a longer filesystem path than the repository and
`-fmacro-prefix-map` does not reach debug info — the same allocated-sections-only
rule the `zero-cost` CI job argues for.

**`bridge` was not**, because the seven new profile/adapter sources were added
inside `if(CONFIG_RADIANT_BRIDGE)` and `bridge.conf` is the one existing arm that
sets it:

| | base | head | delta | **after the fix** | residual |
|---|---:|---:|---:|---:|---:|
| FLASH | 307 496 B (19.70 %) | 310 060 B (19.87 %) | +2 564 | **307 608 B** | **+112 B** |
| RAM | 185 672 B (70.83 %) | 187 144 B (71.39 %) | +1 472 | **186 184 B** | **+512 B** |
| `text` | 280 444 | 282 628 | +2 184 | **280 540** | **+96** |
| `rodata` | 19 808 | 20 196 | +388 | **19 824** | **+16** |
| `bss` | 78 882 | 80 356 | +1 474 | **79 394** | **+512** |
| defined symbols | 5 993 | 6 014 | +22, −1 | **5 992** | **−1** |

The fix: those sources are gated on a Kconfig symbol of their own,
**`ANT_DONGLE_PROFILES_EXTRA`, `default n` and `default y if ANT_DONGLE_MATTER`**
— the same treatment `ANT_DONGLE_MATTER_MAP` already had, beside it inside
`if RADIANT_BRIDGE`. `matter.conf` needed no line: the
`default y if ANT_DONGLE_MATTER` is the whole mechanism, and an explicit `=y`
would only be a second place to keep in step. `CMakeLists.txt` puts the seven
sources and the `radiant/src/profiles` include directory behind the new symbol
while `radiant_bridge.c`, `radiant_binding.c`, `radiant_hr_adapter.c`,
`radiant_rules.c`, `radiant_liveness.c` and `src/bridge_pump.c` stay under
`CONFIG_RADIANT_BRIDGE`; `src/rx_tap.c` puts the three extra adapter arrays,
their init calls, the Table 4-1 common-page path and the `0x0B`/`0x11`/`0x19`
dispatch arms behind `#ifdef`, so with the symbol off it is the `0x78`-only tap
it was before.

**In `src/self_channels.c` the off path is the original code verbatim, not a
one-row table.** A single-row table with a modulo-1 rotation reads better and
does *not* restore the old image: it still emits `self_profiles[]`, `self_row[]`
and `configure_and_open()`, still makes the per-row
`antr_channel_search_timeout_set()` call the original never made, and still
carries the profile-naming log strings in `.rodata`. Byte-identity was the
requirement, so the guard is one `#ifdef` around the table and the open/retune
helpers plus two more inside `self_thread_fn()`.

Every other allocated section — `.ARM.exidx`, `initlevel`, `device_area`,
`_static_thread_data_area`, all seven `*_driver_api_area`s,
`net_socket_register_area`, `log_const_area`, `log_backend_area`, `tbss`,
`radiant_sink_area`, `.last_section`, `noinit` — is byte-identical.

**The residual +112 B / −1 symbol is not this fix**, and per-object
`arm-zephyr-eabi-size` says so directly:

| object | base `text` | head `text` | `bss` | why |
|---|---:|---:|---|---|
| `self_channels.c.obj` | 1708 | **1708** | 1692 → **1692** | **identical** |
| `rx_tap.c.obj` | 215 | 223 (+8) | 132 → 196 (+64) | `struct radiant_hr_adapter` grew |
| `radiant_rules.c.obj` | 751 | 835 (+84) | 640 → 1088 (+448) | the file was rewritten |
| `radiant_hr_adapter.c.obj` | 204 | 212 (+8) | 0 → 0 | the running-total fix |
| `mqtt_sink.c.obj` | 3192 | 3208 (+16) | 3350 → 3350 | an 18-line diff |

Everything else in `app.dir` is byte-identical on all three columns. 116 B of
object growth against 112 B in the image is the linker's alignment, and
448 + 64 = 512 accounts for the RAM delta exactly. All three causes are in files
outside the gated CMake list: `radiant_hr_adapter` publishing a running total for
`RADIANT_HR_FIELD_BEAT_COUNT` instead of a per-message delta needed a
`uint64_t acc_beats`, taking the struct from 16 B to 24 B and `rx_tap.c`'s
`hr_adapters[RADIANT_BINDING_MAX]` array from 0x80 to 0xC0 (the +4 B in each of
`ant_bridge_channel_bind()` and `ant_dongle_rx_tap()` is the index scaling
changing from a shift by 16 to a multiply by 24); and `radiant_rules.c`'s rewrite
dropped its static helper `dwell_update`, which is the `−1`. **Byte-identity is
therefore reachable for the gated work and was reached**; 0.036 % of flash and
0.20 % of RAM remain, fully attributed, none of it changing what the radio does.

### 6.4 The compiled-sources inventory

| Arm | `CONFIG_RADIANT_BRIDGE` | objects, base | objects, head | new sources compiled |
|---|---|---:|---:|---|
| `med` | n | 678 | 678 | **none** |
| `coex` | n | 280 | 280 | **none** |
| `bridge` (before the fix) | y | 694 | **701** | all seven |

The seven are `radiant_power_adapter.c`, `radiant_env_adapter.c`,
`radiant_common_adapter.c`, `profile_common.c`, `profile_power_decode.c`,
`profile_fec.c` and `profile_env.c`; `radiant_hr_adapter.c` was already in both.
On the Matter arm all seven are in `app.dir` with 15-19 references each in
`zephyr.map` (`radiant_common_adapter` 19, `radiant_power_adapter` 18,
`profile_common` 18, `profile_env` 17, `radiant_env_adapter` 16, `profile_fec`
16, `profile_power_decode` 15); after the fix the `bridge` arm has zero of them.

**`profile_power.c` is NOT compiled** — only `profile_power_decode.c`. That split
is the reason the CMake comment gives for existing (the master side reaches
`profile_compat.c` and thence `radiant_sec`), and the object inventory is where
it can be checked rather than asserted. `radiant/tests` is unaffected: it
compiles these sources unconditionally and knows nothing of the new symbol,
verified by building it rather than assumed (`nrf5340dk/nrf5340/cpuapp`,
`--no-sysbuild`, `-p always`, exit 0, all seven objects present).

---

## 7. What runs where: threads, sinks and deadbands

```
ANT pump (prio 7)          CHIP event loop (K_PRIO_PREEMPT(1))
  |                          |
  writes shadow              reads shadow, calls the data model
  under k_mutex              under no lock of ours
  |                          ^
  +-- 1 Hz delayable work ---+  PlatformMgr().ScheduleWork(), single-in-flight
```

**The pump thread never takes the CHIP stack lock.** `LockChipStack()` from a
priority-7 thread lets it block the priority-1 thread servicing BLE commissioning
and OpenThread — the exact coupling `src/bridge_pump.c` exists to prevent,
approached from the other side. So the pump writes a static shadow row under an
ordinary `k_mutex`, that mutex is never held across a CHIP call, and no CHIP API
is called from any thread but CHIP's own.

The flush is armed by a **delayable work item** rather than a `k_timer` (whose
handler runs in ISR context, where a queue put is not something to do casually)
and rather than a thread (which would cost a stack for microseconds of work).
`atomic_cas` keeps a single flush in flight, so a slow CHIP thread coalesces
rather than queueing.

**`main()` does not move.** `ant_matter_start()` is called from
`ant_dongle_post_radio_init()`, **returns immediately**, and does nothing but
create a thread — because that hook runs before `usb_ant_class_init()`, and this
repository has already paid once for a pre-transport stall being
indistinguishable from a dead board (`thread.conf:60-75`: 31.04 s in
`net_config_init()`, read as a hung board by a human and by `ant_verify.py` in the
same session).

**Four sinks, not two and not three.** `src/matter/ant/ant_matter_sink.c`
registers `radiant_matter_glue_sink`, a second `RADIANT_SINK_DEFINE()` beside the
one in `radiant/src/bridge/radiant_matter.c`, because that first sink cannot see
two things the Matter plane needs: **bindings** (`radiant_matter.c` passes `NULL`
for `binding_changed`, since a type map has no use for a device number, and the
glue needs one for the stable key it persists an endpoint id against) and
**staleness** (`RADIANT_SAMPLE_STALE` arrives on fields the type map declines —
heart rate first, on a strap — so a `want()` filtered to mapped types would never
observe it). The Matter arm therefore links four and `bridge_pump.c` logs
`bridge: 4 sinks` at boot: `radiant_liveness_sink`, `radiant_rules_sink`,
`radiant_matter_sink`, `radiant_matter_glue_sink`. The CI row's
`assert_sink_names` diffs them as an **exact set**, not a subset, so an
out-of-date list fails the build; the row lists four.

**Deadbands.** `radiant_matter.h` carries `deadband` and `heartbeat_s` per row,
with sentinels `RADIANT_MATTER_DEADBAND_EVERY` (−1) and `_ON_CHANGE` (0), so a
zeroed row gets on-change suppression — the one suppression that can never lose
information. These are an **efficiency** measure and not a conformance one:
min/max interval enforcement belongs to the Matter server's `ReadHandler` and
writing an attribute at 4 Hz is legal, merely wasteful — each write walks the
subscription list and bumps `DataVersion` on the CHIP thread, which sits above
the ANT host thread, and a controller subscribing with `MinIntervalFloor = 0`
would put 4 Hz per attribute straight into the coexistence budget of
`docs/radiant-bridge.md` section 7. **The threshold is compared after conversion,
in the cluster's own units**, so a 0.5 degC temperature deadband is `50`;
comparing before conversion would make it depend on whichever exponent a decoder
happened to pick. The heartbeat is not optional: without one a sensor whose
reading never changes stops writing forever, and "unchanged" becomes
indistinguishable from "gone" to anything reading timestamps rather than
`Reachable`.

**Tests.** `radiant_matter` is 28 cases; the full `radiant/tests` application is
**723 passing, 0 failing**, run on real hardware (nRF5340 DK) via
`scripts/run_ztest_hw.ps1`.

---

## 8. Per-SoC blockers

### 8.1 nRF52840: two `#if` gaps in `radiant_radio_nrf.c`

**`nrf52840 + RADIANT_BACKEND_NRF_GATE_MPSL` does not build from the repository
as measured**; the nRF52840 figures above were taken on a patched copy.

```
:3474: error: 'NRF_RADIO_Type' has no member named 'PUBLISH_ADDRESS'
:3475: error: 'NRF_RADIO_Type' has no member named 'PUBLISH_RXREADY'
:3476: error: 'NRF_RADIO_Type' has no member named 'SUBSCRIBE_RSSISTART'
:3477: error: 'NRF_RADIO_Type' has no member named 'SUBSCRIBE_RXEN'
:3478: error: 'NRF_RADIO_Type' has no member named 'SUBSCRIBE_TXEN'
:3479: error: 'NRF_RADIO_Type' has no member named 'SUBSCRIBE_DISABLE'
:3581: error: implicit declaration of function 'radio_ep_reg'
```

Not a Matter problem and not a budget problem. It is the failure the file's own
comment describes: RADIO's `PUBLISH_`/`SUBSCRIBE_` registers do not exist on a
part with the old PPI. The `RADIANT_NRF_RADIO_EP` discriminator
(`#if defined(DPPIC_PRESENT)`) was applied to `radio_ep_reg()` and to
`radio_endpoints_attach_off_at_init()`, and **was not applied to
`radio_endpoints_save()` and `radio_endpoints_attach()`**, which are guarded by
`#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)` alone. So the direct nRF52840
build compiles and the arbitrated one does not — and every build in this project
that exercises arbitration is on an nRF54L or nRF53, which have DPPIC, so nothing
noticed. **Any** nRF52840 `GATE_MPSL` build fails the same way, coexistence arms
included. The fix costs nothing on any DPPIC part (the `#if` selects the existing
code byte for byte); the bodies are empty because on a PPI part there is no
endpoint register for another stack to hold:

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

### 8.2 Two stack-size defaults that do not land

`radiant/Kconfig`'s own help calls both of these requirements rather than tuning
knobs, and the failure mode it describes is a silent reboot loop that looks like
a wedged board.

| Symbol | `radiant/Kconfig` wants | nRF52840 got | nRF54L15 got |
|---|---:|---:|---:|
| `ISR_STACK_SIZE` | 4096 | **4096** | 4096 |
| `SYSTEM_WORKQUEUE_STACK_SIZE` | 4096 | **1120** | **1120** |
| `MPSL_WORK_STACK_SIZE` | 2048 | **1024** | 2048 |

`radiant/Kconfig`'s `default N if RADIANT_BACKEND_NRF_GATE_MPSL` wins for
`ISR_STACK_SIZE` (nothing else supplies an earlier default) and **loses for the
other two**, because an earlier definition already supplies one. For
`MPSL_WORK_STACK_SIZE` the earlier definition is CHIP's own:

```
modules/lib/matter/config/nrfconnect/chip-module/Kconfig.defaults:80
    config MPSL_WORK_STACK_SIZE
        default 2048 if SOC_SERIES_NRF54L   # nRF54L requires more memory due to crypto backend
```

So **the nRF54L15 gets 2048 by coincidence** — CHIP asking for it for an
unrelated reason — and the nRF52840 gets 1024, which `radiant/Kconfig` explicitly
says is not enough. `SYSTEM_WORKQUEUE_STACK_SIZE` comes out 1120 on **both**
boards, so that half is a pre-existing condition of the shipping nRF54L15 image.
**Costed: raising both to what `radiant/Kconfig` asks for is 2 976 + 1 024 =
4 000 B of RAM**, which the nRF52840's 28 328 B of headroom absorbs comfortably.

### 8.3 nRF5340: the gate cannot configure for cpuapp at all

`matter.conf` with `-DRADIANT_BACKEND=nrf` fails at Kconfig:

```
warning: The choice symbol RADIANT_BACKEND_NRF (radiant/Kconfig:57) was selected
(set =y), but RADIANT_BACKEND_NULL ended up as the choice selection.

warning: MPSL has direct dependencies SOC_SERIES_BSIM_NRFXX || SOC_SERIES_NRF52
|| SOC_NRF5340_CPUNET || SOC_NRF54H20_CPURAD || SOC_SERIES_NRF54L ||
SOC_SERIES_NRF71 with value n, but is currently being y-selected by
RADIANT_BACKEND_NRF_GATE_MPSL

error: Aborting due to Kconfig warnings
```

Two blockers. `radiant/Kconfig` admits `SOC_COMPATIBLE_NRF52X ||
SOC_COMPATIBLE_NRF54LX` only, so the backend falls to NULL; and
**`RADIANT_BACKEND_NRF_GATE_MPSL` `select MPSL` unconditionally, and MPSL does
not exist on `nrf5340/cpuapp` at all** — the nRF5340's radio *and its arbiter* are
on cpunet, so the gate has to move to cpunet with the backend, and the Kconfig
consequence is that **no `apps/dongle_thread` fragment that names the gate can
ever configure for `nrf5340/cpuapp`.** That includes `thread.conf`, `gate.conf`,
`coex.conf` and `bridge.conf`, i.e. every arm this application has.

A third, unrelated blocker means **`apps/dongle_thread` has never been configured
for this board**: `prj.conf` selects `CONFIG_SEGGER_RTT_MODE_NO_BLOCK_SKIP` and
`CONFIG_LOG_BACKEND_RTT_MODE_DROP` while `boards/nrf5340dk_nrf5340_cpuapp.conf`
turns RTT off without unsetting them, leaving two orphaned choice selections; and
that board file's own header still tells the reader to build it with
`-DEXTRA_CONF_FILE=stub.conf`, a file this application does not have. Both are
Kconfig-warning-level and therefore fatal.

The §2.2 numbers were taken with an uncommitted scratch fragment —
`thread.conf` + `matter.conf` with every MPSL and backend line removed, plus the
three board-hygiene overrides, and `-DRADIANT_BACKEND=null`. **They answer the
CHIP-side question and nothing about RadiANT on that part.** The netcore
selection also cannot live in the shared `matter_sysbuild.conf`, because
`SB_CONFIG_NETCORE_IPC_RADIO` and its two transports do not default on and the
symbols do not exist on the nRF54L15 (`SUPPORT_NETCORE` is nRF53/nRF54H only), so
they are command-line flags: `-DSB_CONFIG_NETCORE_IPC_RADIO=y`,
`-DSB_CONFIG_NETCORE_IPC_RADIO_IEEE802154=y`,
`-DSB_CONFIG_NETCORE_IPC_RADIO_BT_HCI_IPC=y`.

---

## 9. What is not settled

1. **Peak RAM, on either part.** Every figure here is static. Matter allocates
   hardest during commissioning, out of the 10 KB CHIP heap that OpenThread also
   uses, and no number here bounds that. `CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE` may
   have to go up, straight out of the remaining headroom. **The margin is one
   subsystem wide, not two:** anyone who adds a second general-purpose heap,
   raises `BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER` above 16, or turns MCUboot on
   (tier 1, deferred, and a shipping requirement) spends the whole of §2.4's
   result.
2. **The kernel-heap `nm` check is conditional and must keep being re-run.** It
   is 24 584 of the 24 624 bytes reclaimed and rests on `k_malloc` having no
   caller.
3. **DFU is the real nRF52840 constraint and is untested.** This image has no
   MCUboot, no factory data and no OTA. NCS's own nRF52840 Matter layout
   (`nrf/dts/samples/matter/nrf52840_partitions.dtsi`) is 28 KB MCUboot +
   **960 KB slot0** + 4 KB factory data + 32 KB storage, with **slot1 on an
   external QSPI part** (`mx25r64`), because two 960 KB slots do not fit in 1 MB.
   797 684 B fits in the 983 040 B slot0 with 185 356 B (18.9 %) spare — before
   factory data and the OTA requestor, which the nRF54L15's 1524 KB makes a
   non-question. **A shipping nRF52840 bridge needs an external flash chip on the
   board.** That is a BOM decision, and the one place where the nRF52840 is the
   worse part.
4. **The nRF52840's radio backend has never been on the air.**
   `radiant/Kconfig`'s nrf arm says so: the address arithmetic and packet
   configuration are measured on nRF54L15 (Spike A) and the nRF52840 is
   *predicted* to behave identically. §8.1's fix removes a whole per-grant
   endpoint-swap layer on that part — correctly, since the registers do not exist
   — but that reasoning has never been checked against a running nRF52840 beside
   a second stack. There is no nRF52840 DK on this bench (the attached nRF52840
   devices are a dongle and a Feather), so the board files in §2.3 have never
   booted, the `timer4` choice has never been exercised, and the `uart1` pinout is
   whatever the DK's own DTS routes.
5. **Trap 5 and trap 19 are read off source, not observed** — that every
   dynamic-endpoint attribute is external storage, and that declaring
   `OccupancySensing` in the ZAP is a trap rather than a help.
6. **Trap 13 has no automated check.** Nothing in this repository would notice a
   hand-edit of `zap-generated/`; NCS ships a `check_zap.py` that could be wired
   into CI.
7. **Factory reset is untouched.** Both boards put OpenThread's dataset, Matter's
   fabrics and (if `CONFIG_RADIANT_NODE_NVM_SETTINGS` is ever on) radiant's node
   identity in one settings partition.
8. **On-air behaviour is `docs/g-hardware-bringup.md`'s subject.** Everything
   above is a build, a symbol table and a host-independent test suite.
