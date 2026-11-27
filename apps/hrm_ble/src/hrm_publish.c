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

/*
 * The 1/1024 s event times wrap every 65 536 ms, so an interval of 64 s + x
 * reads as x. The delivery times (ms, not wrapping) bound the true interval
 * from above, give or take the baseline beat's own delivery latency: while
 * they are less than 64 000 - stale ms apart, a true interval that wrapped
 * into the accepted range (<= stale) is impossible, with 1.5 s to spare for
 * that latency. At or beyond it the interval is ambiguous, and treated as a
 * gap. NOT the stale limit itself: beats coalesced by a workqueue stall are
 * delivered late together, and their genuine intervals must survive.
 */
static uint32_t wrap_guard_ms(const struct hrm_publish *p)
{
	return 64000u - p->stale_ms;
}

static void refresh(struct hrm_publish *p, uint32_t now_ms)
{
	p->deadline_ms = now_ms + p->stale_ms;
	p->armed = true;
}

/* A gap - contact loss or expiry. No interval spans it, the recent intervals
 * no longer describe the heart, and the old reported rate is not revived. */
static void gap(struct hrm_publish *p)
{
	p->have_baseline = false;
	p->recent_count = 0u;
	p->recent_next = 0u;
	p->rate_fresh = false;
}

static void rr_push(struct hrm_publish *p, uint16_t rr)
{
	if (p->rr_count == HRM_PUBLISH_RR_MAX) {
		/* Full: the oldest goes - the one legal loss. */
		memmove(&p->rr[0], &p->rr[1], (HRM_PUBLISH_RR_MAX - 1u) * sizeof(p->rr[0]));
		p->rr_count--;
		p->rr_skipped++;
	}
	p->rr[p->rr_count++] = rr;

	p->recent[p->recent_next] = rr;
	p->recent_next = (uint8_t)((p->recent_next + 1u) % HRM_PUBLISH_DERIVE_N);
	if (p->recent_count < HRM_PUBLISH_DERIVE_N) {
		p->recent_count++;
	}
}

/* 60 * 1024 / mean(recent), rounded; 0 with no interval yet. */
static uint8_t derived_bpm(const struct hrm_publish *p)
{
	uint32_t sum = 0u;
	uint32_t bpm;

	for (uint8_t i = 0u; i < p->recent_count; i++) {
		sum += p->recent[i];
	}
	if (sum == 0u) {
		return 0u;
	}
	bpm = (60u * 1024u * p->recent_count + sum / 2u) / sum;
	if (bpm > 0xFFu) {
		bpm = 0xFFu; /* 0xFF is a real 255 bpm on this profile */
	}
	return (bpm == 0u) ? 1u : (uint8_t)bpm;
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
	if (!p->contact) {
		/* Off the chest there is no rate; a report now would otherwise
		 * be "fresh" the moment contact returned. */
		return;
	}
	p->bpm = bpm;
	if (bpm == 0u) {
		p->armed = false;
		p->rate_fresh = false;
		return;
	}
	p->rate_fresh = true;
	refresh(p, now_ms);
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
		/* A beat without contact - noise off the skin, as far as anyone
		 * can tell - counts on ANT+ (the driver says it happened) and
		 * does nothing else: no interval, no baseline, no refresh. */
		p->have_baseline = false;
		return n;
	}

	refresh(p, now_ms);

	if (n == 1u && p->have_baseline) {
		iv = (uint16_t)(event_time - p->baseline_time);
		/* Both clocks: the 1/1024 s difference wraps every 65.536 s,
		 * so a 66 s pause (kept alive by rate reports) would read as
		 * a plausible 2 s interval. The delivery times do not wrap;
		 * see wrap_guard_ms(). */
		if (iv != 0u && iv <= rr_limit_1024(p) &&
		    (uint32_t)(now_ms - p->baseline_ms) < wrap_guard_ms(p)) {
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
	p->baseline_ms = now_ms;
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
		gap(p);
		hrm_publish_rr_clear(p);
	}
	p->contact = detected;
}

uint8_t hrm_publish_bpm(const struct hrm_publish *p, uint32_t now_ms)
{
	if (!p->contact || !p->armed || reached(now_ms, p->deadline_ms)) {
		return 0u;
	}
	return p->rate_fresh ? p->bpm : derived_bpm(p);
}

bool hrm_publish_tick(struct hrm_publish *p, uint32_t now_ms)
{
	if (p->armed && reached(now_ms, p->deadline_ms)) {
		p->armed = false;
		gap(p);
		return true;
	}
	return false;
}

bool hrm_publish_ble_step(struct hrm_publish *p, uint32_t now_ms, bool subscribed,
			  bool disconnected, struct radiant_ble_hrs_meas *m)
{
	if (disconnected) {
		hrm_publish_rr_clear(p);
	}
	if (!subscribed) {
		hrm_publish_rr_clear(p);
		p->notifying = false;
		return false;
	}
	if (!p->notifying) {
		p->notifying = true;
		p->next_notify_ms = now_ms + HRM_PUBLISH_NOTIFY_MS;
	} else if (!reached(now_ms, p->next_notify_ms)) {
		return false;
	} else {
		/* Accumulate the period rather than re-anchor to the tick, so
		 * the average stays 1 Hz on a 100 ms tick (the hrm_sim_sched
		 * lesson); re-anchor only after falling a whole period behind. */
		p->next_notify_ms += HRM_PUBLISH_NOTIFY_MS;
		if (reached(now_ms, p->next_notify_ms)) {
			p->next_notify_ms = now_ms + HRM_PUBLISH_NOTIFY_MS;
		}
	}
	hrm_publish_meas(p, now_ms, m);
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

void hrm_publish_rr_consume(struct hrm_publish *p, size_t n)
{
	if (n >= p->rr_count) {
		p->rr_count = 0u;
		return;
	}
	memmove(&p->rr[0], &p->rr[n], (p->rr_count - n) * sizeof(p->rr[0]));
	p->rr_count = (uint8_t)(p->rr_count - n);
}

void hrm_publish_rr_clear(struct hrm_publish *p)
{
	p->rr_count = 0u;
}
