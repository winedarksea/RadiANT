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
 * are this project's own documents and code. No adopter-gated ANT+ device
 * profile document was read for this file.
 *
 * ---------------------------------------------------------------------------
 * THIS FILE EXISTS SO profile_hr.c AND profile_power.c DO NOT HAVE TO
 * ---------------------------------------------------------------------------
 * The plan's boundary is one sentence - "the profile modules themselves never
 * call radiant_sec" - and the shortest honest reading of it is that something
 * has to. There are three candidates and two of them are wrong:
 *
 *   profile_sched.c   is the RF plan's, owns the 119/120/121 cadence for
 *                     device type 0x60, and knows no page number belonging to
 *                     any profile. Putting a key and a CMAC into the shared
 *                     rotation engine would give the envelope a dependency on
 *                     the compatibility work, which is the wrong direction and
 *                     is what "as a CLIENT of it, through a seam, not as a
 *                     second scheduler" exists to prevent.
 *   profile_hr.c      would put a key in a file whose whole claim is that it
 *   profile_power.c   is a byte-exact ANT+ sensor. That is the invariant.
 *   THIS FILE         is the client. It knows the page numbers and the frame
 *                     layouts; radiant_sec_compat.c knows the cryptography and
 *                     no page number; profile_hr.c and profile_power.c know
 *                     neither and call neither.
 *
 * So the call path is
 *
 *   profile_hr.c / profile_power.c  ->  profile_compat.c  ->  radiant_sec_compat.c
 *          (no crypto)                  (no ANT+ page)         (no page at all)
 *
 * and the invariant is structural rather than remembered: NOTHING IN THIS
 * HEADER NAMES A radiant_sec TYPE, so a profile module that includes it does
 * not even reach a radiant_core header transitively. tools/test_compat_capture.py
 * greps for that, because an invariant nobody checks is a comment.
 *
 * ---------------------------------------------------------------------------
 * What goes on the air, and what it costs
 * ---------------------------------------------------------------------------
 *   0x70  the capability beacon, two frames on the descriptor's
 *         (index << 4) | (count - 1) convention, one frame per 121-message
 *         cycle. 0.8% of slots.
 *   0x71  Tier I identity attestation, one page every T seconds - DECOUPLED
 *         FROM THE DATA RATE, which is the whole compatibility argument. 1.2%
 *         of slots at 4 Hz and T = 20 s, and proportionally LESS on a slower
 *         profile.
 *   0x72  Tier II data attestation, off by default, one page in N transmitted
 *         messages.
 *
 * 2.0% of slots in the default configuration, against the 1.65% ANT+ itself
 * already spends on common pages 80 and 81. That is the number the
 * compatibility claim rests on.
 *
 * ---------------------------------------------------------------------------
 * The one interaction the seam's rule creates, stated rather than discovered
 * ---------------------------------------------------------------------------
 * The seam never offers message 119, message 120 or a descriptor frame,
 * because those three ARE the cadence rule. Tier I does not care: its counter
 * is derived from elapsed time and a slot of slip is invisible to it, and the
 * beacon does not care either - a frame whose slot was taken by an attestation
 * page is OWED rather than lost, and rides the next slot this client is
 * offered, so the cadence stays at one frame per cycle.
 *
 * TIER II DOES CARE, and this is where it shows. Its window closes at the Nth
 * transmitted message exactly and has no slack at all, so a window whose Nth
 * message lands on message 119 or 120 closes with no tag: radiant_sec_compat.c
 * absorbs that (the next window starts clean) and a receiver reports one
 * unverified window, which is the same cost a lost packet already has in that
 * tier. With N = 8 and a 121-message cycle that is roughly two windows in
 * fifteen. It is a real cost of not forking the rotation, it is bounded, it
 * only exists in the tier that is off by default, and the alternative - a
 * client that can displace a common page - is forking the rotation with extra
 * steps.
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
 * three: the SWITCH/RETURN announcement rides frames 2 and 3 of the beacon
 * page's own set, and spending a third number on it was rejected on the 7-bit
 * namespace alone - heart rate's byte 0 carries a page-change toggle in bit 7,
 * so nothing at or above 0x80 is expressible on that device type at all.
 *
 * 0x70 IS THE BEACON AND ALSO THE NIBBLE BASE OF THE ATTESTATION CLAIM. An
 * attestation page's byte [0] is PROFILE_COMPAT_PAGE_BASE | subtype, and that
 * subtype is the same nibble radiant_sec_compat.c put into the MAC'd nonce at
 * position 9 - so the page byte is DERIVED from what went into the tag rather
 * than being a second, independent statement of the same thing.
 */
#define PROFILE_COMPAT_PAGE_BASE       0x70u
#define PROFILE_COMPAT_PAGE_BEACON     0x70u
#define PROFILE_COMPAT_PAGE_ATTEST_I   0x71u
#define PROFILE_COMPAT_PAGE_ATTEST_II  0x72u

/* Byte 0's high bit on device type 0x78. Not part of the page number. */
#define PROFILE_COMPAT_PAGE_TOGGLE     0x80u
#define PROFILE_COMPAT_PAGE_MASK       0x7Fu

#define PROFILE_COMPAT_VERSION         1u

/* Frame indices in the beacon page's set. Two frames in the steady state; the
 * announcement's frames 2 and 3 are Layer C's (profile_private.h pins them),
 * and the set is four frames for as long as a countdown runs. */
#define PROFILE_COMPAT_FRAME_BEACON_0  0u
#define PROFILE_COMPAT_FRAME_BEACON_1  1u
#define PROFILE_COMPAT_BEACON_FRAMES   2u

/*
 * How large the beacon page's set may grow. Byte [1] is
 * (index << 4) | (count - 1), so the count is a nibble and sixteen is not a
 * budget - it is the format.
 */
#define PROFILE_COMPAT_SET_FRAMES_MAX  16u

/*
 * The promoted beacon cadence: one frame every eight transmitted messages
 * while a client is contributing frames, against one per 121-message cycle in
 * the steady state.
 *
 * This is the same number Layer C promotes to for a countdown
 * (docs/radiant-security.md section 11.5, "the beacon promoted to 1 in 8 for
 * the duration"), and it is arithmetic rather than symmetry. An enrolment
 * window is 60 s and its frame set is eight frames; at the steady cadence one
 * frame per cycle is one frame per 30 s at 4 Hz, so the set would need four
 * minutes to go out once and the window would close having transmitted a
 * quarter of a public key. At 1 in 8 the eight frames take 64 messages, 16 s,
 * and the set goes out twice inside the window with room for loss.
 *
 * The cost is 12.4% of slots and it is paid only while a human is holding a
 * button. Messages 119 and 120 are never offered to this client at all, so the
 * promotion cannot displace a common page.
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
 * The four policy states, frame 0 byte [3] bits 1..0. `never` is the default
 * and it is the configuration a shipped strap should ship in: a byte-exact
 * ANT+ sensor for its whole life, which will refuse an authenticated switch
 * command from a keyholder it trusts and count the refusal.
 *
 * The state machine behind the other three is src/profiles/profile_private.c's
 * (compat-C8), which resolves the policy from NVM or Kconfig and hands the
 * result here. This file still decides only one thing about it: a `never` node
 * must advertise private-available = 0 and carry no locator, and the two must
 * agree or the beacon is malformed.
 */
#define PROFILE_COMPAT_POLICY_NEVER    0u
#define PROFILE_COMPAT_POLICY_PHYSICAL 1u
#define PROFILE_COMPAT_POLICY_COMMAND  2u
#define PROFILE_COMPAT_POLICY_ALWAYS   3u

/*
 * Frame 0 byte [3] bit 5: a countdown is running and this node is going
 * somewhere. Layer C's, set through profile_compat_set_pending_switch().
 *
 * It is never set on a node with announce = silent, and that is not this
 * file's rule to enforce - a silent node simply never calls the setter, along
 * with never publishing a locator and never emitting an announcement frame.
 * Three absences, one decision, and it lives in profile_private.c where the
 * setting is read.
 */
#define PROFILE_COMPAT_PENDING_SWITCH  0x20u

/* Attestation mode, frame 0 byte [3] bit 4. Opportunistic substitution is
 * specified so it is a later configuration change rather than a break, and is
 * deliberately not implemented in v1. */
#define PROFILE_COMPAT_MODE_FIXED         0u
#define PROFILE_COMPAT_MODE_OPPORTUNISTIC 1u

/*
 * Which message of the 121-cycle carries a beacon frame.
 *
 * Message 0, so the on-air order is ... 80, 81, beacon ... - the beacon joins
 * the back of the burst a certified sensor already sends rather than inventing
 * a cadence of its own, which is exactly what tools/ant_sim.py does and what
 * every deployed receiver already absorbs. It is legal only because a compat
 * family has no descriptor: on device type 0x60 message 0 restarts the
 * descriptor set and the seam is not offered it at all.
 */
#define PROFILE_COMPAT_BEACON_SLOT 0u

/*
 * What the node advertises.
 *
 * `ch` is the radiant_sec_compat channel index - the node has already called
 * radiant_sec_compat_set_key(), _set_devnum(), _configure() and _set_epoch()
 * on it before this init runs, because those take a root key and this layer
 * must never see one.
 *
 * `epoch` is here only to compute the key-group hint. IT IS NOT BROADCAST and
 * there is no beacon field for it: for a hostless node the epoch is the boot
 * counter, and a slowly incrementing 32-bit number sent every 30 s is a device
 * fingerprint that survives every other privacy measure in this design. A
 * receiver recovers it by searching forward against the hint instead.
 */
struct profile_compat_cfg {
	uint8_t  ch;
	uint32_t epoch;

	/* advertise = off emits no beacon at all. A keyholder can still
	 * re-acquire such a node - it trial-verifies candidate epochs against
	 * the attestation tag directly - so this is slower to find, not
	 * undiscoverable. */
	bool     advertise;

	bool     attest_available;
	bool     pairing_available;
	bool     pairing_open;

	uint8_t  policy;  /* PROFILE_COMPAT_POLICY_* */
	uint8_t  window;  /* N announced in the beacon; one of 4, 8, 16, 32 */
	uint8_t  mode;    /* PROFILE_COMPAT_MODE_* */

	/*
	 * The private-mode locator. Zero on a `never` node - there is nowhere
	 * for it to go - and zero on any node with announce = silent, whatever
	 * its policy. profile_compat_init() REFUSES a `never` node that
	 * supplies one, the same way tools/ant_pages.py's decoder refuses to
	 * believe half a malformed beacon.
	 */
	uint8_t  target_device_type;
	uint16_t target_device_number;
	uint16_t target_period;

	/*
	 * The page-change toggle, for a device type that has one.
	 *
	 * NULL for bicycle power, whose byte [0] is a whole page number. On
	 * heart rate every page carries the toggle in bit 7 including the
	 * common pages, so a compat page that did not would be the one page in
	 * the stream out of step with the sensor's own sequence - and the
	 * toggle sequence is a bench question against a real head unit
	 * (docs/decisions/0008, "Costs, accepted"), which is a reason to be
	 * consistent rather than clever.
	 *
	 * The callback rather than a flag because the value changes per
	 * message and the rule for changing it belongs to the profile.
	 */
	bool   (*toggle)(void *user);
	void    *user;
};

/*
 * ---------------------------------------------------------------------------
 * The beacon page's own client seam
 * ---------------------------------------------------------------------------
 * The same shape as profile_sched.h's seam, one layer down, and for the same
 * reason: there is a second thing that wants to put frames in this page's set
 * and forking the set is forking the framing convention.
 *
 * Layer D's enrolment window is six frames of a public key
 * (src/profiles/profile_enrol.c). They are not a new page number - ADR 0008
 * allocates exactly two and the argument is the 7-bit namespace - so they are
 * additional frames in the set this page already sends, exactly as Layer C's
 * SWITCH and RETURN are.
 *
 * THE DIVISION OF LABOUR IS THE POINT. This file owns byte [0] (the page
 * number and heart rate's toggle) and byte [1] (the
 * (index << 4) | (count - 1) convention, including the rule that a set which
 * grows to eight says eight in EVERY frame of it, frames 0 and 1 included).
 * The client owns bytes [2..7] of its own frames and nothing else, so a client
 * never learns a page number and there is exactly one place where the framing
 * convention is written.
 *
 * `frames` is asked afresh on every claimed slot and may answer 0. A client
 * whose count changes mid-set tears that set, and the receiver's accumulator
 * restarts on the count change - which is the behaviour the convention is
 * built for and is why a torn set costs a repeat rather than a wrong key.
 *
 * `now_us` is handed in rather than read from a clock, so a client can expire
 * a bounded window without owning a timer and a whole 60-second window is
 * testable in a millisecond.
 *
 * THE ONE INTERLOCK, and C8 is the phase that owns it: Layer C's SWITCH/RETURN
 * frames are pinned at indices 2 and 3, which is where the first client's block
 * also starts. A countdown and an enrolment window must not run at once, and
 * whichever of the two arrives second is the one that is refused.
 *
 * It is arbitrated in TWO places and they answer two different questions:
 *
 *   profile_compat_client_busy()  is the REQUEST-time answer, and it is the one
 *                                 that matters. A window that opened while a
 *                                 countdown was running would burn a pairing
 *                                 counter, enter pairing mode and transmit
 *                                 nothing; a countdown that started while a
 *                                 window was open would tear a public key in
 *                                 half. Both callers ask before they commit,
 *                                 and both count the refusal.
 *   the claim path                is the BACKSTOP. If two clients somehow
 *                                 answer non-zero in one slot, the first one
 *                                 contributes and the second's frames are not
 *                                 taken. That keeps the set's size a number
 *                                 both ends agree about even when the rule
 *                                 above has been broken, rather than putting a
 *                                 ten-frame set on the air that nothing can
 *                                 reassemble.
 *
 * Two slots, not one, because both clients are installed for the life of the
 * node and are idle almost all of it: the exclusion is between two ACTIVITIES,
 * not between two registrations, and a seam that arbitrated at attach time
 * would refuse whichever module was wired up second, forever.
 */
#define PROFILE_COMPAT_CLIENTS_MAX 2u

struct profile_compat_client {
	/* Frames this client contributes right now: 0 when it is idle. */
	uint8_t (*frames)(void *user, uint64_t now_us);
	/* Fill bytes [2..7] of the client's frame `i`, 0-based within the
	 * client's own block. Return false to abandon the slot. */
	bool    (*frame)(void *user, uint8_t i, uint8_t *payload);
	/*
	 * Optional. Every transmitted message, INCLUDING the ones this client
	 * did not build and the ones no client built - the same discipline
	 * profile_compat_sent() already owes the attestation layer, and for a
	 * related reason: Layer C's countdown is measured in transmitted
	 * messages, so a client that counted only its own frames would count
	 * eight of them and call it sixty-four.
	 *
	 * It is also the only way a client learns the FINAL bytes of a frame it
	 * contributed. Byte [0] carries a toggle that changes per message and
	 * byte [1] carries a count that changes with the set, so a frame's
	 * transmitted form is not knowable when its payload is built - and an
	 * announcement whose tag has to cover all eight transmitted bytes has
	 * to see them.
	 */
	void    (*sent)(void *user, const uint8_t *body);
	void     *user;
};

struct profile_compat {
	struct profile_compat_cfg cfg;

	/* Encoded once at init: the two beacon frames change only when the
	 * node's capabilities do, and re-encoding them per slot would be work
	 * in the hot path for bytes that are identical. */
	uint8_t frames[PROFILE_COMPAT_BEACON_FRAMES][PROFILE_COMPAT_FRAME_LEN];
	uint8_t n_frames;
	uint8_t frame_cursor;

	/* The key-group hint, kept so that flipping a capability bit re-encodes
	 * frame 0 without asking the layer that holds the key for it again. */
	uint8_t hint[PROFILE_COMPAT_HINT_BYTES];

	struct profile_compat_client clients[PROFILE_COMPAT_CLIENTS_MAX];
	uint8_t                      n_clients;
	/* Which client's block this slot is building, 1-based; 0 for none. Set
	 * when the frame count is taken and read when a frame is asked for, so
	 * the two cannot disagree inside one slot. */
	uint8_t                      active_client;

	/* Frame 0 byte [3] bit 5. */
	bool                        pending_switch;

	/* A beacon frame whose slot was taken by an attestation page. It rides
	 * the next slot this client is offered rather than the next cycle - see
	 * compat_claim() for why message 0 is where slipped pages land. */
	bool    beacon_due;

	/* The instant the current slot goes out, handed in per slot rather
	 * than read from a clock here - radiant_sec_compat.h's rule, inherited:
	 * a whole day of attestation is then testable in a millisecond. */
	uint64_t now_us;

	bool armed;

	uint32_t beacons_sent;
	/* Frames the page's client put on the air. Kept apart from
	 * beacons_sent because the 0.8%-of-slots claim is about the steady
	 * state and a bounded burst inside it would make that number
	 * unreadable. */
	uint32_t client_sent;
	uint32_t tier1_sent;
	uint32_t tier2_sent;
};

/*
 * Build the beacon frames and read the key-group hint out of the key the node
 * already installed.
 *
 * Returns 0; -EINVAL for a malformed configuration (a `never` node carrying a
 * locator, a window outside {4, 8, 16, 32}, a device type with bit 7 set); or
 * -ENOTSUP when this build has no compat attestation at all, which is not an
 * error a node has to handle - it simply stays a plain ANT+ sensor, which is
 * what most of them should be.
 */
int profile_compat_init(struct profile_compat *pc,
			const struct profile_compat_cfg *cfg);

/* Register as the scheduler's client. Only an initialised, armed instance
 * registers, so a build with no attestation attaches nothing and the rotation
 * never learns a client existed. */
int profile_compat_attach(struct profile_compat *pc, struct profile_sched *ps);

/*
 * Install or replace one of this page's clients, keyed by `user`: a second call
 * with the same `user` replaces that client rather than taking a second slot.
 * NULL removes them all.
 *
 * Returns 0, -EINVAL for a client with no frames/frame callback, or -ENOSPC
 * when both slots are held by other users. There are two, and the argument for
 * two rather than sixteen is the interlock above: only one client may be ACTIVE
 * at a time, so a third would be a third thing that could not run.
 */
int profile_compat_set_client(struct profile_compat *pc,
			      const struct profile_compat_client *client);

/*
 * THE INTERLOCK, asked before committing to an activity: is another client
 * contributing frames right now?
 *
 * `except_user` is the asking client's own `user` pointer, so a module never
 * sees itself as the reason it cannot start. NULL asks about every client.
 *
 * It POLLS rather than reading a cached flag, and the side effect is wanted: a
 * client with a bounded window discovers here that the window has closed, so a
 * countdown is not refused by an enrolment window that expired thirty seconds
 * ago and has not been asked since.
 */
bool profile_compat_client_busy(struct profile_compat *pc, uint64_t now_us,
				const void *except_user);

/*
 * Frame 0 byte [3] bit 5, the pending-switch bit, and frame 1's locator.
 *
 * Both are Layer C's (src/profiles/profile_private.c) and neither is set by
 * this file on its own behalf. The locator is REFUSED on a `never` node with
 * -EINVAL, exactly as a `never` node carrying one is refused at init: a
 * receiver that saw private-available = 0 beside a locator would have to decide
 * which half of the beacon to believe, and the answer is neither.
 *
 * A node with announce = silent simply never calls either one. That is the
 * whole of `silent` on this page: no locator field, no pending-switch bit, and
 * no announcement frames, so a capture contains nothing to correlate.
 *
 * Both answer 0 and do nothing on an advertise = off node, which has no beacon
 * to carry a bit in - the same non-error profile_compat_set_pairing_open()
 * returns for the same reason.
 */
int profile_compat_set_pending_switch(struct profile_compat *pc, bool pending);

int profile_compat_set_locator(struct profile_compat *pc, uint8_t device_type,
			       uint16_t device_number, uint16_t period);

/*
 * Flip the pairing-open capability bit and re-encode frame 0.
 *
 * This is how an enrolment window reaches the receivers that already exist,
 * and section 11.7 makes it a requirement rather than a nicety: an enrolment
 * the owner did not perform is the whole attack, so a window that opened
 * silently would be the mistake.
 *
 * Answers 0 and does nothing on an `advertise = off` node: there is no beacon
 * to carry the bit. Such a node's window is still visible - the six pubkey
 * frames are on the air either way - it simply has no capability field to
 * announce it in.
 */
int profile_compat_set_pairing_open(struct profile_compat *pc, bool open);

/*
 * One slot, driven end to end: decide the page, and tell the attestation layer
 * what actually went on the air.
 *
 * The second half is not optional and is why this wrapper exists at all. Tier
 * II's window is N consecutive TRANSMITTED messages - the common pages, the
 * beacon and the Tier I page are inside the tag too - so a caller that
 * reported only its data pages would build a window neither side agrees about.
 * Making that a wrapper rather than a rule in a comment is the difference
 * between an invariant and a hope.
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
