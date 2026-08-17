/* SPDX-License-Identifier: Apache-2.0 */
/* profile_lev.c - see profile_lev.h. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_common.h"
#include "profile_lev.h"
#include "profile_sched.h"

/* A 12-bit field: whole low byte, then the next byte's low nibble with the
 * high nibble reserved as 0xF. */
static void put12(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v & 0xFFu);
	b[1] = (uint8_t)(0xF0u | ((v >> 8) & 0x0Fu));
}

static uint16_t get12(const uint8_t *b)
{
	return (uint16_t)(b[0] | ((uint16_t)(b[1] & 0x0Fu) << 8));
}

/* ── Bit-field pack/unpack ──────────────────────────────────────────────── */

uint8_t profile_lev_pack_temperature(bool motor_alert, uint8_t motor_level,
				     bool battery_alert, uint8_t battery_level)
{
	return (uint8_t)((motor_alert ? 0x80u : 0u) |
			 ((motor_level & 0x07u) << 4) |
			 (battery_alert ? 0x08u : 0u) |
			 (battery_level & 0x07u));
}

void profile_lev_unpack_temperature(uint8_t raw, bool *motor_alert,
				    uint8_t *motor_level, bool *battery_alert,
				    uint8_t *battery_level)
{
	if (motor_alert != NULL) {
		*motor_alert = (raw & 0x80u) != 0u;
	}
	if (motor_level != NULL) {
		*motor_level = (uint8_t)((raw >> 4) & 0x07u);
	}
	if (battery_alert != NULL) {
		*battery_alert = (raw & 0x08u) != 0u;
	}
	if (battery_level != NULL) {
		*battery_level = (uint8_t)(raw & 0x07u);
	}
}

uint8_t profile_lev_pack_travel_mode(uint8_t assist, uint8_t regen)
{
	return (uint8_t)(((assist & 0x07u) << 3) | (regen & 0x07u));
}

void profile_lev_unpack_travel_mode(uint8_t raw, uint8_t *assist,
				    uint8_t *regen)
{
	if (assist != NULL) {
		*assist = (uint8_t)((raw >> 3) & 0x07u);
	}
	if (regen != NULL) {
		*regen = (uint8_t)(raw & 0x07u);
	}
}

uint8_t profile_lev_pack_gear(bool gears_exist, bool manual, uint8_t rear,
			      uint8_t front)
{
	return (uint8_t)((gears_exist ? 0x80u : 0u) | (manual ? 0x40u : 0u) |
			 ((rear & 0x0Fu) << 2) | (front & 0x03u));
}

void profile_lev_unpack_gear(uint8_t raw, bool *gears_exist, bool *manual,
			     uint8_t *rear, uint8_t *front)
{
	if (gears_exist != NULL) {
		*gears_exist = (raw & 0x80u) != 0u;
	}
	if (manual != NULL) {
		*manual = (raw & 0x40u) != 0u;
	}
	if (rear != NULL) {
		*rear = (uint8_t)((raw >> 2) & 0x0Fu);
	}
	if (front != NULL) {
		*front = (uint8_t)(raw & 0x03u);
	}
}

uint8_t profile_lev_pack_modes_supported(uint8_t assist, uint8_t regen)
{
	return profile_lev_pack_travel_mode(assist, regen);
}

void profile_lev_unpack_modes_supported(uint8_t raw, uint8_t *assist,
					uint8_t *regen)
{
	profile_lev_unpack_travel_mode(raw, assist, regen);
}

uint16_t profile_lev_pack_command(uint8_t rear, uint8_t front, uint16_t flags)
{
	return (uint16_t)(((uint16_t)(rear & 0x0Fu) << 6) |
			  ((uint16_t)(front & 0x03u) << 4) | (flags & 0x0Fu));
}

void profile_lev_unpack_command(uint16_t raw, uint8_t *rear, uint8_t *front,
				uint16_t *flags)
{
	if (rear != NULL) {
		*rear = (uint8_t)((raw >> 6) & 0x0Fu);
	}
	if (front != NULL) {
		*front = (uint8_t)((raw >> 4) & 0x03u);
	}
	if (flags != NULL) {
		*flags = (uint16_t)(raw & 0x0Fu);
	}
}

/* ── Travel-mode mapping, Table 6-1 ─────────────────────────────────────── */

/* Row n-1 holds the n recommended settings, which are also the upper bound of
 * each group - so one table serves both directions. */
static const uint8_t lev_modes[7][7] = {
	{ 7 },
	{ 3, 7 },
	{ 2, 4, 7 },
	{ 1, 3, 5, 7 },
	{ 1, 2, 3, 5, 7 },
	{ 1, 2, 3, 4, 5, 7 },
	{ 1, 2, 3, 4, 5, 6, 7 },
};

int profile_lev_mode_setting(uint8_t n_supported, uint8_t index, uint8_t *mode)
{
	if (mode == NULL || n_supported < 1u ||
	    n_supported > PROFILE_LEV_LEVEL_MAX || index >= n_supported) {
		return -EINVAL;
	}
	*mode = lev_modes[n_supported - 1u][index];
	return 0;
}

int profile_lev_mode_group(uint8_t n_supported, uint8_t mode, uint8_t *index)
{
	uint8_t i;

	if (index == NULL || n_supported < 1u ||
	    n_supported > PROFILE_LEV_LEVEL_MAX || mode < 1u ||
	    mode > PROFILE_LEV_LEVEL_MAX) {
		return -EINVAL;
	}
	for (i = 0u; i < n_supported; i++) {
		if (mode <= lev_modes[n_supported - 1u][i]) {
			*index = i;
			return 0;
		}
	}
	return -EINVAL;
}

/* ── Pages ──────────────────────────────────────────────────────────────── */

int profile_lev_encode_page1(const struct profile_lev_page1 *p, uint8_t *out)
{
	if (p == NULL || out == NULL || p->speed > PROFILE_LEV_SPEED_MAX) {
		return -EINVAL;
	}

	out[0] = PROFILE_LEV_PAGE_SPEED_SYSTEM_1;
	out[1] = p->temperature;
	out[2] = p->travel_mode;
	out[3] = p->system;
	out[4] = p->gear;
	out[5] = p->error;
	put12(&out[6], p->speed);
	return 0;
}

int profile_lev_decode_page1(const uint8_t *body, struct profile_lev_page1 *p)
{
	if (body == NULL || p == NULL ||
	    body[0] != PROFILE_LEV_PAGE_SPEED_SYSTEM_1) {
		return -EINVAL;
	}

	p->temperature = body[1];
	p->travel_mode = body[2];
	p->system = body[3];
	p->gear = body[4];
	p->error = body[5];
	p->speed = get12(&body[6]);
	return 0;
}

int profile_lev_encode_page2(const struct profile_lev_page2 *p, uint8_t *out)
{
	if (p == NULL || out == NULL || p->speed > PROFILE_LEV_SPEED_MAX ||
	    p->range_km > PROFILE_LEV_RANGE_MAX ||
	    p->odometer > PROFILE_LEV_ODOMETER_MAX) {
		return -EINVAL;
	}

	out[0] = PROFILE_LEV_PAGE_SPEED_DISTANCE;
	out[1] = (uint8_t)(p->odometer & 0xFFu);
	out[2] = (uint8_t)((p->odometer >> 8) & 0xFFu);
	out[3] = (uint8_t)((p->odometer >> 16) & 0xFFu);
	put12(&out[4], p->range_km);
	put12(&out[6], p->speed);
	return 0;
}

int profile_lev_decode_page2(const uint8_t *body, struct profile_lev_page2 *p)
{
	if (body == NULL || p == NULL ||
	    body[0] != PROFILE_LEV_PAGE_SPEED_DISTANCE) {
		return -EINVAL;
	}

	p->odometer = (uint32_t)body[1] | ((uint32_t)body[2] << 8) |
		      ((uint32_t)body[3] << 16);
	p->range_km = get12(&body[4]);
	p->speed = get12(&body[6]);
	return 0;
}

int profile_lev_encode_page34(const struct profile_lev_page34 *p, uint8_t *out)
{
	if (p == NULL || out == NULL || p->speed > PROFILE_LEV_SPEED_MAX ||
	    p->fuel > PROFILE_LEV_FUEL_MAX ||
	    p->odometer > PROFILE_LEV_ODOMETER_MAX) {
		return -EINVAL;
	}

	out[0] = PROFILE_LEV_PAGE_SPEED_DISTANCE_2;
	out[1] = (uint8_t)(p->odometer & 0xFFu);
	out[2] = (uint8_t)((p->odometer >> 8) & 0xFFu);
	out[3] = (uint8_t)((p->odometer >> 16) & 0xFFu);
	put12(&out[4], p->fuel);
	put12(&out[6], p->speed);
	return 0;
}

int profile_lev_decode_page34(const uint8_t *body, struct profile_lev_page34 *p)
{
	if (body == NULL || p == NULL ||
	    body[0] != PROFILE_LEV_PAGE_SPEED_DISTANCE_2) {
		return -EINVAL;
	}

	p->odometer = (uint32_t)body[1] | ((uint32_t)body[2] << 8) |
		      ((uint32_t)body[3] << 16);
	p->fuel = get12(&body[4]);
	p->speed = get12(&body[6]);
	return 0;
}

int profile_lev_encode_page3(const struct profile_lev_page3 *p, uint8_t *out)
{
	if (p == NULL || out == NULL || p->speed > PROFILE_LEV_SPEED_MAX ||
	    p->soc_pct > PROFILE_LEV_PERCENT_MAX ||
	    (p->assist_pct > PROFILE_LEV_PERCENT_MAX &&
	     p->assist_pct != PROFILE_LEV_ASSIST_PCT_UNKNOWN)) {
		return -EINVAL;
	}

	out[0] = PROFILE_LEV_PAGE_SPEED_SYSTEM_2;
	out[1] = (uint8_t)((p->battery_empty ? PROFILE_LEV_SOC_EMPTY_WARNING
					     : 0u) | (p->soc_pct & 0x7Fu));
	out[2] = p->travel_mode;
	out[3] = p->system;
	out[4] = p->gear;
	out[5] = p->assist_pct;
	put12(&out[6], p->speed);
	return 0;
}

int profile_lev_decode_page3(const uint8_t *body, struct profile_lev_page3 *p)
{
	if (body == NULL || p == NULL ||
	    body[0] != PROFILE_LEV_PAGE_SPEED_SYSTEM_2) {
		return -EINVAL;
	}

	p->soc_pct = (uint8_t)(body[1] & 0x7Fu);
	p->battery_empty = (body[1] & PROFILE_LEV_SOC_EMPTY_WARNING) != 0u;
	p->travel_mode = body[2];
	p->system = body[3];
	p->gear = body[4];
	p->assist_pct = body[5];
	p->speed = get12(&body[6]);
	return 0;
}

/* Byte [3] finishes two unrelated fields: bits 3..0 the charge-cycle count
 * whose low byte is [2], bits 7..4 the fuel consumption whose low byte is [4]. */
int profile_lev_encode_page4(const struct profile_lev_page4 *p, uint8_t *out)
{
	if (p == NULL || out == NULL ||
	    p->charge_cycles > PROFILE_LEV_CYCLES_MAX ||
	    p->fuel > PROFILE_LEV_FUEL_MAX) {
		return -EINVAL;
	}

	out[0] = PROFILE_LEV_PAGE_BATTERY;
	out[1] = PROFILE_COMMON_INVALID_U8;
	out[2] = (uint8_t)(p->charge_cycles & 0xFFu);
	out[3] = (uint8_t)((((p->fuel >> 8) & 0x0Fu) << 4) |
			   ((p->charge_cycles >> 8) & 0x0Fu));
	out[4] = (uint8_t)(p->fuel & 0xFFu);
	out[5] = p->voltage_quarter;
	out[6] = (uint8_t)(p->charge_distance & 0xFFu);
	out[7] = (uint8_t)((p->charge_distance >> 8) & 0xFFu);
	return 0;
}

int profile_lev_decode_page4(const uint8_t *body, struct profile_lev_page4 *p)
{
	if (body == NULL || p == NULL || body[0] != PROFILE_LEV_PAGE_BATTERY) {
		return -EINVAL;
	}

	p->charge_cycles = (uint16_t)(body[2] |
				      ((uint16_t)(body[3] & 0x0Fu) << 8));
	p->fuel = (uint16_t)(body[4] | ((uint16_t)(body[3] >> 4) << 8));
	p->voltage_quarter = body[5];
	p->charge_distance = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	return 0;
}

int profile_lev_encode_page5(const struct profile_lev_page5 *p, uint8_t *out)
{
	if (p == NULL || out == NULL || p->wheel_mm > PROFILE_LEV_WHEEL_MAX) {
		return -EINVAL;
	}

	out[0] = PROFILE_LEV_PAGE_CAPABILITIES;
	out[1] = PROFILE_COMMON_INVALID_U8;
	out[2] = p->modes_supported;
	put12(&out[3], p->wheel_mm);
	memset(&out[5], PROFILE_COMMON_INVALID_U8, 3);
	return 0;
}

int profile_lev_decode_page5(const uint8_t *body, struct profile_lev_page5 *p)
{
	if (body == NULL || p == NULL ||
	    body[0] != PROFILE_LEV_PAGE_CAPABILITIES) {
		return -EINVAL;
	}

	p->modes_supported = body[2];
	p->wheel_mm = get12(&body[3]);
	return 0;
}

int profile_lev_encode_display(const struct profile_lev_display *d,
			       uint8_t *out)
{
	if (d == NULL || out == NULL || d->wheel_mm > PROFILE_LEV_WHEEL_MAX) {
		return -EINVAL;
	}

	out[0] = PROFILE_LEV_PAGE_DISPLAY;
	put12(&out[1], d->wheel_mm);
	out[3] = d->travel_mode;
	out[4] = (uint8_t)(d->command & 0xFFu);
	out[5] = (uint8_t)((d->command >> 8) & 0xFFu);
	out[6] = (uint8_t)(d->manufacturer_id & 0xFFu);
	out[7] = (uint8_t)((d->manufacturer_id >> 8) & 0xFFu);
	return 0;
}

int profile_lev_decode_display(const uint8_t *body,
			       struct profile_lev_display *d)
{
	if (body == NULL || d == NULL || body[0] != PROFILE_LEV_PAGE_DISPLAY) {
		return -EINVAL;
	}

	d->wheel_mm = get12(&body[1]);
	d->travel_mode = body[3];
	d->command = (uint16_t)(body[4] | ((uint16_t)body[5] << 8));
	d->manufacturer_id = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	return 0;
}

int profile_lev_encode_request(const struct profile_lev_request *r,
			       uint8_t *out)
{
	if (r == NULL || out == NULL || r->transmit_response == 0u) {
		return -EINVAL;
	}

	out[0] = PROFILE_LEV_PAGE_REQUEST;
	memset(&out[1], PROFILE_COMMON_INVALID_U8, 4);
	out[5] = r->transmit_response;
	out[6] = r->page;
	out[7] = r->command_type;
	return 0;
}

int profile_lev_decode_request(const uint8_t *body,
			       struct profile_lev_request *r)
{
	if (body == NULL || r == NULL || body[0] != PROFILE_LEV_PAGE_REQUEST) {
		return -EINVAL;
	}

	r->transmit_response = body[5];
	r->page = body[6];
	r->command_type = body[7];
	return 0;
}

/* ── The master ─────────────────────────────────────────────────────────── */

static int lev_encode_page(struct profile_lev *lev, uint8_t page, uint8_t *body)
{
	switch (page) {
	case PROFILE_LEV_PAGE_SPEED_SYSTEM_1:
		return profile_lev_encode_page1(&lev->page1, body);
	case PROFILE_LEV_PAGE_SPEED_DISTANCE:
		return profile_lev_encode_page2(&lev->page2, body);
	case PROFILE_LEV_PAGE_SPEED_DISTANCE_2:
		return profile_lev_encode_page34(&lev->page34, body);
	case PROFILE_LEV_PAGE_SPEED_SYSTEM_2:
		return profile_lev_encode_page3(&lev->page3, body);
	case PROFILE_LEV_PAGE_BATTERY:
		return profile_lev_encode_page4(&lev->page4, body);
	case PROFILE_LEV_PAGE_CAPABILITIES:
		return profile_lev_encode_page5(&lev->page5, body);
	case PROFILE_COMMON_PAGE_80:
		return profile_common_80(&lev->cfg.id, body);
	case PROFILE_COMMON_PAGE_81:
		return profile_common_81(&lev->cfg.id, body);
	default:
		return -EINVAL;
	}
}

static bool lev_common_due(const struct profile_lev *lev)
{
	return lev->request_left == 0u &&
	       lev->slot == PROFILE_LEV_ROTATION_SLOTS - 1u &&
	       lev->since_common >= PROFILE_LEV_COMMON_EVERY;
}

/* The common pair rides the client seam because profile_sched.c's 119/120-of-
 * 121 placement is not this profile's every-20th-channel-period cadence. */
static bool lev_claim(uint32_t m, uint8_t *body, void *user)
{
	struct profile_lev *lev = (struct profile_lev *)user;

	(void)m;

	if (lev == NULL || !lev_common_due(lev)) {
		return false;
	}
	return lev_encode_page(lev, lev->common_81_next ? PROFILE_COMMON_PAGE_81
							: PROFILE_COMMON_PAGE_80,
			       body) == 0;
}

static int lev_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			 void *user)
{
	struct profile_lev *lev = (struct profile_lev *)user;

	(void)page;
	(void)counter;

	if (lev == NULL) {
		return -EINVAL;
	}

	if (lev->request_left != 0u) {
		return lev_encode_page(lev, lev->request_page, body);
	}

	switch (lev->slot) {
	case 0:
		return lev_encode_page(lev, PROFILE_LEV_PAGE_SPEED_SYSTEM_1,
				       body);
	case 1:
		if (lev->cfg.use_page_34 &&
		    lev->since_page_2 < PROFILE_LEV_PAGE_2_EVERY) {
			return lev_encode_page(
				lev, PROFILE_LEV_PAGE_SPEED_DISTANCE_2, body);
		}
		return lev_encode_page(lev, PROFILE_LEV_PAGE_SPEED_DISTANCE,
				       body);
	case 2:
		return lev_encode_page(lev, PROFILE_LEV_PAGE_SPEED_SYSTEM_2,
				       body);
	default:
		if (lev->cfg.send_page_4 && !lev->page_5_next) {
			return lev_encode_page(lev, PROFILE_LEV_PAGE_BATTERY,
					       body);
		}
		return lev_encode_page(lev, PROFILE_LEV_PAGE_CAPABILITIES,
				       body);
	}
}

int profile_lev_init(struct profile_lev *lev, const struct profile_lev_cfg *cfg)
{
	struct profile_sched_cfg    sched_cfg;
	struct profile_sched_client client;
	int                         rc;

	if (lev == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(lev, 0, sizeof(*lev));
	lev->cfg = *cfg;
	lev->rotation[0] = PROFILE_LEV_PAGE_SPEED_SYSTEM_1;
	lev->page1.error = PROFILE_LEV_ERROR_NONE;
	lev->page3.assist_pct = PROFILE_LEV_ASSIST_PCT_UNKNOWN;

	memset(&sched_cfg, 0, sizeof(sched_cfg));
	sched_cfg.desc = NULL;
	sched_cfg.pages = lev->rotation;
	sched_cfg.n_pages = (uint8_t)sizeof(lev->rotation);
	sched_cfg.data_page = lev_data_page;
	/* NULL: the pair rides the client seam, and LEV has no use for page 82
	 * (page 4 carries a richer battery picture). */
	sched_cfg.common_80 = NULL;
	sched_cfg.common_81 = NULL;
	sched_cfg.common_82 = NULL;
	sched_cfg.user = lev;

	rc = profile_sched_init(&lev->sched, &sched_cfg);
	if (rc != 0) {
		return rc;
	}

	client.claim = lev_claim;
	client.user = lev;
	return profile_sched_set_client(&lev->sched, &client);
}

void profile_lev_set_page1(struct profile_lev *lev,
			   const struct profile_lev_page1 *p)
{
	if (lev != NULL && p != NULL) {
		lev->page1 = *p;
	}
}

void profile_lev_set_page2(struct profile_lev *lev,
			   const struct profile_lev_page2 *p)
{
	if (lev != NULL && p != NULL) {
		lev->page2 = *p;
	}
}

void profile_lev_set_page34(struct profile_lev *lev,
			    const struct profile_lev_page34 *p)
{
	if (lev != NULL && p != NULL) {
		lev->page34 = *p;
	}
}

void profile_lev_set_page3(struct profile_lev *lev,
			   const struct profile_lev_page3 *p)
{
	if (lev != NULL && p != NULL) {
		lev->page3 = *p;
	}
}

void profile_lev_set_page4(struct profile_lev *lev,
			   const struct profile_lev_page4 *p)
{
	if (lev != NULL && p != NULL) {
		lev->page4 = *p;
	}
}

void profile_lev_set_page5(struct profile_lev *lev,
			   const struct profile_lev_page5 *p)
{
	if (lev != NULL && p != NULL) {
		lev->page5 = *p;
	}
}

int profile_lev_request(struct profile_lev *lev, uint8_t page, uint8_t count)
{
	if (lev == NULL) {
		return -EINVAL;
	}

	switch (page) {
	case PROFILE_LEV_PAGE_SPEED_SYSTEM_1:
	case PROFILE_LEV_PAGE_SPEED_DISTANCE:
	case PROFILE_LEV_PAGE_SPEED_DISTANCE_2:
	case PROFILE_LEV_PAGE_SPEED_SYSTEM_2:
	case PROFILE_LEV_PAGE_BATTERY:
	case PROFILE_LEV_PAGE_CAPABILITIES:
	case PROFILE_COMMON_PAGE_80:
	case PROFILE_COMMON_PAGE_81:
		break;
	default:
		return -ENOTSUP;
	}

	lev->request_page = page;
	lev->request_left = (count == 0u) ? 1u
			  : (count > PROFILE_LEV_REQUEST_COUNT_MAX)
				    ? PROFILE_LEV_REQUEST_COUNT_MAX
				    : count;
	return 0;
}

int profile_lev_apply_request(struct profile_lev *lev, const uint8_t *body)
{
	struct profile_lev_request req;
	int                        rc;

	if (lev == NULL) {
		return -EINVAL;
	}

	rc = profile_lev_decode_request(body, &req);
	if (rc != 0) {
		return rc;
	}
	if (req.command_type != PROFILE_LEV_REQUEST_CMD_PAGE ||
	    req.transmit_response == 0u) {
		return -EINVAL;
	}

	return profile_lev_request(lev, req.page,
				   (uint8_t)(req.transmit_response & 0x7Fu));
}

enum profile_slot_kind profile_lev_next(struct profile_lev *lev, uint8_t *body)
{
	enum profile_slot_kind kind;

	if (lev == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}

	kind = profile_sched_next(&lev->sched, body);
	if (kind == PROFILE_SLOT_IDLE) {
		return kind;
	}

	if (kind == PROFILE_SLOT_CLIENT) {
		kind = (body[0] == PROFILE_COMMON_PAGE_80)
			       ? PROFILE_SLOT_COMMON_80
			       : PROFILE_SLOT_COMMON_81;
		lev->common_81_next = !lev->common_81_next;
		lev->since_common = 0u;
	} else {
		lev->since_common++;
	}

	if (body[0] == PROFILE_LEV_PAGE_SPEED_DISTANCE) {
		lev->since_page_2 = 0u;
	} else if (lev->since_page_2 < PROFILE_LEV_PAGE_2_EVERY) {
		lev->since_page_2++;
	}

	lev->messages++;

	/* A requested page pre-empts the slot it landed in and the rotation
	 * restarts at page 1 once the reply is done. */
	if (lev->request_left != 0u) {
		lev->request_left--;
		if (lev->request_left == 0u) {
			lev->slot = 0u;
		}
		return kind;
	}

	if (lev->slot == PROFILE_LEV_ROTATION_SLOTS - 1u &&
	    kind != PROFILE_SLOT_COMMON_80 && kind != PROFILE_SLOT_COMMON_81) {
		lev->page_5_next = !lev->page_5_next;
	}
	lev->slot = (uint8_t)((lev->slot + 1u) % PROFILE_LEV_ROTATION_SLOTS);
	return kind;
}
