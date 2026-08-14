/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_compat.c - the RadiANT compat pages, inserted through
 * profile_sched.c's client seam.
 *
 * Provenance: docs/radiant-security.md section 11 (normative for the bytes),
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md and
 * docs/profile-registry.md, mirrored byte for byte by tools/ant_pages.py's
 * encode_compat_beacon() / encode_compat_attest_tier1() /
 * encode_compat_attest_tier2(). All of those are this project's own documents
 * and code; no ANT+ device profile document was read for this
 * file and nothing here derives from libant.a.
 *
 * The only file in src/profiles/ that calls radiant_sec_compat (see header):
 * radiant_sec_compat.c knows no page number, profile_hr.c/profile_power.c
 * know no key, and this file is where a subtype nibble becomes a page byte.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_sec_compat.h>

#include "profile_compat.h"
#include "profile_sched.h"

/* The window sizes, in the order their 2-bit code numbers them. Enumerated
 * rather than ranged because N is simultaneously the airtime cost, the
 * verification latency and the DoS amplification factor. */
static const uint8_t compat_windows[4] = { 4u, 8u, 16u, 32u };

static int window_code(uint8_t n)
{
	uint8_t i;

	for (i = 0u; i < (uint8_t)sizeof(compat_windows); i++) {
		if (compat_windows[i] == n) {
			return (int)i;
		}
	}
	return -EINVAL;
}

bool profile_compat_is_compat_page(uint8_t byte0)
{
	uint8_t page = (uint8_t)(byte0 & PROFILE_COMPAT_PAGE_MASK);

	return page == PROFILE_COMPAT_PAGE_BEACON ||
	       page == PROFILE_COMPAT_PAGE_ATTEST_I ||
	       page == PROFILE_COMPAT_PAGE_ATTEST_II;
}

static bool toggle_now(const struct profile_compat *pc)
{
	if (pc->cfg.toggle == NULL) {
		return false;
	}
	return pc->cfg.toggle(pc->cfg.user);
}

/*
 * The two beacon frames.
 *
 *   frame 0  [1] = (0 << 4) | (count - 1)
 *     [2]    bits 7:4 version, bit 3 pairing-available, bit 2 pairing-open,
 *            bit 1 private-available, bit 0 attest-available
 *     [3]    bits 1:0 policy, bits 3:2 window N, bit 4 mode,
 *            bit 5 pending switch, bits 7:6 reserved
 *     [4..6] key-group hint, trunc24( CMAC(K_id, epoch) )
 *     [7]    reserved, must be 0
 *
 *   frame 1  [1] = (1 << 4) | (count - 1)
 *     [2]    private-mode target device type (bits 6:0)
 *     [3..4] private-mode target device number, LE
 *     [5..6] private-mode target channel period, LE
 *     [7]    reserved, must be 0
 *
 * No epoch field, deliberately - see docs/decisions/0008, "The epoch is not
 * broadcast".
 */
static int encode_beacon(struct profile_compat *pc,
			 const uint8_t hint[PROFILE_COMPAT_HINT_BYTES])
{
	const struct profile_compat_cfg *cfg = &pc->cfg;
	uint8_t *f0 = pc->frames[PROFILE_COMPAT_FRAME_BEACON_0];
	uint8_t *f1 = pc->frames[PROFILE_COMPAT_FRAME_BEACON_1];
	bool     private_available;
	int      code;

	code = window_code(cfg->window);
	if (code < 0) {
		return code;
	}
	if (cfg->mode != PROFILE_COMPAT_MODE_FIXED) {
		/* Not implemented in v1; refused rather than emitted so a
		 * receiver never waits for pages that will never come. */
		return -ENOTSUP;
	}
	if (cfg->policy > PROFILE_COMPAT_POLICY_ALWAYS) {
		return -EINVAL;
	}
	if ((cfg->target_device_type & PROFILE_COMPAT_PAGE_TOGGLE) != 0u) {
		/* Device type is 7 bits here; bit 7 is the ANT channel id's
		 * pairing bit and not ours to set. */
		return -EINVAL;
	}

	private_available = cfg->policy != PROFILE_COMPAT_POLICY_NEVER;

	/* A `never` node has no locator to carry; refuse to build one rather
	 * than leave a receiver guessing which half of the beacon to trust. */
	if (!private_available &&
	    (cfg->target_device_type != 0u || cfg->target_device_number != 0u ||
	     cfg->target_period != 0u)) {
		return -EINVAL;
	}

	f0[0] = PROFILE_COMPAT_PAGE_BEACON;
	f0[1] = (uint8_t)((PROFILE_COMPAT_FRAME_BEACON_0 << 4) |
			  (PROFILE_COMPAT_BEACON_FRAMES - 1u));
	f0[2] = (uint8_t)((PROFILE_COMPAT_VERSION << 4) |
			  (cfg->pairing_available
				   ? PROFILE_COMPAT_CAP_PAIRING_AVAILABLE : 0u) |
			  (cfg->pairing_open
				   ? PROFILE_COMPAT_CAP_PAIRING_OPEN : 0u) |
			  (private_available
				   ? PROFILE_COMPAT_CAP_PRIVATE_AVAILABLE : 0u) |
			  (cfg->attest_available
				   ? PROFILE_COMPAT_CAP_ATTEST_AVAILABLE : 0u));
	f0[3] = (uint8_t)(cfg->policy | ((uint8_t)code << 2) |
			  (uint8_t)(cfg->mode << 4) |
			  (pc->pending_switch ? PROFILE_COMPAT_PENDING_SWITCH
					      : 0u));
	f0[4] = hint[0];
	f0[5] = hint[1];
	f0[6] = hint[2];
	f0[7] = 0u; /* reserved */

	f1[0] = PROFILE_COMPAT_PAGE_BEACON;
	f1[1] = (uint8_t)((PROFILE_COMPAT_FRAME_BEACON_1 << 4) |
			  (PROFILE_COMPAT_BEACON_FRAMES - 1u));
	f1[2] = cfg->target_device_type;
	f1[3] = (uint8_t)(cfg->target_device_number & 0xFFu);
	f1[4] = (uint8_t)((cfg->target_device_number >> 8) & 0xFFu);
	f1[5] = (uint8_t)(cfg->target_period & 0xFFu);
	f1[6] = (uint8_t)((cfg->target_period >> 8) & 0xFFu);
	f1[7] = 0u; /* reserved */

	pc->n_frames = PROFILE_COMPAT_BEACON_FRAMES;
	return 0;
}

int profile_compat_init(struct profile_compat *pc,
			const struct profile_compat_cfg *cfg)
{
	uint8_t hint[PROFILE_COMPAT_HINT_BYTES];
	int     rc;

	if (pc == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(pc, 0, sizeof(*pc));
	pc->cfg = *cfg;

	/* The hint is the one thing needing a key; asked from the layer that
	 * holds it. No compat attestation -> ENOTSUP -> node just runs as an
	 * ordinary ANT+ sensor, not a degraded mode. */
	rc = radiant_sec_compat_hint(cfg->ch, cfg->epoch, hint);
	if (rc != RADIANT_SEC_OK) {
		return -ENOTSUP;
	}

	memcpy(pc->hint, hint, sizeof(pc->hint));

	if (cfg->advertise) {
		rc = encode_beacon(pc, hint);
		if (rc != 0) {
			return rc;
		}
	}

	pc->armed = true;
	return 0;
}

int profile_compat_set_client(struct profile_compat *pc,
			      const struct profile_compat_client *client)
{
	uint8_t i;

	if (pc == NULL) {
		return -EINVAL;
	}
	if (client == NULL) {
		memset(pc->clients, 0, sizeof(pc->clients));
		pc->n_clients = 0u;
		pc->active_client = 0u;
		pc->frame_cursor = 0u;
		return 0;
	}
	if (client->frames == NULL || client->frame == NULL) {
		return -EINVAL;
	}

	/* Keyed by `user`, so a module that attaches twice replaces itself
	 * rather than spending the other module's slot. */
	for (i = 0u; i < pc->n_clients; i++) {
		if (pc->clients[i].user == client->user) {
			pc->clients[i] = *client;
			return 0;
		}
	}
	if (pc->n_clients >= (uint8_t)PROFILE_COMPAT_CLIENTS_MAX) {
		return -ENOSPC;
	}
	pc->clients[pc->n_clients++] = *client;
	/* Set is about to change size; restart the rotation at frame 0. */
	pc->frame_cursor = 0u;
	return 0;
}

bool profile_compat_client_busy(struct profile_compat *pc, uint64_t now_us,
				const void *except_user)
{
	uint8_t i;

	if (pc == NULL) {
		return false;
	}
	for (i = 0u; i < pc->n_clients; i++) {
		if (pc->clients[i].user == except_user) {
			continue;
		}
		if (pc->clients[i].frames(pc->clients[i].user, now_us) != 0u) {
			return true;
		}
	}
	return false;
}

int profile_compat_set_pending_switch(struct profile_compat *pc, bool pending)
{
	if (pc == NULL) {
		return -EINVAL;
	}
	if (pc->pending_switch != pending) {
		/* Restart at the announcement, not frame 0: a short countdown
		 * could otherwise expire before reaching it. Legal because
		 * byte [1] carries the frame's index, so send order doesn't
		 * matter to a receiver reassembling by index. Ending a
		 * countdown restarts at frame 0. */
		pc->frame_cursor = pending ? pc->n_frames : 0u;
	}
	pc->pending_switch = pending;
	if (pc->n_frames == 0u) {
		return 0;   /* advertise = off: no beacon, no capability field */
	}
	return encode_beacon(pc, pc->hint);
}

int profile_compat_set_locator(struct profile_compat *pc, uint8_t device_type,
			       uint16_t device_number, uint16_t period)
{
	struct profile_compat_cfg was;
	int                       rc;

	if (pc == NULL) {
		return -EINVAL;
	}

	was = pc->cfg;
	pc->cfg.target_device_type = device_type;
	pc->cfg.target_device_number = device_number;
	pc->cfg.target_period = period;
	if (pc->n_frames == 0u) {
		return 0;
	}
	/* encode_beacon() is the sole place deciding whether a locator may
	 * exist; a refusal here puts the configuration back rather than
	 * leaving a beacon half-changed. */
	rc = encode_beacon(pc, pc->hint);
	if (rc != 0) {
		pc->cfg = was;
		(void)encode_beacon(pc, pc->hint);
	}
	return rc;
}

int profile_compat_set_pairing_open(struct profile_compat *pc, bool open)
{
	if (pc == NULL) {
		return -EINVAL;
	}
	if (pc->cfg.pairing_open != open) {
		/* Set is about to change size; frame 0 carries the capability
		 * bit and new count, so restart the rotation there to get the
		 * changed frame out first rather than up to seven frames late. */
		pc->frame_cursor = 0u;
	}
	pc->cfg.pairing_open = open;
	if (pc->n_frames == 0u) {
		/* advertise = off: no beacon, no capability field. Not an
		 * error - see the header. */
		return 0;
	}
	return encode_beacon(pc, pc->hint);
}

/* Frames the client wants in this set, clamped so that byte [1]'s count nibble
 * can express the total. A client asking for more than the format holds is a
 * bug in the client, and taking none of its frames makes it a visible one. */
static uint8_t client_frames(struct profile_compat *pc, uint64_t now_us)
{
	uint8_t i;
	uint8_t n;

	pc->active_client = 0u;

	/* Interlock's backstop: the first client contributing frames owns
	 * the slot; if the request-time exclusion is ever broken, one
	 * client's block goes out and the other waits, rather than a set
	 * too large for the count nibble or any receiver to hold. */
	for (i = 0u; i < pc->n_clients; i++) {
		n = pc->clients[i].frames(pc->clients[i].user, now_us);
		if (n == 0u) {
			continue;
		}
		if ((uint16_t)n + (uint16_t)pc->n_frames >
		    (uint16_t)PROFILE_COMPAT_SET_FRAMES_MAX) {
			/* A client asking for more than the format holds is a
			 * bug in the client, and taking none of its frames
			 * makes it a visible one. */
			continue;
		}
		pc->active_client = (uint8_t)(i + 1u);
		return n;
	}
	return 0u;
}

/*
 * The seam. Priority order:
 *   1. ask the attestation layer whether this slot owes a page (Tier II
 *      first when both fall due - its window closes at the Nth transmitted
 *      message with no slack, while Tier I's counter is time-derived).
 *   2. otherwise, the beacon on its own slot.
 *   3. otherwise decline; the rotation has the slot back at no cost.
 *
 * Asking the attestation layer first, even on the beacon's slot, matters
 * because radiant_sec_compat_tx_attest() is stateful (records a served Tier I
 * interval / closing Tier II window) - asking and discarding a positive
 * answer would corrupt both. The cost is the beacon occasionally slipping a
 * cycle, which just costs a receiver one frame of a set reassembled across
 * cycles anyway.
 */
static bool compat_claim(uint32_t m, uint8_t *body, void *user)
{
	struct profile_compat *pc = (struct profile_compat *)user;
	uint8_t                n_client;
	uint8_t                total;
	uint8_t                idx;
	int                    sub;

	if (pc == NULL || !pc->armed) {
		return false;
	}

	/* Asked every offered slot, not just the beacon's: this is also where
	 * a client with a bounded window discovers it has closed. */
	n_client = client_frames(pc, pc->now_us);
	total = (uint8_t)(pc->n_frames + n_client);

	/* The beacon's slot comes due here, not necessarily served here.
	 * Message 0 is the slot right after the two the seam never offers,
	 * so a Tier I page slipping from 119/120 lands there too; owing the
	 * frame instead of dropping it keeps cadence at one frame per cycle.
	 *
	 * With a client active the cadence promotes to one frame every eight
	 * messages (PROFILE_COMPAT_BEACON_PROMOTED_EVERY) - an eight-frame
	 * set needs four minutes at the steady rate, too slow for a 60 s
	 * enrolment window. Message 0 satisfies both rules, so steady-state
	 * is a special case of the promoted cadence. */
	if (total != 0u) {
		if (n_client != 0u) {
			if ((m % PROFILE_COMPAT_BEACON_PROMOTED_EVERY) == 0u) {
				pc->beacon_due = true;
			}
		} else if (m == PROFILE_COMPAT_BEACON_SLOT) {
			pc->beacon_due = true;
		}
	}

	/* Bytes [1..7]; byte [0] is the page number, which is the one byte the
	 * attestation layer will not write because it knows no page numbers. */
	sub = radiant_sec_compat_tx_attest(pc->cfg.ch, pc->now_us, &body[1],
					   PROFILE_COMPAT_FRAME_LEN - 1u);
	if (sub > 0) {
		body[0] = (uint8_t)(PROFILE_COMPAT_PAGE_BASE | (uint8_t)sub);
		if (toggle_now(pc)) {
			body[0] |= PROFILE_COMPAT_PAGE_TOGGLE;
		}
		if (sub == RADIANT_SEC_COMPAT_SUBTYPE_TIER_II) {
			pc->tier2_sent++;
		} else {
			pc->tier1_sent++;
		}
		return true;
	}

	if (!pc->beacon_due || total == 0u) {
		return false;
	}

	/* The set changed size under the rotation - a window opened or closed.
	 * Start it again rather than emitting an index the count cannot hold. */
	if (pc->frame_cursor >= total) {
		pc->frame_cursor = 0u;
	}
	idx = pc->frame_cursor;

	if (idx < pc->n_frames) {
		memcpy(body, pc->frames[idx], PROFILE_COMPAT_FRAME_LEN);
	} else {
		const struct profile_compat_client *cl;

		if (pc->active_client == 0u) {
			return false;
		}
		cl = &pc->clients[pc->active_client - 1u];
		memset(body, 0, PROFILE_COMPAT_FRAME_LEN);
		body[0] = PROFILE_COMPAT_PAGE_BEACON;
		if (!cl->frame(cl->user, (uint8_t)(idx - pc->n_frames),
			       &body[2])) {
			/* Client changed its mind between being counted and
			 * asked. Decline; the cursor has not moved. */
			return false;
		}
	}

	/* Byte [1] written here and nowhere else, overriding what
	 * encode_beacon() baked into frames 0/1: the count must match the
	 * whole set's size in EVERY frame, or a receiver hearing only
	 * frames 0/1 would think no window was open (see docs/radiant-
	 * security.md section 11.5). */
	body[1] = (uint8_t)((idx << 4) | (uint8_t)(total - 1u));

	if (toggle_now(pc)) {
		body[0] |= PROFILE_COMPAT_PAGE_TOGGLE;
	}

	pc->beacon_due = false;
	pc->frame_cursor = (uint8_t)((idx + 1u) % total);
	/* Two counters: the steady-state claim and a client's bounded burst
	 * would make one number unreadable together. */
	if (idx < pc->n_frames) {
		pc->beacons_sent++;
	} else {
		pc->client_sent++;
	}
	return true;
}

int profile_compat_attach(struct profile_compat *pc, struct profile_sched *ps)
{
	struct profile_sched_client client;

	if (pc == NULL || ps == NULL) {
		return -EINVAL;
	}
	if (!pc->armed) {
		return -ENOTSUP;
	}

	client.claim = compat_claim;
	client.user = pc;
	return profile_sched_set_client(ps, &client);
}

void profile_compat_before(struct profile_compat *pc, uint64_t now_us)
{
	if (pc != NULL) {
		pc->now_us = now_us;
	}
}

void profile_compat_sent(struct profile_compat *pc, const uint8_t *body)
{
	uint8_t i;

	if (pc == NULL || body == NULL || !pc->armed) {
		return;
	}
	(void)radiant_sec_compat_tx_sent(pc->cfg.ch, body,
					 PROFILE_COMPAT_FRAME_LEN);

	/* Every client, every message, whoever built it - same rule this
	 * function already owes the attestation layer above. */
	for (i = 0u; i < pc->n_clients; i++) {
		if (pc->clients[i].sent != NULL) {
			pc->clients[i].sent(pc->clients[i].user, body);
		}
	}
}

enum profile_slot_kind profile_compat_next(struct profile_compat *pc,
					   struct profile_sched *ps,
					   uint64_t now_us, uint8_t *body)
{
	enum profile_slot_kind kind;

	profile_compat_before(pc, now_us);
	kind = profile_sched_next(ps, body);
	if (kind != PROFILE_SLOT_IDLE) {
		profile_compat_sent(pc, body);
	}
	return kind;
}
