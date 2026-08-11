/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_power.c - ANT+ Bicycle Power, device type 0x0B.
 *
 * Provenance: docs/ant-plus-profiles.md lines 53-124, this project's own prior
 * clean-room derivation of the four bicycle-power page layouts, and its mirror
 * in tools/ant_pages.py (encode_power_std, encode_power_torque,
 * encode_power_torque_freq). See the header. No sdk-ant source was consulted
 * and nothing here derives from libant.a.
 *
 * There is no cryptography in this file and no call to radiant_sec anywhere in
 * it.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_common.h"
#include "profile_compat.h"
#include "profile_power.h"
#include "profile_sched.h"

/* ── The pages ──────────────────────────────────────────────────────────── */

int profile_power_encode_std(uint8_t event_count, uint8_t pedal_power,
			     uint8_t cadence, uint16_t acc_power,
			     uint16_t inst_power, uint8_t *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	out[0] = PROFILE_POWER_PAGE_STANDARD;
	out[1] = event_count;
	out[2] = pedal_power;
	out[3] = cadence;
	out[4] = (uint8_t)(acc_power & 0xFFu);
	out[5] = (uint8_t)((acc_power >> 8) & 0xFFu);
	out[6] = (uint8_t)(inst_power & 0xFFu);
	out[7] = (uint8_t)((inst_power >> 8) & 0xFFu);
	return 0;
}

int profile_power_encode_torque(uint8_t page,
				const struct profile_power_torque *t,
				uint8_t cadence, uint8_t *out)
{
	if (t == NULL || out == NULL) {
		return -EINVAL;
	}
	if (page != PROFILE_POWER_PAGE_WHEEL_TORQUE &&
	    page != PROFILE_POWER_PAGE_CRANK_TORQUE) {
		return -EINVAL;
	}

	out[0] = page;
	out[1] = t->event_count;
	out[2] = t->ticks;
	out[3] = cadence;
	out[4] = (uint8_t)(t->acc_period & 0xFFu);
	out[5] = (uint8_t)((t->acc_period >> 8) & 0xFFu);
	out[6] = (uint8_t)(t->acc_torque & 0xFFu);
	out[7] = (uint8_t)((t->acc_torque >> 8) & 0xFFu);
	return 0;
}

int profile_power_encode_torque_freq(uint8_t event_count,
				     uint16_t slope_tenth_nm_hz,
				     uint16_t time_stamp, uint16_t torque_ticks,
				     uint8_t *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	/* BIG-ENDIAN, and it is the only page in this repository that is. A
	 * copy-paste of the little-endian writer above produces a page that
	 * decodes to a plausible slope and an absurd power. */
	out[0] = PROFILE_POWER_PAGE_TORQUE_FREQ;
	out[1] = event_count;
	out[2] = (uint8_t)((slope_tenth_nm_hz >> 8) & 0xFFu);
	out[3] = (uint8_t)(slope_tenth_nm_hz & 0xFFu);
	out[4] = (uint8_t)((time_stamp >> 8) & 0xFFu);
	out[5] = (uint8_t)(time_stamp & 0xFFu);
	out[6] = (uint8_t)((torque_ticks >> 8) & 0xFFu);
	out[7] = (uint8_t)(torque_ticks & 0xFFu);
	return 0;
}

/* ── The master ─────────────────────────────────────────────────────────── */

static int power_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			   void *user)
{
	struct profile_power *pw = (struct profile_power *)user;

	/* The engine's rotation names the page here, unlike heart rate: this
	 * profile's pages ARE a round robin, one per page number the node
	 * emits, and each carries its own event count in byte [1]. */
	(void)counter;

	if (pw == NULL) {
		return -EINVAL;
	}

	switch (page) {
	case PROFILE_POWER_PAGE_STANDARD:
		return profile_power_encode_std(pw->event_count,
						pw->pedal_power, pw->cadence,
						pw->acc_power, pw->inst_power,
						body);
	case PROFILE_POWER_PAGE_WHEEL_TORQUE:
		return profile_power_encode_torque(page, &pw->wheel,
						   pw->cadence, body);
	case PROFILE_POWER_PAGE_CRANK_TORQUE:
		return profile_power_encode_torque(page, &pw->crank,
						   pw->cadence, body);
	case PROFILE_POWER_PAGE_TORQUE_FREQ:
		return profile_power_encode_torque_freq(
			pw->freq_event_count, pw->cfg.slope_tenth_nm_hz,
			pw->time_stamp, pw->torque_ticks, body);
	default:
		return -EINVAL;
	}
}

static int power_common_80(uint8_t *body, void *user)
{
	struct profile_power *pw = (struct profile_power *)user;

	return (pw == NULL) ? -EINVAL : profile_common_80(&pw->cfg.id, body);
}

static int power_common_81(uint8_t *body, void *user)
{
	struct profile_power *pw = (struct profile_power *)user;

	return (pw == NULL) ? -EINVAL : profile_common_81(&pw->cfg.id, body);
}

static int power_common_82(uint8_t *body, void *user)
{
	struct profile_power *pw = (struct profile_power *)user;

	return (pw == NULL) ? -EINVAL
			    : profile_common_82(&pw->cfg.battery, body);
}

int profile_power_init(struct profile_power *pw,
		       const struct profile_power_cfg *cfg)
{
	struct profile_sched_cfg sched_cfg;
	uint8_t                  i;

	if (pw == NULL || cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->n_pages == 0u || cfg->n_pages > PROFILE_POWER_MAX_PAGES) {
		return -EINVAL;
	}
	for (i = 0u; i < cfg->n_pages; i++) {
		switch (cfg->pages[i]) {
		case PROFILE_POWER_PAGE_STANDARD:
		case PROFILE_POWER_PAGE_WHEEL_TORQUE:
		case PROFILE_POWER_PAGE_CRANK_TORQUE:
		case PROFILE_POWER_PAGE_TORQUE_FREQ:
			break;
		default:
			/* A page this profile does not define would go out as
			 * whatever power_data_page() refused to build, i.e. as
			 * a silent slot for the life of the node. */
			return -EINVAL;
		}
	}

	memset(pw, 0, sizeof(*pw));
	pw->cfg = *cfg;

	/* Not reported, until the node says otherwise. 0xFF and not 0: zero
	 * cadence is a real reading, and a sensor that starts by claiming a
	 * genuine 0 rpm is claiming the rider has stopped. */
	pw->cadence = PROFILE_COMMON_INVALID_U8;
	pw->pedal_power = PROFILE_COMMON_INVALID_U8;

	memset(&sched_cfg, 0, sizeof(sched_cfg));
	sched_cfg.desc = NULL;
	sched_cfg.pages = pw->cfg.pages;
	sched_cfg.n_pages = pw->cfg.n_pages;
	sched_cfg.data_page = power_data_page;
	sched_cfg.common_80 = power_common_80;
	sched_cfg.common_81 = power_common_81;
	sched_cfg.common_82 = (cfg->common_82_every != 0u) ? power_common_82
							   : NULL;
	sched_cfg.common_82_every = cfg->common_82_every;
	sched_cfg.user = pw;

	return profile_sched_init(&pw->sched, &sched_cfg);
}

int profile_power_set_compat(struct profile_power *pw,
			     struct profile_compat *compat)
{
	if (pw == NULL) {
		return -EINVAL;
	}

	pw->compat = compat;
	if (compat == NULL) {
		return profile_sched_set_client(&pw->sched, NULL);
	}

	/* No toggle on this device type: byte [0] is a whole page number and
	 * bit 7 belongs to the page number. Installing heart rate's rule here
	 * would put page 0x90 on the air. */
	compat->cfg.toggle = NULL;
	compat->cfg.user = NULL;
	return profile_compat_attach(compat, &pw->sched);
}

void profile_power_event(struct profile_power *pw, uint16_t watts,
			 uint8_t cadence)
{
	if (pw == NULL) {
		return;
	}
	pw->event_count++;              /* wraps at 256, and is meant to */
	pw->acc_power += watts;         /* wraps at 65536, and is meant to */
	pw->inst_power = watts;
	pw->cadence = cadence;
}

void profile_power_torque_event(struct profile_power *pw, bool crank,
				uint16_t period_2048, uint16_t torque_32nm)
{
	struct profile_power_torque *t;

	if (pw == NULL) {
		return;
	}
	t = crank ? &pw->crank : &pw->wheel;
	t->event_count++;
	t->ticks++;
	t->acc_period += period_2048;
	t->acc_torque += torque_32nm;
}

void profile_power_freq_event(struct profile_power *pw, uint16_t d_time_2000,
			      uint16_t d_ticks)
{
	if (pw == NULL) {
		return;
	}
	pw->freq_event_count++;
	pw->time_stamp += d_time_2000;
	pw->torque_ticks += d_ticks;
}

void profile_power_set_pedal_balance(struct profile_power *pw, uint8_t percent)
{
	if (pw != NULL) {
		pw->pedal_power = percent;
	}
}

enum profile_slot_kind profile_power_next(struct profile_power *pw,
					  uint64_t now_us, uint8_t *body)
{
	if (pw == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}
	if (pw->compat != NULL) {
		return profile_compat_next(pw->compat, &pw->sched, now_us, body);
	}
	return profile_sched_next(&pw->sched, body);
}
