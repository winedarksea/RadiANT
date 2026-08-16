/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadiANT treadmill: one node, two ANT+ masters, two BLE services, one radio.
 *
 * It is apps/hrm_ble's shape with the parts a treadmill needs that a chest
 * strap does not, and the differences are the interesting part:
 *
 *   TWO ANT+ MASTERS, NOT ONE. FE-C (0x11, 8192 counts) is what a
 *   control-capable head unit pairs with; SDM (0x7C, 8134 counts) is what
 *   anything that only knows foot pods pairs with, Zwift Run included. They
 *   are different device types with different periods and no shared page
 *   space, so they are two channels and two profile_sched instances - not one
 *   channel with a wider page set. CONFIG_TREADMILL_ANT_SDM turns the second
 *   one off, because two masters beating against each other at 4.00 and
 *   4.03 Hz beside BLE is more contention than anything this project has
 *   measured, and the fallback has to exist before it is needed.
 *
 *   BIDIRECTIONAL. A strap is told nothing. Here, acknowledged control pages
 *   arrive from a head unit and the FTMS control point arrives from a phone,
 *   and BOTH land on treadmill_source_set_incline() - one function, one state,
 *   so a grade set over ANT+ is immediately visible over BLE and the other way
 *   round. That symmetry is the main thing worth getting right in this
 *   application and the main thing worth testing.
 *
 *   AND EVERY COMMAND OWES AN ANSWER. FE-C §8.8 makes a page 71 command status
 *   the obligation, not a courtesy: getting it wrong is the difference between
 *   a machine that ignores you and a machine a controller reports as broken.
 *   That is why every branch of handle_control_page() ends in a
 *   profile_fec_tx_command_answered() call, including the branches that refuse.
 *
 *   NO SUPPRESSION RULE. apps/hrm_ble stops ANT+ while a phone is connected,
 *   on the argument that the phone already has the heart rate. That argument
 *   does not transfer: a treadmill is expected to serve a head unit and a
 *   fitness app at the same time, so the contended case is the NORMAL case
 *   here rather than the stress case, and there is deliberately no
 *   TREADMILL_SUPPRESS_ANT_WHEN_CONNECTED.
 *
 * See docs/treadmill-reference-design.md for the porting guide, the production
 * checklist and the bench procedure; src/treadmill_source.h for the one seam a
 * manufacturer implements; and src/treadmill_state.h for why there is exactly
 * one copy of the physics.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_radio.h"
#include "ant_wire.h"
#include "node_ident.h"
#include "profile_common.h"
#include "profile_fec.h"
#include "profile_fec_tx.h"
#include "profile_sdm.h"
#include "treadmill_source.h"
#include "treadmill_state.h"

#if defined(CONFIG_TREADMILL_BLE)
#include "treadmill_ble.h"
#endif
#if defined(CONFIG_TREADMILL_HR_RX)
#include "treadmill_hr_rx.h"
#endif

LOG_MODULE_REGISTER(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

/*
 * The public ANT+ network key, used by every ANT+ device on the air - not the
 * stack licence key (an sdk-ant concept radiant does not have). Confusing the
 * two produces a node that runs and is heard by nothing.
 */
static const uint8_t ant_plus_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

#define ANT_NETWORK       0u
#define ANT_RF_FREQ       57u
#define ANT_TRANS_TYPE    5u

#define FEC_CHANNEL   0u
#define SDM_CHANNEL   1u
/* Phase 5's heart-rate slave. Channel 2 whether or not SDM is compiled in, so
 * a channel number in a log means the same thing in every build. */
#define HR_RX_CHANNEL 2u

/* The node's housekeeping cadence. Not the page cadence and not the source's:
 * the pages are paced by EVENT_TX and the source ticks on its own. */
#define NODE_TICK_MS 100u

/* SDM page 1's update-latency field, in 1/32 s: how stale the reading in this
 * message may be. It is the node tick, converted once. */
#define SDM_LATENCY_32 ((NODE_TICK_MS * 32u) / 1000u)

static struct treadmill_state  state;
static struct profile_fec_tx   fec;
#if defined(CONFIG_TREADMILL_ANT_SDM)
static struct profile_sdm      sdm;
#endif

#if defined(CONFIG_TREADMILL_FEC_METABOLIC)
/* For the caloric-burn-rate field of FE-C page 18, which is a RATE and this
 * project only ever computes a total. */
static uint64_t last_energy_mkcal;
#endif

static void node_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(node_tick, node_tick_fn);

/* ---------------------------------------------------------------------------
 * State -> pages. The one place the SI-unit truth becomes wire fields.
 * ---------------------------------------------------------------------------
 */

static void publish_fec(void)
{
	struct profile_fec_general   g;
	struct profile_fec_settings  s;
	struct profile_fec_treadmill t;

	memset(&g, 0, sizeof(g));
	g.equipment_type = PROFILE_FEC_TYPE_TREADMILL;
	/* 0.25 s units in a u8, so it rolls every 64 s - and is MEANT to. A
	 * receiver differences it. */
	g.elapsed_time_qs = (uint8_t)((state.elapsed_ms / 250u) & 0xFFu);
	g.distance_m = (uint8_t)((state.distance_mm / 1000u) & 0xFFu);
	g.distance_valid = true; /* capabilities bit 2, not a sentinel */
	g.speed_mm_s = (uint16_t)state.belt_speed_mm_s;
	g.speed_valid = true;
	g.speed_virtual = false; /* a belt speed is a measurement of something */
	g.heart_rate_bpm = state.heart_rate_bpm;
	g.heart_rate_valid = (state.heart_rate_bpm != TREADMILL_HR_NONE);
	g.hr_source = state.hr_from_ant ? PROFILE_FEC_HR_SRC_ANTPLUS
					: PROFILE_FEC_HR_SRC_INVALID;
	g.state = state.state;
	g.lap_toggle = state.lap_toggle;
	profile_fec_tx_set_general(&fec, &g);

	memset(&s, 0, sizeof(s));
	s.cycle_length_cm = treadmill_stride_length_cm(state.distance_mm,
						       state.strides);
	s.cycle_length_valid = (s.cycle_length_cm != 0u);
	/* §10.1.2.1: the incline field SHALL be supported on a treadmill, and
	 * §8.8.4 asks it to match the grade a page 51 command carried. It is
	 * reported from where the deck ACTUALLY IS, which during an actuator
	 * ramp is not yet the target - claiming otherwise would be reporting a
	 * position the machine has not reached. */
	s.incline_centi_pct = state.incline_centi_pct;
	s.incline_valid = true;
	s.resistance_valid = false; /* a treadmill has no resistance level */
	s.state = state.state;
	s.lap_toggle = state.lap_toggle;
	profile_fec_tx_set_settings(&fec, &s);

	memset(&t, 0, sizeof(t));
	t.cadence_strides_min =
		treadmill_cadence_strides_min(state.cadence_strides_min_16);
	t.cadence_valid = true; /* 0 is a real standing runner, not "unknown" */
	/* 0.1 m units from millimetres. Both accumulators are unsigned and both
	 * count UP - the negative one is a magnitude of descent. */
	t.neg_vertical_dm = (uint8_t)((state.neg_vertical_mm / 100u) & 0xFFu);
	t.pos_vertical_dm = (uint8_t)((state.pos_vertical_mm / 100u) & 0xFFu);
	t.capabilities = PROFILE_FEC_TREADMILL_CAP_NEG_VERTICAL |
			 PROFILE_FEC_TREADMILL_CAP_POS_VERTICAL;
	t.state = state.state;
	t.lap_toggle = state.lap_toggle;
	profile_fec_tx_set_treadmill(&fec, &t);

#if defined(CONFIG_TREADMILL_FEC_METABOLIC)
	{
		struct profile_fec_metabolic m;
		uint64_t                     delta;

		memset(&m, 0, sizeof(m));
		delta = state.energy_mkcal - last_energy_mkcal;
		last_energy_mkcal = state.energy_mkcal;
		/* milli-kcal per tick -> 0.1 kcal/h:
		 * delta/1000 kcal per NODE_TICK_MS ms is
		 * delta * 3600000 / (1000 * NODE_TICK_MS) kcal/h, x10 for the
		 * field = delta * 36000 / NODE_TICK_MS. */
		{
			uint64_t rate = (delta * 36000u) / NODE_TICK_MS;

			m.burn_rate_deci = (rate > 0xFFFEu) ? 0xFFFEu
							    : (uint16_t)rate;
			m.burn_rate_valid = true;
		}
		/* METs are deliberately NOT reported. The estimate behind the
		 * calorie total is a population-average equation with no
		 * measurement in it (see treadmill_state.c), and a METs field
		 * is read as a physiological statement about the person on the
		 * belt in a way that a calorie counter on a console is not. */
		m.mets_valid = false;
		m.calories = (uint8_t)((state.energy_mkcal / 1000u) & 0xFFu);
		m.capabilities = PROFILE_FEC_METABOLIC_CAP_CALORIES;
		m.state = state.state;
		m.lap_toggle = state.lap_toggle;
		profile_fec_tx_set_metabolic(&fec, &m);
	}
#endif
}

#if defined(CONFIG_TREADMILL_ANT_SDM)
static void publish_sdm(void)
{
	profile_sdm_set_motion(&sdm, state.belt_speed_mm_s,
			       treadmill_cadence_strides_min(
				       state.cadence_strides_min_16),
			       (uint8_t)(state.cadence_strides_min_16 & 0x0Fu),
			       state.state == TREADMILL_STATE_IN_USE);
	profile_sdm_set_accumulators(&sdm, state.elapsed_ms, state.distance_mm,
				     state.strides, SDM_LATENCY_32);
	profile_sdm_set_calories(&sdm,
				 (uint8_t)((state.energy_mkcal / 1000u) & 0xFFu));
}
#endif

static void publish(void)
{
	publish_fec();
#if defined(CONFIG_TREADMILL_ANT_SDM)
	publish_sdm();
#endif
#if defined(CONFIG_TREADMILL_BLE)
	treadmill_ble_state_changed(&state);
#endif
}

/* ---------------------------------------------------------------------------
 * The seam's other half
 * ---------------------------------------------------------------------------
 *
 * RUNS ON THE SYSTEM WORKQUEUE, which is the whole reason it is a separate
 * function rather than being done inside treadmill_source_report_*(). The
 * profile modules have no lock and are already reached from the radiant
 * callback path; doing the state work here keeps that at two contexts however
 * the source is driven, including from an ISR. treadmill_source.h states the
 * rule in full.
 */
void treadmill_node_deliver(uint32_t strides)
{
	treadmill_state_set_speed(&state, treadmill_source_last_speed());
	treadmill_state_set_incline(&state, treadmill_source_last_incline());
	treadmill_state_set_cadence_16(&state,
				       treadmill_source_last_cadence_16());
	treadmill_state_set_state(&state, treadmill_source_last_state());
	treadmill_state_add_strides(&state, strides);

	publish();
}

/* ---------------------------------------------------------------------------
 * The control path: FE-C acknowledged pages in, page 71 out
 * ---------------------------------------------------------------------------
 */

/*
 * Map the seam's three answers onto FE-C's, which is the whole reason
 * treadmill_source_set_incline() has three of them.
 *
 *    0        -> PASS           the actuator is on its way
 *   -ENOTSUP  -> NOT_SUPPORTED  this machine has a fixed deck
 *    other    -> FAIL           a fault, an interlock, a user override
 *
 * -ENODEV (no source registered) becomes FAIL rather than NOT_SUPPORTED: the
 * machine's capabilities have not changed, its driver is simply missing, and
 * telling a controller the feature does not exist would make an integrator's
 * unfinished build look like a fixed-incline product.
 */
static uint8_t fec_status_for(int rc)
{
	if (rc == 0) {
		return PROFILE_FEC_CMD_STATUS_PASS;
	}
	if (rc == -ENOTSUP) {
		return PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
	}
	return PROFILE_FEC_CMD_STATUS_FAIL;
}

/*
 * One acknowledged control page.
 *
 * `body` is the eight-byte payload. EVERY path through this function ends in
 * profile_fec_tx_command_answered() - including the unrecognised-page path,
 * because §8.8 wants a page 71 saying NOT_SUPPORTED rather than a silence, and
 * silence is what a controller reports as a broken machine.
 *
 * THERE IS NO SEQUENCE NUMBER IN AN FE-C CONTROL PAGE. Unlike ANT+ Controls
 * (0x10), whose pages carry one, the FE-C command pages do not - so page 71's
 * sequence field is the FE's own count of commands received, which is what
 * lets a controller tell "you answered my command" from "you re-sent the
 * answer to the previous one".
 */
static void handle_control_page(const uint8_t *body)
{
	static uint8_t sequence;
	uint8_t        status = PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
	uint8_t        page = body[0];

	sequence++;

	switch (page) {
	case PROFILE_FEC_PAGE_TRACK_RESISTANCE: {
		struct profile_fec_cmd_track_resistance cmd;
		int32_t                                 adopted;
		int                                     rc;

		if (profile_fec_decode_track_resistance(body, &cmd) != 0) {
			break;
		}
		/* cmd.grade_centi_pct is already 0 when the controller sent the
		 * 0xFFFF sentinel, which §8.8.4.1 defines as "assume flat" -
		 * not as "no command". Byte 7's rolling resistance is decoded
		 * and deliberately not acted on (§8.8.4.2). */
		adopted = treadmill_state_request_incline(&state,
							  cmd.grade_centi_pct);
		rc = treadmill_source_set_incline(adopted);
		status = fec_status_for(rc);
		LOG_INF("FE-C page 51: grade %d.%02d %% -> %s",
			adopted / 100, (adopted < 0 ? -adopted : adopted) % 100,
			(status == PROFILE_FEC_CMD_STATUS_PASS) ? "pass"
								: "refused");
		break;
	}

	case PROFILE_FEC_PAGE_WIND_RESISTANCE:
		/* Part of the simulation-mode parameter set, and a treadmill
		 * has nothing to do with any of it. Accepted and acknowledged
		 * rather than refused: refusing one parameter of a set the
		 * machine advertises support for is how a controller decides
		 * simulation mode is broken and stops sending page 51 too. */
		status = PROFILE_FEC_CMD_STATUS_PASS;
		break;

	case PROFILE_FEC_PAGE_USER_CONFIG: {
		struct profile_fec_cmd_user_config cmd;

		if (profile_fec_decode_user_config(body, &cmd) == 0 &&
		    cmd.user_weight_valid) {
			treadmill_state_set_user_mass(&state,
						      cmd.user_weight_10g);
			status = PROFILE_FEC_CMD_STATUS_PASS;
		} else {
			status = PROFILE_FEC_CMD_STATUS_FAIL;
		}
		break;
	}

	case PROFILE_FEC_PAGE_REQUEST: {
		struct profile_fec_cmd_request cmd;

		if (profile_fec_decode_request(body, &cmd) == 0 &&
		    profile_fec_tx_request(&fec, cmd.page, cmd.tx_count) == 0) {
			status = PROFILE_FEC_CMD_STATUS_PASS;
		}
		break;
	}

	case PROFILE_FEC_PAGE_BASIC_RESISTANCE:
	case PROFILE_FEC_PAGE_TARGET_POWER:
		/* Page 54 declares neither Basic Resistance mode nor Target
		 * Power mode, so a conforming controller never sends these -
		 * and one that does gets the answer the declaration implies.
		 * The pairing is the point: declaring a capability and
		 * honouring its command page go together in both directions. */
		status = PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
		break;

	default:
		status = PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
		break;
	}

	/* Bytes [4..7] of the command, echoed position for position, so a
	 * controller reads page 51's grade back at the offsets it wrote it. */
	profile_fec_tx_command_answered(&fec, page, sequence, status, &body[4]);
}

#if defined(CONFIG_TREADMILL_BLE)
/*
 * The FTMS control point's incline command, called from treadmill_ble.c.
 *
 * The SAME two lines as the FE-C branch above - request, then command the
 * source - which is exactly the point: one state, one actuator call, so the
 * two radios cannot disagree about the grade. The only difference is the unit,
 * and the conversion lives in treadmill_state.h with its own unit test rather
 * than being spelled out at either call site.
 */
int treadmill_node_set_incline_deci(int16_t deci_pct, int32_t *adopted_centi)
{
	int32_t centi = treadmill_fec_incline_from_ftms(deci_pct);
	int32_t adopted = treadmill_state_request_incline(&state, centi);
	int     rc = treadmill_source_set_incline(adopted);

	if (adopted_centi != NULL) {
		*adopted_centi = adopted;
	}
	return rc;
}

int treadmill_node_set_speed_mm_s(uint32_t mm_s, uint32_t *adopted_mm_s)
{
	uint32_t adopted = treadmill_state_request_speed(&state, mm_s);
	int      rc = treadmill_source_set_speed(adopted);

	if (adopted_mm_s != NULL) {
		*adopted_mm_s = adopted;
	}
	return rc;
}

const struct treadmill_state *treadmill_node_state(void)
{
	return &state;
}
#endif /* CONFIG_TREADMILL_BLE */

#if defined(CONFIG_TREADMILL_HR_RX)
/* Phase 5: an ANT+ strap's bpm, from treadmill_hr_rx.c on the workqueue. */
void treadmill_node_heart_rate(uint8_t bpm)
{
	treadmill_state_set_heart_rate(&state, bpm, true);
	publish();
}
#endif

/* ---------------------------------------------------------------------------
 * The radio
 * ---------------------------------------------------------------------------
 */

/*
 * Load the next page for one channel, paced by EVENT_TX rather than a wall
 * clock: the stack raises it exactly when a payload just went on the air,
 * which is when the next one should be staged. A timer-driven load would drift
 * against the channel period and, under an arbiter, stage for slots never
 * granted.
 */
static void load_fec_page(void)
{
	uint8_t body[8];

	if (profile_fec_tx_next(&fec, body) == PROFILE_SLOT_IDLE) {
		return;
	}
	(void)antr_broadcast_message_tx(FEC_CHANNEL, sizeof(body), body);
}

#if defined(CONFIG_TREADMILL_ANT_SDM)
static void load_sdm_page(void)
{
	uint8_t body[8];

	if (profile_sdm_next(&sdm, body) == PROFILE_SLOT_IDLE) {
		return;
	}
	(void)antr_broadcast_message_tx(SDM_CHANNEL, sizeof(body), body);
}
#endif

/*
 * The backend's only way up. A dongle forwards this to USB; a node is the
 * application the messages were for.
 */
void antr_on_message(const struct antr_msg *msg)
{
	if (msg == NULL || msg->data == NULL) {
		return;
	}

#if defined(CONFIG_TREADMILL_HR_RX)
	/* First refusal to the slave, because it owns a channel of its own and
	 * every test below is keyed on a channel number this one is not. */
	if (treadmill_hr_rx_message(msg)) {
		return;
	}
#endif

	if (msg->id == ANTW_MESG_RESPONSE_EVENT_ID &&
	    msg->len >= ANTW_MESG_RESPONSE_EVENT_SIZE) {
		/* data[0] channel, data[1] the message id being responded to
		 * (0 for an event), data[2] the code. */
		if (msg->data[2] != ANTW_EVENT_TX) {
			return;
		}
		if (msg->data[0] == FEC_CHANNEL) {
			load_fec_page();
		}
#if defined(CONFIG_TREADMILL_ANT_SDM)
		else if (msg->data[0] == SDM_CHANNEL) {
			load_sdm_page();
		}
#endif
		return;
	}

	/*
	 * THE CONTROL PAGES ARRIVE HERE, as ACKNOWLEDGED data on a channel this
	 * node is the MASTER of. That is not the usual direction and it is the
	 * whole of FE-C's "C": a controller sends an acknowledged message into
	 * a master's channel and the master answers on its next page 71.
	 *
	 * radiant/src/radiant_burst.c refuses at config time any backend that
	 * cannot acknowledge, which is the right behaviour and means this path
	 * is not portable to every backend for free. It is worth knowing that
	 * before the first control page rather than at it.
	 */
	if ((msg->id == ANTW_MESG_ACKNOWLEDGED_DATA_ID ||
	     msg->id == ANTW_MESG_EXT_ACKNOWLEDGED_DATA_ID) &&
	    msg->len >= 9u && msg->data[0] == FEC_CHANNEL) {
		handle_control_page(&msg->data[1]);
	}
}

static int ant_master_start(uint8_t channel, uint16_t devnum,
			    uint8_t device_type, uint16_t period,
			    void (*stage)(void))
{
	antr_err_t rc;

	rc = antr_channel_assign(channel, ANTW_CHANNEL_TYPE_MASTER, ANT_NETWORK,
				 0u);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u assign: %d", channel, (int)rc);
		return -EIO;
	}

	rc = antr_channel_id_set(channel, devnum, device_type, ANT_TRANS_TYPE);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u channel id: %d", channel, (int)rc);
		return -EIO;
	}

	/* THE PERIOD IS THE PROFILE'S AND NOT A CHOICE, and the two here are
	 * eight counts apart in a way that reads as a typo: 8192 is FE-C's
	 * 4.00 Hz and 8134 is SDM's ~4.03 Hz. A receiver told the wrong one
	 * never opens the channel and nothing anywhere names the period as the
	 * reason. */
	rc = antr_channel_period_set(channel, period);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u period: %d", channel, (int)rc);
		return -EIO;
	}

	rc = antr_channel_radio_freq_set(channel, ANT_RF_FREQ);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u rf freq: %d", channel, (int)rc);
		return -EIO;
	}

	/* Stage before opening, not after: a master transmits the instant the
	 * channel opens, so the first slot is gone before any EVENT_TX could
	 * fill it and the first frame would otherwise go out empty. */
	stage();

	rc = antr_channel_open(channel);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u open: %d", channel, (int)rc);
		return -EIO;
	}

	return 0;
}

static void node_tick_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	/* The physics. Told how much time passed rather than reading a clock,
	 * which is what lets the same function run inside a ztest. */
	treadmill_state_tick(&state, NODE_TICK_MS);

	/* The slow, level-valued things a source may want a thread context
	 * for. Never stride events; see the seam. */
	treadmill_source_poll();

	publish();

	k_work_schedule(&node_tick, K_MSEC(NODE_TICK_MS));
}

static void fill_common_id(struct profile_common_id *id, uint16_t devnum)
{
	/*
	 * The common-page identity (0x50/0x51), not the ANT channel id -
	 * conflating them produces a machine that is found and then
	 * misidentifies itself.
	 *
	 * THE PLACEHOLDER 1s ARE DISPLAYED BY A HEAD UNIT. model_number,
	 * hw_revision and the two sw_revision fields land on a watch's
	 * device-information screen, so a shipped product must set them from
	 * something real. Production checklist item 2 in
	 * docs/treadmill-reference-design.md.
	 */
	memset(id, 0, sizeof(*id));
	id->hw_revision = 1u;
	id->manufacturer_id = CONFIG_TREADMILL_MANUFACTURER_ID;
	id->model_number = 1u;
	id->sw_revision_main = 1u;
	id->sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	id->serial_number = devnum;
}

int main(void)
{
	struct profile_fec_tx_cfg fec_cfg;
	uint16_t                  devnum = CONFIG_TREADMILL_DEVICE_NUMBER;
	int                       err;

	/*
	 * Assembled by the preprocessor into ONE string literal before it
	 * reaches the macro, rather than by putting an #if inside the argument
	 * list. Preprocessor directives inside a function-like macro's
	 * arguments are undefined behaviour in C, and LOG_INF is a macro -
	 * it happens to work on some toolchains, which is the worst of both.
	 */
#if defined(CONFIG_TREADMILL_ANT_SDM)
#define ANT_BANNER "ANT+ 0x11 FE-C + 0x7C SDM"
#else
#define ANT_BANNER "ANT+ 0x11 FE-C only (SDM disabled)"
#endif
	LOG_INF("RadiANT treadmill: " ANT_BANNER);

	if (antr_init() != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("antr_init failed");
		return 0;
	}

	/*
	 * Device number from the identity record when provisioned (ADR 0009),
	 * falling back to Kconfig so a freshly-flashed DK works on a bench.
	 *
	 * NOTHING IN apps/ CALLS node_ident_provision() TODAY, so on every
	 * board in this tree the fallback is the only branch that runs - every
	 * treadmill flashed from one build shares one identity. For a single
	 * demo that is fine; FOR A GYM IT IS FATAL, and it is the first item on
	 * this application's production checklist for that reason. See
	 * docs/treadmill-reference-design.md and
	 * docs/hrm-reference-design.md's items 3 and 4, which record the same
	 * gap and the ADR it needs.
	 */
	if (node_ident_boot() == 0 && node_ident_is_provisioned()) {
		uint16_t d;

		if (node_ident_devnum(&d) == 0 && d != 0u) {
			devnum = d;
			LOG_INF("device number %u from the identity record", d);
		}
	} else {
		LOG_INF("device number %u from Kconfig (node not provisioned)",
			devnum);
	}

	treadmill_state_init(&state);

	memset(&fec_cfg, 0, sizeof(fec_cfg));
	fill_common_id(&fec_cfg.id, devnum);
	fec_cfg.equipment_type = PROFILE_FEC_TYPE_TREADMILL;
	/*
	 * SIMULATION MODE IS WHAT UNLOCKS PAGE 51, and declaring it is a claim
	 * this node has to be able to keep: a controller reads page 54, sees
	 * bit 2, and only then sends a track-resistance command. The bit and
	 * the honouring go together in both directions - which is why a
	 * machine whose source returns -ENOTSUP from set_incline() should clear
	 * it here as well as answering NOT_SUPPORTED.
	 */
	fec_cfg.caps.capabilities = PROFILE_FEC_CAP_SIMULATION;
	fec_cfg.caps.max_resistance_valid = false;
#if defined(CONFIG_TREADMILL_FEC_METABOLIC)
	fec_cfg.metabolic = true;
#endif

	err = profile_fec_tx_init(&fec, &fec_cfg);
	if (err != 0) {
		LOG_ERR("profile_fec_tx_init: %d", err);
		return 0;
	}

#if defined(CONFIG_TREADMILL_ANT_SDM)
	{
		struct profile_sdm_cfg sdm_cfg;

		memset(&sdm_cfg, 0, sizeof(sdm_cfg));
		fill_common_id(&sdm_cfg.id, devnum);
		/* OTHER, not laces and not midsole: this is not on anybody's
		 * shoe, and a receiver that draws a foot-pod icon from the
		 * location field should not draw one here. */
		sdm_cfg.location = PROFILE_SDM_LOC_OTHER;
		sdm_cfg.battery = PROFILE_SDM_BATTERY_NEW; /* mains */
		sdm_cfg.health = PROFILE_SDM_HEALTH_OK;
		/* On this profile the capability bits ARE the validity signal -
		 * there is no 0xFF sentinel anywhere - so these must name
		 * exactly the fields this machine really produces. No calories
		 * bit unless page 18's estimate is compiled in. */
		sdm_cfg.caps.flags = PROFILE_SDM_CAP_TIME |
				     PROFILE_SDM_CAP_DISTANCE |
				     PROFILE_SDM_CAP_SPEED |
				     PROFILE_SDM_CAP_LATENCY |
				     PROFILE_SDM_CAP_CADENCE;
#if defined(CONFIG_TREADMILL_FEC_METABOLIC)
		sdm_cfg.caps.flags |= PROFILE_SDM_CAP_CALORIES;
#endif

		err = profile_sdm_init(&sdm, &sdm_cfg);
		if (err != 0) {
			LOG_ERR("profile_sdm_init: %d", err);
			return 0;
		}
	}
#endif

	/*
	 * Between the profiles and the channels opening, which is the only
	 * correct window: the profiles must exist before a report can land in
	 * them, and the source must be running before a channel opens so the
	 * first staged page already carries a reading.
	 *
	 * The NAME is logged rather than the option, so a manufacturer reads
	 * THEIR driver's name at boot - or the reference simulator's, and there
	 * is no third answer. A -ENODEV here is the deliberate
	 * CONFIG_TREADMILL_SOURCE_CUSTOM failure.
	 */
	err = treadmill_source_start();
	if (err != 0) {
		LOG_ERR("treadmill source did not start: %d", err);
	} else {
		LOG_INF("treadmill source: %s", treadmill_source_name());
	}

	publish();

	if (antr_network_address_set(ANT_NETWORK, ant_plus_key) !=
	    ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("network key");
		return 0;
	}

	if (ant_master_start(FEC_CHANNEL, devnum, PROFILE_FEC_DEVICE_TYPE,
			     PROFILE_FEC_PERIOD, load_fec_page) != 0) {
		return 0;
	}
#if defined(CONFIG_TREADMILL_ANT_SDM)
	if (ant_master_start(SDM_CHANNEL, devnum, PROFILE_SDM_DEVICE_TYPE,
			     PROFILE_SDM_PERIOD, load_sdm_page) != 0) {
		return 0;
	}
#endif

	k_work_schedule(&node_tick, K_MSEC(NODE_TICK_MS));
	LOG_INF("transmitting: device #%u", devnum);

#if defined(CONFIG_TREADMILL_HR_RX)
	/* Phase 5, after the masters: a slave that fails to acquire must not
	 * look like a master that failed to transmit. */
	(void)treadmill_hr_rx_start(HR_RX_CHANNEL);
#endif

#if defined(CONFIG_TREADMILL_BLE)
	/* Last, for the same reason apps/hrm_ble starts its controller last:
	 * bringing BLE up after the ANT+ channels means a failure to advertise
	 * cannot be mistaken for a failure to transmit. */
	(void)treadmill_ble_start();
#endif

	return 0;
}
