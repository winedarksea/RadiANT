/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_compat.h - the RadiANT compat pages, inserted through
 * profile_sched.c's client seam.
 *
 * Provenance: docs/radiant-security.md section 11 (normative for the bytes),
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md (which
 * pins the beacon layout, the two attestation subtypes and the two-page
 * allocation) and docs/profile-registry.md (which registers 0x70-0x72). Its
 * byte-for-byte mirror is tools/ant_pages.py's encode_compat_beacon(),
 * encode_compat_attest_tier1() and encode_compat_attest_tier2(). All of those
 * are this project's own documents and code. No ANT+ device
 * profile document was read for this file.
 *
 * ---------------------------------------------------------------------------
 * THIS FILE EXISTS SO profile_hr.c AND profile_power.c DO NOT HAVE TO
 * ---------------------------------------------------------------------------
 * The plan's boundary is "the profile modules themselves never call
 * radiant_sec", and something has to. profile_sched.c owns the shared
 * 119/120/121 rotation and must not depend on the compatibility work;
 * profile_hr.c/profile_power.c must stay byte-exact ANT+ sensors with no key.
 * So this file is the client: it knows the page numbers and frame layouts,
 * radiant_sec_compat.c knows the cryptography and no page number, and
 * profile_hr.c/profile_power.c know neither and call neither:
 *
 *   profile_hr.c / profile_power.c  ->  profile_compat.c  ->  radiant_sec_compat.c
 *          (no crypto)                  (no ANT+ page)         (no page at all)
 *
 * Enforced structurally: nothing in this header names a radiant_sec type, so
 * including it never transitively reaches a radiant header.
 * tools/test_compat_capture.py greps for that.
 *
 * ---------------------------------------------------------------------------
 * What goes on the air, and what it costs
 * ---------------------------------------------------------------------------
 *   0x70  capability beacon, two frames, one per 121-message cycle. 0.8% of
 *         slots.
 *   0x71  Tier I identity attestation, one page every T seconds - decoupled
 *         from the data rate. 1.2% of slots at 4 Hz, T = 20 s.
 *   0x72  Tier II data attestation, off by default, one page per N
 *         transmitted messages.
 *
 * 2.0% of slots in the default configuration, against ANT+'s own 1.65% for
 * common pages 80/81.
 *
 * ---------------------------------------------------------------------------
 * The one interaction the seam's rule creates
 * ---------------------------------------------------------------------------
 * The seam never offers message 119, 120, or a descriptor frame. Tier I
 * doesn't care (time-derived counter, slip is invisible); the beacon doesn't
 * either (a displaced frame is owed and rides the next offered slot, keeping
 * one frame per cycle). Tier II does care: its window closes at exactly the
 * Nth transmitted message with no slack, so a window landing on 119/120
 * closes with no tag - radiant_sec_compat.c absorbs it (next window starts
 * clean), a receiver reports one unverified window, same cost as a lost
 * packet in that tier. With N=8 and a 121-message cycle that's roughly two
 * windows in fifteen: bounded, off by default, and the alternative (a client
 * that can displace a common page) is forking the rotation with extra steps.
 */

#ifndef RADIANT_PROFILE_COMPAT_H_
#define RADIANT_PROFILE_COMPAT_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The allocation, registered in docs/profile-registry.md. Two numbers, not
 * three: SWITCH/RETURN rides frames 2/3 of the beacon's own set instead,
 * since heart rate's byte 0 uses bit 7 for its own page-change toggle,
 * leaving nothing at or above 0x80 expressible on that device type.
 *
 * 0x70 is also the nibble base of the attestation claim: an attestation
 * page's byte [0] is PROFILE_COMPAT_PAGE_BASE | subtype, and that subtype is
 * the same nibble radiant_sec_compat.c put into the MAC'd nonce at position
 * 9 - the page byte is derived from what went into the tag, not a second
 * independent statement of it.
 */
#define PROFILE_COMPAT_PAGE_BASE       0x70u
#define PROFILE_COMPAT_PAGE_BEACON     0x70u
#define PROFILE_COMPAT_PAGE_ATTEST_I   0x71u
#define PROFILE_COMPAT_PAGE_ATTEST_II  0x72u

/* Byte 0's high bit on device type 0x78. Not part of the page number. */
#define PROFILE_COMPAT_PAGE_TOGGLE     0x80u
#define PROFILE_COMPAT_PAGE_MASK       0x7Fu

#define PROFILE_COMPAT_VERSION         1u

/* Frame indices in the beacon page's set: two frames steady-state; frames
 * 2/3 are Layer C's (profile_private.h), four frames while a countdown runs. */
#define PROFILE_COMPAT_FRAME_BEACON_0  0u
#define PROFILE_COMPAT_FRAME_BEACON_1  1u
#define PROFILE_COMPAT_BEACON_FRAMES   2u

/* How large the beacon page's set may grow. Byte [1] is
 * (index << 4) | (count - 1): the count is a nibble, so 16 is the format,
 * not a budget. */
#define PROFILE_COMPAT_SET_FRAMES_MAX  16u

/*
 * Promoted beacon cadence while a client contributes frames: one frame per
 * eight transmitted messages instead of one per 121-message cycle. Matches
 * Layer C's countdown promotion (docs/radiant-security.md section 11.5).
 * Arithmetic, not symmetry: an 8-frame enrolment set at steady cadence
 * (1/30s at 4 Hz) would need four minutes, longer than the 60 s window; at
 * 1-in-8 the set takes 16 s and goes out twice with room for loss. Costs
 * 12.4% of slots, paid only while a human holds a button; messages 119/120
 * are never offered to this client, so it can't displace a common page.
 */
#define PROFILE_COMPAT_BEACON_PROMOTED_EVERY 8u

#define PROFILE_COMPAT_FRAME_LEN       8u
#define PROFILE_COMPAT_HINT_BYTES      3u

/* Frame 0 byte [2] bits 3..0. */
#define PROFILE_COMPAT_CAP_ATTEST_AVAILABLE  0x01u
#define PROFILE_COMPAT_CAP_PRIVATE_AVAILABLE 0x02u
#define PROFILE_COMPAT_CAP_PAIRING_OPEN      0x04u
#define PROFILE_COMPAT_CAP_PAIRING_AVAILABLE 0x08u

/*
 * Four policy states, frame 0 byte [3] bits 1..0. `never` is the default and
 * what a shipped strap should ship in: a byte-exact ANT+ sensor for its
 * whole life, refusing (and counting) even a trusted authenticated switch.
 *
 * The other three's state machine is src/profiles/profile_private.c's
 * (compat-C8). This file decides only one thing: a `never` node must
 * advertise private-available = 0 and carry no locator, or the beacon is
 * malformed.
 */
#define PROFILE_COMPAT_POLICY_NEVER    0u
#define PROFILE_COMPAT_POLICY_PHYSICAL 1u
#define PROFILE_COMPAT_POLICY_COMMAND  2u
#define PROFILE_COMPAT_POLICY_ALWAYS   3u

/* Frame 0 byte [3] bit 5: a countdown is running and this node is going
 * somewhere. Layer C's, set through profile_compat_set_pending_switch().
 * Never set on an announce = silent node - that node simply never calls the
 * setter (nor publishes a locator, nor emits an announcement frame); the
 * decision lives in profile_private.c. */
#define PROFILE_COMPAT_PENDING_SWITCH  0x20u

/* Attestation mode, frame 0 byte [3] bit 4. Opportunistic substitution is
 * specified but deliberately not implemented in v1. */
#define PROFILE_COMPAT_MODE_FIXED         0u
#define PROFILE_COMPAT_MODE_OPPORTUNISTIC 1u

/* Which message of the 121-cycle carries a beacon frame: message 0, so the
 * on-air order is ...80, 81, beacon... joining the back of an existing
 * burst rather than a new cadence. Legal because a compat family has no
 * descriptor - on device type 0x60 message 0 restarts the descriptor set,
 * and the seam is never offered it. */
#define PROFILE_COMPAT_BEACON_SLOT 0u

/*
 * What the node advertises.
 *
 * `ch` is the radiant_sec_compat channel index - the node has already
 * called _set_key()/_set_devnum()/_configure()/_set_epoch() on it before
 * this init runs, since this layer must never see the root key.
 *
 * `epoch` is here only to compute the key-group hint. It is NOT broadcast:
 * for a hostless node the epoch is the boot counter, and sending it every
 * 30 s would fingerprint the device past every other privacy measure here.
 * A receiver instead recovers it by searching forward against the hint.
 */
struct profile_compat_cfg {
	uint8_t  ch;
	uint32_t epoch;

	/* advertise = off emits no beacon. A keyholder can still re-acquire
	 * the node by trial-verifying candidate epochs against the
	 * attestation tag directly - slower to find, not undiscoverable. */
	bool     advertise;

	bool     attest_available;
	bool     pairing_available;
	bool     pairing_open;

	uint8_t  policy;  /* PROFILE_COMPAT_POLICY_* */
	uint8_t  window;  /* N announced in the beacon; one of 4, 8, 16, 32 */
	uint8_t  mode;    /* PROFILE_COMPAT_MODE_* */

	/* The private-mode locator. Zero on a `never` node (refused with a
	 * value by profile_compat_init()) and zero on any announce = silent
	 * node regardless of policy. */
	uint8_t  target_device_type;
	uint16_t target_device_number;
	uint16_t target_period;

	/* The page-change toggle, for a device type that has one. NULL for
	 * bicycle power (byte [0] is a whole page number there). On heart
	 * rate every page including common pages carries it in bit 7, so a
	 * compat page must too or it's out of step with the sensor's own
	 * sequence. A callback, not a flag, since the value changes per
	 * message and the rule belongs to the profile. */
	bool   (*toggle)(void *user);
	void    *user;
};

/*
 * ---------------------------------------------------------------------------
 * The beacon page's own client seam
 * ---------------------------------------------------------------------------
 * Same shape as profile_sched.h's seam, one layer down: a second thing wants
 * to put frames in this page's set, and forking the set forks the framing
 * convention. Layer D's enrolment window (src/profiles/profile_enrol.c) is
 * six frames of a public key - not a new page number (ADR 0008 allocates
 * exactly two) but additional frames in the set, like Layer C's SWITCH/RETURN.
 *
 * Division of labour: this file owns byte [0] (page number, toggle) and
 * byte [1] (the (index << 4) | (count - 1) convention, including that a set
 * growing to eight says eight in EVERY frame, 0/1 included). The client owns
 * bytes [2..7] of its own frames only - it never learns a page number.
 *
 * `frames` is asked afresh on every claimed slot and may answer 0; a count
 * change mid-set tears it, and a receiver's accumulator restarts on the
 * count change, which is why a torn set costs a repeat rather than a wrong
 * key.
 *
 * `now_us` is handed in rather than read from a clock, so a bounded window
 * is testable in a millisecond without the client owning a timer.
 *
 * The one interlock: Layer C's SWITCH/RETURN frames are pinned at indices
 * 2/3, where the first client's block also starts, so a countdown and an
 * enrolment window must not run at once - whichever arrives second is
 * refused. Arbitrated in two places:
 *
 *   profile_compat_client_busy()  REQUEST-time, the one that matters both
 *                                 callers ask before committing (a window
 *                                 opening mid-countdown would burn a pairing
 *                                 counter for nothing; a countdown starting
 *                                 mid-window would tear a public key).
 *   the claim path                BACKSTOP: if two clients somehow both
 *                                 answer non-zero in one slot, only the
 *                                 first contributes, keeping the set size
 *                                 something both ends agree on.
 *
 * Two slots, not one: both clients are installed for the node's life and
 * idle almost all of it, so the exclusion is between activities, not
 * registrations.
 */
#define PROFILE_COMPAT_CLIENTS_MAX 2u

struct profile_compat_client {
	/* Frames this client contributes right now: 0 when it is idle. */
	uint8_t (*frames)(void *user, uint64_t now_us);
	/* Fill bytes [2..7] of the client's frame `i`, 0-based within the
	 * client's own block. Return false to abandon the slot. */
	bool    (*frame)(void *user, uint8_t i, uint8_t *payload);
	/* Optional. Every transmitted message, including ones this client
	 * didn't build - Layer C's countdown is measured in transmitted
	 * messages, not just this client's frames. Also the only way a
	 * client learns the final bytes of a frame it contributed, since
	 * byte [0]'s toggle and byte [1]'s count aren't known until then. */
	void    (*sent)(void *user, const uint8_t *body);
	void     *user;
};

struct profile_compat {
	struct profile_compat_cfg cfg;

	/* Encoded once at init: frames change only when capabilities do. */
	uint8_t frames[PROFILE_COMPAT_BEACON_FRAMES][PROFILE_COMPAT_FRAME_LEN];
	uint8_t n_frames;
	uint8_t frame_cursor;

	/* Key-group hint, kept so flipping a capability bit re-encodes frame
	 * 0 without asking the key-holding layer again. */
	uint8_t hint[PROFILE_COMPAT_HINT_BYTES];

	struct profile_compat_client clients[PROFILE_COMPAT_CLIENTS_MAX];
	uint8_t                      n_clients;
	/* Which client's block this slot is building, 1-based, 0 for none;
	 * set when the count is taken, read when a frame is asked for. */
	uint8_t                      active_client;

	/* Frame 0 byte [3] bit 5. */
	bool                        pending_switch;

	/* A beacon frame displaced by an attestation page; rides the next
	 * slot this client is offered (see compat_claim()). */
	bool    beacon_due;

	/* Handed in per slot rather than read from a clock, so a whole day
	 * of attestation is testable in a millisecond. */
	uint64_t now_us;

	bool armed;

	uint32_t beacons_sent;
	/* Kept apart from beacons_sent: mixing steady-state beacon slots
	 * with a client's bounded burst would make both numbers unreadable. */
	uint32_t client_sent;
	uint32_t tier1_sent;
	uint32_t tier2_sent;
};

/*
 * Build the beacon frames and read the key-group hint out of the key the
 * node already installed.
 *
 * Returns 0; -EINVAL for a malformed configuration (`never` node with a
 * locator, window outside {4,8,16,32}, device type with bit 7 set); or
 * -ENOTSUP with no compat attestation built in, not an error to handle -
 * the node just stays a plain ANT+ sensor.
 */
int profile_compat_init(struct profile_compat *pc,
			const struct profile_compat_cfg *cfg);

/* Register as the scheduler's client. Only an initialised, armed instance
 * registers, so a build with no attestation attaches nothing. */
int profile_compat_attach(struct profile_compat *pc, struct profile_sched *ps);

/*
 * Install or replace one of this page's clients, keyed by `user`: a second
 * call with the same `user` replaces rather than taking a second slot. NULL
 * removes them all.
 *
 * Returns 0, -EINVAL for a client with no frames/frame callback, or -ENOSPC
 * when both slots are held. Two, not sixteen, because only one client may
 * be active at a time (see the interlock above).
 */
int profile_compat_set_client(struct profile_compat *pc,
			      const struct profile_compat_client *client);

/*
 * The interlock, asked before committing to an activity: is another client
 * contributing frames right now? `except_user` is the asking client's own
 * `user` pointer, so a module never sees itself as the blocker. NULL asks
 * about every client.
 *
 * Polls rather than reading a cached flag, deliberately: a client with a
 * bounded window discovers here that it has closed, so a countdown isn't
 * refused by a window that expired unnoticed.
 */
bool profile_compat_client_busy(struct profile_compat *pc, uint64_t now_us,
				const void *except_user);

/*
 * Frame 0 byte [3] bit 5 (pending-switch) and frame 1's locator. Both are
 * Layer C's (src/profiles/profile_private.c); the locator is refused with
 * -EINVAL on a `never` node, same as at init. A node with announce = silent
 * simply never calls either - no locator, no pending-switch bit, no
 * announcement frames, so a capture has nothing to correlate.
 *
 * Both are no-ops on an advertise = off node (no beacon to carry a bit in) -
 * same non-error profile_compat_set_pairing_open() returns.
 */
int profile_compat_set_pending_switch(struct profile_compat *pc, bool pending);

int profile_compat_set_locator(struct profile_compat *pc, uint8_t device_type,
			       uint16_t device_number, uint16_t period);

/*
 * Flip the pairing-open capability bit and re-encode frame 0. Section 11.7
 * requires this: a silently opened window is the attack this announces
 * against.
 *
 * No-op on an `advertise = off` node: no beacon to carry the bit, though the
 * six pubkey frames still go out - just with no capability field for them.
 */
int profile_compat_set_pairing_open(struct profile_compat *pc, bool open);

/*
 * One slot, driven end to end: decide the page, then tell the attestation
 * layer what actually went on the air. The second half isn't optional -
 * Tier II's window is N consecutive transmitted messages, common pages and
 * all, so a caller reporting only its own data pages would build a window
 * neither side agrees about.
 */
enum profile_slot_kind profile_compat_next(struct profile_compat *pc,
					   struct profile_sched *ps,
					   uint64_t now_us, uint8_t *body);

/* The halves of the above, for a node that drives profile_sched_next() itself.
 * profile_compat_before() must be called first and profile_compat_sent() for
 * EVERY transmitted message, including the ones this client did not build. */
void profile_compat_before(struct profile_compat *pc, uint64_t now_us);
void profile_compat_sent(struct profile_compat *pc, const uint8_t *body);

/* True for a page number this layer put on the air - 0x70, 0x71 or 0x72, with
 * heart rate's toggle bit already discounted. The one place a decoder should
 * ask "is this page mine", so that a profile never has to name 0x70. */
bool profile_compat_is_compat_page(uint8_t byte0);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_COMPAT_H_ */
