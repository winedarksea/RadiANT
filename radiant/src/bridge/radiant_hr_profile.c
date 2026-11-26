/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_hr_profile.c - see radiant_hr_profile.h.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/spinlock.h>

#include <radiant/radiant_binding.h>
#include <radiant/radiant_hr_profile.h>

/* Kconfig, with the documented defaults for builds that compile this file
 * without RADIANT_BRIDGE_RULES (radiant/tests compiles sources directly). */
#ifndef CONFIG_RADIANT_BRIDGE_HR_MAX_PRIOR
#define CONFIG_RADIANT_BRIDGE_HR_MAX_PRIOR 180
#endif
#ifndef CONFIG_RADIANT_BRIDGE_HR_REST_PRIOR
#define CONFIG_RADIANT_BRIDGE_HR_REST_PRIOR 65
#endif
#ifndef CONFIG_RADIANT_BRIDGE_HR_PRIOR_WEIGHT_S
#define CONFIG_RADIANT_BRIDGE_HR_PRIOR_WEIGHT_S 1800
#endif
#ifndef CONFIG_RADIANT_BRIDGE_HR_PROFILES
#define CONFIG_RADIANT_BRIDGE_HR_PROFILES 8
#endif

_Static_assert(sizeof(struct radiant_hr_profile_record) == 2u + 2u * RADIANT_HR_HIST_BINS,
	       "the persisted record must have no padding");
_Static_assert(RADIANT_HR_HIST_BINS == 38u, "section 6.3: 38 bins");

/* Karvonen fractions, in percent of HRR (section 6.2 and the 5-zone model). */
#define REST_PCT  15u
#define ZONE2_PCT 60u
#define ZONE3_PCT 70u

/* ---- pure estimator ---------------------------------------------------- */

void radiant_hr_hist_add(struct radiant_hr_hist *h, uint32_t bpm)
{
	uint32_t i;
	uint32_t b;

	if (bpm < RADIANT_HR_HIST_MIN_BPM || bpm >= RADIANT_HR_HIST_MAX_BPM) {
		return;
	}
	b = (bpm - RADIANT_HR_HIST_MIN_BPM) / RADIANT_HR_HIST_BIN_BPM;

	if (h->bin[b] == UINT16_MAX) {
		/* Cannot happen after the halving below, but a record loaded
		 * from storage is not this function's own output. */
		for (i = 0u; i < RADIANT_HR_HIST_BINS; i++) {
			h->bin[i] /= 2u;
		}
	}
	h->bin[b]++;
	if (h->bin[b] == UINT16_MAX) {
		/* Forgetting is free (section 6.3): halve every bin the moment
		 * any one saturates, so ratios between bins survive. */
		for (i = 0u; i < RADIANT_HR_HIST_BINS; i++) {
			h->bin[i] /= 2u;
		}
	}
}

uint32_t radiant_hr_hist_total(const struct radiant_hr_hist *h)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0u; i < RADIANT_HR_HIST_BINS; i++) {
		n += h->bin[i];
	}
	return n;
}

uint32_t radiant_hr_hist_quantile_x10(const struct radiant_hr_hist *h,
				      uint32_t permille)
{
	uint64_t total = radiant_hr_hist_total(h);
	uint64_t target;   /* in thousandths of a sample */
	uint64_t cum = 0u; /* in thousandths of a sample */
	uint32_t i;

	if (total == 0u) {
		return 0u;
	}
	if (permille > 1000u) {
		permille = 1000u;
	}
	target = total * permille;

	for (i = 0u; i < RADIANT_HR_HIST_BINS; i++) {
		uint64_t here = (uint64_t)h->bin[i] * 1000u;

		if (here != 0u && cum + here >= target) {
			uint64_t lo_x10 = (uint64_t)(RADIANT_HR_HIST_MIN_BPM +
						     i * RADIANT_HR_HIST_BIN_BPM) * 10u;
			uint64_t into = (target > cum) ? (target - cum) : 0u;

			/* Linear inside the bin: sub-5-bpm resolution. */
			return (uint32_t)(lo_x10 +
					  (into * RADIANT_HR_HIST_BIN_BPM * 10u) / here);
		}
		cum += here;
	}
	/* permille == 1000 on rounding: the top of the last non-empty bin. */
	for (i = RADIANT_HR_HIST_BINS; i-- > 0u;) {
		if (h->bin[i] != 0u) {
			return (RADIANT_HR_HIST_MIN_BPM + (i + 1u) * RADIANT_HR_HIST_BIN_BPM) * 10u;
		}
	}
	return 0u;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
	return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static uint32_t blend_x10(uint32_t prior_bpm, uint32_t weight, uint32_t obs_x10,
			  uint32_t n)
{
	uint64_t num = (uint64_t)prior_bpm * 10u * weight + (uint64_t)obs_x10 * n;
	uint64_t den = (uint64_t)weight + n;

	if (den == 0u) {
		return prior_bpm * 10u;
	}
	return (uint32_t)((num + den / 2u) / den);
}

void radiant_hr_thresholds_compute(const struct radiant_hr_hist *h,
				   uint32_t prior_max_bpm, uint32_t prior_rest_bpm,
				   uint32_t prior_weight_s,
				   struct radiant_hr_thresholds *out)
{
	uint32_t n = (h != NULL) ? radiant_hr_hist_total(h) : 0u;
	uint32_t max_x10;
	uint32_t rest_x10;
	uint32_t hrr_c;  /* hundredths of a bpm */
	uint32_t rest_c;

	if (n == 0u) {
		/* The prior, exactly: n = 0 of the same formula. */
		max_x10 = prior_max_bpm * 10u;
		rest_x10 = prior_rest_bpm * 10u;
	} else {
		max_x10 = blend_x10(prior_max_bpm, prior_weight_s,
				    radiant_hr_hist_quantile_x10(h, 990u), n);
		rest_x10 = blend_x10(prior_rest_bpm, prior_weight_s,
				     radiant_hr_hist_quantile_x10(h, 10u), n);
	}

	/* Not in the doc: a strap that misreads must not invert the zones.
	 * The floors and ceilings alone guarantee HRR >= 40 bpm. */
	max_x10 = clamp_u32(max_x10, RADIANT_HR_MAX_FLOOR * 10u, RADIANT_HR_MAX_CEIL * 10u);
	rest_x10 = clamp_u32(rest_x10, RADIANT_HR_REST_FLOOR * 10u, RADIANT_HR_REST_CEIL * 10u);

	rest_c = rest_x10 * 10u;
	hrr_c = (max_x10 - rest_x10) * 10u;

	out->hr_max = (uint8_t)((max_x10 + 5u) / 10u);
	out->hr_rest = (uint8_t)((rest_x10 + 5u) / 10u);
	/* "rest < X": floor, so 82.25 -> 82 and 82 bpm is not at rest, as
	 * before personalisation landed. "zone >= X": ceiling, so 145.5 ->
	 * 146 and the band never starts below its own definition. */
	out->rest_below = (uint8_t)((rest_c + (hrr_c * REST_PCT) / 100u) / 100u);
	out->zone2_at = (uint8_t)((rest_c + (hrr_c * ZONE2_PCT) / 100u + 99u) / 100u);
	out->zone3_at = (uint8_t)((rest_c + (hrr_c * ZONE3_PCT) / 100u + 99u) / 100u);
}

void radiant_hr_thresholds_default(const struct radiant_hr_hist *h,
				   struct radiant_hr_thresholds *out)
{
	radiant_hr_thresholds_compute(h, CONFIG_RADIANT_BRIDGE_HR_MAX_PRIOR,
				      CONFIG_RADIANT_BRIDGE_HR_REST_PRIOR,
				      CONFIG_RADIANT_BRIDGE_HR_PRIOR_WEIGHT_S, out);
}

/* ---- slot table -------------------------------------------------------- */

static const struct radiant_hr_profile_store *store;

void radiant_hr_profile_set_store(const struct radiant_hr_profile_store *ops)
{
	store = ops;
}

#if CONFIG_RADIANT_BRIDGE_HR_PROFILES > 0

struct slot {
	bool                   used;
	uint64_t               uuid;
	uint32_t               last_active; /* session sequence number; no RTC */
	uint32_t               unsaved;     /* samples since the last save */
	uint8_t                saves;       /* this boot */
	struct radiant_hr_hist hist;
};

static struct slot slots[CONFIG_RADIANT_BRIDGE_HR_PROFILES];
static uint32_t    session_seq;

/*
 * forget() is called from the user-eviction path, which runs on the
 * self-channel thread while every other function here runs on the bridge
 * pump. So forget() erases the stored record at once (the store is
 * thread-safe) but only QUEUES the RAM slot drop; find() applies the queue
 * before it looks anything up, on the pump thread. On queue overflow the
 * drop falls back to "every slot whose uuid is no longer bound", which is a
 * superset of what was asked.
 */
#define FORGET_QUEUE 8u
static struct k_spinlock forget_lock;
static uint64_t          forget_q[FORGET_QUEUE];
static uint32_t          forget_n;
static bool              forget_overflow;

void radiant_hr_profile_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&forget_lock);

	forget_n = 0u;
	forget_overflow = false;
	k_spin_unlock(&forget_lock, key);
	memset(slots, 0, sizeof(slots));
	session_seq = 0u;
}

static bool uuid_bound(uint64_t uuid)
{
	uint32_t src;

	for (src = 0u; src < RADIANT_BINDING_MAX; src++) {
		const struct radiant_binding *b = radiant_binding_get(src);

		if (b != NULL && b->uuid == uuid) {
			return true;
		}
	}
	return false;
}

static void apply_forgets(void)
{
	uint64_t         q[FORGET_QUEUE];
	uint32_t         n;
	bool             overflow;
	uint32_t         i;
	uint32_t         j;
	k_spinlock_key_t key = k_spin_lock(&forget_lock);

	n = forget_n;
	overflow = forget_overflow;
	memcpy(q, forget_q, sizeof(q[0]) * n);
	forget_n = 0u;
	forget_overflow = false;
	k_spin_unlock(&forget_lock, key);

	for (i = 0u; i < CONFIG_RADIANT_BRIDGE_HR_PROFILES; i++) {
		bool drop = false;

		if (!slots[i].used) {
			continue;
		}
		for (j = 0u; j < n; j++) {
			if (slots[i].uuid == q[j]) {
				drop = true;
			}
		}
		if (overflow && !uuid_bound(slots[i].uuid)) {
			drop = true;
		}
		if (drop) {
			memset(&slots[i], 0, sizeof(slots[i]));
		}
	}
}

static struct slot *find(uint64_t uuid)
{
	uint32_t i;

	apply_forgets();
	for (i = 0u; i < CONFIG_RADIANT_BRIDGE_HR_PROFILES; i++) {
		if (slots[i].used && slots[i].uuid == uuid) {
			return &slots[i];
		}
	}
	return NULL;
}

static bool save_gated(struct slot *s)
{
	struct radiant_hr_profile_record rec;

	if (s->unsaved < RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES ||
	    s->saves >= RADIANT_HR_PROFILE_SAVES_PER_BOOT ||
	    store == NULL || store->save == NULL) {
		return false;
	}
	memset(&rec, 0, sizeof(rec));
	rec.version = RADIANT_HR_PROFILE_RECORD_VERSION;
	rec.hist = s->hist;
	/* Counted whether or not the write succeeded: the cap is a wear
	 * guard, and a failing store must not be retried every session. */
	s->saves++;
	if (store->save(s->uuid, &rec) != 0) {
		return false;
	}
	s->unsaved = 0u;
	return true;
}

bool radiant_hr_profile_attach(uint64_t uuid)
{
	struct slot *s = find(uuid);
	uint32_t     i;

	if (s != NULL) {
		s->last_active = ++session_seq;
		return true;
	}

	for (i = 0u; i < CONFIG_RADIANT_BRIDGE_HR_PROFILES; i++) {
		if (!slots[i].used) {
			s = &slots[i];
			break;
		}
	}
	if (s == NULL) {
		/* Least-recently-active eviction (section 6.4). Its unsaved
		 * samples get one gated save on the way out. */
		s = &slots[0];
		for (i = 1u; i < CONFIG_RADIANT_BRIDGE_HR_PROFILES; i++) {
			if (slots[i].last_active < s->last_active) {
				s = &slots[i];
			}
		}
		(void)save_gated(s);
	}

	memset(s, 0, sizeof(*s));
	s->used = true;
	s->uuid = uuid;
	s->last_active = ++session_seq;

	if (store != NULL && store->load != NULL) {
		struct radiant_hr_profile_record rec;

		memset(&rec, 0, sizeof(rec));
		if (store->load(uuid, &rec) == 0 &&
		    rec.version == RADIANT_HR_PROFILE_RECORD_VERSION) {
			s->hist = rec.hist;
		}
	}
	return true;
}

void radiant_hr_profile_add(uint64_t uuid, uint32_t bpm)
{
	struct slot *s = find(uuid);

	if (s == NULL) {
		return;
	}
	if (bpm >= RADIANT_HR_HIST_MIN_BPM && bpm < RADIANT_HR_HIST_MAX_BPM) {
		radiant_hr_hist_add(&s->hist, bpm);
		s->unsaved++;
	}
}

void radiant_hr_profile_thresholds(uint64_t uuid, struct radiant_hr_thresholds *out)
{
	struct slot *s = find(uuid);

	radiant_hr_thresholds_default((s != NULL) ? &s->hist : NULL, out);
}

bool radiant_hr_profile_session_end(uint64_t uuid)
{
	struct slot *s = find(uuid);

	if (s == NULL) {
		return false;
	}
	s->last_active = ++session_seq;
	return save_gated(s);
}

void radiant_hr_profile_forget(uint64_t uuid)
{
	k_spinlock_key_t key = k_spin_lock(&forget_lock);

	if (forget_n < FORGET_QUEUE) {
		forget_q[forget_n++] = uuid;
	} else {
		forget_overflow = true;
	}
	k_spin_unlock(&forget_lock, key);

	if (store != NULL && store->erase != NULL) {
		(void)store->erase(uuid);
	}
}

const struct radiant_hr_hist *radiant_hr_profile_hist(uint64_t uuid)
{
	struct slot *s = find(uuid);

	return (s != NULL) ? &s->hist : NULL;
}

#else /* CONFIG_RADIANT_BRIDGE_HR_PROFILES == 0: prior only */

void radiant_hr_profile_reset(void)
{
}

bool radiant_hr_profile_attach(uint64_t uuid)
{
	(void)uuid;
	return false;
}

void radiant_hr_profile_add(uint64_t uuid, uint32_t bpm)
{
	(void)uuid;
	(void)bpm;
}

void radiant_hr_profile_thresholds(uint64_t uuid, struct radiant_hr_thresholds *out)
{
	(void)uuid;
	radiant_hr_thresholds_default(NULL, out);
}

bool radiant_hr_profile_session_end(uint64_t uuid)
{
	(void)uuid;
	return false;
}

void radiant_hr_profile_forget(uint64_t uuid)
{
	if (store != NULL && store->erase != NULL) {
		(void)store->erase(uuid);
	}
}

const struct radiant_hr_hist *radiant_hr_profile_hist(uint64_t uuid)
{
	(void)uuid;
	return NULL;
}

#endif
