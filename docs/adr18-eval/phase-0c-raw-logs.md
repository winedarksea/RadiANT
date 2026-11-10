# Phase 0c raw logs

All files are in `C:\adr18_logs\phase-0c\`, outside the repository. Measured
2026-11-10 on branch `wip/adr18-0c`, nRF54L15 DK (J-Link `1057737173`,
console on MI_02 = COM3 with DTR). Report: [phase-0c.md](phase-0c.md).

Every log line from the board carries a host timestamp. Lines starting
`# host:` are the driver's own. Grep keys: `F1|` per grant, `F1S|` per row,
`F2|` per iteration, `F2S` per row, `F3|` per row, `FX|session_open`.

## Images (spike code `b1e76ad`)

| File | What it is |
|---|---|
| `build_0c.ps1` | the build wrapper (west, `--no-sysbuild`, `-o=-j4`, `.config` read-back) |
| `build_none.log`, `config_none.txt` | no-stack build and its `.config` (`MPSL=y`, `SESSION_COUNT=2`, `BT` not set) |
| `build_ble.log`, `config_ble.txt` | `-DEXTRA_CONF_FILE=ble.conf` build and its `.config` (`BT=y`, `BT_LL_SOFTDEVICE=y`) |
| `spike_0c_none_b1e76ad.hex` | flashed no-stack image, SHA-256 `02479A61798AF066041DD3DD97A50E48739E19E6D6EFA1545B0F0BD45B96AC5F` |
| `spike_0c_ble_b1e76ad.hex` | flashed BLE image, SHA-256 `5B86D27EA417E8ACAB11A374E504A0A8F29AC491BAC8F10643782742716CDD22` |
| `flash_none.txt`, `flash_ble.txt`, `flash_ble_banner.txt` | `flash_sim_jlink.ps1 -Serial 001057737173` output for each flash |
| `ble_boot_banner.log` | the BLE image's boot banner, captured across a reflash: `second stack: BLE advertiser`, `advertiser: 1.0-1.2 s` |

## Runs

| File | What it is |
|---|---|
| `none_facts.log` | no-stack image: `a` (F1), `b` (F2), `c` (F3). The tables' no-stack numbers |
| `ble_facts.log` | BLE image, run 1: `a`, `b`, `c` |
| `ble_facts_run2.log` | BLE image, run 2, after the banner reflash. Console lost bytes in `jit1_big40` (4 grant lines and its summary missing, `g=15` cut) |

## Earlier image (not used in the tables)

Same code as `b1e76ad` minus the F2 200 us step variant and the F3 50/100 us
gaps, no-stack only. Same results.

| File | What it is |
|---|---|
| `img1_build_none.log`, `img1_spike_0c_none.hex` | its build and image |
| `img1_none_F1.log`, `img1_none_F2.log`, `img1_none_F3.log` | F1, F2, F3. One cut line each in F2 and F3 |
