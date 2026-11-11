/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gate_under_test.c - the real gate, compiled with a window into it. The
 * one #include below is the file under test, verbatim and unmodified;
 * everything after it is read-only.
 *
 * Why the .c and not the .o: the invariant this suite defends is observable
 * from outside (the fake backend counts calls), but the STATE it's carried
 * by is not - g.hw_held (A1's fix) and stats.late_disarm (names a fault
 * with no other symptom) are both static. An accessor behind
 * #ifdef CONFIG_ZTEST in the production file would be worse here
 * specifically: the subject is which code path runs on which exit, so a
 * compiled-in conditional would make the tested build and the shipping
 * build different builds. Including the translation unit costs nothing at
 * runtime and leaves the shipping file byte-identical.
 *
 * Consequence to remember: this must be the ONLY file that includes it, or
 * the statics exist twice and the suites silently test two different gates.
 */

#include "../../src/radiant_radio_nrf_gate_mpsl.c"

#include "gate_probe.h"

void gate_probe_read(struct gate_probe *out)
{
	out->granted          = stats.granted;
	out->grant_end_calls  = stats.grant_end_calls;
	out->release_disarm   = stats.release_disarm;
	out->idle_disarm      = stats.idle_disarm;
	out->late_disarm      = stats.late_disarm;
	out->placed           = stats.placed;
	out->blocked          = stats.blocked;
	out->cancelled        = stats.cancelled;
	out->refused_eagain   = stats.refused_eagain;
	out->extends_ok       = stats.extends_ok;
	out->extends_failed   = stats.extends_failed;
	out->sent_after_grant = stats.sent_after_grant;
	out->sent_blocked     = stats.sent_blocked;
	out->deny_suppressed  = stats.deny_suppressed;
	out->overstayed       = stats.overstayed;
	out->invalid_return   = stats.invalid_return;
	out->unknown_signal   = stats.unknown_signal;
	out->answered_inline  = stats.answered_inline;
	out->den_owed         = stats.den_owed;

	out->hw_held        = g.hw_held;
	out->granted_flag   = g.granted;
	out->pending        = g.pending;
	out->release_wanted = g.release_wanted;
	out->ended          = g.ended;
	/* Session 0's: the only session every pre-d2 test has. */
	out->bootstrapping  = ss[0].bootstrapping;
	out->anchor_valid   = ss[0].anchor_valid;
	out->mpsl_owes      = ss[0].mpsl_owes;
	out->granted_len_us = g.granted_len_us;
	out->want_len_us    = g.want_len_us;
	out->extendable     = g.extendable;
	out->end_margin_us  = g.end_margin_us;

	out->n_open = n_open;
	for (uint32_t i = 0u; i < 2u; i++) {
		const struct sess *s = &ss[(i < NSESS) ? i : 0u];

		out->role[i] = (i < NSESS) ? s->role : 0u;
		out->book_failed[i] = (i < NSESS) && s->book_failed;
		out->s_owes[i] = (i < NSESS) && s->mpsl_owes;
		out->s_anchor_valid[i] = (i < NSESS) && s->anchor_valid;
	}
	out->op_sess        = g.op_sess;
	out->book_placed    = stats.book_placed;
	out->book_hit       = stats.book_hit;
	out->book_denied    = stats.book_denied;
	out->book_blocked   = stats.book_blocked;
	out->book_mismatch  = stats.book_mismatch;
	out->book_early     = stats.book_early;
	out->book_busy      = stats.book_busy;
	out->book_near      = stats.book_near;
	out->orphan_start   = stats.orphan_start;
	out->den_nosess     = stats.den_nosess;
	out->refused_too_near = stats.refused_too_near;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	out->el_lead_us     = el_lead_us;
	out->d2_on          = d2_on;
#endif
}

void gate_probe_fire_booking_due(void)
{
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	booking_due_fire();
#endif
}

void gate_probe_refresh_anchor(uint32_t session)
{
	ss[session].anchor = radiant_radio_now();
	ss[session].anchor_valid = true;
	ss[session].want_place = false;
	ss[session].bootstrapping = false;
	ss[session].mpsl_owes = false;
}

/* The one writer in this file: test-harness state reset, never behaviour. */
void gate_probe_reset_elastic(void)
{
	elastic_initial_us = ELASTIC_INITIAL_US;
	elastic_skew_us = 0u;
	for (uint32_t i = 0u; i < NSESS; i++) {
		ss[i].blocked_run = 0u;
	}
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	el_lead_us = EL_LEAD_START_US;
	el_block_run = 0u;
#endif
}
