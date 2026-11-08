# 2026-11-07-vcom-soak: receivers vs Garmin

Generated 2026-11-07T23:58:08.828992+00:00. Reference: **Garmin** (usb:006).

**Verdict tally:** Better 2

## Devices

| device | address | stack | part | version | channels/networks |
|---|---|---|---|---|---|
| Garmin | `usb:006` | reference | - | - | -/- |
| Dongle | `usb:3D55F77818BE772A` | radiant | nrf52840 | - | -/- |
| 54L15 | `port:COM4` | radiant | nrf54l15 | - | -/- |
| Feather | `usb:755972D7183A6183` | radiant | nrf52840 | - | -/- |

## Placement

```toml

[placement]
notes = "Same bench as 2026-11-07-fixed; boards unmoved."
```

## Verdicts

| round | metric | receiver | verdict | detail |
|---|---|---|---|---|
| S | loss (multi, pooled) | Dongle | **Better** | 0.58 % [0.52, 0.66] vs reference 2.96 % [2.81, 3.12] (window warnings present - see below) |
| S | loss (multi, pooled) | 54L15 | **Better** | 0.56 % [0.50, 0.63] vs reference 2.96 % [2.81, 3.12] (window warnings present - see below) |

## Round S - transmitter(s): Feather

### loss (multi, pooled), % per window

| receiver | W1 | W2 | W3 | W4 | W5 | W6 | W7 | W8 | W9 | W10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Garmin | 3.02 | 2.81 | 2.94 | 2.96 | 2.87 | 2.81 | 3.08 | 3.00 | 3.04 | 3.08 |
| Dongle | 0.60 | 0.62 | 0.66 | 0.58 | 0.60 | 0.60 | 0.54 | 0.54 | 0.60 | 0.50 |
| 54L15 | 0.54 | 0.60 | 0.70 | 0.56 | 0.58 | 0.60 | 0.52 | 0.54 | 0.56 | 0.46 |

### multi, loss % per profile (pooled over windows; `wc` = wall-clock basis, no exact counter)

| receiver | power | heart-rate | csc | fec-treadmill |
|---|---|---|---|---|
| Garmin | 2.75 wc | 3.27 wc | 2.98 wc | 2.84 wc |
| Dongle | 0.57 wc | 0.65 wc | 0.61 wc | 0.51 wc |
| 54L15 | 0.54 wc | 0.64 wc | 0.56 wc | 0.50 wc |

### window warnings

- multi 1: Feather: ant_sim rc 1
- multi 1: Feather: 10 messages on the wall-clock fallback (pacing guessed)
- multi 2: Feather: ant_sim rc 1
- multi 2: Feather: 9 messages on the wall-clock fallback (pacing guessed)
- multi 3: 54L15: ANT port died at 204 s: SerialException: Cannot configure port, something went wrong. Original message: PermissionError(13, 'A device attached to the system is not functioning.', None, 31)
- multi 3: 54L15: ran 205 s of a 300 s window (dropped port?)
- multi 3: Feather: ant_sim rc 1
- multi 3: Feather: 15 messages on the wall-clock fallback (pacing guessed)
- multi 4: Feather: ant_sim rc 1
- multi 4: Feather: 11 messages on the wall-clock fallback (pacing guessed)
- multi 5: Feather: ant_sim rc 1
- multi 5: Feather: 14 messages on the wall-clock fallback (pacing guessed)
- multi 6: Feather: ant_sim rc 1
- multi 6: Feather: 13 messages on the wall-clock fallback (pacing guessed)
- multi 7: Feather: ant_sim rc 1
- multi 7: Feather: 14 messages on the wall-clock fallback (pacing guessed)
- multi 8: Feather: ant_sim rc 1
- multi 8: Feather: 11 messages on the wall-clock fallback (pacing guessed)
- multi 9: Feather: ant_sim rc 1
- multi 9: Feather: 13 messages on the wall-clock fallback (pacing guessed)
- multi 10: Feather: ant_sim rc 1
- multi 10: Feather: 10 messages on the wall-clock fallback (pacing guessed)

## Decision rules (pre-registered)

- **loss**: Wilson 95 % CI on exact loss. Equal: RadiANT upper CI <= reference point + 0.5 pp. Better: RadiANT upper CI < reference lower CI. Worse: RadiANT lower CI > reference point + 0.5 pp.
- **acquire**: Median time to first packet over the cycles. Equal: <= reference median + 0.25 s. Better: lower median AND RadiANT Q3 < reference Q1. Worse: > reference median + 0.25 s.
- **ladder**: 5 %-loss TX power. Equal: within 1 dB (the ab_gates repeatability limit). Better: more than 1 dB lower. Worse: more than 1 dB higher. Inconclusive if either dial check failed or either ladder never reached its knee.
- **acked**: Wilson 95 % CI on the acknowledged-transfer failure rate (sent - completed), the loss rule applied unchanged.
- **usb**: ant_bench round-trip latency. Equal: median <= reference median x 1.1. Better: lower median AND lower p99. Worse: median > reference median x 1.1.

## Caveats

- Every figure is for this fixed placement: antenna and position are part of each result until a swap round separates them.
- The Feather transmits from ant_sim.py (EVENT_TX-paced host masters) on its pre-SLIP_GUARD image, at its bridge default power.
- A drop is the finding here, not the loss figures; four masters on one board share one clock.
- 54L15 attach over UART, not USB: excluded from the usb phase, and their receive path includes a UART rather than the USB stack a real dongle has.
- RSSI is not comparable across chip families (each reports on its own calibration); it is shown, never judged.
- Some loss figures are wall-clock (received radio messages against the window's slot count), not the transmitter's event counter: ant_verify withholds 'loss (exact)' once the counter stands still even once, and the DK master occasionally re-sends its previous buffer when a host refill misses a slot. That repeat lands on every receiver in the window alike, so the receiver-minus-reference gap is unaffected; the absolute figures assume the transmitter's crystal is exact.

## Commands

Every child command is in `summary.json` under `records[].commands`; to repeat the whole run:

```
python tools/ant_compare.py --rig tools\compare_rigs\2026-11-07-vcom-soak.toml
```
