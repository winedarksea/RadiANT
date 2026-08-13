/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gate_under_test.c - the real gate, compiled with a window into it.
 *
 * Provenance: original clean-room work. The one #include below is the file
 * under test, verbatim and unmodified; everything after it is read-only.
 *
 * ---------------------------------------------------------------------------
 * WHY THE .c AND NOT THE .o
 * ---------------------------------------------------------------------------
 *
 * The invariant this suite defends - exactly one radiant_nrf_gate_on_grant_end()
 * per granted timeslot, on every exit - is observable from outside: the fake
 * backend counts the calls. But the STATE the invariant is carried by is not.
 * g.hw_held is the whole of A1's fix and it is a static bool; stats.late_disarm
 * exists precisely to name a fault that has no other symptom, and it is a static
 * counter. A test that could not read them would be asserting on the symptom of
 * the fix rather than on the fix.
 *
 * The alternative was an accessor inside the production file behind
 * #ifdef CONFIG_ZTEST. That is worse, and specifically worse HERE: the whole
 * subject is which code path runs on which exit, so a conditional compiled into
 * the file under test means the tested build and the shipping build are not the
 * same build. Including the translation unit costs nothing at runtime and leaves
 * the shipping file byte-identical.
 *
 * The consequence to remember: this file must be the ONLY one that includes it,
 * or the statics inside it exist twice and the suites silently test two
 * different gates.
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
}
