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
/*
 * THE SEND-LATENCY HISTOGRAM, AND WHY IT IS BUCKETS AND NOT A MEAN.
 *
 * The scheduler cost of a reserve is not a shift in the average - it is a TAIL.
 * A window we hold is ~96 ms of air the 802.15.4 stack cannot have, four times a
 * second per tracked sensor; the sends that fall outside those windows are
 * completely unaffected, and the ones that land inside them wait. Averaged
 * together those two populations produce a small number that is true of neither,
 * and a mean is exactly the statistic that would let a gate this phase is
 * supposed to fail slip through. The buckets are logarithmic because the
 * quantity being resolved spans from "no contention at all" to "one whole
 * grant".
 */
#define THREAD_COEX_LAT_BUCKETS 13

struct thread_coex_stats {
	uint32_t sent;        /* datagrams handed to the stack           */
	uint32_t send_failed; /* ot/net refused it - NOT an air loss     */
	uint32_t role_changes;
	uint8_t  role;        /* otDeviceRole, last observed             */
	bool     attached;

	/* zsock_sendto() call latency. Bucket 0 is everything under 64 us;
	 * bucket i in 1..11 is [64<<(i-1), 64<<i) us; bucket 12 is everything
	 * at or above 128 ms - which is longer than one whole grant, so a count
	 * there is not the reserve and needs a different explanation. */
	uint32_t lat[THREAD_COEX_LAT_BUCKETS];
	uint32_t lat_max_us;

	/*
	 * THE 15.4 STACK'S OWN VIEW OF BEING PUSHED OFF THE AIR, which is the
	 * half of the scheduler cost that the socket call cannot see. A send
	 * that returns promptly and is then retried three times, or abandoned on
	 * a busy channel, cost the other stack real air; the latency histogram
	 * records it as fast. These counters are the Thread-side mirror of the
	 * gate's own ext=a/b, and a CCA failure in particular IS our grant seen
	 * from the other side.
	 */
	uint32_t mac_tx_total;
	uint32_t mac_tx_retry;
	uint32_t mac_tx_err_cca;
	uint32_t mac_tx_err_abort;
	uint32_t mac_tx_err_busy;
};

void thread_coex_load_stats(struct thread_coex_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* ANT_DONGLE_THREAD_COEX_LOAD_H_ */
