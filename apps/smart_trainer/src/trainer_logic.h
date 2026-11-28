/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_logic.h - the node's decisions, kernel-free: what main.c does with
 * an FE-C control page, a dropped one, a driver report or event, and a
 * successful FTMS Reset. main.c keeps only the threads, the radio and the
 * BLE glue; tests/glue drives this file against the real FE-C master.
 *
 * Everything here runs on the system workqueue.
 */
#ifndef TRAINER_LOGIC_H_
#define TRAINER_LOGIC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_profile_fec_tx.h>

#include "trainer_equip.h"
#include "trainer_source.h"

#ifdef __cplusplus
extern "C" {
#endif

struct trainer_logic_cfg {
	struct trainer_equip *equip;
	struct radiant_profile_fec_tx *fec;
	/* The session moved (or only its epoch did: prev == next). `notify`
	 * false for an FTMS Reset, whose Reset (0x01) status and Idle
	 * Training Status the FTMS core sends itself. */
	void (*session_changed)(enum trainer_run prev, enum trainer_run next, bool notify,
				void *ctx);
	/* A Fitness Machine Status an FE-C change owes BLE subscribers. */
	void (*ftms_status)(const uint8_t *msg, size_t len, void *ctx);
	void *ctx;
};

void trainer_logic_init(const struct trainer_logic_cfg *cfg);

/*
 * One FE-C page received under `rx_epoch`:
 *   70           radiant_profile_fec_tx_handle_request(); no page 71
 *   48..51, 55   trainer_cmd_fec(); a page 71 and the sequence number only
 *                when it owes one (a supported control page)
 *   anything else ignored
 */
void trainer_logic_fec_page(const uint8_t *body, uint32_t rx_epoch, uint32_t now_ms);

/* A control page the receive ring had no room for. Answered FAIL - and the
 * sequence advanced - only for a page the trainer supports, as the live path
 * would have answered it; anything else is ignored as the live path would. */
void trainer_logic_fec_dropped(uint8_t page);

/* Page 71's sequence number now (FE-C SS8.12.3.2: 255 until the first
 * supported command, then counted per supported command, 0..254). */
uint8_t trainer_logic_sequence(void);

/* A driver report and event (pairing and bond events excluded: main.c's). A
 * console reset also releases the brake (trainer_cmd_release_brake());
 * returns that result, else 0. */
void trainer_logic_report(const struct trainer_report *r);
int trainer_logic_event(enum trainer_source_event evt);

/* FTMS Reset succeeded (the core's reset_done): the FTMS view zeroed, the
 * session to IDLE, the epoch moved - quietly towards FTMS. */
void trainer_logic_ftms_reset_done(void);

#ifdef __cplusplus
}
#endif

#endif /* TRAINER_LOGIC_H_ */
