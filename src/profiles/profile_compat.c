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
 * and code; no adopter-gated ANT+ device profile document was read for this
 * file and nothing here derives from libant.a.
 *
 * This is the ONLY file in src/profiles/ that calls radiant_sec_compat, and the
 * header says at length why. The short version: radiant_sec_compat.c knows no
 * page number, profile_hr.c and profile_power.c know no key, and this file is
 * the one place where a subtype nibble becomes a page byte.
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
 * THERE IS NO EPOCH FIELD, and its absence is a constraint rather than an
 * omission. See docs/decisions/0008, "The epoch is not broadcast".
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
		/* Specified so it is a later configuration change rather than a
		 * break, and deliberately not implemented in v1. Refused here
		 * rather than emitted, because a receiver that believed it
		 * would wait for pages this node will never send. */
		return -ENOTSUP;
	}
	if (cfg->policy > PROFILE_COMPAT_POLICY_ALWAYS) {
		return -EINVAL;
	}
	if ((cfg->target_device_type & PROFILE_COMPAT_PAGE_TOGGLE) != 0u) {
		/* A device type is 7 bits in this field; bit 7 is the pairing
		 * bit of an ANT channel id and is not ours to set. */
		return -EINVAL;
	}

	private_available = cfg->policy != PROFILE_COMPAT_POLICY_NEVER;

	/*
	 * A `never` node has no private-mode locator to carry, and a receiver
	 * that saw one would have to decide which half of the beacon to
	 * believe. Refuse to build it instead.
	 */
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
			  (uint8_t)(cfg->mode << 4));
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

	/*
	 * The hint is the one thing this layer needs a key for, and it asks the
	 * layer that holds one rather than holding it itself. A build with no
	 * compat attestation answers ENOTSUP here, and the node then simply
	 * runs as an ordinary ANT+ sensor - which is the setting most straps
	 * should ship in, not a degraded mode.
	 */
	rc = radiant_sec_compat_hint(cfg->ch, cfg->epoch, hint);
	if (rc != RADIANT_SEC_OK) {
		return -ENOTSUP;
	}

	if (cfg->advertise) {
		rc = encode_beacon(pc, hint);
		if (rc != 0) {
			return rc;
		}
	}

	pc->armed = true;
	return 0;
}

/*
 * The seam.
 *
 * The order is a priority and every step of it is load-bearing:
 *
 *   1. ask the attestation layer whether this slot owes a page. It answers
 *      Tier II first when both fall due, because Tier II's window closes at the
 *      Nth transmitted message and has no slack, while Tier I's counter is
 *      derived from elapsed time and a slot of slip is invisible to it.
 *   2. otherwise, the beacon on its own slot.
 *   3. otherwise decline, and the rotation has the slot back at no cost.
 *
 * Asking the attestation layer FIRST, even on the beacon's slot, is the subtle
 * one. radiant_sec_compat_tx_attest() is stateful - it records that a Tier I
 * interval has been served and that a Tier II window is closing - so a caller
 * that asked and then discarded a positive answer would corrupt both. Asking
 * only when we intend to use the answer means the beacon occasionally slips a
 * cycle, which costs a receiver one frame of a set it reassembles across cycles
 * anyway.
 */
static bool compat_claim(uint32_t m, uint8_t *body, void *user)
{
	struct profile_compat *pc = (struct profile_compat *)user;
	int                    sub;

	if (pc == NULL || !pc->armed) {
		return false;
	}

	/*
	 * The beacon's slot comes DUE here and is not necessarily SERVED here.
	 *
	 * Message 0 is the slot immediately after the two the seam can never
	 * offer, so it is exactly where a Tier I page that came due at message
	 * 119 or 120 lands after slipping - and a beacon that simply lost its
	 * slot would then miss a whole cycle every time that happened. Owing
	 * the frame instead costs one slot of delay and keeps the cadence at
	 * one frame per cycle, which is the number the 0.8% claim is made of.
	 */
	if (pc->n_frames != 0u && m == PROFILE_COMPAT_BEACON_SLOT) {
		pc->beacon_due = true;
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

	if (pc->beacon_due) {
		pc->beacon_due = false;
		memcpy(body, pc->frames[pc->frame_cursor],
		       PROFILE_COMPAT_FRAME_LEN);
		if (toggle_now(pc)) {
			body[0] |= PROFILE_COMPAT_PAGE_TOGGLE;
		}
		pc->frame_cursor = (uint8_t)((pc->frame_cursor + 1u) %
					     pc->n_frames);
		pc->beacons_sent++;
		return true;
	}

	return false;
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
	if (pc == NULL || body == NULL || !pc->armed) {
		return;
	}
	(void)radiant_sec_compat_tx_sent(pc->cfg.ch, body,
					 PROFILE_COMPAT_FRAME_LEN);
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
