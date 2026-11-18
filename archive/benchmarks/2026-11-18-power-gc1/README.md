# Gate GC1: TI (CC2652) power-diag, 2026-11-18

> The raw console capture (2400 s, about 1.2 MB with one `tx power req` line per transmit), the `ant_verify` record
> file, the images and the `.config` stay on the bench machine (gitignored; the image rebuilds from the commit and
> conf below). This folder holds the small text files. The method and the conclusions are in `docs/bench-runbook.md`,
> section "Power without a meter" → "TI (CC2652) power-diag, GC1".

**Every current figure here is an ESTIMATE**: the firmware's `radiant_power_diag` log line multiplied by typical
datasheet currents (`tools/ant_power_est.py`). No current meter was used.

Code: local master db5abfb (Pass C1 merged). Branch `power/gc1` adds only this folder and the runbook subsection.

## Image

| Build dir (in `C:\wt_power_gc1`) | Command | `zephyr.hex` md5 |
|---|---|---|
| `build/gc1_ti_pd` | `west -z C:\ncs\v3.4.0\zephyr build -s radiant\samples\hr_transmitter -d build\gc1_ti_pd -b cc26x2r1_launchxl -p always -- -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti -DEXTRA_CONF_FILE=power_diag.conf` (run from `C:\ncs\v3.4.0`) | 171501bf605b89adf22d29f4c9a9b402 |

`.config` read back: `CONFIG_RADIANT_BACKEND_CC26XX=y`, `CONFIG_RADIANT_POWER_DIAG=y`,
`CONFIG_RADIANT_CC26XX_RF_HWATTRS=y`, `CONFIG_CC13X2_CC26X2_HAS_CUSTOM_RF_HWATTRS=y`, `CONFIG_LOG_MODE_DEFERRED=y`,
`CONFIG_RADIANT_POWER_DIAG_IDLE=y`, `# CONFIG_HR_TX_ONE_WAY is not set`. `CONFIG_TRACING_BACKEND_UART=y` appears in
`.config`, but only `tracing_user.c` and `tracing_none.c` are compiled, so nothing but the log goes to uart0.

Flashed with `scripts\flash_ti.ps1 -Serial L1100TY2 -HexPath ...` (dslite, then the backchannel reset). The TI was
left on this image.

## Rig

- LAUNCHXL-CC26X2R1 (XDS110 L1100TY2): the device under test. Console on COM11 (XDS110 Application/User UART) at
  115200, captured with `scripts/gc1_capture.py`. It opens the port with DTR and RTS, then pulses `xds110reset.exe`,
  so the capture starts at boot (host +0.27 s).
- Pro Micro EE6986854FD72803 (not reflashed): the receiver.
  `tools/ant_verify.py --profile heart-rate --device-number 12345 --seconds 300 --serial EE6986854FD72803
  --expect-bpm 120`, run during the capture (TI uptime ~40–340 s).
- The nRF54L15 DK was not touched. It still carries the GA lowpower+quiet image, which also transmits #12345. The
  received stream is attributed to the TI by RSSI: −42.1 dBm, range −44 to −42. That is the TI's band at GA (−43.3,
  max −40). The L15's band at GA was −46 to −52.
- The nRF5340 DK and the Garmin were not used.

## Results

`gc1_summary.txt` (from `scripts/gc1_summary.py`) covers 239 lines and 2390.2 s:

- TX 1115.3 µs/s, 4.0602 ops/s, 274.7 µs/op. RX 4361.7 µs/s, 4.0602 ops/s, 1074.2 µs/op.
- RF core power-ups: 9705, i.e. 4.0602/s, **1.0000 per TX op**. `rfpu_x10` reads 39 or 40 per line; the per-line
  counts were recovered with ceil(x10 × dt_ms / 10000).
- RF core on: `rfon_pm` 17 or 18 on every line, i.e. 17.4–18.4 ‰. That is **4.29–4.54 ms per power-up**, against
  1.35 ms of TX + RX op time.
- `pudur_us` median 448 (424–480).
- Final cumulative counters: `fsx=0 bks=0 bks_max=0 wraps=2 cold=0 txl=0 txl_max_us=0`.
- Wraps: the first appears on the line at uptime 1081.1 s (previous line 1071.1 s), the second at 2151.2 s (previous
  2141.2 s). Spacing 1060–1080 s, against 2^32 / 4 MHz = 1073.7 s.
- CPU 55.6 ‰ (floored lines 53–60). Idle entries 37.4/s.

`verify.log` / `verify.json`: 1218/1219 packets, loss 0.08 % (one RX_FAIL), jitter stddev 9.7 ms, timing 0.011 ms
off grid by the radio's clock, 120 bpm exact. The `common pages` FAIL is expected: the sample sends page 0 only.

`est_cc2652_uart_on.txt` / `.json` (`--board cc2652 --log <capture> --uart-on`): ESTIMATE total of known parts
**1028 µA**:

- idle floor 675
- UART 167.5
- CPU 151.0
- RX 27.2
- TX 7.1
- XOSC_HF UNKNOWN, excluded

Without `--uart-on` the total is 860. `est_cc2652_ti_standby_whatif.txt` (`--ti-standby`, a what-if; this firmware
has no standby) gives 227. The estimator does not use `rfon_pm`, so RF-core-on time outside TX/RX (~12 ms/s) is not
in any total.

`ti_console_power_lines.txt`: the capture with the per-TX `tx power req` lines removed (one kept at the end as a
sample): boot banner plus all 239 `power` lines.

## Verdicts

- **H1 holds.** The RF core powers up once per slot, and TX and listen share that power-up. It is on 1.7–1.8 % of
  the time.
- **Finding 4 not reproduced.** 0 back-steps over 9705 power cycles. The two genuine wraps came about 1074 s apart.
  The fold bug-fix pass is not triggered.

## Odd things

- `apply_power()` (`radiant/src/radiant_radio_cc26xx.c`, called from the TX arm) logs `tx power req=...` at INF and
  calls `RF_setTxPower()` on **every** transmit: 9748 lines in 2400 s. This is the likely cause of the 55.6 ‰ CPU,
  against 5.6 ‰ for the console-on L15 arm at GA (not verified). It inflates the CPU term of the estimate.
- About 3 ms of RF-core on-time per slot is neither TX nor RX. The TI driver source suggests that, with
  `nInactivityTimeout = WAIT_FOREVER`, power-down waits until the next far-future command is posted. That is
  inferred, not measured.
- Idle entries are 37.4/s, against 25.3/s on the L15 bidirectional arms. Not investigated.
