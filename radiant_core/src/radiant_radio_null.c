/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_null.c - a radio backend that has no radio.
 *
 * Provenance: clean-room. Written from radiant_core/include/radiant_core/radiant_radio_hal.h alone -
 * the capability numbers below are the nRF52840 figures already recorded in
 * docs/ant-radio-link.md and docs/backends.md, restated here so that the core's
 * policy modules compute the same geometry they will compute against the real
 * backend. Nothing here derives from sdk-ant, from libant.a, from disassembly of
 * any binary, or from any adopter-gated ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE EXISTS, AND WHEN IT SHOULD STOP EXISTING
 * ---------------------------------------------------------------------------
 * radiant_core is a link layer. It decides what goes on the air and when; a BACKEND
 * decides how. radiant_radio_hal.h is the whole of that boundary and exactly one
 * implementation of it exists in this tree today - radiant_core/tests/fake_radio.c,
 * a simulator with a virtual clock, which belongs in a test image and nowhere
 * near a dongle.
 *
 * So a `-DANT_RADIO=core` firmware link has eight undefined symbols that no
 * module in the fan-out was asked to write: radiant_radio_caps_get, _init, _enable,
 * _disable, _now, _tx, _rx and _abort. This file supplies them, and supplies
 * them in the only honest way available with no hardware in the loop: the
 * lifecycle and the clock are real, and EVERY ARM CALL IS REFUSED WITH
 * RADIANT_RADIO_ENOTSUP.
 *
 * Read that as the HAL-level counterpart of src/ant_radio_stub.c. A core image
 * built on it enumerates, answers reset / capabilities / version, accepts every
 * channel configuration the host sends, runs the scheduler, the search policy,
 * the channel state machines and the event queue - and finds no sensors,
 * because nothing is ever transmitted or received. That is a great deal more
 * than the stub proves and a great deal less than a dongle.
 *
 * IT IS NOT A PLACEHOLDER FOR A MISSING FEATURE - it is a placeholder for a
 * missing MODULE, and the module is the largest single piece of work left in
 * the stack. When an nRF backend lands it must REPLACE this file, not sit
 * beside it: two definitions of radiant_radio_now() is a duplicate-symbol link
 * error, which is the right way for that mistake to be discovered.
 * radiant_core/CMakeLists.txt selects between them with RADIANT_CORE_BACKEND and
 * the comment there says so.
 *
 * ---------------------------------------------------------------------------
 * The capability block is not zeroed, and that is deliberate
 * ---------------------------------------------------------------------------
 * radiant_search.c computes its sweep geometry from caps.max_filters (8 filters ->
 * 32 sets on nRF, 2 -> 128 on RAIL); radiant_sched.c reads max_filters and
 * min_arm_lead_us on every pass; radiant_transfer_init() refuses outright a backend
 * whose min_arm_lead_us + RADIANT_TRANSFER_ACK_GUARD_US exceeds
 * RADIANT_TRANSFER_REPLY_US, or whose rx_to_tx_us exceeds it. A backend advertising
 * zeros would make all three of those decisions differently from the real one,
 * so the numbers here are the nRF52840's, and the footprint and the geometry a
 * core build reports are therefore the ones the real backend will produce.
 *
 * The two flags that are NOT flattered: has_sync_timestamp and has_rssi are
 * false, because this backend captures neither. radiant_event.c's default policy
 * suppresses the 0xE0 RX timestamp field entirely on an inexact stamp rather
 * than reporting a worse number as if it were the good one, which is exactly
 * the behaviour a null backend should provoke.
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

	/* One base address plus eight prefixes. This is the number the whole
	 * of radiant_search.c's sweep geometry and radiant_sched.c's merge width are
	 * computed from, and getting it wrong here would change both. */
	.max_filters = 8u,
	/* Two, matching the nRF backend rather than the arithmetic: this block
	 * exists so the modules above it compute the geometry they would
	 * compute on real hardware, and a null backend advertising an
	 * unconstrained matcher would let a merge bug through here and stop it
	 * on the bench. */
	.max_addr_groups = 2u,
	.filter_wildcard_dev = false,
	.addr_len_hw_max = 5u,
	.max_body_len = RADIANT_RADIO_BODY_MAX,

	.phys = null_phys,
	.n_phys = (uint8_t)(sizeof(null_phys) / sizeof(null_phys[0])),
	.phy_switch_us = 0u,

	/* Antenna-referenced ramp-up and turnaround. Nothing here measures
	 * them; they are the published nRF52840 figures, and they matter only
	 * because radiant_transfer_init() checks the acknowledged-data budget
	 * against them and would otherwise refuse or accept for the wrong
	 * reason. */
	.ramp_up_us = 140u,
	.rx_to_tx_us = 140u,
	.tx_to_rx_us = 140u,

	/* Software setup, DMA and clock start before an arm can be honoured.
	 * radiant_transfer_init() requires this plus RADIANT_TRANSFER_ACK_GUARD_US
	 * (250) to stay under RADIANT_TRANSFER_REPLY_US (1560), so anything up to
	 * 1310 is accepted; 300 is a realistic figure for a timer-driven arm on
	 * this part and leaves the check meaningful rather than vacuous. */
	.min_arm_lead_us = 300u,

	.time_resolution_ns = 1000u, /* the kernel tick converted to us */

	/*
	 * Both false, and neither is a shortcoming to be tidied away later.
	 * There is no address-capture event without a radio, so every t_sync
	 * this backend would report is an inference; radiant_radio_hal.h's rule is
	 * to degrade the advertised accuracy rather than report the worse
	 * number as the good one, and radiant_event.c honours that by suppressing
	 * the field.
	 */
	.has_sync_timestamp = false,
	.has_rssi = false,

	/* False, on the same terms: energy detect IS an RSSI measurement, and a
	 * backend with no receiver has nothing to measure. radiant_sched.c reads
	 * this before it posts a chunk, so an ED request against this build is
	 * refused at radiant_sched_request_ed() and never occupies a slot. */
	.has_ed_scan = false,

	/* Immaterial while nothing is received, but a real answer costs
	 * nothing: the nRF CRC engine can be made to cover the address bytes,
	 * which is the property that decides this on a given part. */
	.crc_in_hw = true,

	/* False, on the same terms as has_rssi above: this backend receives
	 * nothing, so it reports no received CRC and the core's single-bit
	 * repair never runs on it. Flattering this flag would put a repair path
	 * into a build whose whole purpose is to have no radio in it. */
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
	/* "Static for the lifetime of the program, never NULL, callable before
	 * init" - radiant_sched_init() and radiant_search_init() both rely on the last
	 * clause. */
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
	/* "Abort anything in flight, deliver its terminal event, and power
	 * down." Nothing is ever in flight here, so there is no terminal event
	 * to deliver and the callback table is never used - which is why
	 * null_cbs is stored but only read by the assertion below. */
	null_enabled = false;
	return RADIANT_RADIO_OK_RC;
}

radiant_time_t radiant_radio_now(void)
{
	/*
	 * A real monotonic microsecond clock, because a fake one would break
	 * things that are otherwise fine. The scheduler subtracts timestamps
	 * freely, radiant_search.c credits dwell in microseconds, radiant_channel.c
	 * derives every slot instant from this, and a clock that did not move
	 * would make the search sweep never advance and every deadline never
	 * expire - turning "the radio cannot transmit" into "the link layer is
	 * wedged", which is a much less useful thing to be looking at.
	 *
	 * k_uptime_ticks() rather than k_uptime_get(): the kernel tick is the
	 * finest resolution available and converting once here keeps the
	 * HAL's contract (absolute microseconds, never backwards) exact.
	 */
	return (radiant_time_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

/* ---------------------------------------------------------------------------
 * Operations - all of them refused
 * ---------------------------------------------------------------------------
 */

/*
 * RADIANT_RADIO_ENOTSUP, and specifically not RADIANT_RADIO_EIO or RADIANT_RADIO_ESTATE.
 *
 * The HAL defines ENOTSUP as "well-formed, but this backend cannot do it",
 * which is the literal truth, and radiant_sched.c's arm_failed() treats it the way
 * that keeps a scheduling pass terminating: the request is CONSUMED and its
 * owner is told RADIANT_SCHED_DONE_FAILED. EBUSY and ESTATE describe the radio
 * rather than the request, so the scheduler leaves those requests in the table
 * for the next tick - and against a backend that will never accept anything,
 * that is an unbounded retry every 50 ms for the life of the image.
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
