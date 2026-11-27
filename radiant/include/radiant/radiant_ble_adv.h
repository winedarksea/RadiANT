/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_adv.h - the advertising lifecycle of a RadiANT BLE peripheral.
 *
 * WHY THIS EXISTS. Zephyr stops a connectable advertiser when a connection is
 * established and never resumes it. apps/hrm_ble and apps/treadmill each
 * restarted it once, from `disconnected`, with no retry: with
 * CONFIG_BT_MAX_CONN=1 that restart races the connection object's release
 * (bt_le_adv_start() needs a free connection object and gets -ENOMEM until
 * `recycled`), and a restart that failed for any reason left the device
 * undiscoverable until power-cycled, while ANT+ kept transmitting and every
 * ANT-side check stayed green.
 *
 * So:
 *   - restart from the `recycled` callback, when the object is free again;
 *   - on failure, retry on a delayable work item, 100 ms doubling to 5 s
 *     (CONFIG_RADIANT_BLE_ADV_BACKOFF_*), until it succeeds;
 *   - -EALREADY is success: the advertiser is running;
 *   - a "should be advertising" flag, and radiant_ble_adv_watchdog() for the
 *     application's tick, which re-probes every CONFIG_RADIANT_BLE_ADV_WATCHDOG_MS
 *     while no connection holds the last slot - a probe of a running
 *     advertiser is a cheap -EALREADY.
 *
 * The decisions are the kernel-free radiant_ble_adv_state functions below;
 * radiant/tests/ble runs them on native_sim. src/ble/radiant_ble_adv.c is the
 * Zephyr glue and adds nothing to them.
 *
 * The name-suffix helper is here too because the name lives in the
 * advertising payload.
 */
#ifndef RADIANT_BLE_ADV_H_
#define RADIANT_BLE_ADV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Kernel-free decisions ────────────────────────────────────────────────── */

struct radiant_ble_adv_state {
	uint32_t backoff_ms;    /* delay of the pending retry; 0 = none pending */
	uint32_t failures;      /* consecutive failed starts */
	uint32_t last_probe_ms; /* the watchdog's last probe */
	uint32_t min_ms;        /* first retry delay */
	uint32_t max_ms;        /* retry delay cap */
	uint32_t watchdog_ms;   /* watchdog probe period; 0 = never */
	uint8_t  conns;         /* live connections */
	uint8_t  max_conns;     /* CONFIG_BT_MAX_CONN */
	bool     want;          /* the application wants to be discoverable */
	bool     active;        /* the last start succeeded and no connection
				 * has consumed the advertiser since */
};

void radiant_ble_adv_state_init(struct radiant_ble_adv_state *s,
				uint8_t max_conns, uint32_t min_ms,
				uint32_t max_ms, uint32_t watchdog_ms);

/* Whether a start may be attempted now: wanted, and a connection object is
 * free for it. Says nothing about whether one is already running - a start
 * of a running advertiser is -EALREADY, which is success. */
bool radiant_ble_adv_may_start(const struct radiant_ble_adv_state *s);

/*
 * The start of an attempt (a retry falling due, a recycled object, the
 * watchdog). True when bt_le_adv_start() should be called now. When it may
 * not (a connection took the slot meanwhile, or nobody wants it), any pending
 * retry is dropped, so a later `recycled` or watchdog probe is free to make
 * the next attempt - a retry left marked pending would block both forever.
 */
bool radiant_ble_adv_begin_attempt(struct radiant_ble_adv_state *s);

/*
 * Record a bt_le_adv_start() result. Returns 0 when nothing more is to be
 * done (success, -EALREADY, or not wanted any more), else the delay in ms
 * before the next attempt: min_ms after the first failure, doubling, capped
 * at max_ms.
 */
uint32_t radiant_ble_adv_note_result(struct radiant_ble_adv_state *s, int err);

/* The pure backoff step: 0 -> min, then x2 capped at max. */
uint32_t radiant_ble_adv_next_backoff(uint32_t prev_ms, uint32_t min_ms,
				      uint32_t max_ms);

/* Connection events. A connection (successful or not) ends the connectable
 * advertiser; only a successful one holds a connection slot. */
void radiant_ble_adv_note_connected(struct radiant_ble_adv_state *s, bool ok);
void radiant_ble_adv_note_disconnected(struct radiant_ble_adv_state *s);

/* `recycled`: a connection object came free. Returns true when a start
 * should be attempted now. */
bool radiant_ble_adv_note_recycled(struct radiant_ble_adv_state *s);

/*
 * The watchdog. True when a probe start is due: wanted, a slot is free, no
 * retry is already pending, and watchdog_ms has passed since the last probe.
 * Records `now_ms` as the probe time when it answers true. Wrap-safe.
 */
bool radiant_ble_adv_watchdog_due(struct radiant_ble_adv_state *s,
				  uint32_t now_ms);

/*
 * "<base> <devnum>" into out[cap]: the per-unit advertised name. Returns the
 * length written (without the terminator), or -1 when it does not fit or an
 * argument is NULL/zero, in which case out holds `base` alone if that fits.
 * At most 6 characters longer than `base` (a space and up to five digits).
 */
int radiant_ble_name_with_devnum(char *out, size_t cap, const char *base,
				 uint16_t devnum);

/* What the suffix can add, for the applications' BUILD_ASSERT name budgets. */
#define RADIANT_BLE_NAME_SUFFIX_MAX 6u

/* ── Zephyr glue (CONFIG_RADIANT_BLE) ─────────────────────────────────────── */

struct bt_le_adv_param;
struct bt_data;

/*
 * Register what "advertising" means for this device. The parameters and
 * both arrays are copied by reference and must outlive the advertiser
 * (static storage). `sd` may be NULL with sd_len 0. Call before
 * radiant_ble_adv_ensure(). Returns 0 or -EINVAL.
 */
int radiant_ble_adv_init(const struct bt_le_adv_param *param,
			 const struct bt_data *ad, size_t ad_len,
			 const struct bt_data *sd, size_t sd_len);

/*
 * Be discoverable from now on. Tries once synchronously and returns that
 * result (0 for success or -EALREADY); on failure a retry is already
 * scheduled, so the caller may log and carry on.
 */
int radiant_ble_adv_ensure(void);

/* From the application's periodic tick. Cheap when nothing is due. */
void radiant_ble_adv_watchdog(void);

/* True while the layer believes the advertiser is running. */
bool radiant_ble_adv_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BLE_ADV_H_ */
