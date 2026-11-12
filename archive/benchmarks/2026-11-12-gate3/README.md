# Gate 3 for the nRF short match (ADR 0022), and on-air frequency checks

2026-11-12, branch `fix/discovery-open-items` at `04613b0` (merged to master).

## Images

All three built together from that tree with `health.conf;diag_counters.conf`
(0xF7 page 7 present):

| board | serial | image |
|---|---|---|
| Pro Micro (nice!nano, LFXO) | `EE6986854FD72803` | A = `CONFIG_RADIANT_SEARCH_SHORT_MATCH=n`, B = `=y`, switched by USB reboot to the bootloader |
| nRF52840 Dongle (PCA10059) | `3D55F77818BE772A` | A in every window. DFU-flashed **once** (`build/cu_g3_dgA.zip`) |
| Garmin USB-m | `006` | reference |

## Method

Six 300 s windows, Pro Micro A,B,A,B,A,B, all three sticks in every window:

    ant_scan_rate.py --serial EE6986854FD72803 --serial 3D55F77818BE772A --serial 006 \
        --seconds 300 --reopen 4.5 --track 52233:0x0B --track 52233:0x11

`ant_diag.py --pages 0-7` on both nRF boards before and after each window.
`analyse.py` computes per window D = Pro Micro loss - dongle loss (both
trainer channels pooled). The dongle is a same-SoC, same-image control, so the
room's drift cancels inside a window (memory `bench-gates-at-current-room`:
no quiet room, gate on same-window controls).

Gate (from the plan): mean D(B) - mean D(A) <= max(1 pp, spread of D over A),
and page-7 counters 3-7 on the Pro Micro no higher in B than in A.

## Result

| window | PM % | Dongle % | Garmin % | D pp | PM sightings/s | cut at close | END near close | open late | missed behind scan | slots after sibling | of those missed |
|---|---|---|---|---|---|---|---|---|---|---|---|
| w1 A | 20.43 | 29.94 | 30.54 | -9.52 | 0.49 | 0 | 2 | 0 | 0 | 2 | 2 |
| w2 B | 18.27 | 30.84 | 39.59 | -12.57 | 4.23 | 6 | 8 | 0 | 0 | 11 | 11 |
| w3 A | 19.23 | 30.91 | 49.60 | -11.68 | 0.48 | 0 | 1 | 0 | 0 | 1 | 1 |
| w4 B | 18.65 | 28.64 | 46.01 | -9.99 | 4.27 | 8 | 13 | 0 | 0 | 12 | 12 |
| w5 A | 18.65 | 29.35 | 46.53 | -10.70 | 0.53 | 0 | 0 | 0 | 0 | 2 | 2 |
| w6 B | 18.59 | 28.62 | 47.37 | -10.03 | 8.05 | 9 | 18 | 0 | 0 | 13 | 13 |

(`srch_cut_abort` was 0 everywhere. Page-7 columns are the Pro Micro's.)

- **Loss: pass.** Mean D is -10.63 pp on A and -10.86 pp on B. B - A =
  **-0.23 pp** against a bound of 2.16 pp (the A spread). The short match
  costs no measurable tracked loss against the same-window control. The
  Garmin's 30-50 % is its scan channel colliding with its tracked ones, as on
  2026-11-11.
- **Discovery:** 4.2-8.1 sightings/s on B against about 0.5/s on A, as in the
  2026-11-11 run.
- **Counters 3-5 (the suspected mechanism): never fired** on either image.
  No tracked window opened late and none expired behind a scan chunk, so the
  search END interrupt chain overrunning the dispatch slack, the mechanism the
  diagnostic was built to catch, did not happen in 30 minutes.
- **Counters 6-7: higher on B, and every counted slot missed** (12 per
  window on B, about 2 on A, 100 % missed on both). By the plan's literal
  rule this is "fail with a mechanism": the miss rate after a sibling is
  above the overall miss rate of about 19 %.

### Why counters 6-7 do not settle it

Counter 6 matches on the device number only. The trainer's power and FE-C
channels share #52233, so a counted "sibling" can be the tracked channel's
OWN frame, heard by the search because it arrived before the tracked window
opened. In that case the tracked slot was going to miss anyway (the window was
placed after the frame), and the search only witnessed it. With one set, B
hears every device number in every dwell, so it witnesses more of these. That
fits the data: 100 % missed on both images (a real hand-off cost would not hit
every case), counters 3-5 at zero, and no loss difference. About 10 extra
counted misses per window is 0.4 % of the roughly 2400 tracked slots, inside
the -0.23 pp loss result.

So the decision follows the plan's "fail" branch until the counter can tell
the two apart: **the nRF default stays n**, and A3 (the two waste fixes) stays
open. The next step is small. Record the device type with the search stamp and
split counter 6 into "same channel ID" (the tracked frame came early) and "true
sibling" (another channel of the device), then rerun three A/B pairs.

The dongle's diag read after windows 5 and 6 failed transiently, right after
the scan closed. A later read succeeded, so its w5-w6 counters are folded into
that one. The dongle is the control, so the loss figures are unaffected.

## Step 7: on-air frequency checks (B6)

Garmin as a keyed power master (`ant_sim.py --serial 006 --profile power
--device-number 4660 --rf-freq 66`), Pro Micro as keyed slaves
(`ant_verify.py`, which installs the ANT+ key, so channels declare their
frequency).

| image | test | first packet | packets | loss |
|---|---|---|---|---|
| B | slave on 66 alone | 0.06 s | 158 / 40 s | 1.9 % |
| B | mixed: #52233 on 57, #4660 on 66 | 57: 0.09 s, 66: 0.30 s | 147 / 173 | 66: 3.9 % |
| A | slave on 66 alone | reported 0.0 s | 157 / 40 s | 3.1 % |
| A | mixed: #52233 on 57, #4660 on 66 | 57: 0.06 s, 66: 0.34 s | 148 / 171 | 66: 4.5 % |

Both images acquire a master on 66, and both acquire on 57 and 66 together.
The bounds are n_sets x dwell per frequency: 0.26 s for B and 8.3 s for A
(twice that when mixed). These channels name their device number, so they are
steered and do not exercise the wildcard round robin. The first mixed attempt
on B was void: the previous `ant_sim.py` still held the Garmin
(`s7_B_sim_mixed.txt` from that run was overwritten by the rerun; the void
run's slave saw 2 packets). `ant_verify` exits 1 on these runs only for its
1.5 % loss limit in this room.

## Files

`w<N>_<img>_scan.{json,txt}` per window; `w<N>_<img>_<serial4>_{before,after}.json`
page dumps; `s7_*` step-7 runs; `analyse.py` reproduces the table.
