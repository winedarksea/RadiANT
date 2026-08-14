/* SPDX-License-Identifier: Apache-2.0 */

/*
 * An ANT+ sensor that does not exist, on a board that does.
 *
 * The dongle this repo builds is a transparent bridge with no profile
 * logic, so testing the radio needs a separate sensor. Kept as its own
 * application (not a dongle mode) because the root build's sysbuild/CMake
 * transport selection and CI flash-offset assertions are all dongle-
 * specific and would be wrong here.
 *
 *   west build app/apps/sim -b nrf54l15dk/nrf54l15/cpuapp --sysbuild -p auto
 *   scripts\flash_sim_jlink.ps1
 *
 * Runs on the nRF54L15 DK (no USB device controller in silicon, so it can
 * never be mistaken for a dongle), keeping the nRF5340 DK free for
 * debugging the thing under test.
 *
 * Profile plumbing is sdk-ant's (ant_bpwr/ant_bsc). sim_signal.c is ours:
 * the stock simulators ramp 0-2000 W, which never settles and so can't be
 * checked against a reference value; this holds a target with bounded
 * noise so tools/ant_verify.py can report a mean absolute error.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_ANT_SIM_IDENTITY_TIER_2)
#include <zephyr/random/random.h>
#endif

#include <ant_key_manager.h>
#include <ant_parameters.h>
#include <ant_state_indicator.h>
#include <dk_buttons_and_leds.h>

#if defined(CONFIG_ANT_SIM_PROFILE_BSC_COMBINED)
#include <ant_profiles/bsc/ant_bsc.h>
#else
#include <ant_profiles/bpwr/ant_bpwr.h>
#endif

#include "sim_signal.h"

LOG_MODULE_REGISTER(ant_sim, LOG_LEVEL_INF);

#define BUTTON_DECREASE DK_BTN1_MSK
#define BUTTON_INCREASE DK_BTN2_MSK

/* How much one button press moves the target. Small enough that a press is a
 * nudge rather than a new experiment.
 */
#define TARGET_STEP 5

/* Channel period in microseconds, computed from the profile's ticks rather
 * than written separately: the revolution counter uses this, and a mismatch
 * would be a cadence error indistinguishable from a receiver bug.
 */
#define TICKS_PER_S 32768u
#define PERIOD_US(ticks) ((uint32_t)(((uint64_t)(ticks) * 1000000u) / TICKS_PER_S))

static struct sim_signal sig_power;
static struct sim_signal sig_cadence;
static struct sim_revs crank_revs;
static struct sim_revs wheel_revs;

/* Buttons move these; the signal generators are re-seeded around them. */
static int32_t target_watts = CONFIG_ANT_SIM_TARGET_WATTS;
static int32_t target_rpm = CONFIG_ANT_SIM_TARGET_RPM;

/* The device number this boot reports. See sim_identity_init(). */
static uint16_t sim_devnum = CONFIG_ANT_SIM_DEVICE_NUMBER;

/*
 * Identity tier, resolved once at start-up.
 *
 * Tier 0 (default): provisioned number, never moves, no channel disruption.
 * Tier 2: fresh number every power-up - answers the stalking case (a strap
 * that power-cycles between rides looks like a new node each time) at the
 * cost of a re-pair per session for standard receivers (see Kconfig help).
 * Tier 1 isn't implemented here: no non-volatile storage, so a re-rolled
 * number can't survive a reset. Provision with tools/ant_identity.py.
 */
static void sim_identity_init(void)
{
#if defined(CONFIG_ANT_SIM_IDENTITY_TIER_2)
	/* 1..65535: zero is the wildcard on a channel ID, so a node that rolled
	 * it would match every search and belong to nobody. */
	sim_devnum = (uint16_t)((sys_rand32_get() % 65535u) + 1u);
	LOG_INF("identity tier 2: device #%u this boot - a STANDARD receiver "
		"must re-pair; a keyed RadiANT receiver re-acquires by MAC",
		sim_devnum);
#else
	LOG_INF("identity tier 0: device #%u, provisioned and stable",
		sim_devnum);
#endif
}

/* sdk-ant bakes the ANT+ frequency into a const channel-config macro, so
 * changing it means patching a copy. One instance is enough - a build only
 * ever opens one channel. At the default this is a no-op and the sensor
 * stays a valid ANT+ device; see CONFIG_ANT_SIM_RF_FREQ.
 */
static const ant_channel_config_t *sim_channel_config(
	const ant_channel_config_t *base)
{
	static ant_channel_config_t patched;

	patched = *base;
	patched.rf_freq = CONFIG_ANT_SIM_RF_FREQ;
	/* The device number is baked into sdk-ant's config macro at compile
	 * time, so a tier that chooses one at boot has to patch the copy - the
	 * same reason the frequency is patched here. */
	patched.device_number = sim_devnum;
	return &patched;
}

/*
 * Page 81's serial: a 32-bit globally unique serial, broadcast in the clear
 * every 30 s, is more identifying than the device number and unaffected by
 * a tier-2 re-roll - so a tier-2 node still leaks identity through it
 * unless privacy pages sentinel it out (0xFFFFFFFF, the page's own "not
 * supplied" value).
 */
#if defined(CONFIG_ANT_SIM_PRIVACY_PAGES)
#define SIM_SERIAL_NUM 0xFFFFFFFFu
#else
#define SIM_SERIAL_NUM CONFIG_ANT_SIM_SERIAL_NUM
#endif

/* Transmit power isn't part of the channel config, so it's set separately
 * once the channel exists. Logged rather than fatal: a sensor at default
 * power still works, so failing hard here would cost a bench session.
 */
static void sim_tx_power_set(uint8_t channel)
{
	int err = ant_channel_radio_tx_power_set(
		channel, CONFIG_ANT_SIM_TX_POWER, 0);

	if (err) {
		LOG_WRN("tx power level %d refused (%d) - staying at the "
			"default 0 dBm", CONFIG_ANT_SIM_TX_POWER, err);
	} else {
		LOG_INF("tx power level %d", CONFIG_ANT_SIM_TX_POWER);
	}
}

static void signals_init(void)
{
	sim_signal_init(&sig_power, target_watts, CONFIG_ANT_SIM_NOISE,
			CONFIG_ANT_SIM_SEED);
	/* A different seed for the second generator: sharing one would make
	 * cadence noise a copy of power noise, so their errors would cancel
	 * instead of compounding - flattering a receiver's torque arithmetic.
	 */
	sim_signal_init(&sig_cadence, target_rpm, CONFIG_ANT_SIM_NOISE,
			CONFIG_ANT_SIM_SEED + 1);
}

static void target_adjust(int32_t delta)
{
	target_watts += delta * (TARGET_STEP);
	target_rpm += delta;
	if (target_watts < 0) {
		target_watts = 0;
	}
	if (target_rpm < 0) {
		target_rpm = 0;
	}
	if (target_rpm > 254) {
		/* 255 is the profile's "cadence not reported" sentinel, so a
		 * target that reaches it would silently turn cadence off.
		 */
		target_rpm = 254;
	}

	signals_init();
	LOG_INF("target now %d W, %d rpm", target_watts, target_rpm);
}

#if defined(CONFIG_ANT_SIM_PROFILE_BSC_COMBINED)

/* ---- Combined speed and cadence, device type 0x79 ------------------------ */

#define SIM_PERIOD_US PERIOD_US(BSC_MSG_PERIOD_COMBINED)

static void bsc_evt_handler(ant_bsc_profile_t *p_profile, ant_bsc_evt_t event);

BSC_SENS_CHANNEL_CONFIG_DEF(bsc, CONFIG_ANT_SIM_CHANNEL_NUM,
			    CONFIG_ANT_SIM_TRANS_TYPE, BSC_COMBINED_DEVICE_TYPE,
			    CONFIG_ANT_SIM_DEVICE_NUMBER,
			    CONFIG_ANT_SIM_NETWORK_NUM);
BSC_SENS_PROFILE_CONFIG_DEF(bsc, true, true, ANT_BSC_COMB_PAGE_0,
			    bsc_evt_handler);

static ant_bsc_profile_t bsc;

static void bsc_update(ant_bsc_profile_t *p_profile)
{
	uint32_t rpm = (uint32_t)sim_signal_next(&sig_cadence);
	uint32_t crank = sim_revs_advance(&crank_revs, SIM_PERIOD_US, rpm);
	uint32_t wheel = sim_revs_advance(&wheel_revs, SIM_PERIOD_US,
					  CONFIG_ANT_SIM_WHEEL_RPM);

	/* Event time advances only when a revolution completed, not once per
	 * message. That is what makes roughly two messages in three carry no
	 * new event at 80 rpm - the case a receiver's delta arithmetic has to
	 * survive, and the one a per-message simulator never produces.
	 */
	while (crank-- > 0u) {
		p_profile->BSC_PROFILE_cadence_event_time +=
			sim_event_time_ticks(rpm);
		p_profile->BSC_PROFILE_cadence_rev_count++;
	}

	while (wheel-- > 0u) {
		p_profile->BSC_PROFILE_speed_event_time +=
			sim_event_time_ticks(CONFIG_ANT_SIM_WHEEL_RPM);
		p_profile->BSC_PROFILE_speed_rev_count++;
	}
}

static void bsc_evt_handler(ant_bsc_profile_t *p_profile, ant_bsc_evt_t event)
{
	switch (event) {
	case ANT_BSC_COMB_PAGE_0_UPDATED:
		bsc_update(p_profile);
		break;

	default:
		break;
	}
}

static void ant_evt_handler(ant_evt_t *p_ant_evt)
{
	ant_bsc_sens_evt_handler(p_ant_evt, &bsc);
}

static int profile_setup(void)
{
	int err = ant_bsc_sens_init(&bsc,
				    sim_channel_config(BSC_SENS_CHANNEL_CONFIG(bsc)),
				    BSC_SENS_PROFILE_CONFIG(bsc));

	if (err) {
		LOG_ERR("ant_bsc_sens_init failed: %d", err);
		return err;
	}

	bsc.BSC_PROFILE_manuf_id = CONFIG_ANT_SIM_MFG_ID;
	bsc.BSC_PROFILE_serial_num = SIM_SERIAL_NUM;
	bsc.BSC_PROFILE_hw_version = CONFIG_ANT_SIM_HW_VERSION;
	bsc.BSC_PROFILE_sw_version = CONFIG_ANT_SIM_SW_VERSION;
	bsc.BSC_PROFILE_model_num = CONFIG_ANT_SIM_MODEL_NUM;

	sim_tx_power_set(bsc.channel_number);

	err = ant_bsc_sens_open(&bsc);
	if (err) {
		LOG_ERR("ant_bsc_sens_open failed: %d", err);
		return err;
	}

	ant_state_indicator_channel_opened();
	LOG_INF("combined speed and cadence (0x79) open: device #%d, "
		"trans type %d, %d rpm crank, %d rpm wheel",
		sim_devnum, CONFIG_ANT_SIM_TRANS_TYPE,
		target_rpm, CONFIG_ANT_SIM_WHEEL_RPM);
	return 0;
}

static int indicator_setup(void)
{
	return ant_state_indicator_init(bsc.channel_number,
					BSC_SENS_CHANNEL_TYPE);
}

#else

/* ---- Bicycle power, device type 0x0B ------------------------------------ */

#define SIM_PERIOD_US PERIOD_US(BPWR_MSG_PERIOD)

#if defined(CONFIG_ANT_SIM_PROFILE_BPWR_WHEEL_TORQUE)
#define SIM_TORQUE_USE TORQUE_WHEEL
#elif defined(CONFIG_ANT_SIM_PROFILE_BPWR_CRANK_TORQUE)
#define SIM_TORQUE_USE TORQUE_CRANK
#else
#define SIM_TORQUE_USE TORQUE_NONE
#endif

static void bpwr_evt_handler(ant_bpwr_profile_t *p_profile,
			     ant_bpwr_evt_t event);
static void bpwr_calib_handler(ant_bpwr_profile_t *p_profile,
			       ant_bpwr_page1_data_t *p_page1);

BPWR_SENS_CHANNEL_CONFIG_DEF(bpwr, CONFIG_ANT_SIM_CHANNEL_NUM,
			     CONFIG_ANT_SIM_TRANS_TYPE,
			     CONFIG_ANT_SIM_DEVICE_NUMBER,
			     CONFIG_ANT_SIM_NETWORK_NUM);
BPWR_SENS_PROFILE_CONFIG_DEF(bpwr, SIM_TORQUE_USE, bpwr_calib_handler,
			     bpwr_evt_handler);

static ant_bpwr_profile_t bpwr;

static void bpwr_update_standard(ant_bpwr_profile_t *p_profile)
{
	uint16_t watts = (uint16_t)sim_signal_next(&sig_power);

	p_profile->BPWR_PROFILE_instantaneous_power = watts;
	/* Left to wrap at 65536 per the profile spec (receivers undo it in
	 * 16-bit arithmetic), rather than resetting to zero like the stock
	 * simulator, which puts a discontinuity on air no real sensor
	 * produces.
	 */
	p_profile->BPWR_PROFILE_accumulated_power += watts;
	p_profile->BPWR_PROFILE_instantaneous_cadence =
		(uint8_t)sim_signal_next(&sig_cadence);
	p_profile->BPWR_PROFILE_power_update_event_count++;
}

static void bpwr_update_torque(ant_bpwr_profile_t *p_profile)
{
	uint32_t rpm = (uint32_t)sim_signal_next(&sig_cadence);
	uint32_t watts = (uint32_t)sim_signal_next(&sig_power);

	/* The cadence byte always carries crank rpm, whichever torque page is
	 * being sent - a wheel-torque sensor still reports the rider's
	 * cadence, not the wheel's.
	 */
	p_profile->BPWR_PROFILE_instantaneous_cadence = (uint8_t)rpm;

#if defined(CONFIG_ANT_SIM_PROFILE_BPWR_WHEEL_TORQUE)
	/* Wheel revolutions, at road speed, not crank revolutions. Torque at
	 * the wheel is correspondingly lower for the same power, and a
	 * receiver that mixes the two up gets a number that is wrong by the
	 * gear ratio rather than obviously wrong.
	 */
	uint32_t revs = sim_revs_advance(&wheel_revs, SIM_PERIOD_US,
					 CONFIG_ANT_SIM_WHEEL_RPM);

	while (revs-- > 0u) {
		p_profile->BPWR_PROFILE_wheel_period +=
			sim_period_ticks(CONFIG_ANT_SIM_WHEEL_RPM);
		p_profile->BPWR_PROFILE_wheel_accumulated_torque +=
			sim_torque_ticks(watts, CONFIG_ANT_SIM_WHEEL_RPM);
		p_profile->BPWR_PROFILE_wheel_tick++;
		p_profile->BPWR_PROFILE_wheel_update_event_count++;
	}
#else
	uint32_t revs = sim_revs_advance(&crank_revs, SIM_PERIOD_US, rpm);

	while (revs-- > 0u) {
		p_profile->BPWR_PROFILE_crank_period += sim_period_ticks(rpm);
		p_profile->BPWR_PROFILE_crank_accumulated_torque +=
			sim_torque_ticks(watts, rpm);
		p_profile->BPWR_PROFILE_crank_tick++;
		p_profile->BPWR_PROFILE_crank_update_event_count++;
	}
#endif
}

static void bpwr_evt_handler(ant_bpwr_profile_t *p_profile,
			     ant_bpwr_evt_t event)
{
	switch (event) {
	case ANT_BPWR_PAGE_16_UPDATED:
		bpwr_update_standard(p_profile);
		break;

	case ANT_BPWR_PAGE_17_UPDATED:
	case ANT_BPWR_PAGE_18_UPDATED:
		bpwr_update_torque(p_profile);
		break;

	default:
		/* Pages 1, 80 and 81 are sent by the profile from fields set
		 * once at startup; there is nothing to advance.
		 */
		break;
	}
}

static void bpwr_calib_handler(ant_bpwr_profile_t *p_profile,
			       ant_bpwr_page1_data_t *p_page1)
{
	/* A display that asks a real power meter to calibrate expects an
	 * answer. Answering successfully keeps this looking like a sensor to
	 * anything that pairs with it, and costs one assignment.
	 */
	switch (p_page1->calibration_id) {
	case ANT_BPWR_CALIB_ID_MANUAL:
	case ANT_BPWR_CALIB_ID_AUTO:
		p_profile->BPWR_PROFILE_calibration_id =
			ANT_BPWR_CALIB_ID_MANUAL_SUCCESS;
		p_profile->BPWR_PROFILE_general_calib_data =
			CONFIG_ANT_SIM_CALIBRATION_DATA;
		break;

	default:
		break;
	}
}

static void ant_evt_handler(ant_evt_t *p_ant_evt)
{
	ant_bpwr_sens_evt_handler(p_ant_evt, &bpwr);
}

static int profile_setup(void)
{
	int err = ant_bpwr_sens_init(&bpwr,
				     sim_channel_config(BPWR_SENS_CHANNEL_CONFIG(bpwr)),
				     BPWR_SENS_PROFILE_CONFIG(bpwr));

	if (err) {
		LOG_ERR("ant_bpwr_sens_init failed: %d", err);
		return err;
	}

	bpwr.page_80 = ANT_COMMON_page80(CONFIG_ANT_SIM_HW_VERSION,
					 CONFIG_ANT_SIM_MFG_ID,
					 CONFIG_ANT_SIM_MODEL_NUM);
	bpwr.page_81 = ANT_COMMON_page81(CONFIG_ANT_SIM_SW_VERSION,
					 CONFIG_ANT_SIM_SW_VERSION,
					 SIM_SERIAL_NUM);
	bpwr.BPWR_PROFILE_auto_zero_status = ANT_BPWR_AUTO_ZERO_OFF;
	/* 0xFF is the profile's "not reported" value. Reporting a made-up
	 * balance would be a second signal to be wrong about for no gain.
	 */
	bpwr.BPWR_PROFILE_pedal_power.differentiation = 0x00;

	sim_tx_power_set(bpwr.channel_number);

	err = ant_bpwr_sens_open(&bpwr);
	if (err) {
		LOG_ERR("ant_bpwr_sens_open failed: %d", err);
		return err;
	}

	ant_state_indicator_channel_opened();
	LOG_INF("bicycle power (0x0B) open: device #%d, trans type %d, "
		"torque mode %d, %d W at %d rpm",
		sim_devnum, CONFIG_ANT_SIM_TRANS_TYPE,
		(int)SIM_TORQUE_USE, target_watts, target_rpm);
	return 0;
}

static int indicator_setup(void)
{
	return ant_state_indicator_init(bpwr.channel_number,
					BPWR_SENS_CHANNEL_TYPE);
}

#endif /* CONFIG_ANT_SIM_PROFILE_BSC_COMBINED */

static void button_changed(uint32_t button_state, uint32_t has_changed)
{
	if ((has_changed & BUTTON_INCREASE) && (button_state & BUTTON_INCREASE)) {
		target_adjust(+1);
	}
	if ((has_changed & BUTTON_DECREASE) && (button_state & BUTTON_DECREASE)) {
		target_adjust(-1);
	}
}

static int utils_setup(void)
{
	int err = indicator_setup();

	if (err) {
		LOG_ERR("ant_state_indicator_init failed: %d", err);
		return err;
	}

	err = dk_buttons_init(button_changed);
	if (err) {
		LOG_ERR("dk_buttons_init failed: %d", err);
		return err;
	}

	return 0;
}

static int ant_stack_setup(void)
{
	int err = ant_init();

	if (err) {
		LOG_ERR("ant_init failed: %d", err);
		return err;
	}
	LOG_INF("ANT version %s", ANT_VERSION_STRING);

	err = ant_cb_register(&ant_evt_handler);
	if (err) {
		LOG_ERR("ant_cb_register failed: %d", err);
		return err;
	}

	/* The ANT+ network key, supplied by sdk-ant - not the same thing as
	 * CONFIG_ANT_EVALUATION_KEY (the stack's licence key); conflating the
	 * two produces a build that runs and is never heard.
	 */
	err = ant_plus_key_set(CONFIG_ANT_SIM_NETWORK_NUM);
	if (err) {
		LOG_ERR("ant_plus_key_set failed: %d", err);
	}
	return err;
}

int main(void)
{
	int err;

	LOG_INF("ANT+ sensor simulator starting");

	/* Before profile_setup(), because the device number goes into the
	 * channel config and a channel config is read once. */
	sim_identity_init();

	signals_init();

	err = ant_stack_setup();
	if (err) {
		goto error_exit;
	}

	err = utils_setup();
	if (err) {
		goto error_exit;
	}

	err = profile_setup();
	if (err) {
		goto error_exit;
	}

	/* Everything from here runs out of the ANT event callback: the stack
	 * raises a page-updated event on each transmit, which is the correct
	 * moment to advance the simulated sensor - a k_sleep loop would drift.
	 */
	return 0;

error_exit:
	ant_state_indicator_fatal_error();
	k_oops();

	return 0;
}
