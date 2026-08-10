# Gotchas worth knowing

Checked by: nothing — treat as narrative.

Moved verbatim out of `README.md`: no entry is condensed, reordered or
rewritten, because each one cost real time to find and the value is in the
specificity. Add to the end rather than editing what is here. One post-move
correction has been made — the Partition Manager entry named
`pm_static_adafruit_feather_nrf52840.yml`, a file that no longer exists, and
now names `pm_static_nrf52840_uf2_sdv6.yml`, which `sysbuild.cmake` actually
selects for both the Feather and the Pro Micro. Filename only; the prose around
it is untouched.

---

These are the non-obvious constraints the code is shaped around. Each one cost
real time to find, and several look like a generic "USB doesn't work" failure
from the outside.

- **`west build` needs its cwd inside the workspace.** It is an extension
  command discovered through the workspace manifest, so `-s`/`-d`/`-z` pointing
  elsewhere is not enough — `Push-Location` into the NCS topdir first.
- **Quote `-D` arguments.** PowerShell splits `-DEXTRA_CONF_FILE=stub.conf` at
  the dot and CMake receives two mangled arguments. Always
  `"-DEXTRA_CONF_FILE=stub.conf"`.
- **Don't use `west flash`.** The `uf2` runner fails with
  `ValueError: uf2 doesn't support --dev-id option`. Use `scripts/flash_uf2.ps1`.
- **PowerShell 5.1 reads `.ps1` as ANSI** unless the file has a BOM, so the
  scripts here are deliberately ASCII-only. A stray em-dash is a parse error.
- **Windows caches USB descriptor verdicts, permanently.** The result of its
  first MS OS descriptor query is stored under
  `HKLM\SYSTEM\CurrentControlSet\Control\usbflags\0fcf1009<bcdDevice>`, and per
  Microsoft's documentation a failed first query is *never retried* — so one bad
  build poisons the VID/PID and every later correct build looks equally broken.
  Between iterations either bump `CONFIG_ANT_DONGLE_BCD_DEVICE` (no elevation
  needed) or run `scripts/reset_usb_cache.ps1` from an elevated shell.
- **Endpoint descriptors must say `AUTO_EP_IN`/`AUTO_EP_OUT`, not `0x81`/`0x01`.**
  `usb_fix_descriptor()` pairs each endpoint descriptor with its
  `usb_ep_cfg_data` entry by matching `bEndpointAddress` against `ep_addr`, and
  only then rewrites both with the address it allocates. Writing the final
  ANTUSB-2 addresses straight into the descriptor matches nothing, so
  `usb_get_device_descriptor()` returns NULL and `usb_enable()` fails with -1
  before anything reaches the bus. Allocation starts at endpoint 1 in each
  direction, so `AUTO_EP_*` still yields 0x01 and 0x81.
- **Partition Manager decides the link address, not `zephyr,code-partition`.**
  sdk-ant is an NCS module, so sysbuild turns Partition Manager on, and left
  alone it hands the whole 1 MB to `app` at `0x0`. `CONFIG_FLASH_LOAD_OFFSET`
  stays at `0x26000` from the devicetree, and that is what the UF2 converter
  stamps into the file — so the image is linked for `0x0`, written to
  `0x26000`, and boots into whatever the vector table happens to point at. It
  never enumerates and looks exactly like a USB fault.
  [`pm_static_nrf52840_uf2_sdv6.yml`](../pm_static_nrf52840_uf2_sdv6.yml)
  restates the board's layout in the form PM reads; `sysbuild.cmake` selects it
  by board. Check `build/<d>/partitions.yml` says `app: address: 0x26000`, or
  that `zephyr.hex` opens with an extended-address record rather than
  `:10000000`.
- **Reserving a region Partition Manager already knows about moves your app
  instead of protecting it.** Adding a new board means writing a static map, and
  the obvious first draft reserves the regions the bootloader owns. But PM may
  already define one of them — on the nRF52840 Dongle, `nrf5_mbr`
  (`nrf/subsys/partition_manager/pm.yml.nrf5_mbr`), added whenever
  `CONFIG_BOARD_HAS_NRF5_BOOTLOADER` is set and placed `{after: [start]}`. A
  static entry covering the same bytes wins the address, PM slides its own copy
  along to sit after it, and `app` gets pushed a page further than
  `CONFIG_FLASH_LOAD_OFFSET` says. There is no warning; a static partition being
  honoured is exactly what you asked for. `app` cannot be pinned against this,
  since it is the one that absorbs whatever space the others leave. The fix is
  to delete a partition, not add one. Check `build/<d>/partitions.yml` and
  `.config` agree on the app address.
- **The USB driver's work queue needs more than its 1 KB default.** The nrfx
  driver dispatches `SET_CONFIGURATION` on its own work queue, and the
  `ant_status_cb()` → `arm_rx()` → `usb_transfer()` chain runs there. At the
  default it overflows the moment the host configures the device; the fatal
  error halts everything at once, so the board goes dark on the bus, the LED
  stops and nothing more is logged. See the stack sizes in `prj.conf`.
- **`MESG_SYSTEM_RESET` resets the ANT stack, not the MCU.** Every host library
  opens the device, resets it, then keeps using the same handle. Rebooting
  makes that handle stale and the next transfer fails with a pipe error — at
  the exact point every session begins.
- **UF2 cannot chip-erase.** It only rewrites `0x26000`–`0xEC000`, so anything
  outside that window survives a reflash.
- **The frame checksum covers the `0xA4` SYNC byte, and a test suite cannot tell
  you otherwise.** The parser seeded `running_xor = 0` instead of the SYNC byte
  it had just consumed, so every checksum it computed was off by exactly `0xA4`
  and every frame a real host sent was dropped without a word. Nothing else
  looked wrong: the dongle enumerated, bound its driver, and Zwift listed it as
  present - it simply never executed a single command, so it never replied,
  Zwift timed out after five seconds and fell into its stop path, writing 70,000
  identical `Stopping ANT search` lines and showing no ANT+ sensors at all. It
  hid for as long as it did because `ant_probe.py` made the *same* mistake in
  `frame()`. Firmware and tools agreed perfectly with each other and with
  nothing else in the world, so the whole suite passed - probe, scan, all eight
  channels, real sensors, ack and burst. Every green test was two wrong
  implementations shaking hands. If you change the framing on one side, change
  it on the other in the same commit, and check the result against an
  implementation neither of you wrote: `ANT_DLL.dll` exports
  `ANT_SetDebugLogDirectory`, and the `Device0.txt` it writes gives you Garmin's
  own `Tx`/`Rx` bytes to XOR by hand.
- **A host gives up on one unanswered message, and says nothing useful about
  which.** Zwift calls `ANT_SetTransmitPower`, the device-wide `0x47`, while
  setting a search up. The bridge implemented only the per-channel `0x60` and
  answered `0x47` with `INVALID_MESSAGE`. That was masked by the checksum bug
  above - the message never reached `dispatch()` to be rejected - but it would
  have stalled the search on its own once framing was fixed. What the host
  actually calls is discoverable without guessing: `ANT_DLL.dll` is loaded by
  name, so the ANT functions Zwift resolves are plain strings in `ZwiftApp.exe`
  (`ANT_SetTransmitPower`, `ANT_OpenRxScanMode`, `ANT_EnableLED`, ...).
- **Capabilities are the stack's, coverage is the bridge's, and nothing checks
  they agree.** `ant_capabilities_get()` reports what the ANT stack can do,
  which is a superset of the serial messages `dispatch()` implements - the
  advertisement is what tells a host the feature is safe to use. Two bits
  already carry their weight: scan mode and LED are both reported off, which is
  why Zwift never sends `0x5B`/`0x68` even though it has the calls. Anything
  advertised *and* unimplemented is a trap of the kind above.
- **A USB 3.0 port can deafen the dongle by 10-20 dB, and it looks exactly like
  a flat sensor battery.** USB 3.0 SuperSpeed signalling puts broadband noise
  right across 2.4 GHz, and a dongle plugged directly into such a port - or into
  a hub sitting next to one - sits in it. The receiver is not broken and the
  sensor is not broken; the noise floor has come up under both, and every
  symptom is "it stopped pairing". A USB 2.0 port, or six inches of extension
  cable, is often the entire fix.

  This was invisible until Phase 3 of the RF work, and it is now measurable
  without any equipment. A `core` build logs a `noise rf=... floor=... busy=...`
  line once a minute, sampled from receive windows that heard nothing, which is
  exactly the population whose level IS the noise floor. **The number to compare
  is `floor`**, the 10th percentile; `busy` is the 90th and says how bursty the
  band is rather than how deaf the receiver is. Read it over the log VCOM -
  **which needs DTR asserted** - or with `scripts/read_flash_log.ps1`.

  The comparison is the measurement, not the absolute value: note the floor in
  one port, move the dongle, wait a minute, note it again. A quiet bench sits
  near the part's stated floor; a bad port moves it visibly and immediately.
