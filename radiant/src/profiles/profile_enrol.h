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
 * documents. No ANT+ device profile document was read for this
 * file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * THE PROBLEM: a user must be able to add a second receiver to a strap set up
 * earlier, without re-provisioning the first receiver or opening the strap.
 * The group is rooted in ONE symmetric key (K_id, K_auth, K_enc all derive
 * from it), so adding a receiver means giving it the root - no epoch change,
 * no re-keying, no interruption, existing receivers see only a capability bit
 * flip. REMOVING a receiver is not symmetric: it means re-provisioning every
 * remaining one, since every keyholder holds the same root.
 *
 * THE HONEST LIMIT THIS FILE DOES NOT SOLVE: pairing happens IN THE CLEAR
 * (no prior secret, no out-of-band channel), so an attacker present during
 * the window can be a man in the middle. radiant_sec_pair.c's six-digit
 * fingerprint mitigation needs a human comparing it ACROSS TWO SCREENS; a
 * strap has no screen, so the comparison is one-sided. The mitigations this
 * file does implement: a BOUNDED window (60 s default), ONE PAIRING PER
 * WINDOW, a PHYSICAL TRIGGER at the node, and optionally reduced TX power
 * (the node's to set, not this file's). `closed` stays the recommendation
 * for anything that matters - a printed key or QR code actually defeats a
 * MITM rather than mitigating it (docs/radiant-security.md section 11.7,
 * rule four).
 *
 * WHY NO NEW PAGE NUMBER: ADR 0008 allocates exactly two page numbers for the
 * compat layer (heart rate's byte [0] bit 7 is a toggle, so nothing at or
 * above 0x80 is expressible). So the public key rides the beacon page's own
 * frame set as SIX ADDITIONAL FRAMES: the set grows from two to eight for the
 * window's duration and every frame restates the new count (section 11.5's
 * convention). This file names no page number or frame index -
 * profile_compat.c owns bytes [0..1]; this file owns bytes [2..7] of six
 * frames, same split that keeps profile_hr.c free of a key.
 *
 * "ON WHICHEVER CHANNEL THE NODE IS CURRENTLY ON" (section 11.7 rule one): a
 * private-mode node (device type 0x60) has no compat channel, so the frames
 * are ADDITIONAL PAGES rather than a channel change - nothing here opens,
 * closes, or retunes anything, so a node need not leave private mode to gain
 * a receiver.
 *
 * DEFERRED, named so the absence is a decision:
 *   - the derived locator (trunc16(CMAC(K_id, "priv"||epoch)), rule two) -
 *     landed in C8 as radiant_sec_compat_locator(), needs K_id so it can't
 *     live here.
 *   - the descriptor bit half of "a beacon bit and a descriptor bit" - device
 *     type 0x60's flags byte has no free bit; only the beacon bit
 *     (PROFILE_COMPAT_CAP_PAIRING_OPEN) is implemented.
 *   - a 0x60 page number for these six frames, for a node that is already
 *     private (compat device types need none, per ADR 0008). Belongs with
 *     whatever phase gives a node a reason to be on 0x60.
 * Neither deferral is load-bearing for this phase's claim that enrolment is
 * additive.
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
 * referenced here, because this header must not name a radiant type. */
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
 * 32-byte public key. Integrity, not authentication - an attacker rewriting
 * the key rewrites the check with it (that's the MITM, answered by the
 * fingerprint instead). What it catches is a set spliced from two different
 * pairing windows: the index/count convention says which frame this is,
 * never which key it belongs to, so without this check a receiver joining
 * as one window closes and another opens would silently derive a wrong
 * shared secret. All-zero key chosen because AES-CMAC is already in every
 * build that can pair.
 */

/* ---------------------------------------------------------------------------
 * The `enrol` setting - docs/radiant-security.md section 11.6
 * ---------------------------------------------------------------------------
 */
enum profile_enrol_mode {
	/* No over-air enrolment, ever. Keys arrive only out of band. The
	 * posture radiant_sec.h recommends for anything that matters. */
	PROFILE_ENROL_CLOSED = 0,
	/* THE DEFAULT. A bounded window opened by a physical action at the
	 * node (button, magnet, strap re-seat). One pairing per window. */
	PROFILE_ENROL_PHYSICAL = 1,
	/*
	 * A window a current keyholder opens with an authenticated command,
	 * for a node with no button and no host. Rate-limited, bounded, and
	 * the weakest of the three - exists because a sealed strap otherwise
	 * has no path at all.
	 *
	 * THE AUTHENTICATION IS THE CALLER'S: this module only rate-limits and
	 * bounds. A caller invoking profile_enrol_keyholder_request() on an
	 * unverified command has built "anybody may open this node's pairing
	 * window", worse than `physical` - why this isn't the default.
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

/* The floor between two `open-window` windows: 5 min against a 60 s window,
 * so a node whose command path is abused is open at most 1/6 of the time.
 * A rate limit, not a defence - the defence is command authentication. */
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
	 * Open the pairing window on `ch` and return the 32-byte public key to
	 * put on the air. A hostless strap wires this to ADR 0009's
	 * node_ident_pair_begin()/_pubkey(); a host-attached node wires it to
	 * the 0xF5 path. The seam is why this file needs neither src/node nor
	 * a storage backend to be testable.
	 *
	 * THE ORDERING RULE IS THE CALLEE'S, NOT NEGOTIABLE: pair_counter must
	 * advance and be durably written BEFORE the derived public key leaves
	 * the radio, never on completion - an abandoned window never
	 * completes, so advance-on-completion would reuse the scalar (a
	 * repeated private key) on the next attempt. node_ident.c enforces
	 * this structurally; this module cannot.
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
 * window on boot would pair with whoever is nearest every battery change.
 *
 * Returns 0, or -EINVAL for a null argument, an unknown mode, or a missing
 * open_pairing / close_pairing callback (required in every mode including
 * `closed`, so a misconfigured refusal can't look like a policy refusal).
 */
int profile_enrol_init(struct profile_enrol *pe,
		       const struct profile_enrol_cfg *cfg);

/*
 * Publish this module's frames through the beacon page's client seam, and its
 * open state through the beacon's capability bit.
 *
 * Optional. A device type 0x60 master (a private node) has no beacon page, so
 * it drives profile_enrol_frame_count() / profile_enrol_frame() from its own
 * page rotation instead and never calls this - the window opens on whichever
 * channel the node is on (section 11.7 rule one), and this module has no
 * opinion about which one that is.
 */
int profile_enrol_attach(struct profile_enrol *pe, struct profile_compat *pc);

/* ---------------------------------------------------------------------------
 * Opening the window
 * ---------------------------------------------------------------------------
 */

/*
 * THE PHYSICAL ACTION: a button handler, magnet detector, or strap-contact
 * interrupt calls this. A plain function call so the thing that can't be
 * simulated on a bench (a person pressing a button) isn't also the thing
 * that can't be tested.
 *
 * Permitted in `physical` and `open-window`. Refused in `closed`.
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
 * Expire the window if its time is up. profile_enrol_frame_count() calls
 * this, so a node driven through profile_compat.c owes nothing extra; a
 * sparse node that stops transmitting mid-window must call it itself or the
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
 * The receiver sends its public key over ANT ACKNOWLEDGED DATA, so the node
 * learns each frame arrived - radiant_transfer_ack_data() sends one,
 * radiant_transfer_on_data() acknowledges an inbound one and hands the
 * payload up through radiant_transfer_ops::rx_data(), and this function is
 * what a node calls from that callback. Six acknowledged messages rather
 * than a burst, deliberately: frames are individually indexed and
 * idempotent, so six independent exchanges lose one frame where a burst
 * (stop-and-wait, fails whole) loses a key.
 *
 * The peer sends its own set: count is always PROFILE_ENROL_FRAMES, indices
 * 0..5, byte [1] = (index << 4) | (PROFILE_ENROL_FRAMES - 1) regardless of
 * the node's own beacon set.
 *
 * BYTE [0] IS NOT EXAMINED - the caller has already filtered on the page
 * number.
 *
 * Repeats are idempotent: a frame that arrives twice overwrites itself.
 *
 * Returns 1 when this frame COMPLETED an enrolment (the shared secret is
 * derived, the root key is installed, the fingerprint is readable and the
 * window is closed), 0 when the set is still incomplete, or:
 *   -EINVAL   null, a length that is not 8, or no window open
 *   -EPROTO   byte [1] cannot belong to a six-frame set
 *   -EBADMSG  the set check failed - the frames came from two different
 *             windows, or somebody rewrote one. The accumulator is reset and
 *             the window stays open.
 *   -EACCES   the key layer refused the peer's public key (e.g. a
 *             small-order point) - mandatory, since the result becomes a
 *             root key. Accumulator resets, window stays open: one injected
 *             packet must not burn a window the user opened.
 *
 * radiant_sec's own return codes are not propagated: different numbering
 * (RADIANT_SEC_EINVAL is -1, which is -EPERM here) would make a mixed error
 * unattributable.
 */
int profile_enrol_on_ack_data(struct profile_enrol *pe, const uint8_t *payload,
			      uint8_t len);

/*
 * The six digits, once an enrolment has completed on this window. Read the
 * honest-limit block at the top of this file before showing it to anyone: on
 * a strap it is one half of a comparison with nothing to compare against.
 *
 * Returns 0, or -EINVAL when nothing has completed.
 */
int profile_enrol_fingerprint(const struct profile_enrol *pe, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_ENROL_H_ */
