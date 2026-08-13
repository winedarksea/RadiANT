/* SPDX-License-Identifier: Apache-2.0 */
/*
 * thread_coex_load.c - an 802.15.4 stack whose only job is to want the radio.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 * ---------------------------------------------------------------------------
 *
 * docs/radiant-bridge.md section 7.4 states the P4 gate in full: a build with
 * `CONFIG_NET_L2_OPENTHREAD=y`, attached in the role section 7.3b selects and
 * "carrying continuous traffic", with `loss (exact)` no worse than +0.5 pp
 * against the same board running ANT alone.
 *
 * The words "carrying continuous traffic" are load-bearing, and this project has
 * already paid once for ignoring the equivalent words on the BLE side.
 * `CONFIG_BT=y` alone linked a controller in and left it asleep, and the gate
 * came back with a sweep-rate ratio of exactly 1.000 - a gate that cannot fail.
 * `CONFIG_NET_L2_OPENTHREAD=y` alone is the same trap wearing a different hat,
 * and it is a WORSE one, because an attached MTD is not obviously idle: it has a
 * role, it has an address, `ot state` says `child`, and every one of those is
 * true of a node that has transmitted four MLE frames in the last minute and
 * nothing since. A sleepy child that is doing its job correctly is nearly silent
 * BY DESIGN - that is what section 7.3b chose it FOR - so the configuration that
 * looks most like success is exactly the configuration that measures nothing.
 *
 * So this file sends datagrams on a timer, and counts them, and the count is
 * part of the result. A P4 number quoted without `sent` beside it is not a
 * measurement.
 *
 * ---------------------------------------------------------------------------
 * THE DESTINATION IS A MULTICAST ADDRESS, ON PURPOSE
 * ---------------------------------------------------------------------------
 *
 * Default `ff03::1` - mesh-local all-nodes. Not because a unicast peer would be
 * worse traffic, but because it removes an entire class of silent failure from
 * the rig: with a unicast destination, a peer that has rebooted, moved, or never
 * commissioned turns every send into an address-resolution failure, and the
 * board still logs `sent` while putting a quite different amount of energy on
 * the air than the operator thinks. Multicast to all-nodes is transmitted by
 * this node whenever this node is attached, full stop, and what the rest of the
 * mesh does with it is not part of the measurement.
 *
 * It does mean the traffic is one-directional. That is the honest limitation and
 * it belongs in the write-up: this instrument reproduces the TRANSMIT demand of
 * a chatty Thread node, and the receive demand only insofar as an attached MTD
 * or MED already keeps its receiver on. It is not a substitute for a busy mesh.
 *
 * ---------------------------------------------------------------------------
 * SED VERSUS MED, AND WHY BOTH GET MEASURED
 * ---------------------------------------------------------------------------
 *
 * Section 7.4: "Run it in the MED role as well as the intended one, even though
 * MED is the last preference. MED is the always-on case, so it is the worst
 * case, and a gate measured only in the role that schedules its own windows
 * would be measuring the mitigation rather than the problem."
 *
 * The role is chosen in Kconfig rather than here, because it is a property of
 * the image and changing it at runtime would let one console session's numbers
 * be attributed to the other role. thread.conf and thread_sed.conf are the two
 * builds; this file is identical in both and reports which one it is.
 *
 * ---------------------------------------------------------------------------
 * OFF UNLESS ASKED FOR
 * ---------------------------------------------------------------------------
 *
 * Selecting CONFIG_NET_L2_OPENTHREAD on this application does not start a load;
 * CONFIG_ANT_DONGLE_THREAD_COEX_LOAD does. A shipping dongle builds neither, and
 * docs/decisions/0011-never-a-border-router.md is why it never will.
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
		/*
		 * Loud and fatal to the load, not to the dongle. A malformed
		 * destination that fell through to "send nothing" would produce
		 * a perfect coexistence result from an image with no second
		 * stack traffic at all, which is the single most misleading
		 * outcome this file can have.
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

/*
 * Attachment is a precondition for the traffic, and NOT for the measurement's
 * validity on its own. An unattached MTD is not silent - it is running MLE
 * discovery, which is itself radio demand - so the run is still meaningful; it
 * is just not the run that was asked for. Hence: log the role, count the sends,
 * and let whoever reads the console see which of the two they got.
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

/*
 * THE BUCKET INDEX, AND THE FLOOR IS DELIBERATE.
 *
 * Everything below 64 us lands in bucket 0. There is nothing to resolve down
 * there: the uncontended call is a queue push, and separating a 3 us push from
 * an 11 us push would only add columns that never move. What has to be
 * resolvable is the region from "a few hundred microseconds of ordinary stack
 * work" up to "the whole 96 ms grant", which is what the remaining twelve
 * buckets cover.
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
 * THE MAC COUNTERS, SAMPLED WHERE THEY CAN BE READ AGAINST THE SENDS THAT
 * PROVOKED THEM.
 *
 * Absolute, not deltas, and the report prints them as deltas since the previous
 * report - which is the form that answers the question. A total of 4000 tx with
 * 300 retries says nothing about whether the retries were spread evenly or all
 * arrived in the two minutes an ANT channel was open; the per-report delta,
 * printed beside `sent` on the same line, does.
 *
 * Read WITHOUT taking the OpenThread API lock, which is a deliberate and
 * bounded choice rather than an oversight. note_role() above has always read
 * otThreadGetDeviceRole() the same way and this is a bench instrument reading
 * five monotonic uint32 counters for a printout; the cost of being wrong is a
 * torn value in one report line, and the cost of blocking this work item on the
 * OT mutex is that the instrument measuring scheduler latency becomes a source
 * of it.
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
		/*
		 * TIMED ACROSS THE CALL AND NOTHING ELSE. The bracket is as
		 * tight as it can be on purpose: widen it to include note_role()
		 * or the reporting below and the histogram starts measuring this
		 * file instead of the 15.4 stack.
		 */
		us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
		stats.lat[lat_bucket(us)]++;
		if (us > stats.lat_max_us) {
			stats.lat_max_us = us;
		}
		/* Counted whether or not the send succeeded: a call that took
		 * 40 ms and then failed still waited 40 ms. */
		if (rc < 0) {
			/*
			 * Counted separately from `sent` and never folded into
			 * it. A refusal here is the stack declining to transmit
			 * - buffer exhaustion, no route - and it costs the
			 * arbiter nothing. Folding the two would let an image
			 * that failed every send report the same `sent` as one
			 * that succeeded on every one.
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
		 * CUMULATIVE, NOT PER-REPORT, and that is the right choice for
		 * the histogram even though the MAC counters below are printed
		 * as deltas. An arm is a whole run against one image; the shape
		 * of the tail over the whole run is the result, and resetting
		 * the buckets every report would leave the operator adding
		 * columns by hand across a console capture. The MAC counters
		 * are the opposite case - they are already cumulative in the
		 * stack and have been counting since boot, including through
		 * attach, so only their delta belongs to this arm.
		 *
		 * `us<` labels are the bucket's lower bound: 0 is everything
		 * under 64 us, then 64, 128, 256 ... and the last is >=256 ms.
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

		/*
		 * THIS IS DEFERRAL, MEASURED ON THE OTHER STACK'S OWN TERMS.
		 * A CCA failure or a retry is the 15.4 MAC finding the air
		 * taken, which under this build is very often taken by us -
		 * so these five deltas are what "the reserve costs Thread's
		 * scheduler" means as a number rather than as a claim.
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

/*
 * DELAYED, AND THE DELAY IS AN INSTRUMENT RATHER THAN CAUTION - the same
 * argument ble_coex_load.c makes at length. With the load running from the first
 * millisecond, "ANT+ never worked" and "ANT+ stopped when Thread started" are
 * the same observation. With it arriving seconds later the console shows the
 * sweep running, then the load starting, and then whatever happens next.
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
