/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadiANT smart trainer: the mixed ANT+ / BLE reference design for a
 * controllable bike trainer.
 *
 *   ANT+ FE-C (0x11, equipment type 25): pages 16, 25 and 17 in SS10.1's
 *   interleave, 80/81 from the product record, 54/55/50/51/71 on request,
 *   control pages 48/49/50/51 answered with page 71, page 55 applied.
 *
 *   BLE FTMS Indoor Bike (CONFIG_SMART_TRAINER_BLE): Indoor Bike Data, the
 *   Supported Power and Resistance ranges, the Control Point (Request
 *   Control, Reset, Set Target Resistance/Power, Start/Stop, Indoor Bike
 *   Simulation, Wheel Circumference), DIS, bonding behind a pairing window.
 *
 * ONE EQUIPMENT STATE (trainer_equip.h) behind both radios, ONE command layer
 * (trainer_cmd.h) for both control paths, and the shared FE-C/FTMS arbiter
 * (radiant_ctrl_arbiter.h) between them: an FTMS Request Control is explicit
 * and beats an FE-C controller's implicit claim, which is then answered
 * REJECTED. Nothing is ever suppressed: FE-C pages and FTMS notifications run
 * for every listener whoever holds control.
 *
 * ONE SEAM (trainer_source.h) for the manufacturer's brake. The reference
 * driver is a simulated rider on apps/trainer's physics model.
 *
 * apps/trainer, the ACK-fault instrument, is a different image: this one has
 * none of its fault machinery and is built to carry BLE.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <radiant/radiant_ant.h>
#include <radiant/radiant_wire.h>
#include <radiant/radiant_product.h>
#include <radiant/radiant_profile_common.h>
#include <radiant/radiant_profile_fec.h>
#include <radiant/radiant_profile_fec_tx.h>
#if defined(CONFIG_RADIANT_CTRL_ARBITER)
#include <radiant/radiant_ctrl_arbiter.h>
#endif
#if defined(CONFIG_RADIANT_BLE_PAIRING_WINDOW)
#include <radiant/radiant_ble_pairing.h>
#endif

#include "trainer_cmd.h"
#include "trainer_equip.h"
#include "trainer_logic.h"
#include "trainer_node.h"
#include "trainer_source.h"
#if defined(CONFIG_SMART_TRAINER_BLE)
#include "trainer_ble.h"
#endif

LOG_MODULE_REGISTER(smart_trainer, CONFIG_SMART_TRAINER_LOG_LEVEL);

/* The public ANT+ network key - not a stack licence key. */
static const uint8_t ant_plus_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

#define ANT_NETWORK    0u
#define ANT_RF_FREQ    57u
#define ANT_TRANS_TYPE 5u
#define FEC_CHANNEL    0u

/* One page 25 update per tick, at the channel rate: 4 Hz. */
#define NODE_TICK_MS 250u

#if defined(CONFIG_RADIANT_CTRL_ARBITER)
_Static_assert(RADIANT_CTRL_PAGE_BASIC_RESISTANCE == RADIANT_PROFILE_FEC_PAGE_BASIC_RESISTANCE &&
		       RADIANT_CTRL_PAGE_TARGET_POWER == RADIANT_PROFILE_FEC_PAGE_TARGET_POWER &&
		       RADIANT_CTRL_PAGE_WIND_RESISTANCE == RADIANT_PROFILE_FEC_PAGE_WIND_RESISTANCE &&
		       RADIANT_CTRL_PAGE_TRACK_RESISTANCE == RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE,
	       "radiant_ctrl_arbiter.h's FE-C page numbers have drifted");
#endif

static struct trainer_equip equip;
static struct radiant_profile_fec_tx fec;
static atomic_t session_epoch;

static void node_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(node_tick, node_tick_fn);

/* ── Equipment state -> FE-C pages ──────────────────────────────────────── */

static void publish(void)
{
	struct radiant_profile_fec_general              g;
	struct radiant_profile_fec_settings             s;
	struct radiant_profile_fec_trainer              t;
	struct radiant_profile_fec_cmd_wind_resistance  w;
	struct radiant_profile_fec_cmd_track_resistance tr;
	struct radiant_profile_fec_cmd_user_config      u;

	trainer_equip_pages(&equip, &g, &s, &t);
	trainer_equip_echo(&equip, &w, &tr, &u);

	/* The encode runs on the radio callback; the workqueue can preempt
	 * nothing there, but this copy must not be torn by it either. */
	k_sched_lock();
	radiant_profile_fec_tx_set_general(&fec, &g);
	radiant_profile_fec_tx_set_settings(&fec, &s);
	radiant_profile_fec_tx_set_trainer(&fec, &t);
	radiant_profile_fec_tx_set_wind(&fec, &w);
	radiant_profile_fec_tx_set_track(&fec, &tr);
	radiant_profile_fec_tx_set_user(&fec, &u);
	k_sched_unlock();
}

/* ── The node, as trainer_source.c and trainer_ble.c see it ─────────────── */

struct trainer_equip *trainer_node_equip(void)
{
	return &equip;
}

uint32_t trainer_node_epoch(void)
{
	return (uint32_t)atomic_get(&session_epoch);
}

#if defined(CONFIG_SMART_TRAINER_BLE)
/* Set once trainer_ble_start() has brought the controller up. */
static atomic_t ble_up;
#endif

/* trainer_logic's session callback: the epoch mirror for the BT RX thread,
 * and the FTMS notification unless the logic says the core sends its own. */
static void node_session(enum trainer_run prev, enum trainer_run next, bool notify, void *ctx)
{
	ARG_UNUSED(ctx);
	atomic_set(&session_epoch, (atomic_val_t)equip.epoch);
	if (prev == next) {
		return;
	}
	LOG_INF("session %d -> %d (epoch %u)", (int)prev, (int)next, (unsigned)equip.epoch);
#if defined(CONFIG_SMART_TRAINER_BLE)
	if (notify && atomic_get(&ble_up) != 0) {
		trainer_ble_session_changed(prev, next);
	}
#else
	ARG_UNUSED(notify);
#endif
}

static void node_ftms_status(const uint8_t *msg, size_t len, void *ctx)
{
	ARG_UNUSED(ctx);
#if defined(CONFIG_SMART_TRAINER_BLE)
	/* FTMS subscribers hear a target set over ANT+ too. */
	if (atomic_get(&ble_up) != 0) {
		trainer_ble_notify_status(msg, len);
	}
#else
	ARG_UNUSED(msg);
	ARG_UNUSED(len);
#endif
}

void trainer_node_report(const struct trainer_report *r)
{
	trainer_logic_report(r);
}

void trainer_node_event(enum trainer_source_event evt)
{
	switch (evt) {
	case TRAINER_EVT_PAIRING_WINDOW:
#if defined(CONFIG_RADIANT_BLE_PAIRING_WINDOW)
		/* New phones only inside a window opened at the trainer
		 * (radiant_ble_pairing.h): physical presence authorises. Not
		 * before the controller is up. */
		if (atomic_get(&ble_up) == 0) {
			LOG_WRN("pairing window not opened: BLE is not up yet");
			return;
		}
		(void)radiant_ble_pairing_window_open(CONFIG_SMART_TRAINER_PAIRING_WINDOW_S);
		LOG_INF("pairing window open for %u s",
			(unsigned)CONFIG_SMART_TRAINER_PAIRING_WINDOW_S);
#endif
		return;
	case TRAINER_EVT_FORGET_BONDS:
#if defined(CONFIG_RADIANT_BLE_PAIRING_WINDOW)
		if (atomic_get(&ble_up) == 0) {
			LOG_WRN("bonds not forgotten: BLE is not up yet");
			return;
		}
		LOG_WRN("forgetting every bond: %d", radiant_ble_bonds_forget_all());
#endif
		return;
	default: {
		static bool release_warned;
		int rc = trainer_logic_event(evt);

		/* Once: a driver that cannot release would otherwise log at
		 * every console reset. */
		if (rc != 0 && !release_warned) {
			release_warned = true;
			LOG_WRN("console reset: the brake was not released (%d); "
				"further failures are not logged", rc);
		}
		publish();
		return;
	}
	}
}

/* FTMS Reset succeeded (trainer_ble.c's reset_done). Workqueue. */
void trainer_node_ftms_reset_done(void)
{
	trainer_logic_ftms_reset_done();
	publish();
}

/* ── FE-C: acknowledged control pages in, page 71 out ───────────────────── */

static void apply_control_page(const uint8_t *body, uint32_t rx_epoch)
{
	trainer_logic_fec_page(body, rx_epoch, (uint32_t)k_uptime_get_32());
	publish();
}

/*
 * The thread hop, as apps/treadmill's: radiant_ant_on_message() runs on
 * radiant's event thread and the equipment state is workqueue-only, so a
 * control page is copied into a small ring WITH THE EPOCH AT RECEIPT, and the
 * workqueue applies it. A page that waited out a local stop is refused.
 */
#define FEC_CMD_QUEUE_DEPTH 4u

static struct {
	uint8_t  body[8];
	uint32_t epoch;
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
		uint32_t         epoch = 0u;
		uint8_t          drop_page = 0u;
		bool             have = false;
		bool             dropped = false;

		key = k_spin_lock(&fec_cmd_lock);
		if (fec_cmd_tail != fec_cmd_head) {
			memcpy(body, fec_cmd_q[fec_cmd_tail].body, sizeof(body));
			epoch = fec_cmd_q[fec_cmd_tail].epoch;
			fec_cmd_tail = (uint8_t)((fec_cmd_tail + 1u) % FEC_CMD_QUEUE_DEPTH);
			have = true;
		} else if (fec_cmd_dropped) {
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
			LOG_WRN("FE-C page %u dropped: the command queue was full", drop_page);
			trainer_logic_fec_dropped(drop_page);
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
		fec_cmd_q[fec_cmd_head].epoch = (uint32_t)atomic_get(&session_epoch);
		fec_cmd_head = next;
	}
	k_spin_unlock(&fec_cmd_lock, key);

	(void)k_work_submit(&fec_cmd_work);
}

/* ── The radio ──────────────────────────────────────────────────────────── */

/* Paced by EVENT_TX: the next page is staged when the last one went out. */
static void load_fec_page(void)
{
	uint8_t                        body[8];
	enum radiant_profile_slot_kind kind;

	k_sched_lock();
	kind = radiant_profile_fec_tx_next(&fec, body);
	k_sched_unlock();
	if (kind == RADIANT_PROFILE_SLOT_IDLE) {
		return;
	}
	(void)radiant_ant_broadcast_message_tx(FEC_CHANNEL, sizeof(body), body);
}

void radiant_ant_on_message(const struct radiant_ant_msg *msg)
{
	if (msg == NULL || msg->data == NULL) {
		return;
	}

	if (msg->id == RADIANT_WIRE_MESG_RESPONSE_EVENT_ID &&
	    msg->len >= RADIANT_WIRE_MESG_RESPONSE_EVENT_SIZE) {
		if (msg->data[2] == RADIANT_WIRE_EVENT_TX && msg->data[0] == FEC_CHANNEL) {
			load_fec_page();
		}
		return;
	}

	/* Control pages: acknowledged data into the channel this node is the
	 * master of. */
	if ((msg->id == RADIANT_WIRE_MESG_ACKNOWLEDGED_DATA_ID ||
	     msg->id == RADIANT_WIRE_MESG_EXT_ACKNOWLEDGED_DATA_ID) &&
	    msg->len >= 9u && msg->data[0] == FEC_CHANNEL) {
		queue_control_page(&msg->data[1]);
	}
}

static int ant_master_start(uint8_t channel, uint16_t devnum)
{
	radiant_ant_err_t rc;

	rc = radiant_ant_channel_assign(channel, RADIANT_WIRE_CHANNEL_TYPE_MASTER, ANT_NETWORK,
					0u);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u assign: %d", channel, (int)rc);
		return -EIO;
	}
	rc = radiant_ant_channel_id_set(channel, devnum, RADIANT_PROFILE_FEC_DEVICE_TYPE,
					ANT_TRANS_TYPE);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u channel id: %d", channel, (int)rc);
		return -EIO;
	}
	rc = radiant_ant_channel_period_set(channel, RADIANT_PROFILE_FEC_PERIOD);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u period: %d", channel, (int)rc);
		return -EIO;
	}
	rc = radiant_ant_channel_radio_freq_set(channel, ANT_RF_FREQ);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u rf freq: %d", channel, (int)rc);
		return -EIO;
	}
	rc = radiant_ant_channel_open(channel);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("ch%u open: %d", channel, (int)rc);
		return -EIO;
	}
	load_fec_page();
	return 0;
}

static void node_tick_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	trainer_equip_tick(&equip, NODE_TICK_MS);
#if defined(CONFIG_RADIANT_CTRL_ARBITER)
	/* ANT+ is connectionless: an idle FE-C claim lapses only here. */
	radiant_ctrl_tick((uint32_t)k_uptime_get_32());
#endif
#if defined(CONFIG_SMART_TRAINER_BLE)
	trainer_ble_tick();
#endif
	publish();
	k_work_schedule(&node_tick, K_MSEC(NODE_TICK_MS));
}

int main(void)
{
	const struct trainer_source_ops *src = trainer_source_get();
	const struct radiant_product    *product = radiant_product_get();
	struct radiant_profile_fec_tx_cfg fec_cfg;
	struct radiant_product_unit       unit;
	struct trainer_cmd_cfg            cmd_cfg;
	int                               err;

	LOG_INF("RadiANT smart trainer: ANT+ 0x11 FE-C (type 25)%s",
		IS_ENABLED(CONFIG_SMART_TRAINER_BLE) ? " + BLE FTMS Indoor Bike" : "");

	if (radiant_ant_init() != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("radiant_ant_init failed");
		return 0;
	}

	/* Per-unit device number and serial from the identity record (ADR
	 * 0009/0020). A strict build does not return until it boots; a
	 * development build falls back to the Kconfig number, logged. */
	(void)radiant_product_unit_boot(CONFIG_SMART_TRAINER_DEVICE_NUMBER, &unit);

	if (src == NULL) {
		LOG_ERR("no trainer source registered: FE-C goes out with every "
			"measurement invalid and no control mode declared. See "
			"src/trainer_source.h.");
	}

	trainer_equip_init(&equip, (src != NULL) ? src->max_resistance_n : 0u);
	atomic_set(&session_epoch, (atomic_val_t)equip.epoch);

	memset(&cmd_cfg, 0, sizeof(cmd_cfg));
	cmd_cfg.equip = &equip;
	cmd_cfg.src = src;
	cmd_cfg.allow_erg = IS_ENABLED(CONFIG_SMART_TRAINER_CAP_ERG);
	cmd_cfg.allow_resistance = IS_ENABLED(CONFIG_SMART_TRAINER_CAP_RESISTANCE);
	cmd_cfg.allow_sim = IS_ENABLED(CONFIG_SMART_TRAINER_CAP_SIM);
	cmd_cfg.min_power_w = CONFIG_SMART_TRAINER_MIN_POWER_W;
	cmd_cfg.max_power_w = CONFIG_SMART_TRAINER_MAX_POWER_W;
	if (src != NULL && src->max_power_w != 0u) {
		cmd_cfg.min_power_w = src->min_power_w;
		cmd_cfg.max_power_w = src->max_power_w;
	}
#if defined(CONFIG_RADIANT_CTRL_ARBITER)
	cmd_cfg.ctrl_owner = radiant_ctrl_owner;
	cmd_cfg.ctrl_claim = radiant_ctrl_claim;
	cmd_cfg.ctrl_gated = radiant_ctrl_fec_page_is_gated;
#endif
	trainer_cmd_bind(&cmd_cfg);
	{
		const struct trainer_logic_cfg lcfg = {
			.equip = &equip,
			.fec = &fec,
			.session_changed = node_session,
			.ftms_status = node_ftms_status,
		};

		trainer_logic_init(&lcfg);
	}
#if defined(CONFIG_RADIANT_CTRL_ARBITER)
	/* Every control page this trainer implements needs the token. */
	radiant_ctrl_init(CONFIG_SMART_TRAINER_CTRL_ANT_TIMEOUT_MS, trainer_cmd_ctrl_gates());
#endif

	memset(&fec_cfg, 0, sizeof(fec_cfg));
	(void)radiant_product_fill_common_id(product, unit.serial, &fec_cfg.id);
	fec_cfg.equipment_type = RADIANT_PROFILE_FEC_TYPE_TRAINER;
	/* Declared exactly when honoured (trainer_cmd.c): a declared mode
	 * whose page is refused, or the reverse, is a broken trainer to every
	 * controller. */
	fec_cfg.caps.capabilities = trainer_cmd_fec_capabilities();
	fec_cfg.caps.max_resistance_n = equip.max_resistance_n;
	fec_cfg.caps.max_resistance_valid = (equip.max_resistance_n != 0u);
	fec_cfg.metabolic = false;
	err = radiant_profile_fec_tx_init(&fec, &fec_cfg);
	if (err != 0) {
		LOG_ERR("radiant_profile_fec_tx_init: %d", err);
		return 0;
	}

	/* Deliver what the driver (or a button) pushed before now only once
	 * the equipment state exists AND the driver has started: a console
	 * reset queued at boot drives set_* hooks, which a driver that has not
	 * started cannot take. A driver that fails to start never gets them. */
	err = 0;
	if (src != NULL) {
		LOG_INF("trainer source: %s", src->name);
		if (src->start != NULL && (err = src->start()) != 0) {
			LOG_ERR("trainer source did not start: %d", err);
		}
	}
	if (err == 0) {
		trainer_source_ready();
	}
	publish();

	if (radiant_ant_network_address_set(ANT_NETWORK, ant_plus_key) !=
	    RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("network key");
		return 0;
	}
	if (ant_master_start(FEC_CHANNEL, unit.devnum) != 0) {
		return 0;
	}
	k_work_schedule(&node_tick, K_MSEC(NODE_TICK_MS));
	LOG_INF("transmitting: device #%u, FE-C caps 0x%02x", unit.devnum,
		fec_cfg.caps.capabilities);

#if defined(CONFIG_SMART_TRAINER_BLE)
	/* Last, so a failure to advertise is never mistaken for a failure to
	 * transmit. */
	{
		struct radiant_ble_ftms_caps caps;

		trainer_cmd_ftms_caps(&caps);
		err = trainer_ble_start(unit.devnum, unit.serial, &caps);
		atomic_set(&ble_up, (err == 0) ? 1 : 0);
		if (err != 0) {
			LOG_ERR("BLE did not start: %d. The ANT+ master is unaffected "
				"and this node is now ANT+ only.", err);
		}
	}
#endif
	return 0;
}
