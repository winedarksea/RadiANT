/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_sched.h - which page goes in the next message slot.
 *
 * Provenance: docs/radiant-telemetry.md section 6 ("Interleave cadence: every
 * 121 messages", and the descriptor-set consecutiveness argument) and section
 * 8 ("Descriptor cadence in sparse mode"). This project's own written
 * specification. No ANT+ device profile document was read for
 * this file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * This is a PAGE scheduler, not a radio scheduler
 * ---------------------------------------------------------------------------
 * radiant/src/radiant_sched.c decides which operation the radio performs
 * next and at which absolute microsecond; this file decides which eight bytes
 * go out for a master's transmit slot. Nothing here knows a microsecond and
 * nothing there knows a page number - the only coupling is that a master
 * calls profile_sched_next() to fill the body it hands to
 * radiant_sched_request_tx(), a plain function call needing no new scheduler
 * API.
 *
 * ---------------------------------------------------------------------------
 * The cadence, and why 121 rather than 65
 * ---------------------------------------------------------------------------
 * Page 80 at message 119, page 81 at message 120, cycle length 121 (a real
 * certified sensor's cadence; the generic ANT+ guidance says 65, and a node
 * that follows that is spending radio energy it doesn't need to).
 *
 * The descriptor set is sent CONSECUTIVELY, at messages 0..D. A node with 8
 * fields has D = 9; spreading those frames one per cycle would make a
 * receiver wait 10 x 121 messages - over eight minutes at 2 Hz - before it
 * could decode anything. Consecutive costs 10 slots out of 121 and makes a
 * mid-stream join take one cycle.
 *
 * ---------------------------------------------------------------------------
 * The slot-insertion seam
 * ---------------------------------------------------------------------------
 * Two plans need this engine: this one runs it for device type 0x60, and the
 * ANT+ compatibility work needs the same 119/120/121 interleave for device
 * types 0x78 and 0x0B while inserting its own beacon and attestation pages.
 * Rather than a second implementation of the cadence rule, the other plan is
 * a CLIENT: it registers a claim callback and takes the slots it wants.
 *
 * The seam is one function pointer. A client gets first refusal on any slot
 * the default rotation would fill with a data page (and, on a sparse node,
 * any otherwise-silent slot) - never message 119, 120, or a descriptor frame,
 * since those are the cadence rule itself.
 *
 * A client family with no descriptor at all - an ANT+ compatibility type -
 * configures cfg.desc = NULL and gets the interleave with the descriptor
 * slots simply absent.
 *
 * ---------------------------------------------------------------------------
 * What this needed from radiant_sched.c: nothing
 * ---------------------------------------------------------------------------
 * A page-rotation client claims a radio slot by posting a transmit and
 * posting the next one from inside the completion callback - the intended
 * low-jitter path radiant_sched.h already documents. Sparse mode (the node
 * keeps a channel period configured but declines to transmit in most slots)
 * is expressed by not posting, needing no scheduler concept at all.
 * radiant/tests/src/test_profiles.c drives a real 0x60 master through
 * radiant_sched.c against the mock radio to keep that claim tested rather
 * than merely asserted.
 */

#ifndef RADIANT_PROFILE_SCHED_H_
#define RADIANT_PROFILE_SCHED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What profile_sched_next() decided this slot is. */
enum profile_slot_kind {
	/* Transmit nothing. Only a sparse node sees this: the slot exists,
	 * the timer keeps running, and the radio stays off. */
	PROFILE_SLOT_IDLE = 0,
	PROFILE_SLOT_DESCRIPTOR,
	PROFILE_SLOT_DATA,
	PROFILE_SLOT_COMMON_80,
	PROFILE_SLOT_COMMON_81,
	PROFILE_SLOT_COMMON_82,
	/* A client profile family claimed it through the seam. */
	PROFILE_SLOT_CLIENT
};

/*
 * The seam. One callback, called with the message index within the cycle.
 *
 * Return true and fill body[8] to claim the slot. Return false and the default
 * rotation has it back, at no cost - the engine has not committed to anything
 * by the time it asks.
 */
struct profile_sched_client {
	bool (*claim)(uint32_t m, uint8_t *body, void *user);
	void *user;
};

/*
 * The downlink hook - the second seam, needed because the schedule frame's
 * downlink phase is measured from the t_sync of the frame that carries it,
 * while this engine encodes the descriptor set once at init and retransmits
 * the bytes; a retransmitted phase would point at a window already gone.
 *
 * One function pointer, same shape as the client seam. When the engine is
 * about to hand out the schedule frame it asks the hook for the phase to
 * announce and rewrites that one field of the outgoing copy. A negative
 * return leaves the bytes exactly as the encoder wrote them.
 *
 * Asked only for the schedule frame of a descriptor whose block actually
 * announces a window - never speculatively, so a node announcing nothing
 * pays nothing.
 */
struct profile_sched_downlink {
	/* The phase to announce, in counts of 1/32768 s, for a schedule frame
	 * whose carrier t_sync is `t_sync`. Negative leaves the encoded bytes
	 * alone. profile_cmd_sched_phase() in profile_command.h is the
	 * implementation this tree ships. */
	int32_t (*phase)(radiant_time_t t_sync, void *user);
	void *user;
};

/*
 * What the node supplies.
 *
 * Every page builder is a callback rather than a buffer, because a body must
 * be built at the moment it goes out: the event counter is the packet index,
 * and a cached body would repeat a counter, which under a secured channel is
 * keystream reuse.
 *
 * A NULL builder means the node does not emit that page. Suppressing page 82
 * outright is the privacy rule of section 6, not an omission.
 */
struct profile_sched_cfg {
	/* NULL for a client profile family with no descriptor - an ANT+
	 * compatibility device type. The descriptor slots then do not exist
	 * and the rest of the cadence is unchanged. */
	const struct profile_descriptor *desc;

	/*
	 * The data-page rotation for a family with NO DESCRIPTOR, copied at
	 * init. Ignored entirely when `desc` is non-NULL, where the rotation
	 * is derived from the schema instead.
	 *
	 * Exists because an ANT+ compatibility type's page numbers are fixed
	 * by somebody else's document, not announced by the node, so there is
	 * nothing to derive them from. Without it a descriptor-less family
	 * would reach take_rotation() with no pages and every data slot idle.
	 *
	 * A one-entry rotation is normal, not degenerate: heart rate sends its
	 * main page in almost every data slot and swaps in a background page
	 * on its own cadence - the profile's rule, not this file's.
	 */
	const uint8_t *pages;
	uint8_t        n_pages;

	/* Build the body of one data page. `counter` is the event counter the
	 * engine has already chosen for this transmission; put it in byte [1].
	 * Return 0, or negative to decline (the slot becomes idle). */
	int (*data_page)(uint8_t page, uint8_t counter, uint8_t *body, void *user);

	int (*common_80)(uint8_t *body, void *user);
	int (*common_81)(uint8_t *body, void *user);
	int (*common_82)(uint8_t *body, void *user);

	/* Emit page 82 once every N data slots. 0 (or common_82 == NULL) means
	 * never - a node refusing to broadcast a monotone operating-time
	 * counter. */
	uint16_t common_82_every;

	void *user;
};

struct profile_sched {
	struct profile_sched_cfg cfg;

	struct profile_sched_client client;
	bool                       have_client;

	struct profile_sched_downlink dl;
	bool                          have_dl;
	/* Frame index of the schedule frame within the encoded set, or -1 when
	 * the descriptor has none. Latched at init so the hot path compares
	 * an integer rather than re-deriving a layout. */
	int8_t                        sched_frame;

	/* The encoded descriptor set, built once at init: at most 128 bytes,
	 * changing only when the schema does. */
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	uint8_t n_frames;

	uint8_t pages[PROFILE_TLM_PAGE_DATA_LAST]; /* the rotation, ascending */
	uint8_t n_pages;
	uint8_t page_cursor;

	uint32_t m;       /* message index; mod 121 in periodic mode */
	uint8_t  counter; /* event counter: data pages only, wraps at 256 */

	bool    burst;       /* a descriptor set is being sent */
	uint8_t burst_index;

	/* Sparse. */
	bool     sparse;
	uint32_t hb_slots;  /* slots between heartbeats; 0 = driven externally */
	uint32_t since_hb;
	bool     hb_pending;
	bool     want_82;
	uint8_t  event_page;
	uint8_t  event_left;

	uint16_t data_since_82;
};

/*
 * Latch the configuration and encode the descriptor set.
 *
 * Returns 0, or whatever profile_desc_encode() refused the descriptor for -
 * a malformed schema is a start-up error rather than a node transmitting
 * nonsense for a decade.
 */
int profile_sched_init(struct profile_sched *ps,
		       const struct profile_sched_cfg *cfg);

/* Install or replace the client. NULL removes it. */
int profile_sched_set_client(struct profile_sched *ps,
			     const struct profile_sched_client *client);

/*
 * Install or replace the downlink hook. NULL removes it.
 *
 * -ENOENT when the configured descriptor carries no schedule frame - a caller
 * that thinks it announced a window and did not, worth catching at start-up
 * rather than a downlink that silently never opens.
 */
int profile_sched_set_downlink(struct profile_sched *ps,
			       const struct profile_sched_downlink *dl);

/*
 * Decide the next slot and fill `body` with eight bytes.
 *
 * `body` is untouched for PROFILE_SLOT_IDLE. Every other return means "put
 * these eight bytes on the air in this slot".
 *
 * This form knows no time and therefore cannot re-phase a downlink window; it
 * is profile_sched_next_at() with RADIANT_TIME_NEVER, "do not ask the hook".
 */
enum profile_slot_kind profile_sched_next(struct profile_sched *ps, uint8_t *body);

/*
 * The same decision, told when the slot will actually be transmitted.
 *
 * `t_sync` is the instant the caller is about to hand to
 * radiant_sched_request_tx() as `t_sync_at` - the one microsecond value this
 * scheduler ever sees, and it only forwards it to the downlink hook rather
 * than interpreting it.
 *
 * RADIANT_TIME_NEVER means "not known", the value the hookless form passes.
 */
enum profile_slot_kind profile_sched_next_at(struct profile_sched *ps,
					     uint8_t *body, radiant_time_t t_sync);

/*
 * Send the whole descriptor set starting at the next slot.
 *
 * Three callers: a schema change (section 6), command 0x06 "send the
 * descriptor set now" (section 9), and every sparse-node heartbeat (section 8,
 * done by the engine itself).
 */
void profile_sched_request_descriptor(struct profile_sched *ps);

/*
 * A sparse node has something to say. The page repeats k times, one per slot
 * (k from the descriptor's repeat code), since there is no retransmission and
 * a scanning receiver may be mid-dwell elsewhere. The event counter lets a
 * receiver deduplicate the repeats - the same counter that detects loss in
 * periodic mode.
 *
 * Ignored on a node that is not sparse.
 */
int profile_sched_post_event(struct profile_sched *ps, uint8_t page);

/*
 * Heartbeat now, for an asynchronous sparse node - channel period 0x0000, no
 * slot discipline, so the node's own timer must say when.
 *
 * A slot-aligned sparse node needs this only to force an early heartbeat; it
 * counts its own otherwise.
 */
void profile_sched_post_heartbeat(struct profile_sched *ps);

/* The event counter the next data page will carry. */
uint8_t profile_sched_counter(const struct profile_sched *ps);

/* Message index of the slot profile_sched_next() will serve next; mod 121 in
 * periodic mode, free-running in sparse mode. */
uint32_t profile_sched_m(const struct profile_sched *ps);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_SCHED_H_ */
