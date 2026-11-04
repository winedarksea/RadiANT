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
 * Nothing here includes an ANT header.
 */

#ifndef RADIANT_SELF_POLICY_H_
#define RADIANT_SELF_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
