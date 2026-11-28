/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTMS (0x1826) and RSC (0x1814), beside two ANT+ masters on one radio.
 *
 * THE FTMS SERVICE ITSELF IS radiant/src/ble/radiant_ble_ftms.c, shared with
 * the other fitness-machine reference designs (<radiant/radiant_ble_ftms.h>:
 * the service, the Control Point and its pure decision function, Training
 * Status, the status notifications, Service Data). This file is the treadmill
 * plugged into it:
 *
 *   - the machine's capabilities and ranges (derived, treadmill_caps.c; the
 *     ranges are CONFIG_TREADMILL_MIN/MAX_*, the same values every command
 *     path is bounded by);
 *   - the Treadmill Data record and its Reset baseline (treadmill_ftms_data.c);
 *   - the actuators and the motion authorisation, which are treadmill_cmd.c -
 *     every one behind treadmill_motion_permitted();
 *   - the session (main.c) for Training Status and the epoch;
 *   - RSC, which NCS ships (nrf/subsys/bluetooth/services/rscs.c);
 *   - advertising through radiant_ble_adv, with the FTMS Service Data element;
 *   - the grade provider seam.
 *
 * Specification citations for the record are in treadmill_ftms_data.h; for
 * the Control Point and status rules, in radiant_ble_ftms.h.
 *
 * THE ADVERTISING PAYLOAD DOES NOT FIT WITH A NAME IN IT:
 *      flags                    1 + 1 + 1 =  3
 *      UUID16 list (two UUIDs)  1 + 1 + 4 =  6
 *      FTMS service data        1 + 1 + 5 =  7
 *                                          ----
 *                                            16, leaving N <= 13 for a name.
 * So THE NAME GOES IN THE SCAN RESPONSE; a scannable advertiser answers
 * SCAN_REQ, which costs radio time against the ANT+ masters (ADR 0017).
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <bluetooth/services/rscs.h>

#include <radiant/radiant_ble_adv.h>
#include <radiant/radiant_ble_dis.h>
#include <radiant/radiant_ble_ftms.h>
#if defined(CONFIG_RADIANT_CTRL_ARBITER)
#include <radiant/radiant_ctrl_arbiter.h>
#endif

#include "treadmill_ble.h"
#include "treadmill_cmd.h"
#include "treadmill_ftms_data.h"
#include "treadmill_session.h"
#include "treadmill_state.h"

LOG_MODULE_DECLARE(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

/* 0.625 ms units, which is what the advertising interval is expressed in. */
#define ADV_UNITS(ms) ((uint32_t)(((uint32_t)(ms) * 1000u) / 625u))

/* The ranges this machine advertises: the transport-neutral machine range. */
#define SPEED_MAX_CENTI_KPH ((int32_t)CONFIG_TREADMILL_MAX_SPEED_CENTI_KPH)
#define SPEED_STEP_CENTI_KPH 10u /* 0.10 km/h */
#define INCLINE_MIN_DECI ((int32_t)CONFIG_TREADMILL_MIN_INCLINE_DECI)
#define INCLINE_MAX_DECI ((int32_t)CONFIG_TREADMILL_MAX_INCLINE_DECI)
#define INCLINE_STEP_DECI 5u /* 0.5 % */

BUILD_ASSERT(TREADMILL_FTMS_DATA_MAX <= 20u,
	     "the Treadmill Data record must fit the 20-byte payload of a default "
	     "23-byte ATT_MTU; FTMS 4.19's split is deliberately never exercised");

static atomic_t connections;

/* The Zwift seam. Registered by nothing in this tree; see the header. */
static treadmill_ble_grade_provider_t grade_provider;
static void *grade_provider_user;

static uint32_t last_notify_ms;
static bool     rsc_notify_enabled;
static bool     heart_rate_field;

/* FTMS Reset's view: workqueue-owned (reset_view and encode both run there). */
static struct treadmill_ftms_baseline ftms_baseline;

/* ── The machine, as radiant_ble_ftms sees it ───────────────────────────── */

static enum radiant_ble_ftms_run ftms_run(enum treadmill_run run)
{
	switch (run) {
	case TREADMILL_RUN_RUNNING:
		return RADIANT_BLE_FTMS_RUN_RUNNING;
	case TREADMILL_RUN_PAUSED:
		return RADIANT_BLE_FTMS_RUN_PAUSED;
	case TREADMILL_RUN_STOPPED:
		return RADIANT_BLE_FTMS_RUN_STOPPED;
	case TREADMILL_RUN_SAFETY_STOP:
		return RADIANT_BLE_FTMS_RUN_SAFETY_STOP;
	default:
		return RADIANT_BLE_FTMS_RUN_IDLE;
	}
}

static void machine_session(struct radiant_ble_ftms_session *out, void *ctx)
{
	const struct treadmill_session *s = treadmill_node_session();

	ARG_UNUSED(ctx);
	out->run = ftms_run(s->run);
	out->epoch = s->epoch;
}

static uint32_t machine_epoch(void *ctx)
{
	ARG_UNUSED(ctx);
	return treadmill_node_epoch(); /* atomic: called on the BT RX thread */
}

static int machine_encode(uint8_t *buf, size_t cap, void *ctx)
{
	ARG_UNUSED(ctx);
	return treadmill_ftms_data_encode(treadmill_node_state(), &ftms_baseline,
					  heart_rate_field, buf, cap);
}

static void machine_reset_done(void *ctx)
{
	ARG_UNUSED(ctx);
	/* §4.16.2: the FTMS view's time and accumulated fields go to zero (the
	 * ANT+ accumulators do not move), and the session returns to Idle - the
	 * core sets Training Status to Idle with it. */
	treadmill_ftms_baseline_capture(&ftms_baseline, treadmill_node_state());
	treadmill_node_session_reset();
}

void treadmill_ble_target_incline_changed(int32_t centi_pct)
{
	uint8_t msg[3];
	uint16_t deci = (uint16_t)treadmill_ftms_incline_from_fec(centi_pct);

	msg[0] = RADIANT_BLE_FTMS_STATUS_TARGET_INCLINE;
	msg[1] = (uint8_t)(deci & 0xFFu);
	msg[2] = (uint8_t)(deci >> 8);
	radiant_ble_ftms_notify_status(msg, sizeof(msg));
}

static struct radiant_ble_ftms_machine ftms_machine = {
	.machine_type = RADIANT_BLE_FTMS_TYPE_TREADMILL,
	.ops = {
		.set_target = treadmill_cmd_ftms_set_target,
		.workout = treadmill_cmd_ftms_workout,
		.reset = treadmill_cmd_ftms_reset,
	},
	.auth = { .check = treadmill_cmd_ftms_auth },
	.encode_data = machine_encode,
	.session = machine_session,
	.epoch = machine_epoch,
	.reset_done = machine_reset_done,
};

void treadmill_ble_session_changed(enum treadmill_run prev, enum treadmill_run next,
				   bool revoke)
{
	radiant_ble_ftms_session_changed(ftms_run(prev), ftms_run(next));
	if (revoke) {
		/* A safety-key pull: 0x03 went out above, and the client that
		 * held control is now told it lost it (0xFF). */
		radiant_ble_ftms_revoke_control();
	}
}

static void notify_rsc(const struct treadmill_state *s)
{
	struct bt_rscs_measurement m;

	if (!rsc_notify_enabled) {
		return;
	}

	memset(&m, 0, sizeof(m));
	m.inst_speed = treadmill_rsc_speed_from_mm_s(s->belt_speed_mm_s);
	/*
	 * STEPS PER MINUTE, NOT STRIDES, and this is the one line of the whole
	 * application most likely to be wrong by a factor of two.
	 *
	 * rscs.h comments this field as "1 unit = 1 stride/minute", but
	 * Nordic's own sample (nrf/samples/bluetooth/peripheral_rscs) simulates
	 * 150-180, which is steps - strides would be 75-90 and no runner has a
	 * 170 stride/min gait. Steps is also what real clients expect. FE-C
	 * page 19 and ANT+ SDM both count STRIDES, so the conversion is a
	 * doubling and it lives in exactly one function with its own unit test.
	 */
	m.inst_cadence =
		treadmill_rsc_cadence_from_strides(s->cadence_strides_min_16);
	m.is_inst_stride_len = true;
	/* RSC's stride length is 100 units = 1 m, i.e. centimetres - the same
	 * unit as FE-C page 17's cycle length, which is the one piece of luck
	 * in this application. */
	m.inst_stride_length =
		treadmill_stride_length_cm(s->distance_mm, s->strides);
	m.is_total_distance = true;
	/* 0.1 m units - see treadmill_rsc_total_distance_dm(). Metres here
	 * reported a tenth of the distance run. */
	m.total_distance = treadmill_rsc_total_distance_dm(s->distance_mm);
	/* "Running" above about 2.2 m/s is the conventional split and it is a
	 * display hint, not a measurement. */
	m.is_running = (s->belt_speed_mm_s >= 2200u);

	(void)bt_rscs_measurement_send(NULL, &m);
}

void treadmill_ble_state_changed(const struct treadmill_state *s)
{
	uint32_t now;

	if (s == NULL || atomic_get(&connections) == 0) {
		return;
	}

	/* FTMS: "notify at a regular interval, typically once per second", not
	 * client-configurable. main.c calls this on every tick and report, so
	 * the pacing is here - and every notification is radio time against two
	 * ANT+ masters. */
	now = (uint32_t)k_uptime_get_32();
	if ((now - last_notify_ms) < CONFIG_TREADMILL_BLE_NOTIFY_INTERVAL_MS) {
		return;
	}
	last_notify_ms = now;

	/* The Zwift seam, asked here and answered by nothing in this tree. A
	 * MOTION PATH: treadmill_cmd_grade_provider() puts it behind the motion
	 * rule, and a refused value is dropped. */
	if (grade_provider != NULL) {
		int32_t grade = grade_provider(grade_provider_user);

		if (grade != INT32_MIN) {
			int32_t before = s->target_incline_centi_pct;

			/* Token: the same as FTMS's (treadmill_cmd_grade_token()). */
			if (treadmill_cmd_grade_provider(grade) == 0 &&
			    s->target_incline_centi_pct != before) {
				treadmill_ble_target_incline_changed(s->target_incline_centi_pct);
			}
		}
	}

	radiant_ble_ftms_notify_data();
	notify_rsc(s);
}

int treadmill_ble_grade_provider_register(treadmill_ble_grade_provider_t fn,
					  void *user)
{
	if (grade_provider != NULL) {
		return -EEXIST;
	}
	grade_provider = fn;
	grade_provider_user = user;
	return 0;
}

/* ---------------------------------------------------------------------------
 * Connections and advertising
 * ---------------------------------------------------------------------------
 *
 * Control permission and its release on disconnect are radiant_ble_ftms.c's;
 * this only counts connections for the notify gate.
 */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);
	if (err != 0u) {
		LOG_WRN("BLE connect failed (0x%02x)", err);
		return;
	}
	atomic_inc(&connections);
	LOG_INF("BLE connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	atomic_dec(&connections);
	LOG_INF("BLE disconnected (0x%02x)", reason);
	/* No restart here: radiant_ble_adv restarts from `recycled`. */
}

BT_CONN_CB_DEFINE(treadmill_ble_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* Table 3.1's Service Data element, built by radiant_ble_ftms_service_data()
 * at start: 0x1826, Flags (Fitness Machine Available), Fitness Machine Type
 * (Treadmill). FTMP, not FTMS, obliges advertising it - and it is how FTMS
 * clients filter, so omitting it makes the machine invisible to them. */
static uint8_t ftms_svc_data[RADIANT_BLE_FTMS_SVC_DATA_LEN];

static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_FMS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_RSCS_VAL)),
	BT_DATA(BT_DATA_SVC_DATA16, ftms_svc_data, sizeof(ftms_svc_data)),
};

/*
 * THE NAME IS IN THE SCAN RESPONSE, not the advertisement - see trap 7. That
 * buys a second 31-byte budget, so the name may be up to 29 characters:
 *
 *   name  1 + 1 + N <= 31   ->   N <= 29
 *
 * The failure mode if it does not fit is the one apps/hrm_ble documents:
 * bt_le_adv_start() returns -EINVAL, main.c's call site is
 * `(void)treadmill_ble_start()`, and the node transmits ANT+ perfectly while
 * never advertising, with the only evidence a log line on a board that may
 * have no console attached. Hence a BUILD_ASSERT.
 */
#define NAME_SUFFIX_BUDGET \
	(IS_ENABLED(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX) ? RADIANT_BLE_NAME_SUFFIX_MAX : 0u)

BUILD_ASSERT(sizeof(CONFIG_BT_DEVICE_NAME) - 1u + NAME_SUFFIX_BUDGET <= 29u,
	     "CONFIG_BT_DEVICE_NAME (plus the 6-character devnum suffix when "
	     "CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX is on) does not fit the "
	     "31-byte scan response: the budget is 29 characters (2 + N <= 31). "
	     "Over it, bt_le_adv_start() returns -EINVAL and the node silently "
	     "never advertises.");

/* And the advertisement's own sum, so that adding an AD structure is a build
 * error rather than a node that stops being discoverable. */
BUILD_ASSERT((3u + 6u + 7u) <= 31u,
	     "the advertising payload no longer fits 31 bytes: flags 3 + a "
	     "two-UUID16 list 6 + the FTMS Service Data element 7 = 16. "
	     "Anything added here comes out of nothing, because the name is "
	     "already in the scan response.");

/* The advertised name: CONFIG_BT_DEVICE_NAME, or "<name> <devnum>" once
 * treadmill_ble_start() has written it. */
static char adv_name[sizeof(CONFIG_BT_DEVICE_NAME) + RADIANT_BLE_NAME_SUFFIX_MAX] =
	CONFIG_BT_DEVICE_NAME;

#if defined(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX)
/* bt_set_name() refuses a name longer than its store with -ENOMEM. */
BUILD_ASSERT(sizeof(CONFIG_BT_DEVICE_NAME) - 1u + RADIANT_BLE_NAME_SUFFIX_MAX <=
	     CONFIG_BT_DEVICE_NAME_MAX,
	     "CONFIG_BT_DEVICE_NAME plus the devnum suffix exceeds "
	     "CONFIG_BT_DEVICE_NAME_MAX");
#endif

static struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, adv_name,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1u),
};

/*
 * ONE definition of what "advertising" means here, registered with
 * radiant_ble_adv at start-up and used by it for every restart. Two copies
 * would let the advertisers drift apart, and on this application family the
 * interval is the MPSL arbitration policy against the ANT+ masters, not a
 * power tweak.
 *
 * RESTARTING IS THE SHARED LAYER'S JOB (radiant/src/ble/radiant_ble_adv.c).
 * Zephyr stops a connectable advertiser on connection and does not resume it.
 * This file used to restart it once, from `disconnected`, with no retry -
 * found on apps/hrm_ble on real hardware (2026-08-16) to leave the node
 * invisible until power-cycled, while ANT+ kept every ANT-side check green.
 * The layer restarts from `recycled` (when the connection object is free
 * again), retries with a backoff until it succeeds, and treadmill_ble_tick()
 * runs its watchdog.
 *
 * A NON-NULL `sd` makes this advertiser scannable - see trap 7 and ADR 0017
 * for what it costs.
 */
static struct bt_le_adv_param adv_param;

void treadmill_ble_tick(void)
{
	radiant_ble_adv_watchdog();
}

bool treadmill_ble_connected(void)
{
	return atomic_get(&connections) != 0;
}

static void rscs_evt_handler(enum bt_rscs_evt evt)
{
	rsc_notify_enabled = (evt == RSCS_EVT_MEAS_NOTIFY_ENABLE);
}

static int rscs_set_cumulative(uint32_t total_distance)
{
	/* The one SC Control Point procedure worth implementing: a client
	 * setting the cumulative distance. Refused rather than obeyed, and the
	 * refusal is the honest answer - this node's distance accumulator is
	 * shared with two ANT+ masters and a BLE client rewriting it would
	 * make the same treadmill report two different totals on two radios. */
	ARG_UNUSED(total_distance);
	return -ENOTSUP;
}

static const struct bt_rscs_cp_cb rscs_cp_cb = {
	.set_cumulative = rscs_set_cumulative,
};

int treadmill_ble_start(uint16_t devnum, uint32_t serial, const struct treadmill_caps *caps)
{
	struct bt_rscs_init_params rscs_params;
	int                        err;

	/* The FTMS machine, before anything can connect: what it advertises is
	 * what its driver can do (treadmill_caps.c). */
	heart_rate_field = caps->heart_rate;
	ftms_machine.caps.features = caps->ftms_features;
	ftms_machine.caps.targets = caps->ftms_targets;
	ftms_machine.caps.speed = (struct radiant_ble_ftms_range){
		.min = 0, .max = SPEED_MAX_CENTI_KPH, .step = SPEED_STEP_CENTI_KPH,
		.valid = true,
	};
	ftms_machine.caps.incline = (struct radiant_ble_ftms_range){
		.min = INCLINE_MIN_DECI, .max = INCLINE_MAX_DECI, .step = INCLINE_STEP_DECI,
		.valid = true,
	};
	err = radiant_ble_ftms_register(&ftms_machine);
	if (err != 0) {
		LOG_ERR("radiant_ble_ftms_register: %d", err);
		return err;
	}
	(void)radiant_ble_ftms_service_data(ftms_svc_data, sizeof(ftms_svc_data), true,
					    RADIANT_BLE_FTMS_TYPE_TREADMILL);

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		/*
		 * Bonds live in settings. Without this call a pairing survives
		 * only until reboot, which on a machine whose control point
		 * requires encryption means every power cycle costs the user a
		 * re-pair.
		 *
		 * IT ALSO FINISHES bt_enable(). With CONFIG_BT_SETTINGS=y -
		 * which only this application in the tree sets - Zephyr defers
		 * part of bt_id_init() out of bt_enable() into the settings
		 * commit, and says so at boot: "No ID address. App must call
		 * settings_load()". Skip it and the stack has no identity and
		 * bt_le_adv_start() fails. So this return is not only about
		 * bonds; it is the difference between advertising and not.
		 *
		 * THE RETURN IS CHECKED AND THE FAILURE IS NOT FATAL, which is
		 * the deliberate half. A machine that cannot store a bond is
		 * still far more useful than one that is invisible: it
		 * advertises, a phone can connect, and the pairing simply has
		 * to be redone next boot. Returning here instead would turn a
		 * corrupt settings partition into a node with no BLE at all.
		 * If the identity really did not come up, bt_le_adv_start()
		 * below fails and says so with its own error.
		 *
		 * THIS APPLICATION LOADS SETTINGS TWICE, in an order unique to
		 * it: radiant_node_ident_boot() has already run settings_subsys_init()
		 * + settings_load_subtree() from main() BEFORE bt_enable(),
		 * and this is a second, whole-tree load after it. Both are
		 * correct and neither is redundant - the first predates the
		 * Bluetooth handlers existing, and the `bt/` subtree this one
		 * commits is registered by bt_enable().
		 */
		err = settings_load();
		if (err != 0) {
			LOG_ERR("settings_load: %d - bonds will not persist "
				"across a reboot, and if the Bluetooth identity "
				"failed to commit with it, bt_le_adv_start() "
				"below is what will say so", err);
		}
	}

	memset(&rscs_params, 0, sizeof(rscs_params));
	rscs_params.location = RSC_LOC_OTHER; /* a treadmill is not a foot pod */
	rscs_params.supported_locations.other = 1;
	rscs_params.features.inst_stride_len = 1;
	rscs_params.features.total_distance = 1;
	rscs_params.features.walking_or_running = 1;
	rscs_params.evt_handler = rscs_evt_handler;
	rscs_params.cp_cb = &rscs_cp_cb;

	/*
	 * CONFIG_BT_RSCS=y PUBLISHES THE SERVICE WHETHER OR NOT THIS IS
	 * CALLED - rscs.c uses a static BT_GATT_SERVICE_DEFINE - so a build
	 * that enabled the symbol and forgot the init would advertise a
	 * service whose features and location are whatever zeroed memory says.
	 * Failing loudly here is this repository's house answer to that.
	 *
	 * Note it also publishes Sensor Location (0x2A5D) and SC Control Point
	 * (0x2A55) whether a treadmill wants them or not.
	 */
	err = bt_rscs_init(&rscs_params);
	if (err != 0) {
		LOG_ERR("bt_rscs_init: %d - the RSC service is published by "
			"CONFIG_BT_RSCS regardless, so this build would "
			"advertise an uninitialised one", err);
		return err;
	}

	/* DIS from radiant_product and the unit's serial - after
	 * settings_load() above, which would otherwise replace the values with
	 * any stored ones. A failure keeps the Kconfig strings and is not a
	 * reason to stay invisible. */
	(void)radiant_ble_dis_apply(serial);

#if defined(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX)
	{
		int n = radiant_ble_name_with_devnum(adv_name, sizeof(adv_name),
						     CONFIG_BT_DEVICE_NAME, devnum);

		if (n > 0) {
			sd[0].data_len = (uint8_t)n;
			err = bt_set_name(adv_name);
			if (err != 0) {
				LOG_WRN("bt_set_name(%s): %d", adv_name, err);
			}
		}
	}
#else
	ARG_UNUSED(devnum);
#endif

	adv_param = *BT_LE_ADV_CONN_FAST_1;
	adv_param.interval_min = ADV_UNITS(CONFIG_TREADMILL_BLE_ADV_INTERVAL_MS);
	adv_param.interval_max = ADV_UNITS(CONFIG_TREADMILL_BLE_ADV_INTERVAL_MS) +
				 16u;

	err = radiant_ble_adv_init(&adv_param, ad, ARRAY_SIZE(ad), sd,
				   ARRAY_SIZE(sd));
	if (err != 0) {
		LOG_ERR("radiant_ble_adv_init: %d", err);
		return err;
	}

	/* A failure here is not BLE being dead: the layer has already
	 * scheduled a retry and keeps retrying until advertising starts. So
	 * it is a warning and this returns 0 - only a fatal step above (the
	 * controller not coming up) may make the caller report "ANT+ only". */
	err = radiant_ble_adv_ensure();
	if (err != 0) {
		LOG_WRN("BLE advertising not started yet (%d); retrying in the "
			"background", err);
		return 0;
	}

	LOG_INF("BLE: FTMS (targets 0x%08x) + RSC advertising every %u ms as '%s'",
		(unsigned)caps->ftms_targets,
		(unsigned)CONFIG_TREADMILL_BLE_ADV_INTERVAL_MS, adv_name);
	return 0;
}
