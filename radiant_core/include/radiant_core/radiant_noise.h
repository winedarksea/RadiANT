/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_noise.h - what the band sounds like when nothing is transmitting.
 *
 * Provenance: original clean-room work. Nothing here derives from sdk-ant, from
 * libant.a, or from any adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * The failure this exists to make visible
 * ---------------------------------------------------------------------------
 * The most common real-world 2.4 GHz dongle failure that is not in
 * docs/gotchas.md is USB 3.0 broadband noise desensing the receiver. It is
 * routinely 10-20 dB, it is entirely a property of which port the dongle is in,
 * and today it is invisible: a receiver 15 dB deaf and a sensor with a flat
 * battery produce the same symptom, which is "it does not pair any more".
 *
 * A number would settle that in one look. This is the number.
 *
 * ---------------------------------------------------------------------------
 * Where the samples come from, and why they are trustworthy
 * ---------------------------------------------------------------------------
 * ONLY from windows that ended in a timeout having received nothing. That
 * population IS the noise floor - it is the radio listening to a frequency at a
 * moment when, by observation, nothing was transmitting on it. No new radio
 * operation is needed and no scheduler slot: these are windows the core was
 * going to open anyway, and the samples are free.
 *
 * A window that received a packet contributes nothing, and must not: its RSSI
 * is the transmitter's, which is the opposite measurement.
 *
 * ---------------------------------------------------------------------------
 * What it does NOT do
 * ---------------------------------------------------------------------------
 * It does not change anything. No decision anywhere in the core reads these
 * numbers - they go to the log and stop there. That is the condition on which
 * this was worth building at all: a diagnostic that also steers behaviour is a
 * diagnostic that has to be right, and this one only has to be informative.
 */

#ifndef RADIANT_NOISE_H_
#define RADIANT_NOISE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The histogram's range, in dBm, inclusive at both ends.
 *
 * -110 is below any part's stated noise floor; -47 is far above anything a
 * quiet band produces and well below the level a nearby transmitter reaches, so
 * a sample that clips the top is interference by definition rather than by
 * threshold. 64 bins of 1 dB, which is the resolution RSSI is reported at.
 *
 * Samples outside the range are counted at the edges AND counted separately, so
 * a distribution that has piled up against a wall is visible as such rather
 * than as a percentile that quietly stopped moving.
 */
#define RADIANT_NOISE_DBM_MIN (-110)
#define RADIANT_NOISE_DBM_MAX (-47)
#define RADIANT_NOISE_BINS \
	((uint8_t)(RADIANT_NOISE_DBM_MAX - RADIANT_NOISE_DBM_MIN + 1))

/*
 * How many frequencies are tracked at once.
 *
 * Per rf_index, because the whole point is to tell one frequency from another -
 * a dongle that hears 20 dB more noise on 2457 MHz than on 2402 is being told
 * exactly which Wi-Fi channel it is sitting under. Four rather than 125,
 * because a histogram per possible RF index would be 16 KB of RAM on a part
 * where RAM is the budget being defended, and a running dongle uses one
 * frequency for ANT+ and at most a couple more.
 *
 * A fifth frequency's samples are DROPPED and counted, never silently folded
 * into somebody else's histogram.
 */
#define RADIANT_NOISE_SLOTS 4u

struct radiant_noise_report {
	uint8_t  rf_index;
	uint32_t samples;

	/*
	 * The 10th percentile: the floor. What the band sounds like at its
	 * quietest, which is the number that moves when a USB 3.0 port desenses
	 * the receiver and the number to compare between two ports.
	 */
	int8_t floor_dbm;
	/*
	 * The 90th percentile: the interference. The gap between this and the
	 * floor is how bursty the band is - a quiet room has them within a few
	 * dB of each other, and a room with Wi-Fi on the same channel does not.
	 */
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

/*
 * One sample, from a window on `rf_index` that ended having received nothing.
 *
 * Callable from the radio callback context: it is a bounds check, an array
 * index and two increments, with no loop over anything.
 */
void radiant_noise_note(uint8_t rf_index, int8_t dbm);

/*
 * Read slot `slot` (0 .. RADIANT_NOISE_SLOTS-1). False if that slot has never
 * been used, or if `out` is NULL, or if there are too few samples for a
 * percentile to mean anything.
 */
bool radiant_noise_get(uint8_t slot, struct radiant_noise_report *out);

/*
 * Start a fresh interval for `slot`, keeping its frequency.
 *
 * The intended shape is report-then-clear, so each log line describes the
 * interval since the last one rather than everything since boot. A histogram
 * that never clears converges and stops responding, and the whole use of this
 * is watching a number MOVE when the dongle is plugged into a different port.
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
