# 0023 — A request-only diagnostic message, 0xF7 `MESG_RADIANT_DIAG`, paged and portable across backends

- **Status:** accepted 2026-11-12. Built behind `CONFIG_RADIANT_DIAG_COUNTERS`
  (default **n**) and, on the dongles, `CONFIG_ANT_DONGLE_DIAG` (default **n**,
  depends on the 0xF6 health counters).
- **Date:** 2026-11-12
- **Decides:** the TODO.md "`radiant_search_stats` over the host" item (ADR
  0022 gate 3): the search counters go here, on pages of their own, and 0xF6's
  conditional coverage field is left as it is.
- **Code:** `radiant/include/radiant/radiant_diag.h`,
  `radiant/src/antr/ant_radio_radiant.c` (`antr_diag_page()`),
  `radiant_radio_diag_get()` in each backend, the 0xF7 arm of
  `apps/common/ant_serial_bridge.c`'s request handler, `tools/ant_diag.py`.

> **Provenance.** Clean-room. Written from this project's source and its own
> bench measurements. Nothing here derives from sdk-ant or any ANT document.

## Context

The 2026-11-12 TI bench lost acknowledged messages with no RF entry at all,
and lost every chained burst packet to a missed arm. Both investigations need
counters the firmware already keeps, or nearly keeps, but cannot report:

- `struct radiant_api_stats`, `struct radiant_sched_stats` and
  `struct radiant_search_stats` are always compiled and always counted. Today
  they reach the outside world only through LOG lines and a debugger, and a
  debugger connect wipes the CC2652R's SRAM (memory note
  `ti-launchxl-bench-2026-11-12`).
- The cc26xx backend's `sync_frames` / `sw_addr_mismatch` were read through a
  bench-only patch that remapped 0xF6 fields. That patch was never meant to be
  committed, and it had to be rewritten for every question.
- 0xF6 is u16, saturating, one fixed layout, and one of its fields is
  conditional. Appending search counters after that field needs a layout
  decision TODO.md left open.

The plan's bar: build a new message only if it is good long-term design and
value; otherwise keep the counters in a bench-only patch.

## Decision

Claim **0xF7** from the reserved RadiANT block (0xF7-0xFA) as
`MESG_RADIANT_DIAG`, with these properties.

1. **Request-only.** `MESG_REQUEST [page, 0xF7]`. The page number rides in the
   request's channel byte. Nothing is emitted unsolicited, so a host that never
   asks sees a byte stream identical to a build without it, and the
   conformance transcripts stay byte-identical. A page this build does not have
   answers `INVALID_PARAMETER_PROVIDED`.
2. **One fixed shape for every page, 36 bytes.** `[0]` page, `[1]` structure
   version 1, `[2]` flags (bit 0: the page carries a value read as "not
   measured"), `[3]` backend id (0 null, 1 nRF, 2 cc26xx, 0xFF fake), then
   eight u32 LE counters at `[4..35]`. The longest ANT body is 38 bytes, so
   one page fits with room to spare.
3. **u32, wrapping, never saturating.** Readers diff a before/after pair, as
   every bench in this repo already does. u32 wraps after days even at the
   per-frame rate, so there is no saturation flag to make every field a floor
   (the 0xF6 lesson).
4. **"Not measured" is `0xFFFFFFFF`, never 0.** A backend that cannot count
   something says so, so a zero always means zero (the 0xF6 coverage-field
   lesson, without a variable body length).
5. **Pinned v1.** A field's meaning never changes. A new meaning is a new page
   or a field in a page's unused tail, never a version bump (memory note
   `beta-versions-pinned-v1`).
6. **Portable pages first, one backend pair.** Pages 0-2 and 5-6 are counted
   in the core and mean the same on every backend. Pages 3-4 come from a single
   HAL hook, `radiant_radio_diag_get()`, with a generic shape (a fixed index
   table) that both backends fill. No TI-only page.
7. **Zero cost when off.** The new hot-path counters are a macro that
   compiles to nothing without `CONFIG_RADIANT_DIAG_COUNTERS`, and the page
   function and backend hook are not built. The `zero-cost` CI job's section
   and symbol comparison is unchanged.

### Pages

| page | source | `[4..7]` | `[8..11]` | `[12..15]` | `[16..19]` | `[20..23]` | `[24..27]` | `[28..31]` | `[32..35]` |
|---|---|---|---|---|---|---|---|---|---|
| 0 api | `radiant_api_stats` | sched_dones | sched_missed | sched_failed | sched_denied | slots_missed | slots_denied | rx_sched_events | xfer_no_ack |
| 1 master turnaround | new, api | posted | refused by the scheduler | skipped (no id/address) | turn dropped (not TRACKING) | done OK | done MISSED | done DENIED/FAILED/ABORTED | frames in a MASTER_RX slot |
| 2 transfer/sched | new, burst + `radiant_sched_stats` | continuation submits | continuation late | turnaround min µs | turnaround max µs | TX expiries in `pass_step` | worst TX margin µs (i32) | sched arm_rejected | sched missed |
| 3 backend A | `radiant_radio_diag_get()` | RX posted | RX refused | terminal OK | terminal empty | terminal error | status-error count | last error status | sync entries |
| 4 backend B | `radiant_radio_diag_get()` | sw-address mismatch | window-test drops | CRC fails | alternate-sync hits | RX not-OK/ignored | RX buffer full | drain latency max µs | min arm lead seen µs |
| 5 search A | `radiant_search_stats` | windows | sets_advanced | sweeps | frames_ok | crc_fail | acquired | scan_reports | timeouts |
| 6 search B | `radiant_search_stats` | cache_steers | cache_acquires | permit_rejected | rssi_rejected | decode_failed | bad_filter_index | late_events | sets_advanced_uncovered |
| 7 search/track hand-off | `radiant_radio_diag_get()` [4..15] and [28..35], `radiant_sched.c` [16..27] | search frame cut by the chunk's close | search frame cut by an abort | search END within 300 µs of the close | bounded RX armed opening late | worst such lateness µs | bounded RX expired behind a scan chunk | transmit refused: start compare behind the counter | receive window refused: start compare behind the counter |
| 8 timing | `radiant_radio_diag_get()` [4..19] and [32..35], `radiant_sched.c` [20..31] | frames in the longest drain | longest one-entry work µs (callback excluded) | longest software CRC µs | longest synchronous rx callback µs | longest scheduler pass µs (interrupts off) | most `pass_step()` iterations in one pass | passes over 100 µs | drains over 250 µs |
| 9 pass breakdown | `radiant_sched.c` [4..23], `radiant_radio_diag_get()` [24..35] | expiry sweep µs | arm decision µs (backend call and done callbacks excluded) | backend arm calls µs | `notify_done()` callbacks µs | `book_ahead()` µs | pass `now` → backend `now` µs | smallest early-check slack µs (i32, `0x7FFFFFFF` = no sample yet) | early-check ETIME refusals |
| 10 receive integrity | `radiant_api_stats` [4..19], `radiant_radio_diag_get()` [20..35] | crc_repaired | crc_unrepairable | crc_repair_refuted | crc_repair_implausible | frames dropped by the late-ISR guard (nRF) | worst END-interrupt lateness µs (nRF) | longest `rx_t_sync()` µs (cc26xx) | longest `rx_match()` + copies µs (cc26xx) |

`sets_advanced_uncovered` is `0xFFFFFFFF` without `CONFIG_RADIANT_SEARCH_COVERAGE`.

Page 4 "alternate-sync hits" on cc26xx counts `syncWordId == 1` in a window
with one sync group. Until 2026-11-13 that second word was a decoy (`0x59C5`)
and the field read ~110/min. Since then the one-group window sets
`syncWord1 = 0`, which turns the second correlator off, so the field reads 0
and a non-zero value is an anomaly. The meaning is unchanged, so page 4 stays
at structure version 1. Evidence: `archive/benchmarks/2026-11-13-ti-todo/`.

Page 7 was added on 2026-11-12 for ADR 0022's gate 3: does a search reception
running up to a scan chunk's close delay the tracked window armed after it?
Its first three fields are the backend table's last three indices
(`RADIANT_RADIO_DIAG_SRCH_*`) and read `0xFFFFFFFF` on cc26xx and null. "Late"
counts only a bounded window the scheduler's first pass found still armable
from its own start, so a window posted for "now" (an acknowledgement window)
never counts. Unlike pages 5-6, every page-7 counter lives in a backend or
scheduler static and survives `SYSTEM_RESET`, so a before/after diff is valid
across `--reopen`.

Fields 6-7 first counted tracked slots opening within 2 ms after the search
heard the same device number, and how many of those missed. The gate-3 run
(`archive/benchmarks/2026-11-12-gate3/`) showed that count cannot measure a
hand-off cost. It only counts when the search hears a frame, which the short
match does about nine times as often, and it read 100 % missed on the off image
too. The fields were reserved before the page left the bench (never pushed or
released), so no host depends on them. The gate-3 images in that archive still
report the old meaning.

On 2026-11-14 fields 6-7 were given to the nRF backend (546be4d): a transmit
(6) and a receive window (7) refused `ETIME` at programming because the start
compare was written behind the counter (`start_compare_missed()`), which the
scheduler reports as MISSED. They are the backend table's last two indices,
after page 8's, and read `0xFFFFFFFF` on cc26xx and null. Expected 0 or single
digits. A reserved field always read `0xFFFFFFFF` ("not measured"), so an older
image reads as one without the check and a host needs no version test; the
structure version stays 1 (beta). Taking a reserved field rather than opening a
page is the one judgement call here; page 8 went the other way only because it
needed eight fields. The project owner kept this layout on 2026-11-14.

Page 8 was added on 2026-11-13 to find where the cc26xx receive drain's
~530 µs worst case goes (`archive/benchmarks/2026-11-12-ti-findings/`), and
how long the scheduler's interrupts-off pass runs, before anything is
optimized (`docs/performance-review-2026-11-13.md` finding 5). Every field
except 6 and 7 is a worst case since boot: read the later value, a difference
means nothing. Fields 0-3 and 7 come from the backend table's last five
indices; cc26xx times them on the RAT, and the nRF backend reads
`0xFFFFFFFF` (it trusts the hardware CRC and has no drain loop). Fields 4-6
use `radiant_radio_now()` and are measured on both. A new page rather than
page 7's reserved tail, which has two fields, not eight. Structure version
stays 1.

**Amendment, 2026-11-15: pages 9 and 10, and a cycle clock.** The 2026-11-15
performance review asked where the nRF52840's ~215 µs pass goes, how much of
`ARM_SETUP_US` it leaves, and what the TI per-entry receive work is made of.
Page 8 cannot answer: its pass time is one number, and the radio clocks are
too coarse or too dear to time sub-spans (a cc26xx RAT read costs a few µs).
So two new pages, by this ADR's rule that a new meaning is a new page, and one
helper, the **A0 cycle clock** (`radiant_diag_cycles()` in `radiant_diag.h`):
the core's DWT cycle counter, switched on by the backend's
`radiant_radio_init()` with its core clock (`SystemCoreClock` on nRF, a
48 MHz constant on cc26xx) and checked to advance. Where it does not run
(native_sim, a core without DWT, a counter that did not move), every field
timed with it reads `0xFFFFFFFF`. CYCCNT stops while the core sleeps, so only
spans that cannot idle are timed with it (interrupts off, or an ISR).

- **Page 9, pass breakdown.** Fields 0-4 are the scheduler's: per-pass sums of
  the expiry sweep, the arm decision (the rest of `pass_step()`, including
  `notify_armed()`), the backend arm calls, the `notify_done()` callbacks,
  and `book_ahead()` (`0xFFFFFFFF` without `CONFIG_RADIANT_SCHED_BOOKING`),
  each the worst since boot, in µs. The parts are disjoint; the decision is
  what remains of `pass_step()` and saturates at 0. Fields 5-7 are the
  backend's, at its early ETIME check: 5 is the pass's `now` to the backend's
  own `now`, both `radiant_radio_now()` reads, so radio-clock µs; the
  scheduler stamps it around its arm calls and an arm no pass made (a
  deferred MPSL grant, a direct backend call) is not sampled. This is the
  span `ARM_SETUP_US` must cover. 6 is the smallest slack at that check on an
  accepted arm, `at − (now′ + lead)`, an i32 that is never negative because a
  refused arm is counted in 7 instead; it reads `0x7FFFFFFF` until the first
  accepted arm, a "no sample yet" value distinct from "not measured". 7
  counts those refusals (receive and transmit), which until now the scheduler
  folded into `arm_rejected`. cc26xx's `arm_common()` floor check is the same
  kind of check and fills 5-7 too; null reads `0xFFFFFFFF`.
- **Page 10, receive integrity.** Fields 0-3 are `radiant_api_stats`' CRC
  repair counters (the `crc_repair_page1.patch` bench counters, done
  properly), `0xFFFFFFFF` without `CONFIG_RADIANT_CRC_REPAIR`. Field 4 counts
  frames the nRF late-ISR guard drops; the slot and its counter exist now and
  read 0 until that guard lands (it is a count, so 0 is true, not
  "unmeasured"). Field 5 is the worst nRF END-interrupt lateness,
  `now − (CC_SYNC + software address, body and CRC airtime)`, coded PHY
  skipped. Fields 6-7 are cc26xx's longest `rx_t_sync()` and longest
  `rx_match()` plus the address/body copies of one receive entry, on the A0
  clock. 4-5 read `0xFFFFFFFF` on cc26xx, 6-7 on nRF.
- **Page 8 on cc26xx.** Fields 1-3 (per-entry work, CRC, core callback) move
  from the RAT to the A0 clock. Their meaning (µs) is unchanged, so the page
  keeps its number and version; readings taken before and after this change
  differ by the RAT reads that used to sit inside the spans. `drain_max_us`
  (page 4) stays on the RAT.

Levels on both pages (every field except 9/7, 10/0-4) are worst cases since
boot and must not be diffed; `tools/ant_diag.py --diff` reports the later
value. Structure version stays 1 (beta), and an older image refuses pages 9-10
with `INVALID_PARAMETER_PROVIDED`, which the reader skips.

"Turnaround" on page 2 is NEXT_BLOCK-to-submit: the time from the engine
asking for the next block to the host block arriving.

## Judgement against the bar

- **Long-term value.** Yes. It exposes three always-on stats structs that
  were unreachable without a debugger, it closes a TODO item (search stats)
  with no layout fight, and it replaces a class of bench-only patches with one
  maintained reader.
- **Long-term design.** Yes. It is paged, so adding a page costs nothing and
  never moves an offset. It is fixed-length, so there is no conditional field.
  It is portable, so nRF and TI read with one tool. And it is request-only and
  zero-cost when off, so shipping images are unaffected.
- **Cost.** One message id out of four reserved, one Kconfig pair, about 30
  new counter sites behind a macro, one HAL function per backend. That is
  acceptable.

## Not done

- No unsolicited or periodic emission. A streaming mode would change the
  host byte stream and is a different decision.
- No reset-on-read. Readers diff before/after, and a reset would make two
  readers interfere.
- 0xF6 keeps its layout. It is what `tools/ab_gates.toml`'s loss accounting
  reads, and a host acting on loss should not depend on a debug option.
