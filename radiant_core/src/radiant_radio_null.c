/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_null.c - a radio backend that has no radio.
 *
 * Provenance: clean-room. Written from radiant_core/include/radiant_core/radiant_radio_hal.h alone -
 * the capability numbers below are the nRF52840 figures already recorded in
 * docs/ant-radio-link.md and docs/backends.md, restated here so that the core's
 * policy modules compute the same geometry they will compute against the real
 * backend. Nothing here derives from sdk-ant, from libant.a, from disassembly of
 * any binary, or from any ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * radiant_core is a link layer: it decides what goes on the air and when; a
 * BACKEND decides how. radiant_radio_hal.h is that boundary, and today the only
 * other implementation in this tree is radiant_core/tests/fake_radio.c (a
 * simulator with a virtual clock, for test images only). So a
 * `-DANT_RADIO=core` firmware link needs the HAL symbols supplied, and this
 * file supplies them the only honest way with no hardware in the loop: real
 * lifecycle and clock, but every arm call refused with RADIANT_RADIO_ENOTSUP.
 * A core image built on it enumerates, answers reset/capabilities/version,
 * accepts channel configuration, and runs the scheduler/search/channel state
 * machines/event queue - finding no sensors, since nothing is ever
 * transmitted or received.
 *
 * This is a placeholder for a missing MODULE, not a missing feature. When an
 * nRF backend lands it must REPLACE this file (two definitions of
 * radiant_radio_now() is a duplicate-symbol link error by design).
 * radiant_core/CMakeLists.txt selects between them via RADIANT_CORE_BACKEND.
 *
 * THE CAPABILITY BLOCK IS NOT ZEROED, DELIBERATELY. radiant_search.c computes
 * sweep geometry from caps.max_filters (8 -> 32 sets on nRF, 2 -> 128 on
 * RAIL); radiant_sched.c reads max_filters/min_arm_lead_us every pass;
 * radiant_transfer_init() refuses a backend whose min_arm_lead_us +
 * RADIANT_TRANSFER_ACK_GUARD_US exceeds RADIANT_TRANSFER_REPLY_US, or whose
 * rx_to_tx_us does. Advertising zeros would make all three decide
 * differently than they will against the real backend, so the numbers here
 * are the nRF52840's.
 *
 * has_sync_timestamp and has_rssi are NOT flattered - both false, since this
 * backend captures neither, and radiant_event.c's default policy suppresses
 * the 0xE0 timestamp on an inexact stamp rather than reporting a worse
 * number as good.
 */

#include <zephyr/kernel.h>

#include <radiant_core/radiant_radio_hal.h>

/* ---------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------------
 */

static const enum radiant_phy null_phys[] = { RADIANT_PHY_1M_GFSK };

static const struct radiant_radio_caps null_caps = {
	.name = "null (no radio; radiant_core link-layer bring-up only)",

	/* One base address plus eight prefixes - radiant_search.c's sweep geometry
	 * and radiant_sched.c's merge width are both computed from this. */
	.max_filters = 8u,
	/* Matches the nRF backend's real constraint rather than an unconstrained
	 * value, so a merge bug that only shows up under this limit is still
	 * caught on the bench. */
	.max_addr_groups = 2u,
	.filter_wildcard_dev = false,
	.addr_len_hw_max = 5u,
	.max_body_len = RADIANT_RADIO_BODY_MAX,

	.phys = null_phys,
	.n_phys = (uint8_t)(sizeof(null_phys) / sizeof(null_phys[0])),
	.phy_switch_us = 0u,

	/* Published nRF52840 figures, not measured here - matter because
	 * radiant_transfer_init() checks the acknowledged-data budget against
	 * them. */
	.ramp_up_us = 140u,
	.rx_to_tx_us = 140u,
	.tx_to_rx_us = 140u,

	/* radiant_transfer_init() requires this + RADIANT_TRANSFER_ACK_GUARD_US
	 * (250) to stay under RADIANT_TRANSFER_REPLY_US (1560), i.e. up to 1310;
	 * 300 is realistic for a timer-driven arm on this part, keeping the
	 * check meaningful rather than vacuous. */
	.min_arm_lead_us = 300u,

	.time_resolution_ns = 1000u, /* the kernel tick converted to us */

	/* Both false, deliberately: with no radio there's no address-capture
	 * event, so any t_sync would be an inference. radiant_radio_hal.h's rule
	 * is to degrade the advertised accuracy rather than report a worse
	 * number as good, and radiant_event.c honours that by suppressing the
	 * field. */
	.has_sync_timestamp = false,
	.has_rssi = false,

	/* False: energy detect IS an RSSI measurement, and there's no receiver
	 * to measure with. radiant_sched.c checks this before posting a chunk, so
	 * an ED request is refused at radiant_sched_request_ed() and never
	 * occupies a slot. */
	.has_ed_scan = false,

	/* Immaterial while nothing is received, but answered for real: the
	 * nRF CRC engine can cover the address bytes, which is the property
	 * this reports. */
	.crc_in_hw = true,

	/* False: this backend receives nothing, so it has no received CRC and
	 * the core's single-bit repair never runs - flattering this flag would
	 * add a repair path to a build with no radio to exercise it. */
	.has_rx_crc = false,

	.tx_power_min_dbm = -20,
	.tx_power_max_dbm = 8,
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

static const struct radiant_radio_cbs *null_cbs;
static void                       *null_user;
static bool                        null_inited;
static bool                        null_enabled;

const struct radiant_radio_caps *radiant_radio_caps_get(void)
{
	/* Static for the program's lifetime, never NULL, callable before init -
	 * radiant_sched_init() and radiant_search_init() both rely on the last part. */
	return &null_caps;
}

int radiant_radio_init(const struct radiant_radio_cbs *cbs, void *user)
{
	null_cbs = cbs;
	null_user = user;
	null_inited = true;
	null_enabled = false;
	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_enable(void)
{
	if (!null_inited) {
		return RADIANT_RADIO_ESTATE;
	}
	null_enabled = true;
	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_disable(void)
{
	/* Nothing is ever in flight on this backend, so there's no terminal
	 * event to deliver and the callback table is never used. */
	null_enabled = false;
	return RADIANT_RADIO_OK_RC;
}

radiant_time_t radiant_radio_now(void)
{
	/*
	 * A real monotonic microsecond clock - a fake one would turn "the
	 * radio can't transmit" into "the link layer is wedged" (scheduler
	 * subtracts timestamps freely, search credits dwell in us, channel
	 * state derives every slot instant from this).
	 *
	 * k_uptime_ticks() rather than k_uptime_get(): finest resolution
	 * available, converted once to keep the HAL's contract (absolute
	 * microseconds, never backwards) exact.
	 */
	return (radiant_time_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

/* ---------------------------------------------------------------------------
 * Operations - all of them refused
 * ---------------------------------------------------------------------------
 */

/*
 * RADIANT_RADIO_ENOTSUP specifically, not EIO or ESTATE. The HAL defines
 * ENOTSUP as "well-formed, but this backend can't do it" - literally true
 * here - and radiant_sched.c's arm_failed() consumes the request and reports
 * RADIANT_SCHED_DONE_FAILED for it. EBUSY/ESTATE instead leave the request in
 * the table for retry, which against a backend that never accepts anything
 * would be an unbounded retry every 50ms for the life of the image.
 */
int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op)
{
	if (req == NULL || op == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (!null_enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	*op = 0u;
	return RADIANT_RADIO_ENOTSUP;
}

int radiant_radio_rx(const struct radiant_rx_req *req, uint32_t *op)
{
	if (req == NULL || op == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (!null_enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	*op = 0u;
	return RADIANT_RADIO_ENOTSUP;
}

int radiant_radio_ed(const struct radiant_ed_req *req, uint32_t *op)
{
	if (req == NULL || op == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (!null_enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	*op = 0u;
	return RADIANT_RADIO_ENOTSUP;
}

int radiant_radio_abort(void)
{
	/* "Returns RADIANT_RADIO_OK_RC when there was nothing to do", and there
	 * never is. */
	(void)null_cbs;
	(void)null_user;
	return RADIANT_RADIO_OK_RC;
}
