# 2026-11-07-fixed: receivers vs Garmin

Generated 2026-11-07T09:44:43.013669+00:00. Reference: **Garmin** (usb:006).

**Verdict tally:** Better 7, Equal 3, Worse 2, Inconclusive 2

## Devices

| device | address | stack | part | version | channels/networks |
|---|---|---|---|---|---|
| Garmin | `usb:006` | reference | - | AJK2.05RAF | 8/8 |
| Feather | `usb:755972D7183A6183` | radiant | nrf52840 | RADIANT0.01B00 | 32/3 |
| Dongle | `usb:3D55F77818BE772A` | radiant | nrf52840 | RADIANT0.01B00 | 32/3 |
| 5340 | `port:COM5` | radiant | nrf5340net | RADIANT0.01B00 | 32/3 |
| 54L15 | `port:COM4` | radiant | nrf54l15 | RADIANT0.01B00 | 32/3 |

## Placement

```toml

[placement]
notes = "TODO: describe the room, the bench surface and anything metal nearby"

[placement.positions]
Feather = "TODO: position / orientation / USB port or hub"
Dongle = "TODO"
Garmin = "TODO"
5340 = "TODO"
54L15 = "TODO"

[placement.distances_cm]
5340-Feather = 0
5340-Dongle = 0
5340-Garmin = 0
54L15-Feather = 0
54L15-Dongle = 0
54L15-Garmin = 0
```

## Verdicts

| round | metric | receiver | verdict | detail |
|---|---|---|---|---|
| A | loss | Feather | **Equal** | 0.00 % [0.00, 0.16] vs reference 0.04 % [0.01, 0.24] |
| A | loss | Dongle | **Equal** | 0.00 % [0.00, 0.16] vs reference 0.04 % [0.01, 0.24] |
| A | loss | 54L15 | **Equal** | 0.00 % [0.00, 0.16] vs reference 0.04 % [0.01, 0.24] |
| A | loss (multi, pooled) | Feather | **Better** | 0.72 % [0.52, 1.01] vs reference 3.43 % [2.96, 3.99] (window warnings present - see below) |
| A | loss (multi, pooled) | Dongle | **Better** | 0.72 % [0.52, 1.01] vs reference 3.43 % [2.96, 3.99] (window warnings present - see below) |
| A | loss (multi, pooled) | 54L15 | **Inconclusive** | a figure is missing |
| B | loss | Feather | **Worse** | 2.58 % [2.02, 3.29] vs reference 0.29 % [0.14, 0.60] |
| B | loss | Dongle | **Worse** | 2.41 % [1.87, 3.10] vs reference 0.29 % [0.14, 0.60] |
| B | loss | 5340 | **Inconclusive** | 2.58 % [2.02, 3.30] vs reference 0.29 % [0.14, 0.60]; but the gap moved 1.09 pp between windows (limit 1.0 pp), so it does not repeat |
| B | loss (multi, pooled) | Feather | **Better** | 0.41 % [0.27, 0.64] vs reference 3.43 % [2.95, 3.98] (window warnings present - see below) |
| B | loss (multi, pooled) | Dongle | **Better** | 0.43 % [0.28, 0.66] vs reference 3.43 % [2.95, 3.98] (window warnings present - see below) |
| B | loss (multi, pooled) | 5340 | **Better** | 0.48 % [0.32, 0.71] vs reference 3.43 % [2.95, 3.98] (window warnings present - see below) |
| C | loss (multi, pooled) | Feather | **Better** | 0.81 % [0.65, 1.01] vs reference 3.85 % [3.48, 4.25] |
| C | loss (multi, pooled) | Dongle | **Better** | 0.95 % [0.78, 1.17] vs reference 3.85 % [3.48, 4.25] |

## Round A - transmitter(s): 5340

### loss, % per window

| receiver | W1 | W2 |
|---|---|---|
| Garmin | 0.00 | 0.08 |
| Feather | 0.00 | 0.00 |
| Dongle | 0.00 | 0.00 |
| 54L15 | 0.00 | 0.00 |

### loss (multi, pooled), % per window

| receiver | W1 |
|---|---|
| Garmin | 3.43 |
| Feather | 0.72 |
| Dongle | 0.72 |
| 54L15 | - |

### multi, loss % per profile (pooled over windows; `wc` = wall-clock basis, no exact counter)

| receiver | power | heart-rate | csc | fec-treadmill |
|---|---|---|---|---|
| Garmin | 3.58 wc | 3.78 wc | 2.96 wc | 3.42 wc |
| Feather | 0.83 wc | 0.66 wc | 0.58 wc | 0.83 wc |
| Dongle | 0.83 wc | 0.66 wc | 0.58 wc | 0.83 wc |
| 54L15 | - | - | - | - |

### window warnings

- multi 1: 54L15: no json written - see log
- multi 1: 54L15: no JSON written
- multi 1: 5340: ant_sim rc 1
- multi 1: 5340: 23 messages on the wall-clock fallback (pacing guessed)

## Round B - transmitter(s): 54L15

### loss, % per window

| receiver | W1 | W2 |
|---|---|---|
| Garmin | 0.25 | 0.33 |
| Feather | 2.58 | 2.58 |
| Dongle | 2.33 | 2.49 |
| 5340 | 3.08 | 2.08 |

### loss (multi, pooled), % per window

| receiver | W1 |
|---|---|
| Garmin | 3.43 |
| Feather | 0.41 |
| Dongle | 0.43 |
| 5340 | 0.48 |

### multi, loss % per profile (pooled over windows; `wc` = wall-clock basis, no exact counter)

| receiver | power | heart-rate | csc | fec-treadmill |
|---|---|---|---|---|
| Garmin | 3.83 wc | 3.20 wc | 3.20 wc | 3.50 wc |
| Feather | 0.42 wc | 0.16 wc | 0.49 wc | 0.58 wc |
| Dongle | 0.50 wc | 0.16 wc | 0.41 wc | 0.67 wc |
| 5340 | 0.50 wc | 0.25 wc | 0.49 wc | 0.67 wc |

### window warnings

- multi 1: 54L15: ant_sim rc 1
- multi 1: 54L15: 11 messages on the wall-clock fallback (pacing guessed)

## Round C - transmitter(s): 5340, 54L15

### loss (multi, pooled), % per window

| receiver | W1 | W2 |
|---|---|---|
| Garmin | 3.64 | 4.05 |
| Feather | 0.79 | 0.83 |
| Dongle | 0.95 | 0.95 |

### multi, loss % per profile (pooled over windows; `wc` = wall-clock basis, no exact counter)

| receiver | power | heart-rate | csc | fec-treadmill |
|---|---|---|---|---|
| Garmin | 3.62 wc | 4.15 wc | 4.15 wc | 3.46 wc |
| Feather | 0.37 wc | 1.48 wc | 1.24 wc | 0.13 wc |
| Dongle | 0.50 wc | 1.48 wc | 1.48 wc | 0.33 wc |

## Decision rules (pre-registered)

- **loss**: Wilson 95 % CI on exact loss. Equal: RadiANT upper CI <= reference point + 0.5 pp. Better: RadiANT upper CI < reference lower CI. Worse: RadiANT lower CI > reference point + 0.5 pp.
- **acquire**: Median time to first packet over the cycles. Equal: <= reference median + 0.25 s. Better: lower median AND RadiANT Q3 < reference Q1. Worse: > reference median + 0.25 s.
- **ladder**: 5 %-loss TX power. Equal: within 1 dB (the ab_gates repeatability limit). Better: more than 1 dB lower. Worse: more than 1 dB higher. Inconclusive if either dial check failed or either ladder never reached its knee.
- **acked**: Wilson 95 % CI on the acknowledged-transfer failure rate (sent - completed), the loss rule applied unchanged.
- **usb**: ant_bench round-trip latency. Equal: median <= reference median x 1.1. Better: lower median AND lower p99. Worse: median > reference median x 1.1.

## Caveats

- Every figure is for this fixed placement: antenna and position are part of each result until a swap round separates them.
- The DKs transmit from ant_sim.py (EVENT_TX-paced host masters), not from a sensor's own firmware.
- The nRF5340 network core transmits at 0 dBm at most (no positive TXPOWER setting in silicon), up to 8 dB below the other parts - its rounds sit lower on the link budget.
- 5340, 54L15 attach over UART, not USB: excluded from the usb phase, and their receive path includes a UART rather than the USB stack a real dongle has.
- RSSI is not comparable across chip families (each reports on its own calibration); it is shown, never judged.
- Some loss figures are wall-clock (received radio messages against the window's slot count), not the transmitter's event counter: ant_verify withholds 'loss (exact)' once the counter stands still even once, and the DK master occasionally re-sends its previous buffer when a host refill misses a slot. That repeat lands on every receiver in the window alike, so the receiver-minus-reference gap is unaffected; the absolute figures assume the transmitter's crystal is exact.

## Commands

Every child command is in `summary.json` under `records[].commands`; to repeat the whole run:

```
python tools/ant_compare.py --rig tools\compare_rigs\2026-11-07-fixed.toml
```
