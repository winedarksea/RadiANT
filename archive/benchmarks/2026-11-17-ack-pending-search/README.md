# 2026-11-17: held slave command vs search (TODO item 3), bench

`tools/fec_ack_sweep.py --silence-ms N`: the FE-C master goes off air for N ms
(plus 0-249 ms jitter, at a random start) while the host keeps Zwift's pace: a
command every 1.01 s and a second 0.75 s after each. `judge_silence()` passes a
cycle when:
- every command pending across GO_TO_SEARCH fails at it;
- nothing is refused while the master is away;
- every command held at reacquire completes, and so does the first one after.

Master: apps/trainer on the nRF54L15 DK (`silence` shell command), or a second
ANT stick (`--master-serial` / `--master-port`, `AntMasterSim`).

| file | DUT, image | master | pass | crossed → failed at GTS | held → completed |
|---|---|---|---|---|---|
| pm_silence, pm_silence_fix2 | Pro Micro, carrier skipped | trainer | 0/5 | 5/5 | 0/5 (2500 ms phase lock) |
| pm_silence_2100, _2300 | Pro Micro, carrier released | trainer | 5/5, 5/5 | 10/10 | 10/10 |
| ti_silence | TI, carrier released | trainer | 10/10 | 9/9 | 18/18 |
| l15mpsl_silence | L15 MPSL, no timing gate | Pro Micro | 0/10 | 0 | 9/20 (10 sends expired, -22.6 ms margin) |
| l15mpsl_silence_fix3 | L15 MPSL, final | Pro Micro | 4/10 | 5/5 | 4/9 |
| ti_silence_final | TI, final | Pro Micro | 9/10 (1 air loss) | 7/7 | 8/8 |
| pm_silence_final | Pro Micro, final | TI | 10/10 | 6/6 | 6/6 |

The `held → completed` counts are re-judged with the final `judge_silence()`.
The `*_baseline.json` files are 20-command no-fault runs against the trainer:
the Pro Micro got 19/20 (one inbound lost on air), the TI 20/20.

The final images were built from the commit that adds this file, into
`build/ackps_{promicro,ti_diag,l15_mpsl}`. Diag before and after the final
MPSL run: `l15mpsl_diag_{before,after}.json`.
