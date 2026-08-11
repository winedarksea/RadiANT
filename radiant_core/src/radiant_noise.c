/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_noise.c - the noise-floor histogram.
 *
 * Provenance: original clean-room work. See radiant_noise.h for what this is
 * for and where the samples come from.
 *
 * This file includes no Zephyr header and no HAL entry point, on the same terms
 * as radiant_channel.c. Its gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -fsyntax-only \
 *       radiant_core/src/radiant_noise.c
 *
 * Integer only and no division in the hot path: radiant_noise_note() runs in
 * the radio's interrupt context, once per window that heard nothing, which on a
 * searching dongle is several times a second.
 */

#include <string.h>

#include <radiant_core/radiant_noise.h>

struct noise_slot {
	uint16_t bins[RADIANT_NOISE_BINS];
	uint32_t samples;
	uint32_t below_range;
	uint32_t above_range;
	int8_t   min_dbm;
	int8_t   max_dbm;
	uint8_t  rf_index;
	bool     used;
};

static struct noise_slot noise[RADIANT_NOISE_SLOTS];
static uint32_t noise_unslotted;

void radiant_noise_reset(void)
{
	memset(noise, 0, sizeof(noise));
	noise_unslotted = 0u;
}

static struct noise_slot *slot_for(uint8_t rf_index)
{
	uint8_t i;

	for (i = 0u; i < RADIANT_NOISE_SLOTS; i++) {
		if (noise[i].used && noise[i].rf_index == rf_index) {
			return &noise[i];
		}
	}
	for (i = 0u; i < RADIANT_NOISE_SLOTS; i++) {
		if (!noise[i].used) {
			memset(&noise[i], 0, sizeof(noise[i]));
			noise[i].used = true;
			noise[i].rf_index = rf_index;
			noise[i].min_dbm = 0;
			noise[i].max_dbm = -128;
			return &noise[i];
		}
	}

	/*
	 * Every slot belongs to another frequency.
	 *
	 * Dropped rather than evicted, and counted rather than dropped
	 * silently. Evicting would let a frequency the dongle visits once
	 * destroy the record of the one it lives on; folding the sample into
	 * somebody else's histogram would produce a number that is a mixture of
	 * two bands and looks exactly like a single noisy one.
	 */
	return NULL;
}

void radiant_noise_note(uint8_t rf_index, int8_t dbm)
{
	struct noise_slot *s = slot_for(rf_index);
	int     raw;
	uint8_t index;

	if (s == NULL) {
		noise_unslotted++;
		return;
	}

	if (s->samples == 0u || dbm < s->min_dbm) {
		s->min_dbm = dbm;
	}
	if (s->samples == 0u || dbm > s->max_dbm) {
		s->max_dbm = dbm;
	}

	/*
	 * The out-of-range test is here and the clamp is in radiant_noise_bin().
	 * They used to be one expression, and splitting them is what lets
	 * radiant_chanmap.c share the binning without also inheriting these two
	 * counters, which are this module's own accounting and mean nothing to
	 * a map that keeps no distribution.
	 *
	 * Counted at the edge as well as separately: a percentile computed with
	 * the clipped samples missing would be computed over a different
	 * population than the one `samples` names.
	 */
	raw = (int)dbm - RADIANT_NOISE_DBM_MIN;
	if (raw < 0) {
		s->below_range++;
	} else if (raw >= (int)RADIANT_NOISE_BINS) {
		s->above_range++;
	}
	index = radiant_noise_bin(dbm);

	if (s->bins[index] < 0xFFFFu) {
		s->bins[index]++;
	}
	s->samples++;
}

/*
 * The value at the p-th percentile, walking up from the quiet end.
 *
 * Bins are ordered from most negative dBm to least, so walking from index 0
 * walks from quietest to loudest and a low percentile is the floor. Rank is
 * the (1-based) sample this percentile names, rounded up so that p10 of ten
 * samples is the first one rather than the zeroth.
 */
static int8_t percentile(const struct noise_slot *s, uint32_t p)
{
	uint32_t rank = ((s->samples * p) + 99u) / 100u;
	uint32_t seen = 0u;
	uint8_t i;

	if (rank == 0u) {
		rank = 1u;
	}
	for (i = 0u; i < RADIANT_NOISE_BINS; i++) {
		seen += s->bins[i];
		if (seen >= rank) {
			return radiant_noise_bin_dbm(i);
		}
	}
	/* Unreachable while samples equals the sum of the bins, which it does
	 * by construction - every sample above increments exactly one bin. The
	 * top of the range is the least wrong answer if that ever stops being
	 * true. */
	return (int8_t)RADIANT_NOISE_DBM_MAX;
}

bool radiant_noise_get(uint8_t slot, struct radiant_noise_report *out)
{
	const struct noise_slot *s;

	if (slot >= RADIANT_NOISE_SLOTS || out == NULL) {
		return false;
	}
	s = &noise[slot];
	if (!s->used || s->samples < RADIANT_NOISE_MIN_SAMPLES) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	out->rf_index = s->rf_index;
	out->samples = s->samples;
	out->floor_dbm = percentile(s, 10u);
	out->busy_dbm = percentile(s, 90u);
	out->min_dbm = s->min_dbm;
	out->max_dbm = s->max_dbm;
	out->below_range = s->below_range;
	out->above_range = s->above_range;
	return true;
}

void radiant_noise_clear(uint8_t slot)
{
	uint8_t rf_index;

	if (slot >= RADIANT_NOISE_SLOTS || !noise[slot].used) {
		return;
	}

	/* The frequency survives the clear; the distribution does not. A slot
	 * that gave up its rf_index here would be reallocated to whichever
	 * frequency spoke next, and the log line's identity would wander. */
	rf_index = noise[slot].rf_index;
	memset(&noise[slot], 0, sizeof(noise[slot]));
	noise[slot].used = true;
	noise[slot].rf_index = rf_index;
}

uint32_t radiant_noise_unslotted(void)
{
	return noise_unslotted;
}
