/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The control-owner token. See treadmill_control.h for the policy and for why
 * this file has no kernel in it.
 */

#include <errno.h>

#include "treadmill_control.h"

/*
 * One owner, and the instant it last did something. `last_ms` is only ever
 * consulted for an ANT owner: a BLE claim is released by an event (Reset, or a
 * disconnect) and must NOT lapse on a timer, because a phone that has connected
 * and taken control is still there whether or not it has commanded anything
 * this minute. Expiring it would hand the machine back to a head unit
 * mid-workout with the phone still on screen showing that it owns the deck.
 */
static enum treadmill_ctrl_src ctrl_owner;
static uint32_t                ctrl_last_ms;
static uint32_t                ctrl_ant_timeout_ms;

void treadmill_control_init(uint32_t ant_timeout_ms)
{
	ctrl_owner = TREADMILL_CTRL_NONE;
	ctrl_last_ms = 0u;
	ctrl_ant_timeout_ms = ant_timeout_ms;
}

int treadmill_control_claim(enum treadmill_ctrl_src src, bool explicit_req,
			    uint32_t now_ms)
{
	if (src == TREADMILL_CTRL_NONE) {
		return -EACCES;
	}

	if (ctrl_owner == src) {
		/* A refresh, which is the common case: every FE-C command page
		 * from the owning controller pushes the idle timeout out. */
		ctrl_last_ms = now_ms;
		return 0;
	}

	if (ctrl_owner != TREADMILL_CTRL_NONE && !explicit_req) {
		/* Somebody else has it and this is a claim inferred from the
		 * mere act of commanding. Refused - and the caller owes the
		 * refusal an answer on the wire. */
		return -EACCES;
	}

	/* Free, or an explicit acquire preempting an implicit holder. */
	ctrl_owner = src;
	ctrl_last_ms = now_ms;
	return 0;
}

void treadmill_control_release(enum treadmill_ctrl_src src)
{
	if (ctrl_owner == src) {
		ctrl_owner = TREADMILL_CTRL_NONE;
	}
}

void treadmill_control_tick(uint32_t now_ms)
{
	if (ctrl_owner != TREADMILL_CTRL_ANT || ctrl_ant_timeout_ms == 0u) {
		return;
	}

	/*
	 * Unsigned subtraction, so a wrapped k_uptime_get_32() (49.7 days)
	 * still yields the true elapsed interval rather than a huge one that
	 * would drop the claim instantly. Same idiom as treadmill_ble.c's
	 * notify pacing, and for the same reason.
	 */
	if ((now_ms - ctrl_last_ms) >= ctrl_ant_timeout_ms) {
		ctrl_owner = TREADMILL_CTRL_NONE;
	}
}

enum treadmill_ctrl_src treadmill_control_owner(void)
{
	return ctrl_owner;
}

bool treadmill_control_page_is_gated(uint8_t fec_page)
{
	return (fec_page == TREADMILL_CTRL_PAGE_TRACK_RESISTANCE) ||
	       (fec_page == TREADMILL_CTRL_PAGE_WIND_RESISTANCE);
}
