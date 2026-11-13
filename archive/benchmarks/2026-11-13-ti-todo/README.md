# TI CC2652R: the 2026-11-12 TODO items, 2026-11-13

Branch `ti/todo-2026-11-13` (worktree `C:\Users\Colin\ant_dongle-ti`).
Plan: `the-ti-board-is-vectorized-reddy.md`.

Rig: LAUNCHXL-CC26X2R1 on COM11 at 57600, Garmin USB-m `006` (the same-window
reference), Pro Micro clone `EE6986854FD72803` as `ant_sim` #4242, the nRF54L15
DK's RadiANT UART dongle on COM4 as `ant_sim` #4243, and the trainer #52233.
NCS v3.4.0 interpreter, `python -u`, `ant_probe` on TI and the Garmin before
and after every leg. The room was lossy again: trainer 15-53 % on both receivers.

Scripts: `build_ti.ps1` (the build_all `ti_launchxl` west line plus `-D`
extras, `ti_health.conf;ti_diag.conf`), `run_images.ps1` (flash, then one leg),
`run_leg.sh` (TI and Garmin in one `ant_scan_rate` window tracking #52233,
#4243 and, with CLONE=1, #4242; 0xF7 read before and after), `run_tx_leg.sh`
(TI transmits #4244; Garmin and L15 receive). Leg summaries are in the `*.txt`
files, per-leg data in `<leg>.json` / `<leg>_diag_{before,after}.json`.

## Result

**Item 1 is fixed, and the cause was TI's receiver, not the clone.** tc901's RX
intermediate frequency `intFreq = 0x0800` (0.5 MHz) put the ~1.4 MHz-wide ANT
signal across DC. TI's loss then depended sharply on the transmitter's exact
carrier offset: the clone sat at the bad spot, and the L15 near another bad
spot. At `0x1000` (1.0 MHz) TI loses 0.0-0.6 % of both transmitters, flat
across ±200 kHz of retuning, and less than the Garmin in the same windows. Now
the default (`CONFIG_RADIANT_CC26XX_INT_FREQ`).

**Item 2: the decoy is gone.** One-group windows set `syncWord1 = 0` (second
correlator off). CRC fails halve, tracked loss is unchanged.

**Item 3 is closed without bench time.** See the TODO.md entry: its gate is a
ratio against sdk-ant, which cannot run on TI, it is `required = false`, and
ADR 0022's TI section already compared first sightings with the Garmin in the
same window.

**Item 4 (burst ACK-window reserve on MPSL) is not a TI item. Still open.**

## Images

| name | change from master 1df743a | zephyr.hex md5 |
|---|---|---|
| `tt_p0` | none | `C88C93142C3FE82C69315FE3A72B26E7` |
| `tt_trim0` | + `FREQ_TRIM_KHZ` symbol, value 0 | `C88C93142C3FE82C69315FE3A72B26E7` (identical) |
| `tt_trim_{p,m}{50,100,200,300}` | trim ±50..±300 kHz | see `trim_sweep*.txt` |
| `tt_bw99`, `tt_bw100` | rxBw 99 / 100 | `687F9CD7…`, `F738ADF8…` |
| `tt_if750`, `tt_if1000` | intFreq 0x0C00 / 0x1000 (bench edit) | `2D43C529…`, `AD9A1CFE…` |
| `tt_new` | `INT_FREQ` symbol, default 0x1000 | `AD9A1CFE53DDD07F69C6AB9185C28D8A` (= `tt_if1000`) |
| `tt_new_{p,m}{100,200}` | `tt_new` + trim | see `newtrim.txt` |
| `tt_new_sync0` | `tt_new` + `sync1_zero.patch` | `47BA63E8A2C63061E15F59C073F26BAC` |
| `tt_final` | the committed tree | `47BA63E8A2C63061E15F59C073F26BAC` (= `tt_new_sync0`) |

The AGC images (`tt_agc28/2E/30/3A/40`) were built and never run: the L15
ladder ruled out overload first.

## Phase 0 baseline (`p0_base`, `tt_p0`, 120 s)

| receiver | L15 #4243 | trainer |
|---|---|---|
| Garmin | 4.45 % | 78.8 % |
| TI | 6.67 % | 86.3 % |

The TI-vs-L15 gap reproduced (smaller than yesterday's 5.4 vs 0.34). The trainer
was nearly silent during this leg.

## Phase 1a: power ladders (`p1a_ladder_{clone,l15}.*`)

`ant_sens.py --rungs fine`, 60 s per rung, TI and the Garmin on the same rungs.

- **Clone transmitting:** TI lost 85-87 % at +8/+7 dBm (RSSI -33 dBm), 47 % at
  +5, then 2-5 % at +3..0 dBm (RSSI -47..-50). Yesterday's 87.5 % was the clone
  at its default (top) power. The dial check failed (0.58 dB/dB): the clone's
  RSSI jumps 12 dB between +4 and +5 dBm. The Garmin had its own bad clone band
  (+3/+2 dBm: 76-97 %) where TI was fine. Its ladder is also suspect, because the
  Garmin entered it with channels still open from `p0_base`. `run_leg.sh` now
  resets the Garmin after every leg.
- **L15 transmitting:** TI 5-10 % across the whole ladder, **including RSSI
  -30 dBm**, louder than the clone rungs TI failed on. Dial check OK (1.02 dB/dB).

So overload/AGC is ruled out. The fault is specific to the transmitter. It
depends on the clone's power setting, which fits a carrier that moves with
the PA setting.

## Phase 1b: retuning TI (`trim_*`, `trim2_*`)

TI loss per transmitter, 120 s legs, all with `intFreq = 0x0800`:

| trim kHz | TI clone | TI L15 | Garmin clone | Garmin L15 |
|---|---|---|---|---|
| 0 (7 legs, incl. bw_98 and if_98c) | 82.2, 26.3, 41.5, 45.1, 42.8, 46.3, 51.5 | 8.1-21.6 | 1.4-5.5 | 1.4-3.4 |
| +50 | 0.42 | 27.9 | 2.8 | 2.7 |
| -50 | 1.04 | 78.6 | 2.4 | 1.4 |
| +100 (2) | 0.42, 0.42 | 20.6, 17.5 | 1.4, 5.2 | 1.7, 2.4 |
| -100 (2) | 0.21, 0.62 | 31.6, 79.2 | 1.0, 3.7 | 0.3, 2.4 |
| +200 | 5.00 | 5.42 | 22.2 | 0.0 |
| -200 (2) | 0.62, 0.21 | 0.42, 2.08 | 9.5, 3.4 | 0.0, 3.1 |
| +300 | 18.75 | 76.45 | 2.8 | 0.7 |
| -300 | 0.83 | 2.29 | 2.1 | 0.0 |

The curve is not a bowl around an offset. The clone fails only at trim 0, and
any trim from ±50 kHz fixes it. The L15 fails across -100..+100 and recovers at
±200. A receiver with a null at one particular relative offset produces this.
A carrier offset against a narrow passband does not. The clone's offset from
TI is under 50 kHz, well inside the ±40 ppm budget, so the clone is not
defective.

## Phase 1c: bandwidth, then the IF (`bw_*`, `if_*`)

| leg | image | TI clone | TI L15 | trainer TI - Garmin |
|---|---|---|---|---|
| bw_98a / 98b / if_98c | shipped | 42.8 / 46.3 / 51.5 | 17.7 / 21.6 / 15.2 | -0.9 / +2.9 / +0.6 pp |
| bw_99a / 99b | rxBw 99 | 40.3 / 48.9 | 14.8 / 21.4 | +5.8 / +9.5 pp |
| bw_100a / 100b / if_bw100c | rxBw 100 | 3.5 / 1.9 / 1.9 | 1.9 / 2.9 / 2.7 | +13.4 / +8.0 / +11.0 pp |
| if_750a / 750b | intFreq 0x0C00 | 0.21 / 0.00 | 0.42 / 0.42 | -2.5 / +1.7 pp |
| if_1000a / 1000b | intFreq 0x1000 | 0.00 / 0.21 | 0.21 / 0.21 | -3.2 / -4.9 pp |

rxBw 100 hides the null but costs ~10 pp on the weak trainer, so it fails the
plan's gate. Moving the IF clears the null at no cost. 0x1000 was chosen over
0x0C00 for more clearance from DC. The two measured the same.

**Checks on `tt_new` (0x1000):**

- Retuned ±100/±200 kHz (`newtrim.txt`): TI clone 0.00-0.42 %, L15 0.62-1.46 %,
  Garmin 0.7-8.7 % in the same windows. Flat, so offset tolerance is at least
  ±200 kHz (±80 ppm at 2457 MHz, the combined budget of two ±40 ppm parts).
- TI transmitting (`tx_check.txt`; driverlib says TX uses the IF "if
  supported"): Garmin / L15 loss on TI's #4244 was 0.34 / 0.00 and 0.67 / 0.00 %
  on the old image, and 0.00 / 0.21 and 0.68 / 0.21 % on the new one. TX is
  unaffected.
- Acked data, Garmin originating to the TI master (`acked_new_garmin.log`):
  100/100 COMPLETED.

## Phase 2: decoy vs `syncWord1 = 0` (`sync_*`, `sync_ab.txt`)

A = `tt_new`, B = `tt_new_sync0`, alternating, 300 s each, reflashed per leg.

| leg | trainer TI | trainer Garmin | TI - Garmin | L15 TI | L15 Garmin | sync entries | crc_fail | alt_sync_hits |
|---|---|---|---|---|---|---|---|---|
| A1 | 31.91 | 35.97 | -4.06 | 0.42 | 4.97 | 5150 | 1440 | 605 |
| B1 | 34.64 | 35.10 | -0.46 | 0.25 | 6.12 | 4404 | 789 | 0 |
| A2 | 52.02 | 53.12 | -1.10 | 0.42 | 24.20 | 4448 | 1437 | 539 |
| B2 | 22.62 | 21.76 | +0.86 | 0.33 | 3.34 | 4730 | 722 | 0 |
| A3 | 24.20 | 22.77 | +1.43 | 0.33 | 2.92 | 5254 | 1283 | 567 |
| B3 | 24.69 | 25.52 | -0.83 | 0.50 | 2.93 | 4723 | 717 | 0 |

Gate: B's trainer delta (-0.46..+0.86) lies inside A's spread (-4.06..+1.43).
L15 loss is equal. Sync entries did not rise, and CRC fails fell by roughly the
decoy's hit count. **B passes.** The decoy fired ~110 times a minute here,
against 0-5 per leg in yesterday's acked/burst legs, because today's legs ran a
scan channel and up to three tracked channels.

## Final state

`final_base` (`tt_final`, 120 s, same shape as `p0_base`): L15 TI 0.21 %,
Garmin 2.36 %; trainer TI 31.17 %, Garmin 31.76 %; alt_sync_hits 0. ant_probe
PASS and 0xF7 answers on every image flashed.

Boards left: TI carries `tt_final` (md5 `47BA63E8…`). The Pro Micro clone, the
L15 DK and the Garmin are unchanged.

Not run: the twister suite. Only the cc26xx build compiles
`radiant_radio_cc26xx.c`, and the new Kconfig symbols depend on
`RADIANT_BACKEND_CC26XX`. The TI build is the check, and it is green.
