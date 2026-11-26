/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radar_adapter.c - see radiant_radar_adapter.h.
 */

#include <string.h>

#include <radiant/radiant_radar_adapter.h>

static void post(uint32_t source, uint8_t field_id, uint8_t field_type,
		 int8_t exp, int64_t raw, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = field_id,
		.field_type = field_type,
		.flags = 0u,
		.exp = exp,
		.raw = raw,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
}

static void forget_values(struct radiant_radar_adapter *a)
{
	a->last_count = -1;
	a->last_threat = -1;
	a->last_range_code = -1;
	a->last_speed_code = -1;
	a->last_side = -1;
}

void radiant_radar_adapter_init(struct radiant_radar_adapter *a)
{
	if (a == NULL) {
		return;
	}
	memset(a, 0, sizeof(*a));
	forget_values(a);
	a->last_state = -1;
	a->last_error = -1;
}

/* Post AVAILABLE if it changed (or `force`). */
static uint32_t post_available(struct radiant_radar_adapter *a, uint32_t source,
			       bool available, bool force, uint64_t t_us)
{
	if (!force && a->posted_available && a->available == available) {
		return 0u;
	}
	a->available = available;
	a->posted_available = true;
	post(source, RADIANT_RADAR_FIELD_AVAILABLE, RADIANT_FIELD_BOOLEAN_STATE, 0,
	     available ? 1 : 0, t_us);
	return 1u;
}

/* Target information became unavailable: clear what a sink is showing. */
static uint32_t go_unavailable(struct radiant_radar_adapter *a, uint32_t source,
			       uint64_t t_us)
{
	memset(a->targets, 0, sizeof(a->targets));
	a->have_targets = false;
	forget_values(a);
	return post_available(a, source, false, false, t_us);
}

static uint32_t post_if(int32_t *last, int32_t value, bool force, uint32_t source,
			uint8_t field_id, uint8_t field_type, int8_t exp, int64_t raw,
			uint64_t t_us)
{
	if (!force && *last == value) {
		return 0u;
	}
	*last = value;
	post(source, field_id, field_type, exp, raw, t_us);
	return 1u;
}

static uint32_t on_targets(struct radiant_radar_adapter *a, uint32_t source,
			   const uint8_t *body8, uint64_t t_us)
{
	struct radiant_profile_radar_target got[4];
	uint8_t page;
	uint32_t n = 0u;
	unsigned int base;
	int32_t count = 0, threat = 0;
	int nearest = -1;
	bool force;

	if (radiant_profile_radar_decode_targets(body8, &page, got) != 0) {
		return 0u;
	}
	base = (page == RADIANT_PROFILE_RADAR_PAGE_TARGETS_B) ? 4u : 0u;
	memcpy(&a->targets[base], got, sizeof(got));
	/* Page 48 alone means at most four targets: a stale page-49 half from
	 * an earlier >4 episode must not linger. A radar reporting >4
	 * alternates 48/49 every message, so 49's half is refreshed within
	 * one period when it is real. */
	if (page == RADIANT_PROFILE_RADAR_PAGE_TARGETS_A &&
	    t_us - a->last_target_us > 300000u) {
		memset(&a->targets[4], 0, 4u * sizeof(a->targets[0]));
	}
	a->last_target_us = t_us;
	a->have_targets = true;

	force = (++a->pages_since_repost >= RADIANT_RADAR_REPOST);
	if (force) {
		a->pages_since_repost = 0u;
	}
	n += post_available(a, source, true, force, t_us);

	for (unsigned int i = 0u; i < RADIANT_PROFILE_RADAR_MAX_TARGETS; i++) {
		const struct radiant_profile_radar_target *t = &a->targets[i];

		/* Display rule 6.5.1: no further decode of a threat-0 target. */
		if (!t->valid) {
			continue;
		}
		count++;
		if (t->threat > threat) {
			threat = t->threat;
		}
		if (nearest < 0 || t->range_code < a->targets[nearest].range_code) {
			nearest = (int)i;
		}
	}

	n += post_if(&a->last_count, count, force, source,
		     RADIANT_RADAR_FIELD_TARGET_COUNT, RADIANT_FIELD_ENUM_GENERIC, 0,
		     count, t_us);
	n += post_if(&a->last_threat, threat, force, source,
		     RADIANT_RADAR_FIELD_THREAT, RADIANT_FIELD_ENUM_GENERIC, 0,
		     threat, t_us);
	if (nearest >= 0) {
		const struct radiant_profile_radar_target *t = &a->targets[nearest];

		n += post_if(&a->last_range_code, t->range_code, force, source,
			     RADIANT_RADAR_FIELD_NEAREST_RANGE, RADIANT_FIELD_LENGTH,
			     -3, (int64_t)RADIANT_PROFILE_RADAR_RANGE_MM(t->range_code),
			     t_us);
		n += post_if(&a->last_speed_code, t->speed_code, force, source,
			     RADIANT_RADAR_FIELD_NEAREST_SPEED, RADIANT_FIELD_SPEED,
			     -3, (int64_t)RADIANT_PROFILE_RADAR_SPEED_MM_S(t->speed_code),
			     t_us);
		n += post_if(&a->last_side, t->side, force, source,
			     RADIANT_RADAR_FIELD_NEAREST_SIDE, RADIANT_FIELD_ENUM_GENERIC,
			     0, t->side, t_us);
	} else {
		/* No nearest target: forget, so the next one posts. */
		a->last_range_code = -1;
		a->last_speed_code = -1;
		a->last_side = -1;
	}
	return n;
}

uint32_t radiant_radar_adapter_decode(struct radiant_radar_adapter *a, uint32_t source,
				      const uint8_t body8[8], uint64_t t_us)
{
	struct radiant_profile_radar_status st;
	struct radiant_profile_radar_error err;
	uint32_t n = 0u;

	if (a == NULL || body8 == NULL) {
		return 0u;
	}
	switch (body8[0]) {
	case RADIANT_PROFILE_RADAR_PAGE_TARGETS_A:
	case RADIANT_PROFILE_RADAR_PAGE_TARGETS_B:
		return on_targets(a, source, body8, t_us);
	case RADIANT_PROFILE_RADAR_PAGE_STATUS:
		if (radiant_profile_radar_decode_status(body8, &st) != 0) {
			return 0u;
		}
		n += post_if(&a->last_state, st.state, false, source,
			     RADIANT_RADAR_FIELD_DEVICE_STATE,
			     RADIANT_FIELD_ENUM_GENERIC, 0, st.state, t_us);
		if (st.clear_targets ||
		    st.state == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_REQUESTED ||
		    st.state == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_FORCED) {
			n += go_unavailable(a, source, t_us);
		}
		return n;
	case RADIANT_PROFILE_RADAR_PAGE_ERROR:
		/* 6.4.2: clear and show unavailable "regardless of page content
		 * beyond the page number" - so the decode result only adds the
		 * code, it does not gate the clearing. */
		n += go_unavailable(a, source, t_us);
		if (radiant_profile_radar_decode_error(body8, &err) == 0) {
			n += post_if(&a->last_error, err.profile_code, false, source,
				     RADIANT_RADAR_FIELD_ERROR_CODE,
				     RADIANT_FIELD_ENUM_GENERIC, 0, err.profile_code,
				     t_us);
		}
		return n;
	default:
		return 0u;
	}
}

uint32_t radiant_radar_adapter_tick(struct radiant_radar_adapter *a, uint32_t source,
				    uint64_t now_us)
{
	if (a == NULL) {
		return 0u;
	}
	if (!a->posted_available) {
		/* From connection until the first target page: unavailable. */
		return post_available(a, source, false, true, now_us);
	}
	if (a->have_targets && now_us - a->last_target_us >= RADIANT_RADAR_TIMEOUT_US) {
		return go_unavailable(a, source, now_us);
	}
	return 0u;
}
