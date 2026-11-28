/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_caps.h - WHAT THE MACHINE ADVERTISES, derived from what its driver
 * can do.
 *
 * A capability bit is a claim that a command will be honoured: FTMS Target
 * Setting bit 0 with op code 0x02, bit 1 with 0x03 (§4.16.2 makes each pair
 * conditionally mandatory/excluded together), FE-C page 54's Simulation bit
 * with page 51. Declaring one and refusing its command is how a controller
 * decides a machine is broken. So the bits are computed, once at boot, from
 * which source entry points are non-NULL, and Kconfig can only take one away
 * (CONFIG_TREADMILL_CAP_*_TARGET). The FTMS Heart Rate feature (and the
 * Treadmill Data heart-rate field) exist only with an ANT+ strap receiver
 * (CONFIG_TREADMILL_HR_RX): without one the field could only ever be empty.
 *
 * Kernel-free.
 */

#ifndef TREADMILL_CAPS_H_
#define TREADMILL_CAPS_H_

#include <stdbool.h>
#include <stdint.h>

#include "treadmill_source.h"

#ifdef __cplusplus
extern "C" {
#endif

struct treadmill_caps_cfg {
	bool allow_speed;   /* CONFIG_TREADMILL_CAP_SPEED_TARGET */
	bool allow_incline; /* CONFIG_TREADMILL_CAP_INCLINE_TARGET */
	bool heart_rate;    /* CONFIG_TREADMILL_HR_RX */
};

struct treadmill_caps {
	bool speed_cmd;      /* FTMS 0x02 honoured */
	bool incline_cmd;    /* FTMS 0x03 and FE-C page 51 honoured */
	bool heart_rate;     /* HR feature bit and Treadmill Data field */
	uint32_t ftms_features; /* Fitness Machine Features */
	uint32_t ftms_targets;  /* Target Setting Features */
	bool fec_simulation; /* FE-C page 54 Simulation mode bit */
};

void treadmill_caps_derive(const struct treadmill_source_caps *src,
			   const struct treadmill_caps_cfg *cfg,
			   struct treadmill_caps *out);

#ifdef __cplusplus
}
#endif

#endif /* TREADMILL_CAPS_H_ */
