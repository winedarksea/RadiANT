/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_sched.h - which page goes in the next message slot.
 *
 * Provenance: docs/radiant-telemetry.md section 6 ("Interleave cadence: every
 * 121 messages", and the descriptor-set consecutiveness argument) and section
 * 8 ("Descriptor cadence in sparse mode"). That document is this project's own
 * written specification. No adopter-gated ANT+ device profile document was
 * read for this file, no sdk-ant source was consulted, and nothing here
 * derives from libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * This is a PAGE scheduler, not a radio scheduler
 * ---------------------------------------------------------------------------
 * radiant_core/src/radiant_sched.c decides which operation the radio performs
 * next and at which absolute microsecond. This file decides which eight bytes
 * go out when that operation is a master's transmit slot. They are two
 * different questions and they stay in two different files: nothing here knows
 * a microsecond, and nothing there knows a page number.
 *
 * The whole of the coupling is that a master calls profile_sched_next() to
 * fill the body it then hands to radiant_sched_request_tx(). That is a
 * function call, and it needed no new scheduler API to exist - see the note at
 * the end of this comment.
 *
 * ---------------------------------------------------------------------------
 * The cadence, and why 121 rather than 65
 * ---------------------------------------------------------------------------
 * Page 80 at message 119, page 81 at message 120, cycle length 121. The
 * generic ANT+ guidance says 65; a real certified sensor does 121, and
 * tools/ant_pages.py's COMMON_PAGE_INTERVAL carries the same number and the
 * same reason. A node that sends common pages twice as often as the profile
 * requires is not being careful, it is spending radio energy.
 *
 * The descriptor set is sent CONSECUTIVELY, at messages 0..D, and that is the
 * decision worth defending. A node with 8 fields has D = 9; spreading those
 * frames one per cycle would make a receiver wait 10 x 121 messages - over
 * eight minutes at 2 Hz, over an hour at 0.25 Hz - before it could decode
 * anything at all. Consecutive costs 10 slots out of 121 and makes a
 * mid-stream join take one cycle.
 *
 * ---------------------------------------------------------------------------
 * THE SLOT-INSERTION SEAM, which is why this file is more general than this
 * plan needs
 * ---------------------------------------------------------------------------
 * Two plans need this engine. This one runs it for device type 0x60; the ANT+
 * compatibility work needs the same 119/120/121 interleave for device types
 * 0x78 and 0x0B, and wants to insert its own beacon and attestation pages into
 * the rotation. Two implementations of one cadence rule is one implementation
 * and one drift, so there is one engine and the other plan is a CLIENT of it:
 * it registers a claim callback and takes the slots it wants, rather than
 * forking the rotation.
 *
 * The seam is deliberately one function pointer and nothing else. A client
 * gets FIRST REFUSAL on any slot the default rotation would have filled with a
 * data page - and, on a sparse node, on any slot that would otherwise be
 * silent. It never gets offered message 119, message 120, or a descriptor
 * frame, because those three are the cadence rule itself: a client that could
 * displace a common page or half a descriptor set would be forking the
 * rotation with extra steps.
 *
 * A client profile family with no descriptor at all - which is what an ANT+
 * compatibility type is - configures cfg.desc = NULL and gets the interleave
 * with the descriptor slots simply absent. That is the case this file is built
 * to serve without a retrofit, and it is why it is written now rather than
 * when the second caller shows up.
 *
 * ---------------------------------------------------------------------------
 * What this needed from radiant_sched.c: nothing
 * ---------------------------------------------------------------------------
 * Recorded here because it was an open question and the answer is worth
 * having in writing. A page-rotation client claims a radio slot by posting a
 * transmit against a channel and posting the next one from inside the
 * completion callback, which is what radiant_sched.h already documents as the
 * intended low-jitter path. Sparse mode - "the node keeps a channel period
 * configured and simply declines to transmit in most of its slots" - is
 * expressed by not posting, which needs no scheduler concept at all.
 * radiant_core/tests/src/test_profiles.c drives a real 0x60 master through
 * radiant_sched.c against the mock radio to keep that claim honest rather than
 * merely asserted.
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
	/* Transmit nothing. Only a sparse node ever sees this, and seeing it
	 * is the whole of the energy saving: the slot exists, the timer is
	 * still running, and the radio stays off. */
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
 * What the node supplies.
 *
 * Every page builder is a callback rather than a buffer, because a body must
 * be built at the moment it goes out: the event counter is the packet index
 * and the values are the values at the instant of transmission. A cached body
 * is a repeated counter, and under a secured channel a repeated counter within
 * one (epoch, device number) is keystream reuse.
 *
 * A NULL builder means the node does not emit that page. Suppressing page 82
 * outright is not an omission - it is the privacy rule of section 6, and it
 * is the mitigation that matters most for a node whose whole payload is a
 * stable identity.
 */
struct profile_sched_cfg {
	/* NULL for a client profile family with no descriptor - an ANT+
	 * compatibility device type. The descriptor slots then do not exist
	 * and the rest of the cadence is unchanged. */
	const struct profile_descriptor *desc;

	/* Build the body of one data page. `counter` is the event counter the
	 * engine has already chosen for this transmission; put it in byte [1].
	 * Return 0, or negative to decline (the slot becomes idle). */
	int (*data_page)(uint8_t page, uint8_t counter, uint8_t *body, void *user);

	int (*common_80)(uint8_t *body, void *user);
	int (*common_81)(uint8_t *body, void *user);
	int (*common_82)(uint8_t *body, void *user);

	/* Emit page 82 once every N data slots. 0 means never, which together
	 * with common_82 == NULL is the two ways to say the same thing - a
	 * node that has a battery page and a node that refuses to broadcast a
	 * monotone operating-time counter. */
	uint16_t common_82_every;

	void *user;
};

struct profile_sched {
	struct profile_sched_cfg cfg;

	struct profile_sched_client client;
	bool                       have_client;

	/* The encoded descriptor set, built once at init. It is at most 128
	 * bytes and it changes only when the schema does, so encoding it per
	 * slot would be work proportional to the field count in the hot path
	 * for no benefit. */
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
 * so a malformed schema is a configuration error at start-up rather than a
 * node that transmits nonsense for a decade.
 */
int profile_sched_init(struct profile_sched *ps,
		       const struct profile_sched_cfg *cfg);

/* Install or replace the client. NULL removes it. */
int profile_sched_set_client(struct profile_sched *ps,
			     const struct profile_sched_client *client);

/*
 * Decide the next slot and fill `body` with eight bytes.
 *
 * `body` is untouched for PROFILE_SLOT_IDLE. Every other return means "put
 * these eight bytes on the air in this slot".
 */
enum profile_slot_kind profile_sched_next(struct profile_sched *ps, uint8_t *body);

/*
 * Send the whole descriptor set starting at the next slot.
 *
 * Three callers, all from the specification: a schema change (section 6),
 * command 0x06 "send the descriptor set now" (section 9), and every heartbeat
 * of a sparse node (section 8, which the engine does for itself). The first
 * two are what a receiver that just joined uses to get a schema without
 * waiting for the next cycle.
 */
void profile_sched_request_descriptor(struct profile_sched *ps);

/*
 * A sparse node has something to say. The page is repeated k times, one per
 * slot, k from the descriptor's repeat code - because there is no
 * retransmission and a scanning receiver may be mid-dwell elsewhere. The event
 * counter is what lets the receiver deduplicate the repeats, and it is the
 * same counter that detects loss in periodic mode: one mechanism, two jobs.
 *
 * Ignored on a node that is not sparse.
 */
int profile_sched_post_event(struct profile_sched *ps, uint8_t page);

/*
 * Heartbeat now, for an ASYNCHRONOUS sparse node - channel period 0x0000, no
 * slot discipline at all, so the engine has no grid to count heartbeat
 * intervals on and the node's own timer must say when.
 *
 * A slot-aligned sparse node needs this only to force an early heartbeat; it
 * counts its own.
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
