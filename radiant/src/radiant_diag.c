/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_diag.c - storage for the 0xF7 diagnostic counters. See
 * radiant/include/radiant/radiant_diag.h. Built only with
 * CONFIG_RADIANT_DIAG_COUNTERS.
 *
 * Provenance: clean-room. See the header.
 */

#include <string.h>

#include <radiant/radiant_diag.h>

struct radiant_diag_core radiant_diag_core;

const struct radiant_diag_core *radiant_diag_core_get(void)
{
	return &radiant_diag_core;
}

void radiant_diag_core_reset(void)
{
	memset(&radiant_diag_core, 0, sizeof(radiant_diag_core));
}
