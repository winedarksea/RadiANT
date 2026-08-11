/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_chanmap.h - what the whole band sounds like, one RF index at a time.
 *
 * Provenance: original clean-room work, derived from this project's own RF plan
 * and from radiant_noise.h's existing binning. Nothing here derives from
 * sdk-ant, from libant.a, or from any adopter-gated ANT+ device profile
 * document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is for, and what it is not for YET
 * ---------------------------------------------------------------------------
 * The characterised ~0.4% loss floor on this bench is memoryless per-slot
 * collision at 2457 MHz, inside Wi-Fi channel 11. Nothing in the dongle can fix
 * that; moving off 2457 MHz can. Choosing where to move is adaptive frequency
 * selection, and a selection is only as good as the evidence behind it - so the
 * evidence comes first, and this is it.
 *
 * NOTHING READS THIS MAP TODAY. It is filled by energy-detect dwells and read
 * by the log, exactly as radiant_noise.h describes its own histogram, and for
 * the same reason: a diagnostic that also steers behaviour has to be right,
 * while one that only informs has to be honest. When adaptive frequency lands
 * it takes on the harder standard deliberately and with a measurement in hand.
 *
 * ---------------------------------------------------------------------------
 * ONE SCALE WITH radiant_noise.h, WHICH IS THE POINT OF SHARING THE BINNING
 * ---------------------------------------------------------------------------
 * There are now two ways this stack learns what a frequency sounds like:
 *
 *   OPPORTUNISTIC - radiant_noise.c, from receive windows that ended having
 *   heard nothing. Free, but only ever about the one or two frequencies the
 *   link layer already visits, and it can say nothing at all about a candidate.
 *
 *   DELIBERATE - this file, from radiant_radio_ed() dwells the scheduler fits
 *   into gaps. Costs radio time, and reaches anywhere in 0..124.
 *
 * They are measurements of the same physical quantity and they have to be
 * comparable, or the only cross-check either one has is gone: "the map says
 * RF 26 is 14 dB quieter than RF 57" is a claim about the difference between a
 * deliberate figure and an opportunistic one. So both bin on
 * radiant_noise_bin() - one definition, in radiant_noise.h, with the range and
 * the clamp rule stated once. A second copy would disagree by at most one bin,
 * which is exactly the size of error that never gets noticed.
 *
 * ---------------------------------------------------------------------------
 * Why min and mean rather than a histogram per index
 * ---------------------------------------------------------------------------
 * radiant_noise.c affords a full 64-bin histogram because it holds four
 * frequencies. This holds all 125, and 125 histograms is 16 KB on a part where
 * RAM is the budget being defended. So one entry is eight bytes and the whole
 * map is 1000 bytes, present only when CONFIG_RADIANT_CORE_ED_SCAN is on.
 *
 * What eight bytes buys is the same pair of numbers the histogram is read for:
 *
 *   floor_dbm - the quietest instant ever seen on this index, tracked as the
 *   minimum over every dwell's own minimum. This is radiant_noise's 10th
 *   percentile by another road, and it is the number that moves when a USB 3.0
 *   port desenses the receiver.
 *
 *   busy_dbm - the loudest dwell AVERAGE seen on this index, tracked as the
 *   maximum over every dwell's mean. Deliberately the max of the MEANS rather
 *   than of the mins: a single loud sample is a burst that a 250 ms channel
 *   period will usually step around, while a whole dwell that averaged loud is
 *   a transmitter that lives there. That is the distinction a frequency choice
 *   turns on, and taking the max of the maxima instead would rank every index
 *   by its worst microsecond.
 *
 * The gap between them is how bursty the index is, on the same reading as
 * radiant_noise's floor/busy pair.
 *
 * mean_dbm is the unweighted mean of the per-dwell means, NOT of the samples.
 * A dwell that managed three samples therefore counts as much as one that
 * managed thirty. That is stated rather than fixed because weighting needs a
 * 32-bit sample total per index - half the map again - to correct a bias that
 * only exists if a backend's sample count varies between dwells on the same
 * index, which no planned backend's does.
 */

#ifndef RADIANT_CHANMAP_H_
#define RADIANT_CHANMAP_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant_core/radiant_radio_hal.h>

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
	/* Energy-detect dwells aggregated into this entry. The evidence count,
	 * at the resolution the map keeps; each dwell is itself a min and a
	 * mean over radiant_ed_event.samples RSSI readings. */
	uint16_t dwells;

	/* See the header: the minimum over per-dwell minima, and the maximum
	 * over per-dwell means. */
	int8_t floor_dbm;
	int8_t busy_dbm;

	/* The unweighted mean of the per-dwell means. */
	int8_t mean_dbm;
};

#if defined(CONFIG_RADIANT_CORE_ED_SCAN)

/* Forget everything. Called from radiant_sched_init(), on the same terms as
 * radiant_noise_reset(): the map belongs to the samples that module feeds it,
 * so a stale map from before a reset is never reported as this session's. */
void radiant_chanmap_reset(void);

/*
 * One energy-detect dwell.
 *
 * Callable from the radio callback context - it is a bounds check, an array
 * index and a handful of integer operations, with no loop over anything.
 *
 * samples is carried for the caller's own accounting and for the refusal
 * below; the map does not weight by it. A dwell reporting zero samples is
 * DROPPED, because an event claiming a measurement it did not make is worse
 * than a gap in the map.
 */
void radiant_chanmap_note(uint8_t rf_index, int8_t min_dbm, int8_t mean_dbm,
			  uint16_t samples);

/*
 * Read one index. False if `out` is NULL, if rf_index is out of range, or if
 * that index has fewer than RADIANT_CHANMAP_MIN_DWELLS dwells behind it -
 * which is the honest answer for "not measured yet" and is distinct from
 * "measured and quiet".
 */
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

#else /* !CONFIG_RADIANT_CORE_ED_SCAN */

/*
 * No-op inlines rather than an absent symbol, so that radiant_sched.c has one
 * call site rather than a call site and an #ifdef around it. The same
 * zero-cost discipline the security modules keep: nothing is compiled, nothing
 * is linked, and the CI job that asserts it reads section sizes rather than
 * this comment.
 */
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

#endif /* CONFIG_RADIANT_CORE_ED_SCAN */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CHANMAP_H_ */
