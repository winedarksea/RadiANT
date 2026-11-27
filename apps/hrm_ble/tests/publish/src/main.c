/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The heart-rate node's publish path, one rule per case.
 *
 * Suite hrm_hrs_encode - radiant_ble_hrs_encode(), the pure HRS 1.0 Heart Rate
 * Measurement encoder: flags (format, contact support vs status, never Energy
 * Expended), RR-Interval packing, and the ATT MTU cap that keeps the newest
 * intervals.
 *
 * Suite hrm_publish - the beat ring (src/hrm_beat_ring.c) feeding the snapshot
 * (src/hrm_publish.c) that drives both radios: N coalesced beats yield N
 * intervals, an overflowing ring drops intervals and never mis-states one, a
 * rate-only driver still notifies, a repeated rate stays fresh while silence
 * expires it on both radios, contact loss clears RR and invalidates the rate
 * without touching the support bit, and a disconnect clears RR.
 *
 * Time is a variable: `now` in ms, and a beat's event time is now * 1024 / 1000
 * as hrm_sensor_now_1024() computes it.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_ble_hrs.h>
#include "hrm_beat_ring.h"
#include "hrm_publish.h"

#define STALE_MS 3000u

static uint16_t t1024(uint32_t ms)
{
	return (uint16_t)(((uint64_t)ms * 1024u) / 1000u);
}

/* ══ radiant_ble_hrs_encode ════════════════════════════════════════════════ */

ZTEST_SUITE(hrm_hrs_encode, NULL, NULL, NULL, NULL, NULL);

ZTEST(hrm_hrs_encode, test_contact_support_vs_status)
{
	struct radiant_ble_hrs_meas m = { .bpm = 72 };
	uint8_t buf[20];

	/* Not supported: both bits 0, whatever "detected" says. */
	m.contact_supported = false;
	m.contact_detected = true;
	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 2);
	zassert_equal(buf[0], 0x00);
	zassert_equal(buf[1], 72);

	/* Supported, detected: bits 2 and 1. */
	m.contact_supported = true;
	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 2);
	zassert_equal(buf[0], RADIANT_BLE_HRS_FLAG_CONTACT_SUPP | RADIANT_BLE_HRS_FLAG_CONTACT);

	/* Supported, not detected: bit 2 only. */
	m.contact_detected = false;
	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 2);
	zassert_equal(buf[0], RADIANT_BLE_HRS_FLAG_CONTACT_SUPP);
}

ZTEST(hrm_hrs_encode, test_rate_format)
{
	struct radiant_ble_hrs_meas m = { .bpm = 255 };
	uint8_t buf[20];

	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 2);
	zassert_equal(buf[0] & RADIANT_BLE_HRS_FLAG_UINT16, 0);
	zassert_equal(buf[1], 255);

	m.bpm = 0x0123;
	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 3);
	zassert_equal(buf[0] & RADIANT_BLE_HRS_FLAG_UINT16, RADIANT_BLE_HRS_FLAG_UINT16);
	zassert_equal(buf[1], 0x23);
	zassert_equal(buf[2], 0x01);

	/* "No reading" is a rate of 0, UINT8. */
	m.bpm = 0;
	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 2);
	zassert_equal(buf[1], 0);
}

ZTEST(hrm_hrs_encode, test_rr_packing)
{
	const uint16_t rr[] = { 0x0355, 0x0401, 0x03FF };
	struct radiant_ble_hrs_meas m = { .bpm = 72, .rr = rr, .rr_count = 3 };
	uint8_t buf[20];
	size_t written;

	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), &written), 8);
	zassert_equal(written, 3);
	zassert_equal(buf[0], RADIANT_BLE_HRS_FLAG_RR);
	/* Little-endian, oldest first. */
	zassert_equal(buf[2], 0x55);
	zassert_equal(buf[3], 0x03);
	zassert_equal(buf[4], 0x01);
	zassert_equal(buf[5], 0x04);
	zassert_equal(buf[6], 0xFF);
	zassert_equal(buf[7], 0x03);

	/* No intervals: no RR flag. */
	m.rr_count = 0;
	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), &written), 2);
	zassert_equal(written, 0);
	zassert_equal(buf[0] & RADIANT_BLE_HRS_FLAG_RR, 0);
}

ZTEST(hrm_hrs_encode, test_mtu_cap_keeps_newest)
{
	uint16_t rr[12];
	struct radiant_ble_hrs_meas m = { .bpm = 72, .rr = rr, .rr_count = 12 };
	uint8_t buf[RADIANT_BLE_HRS_DEFAULT_PAYLOAD];
	size_t written;
	int len;

	for (unsigned int i = 0; i < 12; i++) {
		rr[i] = (uint16_t)(1000 + i);
	}

	/* 20 bytes: flags + UINT8 rate + 9 intervals, the newest 9. */
	len = radiant_ble_hrs_encode(&m, buf, sizeof(buf), &written);
	zassert_equal(len, 20);
	zassert_equal(written, 9);
	for (unsigned int i = 0; i < 9; i++) {
		uint16_t v = (uint16_t)(buf[2 + 2 * i] | (buf[3 + 2 * i] << 8));

		zassert_equal(v, rr[3 + i], "slot %u", i);
	}

	/* UINT16 rate costs one interval. */
	m.bpm = 300;
	len = radiant_ble_hrs_encode(&m, buf, sizeof(buf), &written);
	zassert_equal(len, 19);
	zassert_equal(written, 8);
	zassert_equal((uint16_t)(buf[3] | (buf[4] << 8)), rr[4]);

	/* Room for the rate only: the rate still goes, with no RR flag. */
	m.bpm = 72;
	len = radiant_ble_hrs_encode(&m, buf, 3, &written);
	zassert_equal(len, 2);
	zassert_equal(written, 0);
	zassert_equal(buf[0] & RADIANT_BLE_HRS_FLAG_RR, 0);

	/* Not even the rate. */
	zassert_equal(radiant_ble_hrs_encode(&m, buf, 1, &written), -ENOMEM);
	m.bpm = 300;
	zassert_equal(radiant_ble_hrs_encode(&m, buf, 2, &written), -ENOMEM);
}

ZTEST(hrm_hrs_encode, test_never_energy_expended)
{
	uint16_t rr[30];
	struct radiant_ble_hrs_meas m = { .rr = rr };
	uint8_t buf[64];

	memset(rr, 0xAB, sizeof(rr));
	for (unsigned int bpm = 0; bpm <= 300; bpm += 25) {
		for (unsigned int bits = 0; bits < 4; bits++) {
			m.bpm = (uint16_t)bpm;
			m.contact_supported = (bits & 1u) != 0u;
			m.contact_detected = (bits & 2u) != 0u;
			m.rr_count = bits * 7u;
			zassert_true(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL) > 0);
			zassert_equal(buf[0] & RADIANT_BLE_HRS_FLAG_ENERGY, 0);
			zassert_equal(buf[0] & 0xE0, 0, "reserved bits");
		}
	}
}

ZTEST(hrm_hrs_encode, test_invalid_args)
{
	struct radiant_ble_hrs_meas m = { .bpm = 72, .rr = NULL, .rr_count = 1 };
	uint8_t buf[20];
	size_t written = 99;

	zassert_equal(radiant_ble_hrs_encode(NULL, buf, sizeof(buf), &written), -EINVAL);
	zassert_equal(written, 0);
	zassert_equal(radiant_ble_hrs_encode(&m, NULL, sizeof(buf), NULL), -EINVAL);
	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), -EINVAL);
}

/* ══ ring + snapshot ═══════════════════════════════════════════════════════ */

static struct hrm_beat_ring ring;
static struct hrm_publish pub;
static uint32_t now;
static uint32_t ant_beats;    /* what main.c would add to page 0 */
static uint16_t ant_time;     /* and its event time */

static void publish_reset(void *f)
{
	ARG_UNUSED(f);
	hrm_beat_ring_init(&ring);
	hrm_publish_init(&pub, true, STALE_MS);
	now = 100000u;
	ant_beats = 0u;
	ant_time = 0u;
}

ZTEST_SUITE(hrm_publish, NULL, NULL, publish_reset, NULL, NULL);

/* main.c's hrm_node_deliver_beat(), less the profile. */
static void deliver(void *ctx, uint16_t seq, uint16_t event_time)
{
	uint16_t n;

	ARG_UNUSED(ctx);
	n = hrm_publish_beat(&pub, seq, event_time, now);
	if (n != 0u) {
		ant_beats += n;
		ant_time = event_time;
	}
}

static void drain(void)
{
	(void)hrm_beat_ring_drain(&ring, deliver, NULL);
}

/* A beat at `ms` (the sensor's instant), not yet drained. */
static void beat_at(uint32_t ms)
{
	(void)hrm_beat_ring_push(&ring, t1024(ms));
}

ZTEST(hrm_publish, test_burst_of_n_beats_yields_n_intervals)
{
	const uint32_t iv_ms[] = { 800, 812, 790, 805, 1001, 640 };
	uint32_t t = now;

	/* A baseline beat, drained. */
	beat_at(t);
	drain();
	zassert_equal(pub.rr_count, 0);

	/* Six beats before the worker runs again. */
	for (unsigned int i = 0; i < ARRAY_SIZE(iv_ms); i++) {
		t += iv_ms[i];
		beat_at(t);
	}
	now = t;
	drain();

	zassert_equal(ant_beats, 1 + ARRAY_SIZE(iv_ms));
	zassert_equal(ant_time, t1024(t));
	zassert_equal(pub.rr_count, ARRAY_SIZE(iv_ms));
	t = 100000u;
	for (unsigned int i = 0; i < ARRAY_SIZE(iv_ms); i++) {
		uint16_t want = (uint16_t)(t1024(t + iv_ms[i]) - t1024(t));

		zassert_equal(pub.rr[i], want, "interval %u: %u != %u", i, pub.rr[i], want);
		t += iv_ms[i];
	}
}

ZTEST(hrm_publish, test_ring_overflow_drops_rr_never_misstates)
{
	uint32_t times[1 + HRM_BEAT_RING_LEN + 3 + 1];
	unsigned int k = 0;
	uint32_t t = now;

	beat_at(t);
	times[k++] = t;
	drain();

	/* Ring length + 3 beats before the worker: 3 find the ring full. */
	for (unsigned int i = 0; i < HRM_BEAT_RING_LEN + 3; i++) {
		t += 700u + 10u * i;
		times[k++] = t;
		zassert_equal(hrm_beat_ring_push(&ring, t1024(t)), i < HRM_BEAT_RING_LEN,
			      "beat %u", i);
	}
	now = t;
	drain();

	/* The count is exact and the event time is the newest beat's. */
	zassert_equal(ant_beats, 1 + HRM_BEAT_RING_LEN + 3);
	zassert_equal(ant_time, t1024(t));

	/* The 8 stored beats each gave their true interval; the 3 dropped ones
	 * gave none - in particular no interval spans them. */
	zassert_equal(pub.rr_count, HRM_BEAT_RING_LEN);
	for (unsigned int i = 0; i < HRM_BEAT_RING_LEN; i++) {
		zassert_equal(pub.rr[i], (uint16_t)(t1024(times[i + 1]) - t1024(times[i])));
	}
	zassert_true(pub.rr_skipped >= 3);

	/* The next beat measures from the newest one, the true interval. */
	t += 750u;
	times[k++] = t;
	beat_at(t);
	now = t;
	drain();
	zassert_equal(pub.rr_count, HRM_BEAT_RING_LEN + 1);
	zassert_equal(pub.rr[HRM_BEAT_RING_LEN],
		      (uint16_t)(t1024(times[k - 1]) - t1024(times[k - 2])));
	zassert_equal(ant_beats, 1 + HRM_BEAT_RING_LEN + 3 + 1);
}

ZTEST(hrm_publish, test_seq_already_seen_is_skipped)
{
	beat_at(now);
	drain();
	zassert_equal(ant_beats, 1);
	/* The same record again (as `latest` repeats one): nothing. */
	zassert_equal(hrm_publish_beat(&pub, 1, t1024(now), now), 0);
	/* An older seq: nothing. */
	zassert_equal(hrm_publish_beat(&pub, 0, t1024(now), now), 0);
	zassert_equal(pub.beats, 1);
}

ZTEST(hrm_publish, test_rate_only_driver_notifies)
{
	struct radiant_ble_hrs_meas m;
	uint8_t buf[RADIANT_BLE_HRS_DEFAULT_PAYLOAD];
	unsigned int notifications = 0;
	const uint32_t start = now;

	/* A driver with no edge detector: one rate report a second, no beats.
	 * The node tick runs every 100 ms with a collector subscribed. */
	for (; now - start < 10000u; now += 100u) {
		if ((now - start) % 1000u == 0u) {
			hrm_publish_rate(&pub, 72, now);
		}
		(void)hrm_publish_tick(&pub, now);
		if (hrm_publish_notify_due(&pub, now, true)) {
			hrm_publish_meas(&pub, now, &m);
			zassert_equal(m.bpm, 72);
			zassert_equal(m.rr_count, 0);
			zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 2);
			zassert_equal(buf[1], 72);
			hrm_publish_rr_clear(&pub);
			notifications++;
		}
	}
	/* One at subscription, then one a second: t = 0, 1, ... 9 s. */
	zassert_equal(notifications, 10, "%u", notifications);
	zassert_equal(ant_beats, 0, "no beats: the ANT+ count stays frozen");
}

ZTEST(hrm_publish, test_notify_cadence_is_1hz_on_a_late_tick)
{
	unsigned int notifications = 0;
	const uint32_t start = now;

	/* A 100 ms tick that is always 7 ms late still averages 1 Hz: the
	 * period accumulates rather than re-anchoring to the tick. */
	for (; now - start < 60000u; now += 107u) {
		if (hrm_publish_notify_due(&pub, now, true)) {
			notifications++;
		}
	}
	zassert_true(notifications >= 60 && notifications <= 61, "%u", notifications);
}

ZTEST(hrm_publish, test_repeated_bpm_stays_fresh_silence_expires)
{
	struct radiant_ble_hrs_meas m;
	uint32_t last_report = now;

	/* The same 72 every second for 20 s: valid throughout. Before the
	 * fix a repeated rate was dropped as "no change" and refreshed
	 * nothing. */
	for (unsigned int i = 0; i < 200; i++, now += 100u) {
		if (i % 10u == 0u) {
			hrm_publish_rate(&pub, 72, now);
			last_report = now;
		}
		zassert_false(hrm_publish_tick(&pub, now));
		zassert_equal(hrm_publish_bpm(&pub, now), 72, "at %u ms", i * 100u);
	}

	/* Then silence. Valid until the deadline, invalid on both radios at
	 * it: ANT+ byte 7 is hrm_publish_bpm(), BLE the measurement. */
	now = last_report + STALE_MS - 1u;
	zassert_equal(hrm_publish_bpm(&pub, now), 72);
	zassert_false(hrm_publish_tick(&pub, now));
	now = last_report + STALE_MS;
	zassert_equal(hrm_publish_bpm(&pub, now), 0, "ANT+ byte 7");
	hrm_publish_meas(&pub, now, &m);
	zassert_equal(m.bpm, 0, "BLE rate");
	zassert_true(m.contact_supported);
	zassert_true(m.contact_detected, "contact status is the driver's, not expiry's");
	zassert_true(hrm_publish_tick(&pub, now));
	zassert_false(hrm_publish_tick(&pub, now + 100u), "reported once");

	/* A fresh report revives it. */
	hrm_publish_rate(&pub, 75, now);
	zassert_equal(hrm_publish_bpm(&pub, now), 75);
}

ZTEST(hrm_publish, test_beats_keep_rate_fresh_and_zero_is_invalid)
{
	/* The example driver's shape: one rate report, then beats only. */
	hrm_publish_rate(&pub, 72, now);
	for (unsigned int i = 0; i < 20; i++) {
		now += 833u;
		beat_at(now);
		drain();
		zassert_equal(hrm_publish_bpm(&pub, now), 72);
	}
	/* A reported 0 (RADIANT_PROFILE_HR_INVALID_BPM) is invalid at once. */
	hrm_publish_rate(&pub, 0, now);
	zassert_equal(hrm_publish_bpm(&pub, now), 0);
}

ZTEST(hrm_publish, test_silence_is_an_rr_gap)
{
	beat_at(now);
	drain();
	hrm_publish_rate(&pub, 60, now);

	/* Beats stop long enough to expire the rate; the next beat starts a
	 * new baseline, so no interval spans the silence. */
	now += STALE_MS + 500u;
	(void)hrm_publish_tick(&pub, now);
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 0);

	/* An interval longer than the stale limit is a gap even when no tick
	 * ran in between to expire anything. */
	now += STALE_MS + 1u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 0);

	now += 1000u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 1);
	zassert_equal(pub.rr[0], 1024);
}

ZTEST(hrm_publish, test_contact_loss_clears_rr_and_invalidates_bpm)
{
	struct radiant_ble_hrs_meas m;
	uint8_t buf[RADIANT_BLE_HRS_DEFAULT_PAYLOAD];

	hrm_publish_rate(&pub, 72, now);
	for (unsigned int i = 0; i < 4; i++) {
		now += 833u;
		beat_at(now);
		drain();
	}
	zassert_equal(pub.rr_count, 3);
	zassert_equal(hrm_publish_bpm(&pub, now), 72);

	hrm_publish_contact(&pub, false, now);
	zassert_equal(pub.rr_count, 0, "pending RR cleared");
	zassert_equal(hrm_publish_bpm(&pub, now), 0, "invalid at once, before any expiry");

	hrm_publish_meas(&pub, now, &m);
	zassert_true(m.contact_supported, "support is a capability and does not change");
	zassert_false(m.contact_detected);
	zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 2);
	zassert_equal(buf[0], RADIANT_BLE_HRS_FLAG_CONTACT_SUPP);
	zassert_equal(buf[1], 0);

	/* A rate or beat reported without contact does not revive it. */
	hrm_publish_rate(&pub, 72, now);
	zassert_equal(hrm_publish_bpm(&pub, now), 0);

	/* Contact back: still invalid until the next report... */
	now += 500u;
	hrm_publish_contact(&pub, true, now);
	zassert_equal(hrm_publish_bpm(&pub, now), 0);

	/* ...and the first beat after it starts a new baseline: no interval
	 * spans the loss. */
	now += 833u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 0);
	zassert_equal(hrm_publish_bpm(&pub, now), 72, "the beat revives the rate");
	now += 833u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 1);

	hrm_publish_meas(&pub, now, &m);
	zassert_true(m.contact_supported);
	zassert_true(m.contact_detected);
}

ZTEST(hrm_publish, test_contact_ignored_without_support)
{
	struct radiant_ble_hrs_meas m;

	hrm_publish_init(&pub, false, STALE_MS);
	hrm_publish_rate(&pub, 72, now);
	hrm_publish_contact(&pub, false, now);
	zassert_equal(hrm_publish_bpm(&pub, now), 72);
	hrm_publish_meas(&pub, now, &m);
	zassert_false(m.contact_supported);
	zassert_false(m.contact_detected, "status is never sent without support");
}

ZTEST(hrm_publish, test_disconnect_clears_rr)
{
	hrm_publish_rate(&pub, 72, now);
	for (unsigned int i = 0; i < 3; i++) {
		now += 833u;
		beat_at(now);
		drain();
	}
	zassert_equal(pub.rr_count, 2);

	/* main.c's ble_publish() on hrm_ble_take_disconnect(). */
	hrm_publish_rr_clear(&pub);
	zassert_equal(pub.rr_count, 0);

	/* And with nobody subscribed at a notify tick, intervals do not pile
	 * up for the next collector. */
	now += 833u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 1);
	zassert_false(hrm_publish_notify_due(&pub, now, false));
	zassert_equal(pub.rr_count, 0);

	/* The next subscriber is notified at once, with no stale RR. */
	zassert_true(hrm_publish_notify_due(&pub, now, true));
	zassert_equal(pub.rr_count, 0);
}

ZTEST(hrm_publish, test_pending_rr_cap_drops_oldest)
{
	hrm_publish_rate(&pub, 200, now);
	beat_at(now);
	drain();
	for (unsigned int i = 0; i < HRM_PUBLISH_RR_MAX + 4; i++) {
		now += 300u + i;
		beat_at(now);
		drain();
	}
	zassert_equal(pub.rr_count, HRM_PUBLISH_RR_MAX);
	/* The newest is last; the four oldest went. */
	zassert_equal(pub.rr[HRM_PUBLISH_RR_MAX - 1],
		      (uint16_t)(t1024(now) - t1024(now - (300u + HRM_PUBLISH_RR_MAX + 3u))));
	zassert_equal(pub.rr_skipped, 4);
}

ZTEST(hrm_publish, test_ring_wraps_cleanly)
{
	/* Many passes through the ring, one beat per pass and bursts of 1..8,
	 * with the 16-bit seq wrapping: every beat counted, every interval
	 * reported (none dropped, the ring never overflows here). */
	uint32_t total = 0;
	unsigned int burst = 1;

	hrm_publish_rate(&pub, 72, now);
	beat_at(now);
	drain();
	total = 1;
	while (total < 70000u) {
		for (unsigned int i = 0; i < burst; i++) {
			now += 400u;
			beat_at(now);
		}
		drain();
		total += burst;
		hrm_publish_rr_clear(&pub);
		burst = (burst % HRM_BEAT_RING_LEN) + 1u;
	}
	zassert_equal(ant_beats, total);
	zassert_equal(pub.rr_skipped, 0);
	zassert_equal(ant_time, t1024(now));
}
