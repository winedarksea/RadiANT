/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadiANT treadmill: one node, two ANT+ masters, two BLE services, one radio.
 *
 * It is apps/hrm_ble's shape with what a treadmill needs added:
 *
 *   TWO ANT+ MASTERS, NOT ONE. FE-C (0x11, 8192 counts) is what a
 *   control-capable head unit pairs with; SDM (0x7C, 8134 counts) is what
 *   anything that only knows foot pods pairs with, Zwift Run included -
 *   different device types, different periods, no shared page space, so two
 *   channels and two radiant_profile_sched instances. CONFIG_TREADMILL_ANT_SDM turns
 *   the second off, since two masters beating at 4.00 and 4.03 Hz beside BLE
 *   is more contention than anything this project has measured.
 *
 *   BIDIRECTIONAL AND SYMMETRIC. Acknowledged control pages from a head unit
 *   and FTMS control-point writes from a phone both land in treadmill_cmd.c -
 *   one module, one state, one motion rule (treadmill_session.h) - so a grade
 *   set over either transport is immediately visible on the other, and
 *   neither can move the machine around the rule. FE-C §8.8
 *   makes the page 71 command status an obligation, not a courtesy, which is
 *   why every branch of apply_control_page() - including refusals - ends in
 *   radiant_profile_fec_tx_command_answered().
 *
 *   NO SUPPRESSION RULE, BUT A COMMAND OWNER. Unlike apps/hrm_ble (which
 *   stops ANT+ while a phone is connected), a treadmill is expected to serve
 *   a head unit and a fitness app at once, so every page and notification
 *   keeps going out to every listener always - there is deliberately no
 *   TREADMILL_SUPPRESS_ANT_WHEN_CONNECTED. What IS arbitrated is whose
 *   commanded grade the deck actually moves to, since two controllers
 *   writing the same function is last-writer-wins with no signal to either.
 *   The ownership token is <radiant/radiant_ctrl_arbiter.h> - it governs who
 *   the deck obeys, not who gets told; the transmit side above is unaffected.
 *
 *   COMMANDS HOP TO THE WORKQUEUE FIRST. treadmill_state.h's lock-free
 *   design requires everything touching that struct to run on the system
 *   workqueue, but an FE-C page arrives on the radiant_event thread and an
 *   FTMS write arrives on the BT RX thread. Rather than add a lock, each
 *   path stores its request and submits a k_work, restoring the invariant -
 *   which is also what lets the ownership token above need no lock.
 *
 * See docs/treadmill-reference-design.md for the porting guide, the
 * production checklist and the bench procedure; src/treadmill_source.h for
 * the one seam a manufacturer implements; and src/treadmill_state.h for why
 * there is exactly one copy of the physics.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <radiant/radiant_ant.h>
#include <radiant/radiant_wire.h>
#include <radiant/radiant_node_ident.h>
#include <radiant/radiant_product.h>
#include <radiant/radiant_profile_common.h>
#include <radiant/radiant_profile_fec.h>
#include <radiant/radiant_profile_fec_tx.h>
#include <radiant/radiant_profile_sdm.h>
#include "treadmill_caps.h"
#include "treadmill_cmd.h"
#include "treadmill_session.h"
#include "treadmill_source.h"
#include "treadmill_state.h"

#if defined(CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE)
#include <radiant/radiant_ctrl_arbiter.h>
#endif
#if defined(CONFIG_TREADMILL_BLE)
#include "treadmill_ble.h"
#endif
#if defined(CONFIG_RADIANT_BLE_PAIRING_WINDOW)
#include <radiant/radiant_ble_pairing.h>
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

/* FTMS Reset and FE-C's "assume flat" sentinel both command 0; a machine range
 * that excluded it would refuse Reset and clamp "flat" onto a slope. Kconfig's
 * own ranges already guarantee it - this is so nobody widens them unseen. */
BUILD_ASSERT(CONFIG_TREADMILL_MIN_INCLINE_DECI <= 0 &&
	     CONFIG_TREADMILL_MAX_INCLINE_DECI >= 0,
	     "the machine incline range must include 0 %");

static struct treadmill_state  state;

/*
 * THE SESSION and the motion rule's inputs (treadmill_session.h). Workqueue
 * only, except the epoch, which the receive paths (the radiant_event thread
 * for FE-C, the BT RX thread for FTMS) read at receipt through this atomic
 * mirror - updated in session_apply(), the one writer of the session.
 */
static struct treadmill_session session;
static atomic_t session_epoch;
static const struct treadmill_motion_policy motion_policy = {
	.remote_motion_allowed = treadmill_source_remote_motion_allowed,
	.remote_start = IS_ENABLED(CONFIG_TREADMILL_REMOTE_START),
};

/* What this machine advertises, derived once at boot (treadmill_caps.c). */
static struct treadmill_caps caps;
static struct radiant_profile_fec_tx   fec;
#if defined(CONFIG_TREADMILL_ANT_SDM)
static struct radiant_profile_sdm      sdm;
#endif

static void node_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(node_tick, node_tick_fn);

/* ---------------------------------------------------------------------------
 * State -> pages. The one place the SI-unit truth becomes wire fields.
 * ---------------------------------------------------------------------------
 */

static void publish_fec(void)
{
	struct radiant_profile_fec_general   g;
	struct radiant_profile_fec_settings  s;
	struct radiant_profile_fec_treadmill t;

	memset(&g, 0, sizeof(g));
	g.equipment_type = RADIANT_PROFILE_FEC_TYPE_TREADMILL;
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
	g.hr_source = state.hr_from_ant ? RADIANT_PROFILE_FEC_HR_SRC_ANTPLUS
					: RADIANT_PROFILE_FEC_HR_SRC_INVALID;
	g.state = state.state;
	g.lap_toggle = state.lap_toggle;
	radiant_profile_fec_tx_set_general(&fec, &g);

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
	radiant_profile_fec_tx_set_settings(&fec, &s);

	memset(&t, 0, sizeof(t));
	t.cadence_strides_min =
		treadmill_cadence_strides_min(state.cadence_strides_min_16);
	t.cadence_valid = true; /* 0 is a real standing runner, not "unknown" */
	/* 0.1 m units from millimetres. Both accumulators are unsigned and both
	 * count UP - the negative one is a magnitude of descent. */
	t.neg_vertical_dm = (uint8_t)((state.neg_vertical_mm / 100u) & 0xFFu);
	t.pos_vertical_dm = (uint8_t)((state.pos_vertical_mm / 100u) & 0xFFu);
	t.capabilities = RADIANT_PROFILE_FEC_TREADMILL_CAP_NEG_VERTICAL |
			 RADIANT_PROFILE_FEC_TREADMILL_CAP_POS_VERTICAL;
	t.state = state.state;
	t.lap_toggle = state.lap_toggle;
	radiant_profile_fec_tx_set_treadmill(&fec, &t);

#if defined(CONFIG_TREADMILL_FEC_METABOLIC)
	{
		struct radiant_profile_fec_metabolic m;

		memset(&m, 0, sizeof(m));
		/* The tick's own rate, not a delta of the total: publish() runs
		 * from every source report as well as the tick, and each call
		 * used to consume the delta, so page 18 read 0.0 kcal/h
		 * whenever a report landed between ticks. */
		m.burn_rate_deci = (state.burn_rate_deci > 0xFFFEu)
					   ? 0xFFFEu
					   : (uint16_t)state.burn_rate_deci;
		m.burn_rate_valid = true;
		/* METs are deliberately NOT reported. The estimate behind the
		 * calorie total is a population-average equation with no
		 * measurement in it (see treadmill_state.c), and a METs field
		 * is read as a physiological statement about the person on the
		 * belt in a way that a calorie counter on a console is not. */
		m.mets_valid = false;
		m.calories = (uint8_t)((state.energy_mkcal / 1000u) & 0xFFu);
		m.capabilities = RADIANT_PROFILE_FEC_METABOLIC_CAP_CALORIES;
		m.state = state.state;
		m.lap_toggle = state.lap_toggle;
		radiant_profile_fec_tx_set_metabolic(&fec, &m);
	}
#endif
}

#if defined(CONFIG_TREADMILL_ANT_SDM)
static void publish_sdm(void)
{
	radiant_profile_sdm_set_motion(&sdm, state.belt_speed_mm_s,
			       treadmill_cadence_strides_min(
				       state.cadence_strides_min_16),
			       (uint8_t)(state.cadence_strides_min_16 & 0x0Fu),
			       state.state == TREADMILL_STATE_IN_USE);
	radiant_profile_sdm_set_accumulators(&sdm, state.elapsed_ms, state.distance_mm,
				     state.strides, SDM_LATENCY_32);
	radiant_profile_sdm_set_calories(&sdm,
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
/*
 * The ONE writer of the session. Every input lands here, on the workqueue;
 * page 16's state, FTMS Training Status and the FTMS machine status all follow
 * from what it decides, and a safety-key pull revokes every remote controller.
 */
static void session_apply(enum treadmill_session_input in)
{
	struct treadmill_session_change c = treadmill_session_input(&session, in);

	atomic_set(&session_epoch, (atomic_val_t)session.epoch);

	if (c.revoke_control) {
#if defined(CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE)
		/* The FE-C controller, if it held the token, learns through
		 * REJECTED on its next command; the FTMS one gets 0xFF below. */
		(void)radiant_ctrl_revoke();
#endif
	}
	if (c.prev != c.next) {
		LOG_INF("session: %u -> %u (epoch %u)", (unsigned)c.prev,
			(unsigned)c.next, (unsigned)session.epoch);
		treadmill_state_set_state(&state, treadmill_session_fe_state(c.next));
	}
#if defined(CONFIG_TREADMILL_BLE)
	if (c.prev != c.next || c.revoke_control) {
		treadmill_ble_session_changed(c.prev, c.next, c.revoke_control);
	}
#endif
}

void treadmill_node_deliver(uint32_t strides)
{
	/* Touched only here, on the workqueue: the last FE state fed to the
	 * session. EDGES only - a belt still reporting IN_USE while it spins
	 * down after a console Stop must not restart the session. */
	static uint8_t fed_fe_state = TREADMILL_STATE_READY;
	uint8_t fe = treadmill_source_last_state();

	treadmill_state_set_speed(&state, treadmill_source_last_speed());
	treadmill_state_set_incline(&state, treadmill_source_last_incline());
	treadmill_state_set_cadence_16(&state,
				       treadmill_source_last_cadence_16());
	treadmill_state_add_strides(&state, strides);

	if (fe != fed_fe_state) {
		fed_fe_state = fe;
		session_apply((fe == TREADMILL_STATE_IN_USE)     ? TREADMILL_SESSION_FE_IN_USE
			      : (fe == TREADMILL_STATE_FINISHED) ? TREADMILL_SESSION_FE_FINISHED
								 : TREADMILL_SESSION_FE_READY);
	}

	publish();
}

/* The source's events, on the workqueue, in order. */
void treadmill_node_event(enum treadmill_source_event evt)
{
	switch (evt) {
	case TREADMILL_EVT_CONSOLE_START:
		session_apply(TREADMILL_SESSION_CONSOLE_START);
		break;
	case TREADMILL_EVT_CONSOLE_STOP:
		session_apply(TREADMILL_SESSION_CONSOLE_STOP);
		break;
	case TREADMILL_EVT_CONSOLE_PAUSE:
		session_apply(TREADMILL_SESSION_CONSOLE_PAUSE);
		break;
	case TREADMILL_EVT_PAUSED:
		session_apply(TREADMILL_SESSION_PAUSED);
		break;
	case TREADMILL_EVT_RESUMED:
		session_apply(TREADMILL_SESSION_RESUMED);
		break;
	case TREADMILL_EVT_SAFETY_KEY_OUT:
		session_apply(TREADMILL_SESSION_KEY_OUT);
		break;
	case TREADMILL_EVT_SAFETY_KEY_IN:
		session_apply(TREADMILL_SESSION_KEY_IN);
		break;
	case TREADMILL_EVT_PAIRING_WINDOW:
#if defined(CONFIG_RADIANT_BLE_PAIRING_WINDOW)
		/* Equipment bonding: new phones only inside a window the user
		 * opened at the console (radiant_ble_pairing.h). */
		(void)radiant_ble_pairing_window_open(CONFIG_TREADMILL_PAIRING_WINDOW_S);
		LOG_INF("pairing window open for %u s",
			(unsigned)CONFIG_TREADMILL_PAIRING_WINDOW_S);
#endif
		break;
	case TREADMILL_EVT_FORGET_BONDS:
#if defined(CONFIG_RADIANT_BLE_PAIRING_WINDOW)
		LOG_WRN("forgetting every bond: %d", radiant_ble_bonds_forget_all());
#endif
		break;
	default:
		break;
	}
	publish();
}

/* ---------------------------------------------------------------------------
 * The control path: FE-C acknowledged pages in, page 71 out
 * ---------------------------------------------------------------------------
 */

/*
 * THERE IS NO SEQUENCE NUMBER IN AN FE-C CONTROL PAGE. Unlike ANT+ Controls
 * (0x10), whose pages carry one, the FE-C command pages do not - so page 71's
 * sequence field is the FE's own count of commands received, which is what
 * lets a controller tell "you answered my command" from "you re-sent the
 * answer to the previous one".
 *
 * File scope rather than a function static because the drop path below also
 * owes an answer and that answer is a command received too.
 */
static uint8_t fec_sequence;

#if defined(CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE)
/*
 * radiant_ctrl_arbiter.h spells these two page numbers itself so the ztest that
 * drives the policy does not have to pull radiant_profile_fec_tx.h in. This is the
 * cross-check that keeps the two spellings from drifting - the same guard
 * treadmill_state.c puts on its copy of the FE state values.
 */
_Static_assert(RADIANT_CTRL_PAGE_TRACK_RESISTANCE ==
		       RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE,
	       "radiant_ctrl_arbiter.h's copy of FE-C page 51 has drifted from "
	       "radiant_profile_fec_tx.h's");
_Static_assert(RADIANT_CTRL_PAGE_WIND_RESISTANCE ==
		       RADIANT_PROFILE_FEC_PAGE_WIND_RESISTANCE,
	       "radiant_ctrl_arbiter.h's copy of FE-C page 50 has drifted from "
	       "radiant_profile_fec_tx.h's");
#endif

/*
 * One acknowledged control page, ON THE SYSTEM WORKQUEUE.
 *
 * `body` is the eight-byte payload. EVERY path through this function ends in
 * radiant_profile_fec_tx_command_answered() - including the unrecognised-page path,
 * because §8.8 wants a page 71 saying NOT_SUPPORTED rather than a silence, and
 * silence is what a controller reports as a broken machine.
 *
 * It is reached from fec_cmd_work_fn() and never directly from the radio
 * callback; see queue_control_page() for why.
 */
static void apply_control_page(const uint8_t *body, uint32_t rx_epoch)
{
	uint8_t status = RADIANT_PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
	uint8_t page = body[0];

	fec_sequence++;

#if defined(CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE)
	/*
	 * The claim is IMPLICIT - there is no FE-C acquire handshake to
	 * perform - so it loses to a BLE client that ran an explicit FTMS
	 * Request Control. REJECTED (3) is the profile's own constant for
	 * exactly this and is how the controller learns it lost the deck;
	 * there is no other signal, because ANT+ has no connection to drop.
	 */
	if (radiant_ctrl_fec_page_is_gated(page) &&
	    radiant_ctrl_claim(RADIANT_CTRL_ANT, false,
				    (uint32_t)k_uptime_get_32()) != 0) {
		LOG_INF("FE-C page %u refused: a BLE client holds control", page);
		radiant_profile_fec_tx_command_answered(&fec, page, fec_sequence,
						RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED,
						&body[4]);
		return;
	}
#endif

	switch (page) {
	case RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE: {
		struct radiant_profile_fec_cmd_track_resistance cmd;

		if (radiant_profile_fec_decode_track_resistance(body, &cmd) != 0) {
			break;
		}
		/* cmd.grade_centi_pct is already 0 when the controller sent the
		 * 0xFFFF sentinel, which §8.8.4.1 defines as "assume flat" -
		 * not as "no command". Byte 7's rolling resistance is decoded
		 * and deliberately not acted on (§8.8.4.2).
		 *
		 * A simulation grade, so it CLAMPS to the machine range
		 * (CONFIG_TREADMILL_MIN/MAX_INCLINE_DECI) and answers PASS: the
		 * road may be steeper than the deck, and refusing would make a
		 * controller drop simulation mode mid-ride. Any ANT+
		 * transmitter can send this page; the clamp is what keeps it
		 * inside what the machine can physically do.
		 *
		 * A MOTION PATH: treadmill_cmd_fec_track_resistance() puts it
		 * behind the motion rule. The token was claimed above (or there
		 * is no arbitration); a refusal by the rule, or a request
		 * received before a local stop (rx_epoch), is REJECTED. */
		status = treadmill_cmd_fec_track_resistance(cmd.grade_centi_pct,
							    rx_epoch, true);
		LOG_INF("FE-C page 51: grade %d.%02d %% -> status %u",
			cmd.grade_centi_pct / 100,
			(cmd.grade_centi_pct < 0 ? -cmd.grade_centi_pct
						 : cmd.grade_centi_pct) % 100,
			(unsigned)status);
		break;
	}

	case RADIANT_PROFILE_FEC_PAGE_WIND_RESISTANCE:
		/* Part of the simulation-mode parameter set, and a treadmill
		 * has nothing to do with any of it. Accepted and acknowledged
		 * rather than refused: refusing one parameter of a set the
		 * machine advertises support for is how a controller decides
		 * simulation mode is broken and stops sending page 51 too - and
		 * refused exactly when page 51 would be for the epoch. */
		status = treadmill_cmd_fec_wind(rx_epoch, true);
		break;

	case RADIANT_PROFILE_FEC_PAGE_USER_CONFIG: {
		struct radiant_profile_fec_cmd_user_config cmd;

		if (radiant_profile_fec_decode_user_config(body, &cmd) == 0 &&
		    cmd.user_weight_valid) {
			treadmill_state_set_user_mass(&state,
						      cmd.user_weight_10g);
			status = RADIANT_PROFILE_FEC_CMD_STATUS_PASS;
		} else {
			status = RADIANT_PROFILE_FEC_CMD_STATUS_FAIL;
		}
		break;
	}

	case RADIANT_PROFILE_FEC_PAGE_REQUEST: {
		struct radiant_profile_fec_cmd_request cmd;

		if (radiant_profile_fec_decode_request(body, &cmd) == 0 &&
		    radiant_profile_fec_tx_request(&fec, cmd.page, cmd.tx_count) == 0) {
			status = RADIANT_PROFILE_FEC_CMD_STATUS_PASS;
		}
		break;
	}

	case RADIANT_PROFILE_FEC_PAGE_BASIC_RESISTANCE:
	case RADIANT_PROFILE_FEC_PAGE_TARGET_POWER:
		/* Page 54 declares neither Basic Resistance mode nor Target
		 * Power mode, so a conforming controller never sends these -
		 * and one that does gets the answer the declaration implies.
		 * The pairing is the point: declaring a capability and
		 * honouring its command page go together in both directions. */
		status = RADIANT_PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
		break;

	default:
		status = RADIANT_PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
		break;
	}

	/* Bytes [4..7] of the command, echoed position for position, so a
	 * controller reads page 51's grade back at the offsets it wrote it. */
	radiant_profile_fec_tx_command_answered(&fec, page, fec_sequence, status,
					&body[4]);
}

/* ---------------------------------------------------------------------------
 * The thread hop, and why it is a queue rather than a call
 * ---------------------------------------------------------------------------
 *
 * radiant_ant_on_message() runs on radiant's event thread. apply_control_page() writes
 * `state`, and treadmill_state.h's lock-free design is only safe because
 * "everything that touches this struct runs on the system workqueue" - which
 * this path did not. Nor did the FTMS one; see treadmill_ble.c's cp_work.
 *
 * A WORKQUEUE HOP AND NOT A MUTEX, for two reasons. It restores the invariant
 * the code already documents rather than adding a second, weaker one beside
 * it; and once both control paths are on one thread, the ownership token in
 * the arbiter needs no lock either, which is most of why that module
 * can be kernel-free and therefore testable.
 *
 * THE HOP IS INVISIBLE ON AIR. Page 71 is not sent from here - it is staged
 * into radiant_profile_fec_tx and goes out on the next data slot, up to 250 ms later
 * either way. A workqueue turnaround is microseconds against that.
 *
 * IT ALSO DOES NOT CHANGE radiant_profile_fec_tx's CONTEXT RULES, which is worth
 * checking rather than assuming, because it moves a caller across a thread.
 * radiant_profile_fec_tx.h states them: "all plain stores: no locks, and reached only
 * from the system workqueue and the radio callback". Before this, the answer
 * and the request were made from the radio callback and the page setters from
 * the workqueue; now every setter is on the workqueue and only
 * radiant_profile_fec_tx_next() is on the callback. That is the same two contexts the
 * header allows, with the writers consolidated onto one of them rather than
 * split across both - strictly the tidier half of what was already sanctioned.
 * "Plain stores" is safe for the WRITERS only because the workqueue is
 * cooperative and the event thread cannot preempt it. The reverse is not
 * true: the workqueue can preempt the event thread mid-encode, so the READER
 * (load_fec_page()/load_sdm_page()) wraps its _next() in k_sched_lock().
 *
 * A RING AND NOT ONE SLOT. Commands arrive at most once per 250 ms slot and the
 * consumer runs in microseconds, so the depth is never reached in practice.
 * What the ring buys is that no COMMAND is lost or reordered on the way to
 * apply_control_page(). It does NOT buy one page 71 per command: page 71 is a
 * single "last received command" slot in radiant_profile_fec_tx, so two commands
 * answered before the next slot 3 put only the second answer on the air -
 * which is what that page means. Four entries costs 32 bytes. The drop path
 * below is kept anyway, and still answers, because a command that is dropped
 * in silence is exactly the "machine that ignores you" §8.8 is about.
 */
#define FEC_CMD_QUEUE_DEPTH 4u

static struct {
	uint8_t body[8];
	/* The session epoch at receipt: a page that waited out a local Stop
	 * or a safety-key event is refused at dequeue (treadmill_session.h). */
	uint32_t epoch;
} fec_cmd_q[FEC_CMD_QUEUE_DEPTH];

static uint8_t fec_cmd_head; /* producer: the radiant_event thread */
static uint8_t fec_cmd_tail; /* consumer: the system workqueue */
static uint8_t fec_cmd_drop_page;
static bool    fec_cmd_dropped;

/* The handoff needs a lock even though the state does not: two threads, and
 * the producer may in principle be an interrupt. It covers the indices only,
 * never any application state, and is held for a memcpy of eight bytes. */
static struct k_spinlock fec_cmd_lock;

static void fec_cmd_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	for (;;) {
		k_spinlock_key_t key;
		uint8_t          body[8];
		uint32_t         epoch = 0u;
		uint8_t          drop_page = 0u;
		bool             have = false;
		bool             dropped = false;

		key = k_spin_lock(&fec_cmd_lock);
		if (fec_cmd_tail != fec_cmd_head) {
			memcpy(body, fec_cmd_q[fec_cmd_tail].body, sizeof(body));
			epoch = fec_cmd_q[fec_cmd_tail].epoch;
			fec_cmd_tail = (uint8_t)((fec_cmd_tail + 1u) %
						 FEC_CMD_QUEUE_DEPTH);
			have = true;
		} else if (fec_cmd_dropped) {
			/* Only once the ring is empty, so the answers stay in
			 * the order the commands arrived. */
			drop_page = fec_cmd_drop_page;
			fec_cmd_dropped = false;
			dropped = true;
		}
		k_spin_unlock(&fec_cmd_lock, key);

		if (have) {
			apply_control_page(body, epoch);
			continue;
		}
		if (dropped) {
			static const uint8_t no_echo[4] = { 0u, 0u, 0u, 0u };

			fec_sequence++;
			LOG_WRN("FE-C page %u dropped: the command queue was "
				"full", drop_page);
			radiant_profile_fec_tx_command_answered(
				&fec, drop_page, fec_sequence,
				RADIANT_PROFILE_FEC_CMD_STATUS_FAIL, no_echo);
			continue;
		}
		break;
	}
}

static K_WORK_DEFINE(fec_cmd_work, fec_cmd_work_fn);

/* Called from radiant_ant_on_message(), i.e. the radiant_event thread. Does no more
 * than it has to: copy eight bytes and wake the workqueue. */
static void queue_control_page(const uint8_t *body)
{
	k_spinlock_key_t key;
	uint8_t          next;

	key = k_spin_lock(&fec_cmd_lock);
	next = (uint8_t)((fec_cmd_head + 1u) % FEC_CMD_QUEUE_DEPTH);
	if (next == fec_cmd_tail) {
		/* Keep the LAST page number rather than the first: a
		 * controller that swamped the queue is most likely to be
		 * waiting on its newest command. */
		fec_cmd_dropped = true;
		fec_cmd_drop_page = body[0];
	} else {
		memcpy(fec_cmd_q[fec_cmd_head].body, body, 8u);
		fec_cmd_q[fec_cmd_head].epoch = (uint32_t)atomic_get(&session_epoch);
		fec_cmd_head = next;
	}
	k_spin_unlock(&fec_cmd_lock, key);

	(void)k_work_submit(&fec_cmd_work);
}

/* The session, for treadmill_ble.c's Training Status. Workqueue. */
const struct treadmill_session *treadmill_node_session(void)
{
	return &session;
}

/* The epoch, from any thread. */
uint32_t treadmill_node_epoch(void)
{
	return (uint32_t)atomic_get(&session_epoch);
}

const struct treadmill_state *treadmill_node_state(void)
{
	return &state;
}

#if defined(CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE)
/* (d) for an FTMS path: the BLE side holds the cross-transport token. The FTMS
 * core has already checked this client holds FTMS control. */
static bool ftms_token(void)
{
	return radiant_ctrl_owner() == RADIANT_CTRL_BLE;
}
#endif

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
	enum radiant_profile_slot_kind kind;

	/* The event thread is preemptible and publish() runs on the
	 * cooperative workqueue, which can therefore land mid-encode and give
	 * this page half old, half new fields. Lock the encode only - never
	 * the send, which takes a mutex. */
	k_sched_lock();
	kind = radiant_profile_fec_tx_next(&fec, body);
	k_sched_unlock();
	if (kind == RADIANT_PROFILE_SLOT_IDLE) {
		return;
	}
	(void)radiant_ant_broadcast_message_tx(FEC_CHANNEL, sizeof(body), body);
}

#if defined(CONFIG_TREADMILL_ANT_SDM)
static void load_sdm_page(void)
{
	uint8_t body[8];
	enum radiant_profile_slot_kind kind;

	/* As load_fec_page(): SDM page 1's distance_m/distance_frac pair torn
	 * across a metre roll-over reads as a 255 m jump. */
	k_sched_lock();
	kind = radiant_profile_sdm_next(&sdm, body);
	k_sched_unlock();
	if (kind == RADIANT_PROFILE_SLOT_IDLE) {
		return;
	}
	(void)radiant_ant_broadcast_message_tx(SDM_CHANNEL, sizeof(body), body);
}
#endif

/*
 * The backend's only way up. A dongle forwards this to USB; a node is the
 * application the messages were for.
 */
void radiant_ant_on_message(const struct radiant_ant_msg *msg)
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

	if (msg->id == RADIANT_WIRE_MESG_RESPONSE_EVENT_ID &&
	    msg->len >= RADIANT_WIRE_MESG_RESPONSE_EVENT_SIZE) {
		/* data[0] channel, data[1] the message id being responded to
		 * (0 for an event), data[2] the code. */
		if (msg->data[2] != RADIANT_WIRE_EVENT_TX) {
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
	if ((msg->id == RADIANT_WIRE_MESG_ACKNOWLEDGED_DATA_ID ||
	     msg->id == RADIANT_WIRE_MESG_EXT_ACKNOWLEDGED_DATA_ID) &&
	    msg->len >= 9u && msg->data[0] == FEC_CHANNEL) {
		queue_control_page(&msg->data[1]);
	}
}

static int ant_master_start(uint8_t channel, uint16_t devnum,
			    uint8_t device_type, uint16_t period)
{
	radiant_ant_err_t rc;

	rc = radiant_ant_channel_assign(channel, RADIANT_WIRE_CHANNEL_TYPE_MASTER, ANT_NETWORK,
				 0u);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u assign: %d", channel, (int)rc);
		return -EIO;
	}

	rc = radiant_ant_channel_id_set(channel, devnum, device_type, ANT_TRANS_TYPE);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u channel id: %d", channel, (int)rc);
		return -EIO;
	}

	/* THE PERIOD IS THE PROFILE'S AND NOT A CHOICE, and the two here are
	 * eight counts apart in a way that reads as a typo: 8192 is FE-C's
	 * 4.00 Hz and 8134 is SDM's ~4.03 Hz. A receiver told the wrong one
	 * never opens the channel and nothing anywhere names the period as the
	 * reason. */
	rc = radiant_ant_channel_period_set(channel, period);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u period: %d", channel, (int)rc);
		return -EIO;
	}

	rc = radiant_ant_channel_radio_freq_set(channel, ANT_RF_FREQ);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u rf freq: %d", channel, (int)rc);
		return -EIO;
	}

	/* No pre-open stage. It was here to stop the first frame going out
	 * empty, and it never could: radiant_ant_broadcast_message_tx() refuses a
	 * channel that is not open yet (NOT_OPENED, by design), so it only
	 * consumed a page for nothing. open() posts the first slot from
	 * whatever the buffer holds (zeros at boot; FE-C has no page 0x00, so
	 * receivers ignore it) and EVENT_TX after that slot starts the real
	 * stream, group-aligned. */
	rc = radiant_ant_channel_open(channel);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
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

#if defined(CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE)
	/*
	 * Expire an idle ANT+ claim. THE ONLY RELEASE IT HAS: ANT+ is
	 * connectionless, so a head unit that has walked away generates no
	 * event at all - it simply stops sending page 51. A BLE owner is not
	 * touched here; that one is released by Reset or by a disconnect.
	 */
	radiant_ctrl_tick((uint32_t)k_uptime_get_32());
#endif

#if defined(CONFIG_TREADMILL_BLE)
	/* The advertising watchdog (radiant_ble_adv.h): cheap unless a probe
	 * is due, and the backstop for advertising ending unseen. */
	treadmill_ble_tick();
#endif

	publish();

	k_work_schedule(&node_tick, K_MSEC(NODE_TICK_MS));
}

int main(void)
{
	struct radiant_profile_fec_tx_cfg fec_cfg;
	const struct radiant_product     *product = radiant_product_get();
	struct radiant_product_unit       unit;
	uint16_t                          devnum;
	int                               err;

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

	if (radiant_ant_init() != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("radiant_ant_init failed");
		return 0;
	}

	/*
	 * Device number and serial from the identity record (ADR 0009). On the
	 * first boot of a part with no record, radiant_node_ident_boot()
	 * generates K_dev from the on-chip TRNG and stores it first (ADR 0020),
	 * so every treadmill flashed from one build gets its own number - which
	 * a gym needs.
	 *
	 * radiant_product_unit_boot() owns the failure branch. A development
	 * build falls back to the Kconfig number with no transforms, logged as
	 * an error on every boot: on that branch every machine built from the
	 * same image shares one identity, which for a gym is fatal. A strict
	 * build (CONFIG_RADIANT_PRODUCT_STRICT) does not return until the
	 * identity boots - nothing is on the air yet, so that is staying off it.
	 * See docs/treadmill-reference-design.md.
	 *
	 * The common-page identity (0x50/0x51) is not the ANT channel id -
	 * conflating them produces a machine that is found and then
	 * misidentifies itself. It comes from radiant_product
	 * (CONFIG_RADIANT_PRODUCT_*, the record the BLE DIS strings come from
	 * too) and the unit's 32-bit serial; production checklist item 2 in
	 * docs/treadmill-reference-design.md.
	 */
	(void)radiant_product_unit_boot(CONFIG_TREADMILL_DEVICE_NUMBER, &unit);
	devnum = unit.devnum;

	treadmill_state_init(&state);
	treadmill_session_init(&session, &motion_policy);
	atomic_set(&session_epoch, (atomic_val_t)session.epoch);
	/* The machine range bounds every command path, FE-C page 51 included.
	 * Kconfig's ranges already keep 0 inside it; asserted at file scope. */
	(void)treadmill_state_set_limits(
		&state, treadmill_fec_incline_from_ftms(CONFIG_TREADMILL_MIN_INCLINE_DECI),
		treadmill_fec_incline_from_ftms(CONFIG_TREADMILL_MAX_INCLINE_DECI),
		treadmill_mm_s_from_ftms_speed(CONFIG_TREADMILL_MAX_SPEED_CENTI_KPH));

#if defined(CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE)
	radiant_ctrl_init(CONFIG_TREADMILL_CTRL_ANT_TIMEOUT_MS,
			  RADIANT_CTRL_GATE_SIMULATION);
#endif

	/*
	 * WHAT THIS MACHINE ADVERTISES, from what its driver implements (the
	 * source registered from a SYS_INIT, before main()) and the Kconfig
	 * switches that can only take a capability away. FE-C page 54's
	 * Simulation bit and FTMS's Target Setting bits are claims; they are
	 * made exactly when the command they promise will be honoured.
	 */
	{
		struct treadmill_source_caps src;
		const struct treadmill_caps_cfg ccfg = {
			.allow_speed = IS_ENABLED(CONFIG_TREADMILL_CAP_SPEED_TARGET),
			.allow_incline = IS_ENABLED(CONFIG_TREADMILL_CAP_INCLINE_TARGET),
			.heart_rate = IS_ENABLED(CONFIG_TREADMILL_HR_RX),
		};
		struct treadmill_cmd_cfg cmd_cfg = {
			.state = &state,
			.session = &session,
#if defined(CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE)
			.ftms_token = ftms_token,
#endif
		};

		treadmill_source_get_caps(&src);
		treadmill_caps_derive(&src, &ccfg, &caps);
		cmd_cfg.incline_cmd = caps.incline_cmd;
		cmd_cfg.speed_cmd = caps.speed_cmd;
		treadmill_cmd_bind(&cmd_cfg);
	}

	memset(&fec_cfg, 0, sizeof(fec_cfg));
	(void)radiant_product_fill_common_id(product, unit.serial, &fec_cfg.id);
	fec_cfg.equipment_type = RADIANT_PROFILE_FEC_TYPE_TREADMILL;
	/*
	 * SIMULATION MODE IS WHAT UNLOCKS PAGE 51, and declaring it is a claim
	 * this node has to be able to keep: a controller reads page 54, sees
	 * bit 2, and only then sends a track-resistance command. The bit and
	 * the honouring go together in both directions, which is why the bit
	 * is DERIVED (treadmill_caps.c): set exactly when a page 51 would
	 * reach a set_incline() the source implements.
	 */
	fec_cfg.caps.capabilities = caps.fec_simulation ? RADIANT_PROFILE_FEC_CAP_SIMULATION : 0u;
	fec_cfg.caps.max_resistance_valid = false;
#if defined(CONFIG_TREADMILL_FEC_METABOLIC)
	fec_cfg.metabolic = true;
#endif

	err = radiant_profile_fec_tx_init(&fec, &fec_cfg);
	if (err != 0) {
		LOG_ERR("radiant_profile_fec_tx_init: %d", err);
		return 0;
	}

#if defined(CONFIG_TREADMILL_ANT_SDM)
	{
		struct radiant_profile_sdm_cfg sdm_cfg;

		memset(&sdm_cfg, 0, sizeof(sdm_cfg));
		(void)radiant_product_fill_common_id(product, unit.serial, &sdm_cfg.id);
		/* OTHER, not laces and not midsole: this is not on anybody's
		 * shoe, and a receiver that draws a foot-pod icon from the
		 * location field should not draw one here. */
		sdm_cfg.location = RADIANT_PROFILE_SDM_LOC_OTHER;
		sdm_cfg.battery = RADIANT_PROFILE_SDM_BATTERY_NEW; /* mains */
		sdm_cfg.health = RADIANT_PROFILE_SDM_HEALTH_OK;
		/* On this profile the capability bits ARE the validity signal -
		 * there is no 0xFF sentinel anywhere - so these must name
		 * exactly the fields this machine really produces. No calories
		 * bit unless page 18's estimate is compiled in. */
		sdm_cfg.caps.flags = RADIANT_PROFILE_SDM_CAP_TIME |
				     RADIANT_PROFILE_SDM_CAP_DISTANCE |
				     RADIANT_PROFILE_SDM_CAP_SPEED |
				     RADIANT_PROFILE_SDM_CAP_LATENCY |
				     RADIANT_PROFILE_SDM_CAP_CADENCE;
#if defined(CONFIG_TREADMILL_FEC_METABOLIC)
		sdm_cfg.caps.flags |= RADIANT_PROFILE_SDM_CAP_CALORIES;
#endif

		err = radiant_profile_sdm_init(&sdm, &sdm_cfg);
		if (err != 0) {
			LOG_ERR("radiant_profile_sdm_init: %d", err);
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

	if (radiant_ant_network_address_set(ANT_NETWORK, ant_plus_key) !=
	    RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("network key");
		return 0;
	}

	if (ant_master_start(FEC_CHANNEL, devnum, RADIANT_PROFILE_FEC_DEVICE_TYPE,
			     RADIANT_PROFILE_FEC_PERIOD) != 0) {
		return 0;
	}
#if defined(CONFIG_TREADMILL_ANT_SDM)
	/*
	 * FALLS THROUGH ON FAILURE, and it used to `return 0`.
	 *
	 * That early return was a second, independent path to "BLE never
	 * starts" - and to "the heart-rate slave never starts" - triggered by
	 * something with no connection to either: a failure to open the
	 * SECOND ANT+ master. It reported nothing beyond the ch%u log line
	 * already emitted inside ant_master_start(), so the symptom was a node
	 * transmitting FE-C perfectly with no BLE and no explanation.
	 *
	 * Falling through is also the right product behaviour on its own
	 * terms: SDM is the compatibility channel, and losing it should cost
	 * the watches that only speak foot pod, not the head unit and the
	 * phone as well. The FE-C master above still returns, because without
	 * it there is no treadmill on the air at all.
	 */
	if (ant_master_start(SDM_CHANNEL, devnum, RADIANT_PROFILE_SDM_DEVICE_TYPE,
			     RADIANT_PROFILE_SDM_PERIOD) != 0) {
		LOG_ERR("the SDM master did not start; continuing with FE-C "
			"only. Anything that pairs with a foot pod - Zwift Run, "
			"most watches - will not see this machine.");
	}
#endif

	k_work_schedule(&node_tick, K_MSEC(NODE_TICK_MS));
	LOG_INF("transmitting: device #%u", devnum);

#if defined(CONFIG_TREADMILL_HR_RX)
	/* Phase 5, after the masters: a slave that fails to acquire must not
	 * look like a master that failed to transmit. */
	err = treadmill_hr_rx_start(HR_RX_CHANNEL);
	if (err != 0) {
		LOG_ERR("the heart-rate slave did not start: %d. The machine "
			"still runs; page 16's heart-rate field stays invalid.",
			err);
	}
#endif

#if defined(CONFIG_TREADMILL_BLE)
	/*
	 * Last, for the same reason apps/hrm_ble starts its controller last:
	 * bringing BLE up after the ANT+ channels means a failure to advertise
	 * cannot be mistaken for a failure to transmit.
	 *
	 * THE RETURN IS CAPTURED, and it used to be cast to (void).
	 * treadmill_ble_start() LOG_ERRs every step it can fail at, so this
	 * line adds little on its own - but "little" is not "nothing": it is
	 * the one place that says the node is now ANT+-only, in one line, at
	 * the level a reader greps for, rather than leaving that conclusion to
	 * be assembled from an error further up and the absence of the
	 * advertising line further down.
	 */
	err = treadmill_ble_start(devnum, unit.serial, &caps);
	if (err != 0) {
		/* Only fatal steps get here (controller or RSC init): a
		 * failed first advertising start is retried by radiant_ble_adv
		 * and returns 0. */
		LOG_ERR("BLE did not start: %d. The ANT+ masters above are "
			"unaffected and this node is now ANT+ only - no FTMS, "
			"no RSC, and nothing to find in a BLE scan.", err);
	}
#endif

	return 0;
}
