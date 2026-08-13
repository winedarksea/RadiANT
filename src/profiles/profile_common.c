/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_common.c - ANT+ common pages 80, 81 and 82.
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
