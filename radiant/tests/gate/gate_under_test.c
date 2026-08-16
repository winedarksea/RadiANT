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
	out->bootstrapping  = g.bootstrapping;
	out->anchor_valid   = g.anchor_valid;
	out->mpsl_owes      = g.mpsl_owes;
	out->granted_len_us = g.granted_len_us;
	out->want_len_us    = g.want_len_us;
	out->extendable     = g.extendable;
	out->end_margin_us  = g.end_margin_us;
}
