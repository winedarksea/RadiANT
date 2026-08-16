/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_common.c - ANT+ common pages 80, 81 and 82 (encode) and 84 (decode).
 *
 * Provenance: docs/device-profiles.md section "Common pages" (formerly
 * docs/ant-plus-profiles.md, absorbed into it) and its mirror
 * in tools/ant_pages.py (encode_common_80/81/82), which are this project's own
 * prior derivation. See the header for the full note and for why the serial
 * number's sentinel is a privacy rule rather than a convenience.

 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile_common.h"

int profile_common_80(const struct profile_common_id *id, uint8_t *body)
{
	if (id == NULL || body == NULL) {
		return -EINVAL;
	}

	body[0] = PROFILE_COMMON_PAGE_80;
	body[1] = PROFILE_COMMON_INVALID_U8; /* reserved */
	body[2] = PROFILE_COMMON_INVALID_U8; /* reserved */
	body[3] = id->hw_revision;
	body[4] = (uint8_t)(id->manufacturer_id & 0xFFu);
	body[5] = (uint8_t)((id->manufacturer_id >> 8) & 0xFFu);
	body[6] = (uint8_t)(id->model_number & 0xFFu);
	body[7] = (uint8_t)((id->model_number >> 8) & 0xFFu);
	return 0;
}

int profile_common_81(const struct profile_common_id *id, uint8_t *body)
{
	if (id == NULL || body == NULL) {
		return -EINVAL;
	}

	body[0] = PROFILE_COMMON_PAGE_81;
	body[1] = PROFILE_COMMON_INVALID_U8; /* reserved */
	body[2] = id->sw_revision_supplemental;
	body[3] = id->sw_revision_main;
	body[4] = (uint8_t)(id->serial_number & 0xFFu);
	body[5] = (uint8_t)((id->serial_number >> 8) & 0xFFu);
	body[6] = (uint8_t)((id->serial_number >> 16) & 0xFFu);
	body[7] = (uint8_t)((id->serial_number >> 24) & 0xFFu);
	return 0;
}

int profile_common_82(const struct profile_common_battery *b, uint8_t *body)
{
	uint8_t descriptor;

	if (b == NULL || body == NULL) {
		return -EINVAL;
	}

	/* Byte [7]'s resolution bit is inverted: SET means 2-second units,
	 * clear means 16-second units. Getting it backwards scales operating
	 * time by 8 with nothing to catch it. */
	descriptor = (uint8_t)((b->coarse_voltage & 0x0Fu) |
			       ((b->status & 0x07u) << 4));
	if (!b->resolution_16s) {
		descriptor |= 0x80u;
	}

	body[0] = PROFILE_COMMON_PAGE_82;
	body[1] = PROFILE_COMMON_INVALID_U8; /* reserved */
	body[2] = b->battery_id;
	body[3] = (uint8_t)(b->operating_time & 0xFFu);
	body[4] = (uint8_t)((b->operating_time >> 8) & 0xFFu);
	body[5] = (uint8_t)((b->operating_time >> 16) & 0xFFu);
	body[6] = b->fractional_voltage;
	body[7] = descriptor;
	return 0;
}

int profile_common_decode_84(const uint8_t *body, struct profile_common_84 *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}

	/* Table 6-16 byte [0]. Checked rather than assumed: this decoder is
	 * reached from a path that dispatches on the whole 0x40-0x5D range
	 * (radiant_common_adapter.c), so "some other common page" is a routine
	 * caller here, not a bug - but it is the ADAPTER's job to filter, and a
	 * decoder that silently decoded page 0x50's manufacturer id as a
	 * temperature would make that filter's absence invisible. */
	if (body[0] != PROFILE_COMMON_PAGE_84) {
		return -EINVAL;
	}

	/* Byte [1] is reserved 0xFF and is deliberately NOT validated. A master
	 * that puts something else there is out of spec in a way that says
	 * nothing about bytes [2..7], and rejecting the page over it would
	 * discard good weather data to enforce a byte nobody reads. */

	out->slot[0].subpage = body[2];
	out->slot[0].raw = (uint16_t)body[4] | ((uint16_t)body[5] << 8);
	out->slot[1].subpage = body[3];
	out->slot[1].raw = (uint16_t)body[6] | ((uint16_t)body[7] << 8);

	return 0;
}
