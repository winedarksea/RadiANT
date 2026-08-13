/* SPDX-License-Identifier: Apache-2.0 */
/*
 * thread_coex_load.h - attach to a Thread network and keep traffic on it.
 *
 * The bench instrument for the multiprotocol plan's P4 coexistence gate, and the
 * exact counterpart of src/ble_coex_load.h for the other stack. See
 * thread_coex_load.c for why an attached-but-silent MTD is not a second stack,
 * and for the two costs this phase has to report separately.
 */

#ifndef ANT_DONGLE_THREAD_COEX_LOAD_H_
#define ANT_DONGLE_THREAD_COEX_LOAD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Schedule the load. Returns 0, or -EIO if the work item could not be queued.
 *
 * Call AFTER the radio backend is initialised, for the same reason
 * ble_coex_load_start() must be: radiant_radio_nrf.c takes an explicit HFXO
 * request at init that the P0 spike showed is load-bearing, and it has to be
 * held before another stack starts cycling the clock at grant edges.
 */
int thread_coex_load_start(void);

/*
 * THE SCHEDULER COST, sampled rather than derived.
 *
 * docs/radiant-bridge.md section 7.4 and the plan both insist P4 record the
 * scheduler cost and the air cost SEPARATELY, and today the docs record neither.
 * The air cost is ANT+ `loss (exact)` from tools/ant_verify.py, measured off the
 * board entirely. The scheduler cost is this: what the 802.15.4 side actually
 * asked for and got while that loss was being measured. Reporting one without
 * the other is how "coexistence is fine" and "coexistence is fine because the
 * other stack never transmitted" become the same sentence.
 */
struct thread_coex_stats {
	uint32_t sent;        /* datagrams handed to the stack           */
	uint32_t send_failed; /* ot/net refused it - NOT an air loss     */
	uint32_t role_changes;
	uint8_t  role;        /* otDeviceRole, last observed             */
	bool     attached;
};

void thread_coex_load_stats(struct thread_coex_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* ANT_DONGLE_THREAD_COEX_LOAD_H_ */
