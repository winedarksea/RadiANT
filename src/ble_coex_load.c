/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ble_coex_load.c - a BLE advertiser whose only job is to want the radio.
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT THE HEART-RATE STRAP.
 *
 * The multiprotocol plan's P3 gate reads, in full: "`CONFIG_BT=y` with a
 * minimal advertiser: same bar". The bar is a loss figure, and a loss figure
 * measured against a second stack that is merely LINKED IN is a measurement of
 * nothing - it was taken, and it produced a sweep-rate ratio of exactly 1.000
 * across builds, because `CONFIG_BT=y` without a `bt_enable()` leaves the
 * SoftDevice Controller asleep and asking MPSL for no air whatsoever. A gate
 * that cannot fail is not a gate.
 *
 * So this is the smallest thing that makes the arbiter arbitrate: connectable
 * undirected advertising at a chosen interval, started once at boot and never
 * touched again. It has no services, no connection handling and no profile. The
 * heart-rate strap of the plan's second workstream - the SIG HRS beside ANT+
 * 0x78, with ANT+ suppressed once a connection is live - is a different piece of
 * work with its own state machine, and building it here would confuse "does
 * coexistence cost what we predicted" with "is the strap correct".
 *
 * THE INTERVAL IS THE KNOB THAT MATTERS. A BLE advertising event is three
 * channels of about 400 us plus ramp, so the demand this places on the arbiter
 * is roughly 1.3 ms per interval - about 4 % of the air at 30 ms and 0.4 % at
 * 300 ms. Kconfig-selectable rather than fixed, because the interesting question
 * is not whether ANT survives one setting but where it stops surviving, and a
 * hard-coded number answers that question once and then hides it.
 *
 * OFF UNLESS ASKED FOR. Selecting CONFIG_BT on this application does not start
 * an advertiser; CONFIG_ANT_DONGLE_BLE_COEX_LOAD does. A shipping dongle builds
 * neither.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ble_coex_load.h"

LOG_MODULE_REGISTER(ble_coex, LOG_LEVEL_INF);

/*
 * Units of 0.625 ms, which is the BLE advertising interval unit and not a
 * choice this file gets to make. The Kconfig value is in milliseconds because
 * the number a bench operator reasons about is milliseconds.
 */
#define ADV_INTERVAL_UNITS \
	((uint16_t)((CONFIG_ANT_DONGLE_BLE_COEX_LOAD_INTERVAL_MS * 1000) / 625))

static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static void coex_start_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(coex_start, coex_start_fn);

/*
 * DELAYED, AND THE DELAY IS AN INSTRUMENT RATHER THAN CAUTION.
 *
 * Starting the controller inside main() makes "ANT+ never worked" and "ANT+
 * stopped when BLE started" the same observation, because both are true from
 * the first millisecond. With the load arriving seconds later, the console
 * shows the sweep running, then the controller starting, and then whatever
 * happens next - which is the difference between an ordering bug at
 * initialisation and a genuine contention failure, and those need entirely
 * different fixes.
 *
 * It is also better behaviour: the dongle is a working dongle before the bench
 * load turns up, so a run that fails after the load still has a known-good
 * period in front of it to compare against.
 */
int ble_coex_load_start(void)
{
	return k_work_schedule(&coex_start,
			       K_SECONDS(CONFIG_ANT_DONGLE_BLE_COEX_LOAD_DELAY_S)) < 0
		       ? -EIO
		       : 0;
}

static void coex_start_fn(struct k_work *w)
{
	static const struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN, ADV_INTERVAL_UNITS, ADV_INTERVAL_UNITS,
		NULL);
	int rc;

	ARG_UNUSED(w);

	/*
	 * SYNCHRONOUS, AND AFTER antr_init(). bt_enable(NULL) blocks until the
	 * controller is up, which is what lets the log line below mean "the
	 * SoftDevice Controller is running" rather than "it has been asked to".
	 *
	 * The ordering against antr_init() is the same constraint main.c already
	 * documents for USB: whatever brings the radio up must own clock startup
	 * first. Here it is stronger than a preference - radiant_radio_nrf.c
	 * holds an explicit HFXO request that the P0 spike showed is load-bearing
	 * (without it the 1 MHz timebase runs on the RC between grants, 0.42 %
	 * fast, silently), and taking it before the controller exists is what
	 * keeps it held across everything the controller then does.
	 */
	LOG_INF("BLE coexistence load: enabling controller");
	rc = bt_enable(NULL);
	if (rc) {
		LOG_ERR("bt_enable failed: %d", rc);
		return;
	}

	rc = bt_le_adv_start(&param, adv_data, ARRAY_SIZE(adv_data), NULL, 0);
	if (rc) {
		LOG_ERR("bt_le_adv_start failed: %d", rc);
		return;
	}

	LOG_INF("BLE coexistence load: advertising every %d ms",
		CONFIG_ANT_DONGLE_BLE_COEX_LOAD_INTERVAL_MS);
}
