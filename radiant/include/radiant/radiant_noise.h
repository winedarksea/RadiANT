/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_noise.h - what the band sounds like when nothing is transmitting.
 *
 * Provenance: original clean-room work.
 *
 * Exists to make USB 3.0 broadband desense visible: routinely 10-20 dB,
 * port-dependent, and today indistinguishable from "sensor battery is
 * flat" (both just look like "it does not pair any more"). A number
 * settles that in one look.
 *
 * Samples come ONLY from windows that timed out having received nothing -
 * that population IS the noise floor, and the samples are free (no new
 * radio operation, no scheduler slot; these windows open anyway). A window
 * that received a packet contributes nothing - its RSSI is the
 * transmitter's, the opposite measurement.
 *
 * It changes nothing: no decision in the core reads these numbers, they go
 * to the log and stop. That's the condition it was worth building under -
 * a diagnostic that also steers behaviour has to be right, one that only
 * informs only has to be honest.
 */

#ifndef RADIANT_NOISE_H_
#define RADIANT_NOISE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The histogram's range, in dBm, inclusive at both ends. -110 is below any
 * part's stated noise floor; -47 is far above a quiet band and well below
 * a nearby transmitter, so a sample clipping the top is interference by
 * definition. 64 bins of 1 dB, RSSI's reported resolution.
 *
 * Out-of-range samples are counted at the edges AND counted separately, so
 * a distribution piled against a wall is visible as such.
 */
#define RADIANT_NOISE_DBM_MIN (-110)
#define RADIANT_NOISE_DBM_MAX (-47)
#define RADIANT_NOISE_BINS \
	((uint8_t)(RADIANT_NOISE_DBM_MAX - RADIANT_NOISE_DBM_MIN + 1))

/*
 * THE ONE DEFINITION OF THE 1 dB BINNING - inline here rather than private
 * to radiant_noise.c because radiant_chanmap.c shares it. Both files
 * measure the same physical quantity for different reasons ("RF 26 is
 * 14 dB quieter than 57" is only a statement if both are binned the same
 * way), and two copies of an edge rule would disagree by up to a bin,
 * unnoticed.
 *
 * Clamping rather than rejecting matches radiant_noise_note(): an
 * out-of-range sample is counted at the edge AND separately by the caller.
 */
static inline uint8_t radiant_noise_bin(int8_t dbm)
{
	int idx = (int)dbm - RADIANT_NOISE_DBM_MIN;

	if (idx < 0) {
		return 0u;
	}
	if (idx >= (int)RADIANT_NOISE_BINS) {
		return (uint8_t)(RADIANT_NOISE_BINS - 1u);
	}
	return (uint8_t)idx;
}

/* The dBm a bin index names - the inverse of radiant_noise_bin() inside the
 * range, and the range's edge for a clamped bin. */
static inline int8_t radiant_noise_bin_dbm(uint8_t bin)
{
	if (bin >= RADIANT_NOISE_BINS) {
		return (int8_t)RADIANT_NOISE_DBM_MAX;
	}
	return (int8_t)(RADIANT_NOISE_DBM_MIN + (int)bin);
}

/*
 * How many frequencies are tracked at once, per rf_index (the point is
 * telling one frequency from another - 20 dB more noise on 2457 than 2402
 * names the Wi-Fi channel). Four rather than 125, since a histogram per
 * index would be 16 KB and a running dongle uses one ANT+ frequency plus
 * at most a couple more. A fifth frequency's samples are DROPPED and
 * counted, never folded into somebody else's histogram.
 */
#define RADIANT_NOISE_SLOTS 4u

struct radiant_noise_report {
	uint8_t  rf_index;
	uint32_t samples;

	/* The 10th percentile: the floor. Moves when a USB 3.0 port
	 * desenses the receiver - the number to compare between ports. */
	int8_t floor_dbm;
	/* The 90th percentile: interference. Gap to the floor is how
	 * bursty the band is - close together in a quiet room, apart
	 * with Wi-Fi on the same channel. */
	int8_t busy_dbm;

	int8_t min_dbm;
	int8_t max_dbm;

	/* Samples that fell outside the histogram's range and were counted at
	 * its edges. Non-zero at the low end usually means a part quieter than
	 * this range assumes; at the high end it means something was
	 * transmitting. */
	uint32_t below_range;
	uint32_t above_range;
};

/* Forget everything. Called at init and from a stack reset. */
void radiant_noise_reset(void);

/* One sample, from a window on `rf_index` that ended having received
 * nothing. Callable from radio callback context: a bounds check, an array
 * index, two increments, no loops. */
void radiant_noise_note(uint8_t rf_index, int8_t dbm);

/* Read slot `slot` (0 .. RADIANT_NOISE_SLOTS-1). False if never used, `out`
 * is NULL, or too few samples for a percentile to mean anything. */
bool radiant_noise_get(uint8_t slot, struct radiant_noise_report *out);

/*
 * Start a fresh interval for `slot`, keeping its frequency. Intended shape
 * is report-then-clear, so each log line describes the interval since the
 * last one - a histogram that never clears converges and stops responding,
 * defeating the point of watching a number MOVE across a port change.
 */
void radiant_noise_clear(uint8_t slot);

/* Samples dropped because every slot was taken by another frequency. Reported
 * rather than hidden: it is the only way to know a reading is incomplete. */
uint32_t radiant_noise_unslotted(void);

/* How many samples a slot needs before radiant_noise_get() will report a
 * percentile from it. Below this the 10th percentile is one or two samples and
 * says nothing about a distribution. */
#define RADIANT_NOISE_MIN_SAMPLES 20u

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_NOISE_H_ */
