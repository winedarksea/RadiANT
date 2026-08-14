/* SPDX-License-Identifier: Apache-2.0 */
/*
 * thread_coex_load.c - an 802.15.4 stack whose only job is to want the radio.
 *
 * WHY THIS EXISTS. docs/radiant-bridge.md section 7.4's P4 gate requires a
 * build attached and "carrying continuous traffic", loss (exact) no worse
 * than +0.5 pp against ANT alone. `CONFIG_NET_L2_OPENTHREAD=y` alone is not
 * that - the same trap as the BLE side's inert `CONFIG_BT=y` (sweep ratio
 * 1.000, a gate that cannot fail), but worse: a sleepy attached child looks
 * successful (role, address, `ot state child`) while transmitting almost
 * nothing, by design. So this file sends datagrams on a timer and counts
 * them; a P4 number without `sent` beside it is not a measurement.
 *
 * DESTINATION IS MULTICAST (`ff03::1`, mesh-local all-nodes) on purpose: a
 * unicast peer that rebooted or never commissioned would turn every send
 * into a silent address-resolution failure while `sent` still incremented.
 * Multicast transmits whenever this node is attached, full stop - though
 * that also makes the traffic one-directional, reproducing only the
 * transmit demand of a chatty node, not a substitute for a busy mesh.
 *
 * SED VS MED: section 7.4 requires measuring both, since MED (always-on) is
 * the worst case and a gate measured only in the role that schedules its
 * own windows would measure the mitigation rather than the problem. Role is
 * a Kconfig choice (thread.conf / thread_sed.conf), not runtime, so numbers
 * can't be attributed to the wrong role.
 *
 * OFF UNLESS ASKED FOR: CONFIG_ANT_DONGLE_THREAD_COEX_LOAD starts the load,
 * not CONFIG_NET_L2_OPENTHREAD alone. A shipping dongle builds neither.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/openthread.h>

#include <openthread/thread.h>
#include <openthread/link.h>

#include "thread_coex_load.h"

LOG_MODULE_REGISTER(thread_coex, LOG_LEVEL_INF);

static struct thread_coex_stats stats;

/*
 * One socket, opened on first use and never closed. The load runs until the
 * board is reset - there is no "stop" because a gate that can be stopped
 * mid-run invites a number taken across the boundary.
 */
static int sock = -1;

static void coex_send_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(coex_send, coex_send_fn);

/* Filled once, at the first send. */
static struct sockaddr_in6 dest;

static uint8_t payload[CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_PAYLOAD];

static bool socket_ready(void)
{
	int rc;

	if (sock >= 0) {
		return true;
	}

	sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return false;
	}

	memset(&dest, 0, sizeof(dest));
	dest.sin6_family = AF_INET6;
	dest.sin6_port = htons(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_PORT);
	rc = zsock_inet_pton(AF_INET6,
			     CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_DEST,
			     &dest.sin6_addr);
	if (rc != 1) {
		/* Loud and fatal to the load, not the dongle: silently sending
		 * nothing would produce a perfect coexistence result from an
		 * image with no second-stack traffic at all.
		 */
		LOG_ERR("destination '%s' is not an IPv6 address - the "
			"coexistence load will transmit NOTHING and any gate "
			"number from this image is void",
			CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_DEST);
		zsock_close(sock);
		sock = -1;
		return false;
	}

	/* Recognisable on a sniffer, and non-zero so a stuck-at-zero buffer is
	 * distinguishable from this payload. */
	memset(payload, 0xA7, sizeof(payload));
	return true;
}

/* Attachment gates the traffic but not the measurement's validity - an
 * unattached MTD runs MLE discovery, itself radio demand, just not the run
 * that was asked for. Log the role and let the console show which was got.
 */
static void note_role(void)
{
	otInstance *ot = openthread_get_default_instance();
	otDeviceRole role;

	if (ot == NULL) {
		return;
	}
	role = otThreadGetDeviceRole(ot);
	if ((uint8_t)role != stats.role) {
		stats.role = (uint8_t)role;
		stats.role_changes++;
		LOG_INF("thread role -> %s",
			otThreadDeviceRoleToString(role));
	}
	stats.attached = (role == OT_DEVICE_ROLE_CHILD ||
			  role == OT_DEVICE_ROLE_ROUTER ||
			  role == OT_DEVICE_ROLE_LEADER);
}

/* The 64 us floor is deliberate: an uncontended call is a queue push, and
 * separating a 3 us push from an 11 us push would add columns that never
 * move. The remaining twelve buckets resolve the region that matters, from
 * ordinary stack work up to the whole 96 ms grant.
 */
static uint32_t lat_bucket(uint32_t us)
{
	uint32_t i = 1;

	if (us < 64u) {
		return 0;
	}
	us >>= 6; /* now in 64 us units, and at least 1 */
	while (us > 1u && i < (THREAD_COEX_LAT_BUCKETS - 1u)) {
		us >>= 1;
		i++;
	}
	return i;
}

/*
 * MAC counters, stored absolute but reported as deltas since the previous
 * report - a total of 4000 tx/300 retries doesn't say whether the retries
 * clustered inside the two minutes an ANT channel was open; the delta does.
 *
 * Read without the OpenThread API lock, deliberately: this is a bench
 * instrument reading five monotonic counters for a printout, and blocking
 * this work item on the OT mutex would make the latency instrument a source
 * of the latency it measures.
 */
static void sample_mac_counters(void)
{
	otInstance *ot = openthread_get_default_instance();
	const otMacCounters *mc;

	if (ot == NULL) {
		return;
	}
	mc = otLinkGetCounters(ot);
	if (mc == NULL) {
		return;
	}
	stats.mac_tx_total     = mc->mTxTotal;
	stats.mac_tx_retry     = mc->mTxRetry;
	stats.mac_tx_err_cca   = mc->mTxErrCca;
	stats.mac_tx_err_abort = mc->mTxErrAbort;
	stats.mac_tx_err_busy  = mc->mTxErrBusyChannel;
}

static void coex_send_fn(struct k_work *w)
{
	int rc;

	ARG_UNUSED(w);

	note_role();

	if (stats.attached && socket_ready()) {
		uint32_t t0 = k_cycle_get_32();
		uint32_t us;

		rc = zsock_sendto(sock, payload, sizeof(payload), 0,
				  (struct sockaddr *)&dest, sizeof(dest));
		/* Timed across the call and nothing else, deliberately - widen
		 * this bracket and the histogram measures this file instead of
		 * the 15.4 stack.
		 */
		us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
		stats.lat[lat_bucket(us)]++;
		if (us > stats.lat_max_us) {
			stats.lat_max_us = us;
		}
		/* Counted whether or not the send succeeded: a call that took
		 * 40 ms and then failed still waited 40 ms. */
		if (rc < 0) {
			/* Never folded into `sent`: a refusal here (buffer
			 * exhaustion, no route) costs the arbiter nothing, and
			 * folding it in would let a failing image report the
			 * same `sent` as a succeeding one.
			 */
			stats.send_failed++;
		} else {
			stats.sent++;
		}
	}

	/*
	 * Periodic reporting on the same console the gate's own counters use, so
	 * the scheduler cost and the traffic that provoked it are read off one
	 * capture rather than correlated across two.
	 */
	if (stats.sent && (stats.sent %
	    CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_REPORT_EVERY) == 0u) {
		static uint32_t prev_mac[5];
		uint32_t d[5];

		LOG_INF("thread load: role=%u sent=%u failed=%u interval=%dms "
			"payload=%uB", stats.role, stats.sent,
			stats.send_failed,
			CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_INTERVAL_MS,
			(unsigned int)sizeof(payload));

		/*
		 * Histogram is cumulative, not per-report: an arm is a whole
		 * run, and resetting buckets every report would make the
		 * operator add columns by hand. MAC counters below are the
		 * opposite case - already cumulative since boot - so only
		 * their delta belongs to this arm.
		 *
		 * `us<` labels are the bucket's lower bound: 0 is everything
		 * under 64 us, then 64, 128, 256 ... last is >=256 ms.
		 */
		LOG_INF("thread lat us: <64=%u 64=%u 128=%u 256=%u 512=%u "
			"1k=%u 2k=%u 4k=%u 8k=%u 16k=%u 32k=%u 64k=%u "
			">=128k=%u max=%u",
			stats.lat[0], stats.lat[1], stats.lat[2], stats.lat[3],
			stats.lat[4], stats.lat[5], stats.lat[6], stats.lat[7],
			stats.lat[8], stats.lat[9], stats.lat[10],
			stats.lat[11], stats.lat[12], stats.lat_max_us);

		sample_mac_counters();
		d[0] = stats.mac_tx_total     - prev_mac[0];
		d[1] = stats.mac_tx_retry     - prev_mac[1];
		d[2] = stats.mac_tx_err_cca   - prev_mac[2];
		d[3] = stats.mac_tx_err_abort - prev_mac[3];
		d[4] = stats.mac_tx_err_busy  - prev_mac[4];
		prev_mac[0] = stats.mac_tx_total;
		prev_mac[1] = stats.mac_tx_retry;
		prev_mac[2] = stats.mac_tx_err_cca;
		prev_mac[3] = stats.mac_tx_err_abort;
		prev_mac[4] = stats.mac_tx_err_busy;

		/* Deferral measured on the other stack's own terms: a CCA
		 * failure or retry is the 15.4 MAC finding the air taken (often
		 * by us), so these five deltas are what "the reserve costs
		 * Thread's scheduler" means as a number.
		 */
		LOG_INF("thread mac d: tx=%u retry=%u cca=%u abort=%u busy=%u "
			"| tot tx=%u retry=%u cca=%u abort=%u busy=%u",
			d[0], d[1], d[2], d[3], d[4],
			stats.mac_tx_total, stats.mac_tx_retry,
			stats.mac_tx_err_cca, stats.mac_tx_err_abort,
			stats.mac_tx_err_busy);
	}

	(void)k_work_schedule(&coex_send,
		K_MSEC(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_INTERVAL_MS));
}

/* Delayed start is an instrument, not caution (see ble_coex_load.c): running
 * from millisecond zero makes "ANT+ never worked" and "ANT+ stopped when
 * Thread started" the same observation; a delayed start separates them.
 */
int thread_coex_load_start(void)
{
	LOG_INF("thread coexistence load: %s role, %s, every %d ms, to %s",
		IS_ENABLED(CONFIG_OPENTHREAD_MTD_SED) ? "SED" : "MED/FTD",
		IS_ENABLED(CONFIG_OPENTHREAD_MANUAL_START) ? "manual start"
							   : "auto start",
		CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_INTERVAL_MS,
		CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_DEST);

	return k_work_schedule(&coex_send,
			K_SECONDS(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD_DELAY_S)) < 0
		       ? -EIO
		       : 0;
}

void thread_coex_load_stats(struct thread_coex_stats *out)
{
	if (out != NULL) {
		*out = stats;
	}
}
