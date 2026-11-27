/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_conn.h - the connection-parameter policy.
 *
 * Moved here from the "textual twins" in apps/hrm_ble/src/hrm_ble.c and
 * apps/treadmill/src/treadmill_ble.c. The intervals ARE the MPSL priority
 * mechanism: the SoftDevice Controller outranks anything an application can
 * request, so RadiANT keeps its ANT+ slots by shaping BLE's demand to a long
 * connection interval (cf. nrf/samples/esb/esb_ptx_ble, MIN = MAX = 800).
 *
 * With CONFIG_RADIANT_BLE_CONN_POLICY the layer registers le_param_req and
 * le_param_updated itself (src/ble/radiant_ble_conn.c); an application no
 * longer defines them.
 *
 * WHY REWRITE AND NOT REJECT. A rejected request leaves the link at whatever
 * short interval it connected at - Zephyr does not fall back to the PPCP
 * preference - so rewriting the request in place and accepting it is the only
 * branch that moves the link to policy.
 *
 * EVERY ACCEPTED TRIPLE IS VALID. Zephyr's bt_le_conn_params_valid() requires
 * timeout * 4 > (1 + latency) * interval_max (raw units: 10 ms and 1.25 ms).
 * An invalid triple is neg-replied by the controller in silence while the log
 * claims the request was honoured, so both the accept-untouched branch and the
 * rewrite are checked against it: the rewrite forces latency 0 and raises
 * (never lowers) the timeout to the policy floor, which is itself chosen to
 * clear the bound at the policy interval.
 */
#ifndef RADIANT_BLE_CONN_H_
#define RADIANT_BLE_CONN_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The fields of struct bt_le_conn_param, kept separate so the rule is
 * kernel-free. */
struct radiant_ble_conn_param {
	uint16_t interval_min; /* 1.25 ms units */
	uint16_t interval_max; /* 1.25 ms units */
	uint16_t latency;      /* connection events */
	uint16_t timeout;      /* 10 ms units */
};

/* bt_le_conn_params_valid()'s timeout bound. */
bool radiant_ble_conn_param_valid(const struct radiant_ble_conn_param *p);

/*
 * Apply the policy to a peer's request in place. Returns true when the request
 * was rewritten, false when it was already compliant and valid and is left
 * untouched. Either way the caller accepts the (possibly rewritten) request.
 *
 * Compliant: both intervals >= min_interval, latency 0, timeout >=
 * min_timeout. The rewrite sets both intervals to min_interval, latency to 0
 * and the timeout to max(timeout, min_timeout).
 */
bool radiant_ble_conn_param_apply(struct radiant_ble_conn_param *p,
				  uint16_t min_interval, uint16_t min_timeout);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BLE_CONN_H_ */
