# 2026-11-07-fixed: receivers vs Garmin

Generated 2026-11-07T08:47:21.682119+00:00. Reference: **Garmin** (usb:006).

**Verdict tally:** Better 4, Equal 9, Worse 3, Inconclusive 20

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
| A | loss | Feather | **Better** | 0.00 % [0.00, 0.16] vs reference 0.33 % [0.17, 0.65] |
| A | loss | Dongle | **Better** | 0.00 % [0.00, 0.16] vs reference 0.33 % [0.17, 0.65] |
| A | loss | 54L15 | **Equal** | 0.08 % [0.02, 0.30] vs reference 0.33 % [0.17, 0.65] |
| A | loss (multi, pooled) | Feather | **Inconclusive** | a figure is missing |
| A | loss (multi, pooled) | Dongle | **Inconclusive** | a figure is missing |
| A | loss (multi, pooled) | 54L15 | **Inconclusive** | a figure is missing |
| A | acquire | Feather | **Equal** | median 0.20 s (IQR 0.03-0.20, 0 miss) vs reference 0.19 s (IQR 0.12-0.22, 0 miss) |
| A | acquire | Dongle | **Equal** | median 0.17 s (IQR 0.14-0.20, 0 miss) vs reference 0.19 s (IQR 0.12-0.22, 0 miss) |
| A | acquire | 54L15 | **Equal** | median 0.18 s (IQR 0.04-0.23, 0 miss) vs reference 0.19 s (IQR 0.12-0.22, 0 miss) |
| A | ladder | Feather | **Inconclusive** | the reference's ladder never reached its knee: the quietest rung (-40 dBm) still only loses 2.50 %, so the knee is below the bottom of the ladder. The ladder spans 40 dB and the link needs more: move the boards apart and run it again. Extrapolating past the last rung would invent the answer |
| A | ladder | Dongle | **Inconclusive** | the reference's ladder never reached its knee: the quietest rung (-40 dBm) still only loses 2.50 %, so the knee is below the bottom of the ladder. The ladder spans 40 dB and the link needs more: move the boards apart and run it again. Extrapolating past the last rung would invent the answer |
| A | ladder | 54L15 | **Inconclusive** | the reference's ladder never reached its knee: the quietest rung (-40 dBm) still only loses 2.50 %, so the knee is below the bottom of the ladder. The ladder spans 40 dB and the link needs more: move the boards apart and run it again. Extrapolating past the last rung would invent the answer |
| A | acked | Feather | **Inconclusive** | 0.00 % [0.00, 3.10] vs reference 0.00 % [0.00, 3.10]; the interval straddles the 0.5 pp margin |
| A | acked | Dongle | **Inconclusive** | 0.00 % [0.00, 3.10] vs reference 0.00 % [0.00, 3.10]; the interval straddles the 0.5 pp margin |
| A | acked | 54L15 | **Inconclusive** | 0.00 % [0.00, 3.10] vs reference 0.00 % [0.00, 3.10]; the interval straddles the 0.5 pp margin |
| B | loss | Feather | **Worse** | 2.16 % [1.65, 2.82] vs reference 0.29 % [0.14, 0.60] |
| B | loss | Dongle | **Worse** | 2.58 % [2.02, 3.29] vs reference 0.29 % [0.14, 0.60] |
| B | loss | 5340 | **Worse** | 2.00 % [1.51, 2.64] vs reference 0.29 % [0.14, 0.60] |
| B | loss (multi, pooled) | Feather | **Inconclusive** | a figure is missing |
| B | loss (multi, pooled) | Dongle | **Inconclusive** | a figure is missing |
| B | loss (multi, pooled) | 5340 | **Inconclusive** | a figure is missing |
| B | acquire | Feather | **Equal** | median 0.20 s (IQR 0.14-0.22, 0 miss) vs reference 0.16 s (IQR 0.03-0.22, 0 miss) |
| B | acquire | Dongle | **Equal** | median 0.18 s (IQR 0.14-0.20, 0 miss) vs reference 0.16 s (IQR 0.03-0.22, 0 miss) |
| B | acquire | 5340 | **Equal** | median 0.17 s (IQR 0.00-0.21, 0 miss) vs reference 0.16 s (IQR 0.03-0.22, 0 miss) |
| B | ladder | Feather | **Inconclusive** | the receiver's dial (RSSI slope) check failed |
| B | ladder | Dongle | **Inconclusive** | the receiver's ladder never reached its knee: the quietest rung (-28 dBm) still only loses 1.67 %, so the knee is below the bottom of the ladder. The ladder spans 36 dB and the link needs more: move the boards apart and run it again. Extrapolating past the last rung would invent the answer |
| B | ladder | 5340 | **Inconclusive** | the receiver's dial (RSSI slope) check failed |
| B | acked | Feather | **Inconclusive** | 0.00 % [0.00, 3.10] vs reference 0.00 % [0.00, 3.10]; the interval straddles the 0.5 pp margin |
| B | acked | Dongle | **Inconclusive** | 0.00 % [0.00, 6.02] vs reference 0.00 % [0.00, 3.10]; the interval straddles the 0.5 pp margin |
| B | acked | 5340 | **Inconclusive** | 0.00 % [0.00, 3.10] vs reference 0.00 % [0.00, 3.10]; the interval straddles the 0.5 pp margin |
| C | loss (multi, pooled) | Feather | **Inconclusive** | a figure is missing |
| C | loss (multi, pooled) | Dongle | **Inconclusive** | a figure is missing |
| C | acquire | Feather | **Equal** | median 0.10 s (IQR 0.03-0.19, 0 miss) vs reference 0.12 s (IQR 0.01-0.20, 0 miss) |
| C | acquire | Dongle | **Equal** | median 0.20 s (IQR 0.14-0.22, 0 miss) vs reference 0.12 s (IQR 0.01-0.20, 0 miss) |
| - | usb | Feather | **Better** | p50 0.339 ms / p99 0.462 ms vs reference p50 0.581 / p99 0.688 |
| - | usb | Dongle | **Better** | p50 0.326 ms / p99 0.416 ms vs reference p50 0.581 / p99 0.688 |

## Round A - transmitter(s): 5340

### loss, % per window

| receiver | W1 | W2 |
|---|---|---|
| Garmin | 0.42 | 0.25 |
| Feather | 0.00 | 0.00 |
| Dongle | 0.00 | 0.00 |
| 54L15 | 0.00 | 0.17 |

### loss (multi, pooled), % per window

| receiver | W1 |
|---|---|
| Garmin | - |
| Feather | - |
| Dongle | - |
| 54L15 | - |

### multi, loss % per profile (pooled over windows; `wc` = wall-clock basis, no exact counter)

| receiver |  |
|---|
| Garmin |  |
| Feather |  |
| Dongle |  |
| 54L15 |  |

### acquisition, time to first packet (s)

| receiver | median | Q1 | Q3 | acquired | misses |
|---|---|---|---|---|---|
| Garmin | 0.19 | 0.12 | 0.22 | 20/20 | 0 |
| Feather | 0.20 | 0.03 | 0.20 | 20/20 | 0 |
| Dongle | 0.17 | 0.14 | 0.20 | 20/20 | 0 |
| 54L15 | 0.18 | 0.04 | 0.23 | 20/20 | 0 |

### sensitivity knee (5 % loss)

| receiver | knee TX dBm | knee at RX dBm | dial | note |
|---|---|---|---|---|
| Garmin | - | - | ok | the quietest rung (-40 dBm) still only loses 2.50 %, so the knee is below the bottom of the ladder. The ladder spans 40 dB and the link needs more: move the boards apart and run it again. Extrapolating past the last rung would invent the answer |
| Feather | -21.00 | -62.0 | ok | interpolated between the two rungs that bracket it; the bracket is 20 dB wide, which is most of what this number is made of - use --rungs fine for a tighter one |
| Dongle | -35.00 | -66.0 | ok | interpolated between the two rungs that bracket it; the bracket is 20 dB wide, which is most of what this number is made of - use --rungs fine for a tighter one |
| 54L15 | -21.79 | -69.8 | ok | interpolated between the two rungs that bracket it; the bracket is 20 dB wide, which is most of what this number is made of - use --rungs fine for a tighter one |

### acknowledged transfers (receiver as originator)

| receiver | sent | completed | failed | peer received | blocks | errors |
|---|---|---|---|---|---|---|
| Garmin | 120 | 120 | 0 | 120 | 2 | 0 |
| Feather | 120 | 120 | 0 | 120 | 2 | 0 |
| Dongle | 120 | 120 | 0 | 120 | 2 | 0 |
| 54L15 | 120 | 120 | 0 | 120 | 2 | 0 |

### window warnings

- multi 1: Garmin: no json written - see log
- multi 1: Garmin: no JSON written
- multi 1: Feather: no json written - see log
- multi 1: Feather: no JSON written
- multi 1: Dongle: no json written - see log
- multi 1: Dongle: no JSON written
- multi 1: 54L15: no json written - see log
- multi 1: 54L15: no JSON written
- multi 1: 5340: ant_sim rc 1
- multi 1: 5340: 19 messages on the wall-clock fallback (pacing guessed)

## Round B - transmitter(s): 54L15

### loss, % per window

| receiver | W1 | W2 |
|---|---|---|
| Garmin | 0.33 | 0.25 |
| Feather | 2.00 | 2.33 |
| Dongle | 2.50 | 2.66 |
| 5340 | 1.91 | 2.08 |

### loss (multi, pooled), % per window

| receiver | W1 |
|---|---|
| Garmin | - |
| Feather | - |
| Dongle | - |
| 5340 | - |

### multi, loss % per profile (pooled over windows; `wc` = wall-clock basis, no exact counter)

| receiver |  |
|---|
| Garmin |  |
| Feather |  |
| Dongle |  |
| 5340 |  |

### acquisition, time to first packet (s)

| receiver | median | Q1 | Q3 | acquired | misses |
|---|---|---|---|---|---|
| Garmin | 0.16 | 0.03 | 0.22 | 20/20 | 0 |
| Feather | 0.20 | 0.14 | 0.22 | 20/20 | 0 |
| Dongle | 0.18 | 0.14 | 0.20 | 20/20 | 0 |
| 5340 | 0.17 | 0.00 | 0.21 | 20/20 | 0 |

### sensitivity knee (5 % loss)

| receiver | knee TX dBm | knee at RX dBm | dial | note |
|---|---|---|---|---|
| Garmin | - | - | FAIL | the quietest rung (-28 dBm) still only loses 5.00 %, so the knee is below the bottom of the ladder. The ladder spans 36 dB and the link needs more: move the boards apart and run it again. Extrapolating past the last rung would invent the answer |
| Feather | -27.38 | -62.9 | FAIL | interpolated between the two rungs that bracket it; the bracket is 8 dB wide, which is most of what this number is made of - use --rungs fine for a tighter one |
| Dongle | - | - | FAIL | the quietest rung (-28 dBm) still only loses 1.67 %, so the knee is below the bottom of the ladder. The ladder spans 36 dB and the link needs more: move the boards apart and run it again. Extrapolating past the last rung would invent the answer |
| 5340 | -11.90 | -63.9 | FAIL | interpolated between the two rungs that bracket it |

### acknowledged transfers (receiver as originator)

| receiver | sent | completed | failed | peer received | blocks | errors |
|---|---|---|---|---|---|---|
| Garmin | 120 | 120 | 0 | 120 | 2 | 0 |
| Feather | 120 | 120 | 0 | 120 | 2 | 0 |
| Dongle | 60 | 60 | 0 | 60 | 2 | 1 |
| 5340 | 120 | 120 | 0 | 120 | 2 | 0 |

### window warnings

- multi 1: Garmin: no json written - see log
- multi 1: Garmin: no JSON written
- multi 1: Feather: no json written - see log
- multi 1: Feather: no JSON written
- multi 1: Dongle: no json written - see log
- multi 1: Dongle: no JSON written
- multi 1: 5340: no json written - see log
- multi 1: 5340: no JSON written
- multi 1: 54L15: ant_sim rc 1
- multi 1: 54L15: 9 messages on the wall-clock fallback (pacing guessed)
- acked 1: pass 1 Dongle: RuntimeError: endpoint 3D55F77818BE772A did not reset

## Round C - transmitter(s): 5340, 54L15

### loss (multi, pooled), % per window

| receiver | W1 | W2 |
|---|---|---|
| Garmin | - | - |
| Feather | - | - |
| Dongle | - | - |

### multi, loss % per profile (pooled over windows; `wc` = wall-clock basis, no exact counter)

| receiver |  |
|---|
| Garmin |  |
| Feather |  |
| Dongle |  |

### acquisition, time to first packet (s)

| receiver | median | Q1 | Q3 | acquired | misses |
|---|---|---|---|---|---|
| Garmin | 0.12 | 0.01 | 0.20 | 20/20 | 0 |
| Feather | 0.10 | 0.03 | 0.19 | 20/20 | 0 |
| Dongle | 0.20 | 0.14 | 0.22 | 20/20 | 0 |

### window warnings

- multi 1: Garmin: no json written - see log
- multi 1: Garmin: no JSON written
- multi 1: Feather: no json written - see log
- multi 1: Feather: no JSON written
- multi 1: Dongle: no json written - see log
- multi 1: Dongle: no JSON written
- multi 2: Garmin: no json written - see log
- multi 2: Garmin: no JSON written
- multi 2: Feather: no json written - see log
- multi 2: Feather: no JSON written
- multi 2: Dongle: no json written - see log
- multi 2: Dongle: no JSON written

## USB round trip

| receiver | p50 ms | p90 ms | p99 ms | max ms | timeouts | msg/s |
|---|---|---|---|---|---|---|
| Garmin | 0.581 | 0.626 | 0.688 | 5.100 | 0 | 2007 |
| Feather | 0.339 | 0.364 | 0.462 | 0.567 | 0 | 3016 |
| Dongle | 0.326 | 0.374 | 0.416 | 0.501 | 0 | 3018 |

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
