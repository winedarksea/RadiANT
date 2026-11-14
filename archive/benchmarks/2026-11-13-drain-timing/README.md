# Receive-drain and scheduler-pass timing, 2026-11-13

Phase C of the plan acting on `docs/performance-review-2026-11-13.md`
(findings 5 and the TI CRC note). This phase only measured; nothing was
optimized. The instrument is 0xF7 page 8 (commit `02458c8`, ADR 0023):

| # | field | kind |
|---|---|---|
| 0 | `drain_max_frames`: frames in the drain that set `drain_max_us` | level, cc26xx |
| 1 | `frame_max_us`: longest work on one receive entry, core callback excluded | level, cc26xx |
| 2 | `crc_max_us`: longest `crc_compute()` | level, cc26xx |
| 3 | `rx_cb_max_us`: longest synchronous core rx callback | level, cc26xx |
| 4 | `pass_max_us`: longest scheduler `pass()`, interrupts off | level, both |
| 5 | `pass_steps_max`: most `pass_step()` iterations in one pass | level, both |
| 6 | `pass_over_100us`: passes over 100 µs | count, both |
| 7 | `drain_over_250us`: drains over 250 µs | count, cc26xx |

Levels are worst cases since boot; each board was flashed (TI) or idle since
flashing (Pro Micro) before its window, so they cover the window plus idle.

## Rig and workload

Each board ran one 300 s `ant_scan_rate.py` window: a never-locking
background scan reopened every 4.5 s (as Zwift does), plus two tracked
channels. One was an HR sensor, `ant_sim.py --profile heart-rate` #14955 on the
nRF54L15 DK's UART dongle (COM4), since no HR strap was on the air this
evening. The other was the trainer #52233 FE-C (0x11). 0xF7 was read before
and after (`*_before.json`, `*_after.json`, `*_diff.txt`).

- TI: LAUNCHXL-CC26X2R1 on COM11, `dongle_ti` with
  `ti_health.conf;ti_diag.conf` from `02458c8`, md5
  `F498C93CE1938999B4E4F254886373B9`.
- nRF: Pro Micro `EE6986854FD72803`, `apps/dongle` with `diag_counters.conf`
  from `02458c8`, md5 `EEE2D96863FEFB8A1F46BB691738A562`.

The `*_no_hr_tx/` folders hold first attempts whose HR transmitter never ran.
On TI it was a wrong profile name; on the Pro Micro, COM4 was still held by
the previous run's simulator. They tracked only the trainer. They are kept
because their page-8 numbers agree with the runs below.

## Results

| | TI CC2652R | Pro Micro nRF52840 |
|---|---|---|
| HR #14955 tracked loss | 0.59 % (1183 rx) | 0.08 % (1217 rx) |
| trainer #52233 FE-C loss | 32.1 % | 20.6 % |
| scan sightings/s | 5.40 | 6.48 |
| scheduler dones (page 0) | 4729 | 4830 |
| page 4 `drain_max_us` | **452** | n/a |
| `drain_max_frames` | 1 | n/a |
| `frame_max_us` (backend, callback excluded) | 123 | n/a |
| `crc_max_us` | 60 | n/a |
| `rx_cb_max_us` (core callback) | **346** | n/a |
| `pass_max_us` | 214 | 216 |
| `pass_steps_max` | **1** | **1** |
| `pass_over_100us` (300 s) | 4790 | 4883 |
| `drain_over_250us` (300 s) | 2016 | n/a |

(The trainer losses are the room, as all week. Treat the two boards' loss
figures as context only; they were not measured in the same window.)

## Where the ~450-530 µs TI drain goes

The longest drain handled **one** frame, so its length is not a backlog of
entries. Taking the worst case of each part:

- **~346 µs (about three quarters): the core's synchronous rx callback.** That
  is the scheduler's `hal_rx()`, the antr frame handler, and then a full
  `pass()` that arms the next window from inside the callback. The pass alone
  reaches ~214 µs.
- **~123 µs: the backend's own per-entry work.** Of that, **~60 µs is the
  bitwise CRC**, about three times the 15-20 µs estimated earlier. The rest is
  `rx_t_sync()`, `rx_match()` and the copies.

123 + 346 = 469 µs, against a measured worst of 452 µs. Each figure is a
separate maximum, so their sum is an upper bound, which is consistent.
`drain_over_250us` fired 2016 times in 300 s. A drain over 250 µs is normal on
TI, not an outlier.

## The scheduler pass

- **`pass_steps_max` is 1 on both backends.** The loop bound (68 iterations
  for 32 channels) is never approached. A long pass is one expensive
  `pass_step()`, not many cheap ones.
- **Roughly every pass that does work takes over 100 µs** on both parts.
  `pass_over_100us` is about equal to scheduler dones on each board. The worst
  is ~215 µs on both. The nRF52840 at 64 MHz and the CC2652R at 48 MHz land on
  the same number, which suggests the time goes to something both backends
  wait on (arming, i.e. the backend call), not to CPU-bound table scanning.
  This is an inference: page 8 does not split the step.

Limits: each timestamp on TI is an `RF_getCurrentTime()` call (the RAT
through the RF driver), and there are six per entry when diagnostics are on,
so a few µs of each figure is the instrument. Truncation to whole µs on 0.25 µs
ticks loses less than 1 µs per field.

## What the numbers say next (not done here)

1. **Callback/pass split first.** The pass inside the synchronous rx callback
   is the largest single block of the drain. Deferring the re-arm out of the
   drain, or splitting the callback from the pass, attacks ~200 of the ~450 µs.
2. **Time inside `pass_step()` before choosing a scheduler fix.** Split the
   slot-table scan from the backend arm call. An active-slot bitmap helps only
   if the scan dominates. Steps = 1 and the same time on two CPUs point at the
   arm.
3. **A CRC table on TI is worth ~50 µs per frame** (60 µs bitwise; a 256-entry
   CRC-16/CCITT table typically runs a few µs for 10 bytes). It is the
   cheapest of the three and independent of them.
