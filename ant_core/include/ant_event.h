/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_event.h - the event queue, and the assembly of a received frame into the
 * flat struct antr_msg the serial bridge implements.
 *
 * Provenance: clean-room. Written from src/ant_radio.h and
 * docs/sdk-ant-contract.md (the inverted event path, the burst
 * buffer-ownership obligations and the threading table), from
 * src/ant_wire.h - generated from protocol/ant_wire.yaml, never hand-edited -
 * for every wire constant, from ant_core/include/ant_radio_hal.h for the
 * t_sync contract and the callback rules, from ant_core/include/ant_frame.h
 * for the channel ID, and from tools/ant_verify.py's extended_fields() for the
 * decoder this module has to be the exact inverse of. Nothing here derives
 * from sdk-ant, from libant.a, from disassembly of any binary, or from any
 * adopter-gated ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this module is
 * ---------------------------------------------------------------------------
 * Everything that travels chip -> host goes through here, and leaves through
 * exactly one call: antr_on_message(). The module is two halves that are worth
 * keeping separate in your head.
 *
 *   THE QUEUE. Radio callbacks run in the backend's interrupt context, at the
 *   highest priority the backend uses, and must not block, must not do work
 *   proportional to anything, and MUST NOT retain the received body - it is a
 *   DMA buffer that is reused the instant the callback returns
 *   (ant_radio_hal.h; ant_core/tests/fake_radio.c overwrites it with 0xDD to
 *   make a violation reproducible). So a callback copies what it needs into a
 *   ring here and returns. A thread drains the ring later.
 *
 *   THE ASSEMBLY. A queued receive becomes the bytes the host will see:
 *   [channel][payload...] plus, when library configuration asks for them, a
 *   flag byte and up to three appended extended fields. That assembly is the
 *   inverse of tools/ant_verify.py's extended_fields(), and getting the field
 *   order or the flag byte wrong is a Tier 1 byte-diff failure rather than a
 *   cosmetic one.
 *
 * The split is why the drain is a separate call rather than a callback: the
 * expensive half must not run in the ISR, and the ISR half must not allocate,
 * loop or format anything.
 *
 * ---------------------------------------------------------------------------
 * Library configuration 0xE0 is THREE fields, not one
 * ---------------------------------------------------------------------------
 * Every tool in tools/ requests ANTW_LIB_CONFIG_ALL_EXT_FIELDS (0xE0) -
 * channel ID + RSSI + RX timestamp - not the device-ID-only 0x80. All three
 * are assembled here, in this order, each present only if its bit is set:
 *
 *   byte 0 of the tail   flag byte: ANTW_EXT_FLAG_CHANNEL_ID   0x80
 *                                   ANTW_EXT_FLAG_RSSI         0x40
 *                                   ANTW_EXT_FLAG_RX_TIMESTAMP 0x20
 *   channel ID   4 bytes  devnum lo, devnum hi, device type, transmission type
 *   RSSI         3 bytes  measurement type, value, threshold configuration
 *   RX timestamp 2 bytes  32768 Hz counter, little endian
 *
 * The order is fixed but the *offsets* are not: a field's position depends on
 * which earlier fields turned up. That is the whole reason the flag byte
 * exists, and reading a later field at a fixed offset is the mistake
 * extended_fields() is written to avoid - so this module must never emit a
 * placeholder for a field it is not reporting.
 *
 * The device type keeps its pairing bit (bit 7). Masking it here would lose
 * information the host is entitled to; ant_verify.py and ant_scan.py mask it
 * themselves when they want the bare type.
 *
 * ---------------------------------------------------------------------------
 * The RX timestamp comes from t_sync, and from nothing else
 * ---------------------------------------------------------------------------
 * This is the subtlest requirement in the module and the one with no
 * functional symptom when it is wrong.
 *
 * ant_rx_event.t_sync is defined by ant_radio_hal.h as the instant the last
 * bit of the on-air address was at the antenna, on the backend's own
 * microsecond timebase, with each backend applying its measured demodulation
 * correction. That is the number the host must get. A reading of
 * ant_radio_now() taken when the queue is drained, or a host-side clock, or a
 * thread-context tick would pass every functional test and destroy the one
 * measurement the field exists for: the A/B `timing` gate reads 0.009 ms on
 * the radio clock against ~2.6 ms through USB jitter on a host clock, and the
 * sub-millisecond multi-sensor fusion capability rests entirely on the former.
 *
 * Two things in this header are shaped by that:
 *
 *   - struct ant_event_rx has NO timestamp field. It carries a pointer to the
 *     HAL event instead, and this module copies t_sync out of it. There is
 *     nowhere to put a wrong number, which is a stronger guarantee than a
 *     comment asking for the right one.
 *   - the conversion to 32768 Hz ticks happens at drain time, but on the
 *     t_sync value captured in the callback. ant_core/tests/src/test_event.c
 *     advances the virtual clock between arrival and drain and asserts the
 *     reported ticks did not move. That test exists for exactly one bug.
 *
 * WHEN THE BACKEND CANNOT STAMP EXACTLY. A backend with
 * caps.has_sync_timestamp false still fills t_sync best-effort and clears
 * ant_rx_event.t_sync_exact. The rule from the HAL header is to *degrade the
 * advertised accuracy* rather than report the worse number as if it were the
 * good one - and the only accuracy this wire format advertises is the presence
 * of the field. So by default an inexact stamp suppresses
 * ANTW_EXT_FLAG_RX_TIMESTAMP for that message: the host sees no timestamp,
 * which is true, instead of a millisecond-scale number in a field documented
 * to carry a microsecond-scale one. The suppression is counted, not silent
 * (ant_event_stats.ts_suppressed), and a bench build that genuinely wants the
 * approximate value opts in with ANT_EVENT_TS_BEST_EFFORT.
 *
 * The same rule applies to RSSI: ant_rx_event.has_rssi false suppresses
 * ANTW_EXT_FLAG_RSSI rather than emitting 0 dBm, which is a physically
 * implausible reading that a host would nonetheless believe.
 *
 * ---------------------------------------------------------------------------
 * Event codes are wire values
 * ---------------------------------------------------------------------------
 * antr_err_t is a uint8_t whose value *is* the byte the serial protocol puts
 * on the wire, and the same is true of every ANTW_EVENT_* code raised through
 * here. A wrong code is wire-visible and tools/ant_conformance.py's byte diff
 * catches it. Nothing in this module invents a value; every one comes from
 * src/ant_wire.h.
 *
 * Three of those codes carry buffer ownership as well as status, and are what
 * release the bridge's single 24-byte burst block
 * (src/ant_serial_bridge.c:452-502):
 *
 *   ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK   exactly once per accepted block that
 *                                         is not the last of the transfer
 *   ANTW_EVENT_TRANSFER_TX_COMPLETED      the transfer finished
 *   ANTW_EVENT_TRANSFER_TX_FAILED         the transfer failed
 *
 * Missing the first does not fail loudly: the bridge's k_sem_take() waits its
 * full 1000 ms and then answers ANTW_TRANSFER_IN_PROGRESS, so an ANT-FS
 * transfer that should take two seconds takes ten minutes and nothing says
 * why. ant_burst.c owns raising them; this module owns the codes and counts
 * each one (ant_event_stats), so the "release count equals accepted-block
 * count" assertion the contract demands can be made against a counter instead
 * of by reading a log.
 *
 * ---------------------------------------------------------------------------
 * Sized for 32 channels and background scan
 * ---------------------------------------------------------------------------
 * 32 is the serial protocol's own ceiling: the burst header keeps the channel
 * number in its low five bits (ANTW_BURST_HEADER_CHANNEL_MASK), so 0..31 is
 * every channel that can be named on the wire at all. At 4 Hz, 32 tracked
 * channels produce ~128 messages per second, and background scan adds every
 * sensor in range on top of that. The queue is sized from that rate rather
 * than from the eight channels libant.a allocates, because a queue depth is
 * exactly the kind of assumption that is expensive to retrofit.
 */

#ifndef ANT_EVENT_H_
#define ANT_EVENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ant_frame.h"      /* struct ant_channel_id */
#include "ant_radio_hal.h"  /* ant_time_t, struct ant_rx_event */

/*
 * The output format. This module exists to produce struct antr_msg and to call
 * antr_on_message(), so the dependency is the point rather than a leak: it is
 * the one place in ant_core that knows the shape of the seam above it.
 * src/ant_radio.h pulls in src/ant_wire.h, which is where every ANTW_* value
 * below comes from.
 */
#include "ant_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes
 *
 * Negative and module-private, on the same terms as ant_frame.h's: these are
 * internal outcomes, not wire codes, and a caller must never forward one to
 * the host. The functions that DO answer a host message - the library
 * configuration setters - return antr_err_t instead, and are marked as such.
 * ---------------------------------------------------------------------------
 */
#define ANT_EVENT_OK        0
#define ANT_EVENT_EINVAL  (-1)  /* null pointer, bad channel, bad length */
#define ANT_EVENT_ENOSPC  (-2)  /* the queue is full; the message was dropped
				 * and ANTW_EVENT_QUE_OVERFLOW will be raised */
#define ANT_EVENT_ECRC    (-3)  /* the HAL event did not carry a verified CRC.
				 * See ant_event_post_rx() - this is a refusal,
				 * not a failure */
#define ANT_EVENT_ESTATE  (-4)  /* posted before ant_event_init(), or a drain
				 * re-entered from inside antr_on_message() */

/* ---------------------------------------------------------------------------
 * Sizing
 * ---------------------------------------------------------------------------
 */

/* Highest channel number expressible on the wire. The burst header's low five
 * bits are the channel, so this is a protocol fact rather than a policy
 * choice, and it is what makes 32 the natural target. */
#define ANT_EVENT_CHANNEL_MAX  ANTW_BURST_HEADER_CHANNEL_MASK

/* Longest payload one queued message can carry. An advanced-burst block is 8,
 * 16 or 24 bytes (ANTW_ADV_BURST_BLOCK_MAX); everything else is 8. */
#define ANT_EVENT_PAYLOAD_MAX  ANTW_ADV_BURST_BLOCK_MAX

/* The body before any extended tail: the channel byte plus the payload. This
 * is what antr_msg.len counts, so it counts the channel byte too. */
#define ANT_EVENT_BODY_MAX     (ANTW_CHANNEL_NUM_SIZE + ANT_EVENT_PAYLOAD_MAX)

/* The extended tail at its longest: flag byte + 4 + 3 + 2. */
#define ANT_EVENT_EXT_MAX      10u

/* The longest message this module can hand to the bridge. */
#define ANT_EVENT_MSG_MAX      (ANT_EVENT_BODY_MAX + ANT_EVENT_EXT_MAX)

/*
 * Queued messages. Must be a power of two - the ring masks rather than
 * divides, because the producer runs in a radio ISR.
 *
 * 64 slots is ~500 ms of headroom at the 128 messages per second that 32
 * tracked channels at 4 Hz produce, and it comfortably absorbs the 51-packet
 * burst Spike B part 2 captured end to end. It costs about 3 KB of static RAM
 * (see struct ant_event_slot in ant_event.c), which is real: libant.a's whole
 * footprint is 2,389 B, for eight channels rather than thirty-two. Override it
 * with -DANT_EVENT_QUEUE_DEPTH=<n> on a build that would rather have the RAM;
 * 32 - one slot per channel - is the lowest value that cannot lose a message
 * to a single slot's worth of simultaneous arrivals.
 */
#ifndef ANT_EVENT_QUEUE_DEPTH
#define ANT_EVENT_QUEUE_DEPTH  64u
#endif

/* Library configuration bits this module implements. Anything outside it is
 * refused by ant_event_lib_config_set() with ANTW_INVALID_PARAMETER_PROVIDED
 * rather than accepted and ignored - a host that believes it configured a
 * field and did not is worse off than one that got an error.
 *
 * ANTW_LIB_CONFIG_RADIO_CONFIG_ALWAYS is in the set and is a no-op by
 * construction: ant_radio_hal.h rule 6 makes radio configuration per-operation
 * already, so "reconfigure on every event" is what the core does whether or
 * not the bit is set. Accepting it is therefore honest, not a pretence.
 *
 * Which bits a real stick refuses is a byte-diff question rather than a
 * reasoned one; tools/ant_conformance.py is what pins it, and narrowing or
 * widening this mask afterwards is a one-line change. */
#define ANT_EVENT_LIB_CONFIG_SUPPORTED                                       \
	((uint8_t)(ANTW_LIB_CONFIG_RADIO_CONFIG_ALWAYS |                     \
		   ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP |                 \
		   ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI |                       \
		   ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID))

/* ---------------------------------------------------------------------------
 * Two hooks the port provides
 *
 * Deliberately undefined here, so that a build which forgets one fails at the
 * link with a named symbol rather than shipping a silent data race or a queue
 * nothing ever drains. ant_core/src/ant_event.c includes no Zephyr header - it
 * compiles as a freestanding translation unit, the same rule ant_frame.c
 * follows - so it cannot reach irq_lock() or a semaphore itself.
 *
 * In the firmware these are two lines in ant_api.c. In the unit tests they are
 * a no-op and a counter (the mock radio is single-threaded and its clock only
 * moves when a test moves it).
 * ---------------------------------------------------------------------------
 */

/*
 * Enter and leave the critical section protecting the ring.
 *
 * The producer may be a radio ISR and the consumer is a thread, so the two
 * index updates need mutual exclusion. Map these onto irq_lock() and
 * irq_unlock(): a mutex is wrong, because the producer side runs in interrupt
 * context and must not block. The section is a handful of instructions plus
 * one 48-byte copy; it never calls out and never loops.
 */
unsigned int ant_event_crit_enter(void);
void         ant_event_crit_exit(unsigned int key);

/*
 * Called once per successfully queued message, possibly from the radio ISR.
 *
 * Its only job is to make the drain run: k_sem_give(), k_work_submit(), or
 * whatever the port uses. It must be O(1), must be safe from interrupt
 * context, and must not call back into this module - the HAL's "queue it and
 * return" rule applies to everything on this path, not only to HAL calls.
 */
void ant_event_wakeup(void);

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/*
 * Bring the module up: empty ring, library configuration and event filter
 * zeroed, statistics cleared, timestamp policy back to the default.
 *
 * Call it from antr_init() BEFORE the radio is enabled. Posting before this
 * returns is ANT_EVENT_ESTATE, which mirrors the rule that antr_on_message()
 * is never called before antr_init() has returned 0.
 */
void ant_event_init(void);

/*
 * What antr_stack_reset() needs: discard every queued message and put the
 * library configuration and the event filter back to zero, without delivering
 * anything.
 *
 * Discarding rather than draining is the correct behaviour and not a
 * shortcut. A real stick emits nothing between the reset command and the
 * startup message, the bridge discards inbound events for 50 ms afterwards for
 * exactly that reason, and draining here would call antr_on_message()
 * recursively from inside an antr_* call, which docs/sdk-ant-contract.md
 * forbids outright. Discarded messages are counted in
 * ant_event_stats.dropped_flush; statistics themselves are not cleared,
 * because a reset in the middle of a session is a thing a test wants to see.
 */
void ant_event_flush(void);

/* ---------------------------------------------------------------------------
 * Library configuration - the three functions ant_api.c forwards
 * ---------------------------------------------------------------------------
 */

/*
 * Turn bits on. Additive: setting one leaves the others alone.
 *
 * Returns ANTW_RESPONSE_NO_ERROR, or ANTW_INVALID_PARAMETER_PROVIDED for any
 * bit outside ANT_EVENT_LIB_CONFIG_SUPPORTED. Nothing is applied when a bit is
 * refused - a partially applied configuration is worse than none, because the
 * host's error handling cannot tell which half took.
 */
antr_err_t ant_event_lib_config_set(uint8_t config);

/*
 * Turn bits off. Accepts anything, including ANTW_LIB_CONFIG_MASK_ALL, which
 * is how the bridge expresses the host's "set the configuration to zero" -
 * there is no separate clear message on the wire. Returns
 * ANTW_RESPONSE_NO_ERROR.
 */
antr_err_t ant_event_lib_config_clear(uint8_t config);

/* The current bits. Returns ANTW_RESPONSE_NO_ERROR; *config is untouched when
 * it is NULL, which is ANTW_INVALID_PARAMETER_PROVIDED. */
antr_err_t ant_event_lib_config_get(uint8_t *config);

/* ---------------------------------------------------------------------------
 * Event filtering - stored, honoured by nobody, and that is deliberate
 *
 * src/ant_radio.h is explicit: a backend that does not filter must still store
 * and return the mask, because tools/ant_features.py round-trips it, and must
 * NOT pretend to filter, because loss accounting depends on the RX_FAIL events
 * actually arriving and "unexplained loss must be 0" is one of the A/B gates.
 * So this is storage with a getter, and no filtering is applied anywhere in
 * this module.
 * ---------------------------------------------------------------------------
 */
antr_err_t ant_event_filter_set(uint16_t filter);
antr_err_t ant_event_filter_get(uint16_t *filter);

/* ---------------------------------------------------------------------------
 * The timestamp downgrade policy
 * ---------------------------------------------------------------------------
 */
enum ant_event_ts_policy {
	/*
	 * The default. An event whose t_sync_exact is false gets no RX
	 * timestamp field at all: the flag bit is cleared for that message and
	 * ant_event_stats.ts_suppressed counts it. Absence is the only way
	 * this wire format can say "I cannot give you the accurate number",
	 * and saying nothing is better than a number a host will read as
	 * radio-clock accurate when it is not.
	 */
	ANT_EVENT_TS_EXACT_ONLY = 0,
	/*
	 * Bench and bring-up only: emit the best-effort stamp anyway. Useful
	 * when characterising a new backend's correction constant, where a
	 * biased number is exactly what you want to measure. Never appropriate
	 * for a shipped image, and never for a run whose `timing` figure will
	 * be quoted.
	 */
	ANT_EVENT_TS_BEST_EFFORT = 1
};

void                     ant_event_set_ts_policy(enum ant_event_ts_policy p);
enum ant_event_ts_policy ant_event_get_ts_policy(void);

/* ---------------------------------------------------------------------------
 * The extended tail, as a pure function
 * ---------------------------------------------------------------------------
 */

/*
 * Everything a received message can append, gathered in one place.
 *
 * There is no field here for "now". The timestamp is t_sync and only t_sync;
 * see the header comment. has_t_sync exists for the small number of paths that
 * have no HAL event behind them at all, not as an invitation to substitute
 * another clock.
 */
struct ant_event_ext {
	struct ant_channel_id id;

	bool   has_rssi;
	int8_t rssi_dbm;

	/* Straight from ant_rx_event. t_sync is the instant the last bit of
	 * the on-air address was at the antenna; t_sync_exact is false when
	 * the backend inferred it rather than captured it. */
	bool       has_t_sync;
	bool       t_sync_exact;
	ant_time_t t_sync;
};

/*
 * Write the flag byte and the fields `config` asks for into out, in wire
 * order, and return the number of bytes written.
 *
 * Returns 0 - and writes nothing - when no field is emitted. That is not the
 * same as writing a flag byte of zero: a message with no extended fields has
 * no flag byte either, which is what makes a plain broadcast nine bytes long
 * on the wire whatever the library configuration is set to.
 *
 * A field the configuration asked for but the event cannot supply (RSSI on a
 * backend without it, an inexact timestamp under the default policy) is
 * omitted and its flag bit stays clear. out_len must be at least
 * ANT_EVENT_EXT_MAX; anything smaller is ANT_EVENT_EINVAL rather than a
 * truncated tail.
 *
 * Exposed because it is a pure function of its arguments and the byte layout
 * is the part with a byte-diff gate on it - a test should be able to assert
 * the ten bytes without standing up a queue.
 */
int ant_event_build_ext(uint8_t config, const struct ant_event_ext *in,
			uint8_t *out, size_t out_len);

/*
 * t_sync, in the 16 bits of 32768 Hz counter the wire field carries.
 *
 * 65536 ticks is exactly 2,000,000 microseconds, so the field wraps every two
 * seconds and the conversion is exact with no accumulated rounding: reduce the
 * microsecond value modulo 2,000,000 first and the multiply cannot overflow 32
 * bits. Doing it the other way round - converting the full 64-bit value and
 * truncating - is arithmetically identical and needlessly 64-bit on a
 * Cortex-M.
 *
 * Two seconds of range is enough because the host only ever differences
 * consecutive packets; the ~250 ms channel period leaves eight periods of
 * margin before an unwrapped difference becomes ambiguous.
 */
uint16_t ant_event_rx_ticks(ant_time_t t_sync);

/* ---------------------------------------------------------------------------
 * Posting
 * ---------------------------------------------------------------------------
 */

/*
 * One received data message, as the module that decoded the frame sees it.
 *
 * NOTE WHAT IS NOT HERE: no timestamp, no RSSI, no exactness flag. Those come
 * out of `hal`, which is the event the backend handed to the callback, and
 * copying them here would create a second place they could be wrong. The
 * single most valuable property of this structure is that a caller holding a
 * clock reading has nowhere to put it.
 */
struct ant_event_rx {
	/*
	 * The HAL event this message came from. Required, and must have
	 * status ANT_RADIO_STATUS_OK - see the CRC rule on
	 * ant_event_post_rx(). Read only for the duration of the call.
	 */
	const struct ant_rx_event *hal;

	/* ANTW_MESG_BROADCAST_DATA_ID, ANTW_MESG_ACKNOWLEDGED_DATA_ID or
	 * ANTW_MESG_BURST_DATA_ID. Nothing else is a received data message. */
	uint8_t msg_id;

	/* 0 .. ANT_EVENT_CHANNEL_MAX. */
	uint8_t channel;

	/*
	 * Burst framing, for ANTW_MESG_BURST_DATA_ID only and ignored
	 * otherwise. The serial protocol's burst header is
	 * [last<<7 | seq<<5 | channel], and the sequence field there is two
	 * bits wide (ANTW_BURST_HEADER_SEQ_MASK).
	 *
	 * That is a serial-layer counter and is NOT the on-air sequence bit:
	 * Spike B part 2 measured the control byte's sequence as a single
	 * alternating bit with no wider form (docs/spike-b-part2-results.md,
	 * ant_frame.h). ant_burst.c owns the mapping between the two; this
	 * module only places the bits.
	 */
	uint8_t burst_seq;
	bool    burst_last;

	/*
	 * The sender's channel ID, wildcards already resolved. In tracking
	 * this is proven by the address match; in search the caller recovered
	 * devnum_lo from ant_rx_event.filter_index and the rest from the body.
	 * Either way it is settled before it gets here - this module never
	 * guesses an identity.
	 */
	struct ant_channel_id id;

	/* The payload, 1 .. ANT_EVENT_PAYLOAD_MAX bytes. COPIED before this
	 * function returns, which is what lets a callback pass
	 * ant_rx_event.body straight in. */
	const uint8_t *payload;
	uint8_t        payload_len;
};

/*
 * Queue one received data message. Safe to call from the radio callback, and
 * that is where it is meant to be called from.
 *
 * A CRC-FAILED FRAME NEVER BECOMES AN EVENT. A HAL event whose status is
 * ANT_RADIO_STATUS_CRC_FAIL is refused with ANT_EVENT_ECRC and nothing is
 * queued; any other non-OK status is ANT_EVENT_EINVAL. The check is here, at
 * the last gate before the host, as well as in the modules that decide what to
 * deliver - Spike A measured a 3-byte search address triggering the matcher on
 * noise several times a second at -54..-101 dBm, and with eight filters armed
 * the bench sees about 1.4 CRC failures a second while idle. Ranking or gating
 * on match count instead of CRC turns that noise into phantom sensors, so the
 * refusal is duplicated on purpose.
 *
 * Returns ANT_EVENT_OK, ANT_EVENT_ECRC, ANT_EVENT_EINVAL, ANT_EVENT_ENOSPC or
 * ANT_EVENT_ESTATE. ENOSPC is a dropped message, counted, and followed by an
 * ANTW_EVENT_QUE_OVERFLOW to the host - never a silent loss.
 */
int ant_event_post_rx(const struct ant_event_rx *rx);

/*
 * Queue a channel event: [channel][ANTW_MESG_EVENT_ID][code] under message ID
 * ANTW_MESG_RESPONSE_EVENT_ID, which is the shape every ANTW_EVENT_* takes on
 * the wire.
 *
 * Safe from the radio callback. `code` is an ANTW_EVENT_* value and is not
 * validated against a list: eleven of them exist today, the set grows, and a
 * whitelist here would silently swallow a new one. What IS counted is the
 * three that carry buffer ownership.
 */
int ant_event_post_channel_event(uint8_t channel, uint8_t code);

/*
 * Queue an arbitrary message. The escape hatch for anything that is neither a
 * data message nor a channel event - and, today, nothing in the firmware needs
 * it: the bridge builds the startup message itself
 * (src/ant_serial_bridge.c:98), so a backend must not also raise one.
 *
 * body may be NULL only when len is 0. Returns ANT_EVENT_EINVAL for a len
 * above ANT_EVENT_BODY_MAX.
 */
int ant_event_post_raw(uint8_t msg_id, const uint8_t *body, uint8_t len);

/* ---------------------------------------------------------------------------
 * The three burst release codes
 *
 * Wrappers over ant_event_post_channel_event() with the code spelled once, so
 * that ant_burst.c cannot pick the wrong one and so that "released exactly
 * once per accepted block" is a counter comparison rather than a log read.
 * ---------------------------------------------------------------------------
 */
static inline int ant_event_post_transfer_next_block(uint8_t channel)
{
	return ant_event_post_channel_event(
		channel, (uint8_t)ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK);
}

static inline int ant_event_post_transfer_tx_completed(uint8_t channel)
{
	return ant_event_post_channel_event(
		channel, (uint8_t)ANTW_EVENT_TRANSFER_TX_COMPLETED);
}

static inline int ant_event_post_transfer_tx_failed(uint8_t channel)
{
	return ant_event_post_channel_event(
		channel, (uint8_t)ANTW_EVENT_TRANSFER_TX_FAILED);
}

/* ---------------------------------------------------------------------------
 * Draining
 * ---------------------------------------------------------------------------
 */

/* True if a call to ant_event_drain() would deliver anything - including the
 * pending overflow marker, which is a message with no slot behind it. */
bool ant_event_pending(void);

/*
 * Assemble and deliver queued messages through antr_on_message(), oldest
 * first, and return how many were delivered. max_msgs bounds one call; 0 means
 * "everything currently queued".
 *
 * THREAD CONTEXT ONLY. antr_on_message() may run on any context the backend
 * likes, but it must not be re-entered and it must not be called from inside
 * an antr_* call the bridge made - so the drain belongs on ant_api.c's own
 * thread or work queue, woken by ant_event_wakeup(). A drain re-entered from
 * inside antr_on_message() returns ANT_EVENT_ESTATE's worth of nothing: zero,
 * with nothing delivered.
 *
 * Delivery is FIFO and is never reordered. A message dropped because the ring
 * was full is reported to the host as ANTW_EVENT_QUE_OVERFLOW on channel 0 at
 * the head of the next drain; the marker says that something was lost, not
 * where, and it is emitted at the head rather than in position so that a ring
 * which stays busy cannot starve the report.
 */
uint32_t ant_event_drain(uint32_t max_msgs);

/* ---------------------------------------------------------------------------
 * Statistics
 *
 * Every one of these exists because some test or gate needs to distinguish two
 * outcomes that look the same from outside. They are read by
 * ant_core/tests/src/test_event.c and are cheap enough to keep in a shipped
 * image.
 * ---------------------------------------------------------------------------
 */
struct ant_event_stats {
	uint32_t posted;            /* accepted onto the ring */
	uint32_t delivered;         /* handed to antr_on_message() */
	uint32_t dropped_full;      /* refused ANT_EVENT_ENOSPC */
	uint32_t dropped_flush;     /* discarded by ant_event_flush() */
	uint32_t rejected_crc;      /* refused ANT_EVENT_ECRC - the noise gate */
	uint32_t rejected_invalid;  /* refused ANT_EVENT_EINVAL / ESTATE */
	uint32_t ts_suppressed;     /* RX timestamp asked for, not exact,
				     * downgraded to absent */
	uint32_t rssi_suppressed;   /* RSSI asked for, backend has none */
	uint32_t overflow_marks;    /* ANTW_EVENT_QUE_OVERFLOW messages sent */

	/* The three release codes, counted separately so the burst
	 * buffer-ownership obligation is assertable. */
	uint32_t next_data_block;
	uint32_t tx_completed;
	uint32_t tx_failed;

	/* Deepest the ring ever got. A run that never exceeds a handful says
	 * the depth is over-provisioned; one that touches
	 * ANT_EVENT_QUEUE_DEPTH says the drain is not keeping up. */
	uint16_t high_water;
};

const struct ant_event_stats *ant_event_stats_get(void);

/* Queued and not yet delivered. */
uint16_t ant_event_queued(void);

#ifdef __cplusplus
}
#endif

#endif /* ANT_EVENT_H_ */
