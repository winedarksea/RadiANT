# Phase 0a: C5-1, A13-1 and the two MPSL refusal-path defects

Branch `wip/timing-triage`, based on master 7df6ada. Measured 2026-11-09.
Nothing here is merged, pushed or tagged. Raw logs are outside the repo; see
[phase-0a-raw-logs.md](phase-0a-raw-logs.md).

## Verdicts

| Item | Verdict | Evidence in one line |
|---|---|---|
| C5-1 (inline BLOCKED backs off the placed class) | **Keep** | ztest red without the fix, green with it; gate floor raised to 32 |
| A13-1 (zero-latency lock on the clock fold) | **Keep** | No race in the plan's 30-minute soak on either arm, so that soak proves nothing about the fix. A stress build counted 5931 backward folds on `irq_lock` and 0 on `CLOCK_LOCK` |
| ETIME from `program_rx()` on a grant, reported after the grant ends | **Not fixed, recorded** | No ztest can reach `radiant_radio_nrf.c`; 0 on-grant ETIMEs in 87 min of soak. Wrong comment at `gate_mpsl:1160` corrected |
| Pending `gate_failed` not cleared by `radiant_radio_abort()` | **Not fixed, recorded** | Same; 0 in the same soak |
| `program_tx()` compare ordering | Out of scope | As the plan says |

**One unresolved finding:** an MPSL assert (`69:108`) halted the DUT once, on the `CLOCK_LOCK` arm. It did not repeat. See [the assert](#the-mpsl-assert).

**TI:** none of this reaches TI code (`radiant_radio_nrf*.c` are nRF-only). Both
TI rows are binary-identical, build-only. Nothing is verified on air on TI.

## C5-1

Applied from `archive/review-2026-10-31-timing/6df53e7-timing-fixes.patch` with
the plan's `--include` set; `gate_probe_reset_elastic()` added by hand to
`tests/gate/gate_under_test.c`.

Reachable on today's master, on nRF5340 DK (`radiant\tests\gate`):

| State | Result | `test_an_inline_block_backs_off_the_class_that_was_refused` |
|---|---|---|
| Test only, fix absent | 31/32 | **FAIL**: `a refused elastic chunk did not shorten the next one (10000)` |
| Test and fix | 32/32 | pass |

Floors: `min_cases.radiant.gate` 31 to 32, `min_testcases` 3762 to 3763, in the
same commit.

## A13-1

Ported by hand. `CLOCK_LOCK()` is `radiant_swi_zli_lock()` (PRIMASK) when
`CORE_CB_DEFERRED` is defined, the same `irq_lock()` as before otherwise.
`radiant_dbg_clock_back` and the branch in `clock_fold()` are both behind
`CORE_CB_DEFERRED`. The patch's `LOG_DBG` in `program_rx()` was not ported.

### Zero-cost (C5-1 + A13-1 against merge base 7df6ada)

Compared by hand, mirroring release.yml's zero-cost job: allocated section sizes
and `nm --defined-only`, plus symbol sizes and a SHA-256 of `zephyr.bin`. Each row
was built from both trees with an explicit backend, and `.config` was read back
(`RADIANT_BACKEND_NRF=y` or `CC26XX=y`, and no MPSL gate).

| Row | Board | Sections | Symbols | Sizes | `zephyr.bin` |
|---|---|---|---|---|---|
| feather | adafruit_feather_nrf52840/nrf52840/uf2 | same (30) | same (2167) | same | identical |
| nrf52840dongle | nrf52840dongle/nrf52840 | same (30) | same (2162) | same | identical |
| promicro | promicro_nrf52840/nrf52840/uf2 | same (30) | same (2164) | same | identical |
| net5340 | nrf5340dk/nrf5340/cpunet | same (27) | same (1952) | same | identical |
| l15 | nrf54l15dk/nrf54l15/cpuapp | same (26) | same (2106) | same | identical |
| lm20 | nrf54lm20dk/nrf54lm20a/cpuapp | same (32) | same (2513) | same | identical |
| ti | cc26x2r1_launchxl, apps/dongle_ti | same (23) | same (1796) | same | identical |
| ti_gate | cc26x2r1_launchxl, ti_gate.conf | same (23) | same (1805) | same | identical |

`hal_ti` was present at `C:\ncs\v3.4.0\modules\hal\ti`, so the TI rows were built,
not skipped. `ti_patch` and `ti_coex` were not compared; the plan names only
`ti_launchxl` and `ti_launchxl_gate`. The head builds ran before the last two
commits; those add only a comment in the MPSL gate (not compiled into any of
these) and documentation, so the result still holds. Comparing the two trees
also confirms the head side really held A13-1 and the base side did not.

### The two-arm soak (the plan's measurement)

Rig: L15 DK DUT `apps/dongle_thread` with `thread.conf`, attached as a child of
the 5340 DK OpenThread leader (`ot child table` confirmed it before and after
each arm). Host opened two channels over UART: channel 0 tracked a power master
(#2002, nRF52840 Dongle running `ant_sim`) and channel 1 searched for an absent
device with an infinite timeout, so the sweep stayed busy. The Garmin USB-m
tracked the same master in the same window as a same-instant reference.

Instrument (soak tree only, not on the branch): a Kconfig-gated 30 s `LOG_INF`
from the gate work queue with the counters, so no per-window logging.
`CONTROL` flips only `CLOCK_LOCK` back to `irq_lock`; the rest is the same
sources. `phase-0a-soak-instrument.patch` is the whole soak tree against
f282c89: A13-1 plus the instrument. Image SHA-256 of `zephyr.hex`: control
`3B2F2E31...584489`, fixed `B868CF10...9256B8`.

| Run | Arm | Length (uptime) | `clkback` | start_late | gate-failed | abort with failed pending | blocked | cancelled | hi_blocked | hi_granted | ext ok/fail | overstay | Faults | DUT ch0 pkts / loss % | Garmin ref pkts / loss % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dry | control | 161 s | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 550 | 0/1717 | 0 | 0 | rig check | rig check |
| ctrl1 | control | 1031 s, then the probe dropped | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 7543 | 0/21891 | 0 | 0 | 3839 / 0.13 | 7195 / 0.21 (full 1800 s) |
| **ctrl** | **control** | **1871 s** | **0** | 0 | 0 | 0 | **0** | 0 | 0 | 14271 | 0/41408 | 0 | 0 | 7197 / 0.14 | 7167 / 0.59 |
| fixed1 | fixed | 281 s, then assert | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1536 | 0/4475 | 0 | **MPSL assert** | 841 / 0.00 (209 s) | 7185 / 0.34 |
| **fixed** | **fixed** | **1871 s** | **0** | 0 | 0 | 0 | **0** | 0 | 0 | 14259 | 0/41386 | 0 | 0 | 7187 / 0.26 | 7175 / 0.48 |

Counters are the last line each run printed. Loss is packets-against-expected,
because `ant_verify` could not derive an exact figure on most windows of this
`ant_sim` master.

**Reading, against the plan's rule:** `clkback` is **zero on both arms** over 30
minutes, so the race did not fire and this soak does not discriminate between
them. The merge cannot rest on this measurement. No race on the control arm also
means its loss figures are comparable: 0.14 % against 0.26 % for the fixed arm
are within each other's same-instant reference spread and the room floor, not a
regression. The stress runs below supply the discriminating evidence.

BLOCKED was 0 on every arm, and the arbiter also refused 41k extensions (`ext_fail`)
per 30-minute arm with `ext_ok=0`. That extension refusal under an attached MED is
ADR 0018's subject and is not attributed to these items.

**Stress runs (added, not in the plan's protocol):** the soak's zero says the race
is rare, not absent. A soak-only lowest-priority thread calling
`radiant_radio_now()` in a loop raises the odds. Two 10-minute arms, same rig, no
Garmin:

| Run | Lock | `clkback` | hi_granted | DUT ch0 pkts in 600 s | Faults |
|---|---|---|---|---|---|
| hammer_ctrl | `irq_lock` | **5931** (climbing) | 149 | **3** | none |
| hammer_fixed | `CLOCK_LOCK` | **0** | 4661 | 2216 | none |

On the control arm the tracked channel is destroyed, as the reasoning predicts: a
backward fold is a permanent +71.6 min clock step, so every later slot is wrong.
With `CLOCK_LOCK` the same thread produces no backward fold. The hammer starves
threads, so the 2216 packets are not a loss figure. **So A13-1 rests on reasoning
plus a race induced under stress, not on a race observed in representative use.**

### The MPSL assert

Fixed arm, first run, at about 298 s of uptime:
`*** MPSL ASSERT 69:108 placed=6575 granted=6575 blocked=0 ext=0/4894 deadline=0`,
last signal EXTEND_FAILED, trace ending `1:1 3:2` (TIMER0 returned EXTEND, then
EXTEND_FAILED returned END). The board halted (`CONFIG_RESET_ON_FATAL_ERROR=n`).

| Exposure, failed extensions | `irq_lock` | `CLOCK_LOCK` |
|---|---|---|
| Representative soaks | 65,016 with no assert | 46,280 with this one assert |
| Stress runs | 1,665 with no assert | 13,532 with no assert |

A single event. It did not repeat in 30 more minutes on the same image, and the
stress arm, which makes far more PRIMASK sections, also ran clean. With one event
neither arm can be implicated or cleared. `CLOCK_LOCK` holds PRIMASK across one
timer capture and one add; the patch's "under 1 us" was not measured here. MPSL
is closed source, so file 69 line 108 could not be read. The assert path is the failed-extension end, which
happens about 41k times per arm. It is recorded in TODO. Watch for it in any
longer MPSL soak.

### Bench notes

- The L15 probe dropped off USB twice in this session: at about 12:00:42 it
  re-enumerated in 13 s, and it was off between about 13:54 and 14:10, recovering
  with no replug. The first fell during the ctrl1 run. Both VCOMs died together
  and `LastArrivalDate` moved. `ctrl1` is kept as extra exposure, not as the
  30-minute control arm, which is why the control arm ran twice.
- The DUT's counters reached the host only through a 30 s `LOG_INF` line, so the
  fixed-arm figures cover the run up to the assert halt, not 30 minutes; the
  second fixed run did cover 30 minutes.

## The two refusal-path defects

The plan's rule is "fix if a ztest shows them reachable". No ztest can:
`radiant_radio_nrf.c` is not compiled by any suite. `radiant/tests/gate` fakes
`radiant_nrf_gate_on_grant()` and `radiant_nrf_gate_finish_failed()` above the
gate (see the header of `test_gate.c`), and the real on-grant programming path,
the abort and `gate_failed` all live in that backend. A bench-only instrument
answered the reachability question instead, and neither path fired.

- **ETIME on a grant.** `program_rx()` fails only if `SIGNAL_START` is more than
  (`HEAD_MARGIN_US` 250 us minus air lead minus programming time) late. `gatefail`
  counts on-grant programming failures: **0** in all runs, 87 min of uptime, about
  38k tracked grants. `start_late` (the compare-written-late path): **0**. Left
  unfixed. The wrong comment at `gate_mpsl:1160` is corrected: a programming
  failure does not set `release_wanted`, so the grant is held to TIMER0 and
  FAILED comes after it.
- **Abort with `gate_failed` pending.** Only reachable after an on-grant ETIME,
  which never happened. `abort_fail_pending` counted **0**. Left unfixed.

Both stay in TODO's "Left open" list with the reason.

## Ztests (nRF5340 DK, branch code, NCS v3.4.0, one boot each)

| Suite | Result |
|---|---|
| `radiant\tests` | 965 passed, 0 failed, 49 suites |
| `radiant\tests\api` | 65 passed, 0 failed |
| `radiant\tests\gate` | 32 passed, 0 failed |

Run with this worktree's `run_ztest_hw.ps1`. They cover C5-1 and the other gate
logic. A13-1's change sits in the nRF backend, which these suites do not compile.

## 5340 DK image history

- 11:17 to 11:18: ztest gate suite (C5-1 red, then green).
- 11:39 to about 14:11: OpenThread leader for the dry run and every soak arm.
- About 14:12 to 14:17: ztest main, api, gate.
- 14:17 onward: OpenThread leader again, for the stress runs. Still on it.

End state of the rest of the bench: the L15 DK holds the `hammer_fixed` soak
image, so reflash it before using it. The nRF52840 Dongle was not reflashed and
was reset after the last run, so its master is off the air (`ant_probe` reports
`RADIANT0.01B00`). The Pro Micro was not touched.

## Not done

- BLE coexistence: left to the Phase 1 scorecards, as the plan says.
- `ti_patch`/`ti_coex`: not compared (not in the plan's list).
- `program_tx()` compare ordering: out of scope.
- No representative-use race was reproduced, on either arm.
