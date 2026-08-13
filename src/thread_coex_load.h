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
 * docs/radiant-bridge.md section 7.4 requires P4 record the scheduler cost
 * (what 802.15.4 asked for and got) separately from the air cost (ANT+
 * `loss (exact)`, measured off-board) - reporting one without the other
 * hides whether the other stack transmitted at all.
 *
 * The send-latency histogram uses buckets rather than a mean because the
 * cost is a TAIL, not a shift in the average: a ~96 ms reserved window four
 * times a second per tracked sensor leaves sends outside it unaffected and
 * sends inside it waiting, and averaging those two populations produces a
 * number true of neither. Buckets are logarithmic to span "no contention"
 * through "one whole grant".
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
	 * The 15.4 stack's own view of being pushed off the air - the half
	 * of the scheduler cost the socket call cannot see. A send that
	 * returns promptly but was retried or abandoned cost real air while
	 * the latency histogram records it as fast; a CCA failure here IS
	 * our grant seen from the other side.
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
