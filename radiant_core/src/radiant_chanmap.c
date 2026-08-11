/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_chanmap.c - the channel-quality map.
 *
 * Provenance: original clean-room work, derived from this project's own RF plan
 * and from radiant_noise.c's existing 1 dB binning, which it shares rather than
 * repeats. Nothing here derives from sdk-ant, from libant.a, or from any
 * adopter-gated ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * See radiant_chanmap.h for what this is for, why an entry is eight bytes, and
 * why floor and busy are taken from different statistics.
 *
 * This file includes no Zephyr header and no HAL entry point, on the same terms
 * as radiant_noise.c and radiant_channel.c. Its gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -fsyntax-only -DCONFIG_RADIANT_CORE_ED_SCAN=1 \
 *       radiant_core/src/radiant_chanmap.c
 *
 * Integer only and no division in radiant_chanmap_note(): it runs in the radio
 * interrupt, once per energy-detect dwell, and a sweep produces one of those
 * every few hundred microseconds. The one division in the file is in
 * radiant_chanmap_get(), which runs from a log line in thread context.
 */

#include <string.h>

#include <radiant_core/radiant_chanmap.h>

#if defined(CONFIG_RADIANT_CORE_ED_SCAN)

#include <radiant_core/radiant_noise.h>

/*
 * Eight bytes, and the layout is the size.
 *
 * mean_sum accumulates BIN INDICES rather than dBm, which is what keeps it
 * unsigned and keeps the mean's arithmetic away from the sign of a negative
 * eighth-bit value. 64 bins at a uint16 dwell ceiling is 4 194 240, comfortably
 * inside 32 bits, so the sum cannot wrap before the dwell counter saturates.
 *
 * `dwells == 0` IS THE "NEVER SAMPLED" TEST, for min_bin as well as for
 * max_bin, rather than a sentinel value in min_bin itself. A sentinel would
 * have to be written by radiant_chanmap_reset() and by radiant_chanmap_clear()
 * and would be absent from the static zero-initialisation that precedes both -
 * and bin 0 is the QUIETEST bin, so an entry that missed the sentinel would
 * report a floor of RADIANT_NOISE_DBM_MIN and never move off it. That is the
 * most misleading possible default for a map whose whole job is finding quiet,
 * and it would depend on an initialisation order rather than on the data.
 */
struct chanmap_entry {
	uint32_t mean_sum;
	uint16_t dwells;
	uint8_t  min_bin;
	uint8_t  max_bin;
};

static struct chanmap_entry chanmap[RADIANT_CHANMAP_INDICES];
static uint32_t chanmap_dropped_n;

void radiant_chanmap_reset(void)
{
	memset(chanmap, 0, sizeof(chanmap));
	chanmap_dropped_n = 0u;
}

void radiant_chanmap_note(uint8_t rf_index, int8_t min_dbm, int8_t mean_dbm,
			  uint16_t samples)
{
	struct chanmap_entry *e;
	uint8_t min_bin;
	uint8_t mean_bin;

	if (rf_index >= RADIANT_CHANMAP_INDICES || samples == 0u) {
		/*
		 * A dwell with no samples is not a quiet index, and folding it
		 * in as one is the specific way this map could lie in the
		 * direction that matters: adaptive frequency selection would
		 * pick the frequency nothing could be measured on.
		 */
		chanmap_dropped_n++;
		return;
	}

	e = &chanmap[rf_index];

	/* Saturate rather than wrap. A wrapped dwell counter would take an
	 * index that has been measured for an hour back below
	 * RADIANT_CHANMAP_MIN_DWELLS and report it as never measured. */
	if (e->dwells == 0xFFFFu) {
		return;
	}

	min_bin = radiant_noise_bin(min_dbm);
	mean_bin = radiant_noise_bin(mean_dbm);

	if (e->dwells == 0u || min_bin < e->min_bin) {
		e->min_bin = min_bin;
	}
	if (e->dwells == 0u || mean_bin > e->max_bin) {
		e->max_bin = mean_bin;
	}
	e->mean_sum += (uint32_t)mean_bin;
	e->dwells++;
}

bool radiant_chanmap_get(uint8_t rf_index, struct radiant_chanmap_report *out)
{
	const struct chanmap_entry *e;

	if (out == NULL || rf_index >= RADIANT_CHANMAP_INDICES) {
		return false;
	}
	e = &chanmap[rf_index];
	if (e->dwells < RADIANT_CHANMAP_MIN_DWELLS) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	out->rf_index = rf_index;
	out->dwells = e->dwells;
	out->floor_dbm = radiant_noise_bin_dbm(e->min_bin);
	out->busy_dbm = radiant_noise_bin_dbm(e->max_bin);
	/* Rounded down, which is toward the quiet end of the scale. The bias is
	 * half a dB and it is in the pessimistic direction for a floor and the
	 * optimistic one for a busy figure; saying so is cheaper than a
	 * rounding rule that has to be right in both. */
	out->mean_dbm = radiant_noise_bin_dbm(
		(uint8_t)(e->mean_sum / (uint32_t)e->dwells));
	return true;
}

void radiant_chanmap_clear(uint8_t rf_index)
{
	if (rf_index >= RADIANT_CHANMAP_INDICES) {
		return;
	}
	memset(&chanmap[rf_index], 0, sizeof(chanmap[rf_index]));
}

uint32_t radiant_chanmap_dropped(void)
{
	return chanmap_dropped_n;
}

#endif /* CONFIG_RADIANT_CORE_ED_SCAN */
