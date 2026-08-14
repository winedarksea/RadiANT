/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_controls.c - see profile_controls.h.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_common.h"
#include "profile_controls.h"
#include "profile_sched.h"

/* Command numbers 5..31 and 37..32767 are reserved (table 9-8) and refused
 * rather than transmitted. */
static bool generic_command_ok(uint16_t command)
{
	if (command <= PROFILE_CONTROLS_GENERIC_HOME) {
		return true;
	}
	if (command >= PROFILE_CONTROLS_GENERIC_START &&
	    command <= PROFILE_CONTROLS_GENERIC_LAP) {
		return true;
	}
	if (command >= PROFILE_CONTROLS_GENERIC_CUSTOM_MIN &&
	    command <= PROFILE_CONTROLS_GENERIC_CUSTOM_MAX) {
		return true;
	}
	return command == PROFILE_CONTROLS_GENERIC_NO_COMMAND;
}

/* ── Page 0x10 ──────────────────────────────────────────────────────────── */

int profile_controls_encode_av(struct profile_controls *ctrl, bool av,
			       uint8_t command, uint8_t volume_percent,
			       uint8_t *out)
{
	if (ctrl == NULL || out == NULL) {
		return -EINVAL;
	}
	if (command > 0x7Fu) {
		/* Byte [7] bit 7 is the audio/video flag; the command number is
		 * the low 7 bits only. */
		return -EINVAL;
	}

	out[0] = PROFILE_CONTROLS_PAGE_AV_COMMAND;
	out[1] = (uint8_t)(ctrl->cfg.serial_number & 0xFFu);
	out[2] = (uint8_t)((ctrl->cfg.serial_number >> 8) & 0xFFu);
	out[3] = ctrl->sequence;
	out[4] = PROFILE_COMMON_INVALID_U8; /* reserved */
	out[5] = PROFILE_COMMON_INVALID_U8; /* reserved */
	out[6] = volume_percent;
	out[7] = (uint8_t)((command & 0x7Fu) | (av ? 0x80u : 0u));

	ctrl->sequence++; /* wraps at 256 by design - table 9-1's rollover field */
	return 0;
}

int profile_controls_decode_av(const uint8_t *body, bool *av,
			       uint8_t *command, uint8_t *volume_percent,
			       uint16_t *serial_number, uint8_t *sequence)
{
	if (body == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_CONTROLS_PAGE_AV_COMMAND) {
		return -EINVAL;
	}

	if (serial_number != NULL) {
		*serial_number = (uint16_t)(body[1] | ((uint16_t)body[2] << 8));
	}
	if (sequence != NULL) {
		*sequence = body[3];
	}
	if (volume_percent != NULL) {
		*volume_percent = body[6];
	}
	if (av != NULL) {
		*av = (body[7] & 0x80u) != 0u;
	}
	if (command != NULL) {
		*command = body[7] & 0x7Fu;
	}
	return 0;
}

/* ── Page 0x49 ──────────────────────────────────────────────────────────── */

int profile_controls_encode_generic(struct profile_controls *ctrl,
				    uint16_t slave_serial,
				    uint16_t slave_manufacturer_id,
				    uint16_t command, uint8_t *out)
{
	if (ctrl == NULL || out == NULL) {
		return -EINVAL;
	}
	if (!generic_command_ok(command)) {
		return -EINVAL;
	}

	out[0] = PROFILE_CONTROLS_PAGE_GENERIC;
	out[1] = (uint8_t)(slave_serial & 0xFFu);
	out[2] = (uint8_t)((slave_serial >> 8) & 0xFFu);
	out[3] = (uint8_t)(slave_manufacturer_id & 0xFFu);
	out[4] = (uint8_t)((slave_manufacturer_id >> 8) & 0xFFu);
	out[5] = ctrl->sequence;
	out[6] = (uint8_t)(command & 0xFFu);
	out[7] = (uint8_t)((command >> 8) & 0xFFu);

	/* "No Command" does not advance the sequence, so a receiver can tell a
	 * padding packet from a new command by the counter alone. */
	if (command != PROFILE_CONTROLS_GENERIC_NO_COMMAND) {
		ctrl->sequence++;
	}
	return 0;
}

int profile_controls_decode_generic(const uint8_t *body, uint16_t *slave_serial,
				    uint16_t *slave_manufacturer_id,
				    uint16_t *command, uint8_t *sequence)
{
	if (body == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_CONTROLS_PAGE_GENERIC) {
		return -EINVAL;
	}

	if (slave_serial != NULL) {
		*slave_serial = (uint16_t)(body[1] | ((uint16_t)body[2] << 8));
	}
	if (slave_manufacturer_id != NULL) {
		*slave_manufacturer_id = (uint16_t)(body[3] |
						    ((uint16_t)body[4] << 8));
	}
	if (sequence != NULL) {
		*sequence = body[5];
	}
	if (command != NULL) {
		*command = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	}
	return 0;
}

/* ── The master ─────────────────────────────────────────────────────────── */

static int controls_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			      void *user)
{
	struct profile_controls *ctrl = (struct profile_controls *)user;

	(void)page;
	(void)counter;

	if (ctrl == NULL) {
		return -EINVAL;
	}
	if (!ctrl->pending) {
		/* No periodic main page on this device type, so an empty slot
		 * goes idle rather than repeating stale content. */
		return -EAGAIN;
	}

	ctrl->pending = false;
	if (ctrl->pending_generic) {
		return profile_controls_encode_generic(ctrl, ctrl->g_serial,
						       ctrl->g_manufacturer,
						       ctrl->g_command, body);
	}
	return profile_controls_encode_av(ctrl, ctrl->av_flag, ctrl->av_command,
					  ctrl->av_volume, body);
}

static int controls_common_80(uint8_t *body, void *user)
{
	struct profile_controls *ctrl = (struct profile_controls *)user;

	if (ctrl == NULL) {
		return -EINVAL;
	}
	return profile_common_80(&ctrl->cfg.id, body);
}

static int controls_common_81(uint8_t *body, void *user)
{
	struct profile_controls *ctrl = (struct profile_controls *)user;

	if (ctrl == NULL) {
		return -EINVAL;
	}
	return profile_common_81(&ctrl->cfg.id, body);
}

static int controls_common_82(uint8_t *body, void *user)
{
	struct profile_controls *ctrl = (struct profile_controls *)user;

	if (ctrl == NULL) {
		return -EINVAL;
	}
	return profile_common_82(&ctrl->cfg.battery, body);
}

int profile_controls_init(struct profile_controls *ctrl,
			  const struct profile_controls_cfg *cfg)
{
	struct profile_sched_cfg sched_cfg;

	if (ctrl == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->cfg = *cfg;
	ctrl->rotation[0] = PROFILE_CONTROLS_PAGE_AV_COMMAND;

	memset(&sched_cfg, 0, sizeof(sched_cfg));
	sched_cfg.desc = NULL;
	sched_cfg.pages = ctrl->rotation;
	sched_cfg.n_pages = (uint8_t)sizeof(ctrl->rotation);
	sched_cfg.data_page = controls_data_page;
	sched_cfg.common_80 = controls_common_80;
	sched_cfg.common_81 = controls_common_81;
	sched_cfg.common_82 = (cfg->common_82_every != 0u) ? controls_common_82
							    : NULL;
	sched_cfg.common_82_every = cfg->common_82_every;
	sched_cfg.user = ctrl;

	return profile_sched_init(&ctrl->sched, &sched_cfg);
}

int profile_controls_send_av(struct profile_controls *ctrl, bool av,
			     uint8_t command, uint8_t volume_percent)
{
	if (ctrl == NULL) {
		return -EINVAL;
	}
	if (ctrl->pending) {
		return -EBUSY;
	}
	ctrl->pending = true;
	ctrl->pending_generic = false;
	ctrl->av_flag = av;
	ctrl->av_command = command;
	ctrl->av_volume = volume_percent;
	return 0;
}

int profile_controls_send_generic(struct profile_controls *ctrl,
				  uint16_t slave_serial,
				  uint16_t slave_manufacturer_id,
				  uint16_t command)
{
	if (ctrl == NULL) {
		return -EINVAL;
	}
	if (!generic_command_ok(command)) {
		return -EINVAL;
	}
	if (ctrl->pending) {
		return -EBUSY;
	}
	ctrl->pending = true;
	ctrl->pending_generic = true;
	ctrl->g_serial = slave_serial;
	ctrl->g_manufacturer = slave_manufacturer_id;
	ctrl->g_command = command;
	return 0;
}

enum profile_slot_kind profile_controls_next(struct profile_controls *ctrl,
					     uint8_t *body)
{
	if (ctrl == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}
	return profile_sched_next(&ctrl->sched, body);
}
