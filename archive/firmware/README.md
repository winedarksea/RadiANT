# `archive/firmware/` — flash readbacks

Checked by: nothing — treat as narrative. The block counts and address ranges
quoted below are reproducible with the command in each section, but no test
runs them.

Two `.uf2` files, moved here from the old top-level `backup/`. Neither is a
release image and neither should be flashed casually — see *What these are not*
at the end. They are kept because each one is evidence for a claim the project
makes elsewhere, and re-obtaining either needs the board, a human to double-tap
RESET, and knowledge of what state the board was in at the time.

## Where they came from

The Adafruit nRF52840 bootloader mounts a mass-storage volume (`FTHR840BOOT`)
after a double-tap of RESET. Dragging a `.uf2` onto that volume programs the
board; *reading* `CURRENT.UF2` back off it dumps flash as UF2 blocks. Both
files here are that readback, captured at two different moments.

This is not a curiosity — it is the only way to get data off this board. The
Feather has no debugger, so `CONFIG_USE_SEGGER_RTT` compiles and nothing can
read it. `diag.conf` instead commits the log to a reserved flash region, and
[`scripts/read_flash_log.ps1`](../../scripts/read_flash_log.ps1) recovers it
from exactly this readback. That script takes `-Uf2Path`, which is how a saved
readback is analysed offline:

```powershell
.\scripts\read_flash_log.ps1 -Uf2Path .\archive\firmware\CURRENT-after-0.8.0.uf2
```

## `CURRENT-after-0.8.0.uf2` — 1,908,736 B, 3,728 blocks, `0x1000`–`0xEA000`

SHA-256 `f01094c7142c43b3c297e96b800b8b89afd4c4de48405962f79f5053e8db2d0e`

A **whole-flash readback taken after the bootloader was updated to 0.8.0**, of a
Feather running the sdk-ant dongle build. It contains, in one file:

- the UF2 bootloader itself at `0x1000`, which self-identifies in its own
  strings as `UF2 Bootloader 0.8.0 lib/nrfx (v2.0.0) lib/tinyusb
  (0.12.0-145-g9775e7691)`;
- the application at `0x26000`, roughly 186 KB of it, built against
  `nRF Connect SDK v3.2.4-4c3fc0d44534` / `Zephyr OS v4.2.99-9673eec75908`;
- the USB identity strings the dongle enumerates with — `Dynastream
  Innovations`, `ANTUSB2 Stick`, `nRF Serial` — and the bridge's own log
  strings.

**What it is for.** It is the evidence behind the flash-log offset choice
documented in `README.md`: *bootloader 0.8.0 dumps `0x1000`–`0xEA000`*, which
stops short of the `0xEC000` end of the code partition. Running the readback
command above reports exactly that range and reports both log slots
(`0xE6000`, `0x40000`) as present-but-unwritten, which is what confirms the
window rather than the log. Without this file that claim is somebody's memory
of a bench session; with it, anyone can re-derive it in one command.

Use it when changing `CONFIG_ANT_DONGLE_FLASH_LOG_OFFSET` or
`_ALT`, to check the new offset lands inside the window a real bootloader
actually dumps — a slot outside it is written correctly by the firmware and is
simply invisible in the readback, which is indistinguishable from "the log
never ran".

> **Licence caveat.** The application in this image links `libant.a`, so
> Garmin/Nordic's proprietary object code is inside the file. It predates the
> preservation policy and is already in this repository's history. Do not
> attach it to a release and do not add further sdk-ant-linked images here.
> The full reasoning is in [`docs/preservation.md`](../../docs/preservation.md).

## `CURRENT-readback.uf2` — 46,592 B, 91 blocks, `0x26000`–`0x2BB00`

SHA-256 `b0253014d9523fd6031d21ad493c1ab6ccbffcc5610ba7d24cfb055b21ca5d46`

A much earlier readback, from a bootloader that exposed only as far as the last
page it considered programmed. It covers 23,296 bytes of application starting
at the Feather's `0x26000` link address and nothing else — both log slots are
reported as *outside the readback range*, which is the failure mode the second
log copy at `0x40000` exists to work around.

The image is a minimal bring-up build, not a dongle: its devicetree strings
name `clock@40000000`, `gpio@50000000`, `gpio@50000300` and `uart@40002000`,
and there is no USB device node among them. Same SDK pair as the file above
(NCS v3.2.4, Zephyr 4.2.99).

**What it is for.** It is the worked example of the truncated-readback problem.
`read_flash_log.ps1` prints `slot 0xE6000 : outside the readback range` against
it, which is the message a reader will otherwise meet for the first time on a
board they are already confused about. Point the script at this file to see
what that looks like when it is expected, before deciding whether a live board
is showing the same thing for the same reason.

## What these are not

They are **not** flashable release images and not a rollback path. Do not drag
either onto `FTHR840BOOT`:

- `CURRENT-after-0.8.0.uf2` spans the bootloader region as well as the
  application. Writing a whole-flash readback back over a live board is a way
  to lose the bootloader, not a way to restore a firmware version.
- `CURRENT-readback.uf2` is a partial dump of a bring-up build with no USB in
  it. Flashing it produces a board that enumerates as nothing.

To install firmware, take `ant_dongle.uf2` from a release or build it — see
`README.md`. To capture a fresh readback, double-tap RESET and copy
`CURRENT.UF2` off the mounted volume.
