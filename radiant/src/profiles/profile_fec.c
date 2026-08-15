/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_fec.c - ANT+ Fitness Equipment Control, device type 0x11. See
 * profile_fec.h for the provenance, the two page tables and the four traps.
 *
 * No cryptography in this file; no call to radiant_sec. No radiant header and
 * no bridge header, direct or transitive - the same claim profile_hr.c and
 * profile_power.c make about themselves.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile_fec.h"

/*
 * Byte 7's high nibble, split per Table 8-10. Shared by both pages because the
 * spec says so in as many words (section 8.5.2.7: "this bit field is the same
 * for all broadcast FE data pages"), so it is one function rather than two
 * copies that could drift.
 */
static void decode_fe_state(uint8_t byte7, uint8_t *state, bool *lap_toggle)
{
	/* Bits 0-2 OF THE NIBBLE, i.e. bits 4-6 of the byte. Masking the whole
	 * nibble would fold the lap toggle into the state - see the header. */
	*state = (uint8_t)((byte7 >> 4) & 0x07u);
	*lap_toggle = ((byte7 & 0x80u) != 0u);
}

int profile_fec_decode_general(const uint8_t *body,
			       struct profile_fec_general *out)
{
	uint8_t caps;

	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_GENERAL) {
		return -EINVAL;
	}

	/* Table 8-8: bits 5-7 are "Do Not Interpret", so they are dropped here
	 * rather than handed to a caller who might switch on the whole byte. */
	out->equipment_type = (uint8_t)(body[1] & PROFILE_FEC_TYPE_MASK);

	out->elapsed_time_qs = body[2];
	out->distance_m = body[3];

	out->speed_mm_s = (uint16_t)body[4] | ((uint16_t)body[5] << 8);
	out->speed_valid = (out->speed_mm_s != PROFILE_FEC_INVALID_SPEED);

	out->heart_rate_bpm = body[6];
	out->heart_rate_valid = (body[6] != PROFILE_FEC_INVALID_HR);

	caps = (uint8_t)(body[7] & 0x0Fu);
	out->hr_source = (uint8_t)(caps & 0x03u);
	/* Table 8-9 bit 2. The enable bit IS the validity of byte 3; there is
	 * no sentinel to fall back on (section 8.5.2.3). */
	out->distance_valid = ((caps & (1u << 2)) != 0u);
	out->speed_virtual = ((caps & (1u << 3)) != 0u);

	decode_fe_state(body[7], &out->state, &out->lap_toggle);

	return 0;
}

int profile_fec_decode_trainer(const uint8_t *body,
			       struct profile_fec_trainer *out)
{
	uint16_t inst;

	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_TRAINER) {
		return -EINVAL;
	}

	out->event_count = body[1];

	out->cadence_rpm = body[2];
	out->cadence_valid = (body[2] != PROFILE_FEC_INVALID_CADENCE);

	/* Bytes 3-4, one earlier than Bicycle Power page 0x10's 4-5. */
	out->acc_power_w = (uint16_t)body[3] | ((uint16_t)body[4] << 8);

	/* 1.5 bytes: byte 5 is the low 8 bits, byte 6's LOW nibble the high 4. */
	inst = (uint16_t)body[5] | (uint16_t)(((uint16_t)body[6] & 0x0Fu) << 8);
	out->inst_power_w = inst;
	/* The one cross-field sentinel in this repository: 0xFFF here condemns
	 * the accumulator in bytes 3-4 as well (Table 8-25). */
	out->power_valid = (inst != PROFILE_FEC_INVALID_INST_POWER);

	out->trainer_status = (uint8_t)((body[6] >> 4) & 0x0Fu);
	out->flags = (uint8_t)(body[7] & 0x0Fu);

	decode_fe_state(body[7], &out->state, &out->lap_toggle);

	return 0;
}
