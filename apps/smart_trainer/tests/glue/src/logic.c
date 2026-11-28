/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_logic (main.c's decisions, now kernel-free) against the real FE-C
 * master, and trainer_inbox's ordering rule.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_ble_ftms.h>
#include <radiant/radiant_ctrl_arbiter.h>
#include <radiant/radiant_profile_fec_tx.h>

#include "fake.h"
#include "trainer_cmd.h"
#include "trainer_equip.h"
#include "trainer_inbox.h"
#include "trainer_logic.h"

static struct trainer_equip equip;
static struct radiant_profile_fec_tx fec;

static int session_calls;
static int session_notified;
static enum trainer_run last_prev, last_next;
static int status_calls;
static uint8_t last_status[RADIANT_BLE_FTMS_STATUS_MAX];

static void on_session(enum trainer_run prev, enum trainer_run next, bool notify, void *ctx)
{
	ARG_UNUSED(ctx);
	session_calls++;
	if (notify && prev != next) {
		session_notified++;
	}
	last_prev = prev;
	last_next = next;
}

static void on_status(const uint8_t *msg, size_t len, void *ctx)
{
	ARG_UNUSED(ctx);
	status_calls++;
	memcpy(last_status, msg, MIN(len, sizeof(last_status)));
}

/* What main.c's publish() does each tick for the echo pages. */
static void publish(void)
{
	struct radiant_profile_fec_cmd_wind_resistance w;
	struct radiant_profile_fec_cmd_track_resistance tr;
	struct radiant_profile_fec_cmd_user_config u;

	trainer_equip_echo(&equip, &w, &tr, &u);
	radiant_profile_fec_tx_set_wind(&fec, &w);
	radiant_profile_fec_tx_set_track(&fec, &tr);
	radiant_profile_fec_tx_set_user(&fec, &u);
}

static void setup(const struct trainer_source_ops *src)
{
	struct trainer_cmd_cfg c;
	struct radiant_profile_fec_tx_cfg fc;
	struct trainer_logic_cfg lc;

	fake_reset();
	session_calls = session_notified = status_calls = 0;
	trainer_equip_init(&equip, (src != NULL) ? src->max_resistance_n : 0u);
	memset(&c, 0, sizeof(c));
	c.equip = &equip;
	c.src = src;
	c.allow_erg = c.allow_resistance = c.allow_sim = true;
	c.max_power_w = 2000u;
	c.ctrl_owner = radiant_ctrl_owner;
	c.ctrl_claim = radiant_ctrl_claim;
	c.ctrl_gated = radiant_ctrl_fec_page_is_gated;
	trainer_cmd_bind(&c);
	radiant_ctrl_init(30000u, trainer_cmd_ctrl_gates());

	memset(&fc, 0, sizeof(fc));
	fc.id.manufacturer_id = 255u;
	fc.id.serial_number = RADIANT_PROFILE_COMMON_INVALID_U32;
	fc.id.sw_revision_supplemental = RADIANT_PROFILE_COMMON_INVALID_U8;
	fc.equipment_type = RADIANT_PROFILE_FEC_TYPE_TRAINER;
	fc.caps.capabilities = trainer_cmd_fec_capabilities();
	fc.caps.max_resistance_n = equip.max_resistance_n;
	fc.caps.max_resistance_valid = true;
	zassert_equal(radiant_profile_fec_tx_init(&fec, &fc), 0);

	memset(&lc, 0, sizeof(lc));
	lc.equip = &equip;
	lc.fec = &fec;
	lc.session_changed = on_session;
	lc.ftms_status = on_status;
	trainer_logic_init(&lc);
	publish();
}

static void page(uint8_t *b, uint8_t p)
{
	memset(b, 0xFF, 8);
	b[0] = p;
}

static void power_page(uint8_t *b)
{
	page(b, RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	b[6] = 0x20u; /* 0x0320 x 0.25 W = 200 W */
	b[7] = 0x03u;
}

static void request(uint8_t *b, uint8_t p)
{
	page(b, RADIANT_PROFILE_FEC_PAGE_REQUEST);
	b[5] = 1u;
	b[6] = p;
	b[7] = RADIANT_PROFILE_FEC_REQUEST_CMD_DATA_PAGE;
}

static bool find_page(uint8_t want, uint8_t *got)
{
	for (int i = 0; i < 40; i++) {
		uint8_t b[8];

		(void)radiant_profile_fec_tx_next(&fec, b);
		if (b[0] == want) {
			memcpy(got, b, 8);
			return true;
		}
	}
	return false;
}

/* ── Page 70 -> 54/55/71 round trips ─────────────────────────────────────── */

ZTEST(trainer_logic, test_page_70_round_trips_report_the_trainer)
{
	uint8_t b[8];
	uint8_t got[8];

	setup(&fake_full);

	request(b, RADIANT_PROFILE_FEC_PAGE_CAPABILITIES);
	trainer_logic_fec_page(b, equip.epoch, 0u);
	zassert_equal(trainer_logic_sequence(), RADIANT_PROFILE_FEC_CMD_NO_SEQ,
		      "a request is not a command");
	zassert_true(find_page(RADIANT_PROFILE_FEC_PAGE_CAPABILITIES, got));
	zassert_equal(got[7], 0x07u);
	zassert_equal(got[5] | (got[6] << 8), 2000);

	/* A page 49; then 71 on request reports 49, not the request. */
	power_page(b);
	trainer_logic_fec_page(b, equip.epoch, 0u);
	zassert_equal(trainer_logic_sequence(), 0u, "255 -> 0 at the first command");
	zassert_equal(status_calls, 1, "FTMS subscribers hear the new target");
	zassert_equal(last_status[0], RADIANT_BLE_FTMS_STATUS_TARGET_POWER);
	zassert_true(find_page(RADIANT_PROFILE_FEC_PAGE_CMD_STATUS, got));
	request(b, RADIANT_PROFILE_FEC_PAGE_CMD_STATUS);
	trainer_logic_fec_page(b, equip.epoch, 0u);
	zassert_true(find_page(RADIANT_PROFILE_FEC_PAGE_CMD_STATUS, got));
	zassert_equal(got[1], RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	zassert_equal(got[2], 0u);
	zassert_equal(got[3], RADIANT_PROFILE_FEC_CMD_STATUS_PASS);
	zassert_equal(got[6], 0x20u, "Table 8-49: target power LSB echoed");
	zassert_equal(got[7], 0x03u);

	/* 55 after an FE-C page 55: not a command (no sequence), echoed. */
	page(b, RADIANT_PROFILE_FEC_PAGE_USER_CONFIG);
	b[1] = 0x40u; /* 80.00 kg */
	b[2] = 0x1Fu;
	b[4] = 0x40u;
	b[5] = 0x0Bu; /* bike 9.00 kg */
	b[6] = 70u;
	b[7] = 0x00u;
	trainer_logic_fec_page(b, equip.epoch, 0u);
	zassert_equal(trainer_logic_sequence(), 0u);
	publish();
	request(b, RADIANT_PROFILE_FEC_PAGE_USER_CONFIG);
	trainer_logic_fec_page(b, equip.epoch, 0u);
	zassert_true(find_page(RADIANT_PROFILE_FEC_PAGE_USER_CONFIG, got));
	zassert_equal(got[1] | (got[2] << 8), 8000);
	zassert_equal(got[6], 70u);
	zassert_equal((got[4] >> 4) | (got[5] << 4), 180);
}

/* ── Sequence and the drop path (R10) ───────────────────────────────────── */

ZTEST(trainer_logic, test_sequence_counts_supported_commands_only)
{
	uint8_t b[8];

	setup(&fake_erg_only);
	page(b, RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE);
	trainer_logic_fec_page(b, equip.epoch, 0u);
	zassert_equal(trainer_logic_sequence(), RADIANT_PROFILE_FEC_CMD_NO_SEQ,
		      "an unsupported page is ignored, not counted");
	power_page(b);
	for (int i = 0; i < 256; i++) {
		trainer_logic_fec_page(b, equip.epoch, (uint32_t)i);
	}
	zassert_equal(trainer_logic_sequence(), 0u, "0..254 then back to 0, never 255");
}

ZTEST(trainer_logic, test_dropped_page_answered_only_when_supported)
{
	uint8_t got[8];

	setup(&fake_erg_only);
	trainer_logic_fec_dropped(RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE);
	trainer_logic_fec_dropped(RADIANT_PROFILE_FEC_PAGE_USER_CONFIG);
	trainer_logic_fec_dropped(RADIANT_PROFILE_FEC_PAGE_REQUEST);
	zassert_equal(trainer_logic_sequence(), RADIANT_PROFILE_FEC_CMD_NO_SEQ);
	zassert_false(find_page(RADIANT_PROFILE_FEC_PAGE_CMD_STATUS, got));

	trainer_logic_fec_dropped(RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	zassert_equal(trainer_logic_sequence(), 0u);
	zassert_true(find_page(RADIANT_PROFILE_FEC_PAGE_CMD_STATUS, got));
	zassert_equal(got[1], RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	zassert_equal(got[3], RADIANT_PROFILE_FEC_CMD_STATUS_FAIL);
}

/* ── Resets ─────────────────────────────────────────────────────────────── */

ZTEST(trainer_logic, test_console_reset_releases_the_brake)
{
	uint8_t b[8];

	setup(&fake_full);
	power_page(b);
	trainer_logic_fec_page(b, equip.epoch, 0u);
	zassert_equal(equip.mode, TRAINER_EQUIP_ERG);

	zassert_equal(trainer_logic_event(TRAINER_EVT_CONSOLE_RESET), 0);
	zassert_equal(fake.n_sim, 1, "commanded through the hooks");
	zassert_equal(fake.sim.grade_centi_pct, 0);
	zassert_equal(equip.mode, TRAINER_EQUIP_FREE);
	zassert_equal(equip.run, TRAINER_RUN_IDLE);

	/* A brake that refuses: the error comes back, the mode is kept. */
	trainer_logic_fec_page(b, equip.epoch, 0u);
	fake.rc = -EIO;
	zassert_equal(trainer_logic_event(TRAINER_EVT_CONSOLE_RESET), -EIO);
	zassert_equal(equip.mode, TRAINER_EQUIP_ERG);
}

ZTEST(trainer_logic, test_ftms_reset_is_quiet_and_moves_the_epoch)
{
	uint8_t b[8];
	struct trainer_report r;
	uint32_t rx;

	setup(&fake_full);
	memset(&r, 0, sizeof(r));
	r.pedalling = true;
	trainer_logic_report(&r);
	zassert_equal(session_notified, 1, "IDLE -> RUNNING is notified");

	/* A page 49 received before the Reset, applied after it. */
	rx = equip.epoch;
	trainer_logic_ftms_reset_done();
	zassert_equal(equip.run, TRAINER_RUN_IDLE);
	zassert_equal(last_next, TRAINER_RUN_IDLE);
	zassert_equal(session_notified, 1,
		      "no Stopped/Idle from here: the core sends Reset (0x01) and Idle");
	zassert_equal(equip.epoch, rx + 1u);

	power_page(b);
	trainer_logic_fec_page(b, rx, 0u);
	zassert_equal(fake.n_power, 0, "the queued page does not re-apply a target");
	zassert_equal(trainer_logic_sequence(), 0u, "but it is answered (REJECTED)");
}

/* ── The inbox (R2) ─────────────────────────────────────────────────────── */

ZTEST(trainer_logic, test_an_event_drops_the_report_from_before_it)
{
	struct trainer_inbox ib;
	struct trainer_report r;
	struct trainer_report out;
	enum trainer_source_event evt;

	trainer_inbox_init(&ib);
	memset(&r, 0, sizeof(r));
	r.pedalling = true;
	r.power_w = 111u;
	trainer_inbox_report(&ib, &r);
	zassert_equal(trainer_inbox_event(&ib, TRAINER_EVT_PAUSED), 0);
	zassert_equal(trainer_inbox_pop(&ib, &out, &evt), TRAINER_INBOX_EVENT);
	zassert_equal(evt, TRAINER_EVT_PAUSED);
	zassert_equal(trainer_inbox_pop(&ib, &out, &evt), TRAINER_INBOX_EMPTY,
		      "the pedalling report from before the Pause is gone");

	/* A report after the event is kept, and comes after it. */
	(void)trainer_inbox_event(&ib, TRAINER_EVT_STOPPED);
	r.power_w = 222u;
	trainer_inbox_report(&ib, &r);
	zassert_equal(trainer_inbox_pop(&ib, &out, &evt), TRAINER_INBOX_EVENT);
	zassert_equal(trainer_inbox_pop(&ib, &out, &evt), TRAINER_INBOX_REPORT);
	zassert_equal(out.power_w, 222u);

	/* Through the logic: the Pause holds. */
	setup(&fake_full);
	trainer_logic_report(&r);
	zassert_equal(equip.run, TRAINER_RUN_RUNNING);
	(void)trainer_logic_event(TRAINER_EVT_PAUSED);
	trainer_logic_report(&r); /* even a late pedalling report */
	zassert_equal(equip.run, TRAINER_RUN_PAUSED);

	/* A full queue refuses the event and keeps the report. */
	trainer_inbox_init(&ib);
	trainer_inbox_report(&ib, &r);
	for (unsigned int i = 0; i < TRAINER_INBOX_EVENTS - 1u; i++) {
		zassert_equal(trainer_inbox_event(&ib, TRAINER_EVT_STARTED), 0);
	}
	trainer_inbox_report(&ib, &r);
	zassert_equal(trainer_inbox_event(&ib, TRAINER_EVT_STOPPED), -ENOMEM);
	zassert_true(ib.have_rep);
}

/* ── Advertising Service Data (B1) ──────────────────────────────────────── */

ZTEST(trainer_logic, test_indoor_bike_service_data_bytes)
{
	static const uint8_t want[RADIANT_BLE_FTMS_SVC_DATA_LEN] = { 0x26, 0x18, 0x01, 0x20, 0x00 };
	uint8_t got[RADIANT_BLE_FTMS_SVC_DATA_LEN];

	zassert_equal(radiant_ble_ftms_service_data(got, sizeof(got), true,
						    RADIANT_BLE_FTMS_TYPE_INDOOR_BIKE),
		      sizeof(got));
	zassert_mem_equal(got, want, sizeof(want),
			  "trainer_ble.c's constant must equal the core's bytes");
}

ZTEST_SUITE(trainer_logic, NULL, NULL, NULL, NULL, NULL);
