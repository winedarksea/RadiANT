# STARTUP latency after SYSTEM_RESET (TODO item 5), 2026-11-17

A = master 3e07675. B = `fix/startup-latency` 6a851dd: firmware fences replace the 50 ms sleep, plus the
8da4313 tool fix. Each board ran A and B with the same configs as its bench image:

| Board | Transport | Build |
|---|---|---|
| Pro Micro EE6986854FD72803 | legacy USB | apps/dongle + `diag_counters.conf` |
| TI LaunchXL COM11 | UART 57600 | apps/dongle_ti + `ti_health.conf;ti_diag.conf` |
| nRF54L15 DK COM4 | UART 57600, MPSL | apps/dongle + `gate.conf`, `uart_57600.conf`/`.overlay`, `diag_counters.conf` |

Tool: `tools/ant_reset_loop.py`. Read timeout 1000 ms unless noted; `--after-ms 300`.
In traffic runs a slave channel is pinned to a sim power master, #54321 at 8182 counts on the nRF5340 DK net core
(COM5). Each reset then waits a random 0-0.5 s sleep (`--jitter-s 0.5 --seed 1`).

## Latency, SYSTEM_RESET write to STARTUP read (ms)

| Board | A p50 / p99 | B p50 / p99 | Lost STARTUPs, A -> B |
|---|---|---|---|
| Pro Micro, idle (1000) | 52.66 / 73.16 | 2.63 / 3.61 | 0 -> 0 |
| Pro Micro, 50 ms reads (1000) | 52.63 / 68.37 | 2.68 / 3.81 | **5 -> 0** |
| Pro Micro, traffic (500) | 52.81 / 64.22 \* | 2.81 / 3.92 | 0 -> 0 |
| TI, idle (1000) | 55.38 / 55.67 | 5.41 / 6.01 | 0 -> 0 |
| TI, traffic (500) | 55.43 / 55.56 \* | 5.50 / 6.08 | 0 -> 0 |
| L15 MPSL, idle (1000) | 53.15 / 53.38 | 3.11 / 3.95 | 0 -> 0 |
| L15 MPSL, traffic (500) | 53.17 / 53.34 \* | 3.16 / 3.46 | 0 -> 0 |
| Garmin USB-m 006, idle (1000) | 0.66 / 1.24 (reference) | | 0 |

B's figures are A's minus the 50 ms sleep. What remains is transport:

- TI, ~5.4 ms: the XDS110 backchannel.
- L15, ~3.1 ms: the J-Link VCOM at 57600.
- Pro Micro, ~2.6 ms: libusb0 polling.

On every board in every run, nothing reached the host between 0x4A and 0x6F except frames queued before the reset, and
nothing followed 0x6F. Zero violations of either kind.

\* The A traffic runs used the first version of `--jitter-s` (files `*_readjitter*`). See the trap below.

## Negative controls (Pro Micro, traffic, 600 resets each, same seed)

The control images are B plus bench-only `#ifdef`s. They were built in a detached worktree and never committed.

| Image | Change | Stale frames after STARTUP |
|---|---|---|
| c1none | `k_busy_wait(3000)` after the reset check in the event path; fence 1 off; antr drain wait off | **7** |
| c1f1 | same, fence 1 on (drain wait still off) | 0 |
| c2none | 2 ms sleep before each TX-thread write; fence 2 off | **3** |
| c2f2 | same, fence 2 on | 0 |

7/600 matches the expected rate: a 3 ms window over a 245 ms period is 1.2 %.

All 7 c1none hits and all 3 c2none hits fall at jitter values 0.246-0.248 s or 0.496 s. Those are whole periods after
the last frame, exactly where the injected delay sits. With the same seed, iterations 4, 29 and 395 leaked in both "none"
images and stayed clean in both fenced ones.

**Trap, fixed in 8da4313.** The first control run (`*_readjitter*`, 400 each) found 0 in every image. `--jitter-s` was
then a read-dwell. A 1 s USB read returns on the next frame whatever its deadline, so every reset went out just after a
broadcast, when the event and TX paths are idle: the same phase lock as `bench-reopen-phase-lock`. A plain sleep fixed
it.

## Conformance (`tools/ant_conformance.py`, full run, ~300 resets)

| Board | A vs B | B vs archived reference |
|---|---|---|
| TI | byte-identical (both a3a82dbe...) | one record differs in `frame/malformed-oversize`, an INTENTIONAL_DIVERGENCE case |
| Pro Micro | byte-identical (both e4bbb375...) | the same single record differs |

A shows the same archive difference, so it predates this change.

## Zwift path (TI, B)

`tools/ant_zwift_visible.py --dll <scratch copy of Zwift's ANT_DLL>` with the current shim build (not installed):
**PASS**. In `shim_ti_B.log` the SI_Write of 0x4A is at 12979.486260 and the last STARTUP byte is read at 12979.491837:
**5.6 ms**, against about 55 ms on A.

## dongle_thread self channels (L15, B and A, 300 resets each)

Image: `thread.conf;bridge.conf` + `CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT=y`. Masters:

- The Garmin as an HR sim, #54322, bound to self channel 6.
- The power sim on host channel 0.

| | A | B |
|---|---|---|
| Self channels 6 and 7 re-established after host reset | 300/300 each | 300/300 each |
| Host channel 0 opened | 300/300 | 300/300 |
| Host channel 0 heard nothing in the whole iteration | 11 | 9 |
| STARTUP p50 | 52.4 ms | 2.3 ms |

No host channel was released as the bridge's. The ~10 silent iterations are the same on A, so they are search contention
with the searching self channel 7, not this change. Consoles: `self_l15_{A,B}_console.log`.

## Not done

- dongle_thread on `nrf5340dk/nrf5340/cpuapp` fails Kconfig on A and B alike (RADIANT_BACKEND_NRF_GATE_MPSL), so that
  recipe is not a valid build. dongle_thread was built on L15 and nRF52840 DK instead.
- No nRF52840 Dongle on the bench, so no test there.
