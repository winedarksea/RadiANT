/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_equip.h - ONE EQUIPMENT STATE FOR BOTH RADIOS.
 *
 * Everything the trainer reports and everything it was told, held once: the
 * targets (whichever controller set them last, FE-C or FTMS), the session and
 * its epoch, the driver's latest report, and the accumulators. FE-C pages
 * 16/17/25 and the on-request echoes (50/51/55) are built from it, and so is
 * the FTMS Indoor Bike Data record - so the two radios can never disagree
 * about what the trainer is doing.
 *
 * KERNEL-FREE, like apps/treadmill's treadmill_state.h and apps/trainer's
 * trainer_model.h: no Zephyr header, no lock, no clock. It is told how much
 * time passed, and everything that touches it runs on the system workqueue.
 * tests/glue drives it in a counted loop.
 */
#ifndef TRAINER_EQUIP_H_
#define TRAINER_EQUIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_profile_fec.h>
#include <radiant/radiant_profile_fec_tx.h>

#include "trainer_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which target the trainer is following: the last command wins, from either
 * radio (FE-C's own rule, SS6.4: each control page selects its mode). */
enum trainer_equip_mode {
	TRAINER_EQUIP_FREE = 0, /* nothing commanded, or after a Reset */
	TRAINER_EQUIP_BASIC,    /* FE-C 48, FTMS 0x04 */
	TRAINER_EQUIP_ERG,      /* FE-C 49, FTMS 0x05 */
	TRAINER_EQUIP_SIM,      /* FE-C 50/51, FTMS 0x11 */
};

/* The session. FTMS Training Status and the FE-C state nibble both derive
 * from it. */
enum trainer_run {
	TRAINER_RUN_IDLE = 0,
	TRAINER_RUN_RUNNING,
	TRAINER_RUN_PAUSED,
	TRAINER_RUN_STOPPED,
};

struct trainer_equip {
	/* Targets. */
	enum trainer_equip_mode mode;
	uint16_t resistance_permille;
	uint16_t target_power_w;
	struct trainer_sim_params sim;
	struct trainer_user user;

	/* The session, and its epoch: bumped by every LOCAL stop or reset. */
	enum trainer_run run;
	uint32_t epoch;
	/* STOPPED only: a report with pedalling clear has been seen since the
	 * Stop, so the next pedalling report is a fresh start. */
	bool still_since_stop;

	/* The driver's latest report. */
	struct trainer_report rep;

	/* The ANT+ view's accumulators - never reset by FTMS (Phase 3 item
	 * 6's rule). Elapsed time and distance advance only while RUNNING. */
	uint32_t elapsed_ms;
	uint64_t distance_um;
	uint8_t  event_count; /* page 25 update event count */
	uint16_t acc_power_w; /* page 25 accumulated power, wraps */

	/* FTMS Reset's view: what the Indoor Bike Data record subtracts. */
	uint32_t ftms_elapsed_base_ms;
	uint64_t ftms_distance_base_um;

	/* Page 54's maximum, for page 17's resistance share. 0 = unknown. */
	uint16_t max_resistance_n;
};

void trainer_equip_init(struct trainer_equip *e, uint16_t max_resistance_n);

/* Targets back to their defaults: free ride on a flat road, default rider
 * and bike kept (the user configuration is not a target). The equipment
 * state only - trainer_cmd_release_brake() is what commands the brake. */
void trainer_equip_reset_targets(struct trainer_equip *e);

/* A report from the driver. A report with `pedalling` set starts a session
 * that is IDLE. STOPPED is left the same way, but only on a FRESH start: a
 * report with pedalling clear must have been seen since the Stop (legs still
 * turning through a Stop do not restart it, a real new ride does - and an
 * ANT+-only rider, who has no restart command, is not left FINISHED). PAUSED
 * is left only explicitly (TRAINER_EVT_STARTED, FTMS Start/Resume) or by a
 * console action. Returns the run state before it, so the caller can notify
 * a transition. */
enum trainer_run trainer_equip_report(struct trainer_equip *e,
				      const struct trainer_report *r);

/* A session event. Returns the run state before it. CONSOLE_STOP and
 * CONSOLE_RESET bump the epoch; CONSOLE_RESET also zeroes the FTMS view and
 * returns to IDLE. Neither touches the targets: the brake release that a
 * reset owes is trainer_cmd_release_brake(), run by the caller. */
enum trainer_run trainer_equip_event(struct trainer_equip *e,
				     enum trainer_source_event evt);

/* FTMS Reset succeeded: the FTMS view's elapsed time and distance go to zero
 * (the ANT+ accumulators do not move), the session returns to IDLE and the
 * EPOCH MOVES, as the treadmill's REMOTE_RESET does: a control page queued
 * before the Reset must not re-apply a target right after it. Returns the
 * run state before it. */
enum trainer_run trainer_equip_ftms_reset(struct trainer_equip *e);

/* One page 25 update: the event count and the accumulated power advance
 * together, and elapsed time and distance advance by `dt_ms` while RUNNING. */
void trainer_equip_tick(struct trainer_equip *e, uint32_t dt_ms);

/* The FE state nibble (Table 8-10) for a run state. */
uint8_t trainer_equip_fe_state(enum trainer_run run);

/* FE-C pages 16, 17 and 25. */
void trainer_equip_pages(const struct trainer_equip *e,
			 struct radiant_profile_fec_general *g,
			 struct radiant_profile_fec_settings *s,
			 struct radiant_profile_fec_trainer *t);

/* The settings in use, as pages 50, 51 and 55 echo them on request. */
void trainer_equip_echo(const struct trainer_equip *e,
			struct radiant_profile_fec_cmd_wind_resistance *w,
			struct radiant_profile_fec_cmd_track_resistance *tr,
			struct radiant_profile_fec_cmd_user_config *u);

/* ── FTMS Indoor Bike Data (0x2AD2), FTMS 1.0 SS4.9 ─────────────────────────
 *
 * Fields sent, and their flag bits (Table 4.9):
 *   bit 0 = 0  Instantaneous Speed   uint16 0.01 km/h
 *   bit 2      Instantaneous Cadence uint16 0.5 /min
 *   bit 4      Total Distance        uint24 m
 *   bit 5      Resistance Level      sint16, unitless, resolution 1
 *   bit 6      Instantaneous Power   sint16 W
 *   bit 11     Elapsed Time          uint16 s
 * and the Fitness Machine Features that go with them (Table 4.3: cadence,
 * total distance, resistance level, power measurement, elapsed time).
 */
#define TRAINER_IBD_FLAG_CADENCE    (1u << 2)
#define TRAINER_IBD_FLAG_DISTANCE   (1u << 4)
#define TRAINER_IBD_FLAG_RESISTANCE (1u << 5)
#define TRAINER_IBD_FLAG_POWER      (1u << 6)
#define TRAINER_IBD_FLAG_ELAPSED    (1u << 11)
#define TRAINER_IBD_LEN             15u

/* Encode one record into `buf` (at least TRAINER_IBD_LEN). Length, or
 * -EINVAL / -ENOSPC. */
int trainer_equip_ibd_encode(const struct trainer_equip *e, uint8_t *buf, size_t cap);

/* The resistance the trainer is applying now, per mille of maximum: the
 * commanded share in BASIC, else the reported force as a share of page 54's
 * maximum. -1 when unknown. */
int32_t trainer_equip_resistance_permille(const struct trainer_equip *e);

#ifdef __cplusplus
}
#endif

#endif /* TRAINER_EQUIP_H_ */
