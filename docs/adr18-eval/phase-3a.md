# Phase 3 Stage A: building ADR 0021

Measured 2026-11-10 on the nRF54L15 DK (J-Link `1057737173`), branch
`wip/adr18-impl`, NCS v3.4.0. Code at 8b357d7; the commit that carries this
report changes comments and documents only. Design:
[ADR 0021](../decisions/0021-tracked-slots-are-booked-a-period-ahead.md). Raw logs:
[phase-3a-raw-logs.md](phase-3a-raw-logs.md).

Stage A built decisions 1-4 of ADR 0021. The scan tail (decision 5) and the
acceptance bench are Stage B. Nothing here was run on TI: the TI rows are build and
binary comparison only.

The work took two sessions, because the host ran out of memory and rebooted during
the first. The second reviewed the first's code and re-derived its figures from the
raw logs. Three of those figures did not hold and are corrected below.

## Verdicts

| Check | Result at the code head | Log |
|---|---|---|
| Twister, native_sim | **PASS**, 4297 of 4297 in 14 scenarios, counts OK | `twister_a2.txt`, `twister_head.txt` |
| Three suites on the 5340 DK | **PASS**: gate 69, unit 986, API 65 | `ztest_dk_{gate,main,api}.txt` |
| Zero cost against c4e1ef8 | **PASS**, 8 of 8 rows identical, TI included | `zero_cost_a2.txt`, `zero_cost_head.txt` |
| Tracked slots, `gate.conf` | **PASS**: `hi 487/487/0/487` twice, loss 0.0 % against Garmin 0.2 and 0.4 % | `a2_gate{1,2}/` |
| Early release | **PASS**: `t0fix bs` 0, `rel` 487, against the prototype's `bs` 487, `rel` 0 | `a2_gate{1,2}/`, `a2_d2ctrl/` |
| Acked transfers, 60 sends each way | **No regression, not clean**: 3 pairs at the head read 59+60, 59+58, 60+60; the prototype in the same sitting read 58+57 | `a2_ack*/` |
| Tracked slots beside Thread, 30 min | **PASS**: `hi 7199/7199/0/7199` in both arms, loss 0.08 and 0.04 % against Garmin 0.23 and 0.36 % | `soak-d2{skip,noskip}A-*` |
| 69:108, two-session, skip | 0 asserts, 2367 refusals, 30 min | `soak-d2skipA-*` |
| 69:108, two-session, no skip | 0 asserts, 59 495 refusals, 30 min | `soak-d2noskipA-*` |
| Does the skip prevent 69:108? | **Not shown** | see below |
| Lead sweep | Knee between 12 and 16 ms; CONTENDED is 16 ms | `sweep*/sweep_corrected.txt` |
| Sizes | RAM **+448 B** on all three images | `size_*_a2.txt` |

## What was built

| Item | Commits | What |
|---|---|---|
| Early release | d9c2fb2 | The receive `stop_on_first` branch and the transmit END path now owe a grant release before the callback, through the existing handoff |
| Extension skip | f696729 | After 3 refused extensions in a row, the next 64 elastic grants end at their first compare. Sized with the 750 us margin |
| Backend test app | 6742cf8, c082987 | `radiant/tests/backend`: the completion path against register models |
| Port of (d2) | ef5c347 | Two-session pool, bookings, elastic lead, bench counters behind `d2_stats.conf` |
| Reported defects | 8630a51, 6fd671e, 3c4a3a5 | See the next table |
| Lead policy | c07a6b0, b05bcff | OPEN 3 ms or CONTENDED 16 ms, keyed on the extension-refusal run, or 16 refused requests with no grant between them |
| Review fixes | 629d156, b5513c1, 8b357d7 | Three defects the review found |

e7749c2 (the chunk after a tracked slot opens at the window's end) was tried and
reverted by 6128da5. The revert is exact: `radiant_sched.c` at 6128da5 matches
e7749c2's parent.

### The reported defects

| Defect | Real? | Fixed by |
|---|---|---|
| SESSION_IDLE, BLOCKED and CLOSED check-then-act unmasked against the other session's START | Yes | 6fd671e, ztest red then green |
| `in_signal` set for low-priority signals, a second producer on a single-producer ring | Yes | 8630a51, ztest red then green |
| Booking match not atomic | Yes, and half fixed at first: the role was still tested before the mask | 6fd671e, then 8b357d7 |
| A live slot depends on the core arming just after the chunk ends | Yes | 3c4a3a5, a forced pass 2 ms before the start. It saved 523 and 516 slots in the sweeps beside Thread, where short leads kept chunks late, and none in the soaks at the 16 ms lead. `book_late` was 0 throughout |
| Start error on a booking a period old | Logged | `D2START`: -89 to +19 us in the Thread soaks (-679 us at worst in a sweep), -365 to -5 us on `gate.conf`, on this crystal board. No LFRC or `_synth` board was measured |
| Three tracked operations within one lead | Characterised | `test_d2_three_tracked_operations_inside_one_lead`: at least two of three are served each period |

## Review findings

Reviewed c4e1ef8..6128da5. Each fix has a test that failed first
(`ztest_review_red.txt`, 7 failures; `ztest_review_green.txt`).

**Fixed:**

- **A refused booking nobody arms held its session until reset** (629d156). Only the
  matching arm cleared it, so if its channel closed or its slot moved or was missed,
  it stayed. op_forget() could reach the same state from an aborted request. One leak
  wedged a single-session gate, two wedged the pool, and every later acquire was
  refused. Not seen on the bench.
- **A tracked arm could ride a forgotten scan request** (b5513c1). An aborted chunk's
  request is kept as a booking with the chunk's whole window as its air, while MPSL
  knows only a short elastic first bite. A tracked window inside that air would have
  depended on an extension, against ADR 0013 rule 1. A chunk inside a tracked booking
  could also take it. Hits never exceeded bookings placed in any log, so this too was
  not seen on the bench.
- **Two unmasked windows against START** (8b357d7): the booking match's role test,
  and op_forget().

**Checked and left:**

- A BLOCKED handler preempted by the trampoline between its role test and
  `book_failed = true` stages an operation behind a dead request. The slot is already
  lost and the 20 ms backstop answers it, inside A9-3's 100 ms.
- A booking MPSL refuses is not placed again, though it usually has most of a period
  of notice. A refused fresh request costs master the same slot, so it is no
  regression, but it is cheap insurance for Stage B to consider.
- `min_arm_lead_elastic_us` is a uint16, while the two lead Kconfigs allow up to
  100 000. Over about 65 ms it wraps. Bench values only.
- `book_orphan` counts a booking whose operation is already in flight, so it can
  include a slot that was then lost.

**Confirmed safe:** every follow-on is armed in the dispatch that the release settles
after, so the early release keeps the grant for it. The skip's 750 us margin left at
least 706 us (`skip_left_min`). The late-arm subtraction into the backstop does not
wrap for lateness under 20 ms; a test written for it passed before any fix and was
dropped.

Mutation spot-checks on Bluefin (`ztest_review_mutations.txt`) confirm the skip, the
booking-due, the early release, the overdue-chunk and the `book_ahead` tests all fail
without their code.

## 69:108 and the extension skip

Decision 3 rested on the skip stopping the assert. The evidence, by refusals:

| Image | Early release | Skip | Refusals | Asserts | Log |
|---|---|---|---|---|---|
| Phase 0a, master-based | no | no | 4894 to the assert | 1 | `phase-0a/soak-fixed1-*` |
| (a), master-based | no | no | 5613 to the assert | 1 | `phase-1-a/thread2/` |
| (d2) s14, Phase 1 | no | no | 1376 and 3594 to the asserts | 2 | `phase-1-d2/thread{1,2}/` |
| master, `gate.conf`, no other stack | no | no | **0** (`ext=87/0`) | 1 | `phase-1-d2/gate_disc_master/` |
| (d2) s14, today | no | no | 30 411 | 0 | `soak-ctrl_s14-*` |
| Head, no release, no skip | no | no | 27 091 | 1 (at 22 069) | `soak-norelease-*` |
| Single session, no skip, 2 arms | yes | no | 83 642 | 0 | `soak-noskip{A,B}-*` |
| Single session, skip, 2 arms | yes | yes | 3618 | 0 | `soak-skip{A,B}-*` |
| Two sessions, no skip | yes | no | 59 495 | 0 | `soak-d2noskipA-*` |
| Two sessions, skip | yes | yes | 2367 | 0 | `soak-d2skipA-*` |

What this supports:

- **The skip is not shown to prevent the assert.** The no-skip arm did not assert, so
  there was nothing for the skip to prevent. The skip arms' 5985 refusals are too few
  to say anything at the rate below.
- **All six asserts came from images without the early release, and none from an
  image with it.** Without it the rate is about 1 per 15 000 refusals; at that rate,
  143 137 refusals with it would expect about 10. This is suggestive only: the arms
  span builds and days, and no mechanism links a held reserve to the assert.
- **The assert does not need a refusal.** `gate_disc_master` asserted on a grant whose
  extensions had all succeeded, with no other stack.
- **The positive control is weak.** The s14 image, which asserted twice in two runs in
  Phase 1, did not assert in 15 minutes today. The assert is rarer and less regular
  than Phase 1 suggested.

The instrument works. The `norelease` arm's assert was captured on the console, and
the harness recorded it, reflashed and carried on. `thread.conf` halts on an assert,
so a board that asserted would have stopped its 5 s counter lines.

The four earlier single-session arms are valid for what they test. Each ran 30
minutes on one boot with counters to the end. Their `"dut_rc": 1` is `ant_verify`
failing channel 1, the deliberately absent #43210, on every arm, and
`"dut_segments": 0` means the port never died.

The skip stays. Listening with it and without it was the same (468.8 s and 466.0 s
credited in 30 minutes), and it takes about 96 % of EXTEND_FAILED handling out of the
zero-latency callback.

## Lead sweep

The lead held fixed, 60 s a step, beside an attached peer and with the leader stopped.
Elastic requests refused, and the share of air credited as listening:

| Lead | Refused | Listening |
|---|---|---|
| 4 ms | 99-100 % | 0.9-1.5 % |
| 6 ms | 99-100 % | 0-0.1 % |
| 8 ms | 84-89 % | 1.6-2.5 % |
| 12 ms | 75-80 % | 7.4-9.7 % |
| 16 ms | **0 %**, in all 8 steps | 25-28 % |
| 24 ms | 0 % | 21.5-21.6 % |

Attached and detached read the same. The 30-minute soak at CONTENDED confirms 16 ms:
0 of 53 080 requests refused once the lead had settled. Nothing between 12 and 16 ms
was tried.

**The first session's refusal figures were a harness error.** It reported about 40 %
refused at 16 ms. `sweep_sum.py` opened each step at the `D2SWEEP` line, but the
counters logged straight after that line cover the 5 s before the lead changed, so
each row held 5 s of the previous lead. `sweep_sum_corrected.py` fixes the window. The
listening figures in b05bcff hold, and so does the choice of 16 ms.

Listening is capped at about a quarter of the air because every grant under Thread
ends at its first compare, and a first bite is at most 10 ms.

## The e7749c2 regression

With the chunk opening at the window's end, `gate.conf` read 6.1 and 8.2 % tracked
loss against the prototype's 0.2 % in the same sitting. Removing that one change read
0.2 % (`v_nofixend`), and the final image reads 0.0 %. It needs the early release too:
without it (`v_nofix1`) the earlier chunk is dropped for lack of notice and loss is
0.2 %.

**Partly explained, from counters only.** The losses came in bursts: at each one
`mis` 8, `busy` 7, `nosess` 12, `elblk` 4, and about nine tracked arms refused. The
refused scan requests overlapped the other session's booked air by up to 9.4 ms. A
terminal for an operation aborted before it opened also moved `fix_end`, so the scan
could open inside a tracked window still booked. MPSL refused it, and the retries
kept both sessions busy until the tracked arm found none free.

**Restricting the change to real completions removed all of that and left about 2 %**
(`fe2_*`): one slot at a time, each with `early` 1, `eo` 1 and `busy` 4, roughly every
20 s. **That remainder is unexplained**, and the session stopped there because the
revert removes it. The after-slot band therefore stays where it was, and the
evaluation's ~5.5 ms is not achieved.

## Sizes

Against the baselines in [d2.md](d2.md), with the bench counters out:

| Image | Flash | RAM |
|---|---|---|
| L15 `thread.conf;bridge.conf` | +5096 B | +448 B (198 376) |
| L15 `thread.conf;matter.conf` | +5004 B | +448 B (253 708, 8436 free) |
| nRF52840 DK Matter | +5040 B | +448 B (245 144) |

## Unexplained or unverified

- What causes 69:108, and whether the early release really stops it.
- The 2 % left when e7749c2 was restricted to real completions.
- Why refusal falls from 75 % to 0 between 12 and 16 ms.
- Occasional acked-transfer failures. About 1 send in 60 fails on the head and the
  prototype alike, with no pattern by direction.
- The lead policy under BLE has ztests only. Stage A ran no BLE leg.

## For Stage B

1. The plan's acceptance bench: 17-offset A/B/A, R1 pair, ch0 loss and `sched_missed`,
   fallback counters, Thread leg, and the bridge binding a second sensor.
2. Repeat (d2)'s higher tracked loss under BLE A/B/A, and the lead policy with it.
3. Decision 5 is now the only route to a shorter after-slot band. Watch it for the
   e7749c2 failure, whose remainder is unexplained.
4. Listening under Thread is about 26 %, so set the acceptance bar for discovery beside
   Thread with that in mind. Since a skipped grant never extends, a longer first bite
   at the CONTENDED lead is worth sweeping.
5. Use `sweep_sum_corrected.py`, not `sweep_sum.py`, for any further sweep.
6. An `ack_pair` acceptance needs at least 300 s and 300 commands; the 60-send pairs
   here are a regression check.
