# ADR 0018 Phase 2: evaluation

Phase 1 built and measured two of the four candidates: (a), a held grant with
bridging extensions, and (d2), a second MPSL timeslot session. The full
reports are [a.md](a.md) and [d2.md](d2.md), and their raw-log indexes are
[a-raw-logs.md](a-raw-logs.md) and [d2-raw-logs.md](d2-raw-logs.md). The
prototypes live on the unmerged branches `wip/adr18-a` (fd7e344) and
`wip/adr18-d2` (3aee8e9), both based on master 237b7c4. The figures below
were checked against the raw logs before this summary was written.

(d1), chaining the next request with `ACTION_REQUEST`, and (e), a per-stack
place lead, were not run as separate candidates. The reasons are under
"Candidates not run".

## Matrix

Weighted toward the long-term bridge: a Thread or Matter image tracking
several sensors while it searches for more.

| Criterion | Master (237b7c4) | (a) held grant | (d2) second session |
|---|---|---|---|
| **Zero cost** (gate) | n/a | PASS: 8/8 rows identical | PASS: 8/8 rows identical |
| **Rule 1 under Thread** (kill criterion) | every window armed | **FAIL**: 64 windows missed per 150 s, 13.4 % and 15.6 % loss against master's 4.5 % | **PASS**: every slot booked and granted in 4 runs, 5.6-9.7 % loss against master's 6.6-7.6 % |
| **Rule 1 on an R1 pair** (on air, B walked to 12.5 ms) | A 0-9 % lost, **B 100 % lost** | **FAIL**: A 33 %, B 67 % (one slot moved between them) | **PASS**: A 11 %, B 11 % (two slots per period) |
| **Rule 1 under BLE** | baseline | 2-3 windows missed per 120 s | only bookings refused by the advertiser: 2 and 6 per 120 s, the same path master's requests take |
| **Deaf fraction, ztest, 1/2/4/8 channels** | 30 / 40 / 80 / 100 % | 1.6 / 2.5 / 4.1 / 8.4 % | 6.1 / 9.5 / 13.4 / 26.9 % |
| **Phases heard under Thread** (of 25) | 7 | 16 (no grant held; extensions refused) | 24-25 |
| **Phases heard under BLE** (of 25) | 18-21 | 25 | 25 |
| **In-band discovery, `gate.conf`** | 1/5 | 5/5, median 5 s | 4/5, about 9 s |
| **In-band discovery, Thread** | 0/6 | 1/4 | 4/6 |
| **In-band discovery, BLE** | 1/3 | 6/6 | 4/6 |
| **Blind band left** | three 24.4 ms leads per period | under 3 ms per band | first ~9.5 ms after each tracked window (~5.5 ms with the early-release fix) |
| **MPSL ASSERT 69:108 under Thread** | seen once in Phase 0a | once in 2 runs | twice in 2 runs before the extension skip, none in 2 runs (~9000 placements each) after it |
| **HAL contract** | n/a | additions only, one-operation rule kept | additions only (`caps.booking`, `caps.min_arm_lead_elastic_us`, `RADIANT_RX_ELASTIC`, `radiant_radio_book()`), one-operation rule kept |
| **Scheduler and gate complexity** | n/a | hold, bridge, continuation and cap state; cluster rules | two-session pool, booking table, held placement, elastic lead, extension skip |
| **L15 Matter RAM** (free) | 8 884 B | +120 B (8 764 B) | +640 B (8 244 B) |
| **Flash, MPSL images** | n/a | +2.4 kB | +4.9 kB (about half is bench diagnostics) |
| **TI** | n/a | build-only, no edit | build-only, no edit |

## Reading the matrix

**Only (d2) passes the kill criterion where the bridge runs.** The criterion
is that a refused extension or request may cost listening but never a tracked
slot. Under Thread, MPSL refused every extension on every image, so any design
whose tracked boundary depends on an extension fails there. (a) does: its
bridged chunk is denied about 1.7 ms before the tracked window behind it, too
late to place that window again. (d2)'s tracked slot depends only on a fresh
request placed about a period ahead, and Thread granted those requests every
time.

**(a) is better on a quiet radio or under BLE, and nowhere else.** On
`gate.conf` its deaf fraction is a quarter of (d2)'s, and it finds every phase,
including the 4.6 ms phase just after a tracked slot, which (d2) misses. But
its gains need extensions, which another stack can refuse at any time. The R1
cycling is unexplained, and it makes an R1 pair worse than master for the
earlier channel. Building (a) as well would mean two designs at the same
boundary: F2 shows that a bridging extension over a booked instant blocks that
booking.

**(d2) scales less well at 8 channels** (27 % deaf against (a)'s 8 %). Each
tracked channel keeps about 8 ms of turnaround on each side. That is still a
quarter of master's 100 %, and the bands no longer reset at each tracked slot,
so every phase is heard within a few seconds.

**RAM:** (d2) costs 640 B on the tightest image, the L15 Matter build, leaving
8.2 kB free. About 120 B of that is bench counters, which a shipping build can
drop.

## Candidates not run

- **(d1) `ACTION_REQUEST` chain.** It closes a boundary with a request placed
  at very short notice when a grant ends. Under Thread, MPSL refused (d2)'s
  scan requests even with 8-16 ms of notice; its elastic lead settled at
  16-24 ms. Bug 26 saw 2.5 ms of notice about 80 % blocked. (d1) would likely
  fail under Thread the way (a) did, and on `gate.conf` and BLE it adds little
  over (d2). Dropped.
- **(e) Per-stack place lead.** With (d2), the full place lead is spent only on
  bookings placed a period ahead, so it no longer costs listening. What is left
  is (d2)'s elastic scan lead, which already adapts: it starts at 3 ms, doubles
  on a refusal that had full notice, and decays on grants. (e) becomes a Phase 3
  task: sweep that lead per stack, and stop the backoff growing on BLE
  refusals, which do not depend on notice.

## Proposed design

(d2) as prototyped on `wip/adr18-d2`, with:

1. **The extension skip**, kept. After three refused extensions in a row the
   gate stops asking for 64 elastic grants. It is what made (d2) stable under
   Thread, and the same skip may protect master from the 69:108 assert.
2. **Early release for frame-bearing tracked windows**, as a separate fix. On
   master, a tracked window that received a frame holds its ~4 ms follow-on
   reserve to the grant's backstop. Releasing it early shrinks (d2)'s
   after-slot band from ~9.5 ms to ~5.5 ms and gives that air back to the other
   stack.
3. **(e) folded in**: a per-stack sweep of the elastic lead, and a backoff that
   grows only on refusals that correlate with short notice.
4. **Bench diagnostics** made optional (`d2_stats.conf` only), so shipping
   sizes drop.

The design adds to the radio HAL contract, so per the plan it gets a new ADR,
and ADR 0018 is marked accepted with a pointer to it.

## Evidence limits

- Thread and BLE legs: one 120 s rule-1 reading and three in-band trials per
  run, not the 17-offset rig. Phase coverage comes from 10 ms `TEMPDIAG` bins,
  which are coarse.
- The R1 walk is one run per image (A/B) for both candidates.
- (d2)'s Thread A/B/A spans two (d2) builds, with and without the skip.
- The room moved a lot during both sessions: one master image read 5-36 %
  tracked loss within an hour against a Garmin at 0-0.8 %. The rule-1 verdicts
  rest on the scheduler's own counters (booked, granted, missed), not on DUT
  loss alone.
- (d2)'s BLE runs read higher DUT loss than master's (11.9 % and 18.1 %
  against 8.8 %), with every slot granted except those the advertiser refused.
  This is not explained, and Phase 3 should repeat it A/B/A.
- Nothing is verified on air on TI.
