# Phase 0b: shared foundation

Branch `wip/adr18-foundation`, based on master 42475c6 (Phase 0a merged).
Measured 2026-11-09. Nothing is merged, pushed or tagged. Raw logs are outside
the repository: see [phase-0b-raw-logs.md](phase-0b-raw-logs.md).

## Verdicts

| Step | Verdict | One line |
|---|---|---|
| 1. Harness into `tools/` | **Done** | `tools/ant_disc_phase.py` + 12 host tests; `CONFIG_RADIANT_SEARCH_PHASE_DIAG` ported by hand; the harness fails a run whose DUT prints neither diagnostic line |
| 2. Coverage counter | **Done** | `CONFIG_RADIANT_SEARCH_COVERAGE`, 0xF6 `[30..31]`, 30/32-byte body, version 1, clamp without the flag; host and firmware tests |
| 3. Schedule-shape ztests | **Done** | deaf fraction at 1/2/4/8 tracked channels, the three-lead ceiling, the direct-lead control, the R1 characterization |
| 4. `check_zero_cost.ps1` | **Done** | all 8 rows (6 nRF + 2 TI) identical against 42475c6: sections, symbols and `zephyr.bin`; a missing hal_ti fails the run |
| 5. Size baselines | **Partly done; blocked on master** | 42475c6 does not link the L15 bridge arm nor either Matter arm (duplicate sources from 1d0d821). Bridge measured with a two-line local patch; both Matter baselines not measured |
| 6. Baseline A leg | **STOPPED: differs** | **13/17** found against the expected 12/17. Every failure sits in a predicted band; the extra success is a band-edge trial found at 40.6 s. Mask `0x003fcffc` reproduced. Controls and R1 not run, per the stop rule |

**TI:** the changes compile out of every TI build and both TI rows are compared
by binary in step 4. TI is build-only on this bench: nothing here is verified on
air on TI.

## Step 1: the harness

`tools/ant_disc_phase.py` is `C:\adr18_harness\disc_phase.py` with the
hard-coded tool path, interpreter path and COM defaults removed. Each master is
`--a-serial`/`--a-port` and `--b-serial`/`--b-port` (exactly one of each, so
master B can be the Pro Micro on USB); `--bridge` and `--log` are required. Added:
`--shuffle-seed` (the delay list is shuffled reproducibly and logged), a
`summary.json` tally, and `--r1` (B placed 5-20 ms after A by retrying the open
up to six times, the DUT tracking both, `--ref-serial` tracking B in the same
window). `tools/test_ant_disc_phase.py` (12 tests) covers argument rules,
TDIAG/TEMPDIAG parsing, the phase arithmetic, the tally, and that a log with
neither diagnostic line raises `SystemExit(2)`. The check runs after the first
trial and again at the end.

`CONFIG_RADIANT_SEARCH_PHASE_DIAG` (default n, `depends on RADIANT_ANTR_API`) is
`tdiag_frames.patch` ported by hand onto the moved
`radiant/src/antr/ant_radio_radiant.c`. In `radiant_search.c` the hook is
`RADIANT_SEARCH_PHASE_DIAG_FRAME(...)`, which is `((void)0)` when the symbol is
off, and its three locals are inside `#if`. The log lines are byte-for-byte the
patch's formats, so the harness parses both. `apps/dongle_thread/phase_diag.conf`
turns it on. On the leg image the DUT printed 98 `TDIAG frame` and 224
`TEMPDIAG chunks` lines.

## Step 2: the coverage counter

- `CONFIG_RADIANT_SEARCH_COVERAGE` (radiant/Kconfig, default n).
  `ANT_DONGLE_HEALTH_COUNTERS` has `select RADIANT_SEARCH_COVERAGE if
  RADIANT_BACKEND_NRF_GATE_MPSL`, so a plain `health.conf` build does not take it.
  `apps/dongle_thread/health.conf` is a one-line fragment for MPSL arms that want
  the reading.
- The mask: 32 bins of `RADIANT_SEARCH_COVERAGE_PERIOD_US` (246277 us, 8070
  counts), set from each chunk's credited `[t_open, end)` in
  `radiant_search_on_done()`. Credit is unchanged. A bin counts if listening
  touched it. A gap shorter than one bin (7.7 ms) is invisible, but a band of two
  bins or more always leaves an unmarked bin, which is the ADR 0018 band size.
  The fold uses a fixed period, not the tracked channel's. A set spans about 1.4
  tracked periods, so another ANT+ period moves the band by less than a bin.
- A9-3: a chunk reported more than `RADIANT_SCHED_ARMED_STALE_US` past its own
  close is still credited, but marks nothing.
- Retune: the mask is reset in `build_set()`, and a retune is always followed by
  `select_next_set()`, which calls it.
- `sets_advanced_uncovered` is counted when a completed set is closed: in
  `radiant_search_window()`, just before `retune_at_boundary()`.
- Firmware: `ANT_HEALTH_PAYLOAD_LEN` stays 30 as the minimum, and
  `ANT_HEALTH_BODY_LEN` is 32 on a coverage build and 30 otherwise. The `cap`
  check, the memset and the return value use `ANT_HEALTH_BODY_LEN`.
  `ant_health_fill()` reads the counter through
  `radiant_search_sets_uncovered_get()`, which is defined beside the sweep
  instance in `ant_radio_radiant.c`. It clamps at 65535 without touching the
  `[1]` flag.
- Version stays 1. Reworded in `protocol/ant_wire.yaml` (VERSIONING, in both
  0xF4 and 0xF6), the `[0]` comment in `apps/common/ant_health.c`, the comment
  above `FIELDS` in `tools/ant_health.py`, and the `ANT_HEALTH_PAYLOAD_LEN`
  comment in `ant_health.h`. 0xF6 `payload_len` is now `"30..32"`. The YAML
  documents `[30..31]` as optional and states that it clamps without the flag.
  `ant_wire.py`, `radiant_wire.h` and `docs/ant-serial-protocol.md` were
  regenerated; `ant_wire.h` came out unchanged.
- `tools/ant_health.py`: `PAYLOAD_LEN` is still 30. The field is in a separate
  `OPTIONAL_FIELDS` table: its key appears only when the body holds it, and the
  text output prints `-  not in this build` instead of 0.

**0xF6 tests.**

| Body | Expected | Test |
|---|---|---|
| 29 bytes | refused (ValueError) | `test_a_29_byte_body_is_refused`, `test_a_29_byte_body_is_refused_when_coverage_exists` |
| 30 bytes, v1 | decodes, key absent | `test_a_30_byte_v1_body_decodes_every_field`, `test_a_30_byte_v1_body_has_no_coverage_key` |
| 31 bytes | key absent | `test_a_31_byte_body_leaves_the_coverage_key_absent` |
| 32 bytes, v1 | key decoded (0x1234) | `test_a_32_byte_v1_body_decodes_coverage` |
| firmware, coverage build | 32 bytes, `[0]=1`, field LE | `ant_health_coverage.test_a_coverage_build_answers_32_bytes` |
| firmware, counter 70000 | `[30..31]=0xFFFF`, `[1]` bit 0 clear | `ant_health_coverage.test_saturating_the_coverage_field_does_not_set_the_flag` |
| firmware, cap 31 | refused (0), cap 32 accepted | `ant_health_coverage.test_a_buffer_too_small_for_this_build_is_refused` |

The firmware tests build `apps/common/ant_health.c` into
`radiant/tests/src/test_ant_health.c`, with the health symbol forced on and the
getter stubbed. In the five coverage tests in `test_search.c`: a whole dwell
covers every phase and is not counted; phase-locked chunks leave the
after-slot band unmarked and the set is counted, still advancing on time; a
reclaimed chunk marks nothing and keeps its credit; a chunk across the phase
wrap marks bins 31 and 0 only; a displaced chunk marks only what it heard. The
test in `test_search_rf.c`: a retune to RF 66 resets the mask, so two
half-covered sets on two frequencies count twice, not once.

**Conformance.** `ant_conformance.py --list` is identical from 42475c6 and
from the branch (286 lines, so `payload_bounds()` did not reshape any case),
and the branch's tools replay `conformance-nrf-radiant.antser` (1273 records).
The firmware that transcript was recorded on is unchanged where it matters:
the plain L15 `apps/dongle` row is binary-identical in step 4, and 0xF6 is
request-only and absent from that build anyway. So the nRF Tier 1 transcript
stands as recorded. **The cc26xx transcript cannot be checked:** it already
trails Release 2 and re-recording it needs a CC2652, which this bench does not
have.

**Not done on air:** reading a 32-byte reply from a coverage build on the DUT,
and a 30-byte reply from a plain `health.conf` build. Those are in the plan's
end-to-end verification, not in step 2. The bench stopped at step 6 before
there was a slot for them.

## Step 3: schedule-shape ztests

The helpers are in `radiant/tests/src/test_sched.c`. Leads are the MPSL
build's: `min_arm_lead_us` 24418 (PLACE_LEAD 24000 + HEAD_MARGIN 250 + ARM
168), in-grant 200, `max_window_us` 94200. The tracked period is 246277 us,
the tracked window 400 us, and each run is 9 periods with the first 3
discarded. N tracked channels are spaced evenly; a standing continuous scan on
channel 31 asks for 260 ms per chunk. Scan arms are folded modulo the period
in 100 us bins. Each helper reports the never-listened time, the mean listen
per period, the widest circular gap, whether that gap moves period to period,
and the number of aborted scan chunks (the fold assumes none).

Measured on the 5340 DK (`ztest_unit_5340_console.txt`) and run on native_sim
in twister:

| Case | Deaf (never listened) | Listened per period | Widest gap | Moves |
|---|---|---|---|---|
| MPSL lead, 1 tracked | **30.0 %** (73.9 ms) | 172.4 ms | 49.5 ms | no |
| MPSL lead, 2 tracked | 40.2 % (99.1 ms) | 147.2 ms | 49.6 ms | no |
| MPSL lead, 4 tracked | 80.3 % (197.9 ms) | 48.4 ms | 49.5 ms | no |
| MPSL lead, 8 tracked | **100 %** | 0 | the whole period | no |
| direct lead (200 us), 1 tracked | **0.4 %** (1.1 ms) | 245.2 ms | 1.1 ms | no |

- `test_under_arbitration_search_deafness_is_at_most_three_leads`: deaf ≤
  3 × 24418 + window + 3 × 1.5 ms, deaf ≥ 2 leads, locked, no aborted chunk.
  The first row above matches ADR 0018's measured ≈30 %.
- `test_with_a_direct_lead_search_deafness_is_under_one_percent`: the 0.4 %
  row.
- `test_deaf_fraction_by_tracked_channel_count`: the ceiling is
  min((2N+1) × (lead + 1.5 ms) + N × window, period). At N = 4, 19.7 % listened
  matches the ADR's derived "about 20 % listened". At N = 8 with even spacing
  (30.8 ms between slots, less than two leads) the scan never runs at all.
  Even spacing is the worst case, not Zwift's random phases.
- `test_r1_two_tracked_channels_inside_the_lead_today`: two tracked channels,
  12 periods:

| Offset | Earlier slot | Later slot | Arms |
|---|---|---|---|
| 1 ms | 12/12 | 12/12 (merged) | 13 |
| 5 ms | 12/12 | **0/12, MISSED every period** | 13 |
| 12 ms | 12/12 | **0/12** | 13 |
| 24 ms | 12/12 | **0/12** | 13 |

  The test pins today's behaviour (`later_served` false at 5, 12 and 24 ms).
  The chosen fix flips those three rows to strict.

Fake-radio change: `FAKE_RADIO_ARM_LOG_MAX` 64 → 200 and
`FAKE_RADIO_EVENT_LOG_MAX` 128 → 400, because the multi-period shape runs
overflowed the arm log (`a record log is full`). No other test changed.

**Test runs.** `radiant\tests`, `radiant\tests\api` and `radiant\tests\gate` all
PASS on the 5340 DK, one boot each. native_sim twister on the Bluefin server:
13 scenarios, **4220/4220**, `check_test_counts` OK. Floors were raised in the
same commit: +13 for each of `radiant.unit`, `.sec_modes`, `.no_pairing` and
`.ack_faults` (978/978/926/985), and `min_testcases` 4168 → 4220. Twister's
observed counts equal the new floors exactly. `radiant.gate` stays at 32.

## Step 4: `scripts/check_zero_cost.ps1`

It mirrors release.yml's zero-cost job. Each row is built from HEAD and from a
base ref (a detached worktree at `build\zc_base`, refused if not clean or at
the wrong commit). It compares allocated sections (`size -A`, non-zero
address) and the `nm --defined-only` set, and also reports a `zephyr.bin`
hash. Every `.config` is read back: the expected HAL is set, there is no null
backend, the MPSL gate is off on the nRF rows, and neither
`RADIANT_SEARCH_COVERAGE` nor `RADIANT_SEARCH_PHASE_DIAG` is set. The rows are
feather, nrf52840dongle, promicro, net5340 (cpunet), l15 and lm20, all
`apps/dongle`, plus ti and ti_gate (`apps/dongle_ti`, `ti_gate.conf`). **A
missing hal_ti fails the run**: at `-NcsVersion v3.2.4`, which has no hal_ti, it
printed `FAILED: hal_ti module is not at ...` and exited 1
(`zero_cost_no_hal_ti.txt`). Any uncaught error exits 1 through a `trap`.

Result against 42475c6 (`zero_cost.txt`, UTF-16), every row with an explicit
backend and its `.config` read back:

| Row | Board | Sections | Defined symbols | `zephyr.bin` |
|---|---|---|---|---|
| feather | adafruit_feather_nrf52840/nrf52840/uf2 | same (30) | same (2167) | identical |
| dongle | nrf52840dongle/nrf52840 | same (30) | same (2162) | identical |
| promicro | promicro_nrf52840/nrf52840/uf2 | same (30) | same (2164) | identical |
| net5340 | nrf5340dk/nrf5340/cpunet | same (27) | same (1952) | identical |
| l15 | nrf54l15dk/nrf54l15/cpuapp | same (26) | same (2106) | identical |
| lm20 | nrf54lm20dk/nrf54lm20a/cpuapp | same (32) | same (2513) | identical |
| ti | cc26x2r1_launchxl, apps/dongle_ti | same (23) | same (1796) | identical |
| ti_gate | cc26x2r1_launchxl, ti_gate.conf | same (23) | same (1805) | identical |

The head side was built from the worktree at `0771cc3` plus the staged script
fix; every later commit touches only scripts and documentation, so this holds
for the branch tip. The counts match Phase 0a's table row for row. TI:
build-only, binary-identical, not verified on air.

## Step 5: size baselines

**42475c6 does not link the bridge or the Matter arm of `apps/dongle_thread`.**
1d0d821 ("user zephyr module usability improvements", merged into master with
42475c6) added `src/bridge/*` and `src/profiles/*` to the radiant module under
`CONFIG_RADIANT_BRIDGE` / `CONFIG_RADIANT_PROFILES`. `apps/dongle_thread/CMakeLists.txt`
still adds the same files to the app, so the link fails with multiple
definitions:

- `thread.conf;bridge.conf` (L15): `radiant_bridge.c` and
  `radiant_hr_adapter.c` are compiled twice (`radiant_bridge_binding_changed`,
  `radiant_hr_adapter_init`, ...). Before 1d0d821 the module did not list them
  (checked at 7df6ada and at 1d0d821^).
- `thread.conf;matter.conf` (L15): as above, plus `profile_common.c`,
  `profile_env.c`, `profile_fec.c`, `profile_power_decode.c` and
  `radiant_power_adapter.c`.

master moved to 377f926 ("usability follow up") during this phase. That commit
touches only ci.yml and two READMEs and leaves both duplicates in place.
release.yml's MQTT-bridge and Matter rows would fail the same way on the next
tag; push CI does not build them. **This is a master defect, not a 0b change,
and it is not fixed on this branch.** Fixing it means deciding which build
owns these files. That is a user decision and outside 0b's scope. Every
Phase 1 candidate that builds a bridge or Matter image needs it fixed first.

What was measured, with the nm/.config method:

| Image | Tree | Result | FLASH | RAM | Notes |
|---|---|---|---|---|---|
| L15 `thread.conf;bridge.conf` | 42475c6 as is | **link fails** | - | - | `sizes_base.txt` |
| L15 `thread.conf;bridge.conf` | 42475c6 **with `radiant_bridge.c` and `radiant_hr_adapter.c` removed from the app's list** (scratch worktree, never committed) | links | **321 736 B** / 1524 KB (20.62 %) | **197 928 B** / 256 KB (75.50 %) | `.config`: NRF=y, GATE_MPSL=y; nm: `radiant_radio_init` present, 5 `*_sink` symbols |
| L15 `thread.conf;matter.conf` | same patched tree | **link fails** (the profile and power-adapter duplicates) | - | - | not measured |
| nRF52840 DK Matter | same patched tree | **link fails** (same profile duplicates, 19 errors) | - | - | not measured |

The bridge figure is "42475c6 plus a two-line patch", not master. A candidate
quoting a delta against it must build its own arm with the same patch, or with
whatever fix lands, and re-measure the base.

## Step 6: the baseline A leg

**Rig.** All of the following were confirmed attached and identified by serial
and MI_xx (`Get-PnpDevice`): DUT = nRF54L15 DK `1057737173`, ANT VCOM COM4
(MI_00), log COM3 (MI_02, DTR asserted). Master A = nRF52840 Dongle
`3D55F77818BE772A`, #2002. Master B = Pro Micro `EE6986854FD72803`, #5005.
Garmin `006` attached. The 5340 DK (`1050006310`) was the ztest board.

- **Master A (Dongle), verified first, not reflashed:** `ant_probe` PASS:
  STARTUP_COMMAND_RESET, 32 channels / 3 networks, version `RADIANT0.01B00`
  (`dongle_probe.txt`). The probe cannot say which commit the image is from;
  the version string is the stack's, not a build id.
- **Master B (Pro Micro), flashed once at the start of step 6:** HEAD `ccfa2b4`,
  `apps/dongle` `promicro_nrf52840/nrf52840/uf2`, `.config` USB_LEGACY +
  `ANT_DONGLE_UF2_REBOOT_REQUEST=y` + nRF HAL, UF2 target 0x26000 family
  0xADA52840, SHA-256 `719E0C21...633569`. The sequence was `ant_probe
  --reboot-to-bootloader`, then `NICENANO` mounted (bootloader 0.6.0, S140
  6.1.1), then the UF2 was copied. The probe afterwards PASSed with
  `RADIANT0.01B00` (`promicro_flash.txt`). The board was not touched again.
- **Garmin:** probed once (`AJK2.05RAF`). It was not used in a measurement and
  `ant_features.py` was not run, so it needed no reset.
- **DUT:** the L15 held a Phase 0a soak image and was reflashed first. The new
  image is `apps/dongle_thread` `gate.conf;phase_diag.conf` from `0771cc3`;
  `.config` has `RADIANT_BACKEND_NRF=y`, `GATE_MPSL=y` and
  `RADIANT_SEARCH_PHASE_DIAG=y`. The worktree was clean apart from a staged
  PowerShell script that is not a firmware input. It was flashed through
  `flash_sim_jlink.ps1`. `ant_probe --port COM4` PASSed and the log printed
  `TEMPDIAG chunks` lines.

**Run.** `ant_disc_phase.py --delays 0,15,...,240 --shuffle-seed 1 --seconds
60`. ch0 was pinned to #2002 and ch1 was a wildcard HR search. Zero-cost and
size builds were running on the host at the same time. Host load can only
shift the phase at which B opens, and the phase is measured afterwards.

| B's phase after A (ms, host) | DUT phase of B's frame (us) | ch1 | first packet |
|---|---|---|---|
| 4.8 | never heard | **not found** | - |
| 20.1 | never heard | **not found** | - |
| 34.8 | 34674 | found | 8.8 s |
| 50.0 | 49962 | found | 8.9 s |
| 63.5 | 63416 | found | 8.8 s |
| 71.2 | 71140 | found | 8.8 s |
| 86.3 | 86157 | found | 8.9 s |
| 101.2 | 101150 | found | 9.1 s |
| 116.4 | 116295 | found | 8.9 s |
| 131.3 | never heard | **not found** | - |
| 146.2 | 146088 | found | 9.0 s |
| 161.8 | 161729 | found | 9.0 s |
| 175.9 | 175842 | found | 9.0 s |
| 191.4 | 191324 | found | 9.3 s |
| 205.9 | 205823 | found | 9.2 s |
| 221.7 | 221369 | found **late** | **40.6 s** (79 packets) |
| 235.8 | never heard | **not found** | - |

**Result: 13/17 found. The plan expected 12/17. Per the stop rule this is
reported, not adopted, and the leg was not re-run.**

- All four failures are inside ADR 0018's predicted bands: after the slot
  (0-25 ms: 4.8, 20.1), mid-period (≈119-144 ms: 131.3) and before the slot
  (≈221-246 ms: 235.8). The DUT logged zero #5005 frames in each. "Heard but
  not acquired" is refuted again.
- The difference is the 221.7 ms trial, on the leading edge of the
  before-slot band. B was found, but only after 40.6 s; the clean successes
  took 8.8-9.3 s. B drifted 0.4 ms earlier over the trial, away from the band.
  The ADR's own nearest points were a success at 211.2 ms and a failure at
  225.4 ms, so 221.7 ms is within one sweep of the band's edge. The likely
  reading is that the band's edge sits near 221-222 ms and this trial straddled
  it. That is inference; the run cannot prove it.
- With 17 offsets 15 ms apart and three bands of about 25 ms each, the count
  depends on where the grid falls against the edges. 4/17 here and 5/17 in the
  ADR are both consistent with ≈30 % deaf. A pass/fail rule on the raw count
  will flake on band-edge trials.
- **Mask:** `0x003fcffc` in 71 of the 90 non-zero `TEMPDIAG` reports. The
  other 19 are 9 distinct masks in reports that straddle trial boundaries (the
  ADR saw the same). The 115 zero reports come after ch1 had found B, when no
  search is running.
- **Calibration:** with both masters on USB, the DUT's own phase equals the
  host phase minus about 0.1 ms across all 13 heard trials. The ADR measured
  −25.2 ms with B on the 5340 net core over UART. The difference is B's
  transport latency, which no longer differs between A and B.
- ch0 (#2002) received 240-244 packets per 60 s trial in every trial.

**Controls (5/5 expected) and the R1 reading were not run.** The task says to
stop at step 6 if the baseline differs, so the bench stopped there and the
lock was released. `--r1` exists and is host-tested, but it has not run on
air.

## Deviations and things not done

- Step 6 stopped at the leg. No controls, no R1 reading, no on-air 0xF6 read.
- Step 5: both Matter baselines are not measured, and the bridge baseline is on
  a locally patched base (see above).
- `RADIANT_SEARCH_COVERAGE_PERIOD_US` is a fixed 246277 us, not the tracked
  channel's period. That is a choice, explained in step 2.
- The shape helper's tracked window is 400 us (the file's `TEST_WINDOW_US`). A
  first draft used 2 ms, which made the direct-lead control read 1.0 %. The
  window was changed because 2 ms was an arbitrary width, not to pass the test.
  Both runs are in the logs (`ztest_dev2/3`).
- The R1 table in the test first assumed all four offsets would be served. That
  was wrong; the test now pins what was measured.
- `check_zero_cost.ps1` had two bugs on its first runs: indexing single-line
  git output, and a missing row directory. Both died before building anything
  and both are fixed (`ccfa2b4`). It has not been negative-tested against a
  real code difference, only against a missing hal_ti.
- Not explained: why the 221.7 ms trial took 40.6 s rather than never or about
  9 s (above).
