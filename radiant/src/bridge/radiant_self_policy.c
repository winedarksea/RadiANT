/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_self_policy.c - see radiant_self_policy.h. Pure functions; no I/O.
 */

#include <string.h>

#include "radiant_self_policy.h"

void radiant_self_key_init(struct radiant_self_key_state *st)
{
	memset(st, 0, sizeof(*st));
}

enum radiant_self_key_action radiant_self_key_decide(struct radiant_self_key_state *st,
						     const struct radiant_self_key_in *in,
						     bool *new_generation)
{
	bool fresh = !st->seen_any || in->reset_gen != st->reset_gen;
	bool rewritten = in->key_writes != st->key_writes;

	st->seen_any = true;
	st->reset_gen = in->reset_gen;
	st->key_writes = in->key_writes;
	if (new_generation != NULL) {
		*new_generation = fresh;
	}

	if (fresh) {
		/* Once per generation, whatever the host has or has not done
		 * yet. A host that writes its key a moment later is the
		 * `rewritten` case on a later call. */
		if (!in->key_written) {
			return RADIANT_SELF_KEY_APPLY;
		}
		return in->key_is_ours ? RADIANT_SELF_KEY_NONE : RADIANT_SELF_KEY_HOST_FOREIGN;
	}

	/* Same generation, and the host wrote a key since the last look: it
	 * wins over ours either way, so the only question is whether to say
	 * that the self channels just went deaf. */
	if (rewritten && in->key_written && !in->key_is_ours) {
		return RADIANT_SELF_KEY_HOST_FOREIGN;
	}
	return RADIANT_SELF_KEY_NONE;
}

enum radiant_self_action radiant_self_decide(const struct radiant_self_chan_in *in)
{
	/* Wiped by a host reset (or never assigned: boot is the first one). */
	if (in->radio == RADIANT_SELF_RADIO_UNASSIGNED) {
		return RADIANT_SELF_REESTABLISH;
	}
	/* The host took the number after its reset. Never touch its channel;
	 * only let go of a binding that can no longer be served. */
	if (!in->owned) {
		return in->bound ? RADIANT_SELF_RELEASE : RADIANT_SELF_NONE;
	}

	if (in->bound) {
		/* PINNED, the steady state, whatever the window and whether or
		 * not the sensor is there: a dropout and a search are not
		 * reasons to let go. Three ways to not be pinned yet: closed
		 * (just bound, or just re-established), still open on the
		 * discovery search it bound from (an asynchronous close), or
		 * tracking something that is not the bound sensor. All three
		 * are the same action: close if open, else open pinned. */
		if (in->radio == RADIANT_SELF_RADIO_ASSIGNED || !in->open_pinned) {
			return RADIANT_SELF_PIN_REOPEN;
		}
		if (in->radio == RADIANT_SELF_RADIO_TRACKING && !in->identity_matches) {
			return RADIANT_SELF_PIN_REOPEN;
		}
		return RADIANT_SELF_NONE;
	}

	/* Unbound. Still open pinned means an eviction's close has not landed
	 * yet: that search is for the evicted sensor, and must not be allowed
	 * to find it and bind it straight back. */
	if (in->open_pinned && in->radio != RADIANT_SELF_RADIO_ASSIGNED) {
		return RADIANT_SELF_CLOSE_TO_IDLE;
	}

	if (!in->window_open) {
		/* IDLE. An unpaired channel does not search. */
		return (in->radio == RADIANT_SELF_RADIO_ASSIGNED) ? RADIANT_SELF_NONE
								  : RADIANT_SELF_CLOSE_TO_IDLE;
	}

	switch (in->radio) {
	case RADIANT_SELF_RADIO_ASSIGNED:
		/* Closed after a discovery open: its search timed out. */
		return in->open_discover ? RADIANT_SELF_ROTATE : RADIANT_SELF_DISCOVER_OPEN;
	case RADIANT_SELF_RADIO_TRACKING:
		return RADIANT_SELF_BIND;
	default:
		return RADIANT_SELF_NONE;
	}
}

uint8_t radiant_self_pick_evict(const struct radiant_self_evict_in *chans, uint8_t n,
				bool window_open, bool evicted_this_window, uint32_t now_ms)
{
	uint8_t best = RADIANT_SELF_EVICT_NONE;
	uint32_t best_absent = 0u;

	if (!window_open || evicted_this_window) {
		return RADIANT_SELF_EVICT_NONE;
	}
	/* A free channel (IDLE or DISCOVER) means a new pairing has somewhere
	 * to go already. */
	for (uint8_t i = 0u; i < n; i++) {
		if (!chans[i].bound) {
			return RADIANT_SELF_EVICT_NONE;
		}
	}
	for (uint8_t i = 0u; i < n; i++) {
		/* Unsigned subtraction, so a wrapped uptime still orders. */
		uint32_t absent = now_ms - chans[i].last_tracking_ms;

		if (chans[i].tracking) {
			continue;
		}
		if (best == RADIANT_SELF_EVICT_NONE || absent > best_absent) {
			best = i;
			best_absent = absent;
		}
	}
	return best;
}
