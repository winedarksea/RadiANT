/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_chanmap.h - what the whole band sounds like, one RF index at a time.
 *
 * Provenance: original clean-room work, derived from this project's own RF
 * plan and from radiant_noise.h's existing binning.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * The characterised ~0.4% loss floor on this bench is memoryless per-slot
 * collision at 2457 MHz (Wi-Fi channel 11); moving off it needs adaptive
 * frequency selection, and a selection needs evidence first. NOTHING READS
 * THIS MAP TODAY - it's filled by energy-detect dwells and read only by the
 * log, on the same "diagnostics must be honest, not just steer behaviour"
 * principle as radiant_noise.h.
 *
 * Shares radiant_noise_bin()'s binning with radiant_noise.h on purpose: this
 * file's deliberate ED dwells (radiant_radio_ed(), reaches any of 0..124) and
 * that file's opportunistic idle-window samples (free, but only the one or
 * two frequencies already in use) measure the same quantity and must be
 * comparable - a second binning would disagree by up to a bin, unnoticed.
 *
 * One entry is 8 bytes (min/mean per index, not a full histogram - 125
 * histograms would be 16 KB), for a 1000-byte map, present only under
 * CONFIG_RADIANT_ED_SCAN. floor_dbm is the min over per-dwell minima
 * (radiant_noise's 10th-percentile equivalent). busy_dbm is deliberately the
 * MAX OF PER-DWELL MEANS, not of minima: a single loud sample is a burst a
 * 250 ms period steps around, while a dwell that averaged loud is a resident
 * transmitter - the distinction a frequency choice actually turns on.
 * mean_dbm is the unweighted mean of per-dwell means (not of samples), so a
 * 3-sample dwell counts as much as a 30-sample one; not corrected because no
 * planned backend's sample count varies between dwells on the same index.
 */

#ifndef RADIANT_CHANMAP_H_
#define RADIANT_CHANMAP_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_radio_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every RF index the HAL can express: 0..124, meaning 2400..2524 MHz. The whole
 * range rather than a candidate set, because the descriptor carries a full
 * index and a receiver that could only measure three of them could not tell a
 * quiet fourth from an unmeasured one. */
#define RADIANT_CHANMAP_INDICES ((uint16_t)RADIANT_RF_INDEX_MAX + 1u)

/* How many dwells an index needs before radiant_chanmap_get() will report it.
 * Below this the floor is one dwell's luck. Deliberately far smaller than
 * RADIANT_NOISE_MIN_SAMPLES, because a dwell is itself an average over many
 * samples where a noise sample is one reading. */
#define RADIANT_CHANMAP_MIN_DWELLS 4u

struct radiant_chanmap_report {
	uint8_t  rf_index;
	/* Energy-detect dwells aggregated into this entry; each dwell is a
	 * min/mean over radiant_ed_event.samples RSSI readings. */
	uint16_t dwells;

	/* See the header: min over per-dwell minima, max over per-dwell means. */
	int8_t floor_dbm;
	int8_t busy_dbm;

	/* Unweighted mean of the per-dwell means. */
	int8_t mean_dbm;
};

#if defined(CONFIG_RADIANT_ED_SCAN)

/* Forget everything. Called from radiant_sched_init(), on the same terms as
 * radiant_noise_reset(): the map belongs to the samples that module feeds it,
 * so a stale map from before a reset is never reported as this session's. */
void radiant_chanmap_reset(void);

/*
 * One energy-detect dwell. Callable from radio callback context (bounds
 * check, array index, no loops). samples does not weight the map, it's only
 * for the caller's accounting and the refusal below: a dwell reporting zero
 * samples is DROPPED, since a measurement claim with no measurement is worse
 * than a gap.
 */
void radiant_chanmap_note(uint8_t rf_index, int8_t min_dbm, int8_t mean_dbm,
			  uint16_t samples);

/* Read one index. False if `out` is NULL, rf_index is out of range, or the
 * index has fewer than RADIANT_CHANMAP_MIN_DWELLS dwells - "not measured
 * yet", distinct from "measured and quiet". */
bool radiant_chanmap_get(uint8_t rf_index, struct radiant_chanmap_report *out);

/* Start a fresh interval for one index, keeping nothing. The intended shape is
 * report-then-clear, as radiant_noise_clear()'s header describes: a map that
 * never clears converges and stops responding, and the whole use of it is
 * watching an index MOVE when the room changes. */
void radiant_chanmap_clear(uint8_t rf_index);

/* Dwells that fell outside 0..RADIANT_RF_INDEX_MAX, or reported no samples,
 * and were dropped. Reported rather than hidden: it is the only way to know a
 * map is incomplete for a reason other than airtime. */
uint32_t radiant_chanmap_dropped(void);

#else /* !CONFIG_RADIANT_ED_SCAN */

/* No-op inlines rather than an absent symbol, so radiant_sched.c has one call
 * site instead of an #ifdef. Nothing is compiled or linked. */
static inline void radiant_chanmap_reset(void) { }

static inline void radiant_chanmap_note(uint8_t rf_index, int8_t min_dbm,
					int8_t mean_dbm, uint16_t samples)
{
	(void)rf_index;
	(void)min_dbm;
	(void)mean_dbm;
	(void)samples;
}

static inline bool radiant_chanmap_get(uint8_t rf_index,
				       struct radiant_chanmap_report *out)
{
	(void)rf_index;
	(void)out;
	return false;
}

static inline void radiant_chanmap_clear(uint8_t rf_index) { (void)rf_index; }

static inline uint32_t radiant_chanmap_dropped(void) { return 0u; }

#endif /* CONFIG_RADIANT_ED_SCAN */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CHANMAP_H_ */
