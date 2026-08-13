/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_event.c - the event queue and the assembly of a received frame into the
 * flat struct antr_msg the serial bridge implements.
 *
 * Provenance: clean-room. Written from radiant_core/include/radiant_core/radiant_event.h (which
 * carries the reasoning), from src/ant_radio.h and docs/sdk-ant-contract.md,
 * from radiant_core/include/radiant_core/radiant_radio_hal.h for the t_sync contract and the
 * callback rules, and from src/ant_wire.h - generated from
 * protocol/ant_wire.yaml - for every wire constant. The extended-message
 * layout is the exact inverse of tools/ant_verify.py's extended_fields().
 * Nothing here derives from sdk-ant, from libant.a, from disassembly of any
 * binary, or from any ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application beyond
 * the two headers that define its input and its output. It has to compile as a
 * freestanding translation unit - the same rule radiant_frame.c follows - which is
 * why the two things it genuinely needs from an RTOS (a critical section and a
 * way to wake the drain) are port hooks rather than #includes. The gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -I radiant_core/tests -I src \
 *       -fsyntax-only radiant_core/src/radiant_event.c
 *
 * so that a compile error here is never confused with a Zephyr or a board
 * problem.
 */

#include <string.h>

#include <radiant_core/radiant_event.h>

/* ---------------------------------------------------------------------------
 * Compile-time checks (_Static_assert, not Zephyr's BUILD_ASSERT, since this
 * file deliberately includes no Zephyr header)
 * ---------------------------------------------------------------------------
 */

/* The ring masks rather than divides, because the producer runs in a radio
 * ISR. Both properties are load-bearing, so both are checked. */
_Static_assert((RADIANT_EVENT_QUEUE_DEPTH & (RADIANT_EVENT_QUEUE_DEPTH - 1u)) == 0u,
	       "RADIANT_EVENT_QUEUE_DEPTH must be a power of two");
_Static_assert(RADIANT_EVENT_QUEUE_DEPTH >= 32u,
	       "fewer slots than channels can lose a message with the drain "
	       "running normally");
_Static_assert(RADIANT_EVENT_QUEUE_DEPTH <= 4096u,
	       "RADIANT_EVENT_QUEUE_DEPTH is implausible; check the -D");

/* Every channel the wire can name must fit in the burst header's low five
 * bits, which is the whole reason 32 is the target rather than 64. */
_Static_assert(RADIANT_EVENT_CHANNEL_MAX >= 31u,
	       "32 channels are no longer expressible in a burst header");

/* A message this module builds must fit the frame the bridge wraps it in. */
_Static_assert(RADIANT_EVENT_MSG_MAX <= ANTW_MAX_SIZE_VALUE,
	       "an assembled message can exceed the serial LEN byte's ceiling");

/* The extended tail's arithmetic, spelled out so a fourth field cannot be
 * added without the buffer growing with it. */
_Static_assert(1u + 4u + 3u + 2u == RADIANT_EVENT_EXT_MAX,
	       "RADIANT_EVENT_EXT_MAX no longer matches flag + channel id + RSSI "
	       "+ timestamp");

/* The payload buffer must hold the largest frame radiant_frame.c can produce. */
_Static_assert(RADIANT_EVENT_PAYLOAD_MAX >= RADIANT_FRAME_PAYLOAD_MAX,
	       "a frame payload no longer fits in an event slot");

/* The library-configuration request bit and the extended-message flag bit
 * share a value for all three fields - true of the protocol, but they are two
 * different namespaces (a payload byte of message 0x6E vs. a byte appended to
 * every received data message), so the code below maps them explicitly. These
 * assertions catch the explicit map silently disagreeing with the generated
 * header if either one moves. */
_Static_assert((uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID ==
		       (uint8_t)ANTW_EXT_FLAG_CHANNEL_ID,
	       "channel-id request bit and flag bit have diverged");
_Static_assert((uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI ==
		       (uint8_t)ANTW_EXT_FLAG_RSSI,
	       "RSSI request bit and flag bit have diverged");
_Static_assert((uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP ==
		       (uint8_t)ANTW_EXT_FLAG_RX_TIMESTAMP,
	       "timestamp request bit and flag bit have diverged");

/* The one configuration every tool asks for must be entirely supported, or
 * ant_verify.py's three fields become two and the timing gate has nothing to
 * read. */
_Static_assert(((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS &
		(uint8_t)~RADIANT_EVENT_LIB_CONFIG_SUPPORTED) == 0u,
	       "lib config 0xE0 asks for a field this module refuses");

/* ---------------------------------------------------------------------------
 * The 32768 Hz receive timestamp
 * ---------------------------------------------------------------------------
 */

/* 65536 ticks of 32768 Hz is exactly two seconds, so the field's wrap is
 * exact and the reduction below loses nothing. */
#define TS_WRAP_TICKS  65536u
#define TS_WRAP_US     2000000u
/* 32768 / 1000000 reduced: ticks = us * 512 / 15625. */
#define TS_MUL         512u
#define TS_DIV         15625u

_Static_assert(TS_WRAP_US % TS_DIV == 0u &&
		       (TS_WRAP_US / TS_DIV) * TS_MUL == TS_WRAP_TICKS,
	       "the 32768 Hz tick conversion does not close over two seconds");
_Static_assert((unsigned long long)(TS_WRAP_US - 1u) * TS_MUL <= 0xFFFFFFFFull,
	       "the tick conversion overflows 32 bits");

uint16_t radiant_event_rx_ticks(radiant_time_t t_sync)
{
	uint32_t us = (uint32_t)(t_sync % (radiant_time_t)TS_WRAP_US);

	return (uint16_t)((us * TS_MUL) / TS_DIV);
}

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

/* Per-slot flags. Recorded at post time from the HAL event, so the drain never
 * has to reconstruct what the backend could and could not supply. */
#define SLOT_EXT       (1u << 0)  /* extended fields apply to this message */
#define SLOT_HAS_TS    (1u << 1)
#define SLOT_TS_EXACT  (1u << 2)
#define SLOT_HAS_RSSI  (1u << 3)

/* One queued message. Layout chosen for size: 64-bit t_sync first so no hole
 * opens up, everything else a byte. Comes out at 48 bytes / 8-byte alignment,
 * which is what RADIANT_EVENT_QUEUE_DEPTH is costed against in the header. */
struct radiant_event_slot {
	radiant_time_t t_sync;
	uint16_t   device_number;
	uint8_t    msg_id;
	uint8_t    body_len;   /* the channel byte plus the payload */
	uint8_t    device_type;
	uint8_t    trans_type;
	int8_t     rssi_dbm;
	uint8_t    flags;
	uint8_t    body[RADIANT_EVENT_BODY_MAX];
};

static struct {
	struct radiant_event_slot ring[RADIANT_EVENT_QUEUE_DEPTH];

	/* Free-running, masked on use: unsigned wraparound is defined, so
	 * head - tail is the occupancy at any value of either, with no
	 * full-vs-empty ambiguity. */
	uint32_t head;
	uint32_t tail;

	bool ready;             /* radiant_event_init() has run */
	bool overflow_pending;  /* a message was dropped; tell the host */
	bool draining;          /* re-entrancy guard on radiant_event_drain() */

	uint8_t  lib_config;
	uint16_t filter;

	enum radiant_event_ts_policy ts_policy;

	struct radiant_event_stats stats;
} g;

#define RING_MASK  (RADIANT_EVENT_QUEUE_DEPTH - 1u)

/* Counters are plain words, incremented from whichever context reached them.
 * The reject counters take the same critical section as the ring's index
 * moves, so a post from a radio ISR and one from a thread can't lose an
 * increment. Nothing reads a counter to make a decision - this is accounting
 * for tests/reports, not synchronisation. */
static void bump(uint32_t *counter)
{
	unsigned int key = radiant_event_crit_enter();

	(*counter)++;
	radiant_event_crit_exit(key);
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

void radiant_event_init(void)
{
	memset(&g, 0, sizeof(g));
	g.ts_policy = RADIANT_EVENT_TS_EXACT_ONLY;
	g.ready = true;
}

void radiant_event_flush(void)
{
	unsigned int key;
	uint32_t     dropped;

	key = radiant_event_crit_enter();
	dropped = g.head - g.tail;
	g.tail = g.head;
	g.overflow_pending = false;
	g.stats.dropped_flush += dropped;
	g.lib_config = 0u;
	g.filter = 0u;
	radiant_event_crit_exit(key);
}

/* ---------------------------------------------------------------------------
 * Library configuration and event filtering
 * ---------------------------------------------------------------------------
 */

antr_err_t radiant_event_lib_config_set(uint8_t config)
{
	if ((config & (uint8_t)~RADIANT_EVENT_LIB_CONFIG_SUPPORTED) != 0u) {
		/* Refuse the whole request rather than the unknown bits: a
		 * partial configuration is worse than none, since the host
		 * can't tell which half took. */
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	g.lib_config |= config;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t radiant_event_lib_config_clear(uint8_t config)
{
	/* Anything, including ANTW_LIB_CONFIG_MASK_ALL - that is how the
	 * bridge expresses the host's "set the configuration to zero", since
	 * there is no separate clear message on the wire. */
	g.lib_config = (uint8_t)(g.lib_config & (uint8_t)~config);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t radiant_event_lib_config_get(uint8_t *config)
{
	if (config == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	*config = g.lib_config;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t radiant_event_filter_set(uint16_t filter)
{
	/* Stored and never consulted: a backend that doesn't filter must still
	 * round-trip the value for tools/ant_features.py, and must not pretend
	 * to filter, since loss accounting needs RX_FAIL events to arrive. */
	g.filter = filter;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t radiant_event_filter_get(uint16_t *filter)
{
	if (filter == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	*filter = g.filter;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

void radiant_event_set_ts_policy(enum radiant_event_ts_policy p)
{
	g.ts_policy = p;
}

enum radiant_event_ts_policy radiant_event_get_ts_policy(void)
{
	return g.ts_policy;
}

/* ---------------------------------------------------------------------------
 * The extended tail: the inverse of tools/ant_verify.py's extended_fields().
 * Field order is fixed (channel id, RSSI, receive timestamp), but a field's
 * offset depends on which earlier fields turned up, which is what the flag
 * byte is for; nothing here ever emits a placeholder.
 * ---------------------------------------------------------------------------
 */

/* Byte 2 of the RSSI field: the proximity-search threshold configuration in
 * force. Zero is "proximity search off", which is the only state anything in
 * this project puts a channel in; tools/ant_conformance.py's byte diff is what
 * pins it against a real stick. */
#define RSSI_THRESHOLD_CFG_NONE  0x00u

int radiant_event_build_ext(uint8_t config, const struct radiant_event_ext *in,
			uint8_t *out, size_t out_len)
{
	uint8_t buf[RADIANT_EVENT_EXT_MAX];
	uint8_t n = 1u;   /* buf[0] is the flag byte, filled in last */
	uint8_t flags = 0u;

	if (in == NULL || out == NULL || out_len < RADIANT_EVENT_EXT_MAX) {
		return RADIANT_EVENT_EINVAL;
	}

	if ((config & (uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID) != 0u) {
		flags |= (uint8_t)ANTW_EXT_FLAG_CHANNEL_ID;
		buf[n++] = (uint8_t)(in->id.device_number & 0xFFu);
		buf[n++] = (uint8_t)(in->id.device_number >> 8);
		/* The pairing bit stays. Masking it here would throw away
		 * information the host is entitled to; ant_verify.py and
		 * ant_scan.py mask it themselves when they want the bare
		 * type. */
		buf[n++] = in->id.device_type;
		buf[n++] = in->id.trans_type;
	}

	if ((config & (uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI) != 0u) {
		if (in->has_rssi) {
			flags |= (uint8_t)ANTW_EXT_FLAG_RSSI;
			buf[n++] = (uint8_t)ANTW_RSSI_MEASUREMENT_TYPE_DBM;
			buf[n++] = (uint8_t)in->rssi_dbm;
			buf[n++] = RSSI_THRESHOLD_CFG_NONE;
		} else {
			/* A backend with caps.has_rssi false has no number to
			 * give. Emitting 0 dBm would be a physically
			 * implausible reading that a host would nonetheless
			 * believe, so the field is dropped and the flag bit
			 * stays clear. */
			g.stats.rssi_suppressed++;
		}
	}

	if ((config & (uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP) != 0u) {
		bool usable = in->has_t_sync &&
			      (in->t_sync_exact ||
			       g.ts_policy == RADIANT_EVENT_TS_BEST_EFFORT);

		if (usable) {
			uint16_t ticks = radiant_event_rx_ticks(in->t_sync);

			flags |= (uint8_t)ANTW_EXT_FLAG_RX_TIMESTAMP;
			buf[n++] = (uint8_t)(ticks & 0xFFu);
			buf[n++] = (uint8_t)(ticks >> 8);
		} else {
			/* The downgrade: this wire format's only accuracy signal
			 * is field presence, so an inexact stamp is reported as
			 * no stamp rather than a millisecond number the host would
			 * read as microsecond-scale. Counted so a backend quietly
			 * missing the address event shows up as a number, not a
			 * gap in someone's plot. */
			g.stats.ts_suppressed++;
		}
	}

	if (flags == 0u) {
		/* No flag byte either. This is what keeps a plain broadcast
		 * nine bytes long whatever the library configuration says. */
		return 0;
	}

	buf[0] = flags;
	memcpy(out, buf, n);
	return (int)n;
}

/* ---------------------------------------------------------------------------
 * Posting
 * ---------------------------------------------------------------------------
 */

static int enqueue(const struct radiant_event_slot *s)
{
	unsigned int key;
	uint32_t     used;
	int          rc = RADIANT_EVENT_OK;

	if (!g.ready) {
		bump(&g.stats.rejected_invalid);
		return RADIANT_EVENT_ESTATE;
	}

	key = radiant_event_crit_enter();
	used = g.head - g.tail;
	if (used >= (uint32_t)RADIANT_EVENT_QUEUE_DEPTH) {
		/* Drop the newest, not the oldest: a host that loses the head
		 * of a burst is worse off than one that loses its tail. Never
		 * silent - overflow_pending becomes an ANTW_EVENT_QUE_OVERFLOW
		 * at the head of the next drain. */
		g.overflow_pending = true;
		g.stats.dropped_full++;
		rc = RADIANT_EVENT_ENOSPC;
	} else {
		g.ring[g.head & RING_MASK] = *s;
		g.head++;
		used++;
		if (used > (uint32_t)g.stats.high_water) {
			g.stats.high_water = (uint16_t)used;
		}
		g.stats.posted++;
	}
	radiant_event_crit_exit(key);

	if (rc == RADIANT_EVENT_OK) {
		/* Outside the section: the port's wakeup is allowed to be a
		 * semaphore give, and holding interrupts off across it buys
		 * nothing. */
		radiant_event_wakeup();
	}
	return rc;
}

int radiant_event_post_rx(const struct radiant_event_rx *rx)
{
	struct radiant_event_slot s;
	uint8_t               chan_byte;

	if (rx == NULL || rx->hal == NULL || rx->payload == NULL) {
		bump(&g.stats.rejected_invalid);
		return RADIANT_EVENT_EINVAL;
	}

	/* The CRC gate - why this function takes the HAL event, not decoded
	 * values. A 3-byte search address triggers the matcher on noise
	 * (Spike A: -54..-101 dBm, zero valid frames), so a CRC-failed frame
	 * must never become an event; duplicated here as the last gate before
	 * the host. */
	if (rx->hal->status == RADIANT_RADIO_STATUS_CRC_FAIL) {
		bump(&g.stats.rejected_crc);
		return RADIANT_EVENT_ECRC;
	}
	if (rx->hal->status != RADIANT_RADIO_STATUS_OK) {
		bump(&g.stats.rejected_invalid);
		return RADIANT_EVENT_EINVAL;
	}

	if (rx->channel > (uint8_t)RADIANT_EVENT_CHANNEL_MAX ||
	    rx->payload_len == 0u ||
	    rx->payload_len > (uint8_t)RADIANT_EVENT_PAYLOAD_MAX) {
		bump(&g.stats.rejected_invalid);
		return RADIANT_EVENT_EINVAL;
	}

	switch (rx->msg_id) {
	case ANTW_MESG_BROADCAST_DATA_ID:
	case ANTW_MESG_ACKNOWLEDGED_DATA_ID:
	case ANTW_MESG_BURST_DATA_ID:
		break;
	default:
		bump(&g.stats.rejected_invalid);
		return RADIANT_EVENT_EINVAL;
	}

	chan_byte = rx->channel;
	if (rx->msg_id == (uint8_t)ANTW_MESG_BURST_DATA_ID) {
		if (rx->burst_seq > (uint8_t)ANTW_BURST_HEADER_SEQ_MASK) {
			bump(&g.stats.rejected_invalid);
			return RADIANT_EVENT_EINVAL;
		}
		chan_byte = (uint8_t)(chan_byte |
				      (uint8_t)(rx->burst_seq
						<< ANTW_BURST_HEADER_SEQ_SHIFT));
		if (rx->burst_last) {
			chan_byte = (uint8_t)(chan_byte |
					      (uint8_t)ANTW_BURST_HEADER_LAST);
		}
	}

	memset(&s, 0, sizeof(s));
	s.msg_id = rx->msg_id;
	s.body[0] = chan_byte;
	memcpy(&s.body[1], rx->payload, rx->payload_len);
	s.body_len = (uint8_t)(ANTW_CHANNEL_NUM_SIZE + rx->payload_len);

	s.device_number = rx->id.device_number;
	s.device_type = rx->id.device_type;
	s.trans_type = rx->id.trans_type;

	/* The timestamp is the backend's t_sync, copied here in the callback.
	 * Nothing below this point reads a clock - a grep that finds a call to
	 * the HAL's current-time function in this file has found a bug. */
	s.t_sync = rx->hal->t_sync;
	s.flags = (uint8_t)(SLOT_EXT | SLOT_HAS_TS);
	if (rx->hal->t_sync_exact) {
		s.flags = (uint8_t)(s.flags | SLOT_TS_EXACT);
	}
	if (rx->hal->has_rssi) {
		s.flags = (uint8_t)(s.flags | SLOT_HAS_RSSI);
		s.rssi_dbm = rx->hal->rssi_dbm;
	}

	return enqueue(&s);
}

int radiant_event_post_raw(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	struct radiant_event_slot s;

	if (len > (uint8_t)RADIANT_EVENT_BODY_MAX || (body == NULL && len > 0u)) {
		bump(&g.stats.rejected_invalid);
		return RADIANT_EVENT_EINVAL;
	}

	memset(&s, 0, sizeof(s));
	s.msg_id = msg_id;
	s.body_len = len;
	if (len > 0u) {
		memcpy(s.body, body, len);
	}

	return enqueue(&s);
}

int radiant_event_post_channel_event(uint8_t channel, uint8_t code)
{
	uint8_t body[3];
	int     rc;

	if (channel > (uint8_t)RADIANT_EVENT_CHANNEL_MAX) {
		bump(&g.stats.rejected_invalid);
		return RADIANT_EVENT_EINVAL;
	}

	/* [channel][MESG_EVENT_ID][code] under MESG_RESPONSE_EVENT_ID: byte 1 =
	 * 0x01 distinguishes an unsolicited channel event from a reply to a
	 * command; the bridge switches on it for the three burst release codes. */
	body[0] = channel;
	body[1] = (uint8_t)ANTW_MESG_EVENT_ID;
	body[2] = code;

	rc = radiant_event_post_raw((uint8_t)ANTW_MESG_RESPONSE_EVENT_ID, body,
				(uint8_t)sizeof(body));
	if (rc != RADIANT_EVENT_OK) {
		return rc;
	}

	/* The three that carry buffer ownership as well as status, counted so
	 * "released exactly once per accepted block" is a number comparison,
	 * not a log reading. */
	switch (code) {
	case ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK:
		bump(&g.stats.next_data_block);
		break;
	case ANTW_EVENT_TRANSFER_TX_COMPLETED:
		bump(&g.stats.tx_completed);
		break;
	case ANTW_EVENT_TRANSFER_TX_FAILED:
		bump(&g.stats.tx_failed);
		break;
	default:
		break;
	}

	return RADIANT_EVENT_OK;
}

/* ---------------------------------------------------------------------------
 * Draining
 * ---------------------------------------------------------------------------
 */

static void deliver(const struct radiant_event_slot *s)
{
	uint8_t         out[RADIANT_EVENT_MSG_MAX];
	struct antr_msg msg;
	uint8_t         n = s->body_len;

	if (n > 0u) {
		memcpy(out, s->body, n);
	}

	if ((s->flags & SLOT_EXT) != 0u && g.lib_config != 0u) {
		struct radiant_event_ext ext;
		int                  ext_n;

		ext.id.device_number = s->device_number;
		ext.id.device_type = s->device_type;
		ext.id.trans_type = s->trans_type;
		ext.has_rssi = (s->flags & SLOT_HAS_RSSI) != 0u;
		ext.rssi_dbm = s->rssi_dbm;
		ext.has_t_sync = (s->flags & SLOT_HAS_TS) != 0u;
		ext.t_sync_exact = (s->flags & SLOT_TS_EXACT) != 0u;
		ext.t_sync = s->t_sync;

		ext_n = radiant_event_build_ext(g.lib_config, &ext, &out[n],
					    sizeof(out) - (size_t)n);
		if (ext_n > 0) {
			n = (uint8_t)(n + (uint8_t)ext_n);
		}
	}

	msg.id = s->msg_id;
	msg.len = n;
	msg.data = out;
	antr_on_message(&msg);
	g.stats.delivered++;
}

static void deliver_overflow(void)
{
	uint8_t         body[3];
	struct antr_msg msg;

	/* Channel 0: the loss is a property of the queue, not of a channel,
	 * and there is no wire encoding for "no channel". */
	body[0] = 0u;
	body[1] = (uint8_t)ANTW_MESG_EVENT_ID;
	body[2] = (uint8_t)ANTW_EVENT_QUE_OVERFLOW;

	msg.id = (uint8_t)ANTW_MESG_RESPONSE_EVENT_ID;
	msg.len = (uint8_t)sizeof(body);
	msg.data = body;
	antr_on_message(&msg);

	g.stats.delivered++;
	g.stats.overflow_marks++;
}

bool radiant_event_pending(void)
{
	unsigned int key;
	bool         any;

	key = radiant_event_crit_enter();
	any = (g.head != g.tail) || g.overflow_pending;
	radiant_event_crit_exit(key);
	return any;
}

uint16_t radiant_event_queued(void)
{
	unsigned int key;
	uint32_t     used;

	key = radiant_event_crit_enter();
	used = g.head - g.tail;
	radiant_event_crit_exit(key);
	return (uint16_t)used;
}

uint32_t radiant_event_drain(uint32_t max_msgs)
{
	unsigned int key;
	uint32_t     sent = 0u;
	bool         overflow;

	key = radiant_event_crit_enter();
	if (!g.ready || g.draining) {
		/* Re-entered from inside antr_on_message(). The bridge must
		 * never be called recursively (docs/sdk-ant-contract.md);
		 * returning zero keeps that true even if a future caller tries. */
		radiant_event_crit_exit(key);
		return 0u;
	}
	g.draining = true;
	overflow = g.overflow_pending;
	g.overflow_pending = false;
	radiant_event_crit_exit(key);

	if (overflow) {
		deliver_overflow();
		sent++;
	}

	while (max_msgs == 0u || sent < max_msgs) {
		struct radiant_event_slot s;

		key = radiant_event_crit_enter();
		if (g.head == g.tail) {
			radiant_event_crit_exit(key);
			break;
		}
		s = g.ring[g.tail & RING_MASK];
		g.tail++;
		radiant_event_crit_exit(key);

		/* Outside the section: antr_on_message() must not run with
		 * interrupts masked. */
		deliver(&s);
		sent++;
	}

	key = radiant_event_crit_enter();
	g.draining = false;
	radiant_event_crit_exit(key);

	return sent;
}

const struct radiant_event_stats *radiant_event_stats_get(void)
{
	return &g.stats;
}
