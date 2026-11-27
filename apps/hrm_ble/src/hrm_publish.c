/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The snapshot both radios read. Pure: no kernel, no radio. See hrm_publish.h
 * for the rules; apps/hrm_ble/tests/publish states each one as a test.
 */

#include <string.h>

#include "hrm_publish.h"

/* Modular "now has reached t", valid for differences under 2^31 ms. */
static bool reached(uint32_t now_ms, uint32_t t_ms)
{
	return (int32_t)(now_ms - t_ms) >= 0;
}

/* The longest interval reported as an RR-Interval, in 1/1024 s: the stale
 * limit. Anything longer is a gap, whatever the clock says. */
static uint32_t rr_limit_1024(const struct hrm_publish *p)
{
	uint32_t lim = (uint32_t)(((uint64_t)p->stale_ms * 1024u) / 1000u);

	return (lim > 0xFFFFu) ? 0xFFFFu : lim;
}

static void refresh(struct hrm_publish *p, uint32_t now_ms)
{
	p->deadline_ms = now_ms + p->stale_ms;
	p->armed = true;
}

static void rr_push(struct hrm_publish *p, uint16_t rr)
{
	if (p->rr_count == HRM_PUBLISH_RR_MAX) {
		/* Oldest first out, as HRS drops them. */
		memmove(&p->rr[0], &p->rr[1], (HRM_PUBLISH_RR_MAX - 1u) * sizeof(p->rr[0]));
		p->rr_count--;
		p->rr_skipped++;
	}
	p->rr[p->rr_count++] = rr;
}

void hrm_publish_init(struct hrm_publish *p, bool contact_supported, uint32_t stale_ms)
{
	memset(p, 0, sizeof(*p));
	p->stale_ms = stale_ms;
	p->contact_supported = contact_supported;
	/* Start "detected". A sensor without contact support is always taken
	 * to have contact; one with it is believed until its first
	 * hrm_sensor_report_contact(false), so a driver that supports contact
	 * but reports it only on change still publishes from its first beat. */
	p->contact = true;
}

void hrm_publish_rate(struct hrm_publish *p, uint8_t bpm, uint32_t now_ms)
{
	p->bpm = bpm;
	if (bpm == 0u) {
		p->armed = false;
		return;
	}
	/* Without contact the value is kept but not made valid: after
	 * contact returns, a fresh rate or beat is what revives it. */
	if (p->contact) {
		refresh(p, now_ms);
	}
}

uint16_t hrm_publish_beat(struct hrm_publish *p, uint16_t seq, uint16_t event_time,
			  uint32_t now_ms)
{
	uint16_t n = (uint16_t)(seq - p->last_seq);
	uint16_t iv;

	/* Already accounted for (0), or older than the newest (the upper
	 * half of the modular range). */
	if (n == 0u || n >= 0x8000u) {
		return 0u;
	}
	p->last_seq = seq;
	p->beats += n;

	if (!p->contact) {
		/* A beat without contact counts on ANT+ (the driver says it
		 * happened) but starts no interval and refreshes no rate. */
		p->have_baseline = false;
		return n;
	}

	refresh(p, now_ms);

	if (n == 1u && p->have_baseline) {
		iv = (uint16_t)(event_time - p->baseline_time);
		if (iv != 0u && iv <= rr_limit_1024(p)) {
			rr_push(p, iv);
		} else {
			p->rr_skipped++;
		}
	} else if (n > 1u) {
		/* Beats the ring could not hold: their intervals are unknown,
		 * so none is reported, including this one. */
		p->rr_skipped += n;
	}

	p->baseline_time = event_time;
	p->have_baseline = true;
	return n;
}

void hrm_publish_contact(struct hrm_publish *p, bool detected, uint32_t now_ms)
{
	(void)now_ms;

	if (!p->contact_supported) {
		return;
	}
	if (!detected) {
		p->armed = false;
		p->have_baseline = false;
		hrm_publish_rr_clear(p);
	}
	p->contact = detected;
}

uint8_t hrm_publish_bpm(const struct hrm_publish *p, uint32_t now_ms)
{
	if (!p->contact || !p->armed || reached(now_ms, p->deadline_ms)) {
		return 0u;
	}
	return p->bpm;
}

bool hrm_publish_tick(struct hrm_publish *p, uint32_t now_ms)
{
	if (p->armed && reached(now_ms, p->deadline_ms)) {
		p->armed = false;
		/* Silence long enough to expire the rate is a gap: no interval
		 * spans it. */
		p->have_baseline = false;
		return true;
	}
	return false;
}

bool hrm_publish_notify_due(struct hrm_publish *p, uint32_t now_ms, bool subscribed)
{
	if (!subscribed) {
		hrm_publish_rr_clear(p);
		p->notifying = false;
		return false;
	}
	if (!p->notifying) {
		p->notifying = true;
		p->next_notify_ms = now_ms + HRM_PUBLISH_NOTIFY_MS;
		return true;
	}
	if (!reached(now_ms, p->next_notify_ms)) {
		return false;
	}
	/* Accumulate the period rather than re-anchor to the tick, so the
	 * average stays 1 Hz on a 100 ms tick (the hrm_sim_sched lesson);
	 * re-anchor only after falling a whole period behind. */
	p->next_notify_ms += HRM_PUBLISH_NOTIFY_MS;
	if (reached(now_ms, p->next_notify_ms)) {
		p->next_notify_ms = now_ms + HRM_PUBLISH_NOTIFY_MS;
	}
	return true;
}

void hrm_publish_meas(const struct hrm_publish *p, uint32_t now_ms,
		      struct radiant_ble_hrs_meas *m)
{
	m->bpm = hrm_publish_bpm(p, now_ms);
	m->contact_supported = p->contact_supported;
	m->contact_detected = p->contact_supported && p->contact;
	m->rr = p->rr;
	m->rr_count = p->rr_count;
}

void hrm_publish_rr_clear(struct hrm_publish *p)
{
	p->rr_count = 0u;
}
