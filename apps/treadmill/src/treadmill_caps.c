/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_caps.c - see treadmill_caps.h. Kernel-free.
 */

#include <radiant/radiant_ble_ftms.h>

#include "treadmill_caps.h"

/* The data fields the Treadmill Data record always carries
 * (treadmill_ftms_data.c), keyed to their feature bits. */
#define FMF_ALWAYS (RADIANT_BLE_FTMS_FMF_TOTAL_DISTANCE | RADIANT_BLE_FTMS_FMF_INCLINATION | \
		    RADIANT_BLE_FTMS_FMF_ELEVATION_GAIN | RADIANT_BLE_FTMS_FMF_ELAPSED_TIME)

void treadmill_caps_derive(const struct treadmill_source_caps *src,
			   const struct treadmill_caps_cfg *cfg,
			   struct treadmill_caps *out)
{
	*out = (struct treadmill_caps){ 0 };

	out->speed_cmd = src->set_speed && cfg->allow_speed;
	out->incline_cmd = src->set_incline && cfg->allow_incline;
	out->heart_rate = cfg->heart_rate;

	out->ftms_features = FMF_ALWAYS;
	if (out->heart_rate) {
		out->ftms_features |= RADIANT_BLE_FTMS_FMF_HEART_RATE;
	}
	if (out->speed_cmd) {
		out->ftms_targets |= RADIANT_BLE_FTMS_TSF_SPEED;
	}
	if (out->incline_cmd) {
		out->ftms_targets |= RADIANT_BLE_FTMS_TSF_INCLINATION;
	}
	out->fec_simulation = out->incline_cmd;
}
