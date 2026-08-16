/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_profile_sanity.c - see radiant_profile_sanity.h for what this is
 * and, more importantly, for what it must never be applied to.
 *
 * Provenance: clean-room. See the header.
 */

#include <radiant/radiant_profile_sanity.h>
#include <radiant/radiant_frame.h>

/* Sentinels a profile uses to mean "not reported". Reading one of these as a
 * number produces a value that looks like an out-of-range reading rather
 * than an absent one, so both are excluded before the range check runs. */
#define SANITY_INVALID_U8  0xFFu
#define SANITY_INVALID_U16 0xFFFFu

static bool bpwr_implausible(const uint8_t *payload, uint8_t payload_len)
{
	uint16_t inst_power;

	if (payload_len < 8u ||
	    payload[0] != RADIANT_PROFILE_SANITY_PAGE_POWER_STD) {
		/* Every other bicycle power page (wheel torque, crank torque,
		 * torque-frequency) is an accumulator: a power figure needs
		 * the previous frame's reading, which this function is never
		 * handed. No opinion. */
		return false;
	}

	inst_power = (uint16_t)(payload[6] | ((uint16_t)payload[7] << 8));
	if (inst_power == SANITY_INVALID_U16) {
		return false;
	}

	return inst_power > RADIANT_PROFILE_SANITY_BPWR_MAX_WATTS;
}

static bool hr_implausible(const uint8_t *payload, uint8_t payload_len)
{
	uint8_t computed_hr;

	if (payload_len < 8u) {
		return false;
	}

	/* Computed heart rate sits at byte 7 on every ANT+ HRM data page -
	 * the field is common to all of them regardless of which page
	 * number byte 0 carries, which is why there is no page check here
	 * to match bpwr_implausible()'s. */
	computed_hr = payload[7];
	if (computed_hr == SANITY_INVALID_U8) {
		return false;
	}

	return computed_hr > RADIANT_PROFILE_SANITY_HR_MAX_BPM;
}

static bool fec_implausible(const uint8_t *payload, uint8_t payload_len)
{
	uint16_t speed_mm_s;

	if (payload_len < 8u ||
	    payload[0] != RADIANT_PROFILE_SANITY_PAGE_FEC_GENERAL) {
		/* Every other FE-C broadcast page is either an accumulator pair
		 * (19's vertical distances, 25's power) or a setting whose
		 * plausible range is its whole encodable range (17's incline).
		 * No opinion - see the header for why grade in particular is
		 * absent rather than forgotten. */
		return false;
	}

	speed_mm_s = (uint16_t)(payload[4] | ((uint16_t)payload[5] << 8));
	if (speed_mm_s == SANITY_INVALID_U16) {
		return false;
	}

	return speed_mm_s > RADIANT_PROFILE_SANITY_FEC_MAX_MM_S;
}

bool radiant_profile_sanity_implausible(uint8_t device_type,
					const uint8_t *payload,
					uint8_t payload_len)
{
	uint8_t devtype;

	if (payload == NULL || payload_len == 0u) {
		return false;
	}

	devtype = (uint8_t)(device_type & RADIANT_DEVICE_TYPE_MASK);

	switch (devtype) {
	case RADIANT_PROFILE_SANITY_DEVTYPE_BPWR:
		return bpwr_implausible(payload, payload_len);
	case RADIANT_PROFILE_SANITY_DEVTYPE_HR:
		return hr_implausible(payload, payload_len);
	case RADIANT_PROFILE_SANITY_DEVTYPE_FEC:
		return fec_implausible(payload, payload_len);
	default:
		return false;
	}
}
