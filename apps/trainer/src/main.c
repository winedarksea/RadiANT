/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadiANT trainer: an ANT+ FE-C bike trainer (equipment type 25) SIMULATOR,
 * with acknowledgement behaviour that can be programmed at runtime.
 *
 * TWO JOBS, ONE IMAGE:
 *
 *   A USABLE EXAMPLE. A spec-conformant controllable trainer: pages 16, 25 and
 *   17 in SS10.1's interleave, page 54 declaring basic resistance, target
 *   power and simulation, and every control page answered with a page 71.
 *   Zwift or any head unit pairs with it like a real one, and the simulated
 *   rider's power and cadence come from the shell.
 *
 *   A TEST INSTRUMENT. Every radiant master acknowledges at exactly data
 *   t_sync + 1560 us, the centre of the dongle's own 1560 +/- 250 us window,
 *   so pairing a dongle with one reproduces only the case that already
 *   passes. This one can be told, from the shell, to acknowledge late, early
 *   or jittered, to apply a command and lose its ACK, to never hear it, to
 *   lose them in bursts, or to go busy after each one - and it records the
 *   ground truth for every command at the far end. See trainer_ackfault.h for
 *   the model and trainer_shell.c for the record.
 *
 * NO BLE, AND THAT IS NOT AN OMISSION. MPSL would add arbitration and a
 * follow-on reserve around exactly the transmit instant under test, so an
 * image with the arbiter in it would measure the arbiter. CMakeLists.txt
 * refuses the combination.
 *
 * It is apps/treadmill's FE-C skeleton - the EVENT_TX-paced page load, the
 * radiant_event-thread -> workqueue hop for control pages, and the rule that
 * every path through apply_control_page() ends in a page 71 - with the
 * treadmill's state and simulator replaced by trainer_model.h. The treadmill
 * itself is untouched.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "ant_radio.h"
#include "ant_wire.h"
#include "node_ident.h"
#include "profile_common.h"
#include "profile_fec.h"
#include "profile_fec_tx.h"
#include "trainer_model.h"
#include "trainer_node.h"

LOG_MODULE_REGISTER(trainer, CONFIG_TRAINER_LOG_LEVEL);

/* The public ANT+ network key - not a stack licence key. */
static const uint8_t ant_plus_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

#define ANT_NETWORK    0u
#define ANT_RF_FREQ    57u
#define ANT_TRANS_TYPE 5u

#define FEC_CHANNEL 0u

/* One page 25 update per tick, at the channel rate: 4 Hz. */
#define MODEL_TICK_MS 250u

static struct trainer_model model;
static struct profile_fec_tx fec;

/* The rider, from the shell thread. -1 = no change pending. */
static atomic_t rider_power_req = ATOMIC_INIT(-1);
static atomic_t rider_cadence_req = ATOMIC_INIT(-1);

static void model_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(model_tick, model_tick_fn);

/* ── Model -> pages ─────────────────────────────────────────────────────── */

static void publish(void)
{
	struct profile_fec_general              g;
	struct profile_fec_settings             s;
	struct profile_fec_trainer              t;
	struct profile_fec_cmd_wind_resistance  w;
	struct profile_fec_cmd_track_resistance tr;

	trainer_model_pages(&model, &g, &s, &t);
	profile_fec_tx_set_general(&fec, &g);
	profile_fec_tx_set_settings(&fec, &s);
	profile_fec_tx_set_trainer(&fec, &t);

	trainer_model_echo(&model, &w, &tr);
	profile_fec_tx_set_wind(&fec, &w);
	profile_fec_tx_set_track(&fec, &tr);
}

/* ── The control path: acknowledged pages in, page 71 out ───────────────── */

/* FE-C control pages carry no sequence number, so page 71's is this FE's own
 * count of commands received - see apps/treadmill/src/main.c. */
static uint8_t fec_sequence;

/*
 * One acknowledged control page, ON THE SYSTEM WORKQUEUE. Every path ends in
 * profile_fec_tx_command_answered(), refusals included: SS8.8 makes page 71
 * an obligation, and silence is what a controller reports as broken.
 *
 * A command the fault model marked MUTE arrives here like any other - MUTE is
 * "applied, ACK lost" - so page 71 still advances for it. A DEAF one never
 * arrives at all. That difference is exactly what a controller can use to
 * tell the two apart from its side.
 */
static void apply_control_page(const uint8_t *body)
{
	uint8_t status = PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
	uint8_t page = body[0];

	fec_sequence++;

	switch (page) {
	case PROFILE_FEC_PAGE_BASIC_RESISTANCE: {
		struct profile_fec_cmd_basic_resistance cmd;

		if (profile_fec_decode_basic_resistance(body, &cmd) == 0) {
			trainer_model_basic_resistance(&model,
						       cmd.resistance_half_pct);
			status = PROFILE_FEC_CMD_STATUS_PASS;
		}
		break;
	}

	case PROFILE_FEC_PAGE_TARGET_POWER: {
		struct profile_fec_cmd_target_power cmd;

		if (profile_fec_decode_target_power(body, &cmd) == 0) {
			trainer_model_target_power(&model, cmd.power_quarter_w);
			status = PROFILE_FEC_CMD_STATUS_PASS;
		}
		break;
	}

	case PROFILE_FEC_PAGE_WIND_RESISTANCE: {
		struct profile_fec_cmd_wind_resistance cmd;

		if (profile_fec_decode_wind_resistance(body, &cmd) == 0) {
			trainer_model_wind(&model, &cmd);
			status = PROFILE_FEC_CMD_STATUS_PASS;
		}
		break;
	}

	case PROFILE_FEC_PAGE_TRACK_RESISTANCE: {
		struct profile_fec_cmd_track_resistance cmd;

		if (profile_fec_decode_track_resistance(body, &cmd) == 0) {
			trainer_model_track(&model, &cmd);
			status = PROFILE_FEC_CMD_STATUS_PASS;
			LOG_DBG("page 51: grade %d", (int)cmd.grade_centi_pct);
		}
		break;
	}

	case PROFILE_FEC_PAGE_USER_CONFIG: {
		struct profile_fec_cmd_user_config cmd;

		if (profile_fec_decode_user_config(body, &cmd) == 0) {
			trainer_model_user(&model, &cmd);
			status = PROFILE_FEC_CMD_STATUS_PASS;
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

	default:
		status = PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
		break;
	}

	/* The road changed, so the echo pages and page 17 change with it now
	 * rather than at the next tick. */
	publish();

	profile_fec_tx_command_answered(&fec, page, fec_sequence, status,
					&body[4]);
}

/*
 * The thread hop, as apps/treadmill's: antr_on_message() runs on radiant's
 * event thread and the model is workqueue-only, so the command is copied into
 * a small ring and the workqueue applies it. Invisible on air - page 71 goes
 * out on the next data slot either way.
 */
#define FEC_CMD_QUEUE_DEPTH 4u

static struct {
	uint8_t body[8];
} fec_cmd_q[FEC_CMD_QUEUE_DEPTH];

static uint8_t fec_cmd_head;
static uint8_t fec_cmd_tail;
static uint8_t fec_cmd_drop_page;
static bool    fec_cmd_dropped;
static struct k_spinlock fec_cmd_lock;

static void fec_cmd_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	for (;;) {
		k_spinlock_key_t key;
		uint8_t          body[8];
		uint8_t          drop_page = 0u;
		bool             have = false;
		bool             dropped = false;

		key = k_spin_lock(&fec_cmd_lock);
		if (fec_cmd_tail != fec_cmd_head) {
			memcpy(body, fec_cmd_q[fec_cmd_tail].body, sizeof(body));
			fec_cmd_tail = (uint8_t)((fec_cmd_tail + 1u) %
						 FEC_CMD_QUEUE_DEPTH);
			have = true;
		} else if (fec_cmd_dropped) {
			drop_page = fec_cmd_drop_page;
			fec_cmd_dropped = false;
			dropped = true;
		}
		k_spin_unlock(&fec_cmd_lock, key);

		if (have) {
			apply_control_page(body);
			continue;
		}
		if (dropped) {
			static const uint8_t no_echo[4] = { 0u, 0u, 0u, 0u };

			fec_sequence++;
			LOG_WRN("FE-C page %u dropped: the command queue was "
				"full", drop_page);
			profile_fec_tx_command_answered(
				&fec, drop_page, fec_sequence,
				PROFILE_FEC_CMD_STATUS_FAIL, no_echo);
			continue;
		}
		break;
	}
}

static K_WORK_DEFINE(fec_cmd_work, fec_cmd_work_fn);

static void queue_control_page(const uint8_t *body)
{
	k_spinlock_key_t key;
	uint8_t          next;

	key = k_spin_lock(&fec_cmd_lock);
	next = (uint8_t)((fec_cmd_head + 1u) % FEC_CMD_QUEUE_DEPTH);
	if (next == fec_cmd_tail) {
		fec_cmd_dropped = true;
		fec_cmd_drop_page = body[0];
	} else {
		memcpy(fec_cmd_q[fec_cmd_head].body, body, 8u);
		fec_cmd_head = next;
	}
	k_spin_unlock(&fec_cmd_lock, key);

	(void)k_work_submit(&fec_cmd_work);
}

/* ── The radio ──────────────────────────────────────────────────────────── */

/* Paced by EVENT_TX, as the treadmill's: the next page is staged exactly when
 * the last one went out. The encode is locked against the workqueue's
 * setters; the send is not, because it takes a mutex. */
static void load_fec_page(void)
{
	uint8_t                body[8];
	enum profile_slot_kind kind;

	k_sched_lock();
	kind = profile_fec_tx_next(&fec, body);
	k_sched_unlock();
	if (kind == PROFILE_SLOT_IDLE) {
		return;
	}
	(void)antr_broadcast_message_tx(FEC_CHANNEL, sizeof(body), body);
}

void antr_on_message(const struct antr_msg *msg)
{
	if (msg == NULL || msg->data == NULL) {
		return;
	}

	if (msg->id == ANTW_MESG_RESPONSE_EVENT_ID &&
	    msg->len >= ANTW_MESG_RESPONSE_EVENT_SIZE) {
		if (msg->data[2] == ANTW_EVENT_TX &&
		    msg->data[0] == FEC_CHANNEL) {
			load_fec_page();
		}
		return;
	}

	/* The control pages: acknowledged data into a channel this node is the
	 * MASTER of - FE-C's "C". */
	if ((msg->id == ANTW_MESG_ACKNOWLEDGED_DATA_ID ||
	     msg->id == ANTW_MESG_EXT_ACKNOWLEDGED_DATA_ID) &&
	    msg->len >= 9u && msg->data[0] == FEC_CHANNEL) {
		queue_control_page(&msg->data[1]);
	}
}

static int ant_master_start(uint8_t channel, uint16_t devnum)
{
	antr_err_t rc;

	rc = antr_channel_assign(channel, ANTW_CHANNEL_TYPE_MASTER, ANT_NETWORK,
				 0u);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u assign: %d", channel, (int)rc);
		return -EIO;
	}
	rc = antr_channel_id_set(channel, devnum, PROFILE_FEC_DEVICE_TYPE,
				 ANT_TRANS_TYPE);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u channel id: %d", channel, (int)rc);
		return -EIO;
	}
	rc = antr_channel_period_set(channel, PROFILE_FEC_PERIOD);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u period: %d", channel, (int)rc);
		return -EIO;
	}
	rc = antr_channel_radio_freq_set(channel, ANT_RF_FREQ);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u rf freq: %d", channel, (int)rc);
		return -EIO;
	}
	/* The fault hook goes on BEFORE the channel opens, so no command is
	 * ever answered half-registered. */
	if (trainer_ackfault_attach(channel) != 0) {
		LOG_ERR("ch%u ack fault hook did not register", channel);
		return -EIO;
	}
	rc = antr_channel_open(channel);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u open: %d", channel, (int)rc);
		return -EIO;
	}
	return 0;
}

static void model_tick_fn(struct k_work *w)
{
	atomic_val_t power = atomic_set(&rider_power_req, -1);
	atomic_val_t cadence = atomic_set(&rider_cadence_req, -1);

	ARG_UNUSED(w);

	if (power >= 0 || cadence >= 0) {
		trainer_model_set_rider(
			&model,
			(power >= 0) ? (uint16_t)power : model.rider_power_w,
			(cadence >= 0) ? (uint8_t)cadence : model.cadence_rpm);
	}

	trainer_model_tick(&model, MODEL_TICK_MS);
	publish();

	k_work_schedule(&model_tick, K_MSEC(MODEL_TICK_MS));
}

void trainer_node_set_rider(int32_t power_w, int32_t cadence_rpm)
{
	if (power_w >= 0) {
		atomic_set(&rider_power_req, power_w);
	}
	if (cadence_rpm >= 0) {
		atomic_set(&rider_cadence_req, cadence_rpm);
	}
}

void trainer_node_print(const struct shell *sh)
{
	static const char *const modes[] = { "sim", "erg", "basic" };
	struct trainer_model     m;

	/* The workqueue writes the model; a scheduler lock makes this copy a
	 * consistent snapshot without a mutex on the writer's side. */
	k_sched_lock();
	m = model;
	k_sched_unlock();

	shell_print(sh,
		    "mode=%s rider=%uW/%urpm grade=%d.%02d%% crr=%u/5e-5 "
		    "wind=%dkph cw=%u.%02u draft=%u.%02u mass=%u.%02u+%u.%02ukg",
		    modes[m.mode], m.rider_power_w, m.cadence_rpm,
		    (int)(m.grade_centi_pct / 100),
		    (int)((m.grade_centi_pct < 0 ? -m.grade_centi_pct
						 : m.grade_centi_pct) % 100),
		    m.crr_5e5, m.wind_speed_kph, m.wind_coeff_centi / 100u,
		    m.wind_coeff_centi % 100u, m.drafting_centi / 100u,
		    m.drafting_centi % 100u, m.user_weight_10g / 100u,
		    m.user_weight_10g % 100u, (m.bike_weight_50g * 5u) / 100u,
		    (m.bike_weight_50g * 5u) % 100u);
	shell_print(sh,
		    "speed=%u mm/s power=%u W resistance=%d N target=%u qW "
		    "basic=%u half-pct events=%u",
		    (unsigned)m.speed_mm_s, m.power_w, (int)m.resistance_n,
		    m.target_power_qw, m.resistance_half_pct, m.event_count);
}

static void fill_common_id(struct profile_common_id *id, uint16_t devnum)
{
	memset(id, 0, sizeof(*id));
	id->hw_revision = 1u;
	id->manufacturer_id = CONFIG_TRAINER_MANUFACTURER_ID;
	id->model_number = 1u;
	id->sw_revision_main = 1u;
	id->sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	id->serial_number = devnum;
}

int main(void)
{
	struct profile_fec_tx_cfg fec_cfg;
	uint16_t                  devnum = CONFIG_TRAINER_DEVICE_NUMBER;
	int                       err;

	LOG_INF("RadiANT trainer: ANT+ 0x11 FE-C, equipment type 25, "
		"programmable ACK faults");

	if (antr_init() != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("antr_init failed");
		return 0;
	}

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

	trainer_model_init(&model);

	memset(&fec_cfg, 0, sizeof(fec_cfg));
	fill_common_id(&fec_cfg.id, devnum);
	fec_cfg.equipment_type = PROFILE_FEC_TYPE_TRAINER;
	/* All three, because all three are honoured: page 48 drives BASIC,
	 * page 49 ERG, pages 50/51 SIM. Declaring a mode and refusing its
	 * page is the mistake profile_fec_tx.h warns about. */
	fec_cfg.caps.capabilities = PROFILE_FEC_CAP_BASIC_RESISTANCE |
				    PROFILE_FEC_CAP_TARGET_POWER |
				    PROFILE_FEC_CAP_SIMULATION;
	fec_cfg.caps.max_resistance_n = TRAINER_MAX_RESISTANCE_N;
	fec_cfg.caps.max_resistance_valid = true;
	fec_cfg.metabolic = false;

	err = profile_fec_tx_init(&fec, &fec_cfg);
	if (err != 0) {
		LOG_ERR("profile_fec_tx_init: %d", err);
		return 0;
	}

	trainer_model_tick(&model, 0u);
	publish();

	if (antr_network_address_set(ANT_NETWORK, ant_plus_key) !=
	    ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("network key");
		return 0;
	}

	if (ant_master_start(FEC_CHANNEL, devnum) != 0) {
		return 0;
	}

	k_work_schedule(&model_tick, K_MSEC(MODEL_TICK_MS));
	LOG_INF("transmitting: device #%u", devnum);
	return 0;
}
