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
| 7 search/track hand-off | `radiant_radio_diag_get()` [4..15], `radiant_sched.c` [16..27], reserved [28..35] | search frame cut by the chunk's close | search frame cut by an abort | search END within 300 µs of the close | bounded RX armed opening late | worst such lateness µs | bounded RX expired behind a scan chunk | reserved, `0xFFFFFFFF` | reserved, `0xFFFFFFFF` |

`sets_advanced_uncovered` is `0xFFFFFFFF` without `CONFIG_RADIANT_SEARCH_COVERAGE`.

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
