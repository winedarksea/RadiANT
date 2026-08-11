/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_freq.c - adaptive frequency.
 *
 * Provenance: docs/decisions/0005-extension-inside-ant-plus.md axis 5,
 * docs/decisions/0012-adaptive-frequency.md and docs/radiant-telemetry.md's page
 * map, all of which are this project's own documents. No adopter-gated ANT+
 * device profile document was read for this file, no sdk-ant source was
 * consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md. The argument for every rule is in
 * profile_freq.h.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_chanmap.h>
#include <radiant_core/radiant_radio_hal.h>

#include "profile_freq.h"

/* The countdown field is one byte and the unit multiplies it, so K's ceiling is
 * a consequence of the wire rather than a second opinion about it. */
_Static_assert(PROFILE_FREQ_K_MAX ==
		       (uint16_t)(PROFILE_FREQ_COUNTDOWN_MAX * PROFILE_FREQ_UNIT),
	       "K's ceiling no longer follows from the countdown field");
_Static_assert(PROFILE_FREQ_K_DEFAULT % PROFILE_FREQ_UNIT == 0u,
	       "the default K is not expressible in the countdown field");
_Static_assert(PROFILE_FREQ_ANNOUNCE_EVERY == PROFILE_FREQ_UNIT,
	       "the announcement interval and the countdown quantum diverged");
_Static_assert(PROFILE_FREQ_RF_HIGH <= RADIANT_RF_INDEX_MAX,
	       "a default candidate is outside the HAL's index range");

const uint8_t profile_freq_defaults[PROFILE_FREQ_N_DEFAULTS] = {
	PROFILE_FREQ_RF_LOW,
	PROFILE_FREQ_RF_MID,
	PROFILE_FREQ_RF_HIGH,
};

/* ---------------------------------------------------------------------------
 * Selection
 * ---------------------------------------------------------------------------
 */

static const struct profile_freq_evidence *
find_evidence(const struct profile_freq_evidence *ev, uint8_t n, uint8_t rf)
{
	uint8_t i;

	for (i = 0u; i < n; i++) {
		if (ev[i].rf_index == rf) {
			return &ev[i];
		}
	}
	return NULL;
}

/*
 * True when `a` outranks `b`. Quieter busy figure first, then quieter floor,
 * then the lower index.
 *
 * THE LAST TIE-BREAK IS NOT COSMETIC. Two receivers that ranked two equally
 * quiet indices by arrival order would recommend different frequencies for the
 * same node from the same evidence, and the node would take whichever asked
 * last. An arbitrary rule that both sides share beats a natural one that only
 * one of them has.
 */
static bool outranks(const struct profile_freq_evidence *a,
		     const struct profile_freq_evidence *b)
{
	if (a->busy_dbm != b->busy_dbm) {
		return a->busy_dbm < b->busy_dbm;
	}
	if (a->floor_dbm != b->floor_dbm) {
		return a->floor_dbm < b->floor_dbm;
	}
	return a->rf_index < b->rf_index;
}

int profile_freq_select(uint8_t incumbent, const struct profile_freq_evidence *ev,
			uint8_t n, uint8_t *out_rf)
{
	const struct profile_freq_evidence *here;
	const struct profile_freq_evidence *best = NULL;
	uint8_t i;

	if (ev == NULL || out_rf == NULL || n == 0u ||
	    incumbent > RADIANT_RF_INDEX_MAX) {
		return -EINVAL;
	}
	for (i = 0u; i < n; i++) {
		if (ev[i].rf_index > RADIANT_RF_INDEX_MAX) {
			return -EINVAL;
		}
	}

	here = find_evidence(ev, n, incumbent);
	if (here == NULL || !here->have) {
		/* Not a failure of the candidates - a failure to have measured
		 * where we already are, which is the half of the comparison a
		 * margin cannot be computed without. */
		return -ENODATA;
	}

	for (i = 0u; i < n; i++) {
		const struct profile_freq_evidence *c = &ev[i];

		if (!c->have || c->rf_index == incumbent) {
			continue;
		}
		/* Integer arithmetic in int, not int8: busy_dbm + 6 on a
		 * candidate at -123 dBm is representable, and on the promotion
		 * rules of C it is done in int anyway. Written out so that is a
		 * decision rather than an accident. */
		if ((int)c->busy_dbm + PROFILE_FREQ_MARGIN_DB > (int)here->busy_dbm) {
			continue;
		}
		if (best == NULL || outranks(c, best)) {
			best = c;
		}
	}

	if (best == NULL) {
		/* Either nothing was measured, or nothing cleared the margin.
		 * The two are distinguished because they mean different things
		 * to a caller: one is "ask again when the map has filled in",
		 * the other is "you are already in the right place". */
		for (i = 0u; i < n; i++) {
			if (ev[i].have && ev[i].rf_index != incumbent) {
				return -EALREADY;
			}
		}
		return -ENODATA;
	}

	*out_rf = best->rf_index;
	return 0;
}

int profile_freq_select_from_map(uint8_t incumbent, const uint8_t *cand,
				 uint8_t n_cand, uint8_t *out_rf)
{
	struct profile_freq_evidence ev[PROFILE_FREQ_REACQUIRE_MAX];
	struct radiant_chanmap_report rep;
	uint8_t n = 0u;
	uint8_t i;

	if (out_rf == NULL) {
		return -EINVAL;
	}
	if (cand == NULL) {
		cand = profile_freq_defaults;
		n_cand = PROFILE_FREQ_N_DEFAULTS;
	}
	if (n_cand == 0u || n_cand > (uint8_t)(PROFILE_FREQ_REACQUIRE_MAX - 1u)) {
		return -EINVAL;
	}

	memset(ev, 0, sizeof(ev));

	ev[n].rf_index = incumbent;
	ev[n].have = radiant_chanmap_get(incumbent, &rep);
	if (ev[n].have) {
		ev[n].busy_dbm = rep.busy_dbm;
		ev[n].floor_dbm = rep.floor_dbm;
	}
	n++;

	for (i = 0u; i < n_cand; i++) {
		if (cand[i] == incumbent) {
			continue;
		}
		ev[n].rf_index = cand[i];
		ev[n].have = radiant_chanmap_get(cand[i], &rep);
		if (ev[n].have) {
			ev[n].busy_dbm = rep.busy_dbm;
			ev[n].floor_dbm = rep.floor_dbm;
		}
		n++;
	}

	return profile_freq_select(incumbent, ev, n, out_rf);
}

/* ---------------------------------------------------------------------------
 * The page
 * ---------------------------------------------------------------------------
 */

int profile_freq_encode(uint8_t target, uint8_t countdown, uint8_t *body)
{
	if (body == NULL || target > RADIANT_RF_INDEX_MAX || countdown == 0u) {
		return -EINVAL;
	}

	memset(body, 0, PROFILE_FREQ_LEN);
	body[0] = PROFILE_FREQ_PAGE;
	body[1] = PROFILE_FREQ_FRAME_BYTE;
	body[2] = target;
	body[3] = countdown;
	/* [4..7] stay zero: every reservation in this protocol is populated with
	 * zeros, and the decoder below refuses anything else. */
	return 0;
}

int profile_freq_decode(const uint8_t *body, uint8_t len, uint8_t *target,
			uint8_t *countdown)
{
	uint8_t i;

	if (body == NULL || target == NULL || countdown == NULL ||
	    len != PROFILE_FREQ_LEN) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FREQ_PAGE) {
		return -ENOENT;
	}
	if (body[1] != PROFILE_FREQ_FRAME_BYTE) {
		return -EPROTO;
	}
	if (body[2] > RADIANT_RF_INDEX_MAX || body[3] == 0u) {
		return -EPROTO;
	}
	for (i = 4u; i < PROFILE_FREQ_LEN; i++) {
		if (body[i] != 0u) {
			return -EPROTO;
		}
	}

	*target = body[2];
	*countdown = body[3];
	return 0;
}

/* ---------------------------------------------------------------------------
 * The node's half
 * ---------------------------------------------------------------------------
 */

int profile_freq_init(struct profile_freq *pf, const struct profile_freq_cfg *cfg)
{
	if (pf == NULL || cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->k != 0u) {
		if (cfg->k > PROFILE_FREQ_K_MAX ||
		    (cfg->k % PROFILE_FREQ_UNIT) != 0u) {
			return -EINVAL;
		}
	}

	memset(pf, 0, sizeof(*pf));
	pf->cfg = *cfg;
	if (pf->cfg.k == 0u) {
		pf->cfg.k = PROFILE_FREQ_K_DEFAULT;
	}
	if (pf->cfg.min_move_s == 0u) {
		pf->cfg.min_move_s = PROFILE_FREQ_MIN_MOVE_S;
	}
	pf->rf_index = PROFILE_FREQ_RF_HOME;
	pf->target = PROFILE_FREQ_RF_HOME;
	pf->armed = true;
	return 0;
}

uint8_t profile_freq_rf_index(const struct profile_freq *pf)
{
	return (pf != NULL && pf->armed) ? pf->rf_index : PROFILE_FREQ_RF_HOME;
}

bool profile_freq_off_home(const struct profile_freq *pf)
{
	return profile_freq_rf_index(pf) != PROFILE_FREQ_RF_HOME;
}

bool profile_freq_announcing(const struct profile_freq *pf)
{
	return pf != NULL && pf->armed && pf->announcing;
}

uint8_t profile_freq_target(const struct profile_freq *pf)
{
	if (pf == NULL || !pf->armed) {
		return PROFILE_FREQ_RF_HOME;
	}
	return pf->announcing ? pf->target : pf->rf_index;
}

int profile_freq_begin(struct profile_freq *pf, uint8_t target, uint64_t now_us)
{
	if (pf == NULL || !pf->armed) {
		return -EINVAL;
	}
	pf->now_us = now_us;

	if (target > RADIANT_RF_INDEX_MAX) {
		pf->refused_range++;
		return -EINVAL;
	}
	if (target == pf->rf_index) {
		/* Uncounted on purpose. A caller asking a node to move where it
		 * already is has not been refused anything; it has asked for
		 * nothing, and a counter here would tick every time a selector
		 * confirmed the status quo. */
		return -EALREADY;
	}
	if (pf->cfg.sparse) {
		pf->refused_sparse++;
		return -ENOTSUP;
	}
	if (pf->announcing) {
		pf->refused_busy++;
		return -EBUSY;
	}
	if (pf->ever_moved) {
		uint64_t floor_us = (uint64_t)pf->cfg.min_move_s * 1000000u;

		if (now_us < pf->last_move_us ||
		    (now_us - pf->last_move_us) < floor_us) {
			pf->refused_rate++;
			return -EAGAIN;
		}
	}

	pf->announcing = true;
	pf->target = target;
	pf->move_at = pf->msgs + (uint32_t)pf->cfg.k;
	return 0;
}

/*
 * The countdown a fresh announcement can honestly name, and the re-anchor.
 *
 * `after` is the number of messages this node will have sent once the message
 * being built has gone out, which is the instant both ends measure from.
 */
static uint8_t countdown_now(struct profile_freq *pf, uint32_t after)
{
	uint32_t remaining;
	uint32_t c;

	remaining = (pf->move_at > after) ? (pf->move_at - after) : 0u;
	c = (remaining + PROFILE_FREQ_UNIT - 1u) / PROFILE_FREQ_UNIT;
	if (c == 0u) {
		/* The move is due before another announcement could be heard.
		 * One unit is the smallest thing this field can say, and saying
		 * it moves the target out rather than naming a message that has
		 * already gone. */
		c = 1u;
	}
	if (c > PROFILE_FREQ_COUNTDOWN_MAX) {
		c = PROFILE_FREQ_COUNTDOWN_MAX;
	}

	/* MOVE THE TARGET TO WHAT WAS JUST SAID. Every receiver that heard this
	 * copy computes the identical message; one holding an older copy is at
	 * most PROFILE_FREQ_UNIT - 1 messages early, which costs it the tail of
	 * the old channel rather than the head of the new one. */
	pf->move_at = after + c * PROFILE_FREQ_UNIT;
	return (uint8_t)c;
}

bool profile_freq_claim(uint32_t m, uint8_t *body, void *user)
{
	struct profile_freq *pf = (struct profile_freq *)user;
	uint8_t countdown;

	(void)m;

	if (pf == NULL || !pf->armed || !pf->announcing || body == NULL) {
		return false;
	}
	if ((pf->msgs % PROFILE_FREQ_ANNOUNCE_EVERY) != 0u) {
		return false;
	}
	/*
	 * THE LAST SLOT BEFORE THE MOVE IS NOT ANNOUNCED, and without this the
	 * feature never fires at all. The re-anchor below moves the target to
	 * the smallest countdown it can express, so an announcement built for
	 * the message the move is already due on would push the move one unit
	 * further out - and the next one would do it again, forever. A node that
	 * has nothing left to say says nothing and moves.
	 */
	if (pf->move_at <= pf->msgs + 1u) {
		return false;
	}

	countdown = countdown_now(pf, pf->msgs + 1u);
	if (profile_freq_encode(pf->target, countdown, body) != 0) {
		return false;
	}
	pf->announcements++;
	return true;
}

bool profile_freq_sent(struct profile_freq *pf, uint64_t now_us)
{
	if (pf == NULL || !pf->armed) {
		return false;
	}

	pf->now_us = now_us;
	pf->msgs++;
	if (!pf->announcing || pf->msgs < pf->move_at) {
		return false;
	}

	pf->rf_index = pf->target;
	pf->announcing = false;
	pf->ever_moved = true;
	/* The rate limit is measured from HERE and not from the request that
	 * started the countdown, so a long K cannot be used to buy a second move
	 * sooner - the floor is between moves, which is what a receiver
	 * following them actually pays. */
	pf->last_move_us = now_us;
	pf->moves++;
	return true;
}

int profile_freq_apply(struct profile_descriptor *d, uint8_t rf_index)
{
	if (d == NULL || rf_index > PROFILE_TLM_RF_INDEX_MAX) {
		return -EINVAL;
	}

	d->rf_index = rf_index;
	if (rf_index == PROFILE_TLM_RF_INDEX_DEFAULT) {
		d->flags &= (uint8_t)~PROFILE_TLM_FLAG_OFF_RF57;
	} else {
		d->flags |= PROFILE_TLM_FLAG_OFF_RF57;
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * The receiver's half
 * ---------------------------------------------------------------------------
 */

void profile_freq_rx_init(struct profile_freq_rx *rx)
{
	if (rx == NULL) {
		return;
	}
	memset(rx, 0, sizeof(*rx));
	rx->target = PROFILE_FREQ_RF_HOME;
	rx->last_target = PROFILE_FREQ_RF_HOME;
}

int profile_freq_rx_slot(struct profile_freq_rx *rx, const uint8_t *body,
			 uint8_t len)
{
	uint8_t target;
	uint8_t countdown;
	int rc;

	if (rx == NULL) {
		return -EINVAL;
	}

	/* The slot happened whether or not anything was in it, and it is counted
	 * before the message is examined so that an announcement's own slot is
	 * inside both counts - the node's message index and this one - exactly
	 * once. */
	rx->slots++;

	if (body == NULL) {
		return 0;
	}
	if (len != PROFILE_FREQ_LEN || body[0] != PROFILE_FREQ_PAGE) {
		return 0;
	}

	rc = profile_freq_decode(body, len, &target, &countdown);
	if (rc != 0) {
		rx->rejected++;
		return -EPROTO;
	}

	rx->armed = true;
	rx->target = target;
	rx->last_target = target;
	rx->have_last = true;
	rx->move_at = rx->slots + (uint32_t)countdown * PROFILE_FREQ_UNIT;
	rx->heard++;
	return 1;
}

bool profile_freq_rx_due(const struct profile_freq_rx *rx)
{
	return rx != NULL && rx->armed && rx->slots >= rx->move_at;
}

int profile_freq_rx_target(const struct profile_freq_rx *rx, uint8_t *rf_index)
{
	if (rx == NULL || rf_index == NULL) {
		return -EINVAL;
	}
	if (!rx->armed) {
		return -ENOENT;
	}
	*rf_index = rx->target;
	return 0;
}

void profile_freq_rx_clear(struct profile_freq_rx *rx)
{
	if (rx == NULL) {
		return;
	}
	rx->armed = false;
	rx->move_at = 0u;
}

uint8_t profile_freq_reacquire_order(const struct profile_freq_rx *rx,
				     uint8_t *out, uint8_t cap)
{
	uint8_t n = 0u;
	uint8_t i;
	uint8_t j;

	if (out == NULL || cap == 0u) {
		return 0u;
	}

	if (rx != NULL && rx->have_last) {
		out[n++] = rx->last_target;
	}

	for (i = 0u; i < PROFILE_FREQ_N_DEFAULTS && n < cap; i++) {
		bool dup = false;

		for (j = 0u; j < n; j++) {
			if (out[j] == profile_freq_defaults[i]) {
				dup = true;
			}
		}
		if (!dup) {
			out[n++] = profile_freq_defaults[i];
		}
	}

	if (n < cap) {
		bool dup = false;

		for (j = 0u; j < n; j++) {
			if (out[j] == PROFILE_FREQ_RF_HOME) {
				dup = true;
			}
		}
		if (!dup) {
			out[n++] = PROFILE_FREQ_RF_HOME;
		}
	}

	return n;
}
