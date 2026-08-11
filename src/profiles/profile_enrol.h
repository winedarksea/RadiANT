/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_enrol.h - Layer D: adding a receiver to an existing network.
 *
 * Provenance: docs/radiant-security.md section 11.7 (the `enrol` setting and
 * its three values, the four rules, and the screen-free fingerprint limit of
 * section 7.4 which this file does not solve), section 11.6's settings table
 * (which pins `physical` as the default), section 11.5 (the beacon page's
 * (index << 4) | (count - 1) frame set and the rule that a set which grows
 * restates its count in every frame) and
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md and
 * docs/decisions/0009-hostless-node-identity.md (the counter that advances
 * before a public key is transmitted). All of those are this project's own
 * documents. No adopter-gated ANT+ device profile document was read for this
 * file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * The problem, in one sentence
 * ---------------------------------------------------------------------------
 * A user buys a second head unit in March and must be able to add it to a
 * strap they set up in January, without re-provisioning the first head unit
 * and without taking the strap apart.
 *
 * The good news is structural. The group is rooted in ONE symmetric key -
 * K_id, K_auth and K_enc all derive from it - so adding a receiver means
 * giving that receiver the root. No epoch change, no re-keying, no
 * interruption, and every existing receiver observes nothing except a
 * capability bit going up and back down. That is why this can be a supported
 * operation rather than a disruptive one, and it is also why the reverse is
 * not: REMOVING a receiver means re-provisioning every remaining one, because
 * every keyholder holds the same root. RadiANT v1 adds a receiver in sixty
 * seconds and removes one only by re-provisioning the network. Anyone shipping
 * a product should read that sentence before choosing `open-window`.
 *
 * ---------------------------------------------------------------------------
 * THE HONEST LIMIT, WHICH THIS FILE DOES NOT SOLVE AND MUST NOT BE READ AS
 * SOLVING
 * ---------------------------------------------------------------------------
 * radiant_sec.h's pairing section says out-of-band pairing "remains the
 * recommended path for anything that matters", and nothing here weakens that.
 *
 * Pairing happens IN THE CLEAR. There is no prior secret and no out-of-band
 * channel on the air, so an active attacker present during the window can be
 * the man in the middle. The mitigation in radiant_sec_pair.c is the six-digit
 * fingerprint, and it works by a human comparing it ACROSS TWO SCREENS. A
 * heart-rate strap has no screen, so over the air the comparison is ONE-SIDED:
 * the receiver can show a number, and there is nothing at the node for the
 * user to compare it against.
 *
 * The mitigations that remain are all this file implements, and they are
 * mitigations rather than a fix:
 *
 *   - the window is BOUNDED (RADIANT_SEC_PAIR_TIMEOUT_DEFAULT_S, 60 s);
 *   - ONE PAIRING PER WINDOW, so a race that the attacker wins is a race the
 *     user watches fail;
 *   - a PHYSICAL TRIGGER at the node, so the window opens when a person
 *     decided it should;
 *   - optionally REDUCED TX POWER for the duration, so the attacker has to be
 *     in the room. That is the node's to set - this file does not touch the
 *     radio - and it is named here so it is a decision rather than an
 *     omission.
 *
 * `closed` stays the recommendation for anything that matters, and a printed
 * key or a QR code genuinely defeats a man in the middle rather than merely
 * mitigating it. This is not an argument that will be re-opened by a later
 * phase (docs/radiant-security.md section 11.7, rule four).
 *
 * ---------------------------------------------------------------------------
 * Where the frames go, and why there is no new page number
 * ---------------------------------------------------------------------------
 * ADR 0008 allocates exactly TWO page numbers for the whole compatibility
 * layer and the argument is the 7-bit namespace: heart rate's byte [0] carries
 * a page-change toggle in bit 7, so nothing at or above 0x80 is expressible on
 * that device type at all. A third number for enrolment is refused on that
 * ground alone, exactly as a third number for SWITCH/RETURN was.
 *
 * So the public key rides the beacon page's own frame set, through
 * profile_compat.h's client seam, as SIX ADDITIONAL FRAMES. The set grows from
 * two to eight for the duration of the window and every frame of it - frames 0
 * and 1 included - restates the new count, which is the convention section
 * 11.5 already fixes for Layer C's countdown. On an `advertise = off` node
 * there are no beacon frames and the set is the six enrolment frames alone.
 *
 * THIS FILE NAMES NO PAGE NUMBER AND NO FRAME INDEX. profile_compat.c owns
 * byte [0] and byte [1]; this file owns bytes [2..7] of six frames and knows
 * nothing else about the wire. The same division that keeps profile_hr.c free
 * of a key keeps this file free of a page number.
 *
 * ---------------------------------------------------------------------------
 * "On whichever channel the node is currently on"
 * ---------------------------------------------------------------------------
 * Rule one of section 11.7, and it is the case the plan previously had no
 * answer for: a node in private mode is on device type 0x60 and its compat
 * channel does not exist, so a pairing window that lived on the compat channel
 * could never be opened by the node that most needs it.
 *
 * The frames are ADDITIONAL PAGES, not a channel change. Nothing here opens,
 * closes or retunes anything, so the first receiver's stream runs untouched
 * across the whole window and a node does not have to leave private mode to
 * gain a receiver. This module is given a channel index and a frame sink and
 * has no opinion about which device type is underneath either.
 *
 * ---------------------------------------------------------------------------
 * What is deferred, named so the absence is a decision
 * ---------------------------------------------------------------------------
 *   - THE DERIVED LOCATOR (rule two: a newly enrolled receiver finds an
 *     already-private node without an announcement, because
 *     trunc16(CMAC(K_id, "priv" || epoch)) gives it the private device number
 *     directly). LANDED IN C8, not here: the derivation needs K_id, which only
 *     the layer holding the root key may touch, and it is
 *     radiant_sec_compat_locator(). This module still does not know it exists.
 *     Nothing here depends on the `announce` setting, which is the half of rule
 *     two this phase owed and paid, and
 *     radiant_core/tests/src/test_profile_enrol.c's
 *     test_a_private_node_gains_a_receiver_that_derives_it is the other half
 *     paid on top of it.
 *   - THE DESCRIPTOR BIT. Section 11.7 says the pairing-open state is "a
 *     beacon bit and a descriptor bit". The beacon bit is here
 *     (PROFILE_COMPAT_CAP_PAIRING_OPEN). The descriptor bit is not: device
 *     type 0x60's frame-0 flags byte has no free informational bit - bit 3 is
 *     the withdrawn X_PRIV and docs/radiant-telemetry.md keeps it reserved
 *     rather than reused - so allocating one is an envelope change and not
 *     this phase's to make.
 *   - THE 0x60 PAGE NUMBER for these six frames. On a compat device type they
 *     ride the beacon page's set, which needs no allocation because ADR 0008
 *     already made it. Device type 0x60 has its own page namespace and no
 *     beacon in it, so a node that is ALREADY private needs a page number
 *     there. That is an allocation in docs/profile-registry.md and it belongs
 *     with the phase that gives a node a reason to be on 0x60 in the first
 *     place. Nothing in this module's API changes when it lands: the frames
 *     come out of profile_enrol_frame() either way.
 *
 * Neither deferral is load-bearing for what this phase claims, which is that
 * enrolment is additive: it opens no channel, closes none, retunes none, and
 * changes no key that an existing receiver is using.
 */

#ifndef RADIANT_PROFILE_ENROL_H_
#define RADIANT_PROFILE_ENROL_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------------
 */

/* X25519, so 32. RADIANT_SEC_X25519_BYTES is the same number and is not
 * referenced here, because this header must not name a radiant_core type. */
#define PROFILE_ENROL_PUBKEY_BYTES 32u

/* Bytes [2..7] of one frame. Byte [0] is the page number and byte [1] is the
 * framing convention, and profile_compat.c owns both. */
#define PROFILE_ENROL_FRAME_PAYLOAD 6u

/*
 * Six frames, and the arithmetic is exact rather than roomy: 6 x 6 = 36 =
 * 32 bytes of public key plus a 4-byte set check. Five frames would carry 30
 * and could not hold the key; seven would waste a slot of a 60-second window.
 */
#define PROFILE_ENROL_FRAMES 6u
#define PROFILE_ENROL_CHECK_BYTES 4u
#define PROFILE_ENROL_SET_BYTES \
	(PROFILE_ENROL_FRAMES * PROFILE_ENROL_FRAME_PAYLOAD)

/*
 * The set check: the first four bytes of CMAC under an ALL-ZERO KEY over the
 * 32-byte public key.
 *
 * IT IS INTEGRITY, NOT AUTHENTICATION, and the distinction is the whole of
 * what it is for. There is no shared secret during pairing, so an attacker who
 * rewrites the key rewrites the check with it - that attack is the man in the
 * middle, and the fingerprint is what answers it. What this catches is the
 * failure the framing convention cannot: a set assembled from two DIFFERENT
 * pairing windows. The index-and-count convention says which frame this is,
 * never which key it belongs to, so a receiver that joined as one window
 * closed and another opened would otherwise splice two halves into a key
 * neither end holds and produce a shared secret that is simply wrong, with no
 * error anywhere. Four bytes make that a rejected set instead.
 *
 * The all-zero key is the same extraction trick radiant_sec_pair.c already
 * uses on the raw X25519 output, and it is chosen over inventing a CRC here
 * because AES-CMAC is already in every build that can pair at all.
 */

/* ---------------------------------------------------------------------------
 * The `enrol` setting - docs/radiant-security.md section 11.6
 * ---------------------------------------------------------------------------
 */
enum profile_enrol_mode {
	/*
	 * No over-air enrolment, ever. Keys arrive only out of band: the host
	 * message set on a host-attached node, or the manufacturer's
	 * provisioning path. This is the posture radiant_sec.h already
	 * recommends for anything that matters and this file does not weaken
	 * the recommendation.
	 */
	PROFILE_ENROL_CLOSED = 0,
	/*
	 * THE DEFAULT. A bounded window opened by a physical action at the node
	 * - a button, a magnet, a strap re-seat. One pairing per window.
	 */
	PROFILE_ENROL_PHYSICAL = 1,
	/*
	 * A window a CURRENT KEYHOLDER opens with an authenticated command, for
	 * a node with no button and no host. Rate-limited, bounded, and the
	 * weakest of the three; it exists because a sealed strap otherwise has
	 * no path at all.
	 *
	 * THE AUTHENTICATION IS THE CALLER'S. This module rate-limits and
	 * bounds; it does not verify a command, because the command page and
	 * its tag belong to the layer that holds K_auth. A caller that invokes
	 * profile_enrol_keyholder_request() on an unverified command has built
	 * "anybody may open this node's pairing window", which is strictly
	 * worse than `physical` and is the reason this value is not the
	 * default.
	 */
	PROFILE_ENROL_OPEN_WINDOW = 2,
};

/*
 * The compile-time default, from Kconfig, falling back to `physical` when the
 * symbols are absent - which is what the unit-test application sees, and what
 * section 11.6's table says a node ships with.
 */
#if defined(CONFIG_RADIANT_ENROL_CLOSED)
#define PROFILE_ENROL_MODE_DEFAULT PROFILE_ENROL_CLOSED
#elif defined(CONFIG_RADIANT_ENROL_OPEN_WINDOW)
#define PROFILE_ENROL_MODE_DEFAULT PROFILE_ENROL_OPEN_WINDOW
#else
#define PROFILE_ENROL_MODE_DEFAULT PROFILE_ENROL_PHYSICAL
#endif

/* The bounded window, from radiant_sec.h. Restated as a number rather than
 * included, for the same reason PROFILE_ENROL_PUBKEY_BYTES is; the .c file
 * asserts the two agree. */
#define PROFILE_ENROL_TIMEOUT_DEFAULT_S 60u

/*
 * The floor between two `open-window` windows.
 *
 * Five minutes against a sixty-second window, so a node whose command path is
 * being abused is open for at most one sixth of the time instead of
 * continuously. It is a rate limit rather than a defence: the defence is that
 * the command is authenticated and the setting is not the default.
 */
#define PROFILE_ENROL_OPEN_WINDOW_MIN_S 300u

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 */
struct profile_enrol_cfg {
	/* The radiant_sec channel the pairing runs on, and the channel whose
	 * stats the completed enrolment is counted against. */
	uint8_t ch;

	/* enum profile_enrol_mode; PROFILE_ENROL_MODE_DEFAULT. */
	uint8_t mode;

	/* 0 means PROFILE_ENROL_TIMEOUT_DEFAULT_S, never "forever" - the same
	 * rule radiant_sec_pair_enter() applies, for the same reason. */
	uint8_t timeout_s;

	/* 0 means PROFILE_ENROL_OPEN_WINDOW_MIN_S. Ignored in the other two
	 * modes. */
	uint16_t open_window_min_s;

	/*
	 * WHERE THE LOCAL SCALAR COMES FROM, as a seam rather than a
	 * dependency.
	 *
	 * Open the pairing window on `ch` and return the 32-byte public key to
	 * put on the air. A hostless strap wires this to ADR 0009's pair:
	 *
	 *     node_ident_pair_begin(ch, timeout_s);     advances and PERSISTS
	 *                                               pair_counter, derives
	 *                                               the scalar, enters
	 *                                               pairing mode
	 *     node_ident_pair_pubkey(ch, out);
	 *
	 * and a host-attached node wires it to the 0xF5 path. Two lines either
	 * way, and the seam is why this file needs neither src/node nor a
	 * storage backend to be testable.
	 *
	 * THE ORDERING RULE IS THE CALLEE'S AND IT IS NOT NEGOTIABLE:
	 * pair_counter advances and is durably written BEFORE the derived
	 * public key leaves the radio, never on completion. An abandoned window
	 * never completes, windows are abandoned all the time, and
	 * advance-on-completion therefore reuses the scalar on the very next
	 * attempt - which is a repeated private key and an invariant 32-byte
	 * name on the air. node_ident.c enforces it structurally; this module
	 * cannot, and says so rather than pretending.
	 *
	 * Returns 0, or negative to refuse the window.
	 */
	int (*open_pairing)(void *user, uint8_t ch, uint8_t timeout_s,
			    uint8_t *pubkey);

	/* Leave pairing mode. Called on completion, on timeout, and on an
	 * explicit close - one function so those three cannot drift. */
	void (*close_pairing)(void *user, uint8_t ch);

	void *user;
};

struct profile_enrol {
	struct profile_enrol_cfg cfg;
	bool                     armed;

	bool     open;
	uint64_t opened_us;
	uint64_t window_us;   /* timeout_s, in microseconds */
	bool     ever_opened;
	uint64_t last_open_us;

	/* The outbound set: 32 bytes of public key then 4 of set check. */
	uint8_t tx[PROFILE_ENROL_SET_BYTES];

	/* The inbound set, and a bitmask of the frames of it seen so far. */
	uint8_t  rx[PROFILE_ENROL_SET_BYTES];
	uint8_t  rx_have;

	uint32_t fingerprint;
	bool     have_fp;

	/* Counters. `enrolments` is this module's own view of the one in
	 * struct radiant_sec_stats, which is the authority a host reads. */
	uint32_t windows_opened;
	uint32_t windows_refused;
	uint32_t windows_expired;
	uint32_t enrolments;
	uint32_t frames_sent;
	uint32_t rx_frames;
	uint32_t rx_rejected;

	/* The beacon whose capability bit this window shows up in, or NULL. */
	struct profile_compat *compat;
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/*
 * Latch the configuration. Opens nothing - a node that entered a pairing
 * window because it booted would be a node that pairs with whoever is nearest
 * every time its battery is changed.
 *
 * Returns 0, or -EINVAL for a null argument, an unknown mode, or a missing
 * open_pairing / close_pairing callback. Both callbacks are required in every
 * mode INCLUDING `closed`: a `closed` node that was misconfigured into
 * refusing for the wrong reason would look identical to one refusing for the
 * right one.
 */
int profile_enrol_init(struct profile_enrol *pe,
		       const struct profile_enrol_cfg *cfg);

/*
 * Publish this module's frames through the beacon page's client seam, and its
 * open state through the beacon's capability bit.
 *
 * Optional. A node with no beacon page - a device type 0x60 master, which is
 * what a private node is - drives profile_enrol_frame_count() and
 * profile_enrol_frame() from its own page rotation instead and never calls
 * this. That is rule one of section 11.7 expressed as an API: the window opens
 * on whichever channel the node is currently on, and this module has no
 * opinion about which one that is.
 */
int profile_enrol_attach(struct profile_enrol *pe, struct profile_compat *pc);

/* ---------------------------------------------------------------------------
 * Opening the window
 * ---------------------------------------------------------------------------
 */

/*
 * THE PHYSICAL ACTION. A button handler, a magnet detector or a strap-contact
 * interrupt calls this, and there is deliberately nothing else behind it: the
 * trigger is a function call so that the thing which cannot be simulated on a
 * bench - a person pressing a button - is not also the thing that cannot be
 * tested.
 *
 * Permitted in `physical` and in `open-window` (a node with a button and a
 * command path has both). REFUSED in `closed`, which is the whole of what
 * `closed` means.
 *
 * Returns 0, or:
 *   -EPERM   mode is `closed`
 *   -EBUSY   a window is already open; one pairing per window, and a second
 *            trigger does not extend the first
 *   -EINVAL  not initialised
 *   whatever open_pairing() refused with, in which case NO window is entered
 *            and the counter it may have burnt is not rolled back
 */
int profile_enrol_physical_action(struct profile_enrol *pe, uint64_t now_us);

/*
 * The `open-window` path: a current keyholder asks, over an authenticated
 * command the CALLER HAS ALREADY VERIFIED. See PROFILE_ENROL_OPEN_WINDOW.
 *
 * Returns 0, or:
 *   -EPERM   mode is not `open-window`
 *   -EBUSY   a window is already open
 *   -EAGAIN  rate-limited: less than open_window_min_s since the last window
 *            opened. Counted in windows_refused, because a node being asked
 *            repeatedly is a node somebody is working on.
 */
int profile_enrol_keyholder_request(struct profile_enrol *pe, uint64_t now_us);

/* Close early - the user let go, the node is shutting down, a countdown needs
 * the frame indices. Idempotent. */
void profile_enrol_close(struct profile_enrol *pe);

/* True while the window is open. Does NOT expire it; profile_enrol_tick() and
 * the frame-count callback do, because expiry has a side effect (leaving
 * pairing mode) and a predicate that had one would be a trap. */
bool profile_enrol_is_open(const struct profile_enrol *pe);

/*
 * Expire the window if its time is up.
 *
 * profile_enrol_frame_count() calls this, so a node whose frames go through
 * profile_compat.c owes nothing extra. A node that stops transmitting mid
 * window - a sparse node between heartbeats - must call it itself, or its
 * window closes late.
 *
 * Returns true if this call closed the window.
 */
bool profile_enrol_tick(struct profile_enrol *pe, uint64_t now_us);

/* ---------------------------------------------------------------------------
 * The frames
 * ---------------------------------------------------------------------------
 */

/* PROFILE_ENROL_FRAMES while a window is open, 0 otherwise. Expires the window
 * first, so the frame count and the open state can never disagree. Signature
 * matches struct profile_compat_client. */
uint8_t profile_enrol_frame_count(void *user, uint64_t now_us);

/* Bytes [2..7] of frame `i`, 0 <= i < PROFILE_ENROL_FRAMES. False when no
 * window is open or `i` is out of range. Signature matches
 * struct profile_compat_client. */
bool profile_enrol_frame(void *user, uint8_t i, uint8_t *payload);

/* ---------------------------------------------------------------------------
 * The peer's key, over acknowledged data
 * ---------------------------------------------------------------------------
 */

/*
 * One inbound eight-byte payload of the peer's own six-frame set.
 *
 * WHERE THIS COMES FROM, because the plan's wording is a mechanism and not a
 * metaphor: the receiver sends its public key with ANT ACKNOWLEDGED DATA, so
 * the node learns each frame arrived rather than hoping. That path already
 * exists end to end in radiant_core - radiant_transfer_ack_data() sends one,
 * radiant_transfer_on_data() acknowledges an inbound one INSIDE the measured
 * 1.55 ms turnaround and then hands the payload up through
 * radiant_transfer_ops::rx_data() - and this function is what a node calls
 * from that callback. Nothing here re-implements an acknowledgement, opens a
 * window, or touches the radio: the acknowledgement is on the air before this
 * function is reached.
 *
 * Six acknowledged messages rather than a burst, deliberately. A burst is
 * stop-and-wait over the same frames with a transfer that fails whole, and the
 * frames are individually indexed and idempotent here, so six independent
 * exchanges lose one frame where a burst loses a key.
 *
 * THE PEER SENDS A SET OF ITS OWN: count is always PROFILE_ENROL_FRAMES and
 * the indices are 0..5. It is not extending the node's beacon set - it has no
 * beacon - so byte [1] is (index << 4) | (PROFILE_ENROL_FRAMES - 1) whatever
 * the node's own set currently looks like.
 *
 * BYTE [0] IS NOT EXAMINED. It is a page number, this file names none, and the
 * caller has already filtered on it - profile_compat_is_compat_page() is the
 * one function that should ever ask.
 *
 * Repeats are idempotent: a frame that arrives twice overwrites itself, which
 * is what makes a retransmitted acknowledged message harmless.
 *
 * Returns 1 when this frame COMPLETED an enrolment (the shared secret is
 * derived, the root key is installed, the fingerprint is readable and the
 * window is closed), 0 when the set is still incomplete, or:
 *   -EINVAL   null, a length that is not 8, or no window open
 *   -EPROTO   byte [1] cannot belong to a six-frame set
 *   -EBADMSG  the set check failed - the frames came from two different
 *             windows, or somebody rewrote one. The accumulator is reset and
 *             the window stays open.
 *   -EACCES   the key layer refused the peer's public key. A small-order
 *             point is the case that matters and it is MANDATORY to refuse:
 *             the result becomes a root key, so accepting it would let anyone
 *             able to inject one packet fix the group key to a value they
 *             already know. The accumulator is reset and THE WINDOW STAYS
 *             OPEN, because one injected packet must not be able to burn a
 *             window the user opened.
 *
 * radiant_sec's own return codes are not propagated: they are a different
 * numbering (RADIANT_SEC_EINVAL is -1, which is -EPERM here) and a caller that
 * mixed the two would have an unattributable error.
 */
int profile_enrol_on_ack_data(struct profile_enrol *pe, const uint8_t *payload,
			      uint8_t len);

/*
 * The six digits, once an enrolment has completed on this window.
 *
 * READ radiant_sec.h's pairing section and the honest-limit block at the top
 * of this file before showing it to anyone: on a node with a screen this is a
 * comparison that defeats a man in the middle, and on a strap it is one half
 * of a comparison with nothing to compare against.
 *
 * Returns 0, or -EINVAL when nothing has completed.
 */
int profile_enrol_fingerprint(const struct profile_enrol *pe, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_ENROL_H_ */
