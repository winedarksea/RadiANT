/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_event.h - the event queue, and the assembly of a received frame into the
 * flat struct antr_msg the serial bridge implements.
 *
 * Provenance: clean-room, from src/ant_radio.h and docs/sdk-ant-contract.md
 * (event path, burst buffer-ownership, threading), src/ant_wire.h (generated
 * from protocol/ant_wire.yaml) for wire constants, radiant_radio_hal.h for
 * the t_sync contract, radiant_frame.h for the channel ID, and
 * tools/ant_verify.py's extended_fields() for the decoder this must invert.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * Everything chip -> host goes through here and leaves via one call,
 * antr_on_message(). Two halves: THE QUEUE - radio callbacks run in
 * interrupt context, must not block or retain the received body (a DMA
 * buffer reused the instant the callback returns; radiant_core/tests/fake_radio.c
 * overwrites it with 0xDD to make a violation reproducible), so a callback
 * copies into a ring and a thread drains it later. THE ASSEMBLY - a queued
 * receive becomes [channel][payload...] plus, per library configuration, a
 * flag byte and up to three extended fields, the exact inverse of
 * tools/ant_verify.py's extended_fields() (a Tier 1 byte-diff gate). The
 * drain is a separate call, not a callback, because the ISR half must stay
 * cheap and the assembly half must not run in it.
 *
 * Library configuration 0xE0 (every tool in tools/ requests it) is THREE
 * fields assembled in order, each present only if its bit is set: flag byte
 * (CHANNEL_ID 0x80, RSSI 0x40, RX_TIMESTAMP 0x20), then channel ID (4B),
 * RSSI (3B), RX timestamp (2B, 32768 Hz counter LE). Order is fixed but
 * *offsets* are not - a field's position depends on which earlier fields
 * turned up, which is the whole reason the flag byte exists; never emit a
 * placeholder for an unreported field. Device type keeps its pairing bit
 * (bit 7) - tools mask it themselves when they want the bare type.
 *
 * THE RX TIMESTAMP COMES FROM t_sync, AND NOTHING ELSE - the subtlest
 * requirement here, with no functional symptom when wrong. t_sync is the
 * instant the last address bit was at the antenna, on the backend's own
 * corrected microsecond timebase; any other clock (radiant_radio_now() at
 * drain, host clock, thread tick) passes every functional test but destroys
 * the measurement the field exists for (0.009 ms on the radio clock vs.
 * ~2.6 ms of USB jitter on a host clock - sub-ms multi-sensor fusion rests
 * on the former). So struct radiant_event_rx has NO timestamp field, only a
 * pointer to the HAL event this module copies t_sync out of, and the tick
 * conversion happens at drain time but on the value captured in the
 * callback (radiant_core/tests/src/test_event.c asserts this against a
 * clock advanced between arrival and drain).
 *
 * A backend with caps.has_sync_timestamp false still fills t_sync
 * best-effort but clears t_sync_exact; the rule is to *suppress* rather than
 * report a worse number as if accurate, so by default an inexact stamp
 * clears ANTW_EXT_FLAG_RX_TIMESTAMP for that message (counted in
 * radiant_event_stats.ts_suppressed; RADIANT_EVENT_TS_BEST_EFFORT opts back
 * in for bench use). Same rule for RSSI: has_rssi false suppresses the flag
 * rather than emitting an implausible 0 dBm.
 *
 * Event codes are wire values: antr_err_t and every ANTW_EVENT_* code raised
 * here is the literal wire byte (tools/ant_conformance.py byte-diffs it),
 * always from src/ant_wire.h, never invented. Three codes also carry buffer
 * ownership, releasing the bridge's single 24-byte burst block
 * (src/ant_serial_bridge.c:452-502): TRANSFER_NEXT_DATA_BLOCK (once per
 * non-final accepted block), TRANSFER_TX_COMPLETED, TRANSFER_TX_FAILED.
 * Missing the first doesn't fail loudly - the bridge's k_sem_take() just
 * waits its full 1000 ms per block, silently turning a two-second transfer
 * into ten minutes. radiant_burst.c raises them; this module counts each one
 * (radiant_event_stats) so the "release count equals accepted-block count"
 * contract is a counter check, not a log read.
 *
 * Sized for 32 channels (the serial protocol's own ceiling: burst header
 * channel field is 5 bits) and background scan: ~128 msg/s from 32 tracked
 * channels at 4 Hz, plus every sensor scan picks up. Sized from that rate
 * rather than libant.a's eight channels, since queue depth is expensive to
 * retrofit later.
 */

#ifndef RADIANT_EVENT_H_
#define RADIANT_EVENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_frame.h>      /* struct radiant_channel_id */
#include <radiant_core/radiant_radio_hal.h>  /* radiant_time_t, struct radiant_rx_event */

/*
 * The output format. This module exists to produce struct antr_msg and to call
 * antr_on_message(), so the dependency is the point rather than a leak: it is
 * the one place in radiant_core that knows the shape of the seam above it.
 * src/ant_radio.h pulls in src/ant_wire.h, which is where every ANTW_* value
 * below comes from.
 */
#include "ant_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes - negative, module-private internal outcomes, never
 * forwarded to the host. Functions answering a host message (the library
 * configuration setters) return antr_err_t instead, marked as such.
 * ---------------------------------------------------------------------------
 */
#define RADIANT_EVENT_OK        0
#define RADIANT_EVENT_EINVAL  (-1)  /* null pointer, bad channel, bad length */
#define RADIANT_EVENT_ENOSPC  (-2)  /* the queue is full; the message was dropped
				 * and ANTW_EVENT_QUE_OVERFLOW will be raised */
#define RADIANT_EVENT_ECRC    (-3)  /* the HAL event did not carry a verified CRC.
				 * See radiant_event_post_rx() - this is a refusal,
				 * not a failure */
#define RADIANT_EVENT_ESTATE  (-4)  /* posted before radiant_event_init(), or a drain
				 * re-entered from inside antr_on_message() */

/* ---------------------------------------------------------------------------
 * Sizing
 * ---------------------------------------------------------------------------
 */

/* Highest channel number expressible on the wire. The burst header's low five
 * bits are the channel, so this is a protocol fact rather than a policy
 * choice, and it is what makes 32 the natural target. */
#define RADIANT_EVENT_CHANNEL_MAX  ANTW_BURST_HEADER_CHANNEL_MASK

/* Longest payload one queued message can carry. An advanced-burst block is 8,
 * 16 or 24 bytes (ANTW_ADV_BURST_BLOCK_MAX); everything else is 8. */
#define RADIANT_EVENT_PAYLOAD_MAX  ANTW_ADV_BURST_BLOCK_MAX

/* The body before any extended tail: the channel byte plus the payload. This
 * is what antr_msg.len counts, so it counts the channel byte too. */
#define RADIANT_EVENT_BODY_MAX     (ANTW_CHANNEL_NUM_SIZE + RADIANT_EVENT_PAYLOAD_MAX)

/* The extended tail at its longest: flag byte + 4 + 3 + 2. */
#define RADIANT_EVENT_EXT_MAX      10u

/* The longest message this module can hand to the bridge. */
#define RADIANT_EVENT_MSG_MAX      (RADIANT_EVENT_BODY_MAX + RADIANT_EVENT_EXT_MAX)

/*
 * Queued messages. Must be a power of two - the ring masks rather than
 * divides, since the producer runs in a radio ISR.
 *
 * 64 slots is ~500 ms of headroom at 128 msg/s (32 channels at 4 Hz), and
 * comfortably absorbs the 51-packet burst Spike B part 2 captured. Costs
 * ~3 KB static RAM. Override with -DANT_EVENT_QUEUE_DEPTH=<n>; 32 (one slot
 * per channel) is the lowest value that can't lose a message to one slot's
 * worth of simultaneous arrivals.
 */
#ifndef RADIANT_EVENT_QUEUE_DEPTH
#define RADIANT_EVENT_QUEUE_DEPTH  64u
#endif

/* Library configuration bits this module implements. Anything outside it is
 * refused with ANTW_INVALID_PARAMETER_PROVIDED rather than silently ignored.
 *
 * ANTW_LIB_CONFIG_RADIO_CONFIG_ALWAYS is a no-op by construction: HAL rule 6
 * already reconfigures per-operation regardless of this bit, so accepting
 * it is honest, not a pretence. Which bits a real stick refuses is pinned
 * by tools/ant_conformance.py's byte diff. */
#define RADIANT_EVENT_LIB_CONFIG_SUPPORTED                                       \
	((uint8_t)(ANTW_LIB_CONFIG_RADIO_CONFIG_ALWAYS |                     \
		   ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP |                 \
		   ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI |                       \
		   ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID))

/* ---------------------------------------------------------------------------
 * Two hooks the port provides
 *
 * Deliberately undefined here, so a build that forgets one fails at link
 * time rather than shipping a silent data race. radiant_event.c compiles as
 * a freestanding translation unit (no Zephyr header, same rule as
 * radiant_frame.c) so it can't reach irq_lock() or a semaphore itself.
 *
 * In firmware these are two lines in radiant_api.c; in unit tests, a no-op
 * and a counter (the mock radio is single-threaded).
 * ---------------------------------------------------------------------------
 */

/*
 * Enter and leave the critical section protecting the ring. Producer may be
 * a radio ISR, consumer is a thread, so map onto irq_lock()/irq_unlock() - a
 * mutex is wrong since the producer must not block. A handful of
 * instructions plus one 48-byte copy; never calls out, never loops.
 */
unsigned int radiant_event_crit_enter(void);
void         radiant_event_crit_exit(unsigned int key);

/*
 * Called once per successfully queued message, possibly from the radio ISR.
 * Only job is to make the drain run (k_sem_give(), k_work_submit(), ...).
 * Must be O(1), interrupt-safe, and must not call back into this module.
 */
void radiant_event_wakeup(void);

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/*
 * Bring the module up: empty ring, library configuration and event filter
 * zeroed, statistics cleared, timestamp policy back to default.
 *
 * Call from antr_init() BEFORE the radio is enabled. Posting before this
 * returns is RADIANT_EVENT_ESTATE, mirroring the antr_on_message() rule.
 */
void radiant_event_init(void);

/*
 * What antr_stack_reset() needs: discard every queued message and zero the
 * library configuration and event filter, without delivering anything.
 *
 * Discarding, not draining, is correct: a real stick emits nothing between
 * reset and the startup message (the bridge discards inbound events for
 * 50 ms for that reason), and draining here would recursively call
 * antr_on_message() from inside an antr_* call, which
 * docs/sdk-ant-contract.md forbids. Discards counted in
 * radiant_event_stats.dropped_flush; stats themselves are not cleared, so a
 * mid-session reset stays visible to a test.
 */
void radiant_event_flush(void);

/* ---------------------------------------------------------------------------
 * Library configuration - the three functions radiant_api.c forwards
 * ---------------------------------------------------------------------------
 */

/*
 * Turn bits on. Additive: setting one leaves the others alone.
 *
 * Returns ANTW_RESPONSE_NO_ERROR, or ANTW_INVALID_PARAMETER_PROVIDED for any
 * bit outside RADIANT_EVENT_LIB_CONFIG_SUPPORTED. Nothing is applied when
 * refused - a host can't tell which half of a partial configuration took.
 */
antr_err_t radiant_event_lib_config_set(uint8_t config);

/*
 * Turn bits off. Accepts anything, including ANTW_LIB_CONFIG_MASK_ALL, which
 * is how the bridge expresses the host's "set the configuration to zero" -
 * there is no separate clear message on the wire. Returns
 * ANTW_RESPONSE_NO_ERROR.
 */
antr_err_t radiant_event_lib_config_clear(uint8_t config);

/* The current bits. Returns ANTW_RESPONSE_NO_ERROR; *config is untouched when
 * it is NULL, which is ANTW_INVALID_PARAMETER_PROVIDED. */
antr_err_t radiant_event_lib_config_get(uint8_t *config);

/* ---------------------------------------------------------------------------
 * Event filtering - stored, honoured by nobody, deliberately
 *
 * src/ant_radio.h requires a non-filtering backend to still round-trip the
 * mask (tools/ant_features.py checks it) and NOT pretend to filter, since
 * loss accounting needs every RX_FAIL to actually arrive ("unexplained loss
 * must be 0" is an A/B gate). So: storage plus a getter, no filtering.
 * ---------------------------------------------------------------------------
 */
antr_err_t radiant_event_filter_set(uint16_t filter);
antr_err_t radiant_event_filter_get(uint16_t *filter);

/* ---------------------------------------------------------------------------
 * The timestamp downgrade policy
 * ---------------------------------------------------------------------------
 */
enum radiant_event_ts_policy {
	/*
	 * Default. An inexact event (t_sync_exact false) gets no RX timestamp
	 * field - the flag bit clears and radiant_event_stats.ts_suppressed
	 * counts it. Absence is the only way to say "can't give you the
	 * accurate number" without a host reading a bad one as accurate.
	 */
	RADIANT_EVENT_TS_EXACT_ONLY = 0,
	/* Bench/bring-up only: emit the best-effort stamp anyway, for
	 * characterising a new backend's correction constant. Never for a
	 * shipped image or a quoted `timing` run. */
	RADIANT_EVENT_TS_BEST_EFFORT = 1
};

void                     radiant_event_set_ts_policy(enum radiant_event_ts_policy p);
enum radiant_event_ts_policy radiant_event_get_ts_policy(void);

/* ---------------------------------------------------------------------------
 * The extended tail, as a pure function
 * ---------------------------------------------------------------------------
 */

/*
 * Everything a received message can append, gathered in one place. No
 * "now" field - the timestamp is t_sync only (see header comment).
 * has_t_sync exists for the few paths with no HAL event behind them, not as
 * an invitation to substitute another clock.
 */
struct radiant_event_ext {
	struct radiant_channel_id id;

	bool   has_rssi;
	int8_t rssi_dbm;

	/* Straight from radiant_rx_event. t_sync_exact is false when inferred
	 * rather than captured. */
	bool       has_t_sync;
	bool       t_sync_exact;
	radiant_time_t t_sync;
};

/*
 * Write the flag byte and the fields `config` asks for into out, in wire
 * order, and return the number of bytes written.
 *
 * Returns 0 (writes nothing) when no field is emitted - not the same as a
 * flag byte of zero: no extended fields means no flag byte at all, so a
 * plain broadcast is nine bytes regardless of library configuration.
 *
 * A field asked for but the event can't supply (RSSI missing, inexact
 * timestamp under the default policy) is omitted, flag bit clear. out_len
 * must be at least RADIANT_EVENT_EXT_MAX or this is RADIANT_EVENT_EINVAL.
 *
 * Exposed as a pure function so the byte-diff-gated layout can be asserted
 * without standing up a queue.
 */
int radiant_event_build_ext(uint8_t config, const struct radiant_event_ext *in,
			uint8_t *out, size_t out_len);

/*
 * t_sync, in the 16 bits of 32768 Hz counter the wire field carries.
 *
 * 65536 ticks = exactly 2,000,000 us, so the field wraps every 2 s; reducing
 * mod 2,000,000 before multiplying keeps this exact and inside 32 bits
 * (equivalent to converting the full 64-bit value, but avoids 64-bit math on
 * a Cortex-M). Two seconds is enough range since the host only differences
 * consecutive packets, leaving eight periods of margin at ~250 ms.
 */
uint16_t radiant_event_rx_ticks(radiant_time_t t_sync);

/* ---------------------------------------------------------------------------
 * Posting
 * ---------------------------------------------------------------------------
 */

/*
 * One received data message, as the module that decoded the frame sees it.
 *
 * No timestamp, RSSI or exactness flag here - those come from `hal` (the
 * backend's event), and copying them would create a second place they could
 * be wrong. A caller holding a clock reading has nowhere to put it.
 */
struct radiant_event_rx {
	/* The HAL event this came from. Required, status must be
	 * RADIANT_RADIO_STATUS_OK (see the CRC rule below). Read only for the
	 * duration of the call. */
	const struct radiant_rx_event *hal;

	/* ANTW_MESG_BROADCAST_DATA_ID, ANTW_MESG_ACKNOWLEDGED_DATA_ID or
	 * ANTW_MESG_BURST_DATA_ID. Nothing else is a received data message. */
	uint8_t msg_id;

	/* 0 .. RADIANT_EVENT_CHANNEL_MAX. */
	uint8_t channel;

	/*
	 * Burst framing, for ANTW_MESG_BURST_DATA_ID only. Serial burst
	 * header is [last<<7 | seq<<5 | channel], seq field 2 bits wide.
	 * This is a serial-layer counter, NOT the on-air sequence bit,
	 * which is a single alternating bit with no wider form
	 * (docs/spike-b-part2-results.md). radiant_burst.c owns the
	 * mapping; this module only places the bits.
	 */
	uint8_t burst_seq;
	bool    burst_last;

	/* The sender's channel ID, wildcards already resolved (proven by
	 * address match in tracking; recovered from filter_index and the
	 * body in search) - this module never guesses an identity. */
	struct radiant_channel_id id;

	/* The payload, 1 .. RADIANT_EVENT_PAYLOAD_MAX bytes. Copied before
	 * this function returns, so a callback can pass radiant_rx_event.body
	 * straight in. */
	const uint8_t *payload;
	uint8_t        payload_len;
};

/*
 * Queue one received data message. Safe to call from, and meant to be
 * called from, the radio callback.
 *
 * A CRC-FAILED FRAME NEVER BECOMES AN EVENT: RADIANT_RADIO_STATUS_CRC_FAIL is
 * refused with RADIANT_EVENT_ECRC (any other non-OK status is EINVAL). This
 * check is duplicated here on purpose - a bare 3-byte search address
 * triggers the matcher on noise (~1.4 CRC failures/s while idle with eight
 * filters armed), and gating on match count instead of CRC would turn that
 * into phantom sensors.
 *
 * Returns RADIANT_EVENT_OK, ECRC, EINVAL, ENOSPC or ESTATE. ENOSPC is a
 * dropped message, counted, followed by ANTW_EVENT_QUE_OVERFLOW to the host
 * - never a silent loss.
 */
int radiant_event_post_rx(const struct radiant_event_rx *rx);

/*
 * Queue a channel event: [channel][ANTW_MESG_EVENT_ID][code] under message
 * ID ANTW_MESG_RESPONSE_EVENT_ID, the shape every ANTW_EVENT_* takes.
 *
 * Safe from the radio callback. `code` is not validated against a list -
 * eleven exist today and the set grows, so a whitelist would silently
 * swallow a new one. The three that carry buffer ownership are counted.
 */
int radiant_event_post_channel_event(uint8_t channel, uint8_t code);

/*
 * Queue an arbitrary message - the escape hatch for anything neither a data
 * message nor a channel event. Nothing in the firmware needs it today: the
 * bridge builds the startup message itself, so a backend must not also
 * raise one.
 *
 * body may be NULL only when len is 0. Returns RADIANT_EVENT_EINVAL for a
 * len above RADIANT_EVENT_BODY_MAX.
 */
int radiant_event_post_raw(uint8_t msg_id, const uint8_t *body, uint8_t len);

/* ---------------------------------------------------------------------------
 * The three burst release codes
 *
 * Wrappers over radiant_event_post_channel_event() with the code spelled
 * once, so radiant_burst.c can't pick the wrong one and "released exactly
 * once per accepted block" is a counter comparison, not a log read.
 * ---------------------------------------------------------------------------
 */
static inline int radiant_event_post_transfer_next_block(uint8_t channel)
{
	return radiant_event_post_channel_event(
		channel, (uint8_t)ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK);
}

static inline int radiant_event_post_transfer_tx_completed(uint8_t channel)
{
	return radiant_event_post_channel_event(
		channel, (uint8_t)ANTW_EVENT_TRANSFER_TX_COMPLETED);
}

static inline int radiant_event_post_transfer_tx_failed(uint8_t channel)
{
	return radiant_event_post_channel_event(
		channel, (uint8_t)ANTW_EVENT_TRANSFER_TX_FAILED);
}

/* ---------------------------------------------------------------------------
 * Draining
 * ---------------------------------------------------------------------------
 */

/* True if a call to radiant_event_drain() would deliver anything - including the
 * pending overflow marker, which is a message with no slot behind it. */
bool radiant_event_pending(void);

/*
 * Assemble and deliver queued messages through antr_on_message(), oldest
 * first, returning how many were delivered. max_msgs bounds one call; 0
 * means "everything currently queued".
 *
 * THREAD CONTEXT ONLY. antr_on_message() must not be re-entered or called
 * from inside an antr_* call the bridge made, so the drain belongs on
 * radiant_api.c's own thread/work queue, woken by radiant_event_wakeup(). A
 * drain re-entered from inside antr_on_message() delivers nothing and
 * returns zero.
 *
 * FIFO, never reordered. A message dropped for a full ring is reported as
 * ANTW_EVENT_QUE_OVERFLOW on channel 0 at the head of the next drain - at
 * the head rather than in position so a busy ring can't starve the report.
 */
uint32_t radiant_event_drain(uint32_t max_msgs);

/* ---------------------------------------------------------------------------
 * Statistics
 *
 * Each exists because some test or gate needs to distinguish two outcomes
 * that look the same from outside. Read by
 * radiant_core/tests/src/test_event.c; cheap enough to ship.
 * ---------------------------------------------------------------------------
 */
struct radiant_event_stats {
	uint32_t posted;            /* accepted onto the ring */
	uint32_t delivered;         /* handed to antr_on_message() */
	uint32_t dropped_full;      /* refused RADIANT_EVENT_ENOSPC */
	uint32_t dropped_flush;     /* discarded by radiant_event_flush() */
	uint32_t rejected_crc;      /* refused RADIANT_EVENT_ECRC - the noise gate */
	uint32_t rejected_invalid;  /* refused RADIANT_EVENT_EINVAL / ESTATE */
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
	 * RADIANT_EVENT_QUEUE_DEPTH says the drain is not keeping up. */
	uint16_t high_water;
};

const struct radiant_event_stats *radiant_event_stats_get(void);

/* Queued and not yet delivered. */
uint16_t radiant_event_queued(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_EVENT_H_ */
