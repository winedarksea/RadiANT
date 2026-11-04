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
