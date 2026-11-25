# The radio contract — what `radiant` has to implement

**Checked by:** the `BUILD_ASSERT` block in `apps/common/ant_radio_sdk_ant.c` (Wave 2), which stops
compiling if any signature or constant in `<radiant/radiant_ant.h>` has drifted from its sdk-ant
counterpart; and the standalone compile of the header itself —
`arm-zephyr-eabi-gcc -c -I. -Isrc -std=c11 -Wall -Wextra -Werror` over a translation unit that
includes nothing but `<radiant/radiant_ant.h>`. The coverage table below is checked by the link: every
symbol in it is defined by `apps/common/ant_radio_stub.c` and referenced by `apps/common/ant_serial_bridge.c`, so
an omission is a link error, not a review miss.

---

## What this document is

`<radiant/radiant_ant.h>` is the seam between `apps/common/ant_serial_bridge.c` and whatever implements the ANT
link layer. This document is the specification a backend must satisfy to sit under it. It exists
so that an `radiant` author can implement the whole contract **without ever opening
`sdk-ant`** — which is not a convenience, it is the clean-room boundary. Reading sdk-ant
contaminates the rebuild retroactively and there is no way to prove afterwards that it did not
happen. See `docs/decisions/0002-clean-room-policy.md`.

Division of labour between this file and the header:

- **`<radiant/radiant_ant.h>` is normative for per-function semantics.** Every function carries its
  purpose, parameters, return convention and permitted error codes in a comment directly above
  it. Do not re-state that here; it rots.
- **This document is normative for coverage, ordering, threading and the mapping.** Things that
  are true *between* functions, or true of the set as a whole, live here because a header comment
  is the wrong place to say them once.

```
apps/common/ant_serial_bridge.c        unchanged logic; speaks only RADIANT_WIRE_* / radiant_ant_*
        |
   <radiant/radiant_ant.h>             the 50-function contract     (radiant_ant_*)
   radiant/include/radiant/radiant_wire.h              the protocol constants       (RADIANT_WIRE_*)
        |
   +----+--------------+---------------------+
   |                   |                     |
ant_radio_sdk_ant.c  ant_radio_stub.c    radiant/
(forwarders +        (the seam's          (clean-room stack;
 BUILD_ASSERTs)       cheapest proof)      this doc is its spec)
```

---

## Conventions that apply to every function

### 1. The return value is the wire byte

`radiant_ant_err_t` is a `uint8_t`, and the value **is** the response code the serial protocol puts on
the wire. `0` is `RADIANT_WIRE_RESPONSE_NO_ERROR`; anything else is one of the `RADIANT_WIRE_*` channel response
codes and the bridge forwards it to the host unchanged.

A backend must not invent an error space. A code the host does not recognise is worse than a
wrong-but-valid one, because a host library typically hangs waiting for a reply it can parse
rather than reporting a failure.

sdk-ant is the exception, and the shim absorbs it: its `ant_err_t` carries a wide NRF error offset,
which `apps/common/ant_serial_bridge.c` already narrows with a `(uint8_t)` cast at three sites
(`:406`, `:495`, `:885`) before the value reaches the wire. Wave 2 moves that narrowing into
`ant_radio_sdk_ant.c`, one place instead of three. Behaviour is unchanged — the offset is a
multiple of 256, which is exactly why the existing casts work — and the bridge's casts go away.

### 2. Error sets are permitted sets, not a case-by-case promise

Each function's header comment lists the codes it may return. That list bounds what a backend is
allowed to say. It deliberately does **not** pin which case produces which code, because that is
pinned empirically instead: `tools/ant_conformance.py` drives every message the dispatcher
implements, valid and malformed, records the replies to `.antser`, and the A/B pass condition is a
**byte-identical diff** against the sdk-ant capture. That is a stronger check than any prose, and
it is the reason the prose does not try.

Two distinctions in the sets are load-bearing and observable from the host, so they are called out
here rather than left to be discovered:

- **Bad channel number is `RADIANT_WIRE_INVALID_MESSAGE`; bad parameter is
  `RADIANT_WIRE_INVALID_PARAMETER_PROVIDED`.** `radiant_ant_sdu_mask_config()` returns both, one per argument
  (`apps/common/ant_radio_stub.c:578-592` is the existing reference behaviour).
- **`RADIANT_WIRE_CHANNEL_IN_WRONG_STATE` is for a channel that exists in the wrong state;
  `RADIANT_WIRE_CHANNEL_NOT_OPENED` is for a data-transfer call on a channel that is not on air.** They
  are not interchangeable.

### 3. `const` on the way in, ownership only for burst

Everything that takes a buffer the callee reads takes `const uint8_t *` and **copies before
returning**. sdk-ant's equivalents take non-`const` pointers, which forces seven `(uint8_t *)`
casts in the bridge today; those casts disappear in Wave 1 and reappear as one cast per forwarder
in the shim — **seven there too, not eight.** Eight contract functions take a `const uint8_t *`,
but `ant_network_address_set()` is the one sdk-ant function that already takes a `const` pointer,
so its forwarder needs no cast either. The shim is the right place for all of them.

`radiant_ant_burst_tx()` is the single exception in both respects: its buffer is non-`const` (a backend
may transform a block in place) and ownership transfers. See the burst contract below.

### 4. Naming is a mechanical prefix swap, with exactly two exceptions

Every name is `ant_` → `radiant_ant_`, so Wave 2 can rename by substitution and any omission becomes a
link error rather than a silent behaviour change. Two names are not:

| sdk-ant | ours | why |
|---|---|---|
| `ant_pending_transmit()` | `radiant_ant_pending_transmit_get()` | A verb-less name sitting next to `radiant_ant_pending_transmit_clear()` is how a getter gets called for its side effects. |
| `ant_burst_handler_request()` | `radiant_ant_burst_tx()` | It belongs with `radiant_ant_broadcast_message_tx()` and `radiant_ant_acknowledge_message_tx()`, and it is the one function with a buffer-ownership contract — its name should not be the vaguest in the file. |

### 5. Sizes come from `radiant_wire.h`

The header names sizes as `RADIANT_WIRE_*` constants rather than defining competing ones, because two
sources of truth for a buffer length is how a reply gets framed one byte short. `radiant_ant.h`
defines exactly three constants of its own — the `RADIANT_ANT_BURST_SEGMENT_*` flags — and they are the
one thing here with no wire counterpart at all: the serial protocol carries a burst *sequence*
number in the top bits of the channel byte and the bridge derives the segment from it, so the
values never appear on air. The shim `BUILD_ASSERT`s them against sdk-ant's `BURST_SEGMENT_*`
anyway, and that is not a formality: `RADIANT_ANT_BURST_SEGMENT_END` was `0x04` on an assumption about
the bit pattern the bridge assembles, sdk-ant's is `0x02`, and forwarding the wrong one marks the
last block of every burst as a middle block — no `TRANSFER_TX_COMPLETED`, and a 1000 ms semaphore
timeout per transfer. It is `0x02` now and the shim forwards the byte untouched.

### 5a. The three constants the stub had to guess, and what they turned out to be

Three values the stub needs are Nordic extensions whose numbers appear nowhere in this
repository, so `protocol/ant_wire.yaml` recorded them as unresolved and
[`apps/common/ant_radio_stub.c`](../apps/common/ant_radio_stub.c) carried local placeholders. The rule behind the
placeholders is K1's and it is worth keeping: **an unresolved constant may never appear, even
conditionally, in a file that has to build with no sdk-ant present** — a `#define` reaching into
sdk-ant from the stub would reintroduce exactly the dependency the stub exists to disprove, and it
would do it invisibly. The shim recovered all three; they are generated into `radiant/include/radiant/radiant_wire.h` now
and the placeholders are gone.

| Constant | Placeholder | Real value | Consequence of the guess |
|---|---|---|---|
| `RADIANT_WIRE_SDU_MASK_ACK_CONFIG_BIT` | `0x80` | `0x80` | none — guessed correctly |
| `RADIANT_WIRE_MAX_SUPPORTED_ENCRYPTION_MODE` | `1` | `2` | a stub build under-reported the supported encryption mode and refused `radiant_ant_crypto_channel_enable(.., 2, ..)` |
| `RADIANT_WIRE_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE` | `2`, behind an `#ifdef` on the generated name | `4` | none in practice — the `#ifdef` tracked the generated constant the moment it landed, and the LEN byte on that reply is the bridge's, not the stub's |

The third one is the one earlier revisions of this document missed: it was never listed alongside
the other two, even though the stub guarded it the same way — at what was
`apps/common/ant_radio_stub.c:88-93` until the placeholders retired — and used it to bound a `memset`.
Two of three guesses were right,
which is the argument for the rule rather than against it — the one that was wrong was wrong
silently, and only the shim could tell.

**Two size constants in `radiant/include/radiant/radiant_wire.h` are wrong today and must not be used to size a buffer
until they are fixed.** `RADIANT_WIRE_MAX_SIZE_VALUE` is generated as 20, derived from the longest
*inbound* message the parser sees (`MESG_SET_ENCRYPT_INFO` with 19 bytes of custom user data), and
`RADIANT_WIRE_MAX_FRAME_SIZE` follows it at 24. But an advanced-burst data message is one channel byte plus
`RADIANT_WIRE_ADV_BURST_BLOCK_MAX` (24) bytes, so the LEN byte reaches **25**, and
`src/usb_ant_class.c:30` independently states the maximum is 38 with a 42-byte frame. Sizing the
parser's `msg_body[]` from 20 truncates every 24-byte burst packet silently; sizing a frame buffer
from 24 writes the checksum past the end. See the change request in this wave's report — the fix
belongs in `protocol/ant_wire.yaml`, which this document does not own.

---

## The event path is inverted

sdk-ant delivers events by calling a callback registered with `ant_cb_register()`, passing an
`ant_evt_t` that is a four-level nested union. The bridge reaches into it at
`apps/common/ant_serial_bridge.c:990-992` for three values — `ANT_MESSAGE_ucSize`, `ANT_MESSAGE_ucMesgID`,
`ANT_MESSAGE_aucMesgData` — and uses nothing else. That union's layout is not ours to depend on,
and it is the hardest coupling in the bridge.

It is replaced by a flat structure the **bridge** implements and the **backend** calls:

```c
struct radiant_ant_msg {
        uint8_t        id;    /* ANT message ID; goes on the wire unchanged */
        uint8_t        len;   /* becomes the frame's LEN byte: channel byte + payload */
        const uint8_t *data;  /* the body, wire-shaped; valid only during the call */
};

void radiant_ant_on_message(const struct radiant_ant_msg *msg);
```

Three fields, no unions, explicit widths, and it is deliberately wire-shaped rather than
semantically decomposed: the bridge's entire job for an inbound event is to wrap the body in
`[0xA4][len][id][data...][xor]`, so any field beyond these three is a field that can disagree with
the bytes.

**There is no registration call.** `radiant_ant_on_message()` is resolved at link time and exactly one
translation unit in the image defines it — `apps/common/ant_serial_bridge.c` in firmware, a test double in
`radiant/tests/`. `ant_cb_register()` therefore has no counterpart and is the one symbol from
the stub with no `radiant_ant_*` equivalent.

Constraints on a backend calling it, restated from the header because they are ordering facts:

1. Never before `radiant_ant_init()` has returned 0.
2. `msg` and `msg->data` are non-NULL and readable for the duration of the call, and only for the
   duration of the call. The backend may reuse or free the buffer the instant the call returns; a
   pointer into a DMA buffer the radio could rewrite mid-call is a contract violation.
3. Event delivery is serialised by the backend — the bridge is never called concurrently with
   itself, and it relies on that: it does not lock around the burst semaphore.
4. The implementation may not block; it may run on the backend's thread, work queue or callback
   context. The bridge honours this by queueing to a writer thread rather than sending inline.
5. Three `RADIANT_WIRE_EVENT_*` codes carry buffer-ownership meaning as well as status. See below.

---

## Buffer ownership for burst — the expensive one

`apps/common/ant_serial_bridge.c:452-502` holds **one** static 24-byte block behind a binary semaphore.
Each burst packet from the host takes the semaphore, is `memcpy`'d into that block, and the block
is handed to `radiant_ant_burst_tx()`. The semaphore is given back in exactly three places, all in the
inbound event path (`:1002-1019`):

| Event | Meaning |
|---|---|
| `RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK` | done with this block, send the next one |
| `RADIANT_WIRE_EVENT_TRANSFER_TX_COMPLETED` | the whole transfer finished |
| `RADIANT_WIRE_EVENT_TRANSFER_TX_FAILED` | the whole transfer failed |

plus a synchronous release when `radiant_ant_burst_tx()` returns non-zero, because a rejected block was
never accepted.

**The contract, as five obligations on the backend:**

- **B1** — zero return means the backend owns `data` until it raises one of the three events.
- **B2** — non-zero return means the backend owns nothing: do not touch `data`, do not raise an
  event for it.
- **B3** — `NEXT_DATA_BLOCK` **exactly once per accepted block**, for every accepted block that is
  not the last of the transfer.
- **B4** — the last block (the one whose segment carries `RADIANT_ANT_BURST_SEGMENT_END`) is released by
  `TX_COMPLETED` or `TX_FAILED` instead, exactly once, and must not also produce
  `NEXT_DATA_BLOCK`.
- **B5** — raising `NEXT_DATA_BLOCK` for a block that was never accepted is worse than omitting
  it: it returns a semaphore the bridge still holds for a *different* block, and the next host
  packet overwrites a buffer the radio is transmitting from.

**Why this gets its own section.** Missing B3 does not fail loudly. The bridge's `k_sem_take()`
waits its full 1000 ms timeout, then answers the host with `RADIANT_WIRE_TRANSFER_IN_PROGRESS` — so a
burst does not break, it stalls **one second per packet** and then reports a plausible-looking
error. An ANT-FS transfer that should take two seconds takes ten minutes, and nothing in the
build, the log or the host library says why.

**It must be unit-tested, and the test is specified here rather than left to judgement.** Drive a
multi-block transfer against the mock radio and assert that the release-event count equals the
accepted-block count, across three paths:

1. success — N blocks accepted, N−1 `NEXT_DATA_BLOCK` plus one `TX_COMPLETED`;
2. mid-transfer failure — k blocks accepted, k−1 `NEXT_DATA_BLOCK` plus one `TX_FAILED`;
3. rejected block — `radiant_ant_burst_tx()` returns non-zero and **no** event is raised for it.

`radiant/tests` owns this. It runs on Linux CI and on any Linux machine; `native_sim` does not build
on Windows (no host C compiler, no QEMU), where `scripts/run_ztest_hw.ps1` runs the suites on a DK.

`NEXT_DATA_BLOCK` never reaches the host — the bridge consumes it and returns
(`apps/common/ant_serial_bridge.c:1005-1011`). A real ANT stick frames bursts itself and never puts it on
the wire.

---

## `radiant_ant_channel_open()` is a convenience; the offset form is the real one

sdk-ant makes `ant_channel_open()` a macro forwarding to `ant_channel_open_with_offset()`, which
is why `apps/common/ant_radio_stub.c:365` defines only the latter. The split is reproduced:

```c
radiant_ant_err_t radiant_ant_channel_open_with_offset(uint8_t channel, uint16_t offset);

static inline radiant_ant_err_t radiant_ant_channel_open(uint8_t channel)
{
        return radiant_ant_channel_open_with_offset(channel, 0);
}
```

A backend implements exactly one function. `static inline` rather than a macro so the argument is
type-checked and cannot be double-evaluated. The serial protocol's `MESG_OPEN_CHANNEL` carries no
offset, so the bridge only ever calls the convenience form; the offset form exists for a master
phasing its slot away from another channel's, and for `radiant`'s own scheduler.

---

## Ordering and state — what a backend may assume about call sequence

The bridge does not enforce ordering; it forwards what the host sends. A backend must therefore
police its own state machine and return `RADIANT_WIRE_CHANNEL_IN_WRONG_STATE` rather than misbehaving.

```
UNASSIGNED --radiant_ant_channel_assign()--> ASSIGNED --radiant_ant_channel_open*()--> SEARCHING
     ^                                   |  ^                               |
     |                                   |  |                          (acquires)
     +---radiant_ant_channel_unassign()---------+  +---radiant_ant_channel_close()---- TRACKING
```

- **Configuration is legal only on an ASSIGNED, closed channel.** `radiant_ant_channel_id_set()`,
  `radiant_ant_channel_period_set()`, `radiant_ant_channel_radio_freq_set()`, `radiant_ant_channel_radio_crc_mode_set()`
  and `radiant_ant_auto_freq_hop_table_set()` all retune the radio, and retuning underneath a live host
  session is worse than refusing.
- **`radiant_ant_channel_open_with_offset()` requires the channel ID to have been set** — otherwise
  `RADIANT_WIRE_CHANNEL_ID_NOT_SET`.
- **`radiant_ant_channel_unassign()` requires the channel to be closed.** Silently closing it first would
  swallow the `RADIANT_WIRE_EVENT_CHANNEL_CLOSED` the host is waiting on.
- **`radiant_ant_channel_ext_assign_set()` (added 2026-11-21, not in the stub table below) requires an
  assigned, closed channel** and changes only the extended assignment byte; ID, RF, period, ID
  list, search settings and security context stay. The bridge's Open RX Scan Mode (0x5B) uses it
  so a keyed channel 0 keeps its keys and policy. A backend without an in-place change returns
  `RADIANT_WIRE_INVALID_MESSAGE`, and the bridge then falls back to unassign and reassign. sdk-ant and the
  stub do that, so sdk-ant's conformance transcript is unchanged.
- **`radiant_ant_channel_radio_tx_power_set()` requires an assigned channel, and that is the constraint
  that shapes the bridge.** Hosts set power *before* assigning anything, so the bridge holds the
  requested level in `tx_power[]` and reapplies it after each successful assign
  (`apps/common/ant_serial_bridge.c:142-197`, `:541`). A backend does not replicate that and may reject an
  unassigned channel.
- **`radiant_ant_init()` runs before the transport is enabled**, because ANT (and MPSL under it) must own
  HFCLK startup before USB asks for the HFXO. Getting that order wrong does not fail cleanly — USB
  hangs or fails to enumerate. `src/main.c:128` is the call site and the ordering comment above it
  is the reason.
- **`radiant_ant_stack_reset()` is synchronous and total**: every channel closed and unassigned, every
  key forgotten, library configuration back to zero. Channel-closed events raised as a side effect
  are still delivered; the bridge discards them until the startup message, because a real stick emits
  nothing between the reset command and the startup message.
- **Advanced burst must be enabled before `radiant_ant_burst_tx()` accepts a 16- or 24-byte block.**
  Until then, reject anything over 8 — a dongle that takes 24-byte blocks and can never send one
  is worse than one that refuses them.

### Threading

| Path | Context | Rule |
|---|---|---|
| every `radiant_ant_*` call | the bridge's parser thread, priority 5, 2 KB stack (`apps/common/ant_serial_bridge.c:66-70`) | may block briefly; must not block indefinitely, since the same thread drains the host's input |
| `radiant_ant_on_message()` | the backend's own thread / work queue / callback | must not block; serialised by the backend, never re-entrant |
| `radiant_ant_burst_tx()` + its release events | crosses the two | the semaphore in the bridge is the only synchronisation; there is no lock |

A backend must not call any `radiant_ant_*` function itself, and must not call `radiant_ant_on_message()`
recursively from inside a call the bridge made.

---

## Permanent limitations — documented, not to-do

**`radiant_ant_network_address_set()` for an unknown key returns `RADIANT_WIRE_INVALID_PARAMETER_PROVIDED`, and
always will.** The on-air network address is derived from the 8-byte key by a function that is not
public. This project will not recover it: fitting it from samples is the activity most likely to
be characterised as reverse-engineering, and the clean-room policy rules it out.

A clean-room backend therefore holds a small table of key → address pairs, seeded with the
published ANT+ pair (`B9A521FBBD72C345` → network address `A6 C5`), and refuses anything else.
Every ANT+ profile, every fitness app and every shipping sensor uses that one key, so the
limitation costs nothing on the path anyone runs. It may be extended by *observing RF emissions*
from a shipping sdk-ant master, subject to confirming the licence position first — never by
fitting the function.

Two consequences worth writing down:

- The sdk-ant backend has no such limitation, because `libant.a` computes the address. **A/B
  comparisons between backends must use the ANT+ key**, or they are comparing a table against an
  algorithm rather than comparing two radios.
- This is a capability difference between backends and belongs in `docs/backends.md`, not in an
  issue tracker.

**ANT+'s own AES-CTR encryption is not expected of a clean-room backend.** `ANT_DLL.dll` exports no
encryption call at all, so no Windows host can reach it; the mode is malleable and unauthenticated;
and it reserves RAM shared with the plain channels. The honest implementation is to accept the
reads, return `RADIANT_WIRE_INVALID_PARAMETER_PROVIDED` from `radiant_ant_crypto_channel_enable()` for any
non-zero mode, and say so. The four crypto functions still have to *exist* — a backend stands in
for `libant.a`, which exports them regardless — and `radiant_ant_capabilities_get()` still advertises the
bit, because the bit describes what the radio layer can do rather than what the bridge exposes.
What replaces it is a separate, optional design; see `docs/radiant-security.md`.

## Proximity search — what a backend must do

`radiant_ant_prox_search_set(channel, threshold, custom)` follows ANT's proximity search (ANT
Message Protocol and Usage Rev 5.1, sections 6.4 and 9.5.2.22; AN12 Proximity Search; Nordic's
public S212 API documentation for the C form):

- **threshold** 0 = off, 1..10 = bins with 1 the closest, `0x80` = Nordic's custom form. A bin
  accepts anything at or above its level. Out-of-range behaviour is unpublished; the radiant
  backend refuses 11..`0x7F` and `0x81`..`0xFF` with `RADIANT_WIRE_INVALID_PARAMETER_PROVIDED`.
- **custom** is read only with `0x80`. Its encoding is unpublished; the radiant backend takes it
  as a positive magnitude (60 = -60 dBm) `[inferred]` and refuses 0 and anything above 127. The
  serial message has no custom byte, so a host can never reach this form (`docs/ant-serial-protocol.md`).
- **One-shot**: a successful acquisition clears the threshold, a search timeout keeps it, a
  background scan keeps it, and tracking never filters on it.
- **Never store what cannot run**: a backend with no RSSI on search frames refuses any non-zero
  threshold and does not advertise `CAPABILITIES_PROX_SEARCH_ENABLED` (`docs/gotchas.md`).
- The RSSI extended field's byte 2 reports the threshold in force as a signed dBm while it is set.
  Off is `0x00` on radiant today, where the ANT documents give -128; that is an open owner call.

The sdk-ant backend passes all three straight to `ant_prox_search_set()`, whose bins are Nordic's
own. The radiant bin levels are Nordic's approximate nRF51 figures, so a bin means roughly, not
exactly, the same on both.

---

## Coverage — every symbol in the stub, and where it went

The stub is the ground truth for coverage: it has to define every entry point `libant.a` would
otherwise supply, or the stub build does not link. It defines **51** functions. Fifty map to an
`radiant_ant_*` declaration; one (`ant_cb_register`) is deleted by the event inversion.

Columns: the symbol, its definition line, the `radiant_ant_*` counterpart, and the call site. **The
call-site column is the critical-path marker**: a bare `:NNN` is a line in
`apps/common/ant_serial_bridge.c` that a host message reaches today. Only one row is not directly called
(`radiant_ant_channel_open_with_offset`, reached through its wrapper), and only three are conditional
(the encryption writes, behind `CONFIG_ANT_DONGLE_ENCRYPTION`, which is off by default).

**The line-number column is `src/ant_stub.c` — the file as it stood *before* the Wave 2 rename to
[`apps/common/ant_radio_stub.c`](../apps/common/ant_radio_stub.c).** It is deliberately not renumbered against the
current file. These numbers are the audit trail for how the table was built: they say which
definition each row was read off, in the file that was read, and renumbering them against a file
that has since been edited would quietly turn evidence into decoration. The symbol names are what
to search by; the link check in the *Checked by* line above is what keeps the coverage claim
honest, not the line numbers.

### Init and lifecycle

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_init` | 70 | `radiant_ant_init` | `src/main.c:128` |
| `ant_cb_register` | 76 | **dropped** — replaced by `radiant_ant_on_message()` | was `ant_serial_bridge.c:1060` |
| `ant_stack_reset` | 82 | `radiant_ant_stack_reset` | `:424` |

### Channel lifecycle

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_channel_assign` | 259 | `radiant_ant_channel_assign` | `:535` |
| `ant_channel_unassign` | 332 | `radiant_ant_channel_unassign` | `:641` |
| `ant_channel_open_with_offset` | 318 | `radiant_ant_channel_open_with_offset` | via `radiant_ant_channel_open()`, `:627` |
| — (`ant_channel_open` is a macro) | — | `radiant_ant_channel_open` (`static inline`) | `:627` |
| `ant_channel_close` | 326 | `radiant_ant_channel_close` | `:634` |

### Channel configuration

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_channel_id_set` | 270 | `radiant_ant_channel_id_set` | `:551` |
| `ant_channel_period_set` | 287 | `radiant_ant_channel_period_set` | `:567` |
| `ant_channel_radio_freq_set` | 280 | `radiant_ant_channel_radio_freq_set` | `:558` |
| `ant_channel_radio_tx_power_set` | 309 | `radiant_ant_channel_radio_tx_power_set` | `:195`, `:597`, `:618` |
| `ant_channel_search_timeout_set` | 294 | `radiant_ant_channel_search_timeout_set` | `:574` |
| `ant_channel_low_priority_rx_search_timeout_set` | 301 | `radiant_ant_channel_low_priority_rx_search_timeout_set` | `:581` |
| `ant_channel_radio_crc_mode_set` | 422 | `radiant_ant_channel_radio_crc_mode_set` | `:716` |
| `ant_auto_freq_hop_table_set` | 412 | `radiant_ant_auto_freq_hop_table_set` | `:708` |
| `ant_enhanced_channel_spacing_enable` | 538 | `radiant_ant_enhanced_channel_spacing_enable` | `:840` |

### Data transfer

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_broadcast_message_tx` | 338 | `radiant_ant_broadcast_message_tx` | `:860` |
| `ant_acknowledge_message_tx` | 346 | `radiant_ant_acknowledge_message_tx` | `:868` |
| `ant_burst_handler_request` | 354 | **`radiant_ant_burst_tx`** (renamed) | `:491` |
| `ant_pending_transmit` | 174 | **`radiant_ant_pending_transmit_get`** (renamed) | `:298` |
| `ant_pending_transmit_clear` | 544 | `radiant_ant_pending_transmit_clear` | `:849` |

### Search

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_search_waveform_set` | 382 | `radiant_ant_search_waveform_set` | `:677` |
| `ant_prox_search_set` | 389 | `radiant_ant_prox_search_set` | `:686` |
| `ant_search_channel_priority_set` | 398 | `radiant_ant_search_channel_priority_set` | `:693` |
| `ant_search_channel_priority_get` | 167 | `radiant_ant_search_channel_priority_get` | `:288` |
| `ant_active_search_sharing_cycles_set` | 405 | `radiant_ant_active_search_sharing_cycles_set` | `:700` |
| `ant_active_search_sharing_cycles_get` | 187 | `radiant_ant_active_search_sharing_cycles_get` | `:318` |
| `ant_id_list_add` | 429 | `radiant_ant_id_list_add` | `:723` |
| `ant_id_list_config` | 437 | `radiant_ant_id_list_config` | `:731` |

### Network and key

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_network_address_set` | 252 | `radiant_ant_network_address_set` | `:527` |

### Library configuration

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_lib_config_set` | 370 | `radiant_ant_lib_config_set` | `:652`, `:665` |
| `ant_lib_config_clear` | 376 | `radiant_ant_lib_config_clear` | `:653`, `:667` |
| `ant_lib_config_get` | 181 | `radiant_ant_lib_config_get` | `:308` |
| `ant_event_filtering_set` | 446 | `radiant_ant_event_filtering_set` | `:740` |
| `ant_event_filtering_get` | 194 | `radiant_ant_event_filtering_get` | `:328` |
| `ant_adv_burst_config_set` | 452 | `radiant_ant_adv_burst_config_set` | `:755` |
| `ant_adv_burst_config_get` | 204 | `radiant_ant_adv_burst_config_get` | `:339` |
| `ant_sdu_mask_set` | 466 | `radiant_ant_sdu_mask_set` | `:774` |
| `ant_sdu_mask_get` | 218 | `radiant_ant_sdu_mask_get` | `:357` |
| `ant_sdu_mask_config` | 522 | `radiant_ant_sdu_mask_config` | `:765` |

### Status and queries

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_capabilities_get` | 93 | `radiant_ant_capabilities_get` | `:216` |
| `ant_version_get` | 122 | `radiant_ant_version_get` | `:225` |
| `ant_channel_status_get` | 129 | `radiant_ant_channel_status_get` | `:234` |
| `ant_channel_id_get` | 136 | `radiant_ant_channel_id_get` | `:244` |
| `ant_channel_period_get` | 146 | `radiant_ant_channel_period_get` | `:256` |
| `ant_channel_radio_freq_get` | 153 | `radiant_ant_channel_radio_freq_get` | `:268` |
| `ant_channel_radio_crc_mode_get` | 160 | `radiant_ant_channel_radio_crc_mode_get` | `:278` |

### Encryption

All four exist unconditionally. The three **writes** are called only under
`CONFIG_ANT_DONGLE_ENCRYPTION`, which is off by default; the read is always compiled in.

| symbol | line in `src/ant_stub.c` (pre-rename) | `radiant_ant_*` | called from |
|---|---|---|---|
| `ant_crypto_channel_enable` | 479 | `radiant_ant_crypto_channel_enable` | `:787` (`CONFIG_ANT_DONGLE_ENCRYPTION`) |
| `ant_crypto_key_set` | 492 | `radiant_ant_crypto_key_set` | `:798` (`CONFIG_ANT_DONGLE_ENCRYPTION`) |
| `ant_crypto_info_set` | 501 | `radiant_ant_crypto_info_set` | `:830` (`CONFIG_ANT_DONGLE_ENCRYPTION`) |
| `ant_crypto_info_get` | 231 | `radiant_ant_crypto_info_get` | `:373` |

### Totals

| | count |
|---|---|
| functions defined in the stub (`src/ant_stub.c`, pre-rename) | 51 |
| mapped to an `radiant_ant_*` declaration | 50 |
| deliberately dropped (`ant_cb_register`) | 1 |
| `radiant_ant_*` backend functions declared | 50 |
| plus `radiant_ant_channel_open()`, a `static inline` convenience | 1 |
| plus `radiant_ant_on_message()`, implemented by the bridge | 1 |
| called directly by the bridge or `main()` | 49 |
| reached only through the convenience wrapper | 1 (`radiant_ant_channel_open_with_offset`) |

**Nothing in this contract exists only for completeness.** Every one of the 50 is on a path a host
message reaches, which is a stronger statement than the plan's estimate and worth knowing when
scoping `radiant`: there is no dead weight to defer.

### What is deliberately *not* here

- **`ant_cb_register()`** — deleted by the event inversion. There is no registration step; the
  backend calls `radiant_ant_on_message()`, resolved at link time.
- **`ant_evt_t` and its four nested unions** — replaced by `struct radiant_ant_msg`. The bridge read three
  fields out of it and needs nothing else.
- **The eight remaining `libant.a` exports.** `libant.a` exports 59 public functions; the stub
  defines 51 and the bridge references fewer still. The other eight are unknown to this project by
  construction — nothing in the repo names them, so nothing has ever had to link against them, and
  finding out what they are would mean inspecting the binary, which the clean-room policy forbids
  absolutely. If Wave 2's shim turns out to need one, that is new information and belongs in a
  change request against this document, not in a quiet addition to the header. Until then the
  contract is complete with respect to everything this project does.
- **nRF5340 dual-core support.** `apps/common/ant_serial_bridge.c:43` branches on `CONFIG_ANT_NP_HOST` to
  pick a different init header, because the nRF5340's RADIO lives on the network core and sdk-ant
  needs an RPC subsystem for it. v1 targets single-core parts only (nRF52840, nRF54L15), so
  `radiant_ant.h` has no equivalent branch and a backend need not provide one. The `#if` disappears
  from the bridge in Wave 1.

---

## Notes for the Wave 2 implementers

**`sdk-ant-shim` (`apps/common/ant_radio_sdk_ant.c`)** — the only agent in the project permitted to read
sdk-ant, which is what makes the boundary a scope rule rather than a promise.

- Fifty one-line forwarders. Four things are not one-liners:
  1. **The error narrowing** — `return (radiant_ant_err_t)(sdk_err & 0xFFu);` per forwarder. Do it here,
     not in the bridge.
  2. **The `const` casts** — our contract is `const uint8_t *`, sdk-ant's is `uint8_t *`. One cast
     per forwarder; **seven** in total. Eight contract functions take a `const uint8_t *`, but
     `ant_network_address_set()`'s sdk-ant counterpart is already `const`.
  3. **`radiant_ant_channel_open_with_offset()`** forwards to sdk-ant's `..._with_offset` function, not to
     its `ant_channel_open` macro.
  4. **The event normaliser** — register a callback with `ant_cb_register()` at init that fills a
     `struct radiant_ant_msg` from `ANT_MESSAGE_ucSize` / `ucMesgID` / `aucMesgData` and calls
     `radiant_ant_on_message()`. About ten lines. The `struct radiant_ant_msg` may be a stack local; its `data`
     pointer may point straight into the event, since the bridge copies before returning.
- **The `BUILD_ASSERT` block is the point of this file**, not a garnish. Assert every `RADIANT_WIRE_*`
  constant against its `MESG_*` counterpart, the three `RADIANT_ANT_BURST_SEGMENT_*` values against
  sdk-ant's `BURST_SEGMENT_*`, and `RADIANT_WIRE_ADV_BURST_BLOCK_MAX` against sdk-ant's advanced-burst
  maximum.
  If a signature drifts the file stops compiling; if a constant drifts the assert fires. Both
  checks exist only where sdk-ant is present — exactly where they can be checked — and that is
  worth more than the fallback.
- Where a constant genuinely has no sdk-ant counterpart, say so in a comment next to the gap. An
  unasserted constant that looks asserted is the failure mode this block exists to prevent.

**`stub-rename` (`src/ant_stub.c` → `apps/common/ant_radio_stub.c`)**

- Pure prefix swap for 49 of the 51, the two renames in the table above, and delete
  `ant_cb_register()` together with the `stub_evt_cb` static it sets — with the inversion there is
  nothing to register and nothing to call.
- Replace `#include "ant_interface.h"` / `"ant_parameters.h"` / `"ant_init.h"` with
  `#include <radiant/radiant_ant.h>` and `#include <radiant/radiant_wire.h>`. That include set going away is the whole
  point of the exercise.
- `NRF_ANT_SUCCESS` becomes `RADIANT_WIRE_RESPONSE_NO_ERROR`; `NRF_ANT_ERROR_INVALID_PARAMETER_PROVIDED`
  becomes `RADIANT_WIRE_INVALID_PARAMETER_PROVIDED`; `NRF_ANT_ERROR_INVALID_MESSAGE` becomes
  `RADIANT_WIRE_INVALID_MESSAGE`. The offset is gone, not renamed — see the error convention above.
- **Do not move the prose header comment** at the top of the file. It is read at the moment
  someone edits the file, and it explains what the stub is for; docs link to it.
- The existing note at `apps/common/ant_radio_stub.c:410-421` — that the stub never raises
  `EVENT_TRANSFER_NEXT_DATA_BLOCK`, so every burst packet after the first is answered with
  `TRANSFER_IN_PROGRESS` — is now a **documented, deliberate violation of B3** rather than an
  incidental gap. Keep the comment and say which obligation it declines, so the stub reads as an
  illustration of the failure mode rather than as a bug.

**`bridge-rename` (`apps/common/ant_serial_bridge.c`, Wave 1)**

- `ant_cb_register(ant_evt_handler)` at `:1060` goes away. `ant_serial_bridge_init()` then has no
  error to return from that step; it should keep returning `int` and return 0.
- `void ant_evt_handler(ant_evt_t *)` becomes `void radiant_ant_on_message(const struct radiant_ant_msg *msg)`,
  and `:990-992` becomes three plain field reads. Nothing else in that function changes.
- Nothing may call `radiant_ant_on_message()` before `radiant_ant_init()` returns, and `radiant_ant_init()` runs at
  `src/main.c:128` — *before* `ant_serial_bridge_init()` at `:140`. The handler only touches
  statically-initialised state (an `atomic_t`, a `K_SEM_DEFINE`, and a queueing send), so this is
  already safe, but it is safe by accident today and should be safe on purpose: do not add anything
  to `radiant_ant_on_message()` that depends on the bridge thread existing.
- The seven `(uint8_t *)` casts on the way into the radio can go: `:723`, `:755`, `:774`, `:798`,
  `:831`, `:861` and `:869`.
- The three `(uint8_t)err` narrowings at `:406`, `:495` and `:885` can go: `radiant_ant_err_t` is already
  the wire byte.
