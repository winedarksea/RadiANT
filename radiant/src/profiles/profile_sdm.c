/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_sdm.c - ANT+ Stride Based Speed and Distance Monitor, device type
 * 0x7C. See profile_sdm.h for the provenance, the four traps and the
 * interleave.
 *
 * No cryptography in this file and no call to radiant_sec. No radiant header
 * and no bridge header, direct or transitive.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_common.h"
#include "profile_sched.h"
#include "profile_sdm.h"

/* Nothing in this file writes an all-ones sentinel. Trap 2 in the header: on
 * this profile an unused field is zero and validity lives in page 22. */
#define SDM_RESERVED 0x00u

uint8_t profile_sdm_status(uint8_t location, uint8_t battery, uint8_t health,
			   uint8_t use_state)
{
	return (uint8_t)(((location & 0x03u) << 6) |
			 ((battery & 0x03u) << 4) |
			 ((health & 0x03u) << 2) |
			 (use_state & 0x03u));
}

void profile_sdm_status_split(uint8_t status, uint8_t *location,
			      uint8_t *battery, uint8_t *health,
			      uint8_t *use_state)
{
	if (location != NULL) {
		*location = (uint8_t)((status >> 6) & 0x03u);
	}
	if (battery != NULL) {
		*battery = (uint8_t)((status >> 4) & 0x03u);
	}
	if (health != NULL) {
		*health = (uint8_t)((status >> 2) & 0x03u);
	}
	if (use_state != NULL) {
		*use_state = (uint8_t)(status & 0x03u);
	}
}

int profile_sdm_speed_split(uint32_t mm_s, uint8_t *int_mps, uint8_t *frac_256)
{
	uint32_t frac;

	if (int_mps == NULL || frac_256 == NULL) {
		return -EINVAL;
	}
	if (mm_s > PROFILE_SDM_SPEED_MAX_MM_S) {
		/* The integer part is a NIBBLE. Wrapping it would report 16 m/s
		 * as 0 m/s, which is a stopped treadmill on a receiver's
		 * screen. */
		return -EINVAL;
	}

	*int_mps = (uint8_t)(mm_s / 1000u);
	/* (mm_s % 1000) of a metre, in 1/256ths. The multiply is done first
	 * and in 32 bits: 999 * 256 is 255744, so nothing overflows and
	 * nothing is lost to an early divide. */
	frac = ((mm_s % 1000u) * PROFILE_SDM_SPEED_FRAC_DEN) / 1000u;
	*frac_256 = (uint8_t)frac;
	return 0;
}

uint32_t profile_sdm_speed_mm_s(uint8_t int_mps, uint8_t frac_256)
{
	return ((uint32_t)(int_mps & 0x0Fu) * 1000u) +
	       (((uint32_t)frac_256 * 1000u) / PROFILE_SDM_SPEED_FRAC_DEN);
}

/* ── Page 1 (0x01) Default Data ─────────────────────────────────────────── */

int profile_sdm_encode_default(const struct profile_sdm_default *in,
			       uint8_t *out)
{
	if (in == NULL || out == NULL) {
		return -EINVAL;
	}
	if (in->distance_frac_16 > 0x0Fu || in->speed_int_mps > 0x0Fu) {
		/* Both live in nibbles of byte [4]; a value that does not fit
		 * would corrupt the other one rather than itself. */
		return -EINVAL;
	}

	out[0] = PROFILE_SDM_PAGE_DEFAULT;
	out[1] = in->time_frac_200;
	out[2] = in->time_s;
	out[3] = in->distance_m;
	out[4] = (uint8_t)((in->distance_frac_16 << 4) | in->speed_int_mps);
	out[5] = in->speed_frac_256;
	out[6] = in->strides;
	out[7] = in->latency_32;
	return 0;
}

int profile_sdm_decode_default(const uint8_t *body,
			       struct profile_sdm_default *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_SDM_PAGE_DEFAULT) {
		return -EINVAL;
	}

	out->time_frac_200 = body[1];
	out->time_s = body[2];
	out->distance_m = body[3];
	out->distance_frac_16 = (uint8_t)((body[4] >> 4) & 0x0Fu);
	out->speed_int_mps = (uint8_t)(body[4] & 0x0Fu);
	out->speed_frac_256 = body[5];
	out->strides = body[6];
	out->latency_32 = body[7];
	return 0;
}

/* ── Pages 2 and 3, one template ────────────────────────────────────────── */

int profile_sdm_encode_supplementary(uint8_t page,
				     const struct profile_sdm_supplementary *in,
				     uint8_t *out)
{
	if (in == NULL || out == NULL) {
		return -EINVAL;
	}
	if (page != PROFILE_SDM_PAGE_BASE && page != PROFILE_SDM_PAGE_CALORIES) {
		return -EINVAL;
	}
	if (in->cadence_frac_16 > 0x0Fu || in->speed_int_mps > 0x0Fu) {
		return -EINVAL;
	}

	out[0] = page;
	out[1] = SDM_RESERVED;
	out[2] = SDM_RESERVED;
	out[3] = in->cadence_strides_min;
	out[4] = (uint8_t)((in->cadence_frac_16 << 4) | in->speed_int_mps);
	out[5] = in->speed_frac_256;
	/* The one byte that differs between the two pages. */
	out[6] = (page == PROFILE_SDM_PAGE_CALORIES) ? in->calories
						     : SDM_RESERVED;
	out[7] = in->status;
	return 0;
}

int profile_sdm_decode_supplementary(const uint8_t *body,
				     struct profile_sdm_supplementary *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_SDM_PAGE_BASE &&
	    body[0] != PROFILE_SDM_PAGE_CALORIES) {
		return -EINVAL;
	}

	out->cadence_strides_min = body[3];
	out->cadence_frac_16 = (uint8_t)((body[4] >> 4) & 0x0Fu);
	out->speed_int_mps = (uint8_t)(body[4] & 0x0Fu);
	out->speed_frac_256 = body[5];
	out->calories = (body[0] == PROFILE_SDM_PAGE_CALORIES) ? body[6] : 0u;
	out->status = body[7];
	return 0;
}

/* ── Page 16 decimal (0x10) Summary ─────────────────────────────────────── */

int profile_sdm_encode_summary(const struct profile_sdm_summary *in,
			       uint8_t *out)
{
	if (in == NULL || out == NULL) {
		return -EINVAL;
	}
	if (in->strides > 0xFFFFFFu) {
		return -EINVAL; /* 24 bits, and truncating would restart a total */
	}

	out[0] = PROFILE_SDM_PAGE_SUMMARY;
	out[1] = (uint8_t)(in->strides & 0xFFu);
	out[2] = (uint8_t)((in->strides >> 8) & 0xFFu);
	out[3] = (uint8_t)((in->strides >> 16) & 0xFFu);
	out[4] = (uint8_t)(in->distance_256 & 0xFFu);
	out[5] = (uint8_t)((in->distance_256 >> 8) & 0xFFu);
	out[6] = (uint8_t)((in->distance_256 >> 16) & 0xFFu);
	out[7] = (uint8_t)((in->distance_256 >> 24) & 0xFFu);
	return 0;
}

int profile_sdm_decode_summary(const uint8_t *body,
			       struct profile_sdm_summary *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_SDM_PAGE_SUMMARY) {
		return -EINVAL;
	}

	out->strides = (uint32_t)body[1] | ((uint32_t)body[2] << 8) |
		       ((uint32_t)body[3] << 16);
	out->distance_256 = (uint32_t)body[4] | ((uint32_t)body[5] << 8) |
			    ((uint32_t)body[6] << 16) |
			    ((uint32_t)body[7] << 24);
	return 0;
}

/* ── Page 22 decimal (0x16) Capabilities ────────────────────────────────── */

int profile_sdm_encode_capabilities(const struct profile_sdm_capabilities *in,
				    uint8_t *out)
{
	if (in == NULL || out == NULL) {
		return -EINVAL;
	}

	out[0] = PROFILE_SDM_PAGE_CAPABILITIES;
	out[1] = in->flags;
	memset(&out[2], SDM_RESERVED, 6);
	return 0;
}

int profile_sdm_decode_capabilities(const uint8_t *body,
				    struct profile_sdm_capabilities *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_SDM_PAGE_CAPABILITIES) {
		return -EINVAL;
	}

	out->flags = body[1];
	return 0;
}

/* ---------------------------------------------------------------------------
 * The master
 * ---------------------------------------------------------------------------
 */

static int sdm_common_80(struct profile_sdm *sdm, uint8_t *body)
{
	return profile_common_80(&sdm->cfg.id, body);
}

static int sdm_common_81(struct profile_sdm *sdm, uint8_t *body)
{
	return profile_common_81(&sdm->cfg.id, body);
}

/* Messages 64 and 65 of the 66-message cycle carry the pair. Expressed against
 * `messages % PROFILE_SDM_CYCLE` rather than against the group counter,
 * because the pair is two messages inside a four-message group grid and does
 * not line up with it. */
static uint32_t sdm_cycle_pos(const struct profile_sdm *sdm)
{
	return sdm->messages % PROFILE_SDM_CYCLE;
}

static bool sdm_claim(uint32_t m, uint8_t *body, void *user)
{
	struct profile_sdm *sdm = (struct profile_sdm *)user;
	uint32_t            pos;

	(void)m; /* the engine's index; this profile keeps its own cycle */

	if (sdm == NULL) {
		return false;
	}

	pos = sdm_cycle_pos(sdm);
	if (pos == PROFILE_SDM_DATA_MESSAGES) {
		return sdm_common_80(sdm, body) == 0;
	}
	if (pos == PROFILE_SDM_DATA_MESSAGES + 1u) {
		return sdm_common_81(sdm, body) == 0;
	}
	return false;
}

static int sdm_encode_page(struct profile_sdm *sdm, uint8_t page, uint8_t *body)
{
	switch (page) {
	case PROFILE_SDM_PAGE_DEFAULT:
		return profile_sdm_encode_default(&sdm->page1, body);
	case PROFILE_SDM_PAGE_BASE:
	case PROFILE_SDM_PAGE_CALORIES:
		return profile_sdm_encode_supplementary(page, &sdm->supp, body);
	case PROFILE_SDM_PAGE_SUMMARY:
		return profile_sdm_encode_summary(&sdm->summary, body);
	case PROFILE_SDM_PAGE_CAPABILITIES:
		return profile_sdm_encode_capabilities(&sdm->cfg.caps, body);
	case PROFILE_COMMON_PAGE_80:
		return sdm_common_80(sdm, body);
	case PROFILE_COMMON_PAGE_81:
		return sdm_common_81(sdm, body);
	default:
		return -EINVAL;
	}
}

/*
 * One data slot: 1, 1, X, X.
 *
 * `page` from the engine's rotation is ignored for the reason profile_hr.c and
 * profile_fec_tx.c ignore it - the rotation is a shape, not a page list.
 *
 * An on-request page takes the SECOND X slot only (group_slot 3). The first X
 * is left alone so that whichever of pages 2 and 3 this group carries still
 * goes out at least once, and the page 1 pair is never displaced: page 1
 * carries the stride and distance accumulators, which are the fields a
 * receiver differences, and dropping one costs a sample rather than a page.
 */
static int sdm_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			 void *user)
{
	struct profile_sdm *sdm = (struct profile_sdm *)user;

	(void)page;
	(void)counter;

	if (sdm == NULL) {
		return -EINVAL;
	}

	if (sdm->group_slot == 3u && sdm->on_request_left != 0u) {
		int rc = sdm_encode_page(sdm, sdm->on_request_page, body);

		if (rc == 0) {
			sdm->on_request_left--;
		}
		return rc;
	}

	if (sdm->group_slot < 2u) {
		return sdm_encode_page(sdm, PROFILE_SDM_PAGE_DEFAULT, body);
	}

	return sdm_encode_page(sdm,
			       sdm->group_is_calories
				       ? PROFILE_SDM_PAGE_CALORIES
				       : PROFILE_SDM_PAGE_BASE,
			       body);
}

int profile_sdm_init(struct profile_sdm *sdm, const struct profile_sdm_cfg *cfg)
{
	struct profile_sched_cfg    sched_cfg;
	struct profile_sched_client client;
	int                         rc;

	if (sdm == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(sdm, 0, sizeof(*sdm));
	sdm->cfg = *cfg;
	sdm->rotation[0] = PROFILE_SDM_PAGE_DEFAULT;
	sdm->supp.status = profile_sdm_status(cfg->location, cfg->battery,
					      cfg->health,
					      PROFILE_SDM_USE_INACTIVE);

	memset(&sched_cfg, 0, sizeof(sched_cfg));
	sched_cfg.desc = NULL;
	sched_cfg.pages = sdm->rotation;
	sched_cfg.n_pages = (uint8_t)sizeof(sdm->rotation);
	sched_cfg.data_page = sdm_data_page;
	/* NULL: 66 is not 121 - see the header's interleave block. */
	sched_cfg.common_80 = NULL;
	sched_cfg.common_81 = NULL;
	sched_cfg.common_82 = NULL;
	sched_cfg.user = sdm;

	rc = profile_sched_init(&sdm->sched, &sched_cfg);
	if (rc != 0) {
		return rc;
	}

	client.claim = sdm_claim;
	client.user = sdm;
	return profile_sched_set_client(&sdm->sched, &client);
}

void profile_sdm_set_motion(struct profile_sdm *sdm, uint32_t speed_mm_s,
			    uint8_t cadence_strides_min,
			    uint8_t cadence_frac_16, bool active)
{
	uint8_t int_mps;
	uint8_t frac_256;

	if (sdm == NULL) {
		return;
	}

	if (profile_sdm_speed_split(speed_mm_s, &int_mps, &frac_256) != 0) {
		/* Clamp rather than refuse, for the reason
		 * profile_fec_tx_set_settings() clamps: a setter that leaves
		 * the previous value in place on a fast belt reports a slow
		 * one forever, and nothing anywhere says why. */
		int_mps = 0x0Fu;
		frac_256 = 0xFFu;
	}

	sdm->page1.speed_int_mps = int_mps;
	sdm->page1.speed_frac_256 = frac_256;
	sdm->supp.speed_int_mps = int_mps;
	sdm->supp.speed_frac_256 = frac_256;

	sdm->supp.cadence_strides_min = cadence_strides_min;
	sdm->supp.cadence_frac_16 = (uint8_t)(cadence_frac_16 & 0x0Fu);
	sdm->supp.status = profile_sdm_status(sdm->cfg.location,
					      sdm->cfg.battery, sdm->cfg.health,
					      active ? PROFILE_SDM_USE_ACTIVE
						     : PROFILE_SDM_USE_INACTIVE);
}

void profile_sdm_set_accumulators(struct profile_sdm *sdm, uint32_t elapsed_ms,
				  uint64_t distance_mm, uint32_t strides,
				  uint8_t latency_32)
{
	uint64_t distance_256;

	if (sdm == NULL) {
		return;
	}

	/* Page 1's time is seconds plus 1/200 s, and both are taken from the
	 * SAME millisecond value so they cannot disagree by a tick. */
	sdm->page1.time_s = (uint8_t)((elapsed_ms / 1000u) & 0xFFu);
	sdm->page1.time_frac_200 =
		(uint8_t)(((elapsed_ms % 1000u) * PROFILE_SDM_TIME_FRAC_DEN) /
			  1000u);

	sdm->page1.distance_m = (uint8_t)((distance_mm / 1000u) & 0xFFu);
	sdm->page1.distance_frac_16 =
		(uint8_t)(((distance_mm % 1000u) * PROFILE_SDM_DIST_FRAC_DEN) /
			  1000u);

	sdm->page1.strides = (uint8_t)(strides & 0xFFu);
	sdm->page1.latency_32 = latency_32;

	/* The summary page's wider totals, from the same two inputs, so the
	 * roll-over accumulators above and the session totals below can never
	 * describe different runs. */
	distance_256 = (distance_mm * PROFILE_SDM_SPEED_FRAC_DEN) / 1000u;
	sdm->summary.strides = strides & 0xFFFFFFu;
	sdm->summary.distance_256 = (uint32_t)(distance_256 & 0xFFFFFFFFu);
}

void profile_sdm_set_calories(struct profile_sdm *sdm, uint8_t kcal)
{
	if (sdm != NULL) {
		sdm->supp.calories = kcal;
	}
}

int profile_sdm_request(struct profile_sdm *sdm, uint8_t page, uint8_t count)
{
	if (sdm == NULL) {
		return -EINVAL;
	}

	switch (page) {
	case PROFILE_SDM_PAGE_SUMMARY:
	case PROFILE_SDM_PAGE_CAPABILITIES:
	case PROFILE_COMMON_PAGE_80:
	case PROFILE_COMMON_PAGE_81:
		break;
	default:
		return -ENOTSUP;
	}

	sdm->on_request_page = page;
	sdm->on_request_left = (count == 0u) ? 1u : count;
	return 0;
}

enum profile_slot_kind profile_sdm_next(struct profile_sdm *sdm, uint8_t *body)
{
	enum profile_slot_kind kind;

	if (sdm == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}

	kind = profile_sched_next(&sdm->sched, body);

	if (kind == PROFILE_SLOT_CLIENT) {
		kind = (body[0] == PROFILE_COMMON_PAGE_80)
			       ? PROFILE_SLOT_COMMON_80
			       : PROFILE_SLOT_COMMON_81;
	}

	if (kind != PROFILE_SLOT_IDLE) {
		sdm->messages++;
		sdm->group_slot++;
		if (sdm->group_slot >= PROFILE_SDM_GROUP_SLOTS) {
			sdm->group_slot = 0u;
			sdm->group++;
			/* The X pages alternate group by group, so pages 2 and
			 * 3 each get eight of the sixteen groups in a cycle. */
			sdm->group_is_calories = !sdm->group_is_calories;
		}

		/* The common pair is two messages long and the data grid is
		 * four, so the pair would otherwise leave the group phase two
		 * slots out for the rest of time. Realigning at the top of the
		 * cycle keeps "1, 1, X, X" true of every group. */
		if (sdm_cycle_pos(sdm) == 0u) {
			sdm->group_slot = 0u;
		}
	}

	return kind;
}

uint32_t profile_sdm_messages(const struct profile_sdm *sdm)
{
	return (sdm == NULL) ? 0u : sdm->messages;
}
