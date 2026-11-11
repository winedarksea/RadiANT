# Phase 3 Stage B raw logs

All files are in `C:\adr18_logs\phase-3b\`, outside the repository, unless a path says
otherwise. Measured 2026-11-10/11 on branch `wip/adr18-impl`. Report:
[phase-3b.md](phase-3b.md). Scripts named here are in the session scratchpad, and
copies of the patching ones are in the log directory.

## Images

Each `img_<tag>.hex` has `img_<tag>_config.txt` (`.config` read-back, commit,
`git status`, SHA-256) and `build_<tag>.log`.

| Tag | Tree, confs |
|---|---|
| `ctl_gate`, `ctl_ble`, `ctl_thread`, `ctl_bridge`, `ctl_bridge5` | `C:\wt_adr18_ctl` at c4e1ef8 plus the uncommitted control patch (`ctl_patch.py`: a 5 s `GSTAT` counter line and Phase 1's BLE load files); `_bridge5` also has `bridge_tt.py` |
| `head_ble`, `head_bridge`, `head_matter` | branch at 8b357d7 (built from 39ee229/ecc461a trees, whose differences are documents and tools only) |
| `head_bridge5` | `C:\wt_adr18_impl\build\ns` at 8b357d7 plus `bridge_tt.py` |
| `C:\adr18_logs\phase-3\img_a2_gate.hex`, `img_a2_thread.hex` | Stage A's final images, 8b357d7 |
| `tsw_gate`, `tsw_ble`, `tsw_thread` | 0d385b6 with `tail_sweep.conf` |
| `fin_gate`, `fin_ble`, `fin_thread` | ff88a6a (tail 10 ms, re-place) |
| `plain_health` | `apps/dongle`, `health.conf`, L15, at 191285e |

Confs: gate `gate.conf;phase_diag.conf;[d2_stats.conf;]health.conf`; BLE
`coex.conf;ble_load.conf;phase_diag.conf;[d2_stats.conf;]health.conf`; Thread
`thread.conf;phase_diag.conf;[d2_stats.conf;]health.conf`; bridge
`thread.conf;bridge.conf;bridge_bench.conf;[d2_stats.conf;]health.conf`; Matter
`thread.conf;matter.conf;d2_stats.conf;health.conf` with `matter_sysbuild.conf` and
`CONFIG_RADIANT_LOG_LEVEL_INF=y`.

## Readings

| Path | What |
|---|---|
| `void_ambient8265_*` | the voided first A/B/A attempt (ch1 bound strap #8265) |
| `aba_A`, `aba_Ac`, `aba_B`, `aba_Bc`, `aba_A2` | item 1. `disc/results.jsonl`, `dut_log.txt`, `tNN.json`; `aba_summary.txt` (`aba_sum.py`) |
| `r1_ctl12`, `r1_ctl12s`, `r1_head{5,12,12s,20}` | item 2: `r1w_summary.json`, `r1w_dut.antser`, `dut_log.txt`, `run.txt` |
| `gate_{ctl,head}_{1,2}`, `gate_score.txt` | item 3 (`ab_slots.ps1`, `score_slots.py`) |
| `thread_{ctl,head}_{1,2}`, `thread_summary.txt` | items 4 and 7: `rule1/`, `loadping/`, `ping_idle.txt`, `ping_load.txt`, `disc/`, `ot_child_table.txt` |
| `ble_{ctl,head}_{1..6}`, `ble_score.txt` | item 5 |
| `bind_{ctl,head}_{1,2}` (+`_console.txt`) | item 6 (`bind3.py`). `bridge_head1`, `bridge_ctl1`, `bridge_head2`: the first harness (`evict3.py`) |
| `matter1` | item 8 |
| `eight_*` (first driver, two masters on air; holds the assert in `eight_head_1/dut_log.txt`), `eight2_*`, `multi_master_dry.txt` | item 9 |
| `tsw_gate`, `tsw_ble`, `tsw_thread` | the tail sweep; `tail_sum.txt` from `tail_sum.py` |
| `fin_B`, `fin_Bc`, `bletail_*`, `gatetail_*`, `r1t_*`, `threadtail_*` | the tail at the final image |
| `ack_fin/` | 60-send pairs and the 300-send acceptance |
| `f6_coverage*.txt`, `f6_plain*.txt` | 0xF6 reads |
| `zero_cost.txt`, `size_*.txt` (+`.log`, `.cmd`) | close-out |
| `ztest_tail_red.txt`, `ztest_{gate,main,api}_final.txt`, `twister_pre_floors.txt`, `twister_final.txt` | tests |
| `chain*.out`, `b4q*.out` | the bench and build drivers' consoles |
