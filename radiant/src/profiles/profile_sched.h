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
 * This is a PAGE scheduler, not a radio scheduler.
 * radiant/src/radiant_sched.c decides which operation the radio performs
 * next and at which absolute microsecond; this file decides which eight bytes
 * go out for a master's transmit slot. Nothing here knows a microsecond and
 * nothing there knows a page number - the only coupling is that a master
 * calls profile_sched_next() to fill the body it hands to
 * radiant_sched_request_tx(), a plain function call needing no new scheduler
 * API.
 *
 * The cadence, and why 121 rather than 65: page 80 at message 119, page 81 at
 * message 120, cycle length 121 (a real certified sensor's cadence; the
 * generic ANT+ guidance says 65, and a node that follows that is spending
 * radio energy it doesn't need to).
 *
 * The descriptor set is sent CONSECUTIVELY, at messages 0..D. A node with 8
 * fields has D = 9; spreading those frames one per cycle would make a
 * receiver wait 10 x 121 messages - over eight minutes at 2 Hz - before it
 * could decode anything. Consecutive costs 10 slots out of 121 and makes a
 * mid-stream join take one cycle.
 *
 * The slot-insertion seam: two plans need this engine, this one for device
 * type 0x60, and the ANT+ compatibility work needing the same 119/120/121
 * interleave for device types 0x78 and 0x0B while inserting its own beacon
 * and attestation pages. Rather than a second implementation of the cadence
 * rule, the other plan is a CLIENT: it registers a claim callback and takes
 * the slots it wants.
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
 * What this needed from radiant_sched.c: nothing. A page-rotation client
 * claims a radio slot by posting a transmit and posting the next one from
 * inside the completion callback - the intended low-jitter path
 * radiant_sched.h already documents. Sparse mode (the node keeps a channel
 * period configured but declines to transmit in most slots) is expressed by
 * not posting, needing no scheduler concept at all.
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
 * be built at the moment it goes out: the event counter is the slot index,
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
	 * engine has already chosen for this slot - the slot index, which has
	 * also advanced over every descriptor, common and idle slot; put it in
	 * byte [1]. Return 0, or negative to decline (the slot becomes idle,
	 * and the counter still advances). */
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
	 * changing only when the schema does - or, on a sealed node, when
	 * profile_sched_reseal() re-tags it for a new epoch. */
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	uint8_t n_frames;

	uint8_t pages[PROFILE_TLM_PAGE_DATA_LAST]; /* the rotation, ascending */
	uint8_t n_pages;
	uint8_t page_cursor;

	uint32_t m;       /* message index; mod 121 in periodic mode */
	uint8_t  counter; /* event counter: the slot index, +1 every slot,
			   * wraps at 256 */

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

/*
 * The same, for a node whose descriptor announces X_CONF or X_AUTH: the set
 * is encoded with profile_desc_encode_auth(), authentication frame last, into
 * the same 16-frame buffer (sealing costs no RAM). cfg->desc must not be NULL.
 *
 * Returns 0, or whatever profile_desc_encode_auth() refused - including
 * -ENOTSUP from a build without CONFIG_RADIANT_SEC, and -EINVAL for a
 * descriptor with no transform bit (profile_sched_init() is the path for it).
 *
 * A separate function rather than a field in the config, so a standard device
 * - which calls profile_sched_init() - never references the sealed encoder
 * and --gc-sections drops it.
 *
 * TWO RULES THE NODE APPLICATION OWNS:
 *
 *   1. RESEAL ON EVERY EPOCH CHANGE. The tag binds the epoch, so call
 *      profile_sched_reseal() after every radiant_sec_set_epoch(), and when
 *      radiant_sec_get_state() shows the epoch advanced on a counter wrap.
 *      Until it does, the descriptor fails verification at every keyed
 *      receiver - the safe failure, but a failure.
 *   2. THE LOCK. init_auth and reseal run a KDF and a CMAC on the software
 *      AES's single key schedule, so they must run under the same lock as
 *      every other radiant_sec call (api_lock in the applications) - the
 *      hazard TODO item 8 records for the host commands.
 *
 * And one refusal: profile_sched_set_downlink() returns -ENOTSUP on a sealed
 * set, because re-phasing the schedule frame per transmission would change
 * a MAC'd body. A sealed node announcing a downlink window announces it with
 * the phase it was encoded with.
 */
int profile_sched_init_auth(struct profile_sched *ps,
			    const struct profile_sched_cfg *cfg,
			    const struct profile_desc_auth *a);

/*
 * Re-encode a sealed set for a new epoch (or a new epoch_is_time), in place.
 *
 * Refuses (-EINVAL) a set that was not sealed and a re-encode whose frame
 * count differs - a different shape is a schema change, which is
 * profile_sched_init_auth()'s job. A refusal leaves the set on the air as it
 * was. If a burst is in progress it restarts from frame 0, so no receiver can
 * assemble a set that mixes the two epochs.
 *
 * Leave the descriptor's own `epoch` zero: the sealed encoder refuses one
 * that disagrees with a->epoch. Same lock rule as profile_sched_init_auth().
 * Uses 128 bytes of stack for the scratch copy.
 */
int profile_sched_reseal(struct profile_sched *ps,
			 const struct profile_desc_auth *a);

/* Install or replace the client. NULL removes it. */
int profile_sched_set_client(struct profile_sched *ps,
			     const struct profile_sched_client *client);

/*
 * Install or replace the downlink hook. NULL removes it.
 *
 * -ENOENT when the configured descriptor carries no schedule frame - a caller
 * that thinks it announced a window and did not, worth catching at start-up
 * rather than a downlink that silently never opens.
 *
 * -ENOTSUP on a sealed set (profile_sched_init_auth()): the schedule frame is
 * covered by the authentication frame's MAC, and a phase rewritten per
 * transmission would fail verification every time.
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

/* The event counter the next slot will carry - the slot index, whatever that
 * slot turns out to hold. */
uint8_t profile_sched_counter(const struct profile_sched *ps);

/* Message index of the slot profile_sched_next() will serve next; mod 121 in
 * periodic mode, free-running in sparse mode. */
uint32_t profile_sched_m(const struct profile_sched *ps);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_SCHED_H_ */
