/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_logic.c - see trainer_logic.h. No Zephyr header.
 */

#include <string.h>

#include "trainer_cmd.h"
#include "trainer_logic.h"

static struct trainer_logic_cfg cfg;
static uint8_t fec_sequence = RADIANT_PROFILE_FEC_CMD_NO_SEQ;

void trainer_logic_init(const struct trainer_logic_cfg *c)
{
	cfg = *c;
	fec_sequence = RADIANT_PROFILE_FEC_CMD_NO_SEQ;
}

uint8_t trainer_logic_sequence(void)
{
	return fec_sequence;
}

/* SS8.12.3.2: 255 means "none yet"; the count runs 0..254. */
static uint8_t next_sequence(void)
{
	fec_sequence = (fec_sequence >= 254u) ? 0u : (uint8_t)(fec_sequence + 1u);
	return fec_sequence;
}

static void session(enum trainer_run prev, bool notify)
{
	if (cfg.session_changed != NULL) {
		cfg.session_changed(prev, cfg.equip->run, notify, cfg.ctx);
	}
}

void trainer_logic_fec_page(const uint8_t *body, uint32_t rx_epoch, uint32_t now_ms)
{
	struct trainer_fec_answer a;

	if (body == NULL || cfg.fec == NULL) {
		return;
	}
	if (body[0] == RADIANT_PROFILE_FEC_PAGE_REQUEST) {
		/* A request, never answered with a page 71 (SS8.12.3.1). */
		(void)radiant_profile_fec_tx_handle_request(cfg.fec, body);
		return;
	}

	a = trainer_cmd_fec(body, rx_epoch, now_ms);
	if (a.answer) {
		/* Bytes [4..7] of the command go back position for position
		 * (Table 8-49). */
		radiant_profile_fec_tx_command_answered(cfg.fec, body[0], next_sequence(),
							a.status, &body[4]);
	}
	if (a.ftms_status_len != 0u && cfg.ftms_status != NULL) {
		cfg.ftms_status(a.ftms_status, a.ftms_status_len, cfg.ctx);
	}
}

void trainer_logic_fec_dropped(uint8_t page)
{
	if (cfg.fec == NULL || !trainer_cmd_fec_page_supported(page)) {
		return; /* the live path would have ignored it too */
	}
	radiant_profile_fec_tx_command_answered(cfg.fec, page, next_sequence(),
						RADIANT_PROFILE_FEC_CMD_STATUS_FAIL, NULL);
}

void trainer_logic_report(const struct trainer_report *r)
{
	enum trainer_run prev = trainer_equip_report(cfg.equip, r);

	session(prev, true);
}

int trainer_logic_event(enum trainer_source_event evt)
{
	enum trainer_run prev = trainer_equip_event(cfg.equip, evt);
	int rc = 0;

	if (evt == TRAINER_EVT_CONSOLE_RESET) {
		/* The same release FTMS Reset runs, through the hooks. */
		rc = trainer_cmd_release_brake();
	}
	session(prev, true);
	return rc;
}

void trainer_logic_ftms_reset_done(void)
{
	enum trainer_run prev = trainer_equip_ftms_reset(cfg.equip);

	/* Quiet, as the treadmill's REMOTE_RESET: the core notifies Reset
	 * (0x01) and sets Training Status to Idle. Notifying Stopped and Idle
	 * here as well put two more notifications ahead of the 0x01 and the
	 * Control Point indication in one workqueue pass, which is the likely
	 * way the 0x01 was lost on the bench (2026-11-28, STATUS-TO-INITIATOR:
	 * a notify from the system workqueue does not wait for a buffer). */
	session(prev, false);
}
