/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_tracker.c - see profile_tracker.h.
 *
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_common.h"
#include "profile_sched.h"
#include "profile_tracker.h"

/* ── Unit conversions ───────────────────────────────────────────────────── */

int32_t profile_tracker_degrees_to_semicircles(double degrees)
{
	return (int32_t)(degrees * (PROFILE_TRACKER_SEMICIRCLES_PER_180DEG /
				    180.0));
}

double profile_tracker_semicircles_to_degrees(int32_t semicircles)
{
	return (double)semicircles * (180.0 /
				      PROFILE_TRACKER_SEMICIRCLES_PER_180DEG);
}

uint8_t profile_tracker_degrees_to_bradians(double degrees)
{
	/* Rounded, not truncated: the document's own worked example rounds
	 * 42.67 up to 43, and at this field's coarse ~1.4-degree resolution
	 * truncation would bias every conversion downward. */
	double scaled = degrees * (PROFILE_TRACKER_BRADIANS_PER_360DEG / 360.0);

	return (uint8_t)((int32_t)(scaled + (scaled >= 0.0 ? 0.5 : -0.5)) &
			 0xFF);
}

double profile_tracker_bradians_to_degrees(uint8_t bradians)
{
	return (double)bradians * (360.0 / PROFILE_TRACKER_BRADIANS_PER_360DEG);
}

/* ── Page 0x01 / 0x02: location ─────────────────────────────────────────── */

int profile_tracker_encode_location_1(uint8_t asset_index,
				      const struct profile_tracker_location *loc,
				      uint8_t *out)
{
	uint16_t lat_lo;

	if (loc == NULL || out == NULL ||
	    asset_index >= PROFILE_TRACKER_MAX_ASSETS) {
		return -EINVAL;
	}

	lat_lo = (uint16_t)((uint32_t)loc->latitude_semi & 0xFFFFu);

	out[0] = PROFILE_TRACKER_PAGE_LOCATION_1;
	out[1] = (uint8_t)((asset_index & PROFILE_TRACKER_ASSET_INDEX_MASK) |
			   0xE0u); /* reserved top 3 bits = 0x7 */
	out[2] = (uint8_t)(loc->distance_m & 0xFFu);
	out[3] = (uint8_t)((loc->distance_m >> 8) & 0xFFu);
	out[4] = loc->bearing_brad;
	out[5] = loc->status;
	out[6] = (uint8_t)(lat_lo & 0xFFu);
	out[7] = (uint8_t)((lat_lo >> 8) & 0xFFu);
	return 0;
}

int profile_tracker_decode_location_1(const uint8_t *body, uint8_t *asset_index,
				      struct profile_tracker_location *loc)
{
	uint16_t lat_lo;

	if (body == NULL || loc == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TRACKER_PAGE_LOCATION_1) {
		return -EINVAL;
	}

	if (asset_index != NULL) {
		*asset_index = body[1] & PROFILE_TRACKER_ASSET_INDEX_MASK;
	}
	loc->distance_m = (uint16_t)(body[2] | ((uint16_t)body[3] << 8));
	loc->bearing_brad = body[4];
	loc->status = body[5];
	lat_lo = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	/* Only the low half is known until page 2 arrives; store zero-
	 * extended rather than sign-extended, since the sign lives in bit 31,
	 * which page 2 alone carries. */
	loc->latitude_semi = (int32_t)(uint32_t)lat_lo;
	loc->connected = true;
	return 0;
}

int profile_tracker_encode_location_2(uint8_t asset_index,
				      const struct profile_tracker_location *loc,
				      uint8_t *out)
{
	uint16_t lat_hi;

	if (loc == NULL || out == NULL ||
	    asset_index >= PROFILE_TRACKER_MAX_ASSETS) {
		return -EINVAL;
	}

	lat_hi = (uint16_t)(((uint32_t)loc->latitude_semi >> 16) & 0xFFFFu);

	out[0] = PROFILE_TRACKER_PAGE_LOCATION_2;
	out[1] = (uint8_t)((asset_index & PROFILE_TRACKER_ASSET_INDEX_MASK) |
			   0xE0u);
	out[2] = (uint8_t)(lat_hi & 0xFFu);
	out[3] = (uint8_t)((lat_hi >> 8) & 0xFFu);
	out[4] = (uint8_t)((uint32_t)loc->longitude_semi & 0xFFu);
	out[5] = (uint8_t)(((uint32_t)loc->longitude_semi >> 8) & 0xFFu);
	out[6] = (uint8_t)(((uint32_t)loc->longitude_semi >> 16) & 0xFFu);
	out[7] = (uint8_t)(((uint32_t)loc->longitude_semi >> 24) & 0xFFu);
	return 0;
}

int profile_tracker_decode_location_2(const uint8_t *body, uint8_t *asset_index,
				      struct profile_tracker_location *loc)
{
	uint16_t lat_hi;
	uint32_t lat_low16;

	if (body == NULL || loc == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TRACKER_PAGE_LOCATION_2) {
		return -EINVAL;
	}

	if (asset_index != NULL) {
		*asset_index = body[1] & PROFILE_TRACKER_ASSET_INDEX_MASK;
	}
	lat_hi = (uint16_t)(body[2] | ((uint16_t)body[3] << 8));
	/* OR the high 16 bits onto whatever page 1 already stored in the low
	 * 16. Calling this first still produces a correct value for the high
	 * half alone, with the low half at zero. */
	lat_low16 = (uint32_t)loc->latitude_semi & 0xFFFFu;
	loc->latitude_semi = (int32_t)(lat_low16 | ((uint32_t)lat_hi << 16));
	loc->longitude_semi = (int32_t)((uint32_t)body[4] |
					((uint32_t)body[5] << 8) |
					((uint32_t)body[6] << 16) |
					((uint32_t)body[7] << 24));
	loc->connected = true;
	return 0;
}

/* ── Page 0x03: no assets ───────────────────────────────────────────────── */

int profile_tracker_encode_no_assets(uint8_t *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	out[0] = PROFILE_TRACKER_PAGE_NO_ASSETS;
	memset(&out[1], PROFILE_COMMON_INVALID_U8, 7);
	return 0;
}

/* ── Page 0x10 / 0x11: identity ─────────────────────────────────────────── */

int profile_tracker_encode_ident_1(uint8_t asset_index,
				   const struct profile_tracker_ident *id,
				   uint8_t *out)
{
	if (id == NULL || out == NULL ||
	    asset_index >= PROFILE_TRACKER_MAX_ASSETS) {
		return -EINVAL;
	}

	out[0] = PROFILE_TRACKER_PAGE_IDENT_1;
	out[1] = (uint8_t)((asset_index & PROFILE_TRACKER_ASSET_INDEX_MASK) |
			   0xE0u);
	out[2] = id->colour;
	/* First 5 characters, null-padded for a shorter name, applied
	 * identically on both halves. */
	memcpy(&out[3], id->name, 5);
	return 0;
}

int profile_tracker_decode_ident_1(const uint8_t *body, uint8_t *asset_index,
				   struct profile_tracker_ident *id)
{
	if (body == NULL || id == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TRACKER_PAGE_IDENT_1) {
		return -EINVAL;
	}

	if (asset_index != NULL) {
		*asset_index = body[1] & PROFILE_TRACKER_ASSET_INDEX_MASK;
	}
	id->colour = body[2];
	memcpy(id->name, &body[3], 5);
	return 0;
}

int profile_tracker_encode_ident_2(uint8_t asset_index,
				   const struct profile_tracker_ident *id,
				   uint8_t *out)
{
	if (id == NULL || out == NULL ||
	    asset_index >= PROFILE_TRACKER_MAX_ASSETS) {
		return -EINVAL;
	}

	out[0] = PROFILE_TRACKER_PAGE_IDENT_2;
	out[1] = (uint8_t)((asset_index & PROFILE_TRACKER_ASSET_INDEX_MASK) |
			   0xE0u);
	out[2] = id->asset_type;
	memcpy(&out[3], &id->name[5], 5);
	return 0;
}

int profile_tracker_decode_ident_2(const uint8_t *body, uint8_t *asset_index,
				   struct profile_tracker_ident *id)
{
	if (body == NULL || id == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TRACKER_PAGE_IDENT_2) {
		return -EINVAL;
	}

	if (asset_index != NULL) {
		*asset_index = body[1] & PROFILE_TRACKER_ASSET_INDEX_MASK;
	}
	id->asset_type = body[2];
	memcpy(&id->name[5], &body[3], 5);
	return 0;
}

/* ── Page 0x20: disconnect ──────────────────────────────────────────────── */

int profile_tracker_encode_disconnect(uint8_t *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	out[0] = PROFILE_TRACKER_PAGE_DISCONNECT;
	memset(&out[1], PROFILE_COMMON_INVALID_U8, 7);
	return 0;
}

/* ── The master ─────────────────────────────────────────────────────────── */

static bool any_connected(const struct profile_tracker *t)
{
	uint8_t i;

	for (i = 0; i < PROFILE_TRACKER_MAX_ASSETS; i++) {
		if (t->location[i].connected) {
			return true;
		}
	}
	return false;
}

static uint8_t next_connected(const struct profile_tracker *t, uint8_t from)
{
	uint8_t i;

	for (i = 0; i < PROFILE_TRACKER_MAX_ASSETS; i++) {
		uint8_t idx = (uint8_t)((from + i) % PROFILE_TRACKER_MAX_ASSETS);

		if (t->location[idx].connected) {
			return idx;
		}
	}
	return from;
}

static int tracker_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			     void *user)
{
	struct profile_tracker *t = (struct profile_tracker *)user;
	uint8_t                 idx;
	int                      rc;

	/* No envelope-style event counter; every accumulator lives inside
	 * the location pages. */
	(void)page;
	(void)counter;

	if (t == NULL) {
		return -EINVAL;
	}

	if (t->disconnect_left > 0u) {
		t->disconnect_left--;
		return profile_tracker_encode_disconnect(body);
	}

	if (!any_connected(t)) {
		return profile_tracker_encode_no_assets(body);
	}

	idx = next_connected(t, t->next_asset);

	if ((t->ident_pending & (1u << idx)) != 0u) {
		if (t->page_phase == 0u) {
			rc = profile_tracker_encode_ident_1(idx, &t->ident[idx],
							    body);
			t->page_phase = 1u;
			return rc;
		}
		rc = profile_tracker_encode_ident_2(idx, &t->ident[idx], body);
		t->page_phase = 0u;
		t->ident_pending &= ~(1u << idx);
		return rc;
	}

	if (t->page_phase == 0u) {
		rc = profile_tracker_encode_location_1(idx, &t->location[idx],
						       body);
		t->page_phase = 1u;
		return rc;
	}
	rc = profile_tracker_encode_location_2(idx, &t->location[idx], body);
	t->page_phase = 0u;
	t->next_asset = (uint8_t)((idx + 1u) % PROFILE_TRACKER_MAX_ASSETS);
	return rc;
}

static int tracker_common_80(uint8_t *body, void *user)
{
	struct profile_tracker *t = (struct profile_tracker *)user;

	if (t == NULL) {
		return -EINVAL;
	}
	return profile_common_80(&t->cfg.id, body);
}

static int tracker_common_81(uint8_t *body, void *user)
{
	struct profile_tracker *t = (struct profile_tracker *)user;

	if (t == NULL) {
		return -EINVAL;
	}
	return profile_common_81(&t->cfg.id, body);
}

static int tracker_common_82(uint8_t *body, void *user)
{
	struct profile_tracker *t = (struct profile_tracker *)user;

	if (t == NULL) {
		return -EINVAL;
	}
	return profile_common_82(&t->cfg.battery, body);
}

int profile_tracker_init(struct profile_tracker *t,
			 const struct profile_tracker_cfg *cfg)
{
	struct profile_sched_cfg sched_cfg;

	if (t == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(t, 0, sizeof(*t));
	t->cfg = *cfg;
	t->rotation[0] = PROFILE_TRACKER_PAGE_LOCATION_1;

	memset(&sched_cfg, 0, sizeof(sched_cfg));
	sched_cfg.desc = NULL;
	sched_cfg.pages = t->rotation;
	sched_cfg.n_pages = (uint8_t)sizeof(t->rotation);
	sched_cfg.data_page = tracker_data_page;
	sched_cfg.common_80 = tracker_common_80;
	sched_cfg.common_81 = tracker_common_81;
	sched_cfg.common_82 = (cfg->common_82_every != 0u) ? tracker_common_82
							    : NULL;
	sched_cfg.common_82_every = cfg->common_82_every;
	sched_cfg.user = t;

	return profile_sched_init(&t->sched, &sched_cfg);
}

int profile_tracker_connect(struct profile_tracker *t, uint8_t asset_index,
			    const struct profile_tracker_location *loc,
			    const struct profile_tracker_ident *id)
{
	if (t == NULL || loc == NULL || id == NULL ||
	    asset_index >= PROFILE_TRACKER_MAX_ASSETS) {
		return -EINVAL;
	}
	t->location[asset_index] = *loc;
	t->location[asset_index].connected = true;
	t->ident[asset_index] = *id;
	t->ident_pending |= (1u << asset_index);
	return 0;
}

int profile_tracker_update_location(struct profile_tracker *t,
				    uint8_t asset_index,
				    const struct profile_tracker_location *loc)
{
	if (t == NULL || loc == NULL ||
	    asset_index >= PROFILE_TRACKER_MAX_ASSETS) {
		return -EINVAL;
	}
	if (!t->location[asset_index].connected) {
		return -ENOENT;
	}
	t->location[asset_index] = *loc;
	t->location[asset_index].connected = true;
	return 0;
}

void profile_tracker_disconnect_asset(struct profile_tracker *t,
				      uint8_t asset_index)
{
	if (t == NULL || asset_index >= PROFILE_TRACKER_MAX_ASSETS) {
		return;
	}
	t->location[asset_index].connected = false;
	t->ident_pending &= ~(1u << asset_index);
	if (t->next_asset == asset_index) {
		t->next_asset = (uint8_t)((asset_index + 1u) %
					  PROFILE_TRACKER_MAX_ASSETS);
		t->page_phase = 0u;
	}
}

int profile_tracker_request_ident(struct profile_tracker *t,
				  uint8_t asset_index)
{
	if (t == NULL || asset_index >= PROFILE_TRACKER_MAX_ASSETS) {
		return -EINVAL;
	}
	if (!t->location[asset_index].connected) {
		return -ENOENT;
	}
	t->ident_pending |= (1u << asset_index);
	return 0;
}

void profile_tracker_begin_disconnect(struct profile_tracker *t)
{
	if (t != NULL) {
		t->disconnect_left = 5u;
	}
}

enum profile_slot_kind profile_tracker_next(struct profile_tracker *t,
					    uint8_t *body)
{
	if (t == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}
	return profile_sched_next(&t->sched, body);
}
