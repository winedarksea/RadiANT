/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_profile_radar.c - see radiant_profile_radar.h.
 *
 * Provenance: ANT+ Bike Radar Rev 2.1 sections 6.3-6.11 for every page and
 * pattern; on-air captures of three Garmin units for the cadences the spec
 * leaves open (archive/captures/radio/2026-11-25-varia-*). Clean-room; no
 * sdk-ant source consulted.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant/radiant_profile_common.h>
#include <radiant/radiant_profile_radar.h>

/* ── Unit conversion ────────────────────────────────────────────────────── */

uint8_t radiant_profile_radar_range_code(uint32_t range_mm)
{
	uint32_t code = (range_mm + RADIANT_PROFILE_RADAR_RANGE_STEP_MM / 2u) /
			RADIANT_PROFILE_RADAR_RANGE_STEP_MM;

	return (uint8_t)(code > RADIANT_PROFILE_RADAR_RANGE_CODE_MAX
				 ? RADIANT_PROFILE_RADAR_RANGE_CODE_MAX
				 : code);
}

uint8_t radiant_profile_radar_speed_code(uint32_t speed_mm_s)
{
	uint32_t code = (speed_mm_s + RADIANT_PROFILE_RADAR_SPEED_STEP_MM_S / 2u) /
			RADIANT_PROFILE_RADAR_SPEED_STEP_MM_S;

	return (uint8_t)(code > RADIANT_PROFILE_RADAR_SPEED_CODE_MAX
				 ? RADIANT_PROFILE_RADAR_SPEED_CODE_MAX
				 : code);
}

/* ── Pages 48 / 49: targets ─────────────────────────────────────────────── */

int radiant_profile_radar_encode_targets(uint8_t page,
					 const struct radiant_profile_radar_target t[4],
					 uint8_t *out)
{
	uint32_t range = 0u;
	uint8_t threat = 0u, side = 0u, speed_lo = 0u, speed_hi = 0u;

	if (t == NULL || out == NULL ||
	    (page != RADIANT_PROFILE_RADAR_PAGE_TARGETS_A &&
	     page != RADIANT_PROFILE_RADAR_PAGE_TARGETS_B)) {
		return -EINVAL;
	}

	for (unsigned int i = 0u; i < RADIANT_PROFILE_RADAR_TARGETS_PER_PAGE; i++) {
		const uint8_t th = t[i].threat & 0x03u;
		/* Spec 6.5.1-6.5.4: no threat means side, range and speed 0. */
		const bool live = (th != RADIANT_PROFILE_RADAR_THREAT_NONE);
		const uint8_t sd = live ? (t[i].side & 0x03u) : 0u;
		const uint8_t rg = live ? (t[i].range_code & 0x3Fu) : 0u;
		const uint8_t sp = live ? (t[i].speed_code & 0x0Fu) : 0u;

		threat |= (uint8_t)(th << (2u * i));
		side |= (uint8_t)(sd << (2u * i));
		range |= (uint32_t)rg << (6u * i);
		if (i < 2u) {
			speed_lo |= (uint8_t)(sp << (4u * i));
		} else {
			speed_hi |= (uint8_t)(sp << (4u * (i - 2u)));
		}
	}

	out[0] = page;
	out[1] = threat;
	out[2] = side;
	out[3] = (uint8_t)(range & 0xFFu);
	out[4] = (uint8_t)((range >> 8) & 0xFFu);
	out[5] = (uint8_t)((range >> 16) & 0xFFu);
	out[6] = speed_lo;
	out[7] = speed_hi;
	return 0;
}

int radiant_profile_radar_decode_targets(const uint8_t *body, uint8_t *page,
					 struct radiant_profile_radar_target out[4])
{
	uint32_t range;

	if (body == NULL || out == NULL ||
	    (body[0] != RADIANT_PROFILE_RADAR_PAGE_TARGETS_A &&
	     body[0] != RADIANT_PROFILE_RADAR_PAGE_TARGETS_B)) {
		return -EINVAL;
	}
	if (page != NULL) {
		*page = body[0];
	}

	range = (uint32_t)body[3] | ((uint32_t)body[4] << 8) |
		((uint32_t)body[5] << 16);
	for (unsigned int i = 0u; i < RADIANT_PROFILE_RADAR_TARGETS_PER_PAGE; i++) {
		const uint8_t speed_byte = body[6u + i / 2u];

		out[i].threat = (uint8_t)((body[1] >> (2u * i)) & 0x03u);
		out[i].side = (uint8_t)((body[2] >> (2u * i)) & 0x03u);
		out[i].range_code = (uint8_t)((range >> (6u * i)) & 0x3Fu);
		out[i].speed_code = (uint8_t)((speed_byte >> (4u * (i % 2u))) & 0x0Fu);
		/* Threat 3 is reserved; a display shows it as a target (it is
		 * not "no threat") but cannot know more. */
		out[i].valid = (out[i].threat != RADIANT_PROFILE_RADAR_THREAT_NONE);
	}
	return 0;
}

/* ── Page 1: device status ──────────────────────────────────────────────── */

int radiant_profile_radar_encode_status(const struct radiant_profile_radar_status *s,
					uint8_t *out)
{
	if (s == NULL || out == NULL) {
		return -EINVAL;
	}
	out[0] = RADIANT_PROFILE_RADAR_PAGE_STATUS;
	out[1] = (uint8_t)((s->state & 0x03u) |
			   (s->legacy_reserved ? 0x00u : (0x3Fu << 2)));
	out[2] = 0xFFu;
	out[3] = 0xFFu;
	out[4] = 0xFFu;
	out[5] = 0xFFu;
	out[6] = 0xFFu;
	/* Bit 0 is INVERTED: 0 = clear targets. Bits 1-7 reserved 0x7F. */
	out[7] = (uint8_t)((0x7Fu << 1) | (s->clear_targets ? 0u : 1u));
	return 0;
}

int radiant_profile_radar_decode_status(const uint8_t *body,
					struct radiant_profile_radar_status *s)
{
	if (body == NULL || s == NULL ||
	    body[0] != RADIANT_PROFILE_RADAR_PAGE_STATUS) {
		return -EINVAL;
	}
	s->state = body[1] & 0x03u;
	s->clear_targets = (body[7] & 0x01u) == 0u;
	s->legacy_reserved = (body[1] & 0xFCu) == 0u;
	return 0;
}

/* ── Page 2: device command ─────────────────────────────────────────────── */

int radiant_profile_radar_encode_command(uint8_t command, uint8_t *out)
{
	if (out == NULL || command > RADIANT_PROFILE_RADAR_CMD_SHUTDOWN) {
		return -EINVAL;
	}
	out[0] = RADIANT_PROFILE_RADAR_PAGE_COMMAND;
	out[1] = (uint8_t)((RADIANT_PROFILE_RADAR_CMD_RESERVED << 2) | command);
	memset(&out[2], 0xFF, 6);
	return 0;
}

int radiant_profile_radar_decode_command(const uint8_t *body, uint8_t *command)
{
	if (body == NULL || command == NULL ||
	    body[0] != RADIANT_PROFILE_RADAR_PAGE_COMMAND) {
		return -EINVAL;
	}
	*command = body[1] & 0x03u;
	return 0;
}

/* ── Common page 87: error description ──────────────────────────────────── */

int radiant_profile_radar_encode_error(const struct radiant_profile_radar_error *e,
				       uint8_t *out)
{
	if (e == NULL || out == NULL) {
		return -EINVAL;
	}
	out[0] = RADIANT_PROFILE_RADAR_PAGE_ERROR;
	out[1] = 0xFFu;
	/* Bits 4-5 reserved 0b00. */
	out[2] = (uint8_t)((e->component & 0x0Fu) | ((e->level & 0x03u) << 6));
	out[3] = e->profile_code;
	out[4] = (uint8_t)(e->mfg_code & 0xFFu);
	out[5] = (uint8_t)((e->mfg_code >> 8) & 0xFFu);
	out[6] = (uint8_t)((e->mfg_code >> 16) & 0xFFu);
	out[7] = (uint8_t)((e->mfg_code >> 24) & 0xFFu);
	return 0;
}

int radiant_profile_radar_decode_error(const uint8_t *body,
				       struct radiant_profile_radar_error *e)
{
	if (body == NULL || e == NULL ||
	    body[0] != RADIANT_PROFILE_RADAR_PAGE_ERROR) {
		return -EINVAL;
	}
	e->component = body[2] & 0x0Fu;
	e->level = (uint8_t)((body[2] >> 6) & 0x03u);
	e->profile_code = body[3];
	e->mfg_code = (uint32_t)body[4] | ((uint32_t)body[5] << 8) |
		      ((uint32_t)body[6] << 16) | ((uint32_t)body[7] << 24);
	return 0;
}

/* ── Common page 70: request ────────────────────────────────────────────── */

int radiant_profile_radar_decode_request(const uint8_t *body, uint8_t *page,
					 uint8_t *count, bool *acked)
{
	if (body == NULL || page == NULL || count == NULL || acked == NULL ||
	    body[0] != RADIANT_PROFILE_RADAR_PAGE_REQUEST || body[7] != 0x01u) {
		return -EINVAL;
	}
	*page = body[6];
	*count = body[5] & 0x7Fu;
	if (*count == 0u) {
		*count = 1u;
	}
	*acked = (body[5] & 0x80u) != 0u;
	return 0;
}

int radiant_profile_radar_encode_request(uint8_t page, uint8_t count, uint8_t *out)
{
	if (out == NULL || count == 0u || count > 0x7Fu) {
		return -EINVAL;
	}
	/* Descriptor bytes invalid (no subpages); broadcast replies only
	 * (spec 6.11.1.2: a display shall request broadcast). */
	out[0] = RADIANT_PROFILE_RADAR_PAGE_REQUEST;
	out[1] = 0xFFu;
	out[2] = 0xFFu;
	out[3] = 0xFFu;
	out[4] = 0xFFu;
	out[5] = count;
	out[6] = page;
	out[7] = 0x01u;
	return 0;
}

/* ── The master ─────────────────────────────────────────────────────────── */

int radiant_profile_radar_init(struct radiant_profile_radar *r,
			       const struct radiant_profile_radar_cfg *cfg)
{
	if (r == NULL || cfg == NULL) {
		return -EINVAL;
	}
	memset(r, 0, sizeof(*r));
	r->cfg = *cfg;
	if (r->cfg.background_every == 0u) {
		r->cfg.background_every = RADIANT_PROFILE_RADAR_DEFAULT_BACKGROUND_EVERY;
	}
	if (r->cfg.shutdown_msgs == 0u) {
		r->cfg.shutdown_msgs = RADIANT_PROFILE_RADAR_DEFAULT_SHUTDOWN_MSGS;
	}
	if (r->cfg.aborted_msgs == 0u) {
		r->cfg.aborted_msgs = RADIANT_PROFILE_RADAR_DEFAULT_ABORTED_MSGS;
	}
	if (r->cfg.status_every == 0u) {
		r->cfg.status_every = RADIANT_PROFILE_RADAR_DEFAULT_STATUS_EVERY;
	}
	if (r->cfg.status_every < 2u) {
		r->cfg.status_every = 2u; /* 4/8 is the spec's ceiling */
	}
	if (r->cfg.status_every > 8u) {
		r->cfg.status_every = 8u; /* 1/8 is its floor */
	}
	if (r->cfg.stay_on_delay_msgs == 0u) {
		r->cfg.stay_on_delay_msgs = RADIANT_PROFILE_RADAR_DEFAULT_STAY_ON_DELAY;
	}
	r->detecting = true;
	r->state = RADIANT_PROFILE_RADAR_STATE_BROADCASTING;
	return 0;
}

static uint8_t count_targets(const struct radiant_profile_radar *r)
{
	uint8_t n = 0u;

	for (unsigned int i = 0u; i < RADIANT_PROFILE_RADAR_MAX_TARGETS; i++) {
		n += (r->targets[i].threat != RADIANT_PROFILE_RADAR_THREAT_NONE) ? 1u : 0u;
	}
	return n;
}

int radiant_profile_radar_set_targets(struct radiant_profile_radar *r,
				      const struct radiant_profile_radar_target *t,
				      uint8_t n)
{
	uint8_t k = 0u;

	if (r == NULL || (n > 0u && t == NULL) ||
	    n > RADIANT_PROFILE_RADAR_MAX_TARGETS) {
		return -EINVAL;
	}
	memset(r->targets, 0, sizeof(r->targets));
	for (uint8_t i = 0u; i < n; i++) {
		if ((t[i].threat & 0x03u) == RADIANT_PROFILE_RADAR_THREAT_NONE) {
			continue;
		}
		r->targets[k] = t[i];
		r->targets[k].valid = true;
		k++;
	}
	return 0;
}

void radiant_profile_radar_set_error(struct radiant_profile_radar *r,
				     const struct radiant_profile_radar_error *e)
{
	if (r == NULL) {
		return;
	}
	r->error_active = (e != NULL);
	if (e != NULL) {
		r->error = *e;
	}
}

void radiant_profile_radar_set_detecting(struct radiant_profile_radar *r, bool on)
{
	if (r != NULL) {
		r->detecting = on;
	}
}

void radiant_profile_radar_begin_shutdown(struct radiant_profile_radar *r, bool forced)
{
	if (r == NULL || r->off) {
		return;
	}
	r->state = forced ? RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_FORCED
			  : RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_REQUESTED;
	r->state_left = r->cfg.shutdown_msgs;
	r->pattern_m = 0u;
	r->stay_on_wait = 0u;
}

static void begin_aborted(struct radiant_profile_radar *r)
{
	r->state = RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_ABORTED;
	r->state_left = r->cfg.aborted_msgs;
	r->pattern_m = 0u;
	r->stay_on_wait = 0u;
}

int radiant_profile_radar_rx(struct radiant_profile_radar *r, const uint8_t *body)
{
	uint8_t command, page, count;
	bool acked;

	if (r == NULL || body == NULL) {
		return -EINVAL;
	}
	if (radiant_profile_radar_decode_command(body, &command) == 0) {
		if (command == RADIANT_PROFILE_RADAR_CMD_SHUTDOWN) {
			if (r->state == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_REQUESTED ||
			    r->state == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_FORCED) {
				return 0; /* already going */
			}
			if (r->cfg.power_off_on_command) {
				radiant_profile_radar_begin_shutdown(r, false);
			} else if (r->state == RADIANT_PROFILE_RADAR_STATE_BROADCASTING &&
				   r->stay_on_wait == 0u) {
				/* Rev 2.1: a radar that will not power down
				 * says so with the aborted pattern (6.3.3) -
				 * after ~1.5 s, as every Garmin unit does. A
				 * repeat while already aborting is absorbed. */
				r->stay_on_wait = r->cfg.stay_on_delay_msgs;
			}
			return 0;
		}
		if (command == RADIANT_PROFILE_RADAR_CMD_ABORT_SHUTDOWN) {
			/* Only Shutdown Requested can be aborted (6.3.4);
			 * Forced cannot, and in any other state there is
			 * nothing to abort. */
			if (r->state == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_REQUESTED) {
				begin_aborted(r);
			}
			return 0;
		}
		return -ENOTSUP; /* reserved command values */
	}
	if (radiant_profile_radar_decode_request(body, &page, &count, &acked) == 0) {
		/* Answered as broadcasts whatever bit 7 says: section 7 item 1
		 * forbids a radar acknowledged messages to a display. Only
		 * the supported background pages are served. */
		(void)acked;
		if (page == RADIANT_PROFILE_COMMON_PAGE_80 ||
		    page == RADIANT_PROFILE_COMMON_PAGE_81 ||
		    page == RADIANT_PROFILE_COMMON_PAGE_82 ||
		    page == RADIANT_PROFILE_RADAR_PAGE_STATUS) {
			r->req_page = page;
			/* decode_request() turns count 0 into 1; the units
			 * answer it with 5 copies, so the master does too. */
			r->req_left = ((body[5] & 0x7Fu) == 0u)
					      ? RADIANT_PROFILE_RADAR_REQ_COUNT0_COPIES
					      : count;
			return 0;
		}
		return -ENOTSUP;
	}
	return -ENOTSUP;
}

static enum radiant_profile_radar_slot fill_status(struct radiant_profile_radar *r,
						   uint8_t *body, bool clear)
{
	struct radiant_profile_radar_status s = {
		.state = r->state,
		.clear_targets = clear,
		.legacy_reserved = r->cfg.legacy_status,
	};

	(void)radiant_profile_radar_encode_status(&s, body);
	return RADIANT_PROFILE_RADAR_SLOT_STATUS;
}

static enum radiant_profile_radar_slot fill_common(struct radiant_profile_radar *r,
						   uint8_t page, uint8_t *body)
{
	if (page == RADIANT_PROFILE_COMMON_PAGE_80) {
		(void)radiant_profile_common_80(&r->cfg.id, body);
		return RADIANT_PROFILE_RADAR_SLOT_COMMON_80;
	}
	if (page == RADIANT_PROFILE_COMMON_PAGE_81) {
		(void)radiant_profile_common_81(&r->cfg.id, body);
		return RADIANT_PROFILE_RADAR_SLOT_COMMON_81;
	}
	(void)radiant_profile_common_82(&r->cfg.battery, body);
	return RADIANT_PROFILE_RADAR_SLOT_COMMON_82;
}

static enum radiant_profile_radar_slot fill_targets(struct radiant_profile_radar *r,
						    bool page_b, uint8_t *body)
{
	const unsigned int base = page_b ? 4u : 0u;

	(void)radiant_profile_radar_encode_targets(
		page_b ? RADIANT_PROFILE_RADAR_PAGE_TARGETS_B
		       : RADIANT_PROFILE_RADAR_PAGE_TARGETS_A,
		&r->targets[base], body);
	return page_b ? RADIANT_PROFILE_RADAR_SLOT_TARGETS_B
		      : RADIANT_PROFILE_RADAR_SLOT_TARGETS_A;
}

enum radiant_profile_radar_slot radiant_profile_radar_next(struct radiant_profile_radar *r,
							   uint8_t *body)
{
	static const uint8_t background[3] = {
		RADIANT_PROFILE_COMMON_PAGE_80, RADIANT_PROFILE_COMMON_PAGE_81,
		RADIANT_PROFILE_COMMON_PAGE_82,
	};
	const bool in_pattern = r->state != RADIANT_PROFILE_RADAR_STATE_BROADCASTING;
	const uint8_t n_targets = count_targets(r);

	if (r->off) {
		return RADIANT_PROFILE_RADAR_SLOT_OFF;
	}
	r->m++;
	if (r->stay_on_wait > 0u && --r->stay_on_wait == 0u) {
		begin_aborted(r);
	}

	/* Shutdown / aborted patterns run on a message count. */
	if (in_pattern) {
		if (r->state_left == 0u) {
			if (r->state == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_ABORTED) {
				r->state = RADIANT_PROFILE_RADAR_STATE_BROADCASTING;
			} else {
				r->off = true;
				return RADIANT_PROFILE_RADAR_SLOT_OFF;
			}
		} else {
			r->state_left--;
		}
	}

	/* Status interleave in any shutdown/aborted state (6.3.3, 6.3.4):
	 * every status_every-th message from the pattern's first, which is a
	 * status page - the units' "1A1A..." (C5 captures). */
	if (r->state != RADIANT_PROFILE_RADAR_STATE_BROADCASTING) {
		const bool status_slot = (r->pattern_m % r->cfg.status_every) == 0u;

		r->pattern_m++;
		if (status_slot) {
			return fill_status(r, body,
					   r->state == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_REQUESTED ||
					   r->state == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_FORCED);
		}
	}

	/* A requested page goes out next, ahead of the main page. */
	if (r->req_left > 0u) {
		r->req_left--;
		if (r->req_page == RADIANT_PROFILE_RADAR_PAGE_STATUS) {
			return fill_status(r, body, !r->detecting);
		}
		return fill_common(r, r->req_page, body);
	}

	/* Error state: page 87 as the main page (>= 7/8, 6.3.2). */
	if (r->error_active) {
		(void)radiant_profile_radar_encode_error(&r->error, body);
		return RADIANT_PROFILE_RADAR_SLOT_ERROR;
	}

	/* Detection not functioning: page 1 with clear-targets (6.3.1). */
	if (!r->detecting) {
		return fill_status(r, body, true);
	}

	/* >4 targets: 48/49 alternate, no background pages (6.3.6). */
	if (n_targets > RADIANT_PROFILE_RADAR_TARGETS_PER_PAGE) {
		r->ab_toggle = !r->ab_toggle;
		return fill_targets(r, !r->ab_toggle, body);
	}
	r->ab_toggle = false;

	/* <=4 targets: page 48, one background page every N (6.3.5). */
	if (++r->since_background >= r->cfg.background_every) {
		r->since_background = 0u;
		const uint8_t page = background[r->background_next];

		r->background_next = (uint8_t)((r->background_next + 1u) % 3u);
		return fill_common(r, page, body);
	}
	return fill_targets(r, false, body);
}
