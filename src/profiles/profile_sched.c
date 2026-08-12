/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_sched.c - which page goes in the next message slot.
 *
 * Provenance: docs/radiant-telemetry.md section 6 (the 119/120/121 interleave
 * and the consecutive descriptor set) and section 8 (sparse mode: heartbeat,
 * repeat count, and "the node emits the whole descriptor set with every
 * heartbeat"). That document is this project's own written specification. No
 * adopter-gated ANT+ device profile document was read for this file, no
 * sdk-ant source was consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * The priority order in next() is the whole file, and it is worth reading as
 * an ordered list rather than as control flow:
 *
 *   1. message 119 is page 80, message 120 is page 81. Nothing displaces
 *      them, because they are the cadence rule a certified receiver pairs on.
 *   2. a descriptor burst in progress takes the slot. Consecutive is the
 *      point; a burst interrupted by a data page costs a joining receiver a
 *      whole cycle.
 *   3. the client seam gets first refusal.
 *   4. page 82, if the node emits one and its cadence is due.
 *   5. the data-page rotation.
 *
 * Sparse mode replaces 1 and 5 rather than modifying them: there is no 121
 * cycle to hang a common page on, and a slot with nothing to say is silent.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_sched.h"
#include "profile_telemetry.h"

/* The ANT timebase: a channel period is in counts of 1/32768 s. This is the
 * one number from the link layer that a page scheduler legitimately needs -
 * it converts a heartbeat stated in seconds into a count of message slots,
 * and there is no other way to express section 8's heartbeat on a slot
 * grid. */
#define ANT_TICKS_PER_S 32768u

int profile_sched_init(struct profile_sched *ps,
		       const struct profile_sched_cfg *cfg)
{
	int rc;

	if (ps == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(ps, 0, sizeof(*ps));
	ps->cfg = *cfg;
	ps->sched_frame = -1;

	if (cfg->desc != NULL) {
		rc = profile_desc_encode(cfg->desc, ps->frames,
					 PROFILE_TLM_MAX_FRAMES);
		if (rc < 0) {
			return rc;
		}
		ps->n_frames = (uint8_t)rc;

		rc = profile_desc_data_pages(cfg->desc, ps->pages,
					     (uint8_t)sizeof(ps->pages));
		if (rc < 0) {
			return rc;
		}
		ps->n_pages = (uint8_t)rc;

		/* Latched once. -ENOENT here is the ordinary case - most nodes
		 * announce no schedule block at all - and is not an error. */
		rc = profile_desc_schedule_index(cfg->desc);
		ps->sched_frame = (rc < 0) ? -1 : (int8_t)rc;

		ps->sparse = (cfg->desc->flags & PROFILE_TLM_FLAG_SPARSE) != 0u;
		if (ps->sparse && cfg->desc->period != 0u) {
			/* Slot-aligned sparse: the node keeps its period
			 * configured, so the transmissions it does make land
			 * on a predictable phase and a scan receiver's dwell
			 * can be aligned to them. */
			uint32_t slots = ((uint32_t)cfg->desc->heartbeat_s *
					  ANT_TICKS_PER_S) / cfg->desc->period;

			ps->hb_slots = (slots == 0u) ? 1u : slots;
		}
		/* Asynchronous sparse leaves hb_slots at 0: there is no grid,
		 * so the node's own timer calls profile_sched_post_heartbeat().
		 */
	} else if (cfg->pages != NULL && cfg->n_pages != 0u) {
		/*
		 * A family with no descriptor states its rotation outright.
		 * Copied rather than referenced: the rest of this engine reads
		 * ps->pages, and a caller whose array went out of scope would
		 * transmit whatever replaced it for as long as the node ran.
		 */
		if (cfg->n_pages > (uint8_t)sizeof(ps->pages)) {
			return -ENOSPC;
		}
		memcpy(ps->pages, cfg->pages, cfg->n_pages);
		ps->n_pages = cfg->n_pages;
	}

	/*
	 * The first thing a node transmits is its schema. A receiver that was
	 * already listening when the node powered up should not have to wait
	 * for message 0 of the second cycle.
	 *
	 * A sparse node expresses the same thing as a pending heartbeat rather
	 * than as a burst, and the difference is not cosmetic: the heartbeat
	 * is what resets the interval, so starting the burst directly would
	 * leave the pending flag set and fire a second heartbeat two slots
	 * later.
	 */
	if (ps->sparse) {
		ps->hb_pending = true;
	} else if (ps->n_frames != 0u) {
		ps->burst = true;
		ps->burst_index = 0u;
	}

	return 0;
}

int profile_sched_set_client(struct profile_sched *ps,
			     const struct profile_sched_client *client)
{
	if (ps == NULL) {
		return -EINVAL;
	}
	if (client == NULL) {
		ps->have_client = false;
		memset(&ps->client, 0, sizeof(ps->client));
		return 0;
	}
	if (client->claim == NULL) {
		return -EINVAL;
	}
	ps->client = *client;
	ps->have_client = true;
	return 0;
}

int profile_sched_set_downlink(struct profile_sched *ps,
			       const struct profile_sched_downlink *dl)
{
	if (ps == NULL) {
		return -EINVAL;
	}
	if (dl == NULL) {
		ps->have_dl = false;
		memset(&ps->dl, 0, sizeof(ps->dl));
		return 0;
	}
	if (dl->phase == NULL) {
		return -EINVAL;
	}
	if (ps->sched_frame < 0) {
		/* See the header: a hook with no schedule frame under it. */
		return -ENOENT;
	}
	ps->dl = *dl;
	ps->have_dl = true;
	return 0;
}

void profile_sched_request_descriptor(struct profile_sched *ps)
{
	if (ps == NULL || ps->n_frames == 0u) {
		return;
	}
	ps->burst = true;
	ps->burst_index = 0u;
}

int profile_sched_post_event(struct profile_sched *ps, uint8_t page)
{
	uint8_t k;

	if (ps == NULL) {
		return -EINVAL;
	}
	if (!ps->sparse || ps->cfg.desc == NULL) {
		return -ENOTSUP;
	}
	if (page < PROFILE_TLM_PAGE_DATA_FIRST ||
	    page > PROFILE_TLM_PAGE_DATA_LAST) {
		return -EINVAL;
	}

	k = profile_tlm_repeat_for(ps->cfg.desc->k_code);
	if (k == 0u) {
		return -EINVAL;
	}

	ps->event_page = page;
	ps->event_left = k;
	return 0;
}

void profile_sched_post_heartbeat(struct profile_sched *ps)
{
	if (ps != NULL) {
		ps->hb_pending = true;
	}
}

uint8_t profile_sched_counter(const struct profile_sched *ps)
{
	return (ps == NULL) ? 0u : ps->counter;
}

uint32_t profile_sched_m(const struct profile_sched *ps)
{
	return (ps == NULL) ? 0u : ps->m;
}

/* ---------------------------------------------------------------------------
 * The pieces of next()
 * ---------------------------------------------------------------------------
 */

/*
 * Hand out one frame of the encoded set.
 *
 * The re-phase is the only thing in this engine that edits an encoded byte on
 * its way out, and it is confined to one field of one frame: the caller's copy,
 * never ps->frames[], so a hook that returns a different answer next time
 * cannot accumulate. That is why the descriptor set is still encoded exactly
 * once at init, which was the property the interval-0 restriction was protecting
 * and the reason this is a patch rather than a re-encode.
 */
static enum profile_slot_kind take_descriptor(struct profile_sched *ps,
					      uint8_t *body, radiant_time_t t_sync)
{
	uint8_t index = ps->burst_index;

	memcpy(body, &ps->frames[(size_t)index * PROFILE_TLM_FRAME_LEN],
	       PROFILE_TLM_FRAME_LEN);

	if (ps->have_dl && t_sync != RADIANT_TIME_NEVER &&
	    ps->sched_frame >= 0 && index == (uint8_t)ps->sched_frame) {
		int32_t phase = ps->dl.phase(t_sync, ps->dl.user);

		if (phase >= 0) {
			/* -ENOENT (this block announces no window) and -EINVAL
			 * (a phase past the interval) are both a hook
			 * disagreeing with the descriptor it was installed
			 * against. The encoded bytes are the ones that were
			 * validated, so they go out unchanged and the node
			 * announces the phase it was built with rather than one
			 * nobody checked. */
			(void)profile_sched_rephase(&body[2], (uint16_t)phase);
		}
	}

	ps->burst_index++;
	if (ps->burst_index >= ps->n_frames) {
		ps->burst = false;
		ps->burst_index = 0u;
	}
	return PROFILE_SLOT_DESCRIPTOR;
}

/*
 * Build one data page from the rotation.
 *
 * The counter is advanced only when the builder actually produced a body: it
 * counts TRANSMISSIONS, and a slot the application declined is not one.
 */
static enum profile_slot_kind take_data(struct profile_sched *ps, uint8_t page,
					uint8_t *body)
{
	if (ps->cfg.data_page == NULL) {
		return PROFILE_SLOT_IDLE;
	}
	if (ps->cfg.data_page(page, ps->counter, body, ps->cfg.user) != 0) {
		return PROFILE_SLOT_IDLE;
	}
	ps->counter++;
	return PROFILE_SLOT_DATA;
}

static enum profile_slot_kind take_rotation(struct profile_sched *ps,
					    uint8_t *body)
{
	uint8_t page;

	if (ps->n_pages == 0u) {
		/* A node with no fields at all. That is a legal descriptor -
		 * an asset tag is the envelope with everything turned off -
		 * and it has nothing to rotate. */
		return PROFILE_SLOT_IDLE;
	}

	page = ps->pages[ps->page_cursor];
	ps->page_cursor = (uint8_t)((ps->page_cursor + 1u) % ps->n_pages);
	return take_data(ps, page, body);
}

/* Page 82 rides any data slot at whatever cadence the node likes; it is not
 * announced in the descriptor because it is a page every ANT+ receiver already
 * understands. */
static bool page_82_due(struct profile_sched *ps)
{
	if (ps->cfg.common_82 == NULL || ps->cfg.common_82_every == 0u) {
		return false;
	}
	ps->data_since_82++;
	if (ps->data_since_82 < ps->cfg.common_82_every) {
		return false;
	}
	ps->data_since_82 = 0u;
	return true;
}

static enum profile_slot_kind next_periodic(struct profile_sched *ps,
					    uint32_t m, uint8_t *body,
					    radiant_time_t t_sync)
{
	if (m == PROFILE_TLM_SLOT_PAGE_80 && ps->cfg.common_80 != NULL) {
		if (ps->cfg.common_80(body, ps->cfg.user) == 0) {
			return PROFILE_SLOT_COMMON_80;
		}
	}
	if (m == PROFILE_TLM_SLOT_PAGE_81 && ps->cfg.common_81 != NULL) {
		if (ps->cfg.common_81(body, ps->cfg.user) == 0) {
			return PROFILE_SLOT_COMMON_81;
		}
	}

	/* The set restarts at the top of every cycle. */
	if (m == 0u && ps->n_frames != 0u) {
		ps->burst = true;
		ps->burst_index = 0u;
	}
	if (ps->burst) {
		return take_descriptor(ps, body, t_sync);
	}

	if (ps->have_client && ps->client.claim(m, body, ps->client.user)) {
		return PROFILE_SLOT_CLIENT;
	}

	if (page_82_due(ps)) {
		if (ps->cfg.common_82(body, ps->cfg.user) == 0) {
			return PROFILE_SLOT_COMMON_82;
		}
	}

	return take_rotation(ps, body);
}

static enum profile_slot_kind next_sparse(struct profile_sched *ps, uint32_t m,
					  uint8_t *body, radiant_time_t t_sync)
{
	/*
	 * The 121-message interleave is useless to a node that sends forty
	 * messages a day, so section 8 replaces it: the whole descriptor set
	 * rides every heartbeat, and nothing else is periodic.
	 */
	if (!ps->burst) {
		ps->since_hb++;
		if (ps->hb_pending ||
		    (ps->hb_slots != 0u && ps->since_hb >= ps->hb_slots)) {
			ps->hb_pending = false;
			ps->since_hb = 0u;
			if (ps->n_frames != 0u) {
				ps->burst = true;
				ps->burst_index = 0u;
			}
			ps->want_82 = ps->cfg.common_82 != NULL;
		}
	}

	if (ps->burst) {
		return take_descriptor(ps, body, t_sync);
	}

	if (ps->want_82) {
		ps->want_82 = false;
		if (ps->cfg.common_82(body, ps->cfg.user) == 0) {
			return PROFILE_SLOT_COMMON_82;
		}
	}

	if (ps->event_left != 0u) {
		enum profile_slot_kind kind = take_data(ps, ps->event_page, body);

		ps->event_left--;
		return kind;
	}

	/*
	 * Nothing to say. The client seam is offered the silence rather than a
	 * data slot, because on a sparse node silence is where the free slots
	 * are - and a client that wants one has to say so, which is the whole
	 * of the contract.
	 */
	if (ps->have_client && ps->client.claim(m, body, ps->client.user)) {
		return PROFILE_SLOT_CLIENT;
	}

	return PROFILE_SLOT_IDLE;
}

enum profile_slot_kind profile_sched_next_at(struct profile_sched *ps,
					     uint8_t *body, radiant_time_t t_sync)
{
	uint32_t m;

	if (ps == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}

	m = ps->m;
	if (ps->sparse) {
		ps->m++;
		return next_sparse(ps, m, body, t_sync);
	}

	ps->m = (m + 1u) % PROFILE_TLM_CYCLE;
	return next_periodic(ps, m, body, t_sync);
}

enum profile_slot_kind profile_sched_next(struct profile_sched *ps, uint8_t *body)
{
	return profile_sched_next_at(ps, body, RADIANT_TIME_NEVER);
}
