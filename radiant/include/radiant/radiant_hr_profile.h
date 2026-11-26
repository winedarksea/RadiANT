/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_hr_profile.h - heart-rate personalisation: docs/radiant-bridge.md
 * sections 6.2-6.6. The Karvonen threshold maths, the per-strap histogram
 * estimator, and a capped table of histogram slots. No threads and no
 * storage of its own: persistence is an application's, through the
 * radiant_hr_profile_store seam below, the way radiant_binding.h leaves
 * binding persistence to the application.
 *
 * THE PRIOR IS n = 0 OF THE SAME PATH (section 6.3). A binding with no slot,
 * a slot with no samples, and a build with zero slots all compute their
 * thresholds through radiant_hr_thresholds_compute() with an empty histogram,
 * which returns exactly the prior's thresholds. There is no second
 * implementation to drift.
 *
 * The estimator, in integer form:
 *
 *   obs_max  = p99(histogram), obs_rest = p01(histogram)  (interpolated)
 *   est      = (prior * N0 + obs * N) / (N0 + N)
 *
 * N is the histogram total in 1 Hz samples and N0 is
 * CONFIG_RADIANT_BRIDGE_HR_PRIOR_WEIGHT_S (default 1800, i.e. half an hour of
 * wear counts as much as the population prior). That is section 6.3's
 * conjugate normal update with the variance ratio expressed as a sample
 * count. There is no confidence gate (section 6.2).
 *
 * Two additions the doc does not have: the estimates are clamped to HR_max
 * 140-220 and HR_rest 35-100 (so HRR >= 40), so a mis-reading strap cannot
 * invert or collapse the zones.
 *
 * Section 6.6's bias is accepted, not corrected: a strap is worn during
 * exercise, so p01 is "calm during a session" and reads high by perhaps
 * 10-20 bpm. That is what an automation wants from "at rest" anyway.
 */

#ifndef RADIANT_HR_PROFILE_H_
#define RADIANT_HR_PROFILE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Section 6.3's table: 30-220 bpm in 5 bpm bins, 38 of them, uint16. */
#define RADIANT_HR_HIST_MIN_BPM 30u
#define RADIANT_HR_HIST_MAX_BPM 220u /* exclusive */
#define RADIANT_HR_HIST_BIN_BPM 5u
#define RADIANT_HR_HIST_BINS \
	((RADIANT_HR_HIST_MAX_BPM - RADIANT_HR_HIST_MIN_BPM) / RADIANT_HR_HIST_BIN_BPM)

/* The clamps on the blended estimates (bpm). */
#define RADIANT_HR_MAX_FLOOR  140u
#define RADIANT_HR_MAX_CEIL   220u
#define RADIANT_HR_REST_FLOOR 35u
#define RADIANT_HR_REST_CEIL  100u

/* Write gating (section 6.5 plus this implementation's wear guard). A save
 * needs at least this many new 1 Hz samples since the last save (5 min of
 * wear), and one slot saves at most RADIANT_HR_PROFILE_SAVES_PER_BOOT times
 * per boot, so a spoofed strap that toggles worn cannot drive flash wear. */
#define RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES 300u
#define RADIANT_HR_PROFILE_SAVES_PER_BOOT   16u

struct radiant_hr_hist {
	uint16_t bin[RADIANT_HR_HIST_BINS];
};

/* The persisted record. version 1; 78 bytes, no padding. */
#define RADIANT_HR_PROFILE_RECORD_VERSION 1u
struct radiant_hr_profile_record {
	uint8_t                version;
	uint8_t                reserved;
	struct radiant_hr_hist hist;
};

/* The three thresholds a zone rule compares against, plus the estimates
 * they came from (all bpm). */
struct radiant_hr_thresholds {
	uint8_t hr_max;     /* blended, clamped estimate (rounded) */
	uint8_t hr_rest;    /* blended, clamped estimate (rounded) */
	uint8_t rest_below; /* at rest:  hr <  rest_below */
	uint8_t zone2_at;   /* zone 2+:  hr >= zone2_at */
	uint8_t zone3_at;   /* zone 3+:  hr >= zone3_at */
};

/* ---- pure estimator ---------------------------------------------------- */

/* Add one sample. Readings outside 30..219 bpm are dropped (the histogram
 * covers nothing else, and a 240 bpm artifact is exactly what it is for).
 * When a bin saturates, every bin is halved: section 6.3's forgetting. */
void radiant_hr_hist_add(struct radiant_hr_hist *h, uint32_t bpm);

/* Sum of every bin. */
uint32_t radiant_hr_hist_total(const struct radiant_hr_hist *h);

/* The `permille`-th quantile (10 = p01, 990 = p99), in tenths of a bpm,
 * linearly interpolated inside the bin. 0 for an empty histogram. */
uint32_t radiant_hr_hist_quantile_x10(const struct radiant_hr_hist *h,
				      uint32_t permille);

/* Blend `h` (NULL = empty) into the prior and derive the thresholds. With
 * an empty histogram the result is the prior's, exactly. */
void radiant_hr_thresholds_compute(const struct radiant_hr_hist *h,
				   uint32_t prior_max_bpm, uint32_t prior_rest_bpm,
				   uint32_t prior_weight_s,
				   struct radiant_hr_thresholds *out);

/* The same with the Kconfig prior and weight. */
void radiant_hr_thresholds_default(const struct radiant_hr_hist *h,
				   struct radiant_hr_thresholds *out);

/* ---- slot table (CONFIG_RADIANT_BRIDGE_HR_PROFILES, default 8) ---------- */

/*
 * The storage seam. All three run on the caller's thread (the bridge pump,
 * via radiant_rules.c) and are called only at attach (load), at session end
 * (save, gated) and at radiant_hr_profile_forget() (erase). Return 0 on
 * success, negative errno otherwise; load returns -ENOENT for "nothing
 * stored". Keyed on the binding uuid (section 6.5), never on the ANT device
 * number.
 */
struct radiant_hr_profile_store {
	int (*load)(uint64_t uuid, struct radiant_hr_profile_record *rec);
	int (*save)(uint64_t uuid, const struct radiant_hr_profile_record *rec);
	int (*erase)(uint64_t uuid);
};

/* NULL detaches. The pointer is kept, not copied. */
void radiant_hr_profile_set_store(const struct radiant_hr_profile_store *ops);

/* Forget every slot (RAM only; nothing is saved or erased). Test hook and
 * boot-time init. */
void radiant_hr_profile_reset(void);

/*
 * Give `uuid` a slot: its existing one, or a free one, or the
 * least-recently-active one (whose unsaved samples are saved first if the
 * gating allows). A newly claimed slot is loaded from the store. Returns
 * false only when the build has zero slots, which is the prior-only path,
 * not an error.
 */
bool radiant_hr_profile_attach(uint64_t uuid);

/* Feed one 1 Hz-decimated reading. No-op if `uuid` holds no slot. */
void radiant_hr_profile_add(uint64_t uuid, uint32_t bpm);

/* Thresholds for `uuid`: from its histogram if it holds a slot, else the
 * prior. */
void radiant_hr_profile_thresholds(uint64_t uuid, struct radiant_hr_thresholds *out);

/* A session ended: stamp the slot recently-active and save it if at least
 * RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES new samples arrived since the last
 * save and the per-boot cap is not spent. Returns true if a save was made. */
bool radiant_hr_profile_session_end(uint64_t uuid);

/* User eviction (the per-binding reset of section 6.5): drop the slot and
 * erase the stored record. */
void radiant_hr_profile_forget(uint64_t uuid);

/* Introspection for tests: whether `uuid` holds a slot, and its histogram. */
const struct radiant_hr_hist *radiant_hr_profile_hist(uint64_t uuid);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_HR_PROFILE_H_ */
