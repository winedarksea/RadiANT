# MPSL assert 69:108 bench legs, 2026-11-15

Legs for finding what triggers or avoids assert 69:108 (see
`archive/benchmarks/2026-11-14-mpsl-soak/README.md` for the first set). Each leg
runs `soak_run.py` and ends at the first `MPSL ASSERT` line on the DUT console.

## Rig

- DUT: nRF54L15 DK (J-Link 1057737173), ANT on COM4, log on COM3 (DTR).
- Thread peer: nRF5340 DK running the NCS OpenThread CLI with `scripts/p4_peer.conf`
  (`scripts/flash_p4_peer.ps1 -Port COM6`), `leader`; the DUT attached as a
  child in every leg (`<leg>-peer.log`, child table after boot).
- ANT master: Pro Micro `EE6986854FD72803` driven by `ant_sim.py --profile power
  --device-number 2002` (its shipping image, not reflashed).
- DUT channels: ch0 tracks #2002, ch1 searches an absent #43210 (sweep always
  busy). Garmin `006` tracks #2002 as a same-window reference.
- Runner: `soak_run.py` (adapted from `2026-11-14-mpsl-soak/soak_run.py`), one
  flash per leg.

## Running a leg

Use the NCS toolchain interpreter (there is no system Python):
`C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe`. `--tools` and `--flash-script`
default to `<repo root>\tools` and `<repo root>\scripts\flash_sim_jlink.ps1`, the
repo root being three directories above the script.

Thread image (waits `--attach-wait` 75 s, checks the peer before/after):

    python soak_run.py --base D:\out\2026-11-15-assert-69108\arm1_a --hex C:\path\zephyr.hex ^
        --image-label arm1_lock --seconds 1800

Image without Thread (`--no-peer`: no peer commands, waits `--boot-wait` 8 s):

    python soak_run.py --base D:\out\2026-11-15-assert-69108\gate_a --hex C:\path\zephyr.hex ^
        --image-label gate_baseline --no-peer --seconds 1800

Per leg, files are written beside `--base`: `-console.log`, `-sim.log`,
`-dut.log`/`.json`, `-ref.log`/`.json`, `-peer.log`, `-summary.json`. One row is
also appended to `legs.tsv` in the same directory (header written when the file
is new). Other switches: `--no-ref`, `--no-search` (DUT opens only ch0),
`--no-master` (no ant_sim; the DUT still opens both channels unless
`--no-search`), `--no-flash` (reuse the board's image; `--hex` is still
recorded), `--stop-on-reboot`, `--continue-after-assert`, `--grace` (console
seconds kept after the assert, default 3), `--image-label`, `--image-sha256`
(default: sha256 of `--hex`), `--jlink-device`, `--jlink-serial`.

Offline check of the detection code against an existing capture (no hardware):

    python soak_run.py --selftest archive\benchmarks\2026-11-14-mpsl-soak\head1-console.log

## How a leg is timed

- `since_boot`: seconds from the first console line the capture received (the
  `[cap]` host lines do not count) to the assert line. `since_flash` is from the
  end of the flash step; `since_open` is from the start of the DUT
  `ant_verify` process (channels opened). With `--no-flash` there is no reset,
  so `since_boot` is from the first line seen after the capture started.
- The Zephyr uptime printed in the log lines does not start at zero on a flash
  (head1 boots at uptime 00:17:18), so it is not used.
- A leg ends at the first assert, then keeps the console for `--grace` seconds
  so the multi-line dump is captured, then terminates the DUT verify, the
  reference verify and ant_sim. Children are also terminated on Ctrl-C, on a
  flash failure and on any exception.
- `reboots` counts `*** Booting` banners after the first one; the leg only ends
  on a reboot with `--stop-on-reboot`. With `--continue-after-assert` every
  assert is counted (`assert_count`, `asserts[]` in the summary) and the leg
  runs the full duration.
- `assert_fields` in the summary holds every `key=value` pair on the assert line
  and the following dump lines (a repeated key becomes a list, e.g. `granted`
  appears on both of today's lines). Dump lines that are periodic Zephyr
  `<inf>`/`<dbg>` log lines are kept in `assert_dump` but not parsed for fields.
- legs.tsv `placed granted blocked ext skip` come from the assert line when
  there was an assert. For a clean leg they come from the last periodic gate
  stats: `D2STAT` (`pl`, `gr`, `bl`, `ext`) and `ext_skipped` from `D2REL`; they
  are empty if those lines were never seen. `ext` is `ok/fail`.

## Results

| leg | image | result | placed | ext ok/fail | skip |
|---|---|---|---|---|---|
