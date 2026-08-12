/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_hr.c - ANT+ Heart Rate, device type 0x78.
 *
 * Provenance: the ANT+ Heart Rate Monitor device profile, as published in the
 * HRM sample documentation on thisisant.com - publicly retrievable, cited by
 * Colin on 2026-08-11 - by way of this repository's own two prior statements
 * of the same layouts: docs/device-profiles.md section "Heart Rate, device
 * type 0x78" (formerly docs/ant-plus-profiles.md, absorbed into it; cited by
 * section rather than by line number so the citation cannot silently drift)
 * and tools/ant_pages.py's five heart-rate
 * encoders, which are the byte-for-byte reference this file is checked
 * against. ANT+ device profiles are open spec and implementable anywhere in
 * this library (fact 5 of docs/decisions/0008, amending
 * docs/decisions/0002-clean-room-policy.md). No sdk-ant source was consulted
 * and nothing here derives from libant.a.
 *
 * There is no cryptography in this file and no call to radiant_sec anywhere in
 * it. See the header.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_common.h"
#include "profile_compat.h"
#include "profile_hr.h"
#include "profile_sched.h"

/* The background rotation, in the order a strap walks it. */
static const uint8_t hr_background[PROFILE_HR_BACKGROUND_PAGES] = {
	PROFILE_HR_PAGE_DEFAULT,
	PROFILE_HR_PAGE_CUMULATIVE,
	PROFILE_HR_PAGE_MANUFACTURER,
	PROFILE_HR_PAGE_PRODUCT,
};

/* ── The pages ──────────────────────────────────────────────────────────── */

int profile_hr_encode(uint8_t page, const uint8_t body3[3], uint16_t event_time,
		      uint8_t beats, uint8_t computed_hr, bool toggle,
		      uint8_t *out)
{
	if (body3 == NULL || out == NULL) {
		return -EINVAL;
	}
	if (page > PROFILE_HR_PAGE_MASK) {
		/* Byte 0 bit 7 is the page-change toggle, so a page number on
		 * this device type is 7-bit and nothing at or above 0x80 is
		 * expressible at all. That is the constraint that decided the
		 * whole compat allocation, and it is worth an error rather than
		 * a truncation. */
		return -EINVAL;
	}

	out[0] = (uint8_t)(page | (toggle ? PROFILE_HR_PAGE_TOGGLE : 0u));
	out[1] = body3[0];
	out[2] = body3[1];
	out[3] = body3[2];
	out[4] = (uint8_t)(event_time & 0xFFu);
	out[5] = (uint8_t)((event_time >> 8) & 0xFFu);
	out[6] = beats;
	out[7] = computed_hr;
	return 0;
}

/* Every encoder below hands profile_hr_encode() the same four tail values, and
 * none of them can opt out: a page that changed bytes [4..7] would not be a new
 * page, it would be a broken sensor. */
static int hr_page(const struct profile_hr *hr, uint8_t page,
		   const uint8_t body3[3], bool toggle, uint8_t *out)
{
	if (hr == NULL) {
		return -EINVAL;
	}
	return profile_hr_encode(page, body3, hr->event_time, hr->beats,
				 hr->computed_hr, toggle, out);
}

int profile_hr_encode_default(const struct profile_hr *hr, bool toggle,
			      uint8_t *out)
{
	const uint8_t body3[3] = { PROFILE_COMMON_INVALID_U8,
				   PROFILE_COMMON_INVALID_U8,
				   PROFILE_COMMON_INVALID_U8 };

	return hr_page(hr, PROFILE_HR_PAGE_DEFAULT, body3, toggle, out);
}

int profile_hr_encode_cumulative(const struct profile_hr *hr, bool toggle,
				 uint8_t *out)
{
	uint8_t body3[3];

	if (hr == NULL) {
		return -EINVAL;
	}
	body3[0] = (uint8_t)(hr->operating_time_2s & 0xFFu);
	body3[1] = (uint8_t)((hr->operating_time_2s >> 8) & 0xFFu);
	body3[2] = (uint8_t)((hr->operating_time_2s >> 16) & 0xFFu);
	return hr_page(hr, PROFILE_HR_PAGE_CUMULATIVE, body3, toggle, out);
}

int profile_hr_encode_manufacturer(const struct profile_hr *hr, bool toggle,
				   uint8_t *out)
{
	uint8_t body3[3];

	if (hr == NULL) {
		return -EINVAL;
	}
	/* EIGHT bits of manufacturer id here, against common page 80's
	 * sixteen. Different fields, different widths, one struct member each
	 * so the truncation cannot happen silently. */
	body3[0] = hr->cfg.manufacturer_id_8;
	body3[1] = (uint8_t)(hr->cfg.serial_upper16 & 0xFFu);
	body3[2] = (uint8_t)((hr->cfg.serial_upper16 >> 8) & 0xFFu);
	return hr_page(hr, PROFILE_HR_PAGE_MANUFACTURER, body3, toggle, out);
}

int profile_hr_encode_product(const struct profile_hr *hr, bool toggle,
			      uint8_t *out)
{
	uint8_t body3[3];

	if (hr == NULL) {
		return -EINVAL;
	}
	body3[0] = hr->cfg.hw_version;
	body3[1] = hr->cfg.sw_version;
	body3[2] = hr->cfg.model_number_8;
	return hr_page(hr, PROFILE_HR_PAGE_PRODUCT, body3, toggle, out);
}

int profile_hr_encode_previous_beat(const struct profile_hr *hr, bool toggle,
				    uint8_t *out)
{
	uint8_t body3[3];

	if (hr == NULL) {
		return -EINVAL;
	}
	/* Byte [1] is manufacturer-specific and 0xFF when unused; bytes [2..3]
	 * are the PREVIOUS heartbeat event time, which beside bytes [4..5] is
	 * an R-R interval from one packet. */
	body3[0] = PROFILE_COMMON_INVALID_U8;
	body3[1] = (uint8_t)(hr->previous_event_time & 0xFFu);
	body3[2] = (uint8_t)((hr->previous_event_time >> 8) & 0xFFu);
	return hr_page(hr, PROFILE_HR_PAGE_PREVIOUS_BEAT, body3, toggle, out);
}

/* ── The master ─────────────────────────────────────────────────────────── */

bool profile_hr_toggle(const struct profile_hr *hr)
{
	if (hr == NULL) {
		return false;
	}
	/* Transmitted messages, not data pages: the common pages and the compat
	 * pages are messages, and a toggle that skipped them would step out of
	 * the sensor's own sequence twice per 121-message cycle. */
	return ((hr->messages / PROFILE_HR_TOGGLE_INTERVAL) & 1u) != 0u;
}

static bool hr_toggle_cb(void *user)
{
	return profile_hr_toggle((const struct profile_hr *)user);
}

/*
 * One data slot.
 *
 * `page` is what the engine's rotation offered and it is ignored, because this
 * profile's rotation is not a round robin: page 0x04 goes out in every data
 * slot except one in 64, which takes the next background page. The engine's
 * one-entry rotation exists to make the slot a DATA slot at all; which data
 * page rides it is the profile's rule and belongs here.
 */
static int hr_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			void *user)
{
	struct profile_hr *hr = (struct profile_hr *)user;
	bool               toggle;
	uint8_t            background;

	(void)page;
	/* Device type 0x78 carries no event counter of the envelope's kind; its
	 * accumulators are the beat count and the event time, both in the tail
	 * every page shares. */
	(void)counter;

	if (hr == NULL) {
		return -EINVAL;
	}

	toggle = profile_hr_toggle(hr);
	hr->data_pages++;

	if ((hr->data_pages % PROFILE_HR_BACKGROUND_INTERVAL) != 0u) {
		return profile_hr_encode_previous_beat(hr, toggle, body);
	}

	background = hr_background[hr->background_next];
	hr->background_next = (uint8_t)((hr->background_next + 1u) %
					PROFILE_HR_BACKGROUND_PAGES);

	switch (background) {
	case PROFILE_HR_PAGE_DEFAULT:
		return profile_hr_encode_default(hr, toggle, body);
	case PROFILE_HR_PAGE_CUMULATIVE:
		return profile_hr_encode_cumulative(hr, toggle, body);
	case PROFILE_HR_PAGE_MANUFACTURER:
		return profile_hr_encode_manufacturer(hr, toggle, body);
	default:
		return profile_hr_encode_product(hr, toggle, body);
	}
}

/*
 * The common pages carry the toggle too.
 *
 * Every page on this device type does - tools/ant_pages.py's decode_hr()
 * exists because a decoder that dispatched on the raw byte would see each
 * common page under two different numbers and find neither - so a sensor that
 * emitted 0x50 with the toggle bit clear while its data pages carried it set
 * would be inconsistent with itself for four messages in eight.
 */
static int hr_common_80(uint8_t *body, void *user)
{
	struct profile_hr *hr = (struct profile_hr *)user;
	int                rc;

	if (hr == NULL) {
		return -EINVAL;
	}
	rc = profile_common_80(&hr->cfg.id, body);
	if (rc == 0 && profile_hr_toggle(hr)) {
		body[0] |= PROFILE_HR_PAGE_TOGGLE;
	}
	return rc;
}

static int hr_common_81(uint8_t *body, void *user)
{
	struct profile_hr *hr = (struct profile_hr *)user;
	int                rc;

	if (hr == NULL) {
		return -EINVAL;
	}
	rc = profile_common_81(&hr->cfg.id, body);
	if (rc == 0 && profile_hr_toggle(hr)) {
		body[0] |= PROFILE_HR_PAGE_TOGGLE;
	}
	return rc;
}

static int hr_common_82(uint8_t *body, void *user)
{
	struct profile_hr *hr = (struct profile_hr *)user;
	int                rc;

	if (hr == NULL) {
		return -EINVAL;
	}
	rc = profile_common_82(&hr->cfg.battery, body);
	if (rc == 0 && profile_hr_toggle(hr)) {
		body[0] |= PROFILE_HR_PAGE_TOGGLE;
	}
	return rc;
}

int profile_hr_init(struct profile_hr *hr, const struct profile_hr_cfg *cfg)
{
	struct profile_sched_cfg sched_cfg;

	if (hr == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(hr, 0, sizeof(*hr));
	hr->cfg = *cfg;
	hr->rotation[0] = PROFILE_HR_PAGE_PREVIOUS_BEAT;

	memset(&sched_cfg, 0, sizeof(sched_cfg));
	sched_cfg.desc = NULL; /* an ANT+ compatibility type announces no schema:
				* its pages are fixed by somebody else's
				* document, which is the whole point of it */
	sched_cfg.pages = hr->rotation;
	sched_cfg.n_pages = (uint8_t)sizeof(hr->rotation);
	sched_cfg.data_page = hr_data_page;
	sched_cfg.common_80 = hr_common_80;
	sched_cfg.common_81 = hr_common_81;
	sched_cfg.common_82 = (cfg->common_82_every != 0u) ? hr_common_82 : NULL;
	sched_cfg.common_82_every = cfg->common_82_every;
	sched_cfg.user = hr;

	return profile_sched_init(&hr->sched, &sched_cfg);
}

int profile_hr_set_compat(struct profile_hr *hr, struct profile_compat *compat)
{
	if (hr == NULL) {
		return -EINVAL;
	}

	hr->compat = compat;
	if (compat == NULL) {
		return profile_sched_set_client(&hr->sched, NULL);
	}

	/* The compat layer needs one fact about heart rate and gets it as a
	 * callback: what the toggle is for the message about to go out. */
	compat->cfg.toggle = hr_toggle_cb;
	compat->cfg.user = hr;
	return profile_compat_attach(compat, &hr->sched);
}

void profile_hr_beat(struct profile_hr *hr, uint16_t event_time)
{
	if (hr == NULL) {
		return;
	}
	hr->previous_event_time = hr->event_time;
	hr->event_time = event_time;
	hr->beats++; /* wraps at 256, and is MEANT to */
}

void profile_hr_set_computed(struct profile_hr *hr, uint8_t bpm)
{
	if (hr != NULL) {
		hr->computed_hr = bpm;
	}
}

void profile_hr_set_operating_time(struct profile_hr *hr, uint32_t seconds)
{
	if (hr != NULL) {
		hr->operating_time_2s = (seconds / PROFILE_HR_OPERATING_UNIT_S) &
					0xFFFFFFu;
	}
}

enum profile_slot_kind profile_hr_next(struct profile_hr *hr, uint64_t now_us,
				       uint8_t *body)
{
	enum profile_slot_kind kind;

	if (hr == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}

	if (hr->compat != NULL) {
		kind = profile_compat_next(hr->compat, &hr->sched, now_us, body);
	} else {
		kind = profile_sched_next(&hr->sched, body);
	}

	/*
	 * AFTER the message is built, not before. The toggle for message k is
	 * (k / 4) & 1 and k is the index of the message going out, so a counter
	 * advanced first would put every message in the next message's phase.
	 */
	if (kind != PROFILE_SLOT_IDLE) {
		hr->messages++;
	}
	return kind;
}
