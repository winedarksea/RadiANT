/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ctrl_arbiter.c - the FE-C / FTMS control token. See
 * <radiant/radiant_ctrl_arbiter.h> for the policy and for why this file has no
 * kernel in it. Moved from apps/treadmill/src/treadmill_control.c unchanged in
 * behaviour; the gated page set is now a parameter.
 */

#include <errno.h>

#include <radiant/radiant_ctrl_arbiter.h>

/*
 * One owner, and the instant it last did something. `last_ms` is consulted
 * only for an ANT owner: a BLE claim is released by an event and must NOT
 * lapse on a timer, or the machine would be handed back to a head unit
 * mid-workout with the phone still showing that it owns the deck.
 */
static enum radiant_ctrl_src ctrl_owner;
static uint32_t ctrl_last_ms;
static uint32_t ctrl_ant_timeout_ms;
static uint8_t ctrl_gated_pages;

void radiant_ctrl_init(uint32_t ant_timeout_ms, uint8_t gated_pages)
{
	ctrl_owner = RADIANT_CTRL_NONE;
	ctrl_last_ms = 0u;
	ctrl_ant_timeout_ms = ant_timeout_ms;
	ctrl_gated_pages = gated_pages;
}

bool radiant_ctrl_fec_page_is_gated(uint8_t fec_page)
{
	if (fec_page < RADIANT_CTRL_PAGE_BASIC_RESISTANCE ||
	    fec_page > RADIANT_CTRL_PAGE_TRACK_RESISTANCE) {
		return false;
	}
	return (ctrl_gated_pages & RADIANT_CTRL_GATE(fec_page)) != 0u;
}

int radiant_ctrl_claim(enum radiant_ctrl_src src, bool explicit_req, uint32_t now_ms)
{
	if (src == RADIANT_CTRL_NONE) {
		return -EACCES;
	}

	if (ctrl_owner == src) {
		/* A refresh, the common case: every FE-C command page from the
		 * owning controller pushes the idle timeout out. */
		ctrl_last_ms = now_ms;
		return 0;
	}

	if (ctrl_owner != RADIANT_CTRL_NONE && !explicit_req) {
		/* Somebody else has it and this claim is inferred from the act
		 * of commanding: refused, and the caller owes an answer. */
		return -EACCES;
	}

	/* Free, or an explicit acquire preempting an implicit holder. */
	ctrl_owner = src;
	ctrl_last_ms = now_ms;
	return 0;
}

void radiant_ctrl_release(enum radiant_ctrl_src src)
{
	if (ctrl_owner == src) {
		ctrl_owner = RADIANT_CTRL_NONE;
	}
}

enum radiant_ctrl_src radiant_ctrl_revoke(void)
{
	enum radiant_ctrl_src prev = ctrl_owner;

	ctrl_owner = RADIANT_CTRL_NONE;
	return prev;
}

void radiant_ctrl_tick(uint32_t now_ms)
{
	if (ctrl_owner != RADIANT_CTRL_ANT || ctrl_ant_timeout_ms == 0u) {
		return;
	}

	/* Unsigned subtraction: a wrapped 32-bit uptime (49.7 days) still
	 * yields the true elapsed interval. */
	if ((now_ms - ctrl_last_ms) >= ctrl_ant_timeout_ms) {
		ctrl_owner = RADIANT_CTRL_NONE;
	}
}

enum radiant_ctrl_src radiant_ctrl_owner(void)
{
	return ctrl_owner;
}
