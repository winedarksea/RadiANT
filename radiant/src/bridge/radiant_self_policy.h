/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_self_policy.h - what a bridge's self-driven channel should do next.
 *
 * apps/dongle_thread/src/self_channels.c drives a few slave channels of its
 * own beside whatever an ANT host is doing, polls each one once a second, and
 * acts. The DECISIONS in that loop live here, as pure functions over plain
 * inputs, and the file there keeps only the antr_* calls. That split exists
 * because radiant/tests can reach radiant and nothing in apps/ (the treadmill
 * control token moved here for the same reason): a policy that can only be
 * exercised on a bench with real sensors is a policy nobody re-checks.
 *
 * THE MODEL IS ZWIFT'S (docs/radiant-bridge.md section 10.1b, from the USB
 * captures): discovery is a bounded pairing state, and a sensor once paired
 * owns its channel, PINNED to its device number with an infinite search, for
 * as long as it stays paired. A pinned channel is never unbound by a dropout,
 * a search timeout or a stranger; the only way a paired sensor leaves is
 * EVICTION, when a new pairing needs its channel and it is the one that has
 * been absent longest.
 *
 * Nothing here includes an ANT header. Channel state arrives as the small enum
 * below, and the caller maps the serial protocol's status byte onto it.
 */

#ifndef RADIANT_SELF_POLICY_H_
#define RADIANT_SELF_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The radio side of a channel, as its status poll reports it. */
enum radiant_self_radio {
	RADIANT_SELF_RADIO_UNASSIGNED = 0,
	RADIANT_SELF_RADIO_ASSIGNED,  /* assigned and closed */
	RADIANT_SELF_RADIO_SEARCHING,
	RADIANT_SELF_RADIO_TRACKING,
};

/*
 * One channel's inputs, as of this poll.
 *
 * owned is the caller's own record that IT assigned this channel number since
 * the last host reset. A host SYSTEM_RESET wipes every channel, and a host
 * that then assigns this channel number for itself has taken it: from then on
 * its state is the host's, and acting on it would reconfigure the host's
 * channel under it.
 */
struct radiant_self_chan_in {
	enum radiant_self_radio radio;
	bool owned;
	/* A binding is attached to this channel (the sensor is paired). */
	bool bound;
	/* The open now in progress was made pinned. False while a just-bound
	 * channel is still open on its discovery configuration - which, on a
	 * backend whose close is asynchronous, can outlast one poll. */
	bool open_pinned;
	/* The channel was last opened for discovery, so finding it closed
	 * means its search timed out. */
	bool open_discover;
	/* TRACKING and bound: what it tracks is the bound device. An id that
	 * has not resolved yet counts as a match - no evidence otherwise. */
	bool identity_matches;
	bool window_open;
};

enum radiant_self_action {
	RADIANT_SELF_NONE = 0,
	/* Unbound and tracking with the window open: try to bind. On success
	 * the caller closes the channel so the next poll pins it; a device
	 * another channel already holds is the caller's to rotate away. */
	RADIANT_SELF_BIND,
	/* Bound but not open pinned: close it if it is open, else configure
	 * it pinned (device number, type and trans of the binding, the
	 * binding's period, infinite search) and open it. */
	RADIANT_SELF_PIN_REOPEN,
	/* Unbound, closed, window open: open for discovery on its current
	 * search profile. */
	RADIANT_SELF_DISCOVER_OPEN,
	/* Unbound and open with the window closed (or still open on an
	 * evicted sensor's pinned search): close it. A closed window means
	 * IDLE - an unpaired channel does not search. */
	RADIANT_SELF_CLOSE_TO_IDLE,
	/* Discovery timed out: move to the next search profile and reopen. */
	RADIANT_SELF_ROTATE,
	/* Wiped by a host reset (or never assigned: boot): assign it again. */
	RADIANT_SELF_REESTABLISH,
	/* A host reset wiped this channel and the host took the number before
	 * the caller could reassign it. The binding can no longer be served
	 * here: release it (the caller frees the row) and stop touching the
	 * channel. */
	RADIANT_SELF_RELEASE,
};

enum radiant_self_action radiant_self_decide(const struct radiant_self_chan_in *in);

/*
 * Eviction. A new pairing needs a channel and every one is pinned: pick the
 * pinned channel that is NOT tracking and has gone longest since it last was.
 * A tracking sensor is never evicted - a rider's live power meter does not
 * make way for a strap found in a drawer - so this may pick nothing.
 *
 * Picks nothing unless the window is open and no channel is unbound (IDLE or
 * DISCOVER: a new pairing already has somewhere to go). At most once per
 * window opening (evicted_this_window), so that one press of the button frees
 * one channel rather than every absent one; self_channels.c goes further and
 * asks only on the poll that first sees a window open, so a sensor that blips
 * later in the window keeps its channel. A channel the caller may not use
 * (the host took it) is passed as bound and tracking.
 */
struct radiant_self_evict_in {
	bool     bound;
	bool     tracking;
	uint32_t last_tracking_ms;
};

#define RADIANT_SELF_EVICT_NONE 0xFFu

uint8_t radiant_self_pick_evict(const struct radiant_self_evict_in *chans, uint8_t n,
				bool window_open, bool evicted_this_window, uint32_t now_ms);

/*
 * The network key after a host SYSTEM_RESET.
 *
 * A host reset (Zwift sends one at startup) clears every network key along
 * with every channel. The self channels need the ANT+ key on their network,
 * but the host may have chosen another one, and the host owns the stack it is
 * talking to. So:
 *
 *   host has written no key since the reset   -> APPLY ours
 *   host wrote the ANT+ key                   -> nothing to do
 *   host wrote some other key                 -> leave it, and say once that
 *                                                the self channels are deaf
 *
 * and if the host writes a different key AFTER ours was applied, the host
 * wins, and that is said once too.
 *
 * Handled once per reset generation. Boot counts as a generation: the caller
 * starts its state from radiant_self_key_init(), which has seen none.
 */
struct radiant_self_key_state {
	bool     seen_any;
	uint32_t reset_gen;
	uint32_t key_writes;
};

struct radiant_self_key_in {
	uint32_t reset_gen;   /* host resets since boot */
	uint32_t key_writes;  /* host writes of this network's key since boot */
	bool     key_written; /* ...since the last reset */
	bool     key_is_ours; /* the key the host wrote is the one we need */
};

enum radiant_self_key_action {
	RADIANT_SELF_KEY_NONE = 0,
	RADIANT_SELF_KEY_APPLY,
	/* The host's key is not ours: the self channels hear nothing. Reported
	 * once per change, never acted on. */
	RADIANT_SELF_KEY_HOST_FOREIGN,
};

void radiant_self_key_init(struct radiant_self_key_state *st);

/* *new_generation is set when this call is the first to see a reset (or
 * boot), which is the caller's cue that every channel was wiped. */
enum radiant_self_key_action radiant_self_key_decide(struct radiant_self_key_state *st,
						     const struct radiant_self_key_in *in,
						     bool *new_generation);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_SELF_POLICY_H_ */
