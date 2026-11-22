# 0023 — A request-only diagnostic message, 0xF7 `MESG_RADIANT_DIAG`, paged and portable across backends

- **Status:** accepted 2026-11-12. Built behind `CONFIG_RADIANT_DIAG_COUNTERS`
  (default **n**) and, on the dongles, `CONFIG_ANT_DONGLE_DIAG` (default **n**,
  depends on the 0xF6 health counters).
- **Date:** 2026-11-12
- **Decides:** the TODO.md "`radiant_search_stats` over the host" item (ADR
  0022 gate 3): the search counters go here, on pages of their own, and 0xF6's
  conditional coverage field is left as it is.
- **Code:** `radiant/include/radiant/radiant_diag.h`,
  `radiant/src/ant_api/radiant_ant.c` (`radiant_ant_diag_page()`),
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
| 9 pass breakdown | `radiant_sched.c` [4..23], `radiant_radio_diag_get()` [24..35] | expiry sweep µs | arm decision µs (backend call and done callbacks excluded) | backend arm calls µs | `notify_done()` callbacks µs, per scheduler entry (a pass, or a backend callback's body) | `book_ahead()` µs | pass `now` → backend `now` µs | smallest early-check slack µs (i32, `0x7FFFFFFF` = no sample yet) | early-check ETIME refusals |
| 10 receive integrity | `radiant_api_stats` [4..19], `radiant_radio_diag_get()` [20..35] | crc_repaired | crc_unrepairable | crc_repair_refuted | crc_repair_implausible | frames dropped by the late-ISR guard (nRF) | worst END-interrupt lateness µs (nRF) | longest `rx_t_sync()` µs (cc26xx) | longest `rx_match()` + copies µs (cc26xx) |
| 11 scheduler contention | `radiant_sched_stats` [4..27] and [32..35], `radiant_api_stats` [28..31] | windows_merged | preempted | replans | arm_rejected | tx_armed | stale_events | slots_collided (ADR 0024) | turn_yields (ADR 0024) |
| 12 stale events by site | `radiant_sched.c` [4..23] | receive event with a frame, op retired | other receive event, op retired | frame with a filter index the window lacked | transmit event, op retired | energy-detect event, op retired (`0xFFFFFFFF` without `CONFIG_RADIANT_ED_SCAN`) | reserved | reserved | reserved |
| 13 transfer outcomes | `radiant_api_stats` [4..27] | transfers the watchdog terminated | held slave command expired with no opener (tracking, or still searching if accepted while searching) | held slave command failed at RX_FAIL_GO_TO_SEARCH | held slave command sent from the reacquiring frame | failed on a wrong acknowledgement | engine or radio refused to start | reserved | reserved |

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
  `notify_armed()`), the backend calls (the arm calls and `abort_armed()`'s
  `radiant_radio_abort()`), the `notify_done()` callbacks, and
  `book_ahead()` (`0xFFFFFFFF` without `CONFIG_RADIANT_SCHED_BOOKING`), each
  the worst since boot, in µs. The sweep, backend calls and callbacks are
  timed directly and the decision is what remains of `pass_step()`, floored
  at 0. They are not strictly disjoint: a `notify_done()` callback nested
  inside a backend call (an abort that delivers its terminal synchronously)
  counts in both 2 and 3, and the floor keeps 1 from wrapping when that
  happens. Fields 5-7 are the backend's, at its early ETIME check: 5 is the
  pass's `now` to the backend's own `now`, both `radiant_radio_now()` reads,
  so radio-clock µs; the scheduler stamps it (the low 32 bits, so one store
  writes it even with the MPSL signal handler able to interrupt) around its
  arm calls, and an arm no pass made (a deferred MPSL grant, a direct backend
  call, anything in the MPSL signal handler) is not sampled. 5 is a max that
  starts at 0, with no "no sample" value: on MPSL builds most arms are
  granted later and programmed with the stamp cleared, so it is rarely
  sampled there and a 0 means little. This is the span `ARM_SETUP_US` must
  cover. 6 is the smallest slack at that check on an
  accepted arm, `at − (now′ + lead)`, an i32 that is never negative because a
  refused arm is counted in 7 instead; it reads `0x7FFFFFFF` until the first
  accepted arm, a "no sample yet" value distinct from "not measured". 7
  counts those refusals (receive and transmit), which until now the scheduler
  folded into `arm_rejected`. cc26xx's `arm_common()` floor check is the same
  kind of check and fills 5-7 too; null reads `0xFFFFFFFF`.
- **Page 10, receive integrity.** Fields 0-3 are `radiant_api_stats`' CRC
  repair counters (the `crc_repair_page1.patch` bench counters, done
  properly), `0xFFFFFFFF` without `CONFIG_RADIANT_CRC_REPAIR`. Field 4 counts
  frames the nRF late-ISR guard drops: the END branch snapshots the buffer and
  registers between two CC_SYNC reads, and a snapshot is torn if the reads
  differ or the capture is younger than the frame's airtime (the next frame's
  ADDRESS had already landed). It also counts a frame lost because a second
  frame ended while it was being read (END raised again during the snapshot:
  the snapshot is retaken and only the later frame is delivered). Field 5 is the worst nRF END-interrupt
  lateness, `now − (CC_SYNC + software address, body and CRC airtime)`, coded
  PHY skipped, taken from the snapshot of a frame the guard accepted. An
  interrupt late past the next frame's ADDRESS is a torn snapshot, counted in
  field 4 and not in field 5, so field 5 covers only lateness shorter than the
  gap to the next ADDRESS.
  Fields 6-7 are cc26xx's longest `rx_t_sync()` and longest
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

Page 11 (scheduler contention) was added with ADR 0024; that record has the
reasoning. Its row is in the table above.

**Amendment, 2026-11-16: page 9 field 3 measured per entry, and page 12.**

- **Page 9 field 3 read 0 on hardware.** A backend callback entry
  (`hal_rx()`, `hal_tx()`, `hal_ed()` in `radiant_sched.c`) not nested in a
  pass sets `in_pass` for its body, so the `notify_done()` callbacks a
  window's terminal delivers were timed into the per-pass sum. That body then
  ends with `pass()`, whose first act is to clear the sum, and nothing had
  reported it. Nearly every callback runs from that path, so the field held
  only the rare callback made inside a pass (an expiry in the sweep, an
  abort). Now such a body opens and closes its own sum, and the field is the
  worst sum per **scheduler entry**: a pass, or a callback entry's body (the
  pass that body ends with is a separate entry). Each callback counts in
  exactly one entry, and nothing else changes. A callback made from thread
  context outside any entry is still not timed: the ABORTED a co-member hears
  when `drop_slot()` aborts an open window, from `radiant_sched_cancel()` or
  from a `radiant_sched_request_rx/tx/ed()` that replaces an in-flight
  request. (The same calls made from a consumer callback inside an entry are
  nested in that callback's span and counted there.) The open and fold each
  share a critical section with the entry's `in_pass` write, inside the same
  diag-only macros, because on nRF MPSL `radiant_nrf_gate_on_denied()` calls
  `hal_rx()` (DENIED) from a cooperative thread with interrupts on: an ISR
  entry landing between the write and the open or fold would otherwise lose
  its sum. Such a thread-context entry is the one span on this page that can
  be preempted, so on MPSL its sum may include preempting ISR time, contrary
  to the A0 rule that only spans that cannot idle are timed. Treat a field-3
  maximum on an MPSL image as an upper bound.
  The meaning sharpens from "within one pass" to "within one entry". The old
  reading was an undercount, not a different quantity, so the field keeps its
  index and the structure version stays 1. Fields 0, 1 and 4 had no such
  loss: the sweep, the step remainder and `book_ahead()` exist only inside
  `pass()`. Field 2 has a narrower one, left as it is because field 2 is
  defined per pass: the scheduler makes arm calls only from a pass, but it
  also times `abort_armed()`'s `radiant_radio_abort()`, and `drop_slot()`
  calls `abort_armed()` outside a pass from `radiant_sched_cancel()` and from
  `radiant_sched_request_rx/tx/ed()`. Such an abort is timed into the
  per-pass sum and cleared unreported by the next pass. That includes an
  abort a consumer callback makes inside a callback entry. None of these is a
  pass, so none belongs in field 2.
- **Page 12, stale events by site.** Page 11 field 5 counts every event the
  scheduler drops as stale, and preemption makes those routine, so the total
  cannot separate the routine case from a lost frame. Page 12 splits it by
  where it was dropped: 0 a receive event carrying a frame (OK or CRC_FAIL)
  for a retired operation, a frame heard and then dropped; 1 any other
  receive event for a retired operation, mostly the ABORTED terminal every
  preemption makes; 2 a frame for the armed window with a filter index the
  window did not have; 3 a transmit event, and 4 an energy-detect event, for
  a retired operation (4 reads `0xFFFFFFFF` without `CONFIG_RADIANT_ED_SCAN`).
  Fields 0-4 add up to page 11 field 5, give or take a lost increment (the
  counters are not atomic). The split is counted in `radiant_diag_core`, not
  `radiant_sched_stats`, so an image without the diag counters is unchanged.
  By this record's rule a new meaning needs a new page: page 11 is full. Fields
  5-7 are reserved and read `0xFFFFFFFF`, so the page always sets the
  "not measured" flag. An older image refuses page 12 and the reader skips it.
- **Test hook.** On a target without a cycle counter (native_sim)
  `radiant_diag_cycles()` now reads a variable, `radiant_diag_cycles_sim`, that
  is always 0 unless a unit test advances it. `radiant_diag_cycles_to_us()`
  still reads `0xFFFFFFFF` there, so no page changes. Without it, field 3's
  fix could not be tested off hardware.

**Amendment, 2026-11-16: page 8 field 2 on cc26xx without preemption.**
The cc26xx receive drain runs in the TI RF driver's callback, a software
interrupt that `SwiP_zephyr.c` dispatches from the lowest-priority hardware
interrupt, so every other interrupt (the RF core's, the system timer, the
UART) can land inside a span it times. Field 2 read 70 µs before the
`crc16_itu_t()` fast path and 32 and 19 µs after, for a CRC over one
frame's address and body. Those readings held the CRC plus any interrupt that
preempted it. The A0 rule above lets an ISR's span be timed, but this ISR is
the lowest priority, so anything can preempt it.
Now the diag build masks interrupts (`irq_lock()`) around the CRC computation
and its two clock reads only. The CRC is a few microseconds, so the delay this
adds to any other interrupt is the same, and only in a diag image. Without
`CONFIG_RADIANT_DIAG_COUNTERS` nothing is masked, and the image is
byte-identical.

The field keeps its index, and the structure version stays 1. The field was
always defined as the CRC computation time. The old reading overcounted the
same quantity (the converse of page 9 field 3's undercount), so this is not a
new meaning. Like the 2026-11-15 move from the RAT to the A0 clock, readings
from before and after this change are not comparable. Treat an older image's
value as an upper bound. The transmit CRC is still not timed. It runs in
`radiant_radio_tx()`, which the scheduler calls only from a pass, with
interrupts off, so its cost is already inside page 9 field 2 and nothing can
preempt it. Page 8 fields 1 and 3 and page 10 fields 6-7 are timed in the same
drain with interrupts on. They are still upper bounds that include
preemption, apart from the CRC span inside field 1.

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
  (Amended 2026-11-16, below.)

## Amendment 2026-11-16: 0xF6 gains a field and a sentinel

Owner accepted in beta (fix/ti-cmd-stall). This partly reverses the 0xF6 lines
above: the Context's "one of its fields is conditional" and the "without a
variable body length" in decision 4.

- **New field.** 0xF6 appends `uart_rx_overruns` at [32..33]: receiver-overrun
  events on a UART transport, each at least one lost host byte. This is the
  witness for the TI command stall
  (archive/benchmarks/2026-11-16-ti-cmd-stall/).
- **The optional field now has a sentinel.** [30..31] (coverage) used to be
  present or absent, which gave a variable body length. [32..33] needs it
  present, so every build now answers 34 bytes.
  - An optional field the build or transport cannot measure reads `0xFFFF`
    (`ANT_HEALTH_NOT_MEASURED`), the u16 form of this message's `0xFFFFFFFF`
    rule.
  - Real counts clamp at `0xFFFE`.
- **Unchanged.** The structure version stays 1, the layout stays append-only,
  and no offset is reused. Readers still refuse a body shorter than 30 bytes and
  decode what they know from a longer one; they now also treat `0xFFFF` in an
  optional field as absent.
- **Not this message.** 0xF7 is unaffected.
- **Mixed-version readers.** See CHANGELOG [Unreleased]. A v1.2.0 reader shows
  65535 coverage on non-coverage builds; a new reader shows an old MPSL image's
  saturated coverage as not measured.
