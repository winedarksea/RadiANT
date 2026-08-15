/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_power_decode.c - ANT+ Bicycle Power page 0x10, receive side.
 *
 * Provenance: D00001086 Rev 5.1 Table 8-1 "Power-Only Message Format", plus
 * section 8.2.1 (pedal differentiation, bit 7), 8.2.2 (pedal percent, bits
 * 0-6), 8.3 (cadence 0xFF = not reported) and 8.4 (accumulated power is "the
 * running sum of the instantaneous power data"). Read directly from the
 * primary rather than inferred from profile_power_encode_std() next door - the
 * two are checked against each other by test_power_adapter.c's round-trip
 * case, and that is only a check if they were written from the same source
 * independently.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS ITS OWN TRANSLATION UNIT AND NOT THREE MORE LINES IN
 * profile_power.c
 * ---------------------------------------------------------------------------
 *
 * It was three more lines in profile_power.c, and that file cannot be linked
 * into a receiver.
 *
 * profile_power.c is the MASTER: the page rotation, the ANT+ compatibility
 * layer and the RadiANT beacon insertion, so it calls into profile_sched.c and
 * profile_compat.c, and profile_compat.c reaches radiant_sec. Linking it into
 * apps/dongle_thread to get one decoder would drag the whole transmit-side
 * chain - a scheduler, a compat client and the security core - into an image
 * whose entire job is to listen. That is not a size argument (though it is
 * also that): a receive-only dongle that links a power-meter TRANSMITTER is a
 * dongle that can be made to transmit by a linker decision nobody reviewed.
 *
 * The header is shared and that is fine: profile_power.h pulls in
 * profile_compat.h and profile_sched.h, but a declaration costs no symbol.
 * Only .c files create link dependencies, which is the whole point of the
 * split.
 *
 * So: this file includes profile_power.h and nothing else, defines exactly one
 * function, and is the file a receiver lists. If a second decoder is added for
 * the torque pages (0x11/0x12) it belongs here beside this one, NOT back in
 * profile_power.c.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile_power.h"

int profile_power_decode_std(const uint8_t *body, struct profile_power_std *out)
{
	uint8_t pedal;

	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_POWER_PAGE_STANDARD) {
		/* Declining rather than masking: the torque pages put entirely
		 * different quantities in bytes 4-7 and would decode here as a
		 * plausible power figure. */
		return -EINVAL;
	}

	out->event_count = body[1];
	out->acc_power_w = (uint16_t)body[4] | ((uint16_t)body[5] << 8);
	out->inst_power_w = (uint16_t)body[6] | ((uint16_t)body[7] << 8);

	out->cadence_rpm = body[3];
	out->cadence_valid = (body[3] != PROFILE_POWER_INVALID_CADENCE);

	pedal = body[2];
	out->pedal_valid = (pedal != PROFILE_POWER_INVALID_PEDAL_POWER);
	/* Split unconditionally, reported as meaningful only when valid: 0xFF
	 * masks to 127 % with the right-pedal bit set, which is a legal-looking
	 * pair that a caller ignoring pedal_valid would happily publish. */
	out->pedal_right_known = (pedal & PROFILE_POWER_PEDAL_RIGHT_BIT) != 0u;
	out->pedal_percent = (uint8_t)(pedal & PROFILE_POWER_PEDAL_PERCENT_MASK);

	return 0;
}
