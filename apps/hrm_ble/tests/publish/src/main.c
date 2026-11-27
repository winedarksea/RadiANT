/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The heart-rate node's publish path, one rule per case.
 *
 * Suite hrm_hrs_encode - radiant_ble_hrs_encode(), the pure HRS 1.0 Heart Rate
 * Measurement encoder: flags (format, contact support vs status, never Energy
 * Expended), RR-Interval packing, and the ATT MTU cap that sends the OLDEST
 * intervals and leaves the rest for the next notification (HRS 1.0 3.1.1.5).
 *
 * Suite hrm_publish - the beat ring (src/hrm_beat_ring.c) feeding the snapshot
 * (src/hrm_publish.c) that drives both radios: N coalesced beats yield N
 * intervals, an overflowing ring drops intervals and never mis-states one, a
 * 66 s beat pause kept alive by rate reports is a gap and not a 2 s interval,
 * a rate-only driver still notifies, a repeated rate stays fresh while silence
 * expires it on both radios, a gap never revives the old rate (beats derive
 * one), contact loss clears RR without touching the support bit, RR carry
 * over between notifications and survive a failed one, and a dropped link
 * clears them.
 *
 * Suite hrm_pass - the sensor worker's pass order (src/hrm_sensor_pass.c),
 * alone and against the real ring and snapshot, including the loss/noise/
 * regain race.
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
#include "hrm_sensor_pass.h"

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

ZTEST(hrm_hrs_encode, test_mtu_cap_sends_oldest)
{
	uint16_t rr[12];
	struct radiant_ble_hrs_meas m = { .bpm = 72, .rr = rr, .rr_count = 12 };
	uint8_t buf[RADIANT_BLE_HRS_DEFAULT_PAYLOAD];
	size_t written;
	int len;

	for (unsigned int i = 0; i < 12; i++) {
		rr[i] = (uint16_t)(1000 + i);
	}

	/* 20 bytes: flags + UINT8 rate + 9 intervals, the OLDEST 9 (HRS 1.0
	 * 3.1.1.5: the rest go in the next notification). */
	len = radiant_ble_hrs_encode(&m, buf, sizeof(buf), &written);
	zassert_equal(len, 20);
	zassert_equal(written, 9);
	for (unsigned int i = 0; i < 9; i++) {
		uint16_t v = (uint16_t)(buf[2 + 2 * i] | (buf[3 + 2 * i] << 8));

		zassert_equal(v, rr[i], "slot %u", i);
	}

	/* UINT16 rate costs one interval. */
	m.bpm = 300;
	len = radiant_ble_hrs_encode(&m, buf, sizeof(buf), &written);
	zassert_equal(len, 19);
	zassert_equal(written, 8);
	zassert_equal((uint16_t)(buf[3] | (buf[4] << 8)), rr[0]);
	zassert_equal((uint16_t)(buf[17] | (buf[18] << 8)), rr[7]);

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

ZTEST(hrm_publish, test_66s_beat_pause_with_rate_keepalive_is_a_gap)
{
	const uint32_t pause = 66000u;
	uint32_t skipped;

	hrm_publish_rate(&pub, 60, now);
	beat_at(now);
	drain();
	now += 1000u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 1);

	/* No beats for 66 s, but the driver keeps reporting its rate, so the
	 * deadline never expires and the baseline survives. */
	for (uint32_t t = 0; t < pause; t += 100u) {
		now += 100u;
		if (t % 1000u == 0u) {
			hrm_publish_rate(&pub, 60, now);
		}
		zassert_false(hrm_publish_tick(&pub, now));
	}

	/* 66 s in 1/1024 s is 67584, which wraps to 2048 - a plausible 2 s
	 * RR-Interval. It must not go out. */
	zassert_equal((uint16_t)(t1024(now) - t1024(now - pause)), 2048);
	skipped = pub.rr_skipped;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 1, "no interval across the pause");
	zassert_equal(pub.rr_skipped, skipped + 1);

	/* The beat after it measures from it. */
	now += 1000u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 2);
	zassert_equal(pub.rr[1], 1024);
}

ZTEST(hrm_publish, test_rate_only_driver_notifies)
{
	struct radiant_ble_hrs_meas m;
	uint8_t buf[RADIANT_BLE_HRS_DEFAULT_PAYLOAD];
	unsigned int notifications = 0;
	const uint32_t start = now;

	/* A driver with no edge detector: one rate report a second, no beats.
	 * The node tick's BLE step runs every 100 ms with a collector
	 * subscribed, exactly as main.c's ble_publish() drives it. */
	for (; now - start < 10000u; now += 100u) {
		if ((now - start) % 1000u == 0u) {
			hrm_publish_rate(&pub, 72, now);
		}
		(void)hrm_publish_tick(&pub, now);
		if (hrm_publish_ble_step(&pub, now, true, false, &m)) {
			zassert_equal(m.bpm, 72);
			zassert_equal(m.rr_count, 0);
			zassert_equal(radiant_ble_hrs_encode(&m, buf, sizeof(buf), NULL), 2);
			zassert_equal(buf[1], 72);
			notifications++;
		}
	}
	/* One at subscription, then one a second: t = 0, 1, ... 9 s. */
	zassert_equal(notifications, 10, "%u", notifications);
	zassert_equal(ant_beats, 0, "no beats: the ANT+ count stays frozen");
}

ZTEST(hrm_publish, test_notify_cadence_is_1hz_on_a_late_tick)
{
	struct radiant_ble_hrs_meas m;
	unsigned int notifications = 0;
	const uint32_t start = now;

	/* A 100 ms tick that is always 7 ms late still averages 1 Hz: the
	 * period accumulates rather than re-anchoring to the tick. */
	for (; now - start < 60000u; now += 107u) {
		if (hrm_publish_ble_step(&pub, now, true, false, &m)) {
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

ZTEST(hrm_publish, test_expiry_never_revives_old_rate)
{
	/* A rate, then a long silence: the rate expires. */
	hrm_publish_rate(&pub, 150, now);
	beat_at(now);
	drain();
	now += STALE_MS + 60000u;
	zassert_true(hrm_publish_tick(&pub, now));

	/* Beats return at a very different pace, with no rate report. The
	 * minute-old 150 must not come back: the first beat has no interval
	 * yet (0), the second gives a derived rate from its interval. */
	beat_at(now);
	drain();
	zassert_equal(hrm_publish_bpm(&pub, now), 0, "no interval yet, and 150 is stale");
	now += 1000u;
	beat_at(now);
	drain();
	zassert_equal(hrm_publish_bpm(&pub, now), 60, "derived from a 1024/1024 s interval");

	/* A fresh report overrides the derived value. */
	hrm_publish_rate(&pub, 62, now);
	zassert_equal(hrm_publish_bpm(&pub, now), 62);
}

ZTEST(hrm_publish, test_beat_only_driver_gets_derived_rate)
{
	unsigned int bpm;

	/* Never a rate report: 0 until one interval, then 60*1024/mean. */
	beat_at(now);
	drain();
	zassert_equal(hrm_publish_bpm(&pub, now), 0);
	for (unsigned int i = 0; i < 6; i++) {
		now += 800u;
		beat_at(now);
		drain();
		bpm = hrm_publish_bpm(&pub, now);
		zassert_true(bpm >= 74 && bpm <= 76, "%u", bpm);
	}
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
	unsigned int bpm;

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

	/* A rate or a beat reported without contact does nothing but count
	 * the beat on ANT+. */
	hrm_publish_rate(&pub, 72, now);
	now += 400u;
	beat_at(now);
	drain();
	zassert_equal(hrm_publish_bpm(&pub, now), 0);
	zassert_equal(pub.rr_count, 0);
	zassert_equal(ant_beats, 5);

	/* Contact back: still invalid, and the old 72 is not revived... */
	now += 500u;
	hrm_publish_contact(&pub, true, now);
	zassert_equal(hrm_publish_bpm(&pub, now), 0);

	/* ...the first beat after it starts a new baseline: no interval spans
	 * the loss, and with no interval yet there is no rate either. */
	now += 833u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 0);
	zassert_equal(hrm_publish_bpm(&pub, now), 0, "the stale 72 is not revived");

	/* The second beat gives an interval and a rate derived from it. */
	now += 833u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 1);
	bpm = hrm_publish_bpm(&pub, now);
	zassert_true(bpm >= 71 && bpm <= 73, "derived %u", bpm);

	/* A fresh report wins. */
	hrm_publish_rate(&pub, 80, now);
	zassert_equal(hrm_publish_bpm(&pub, now), 80);

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

/* Twelve pending intervals, the oldest first. */
static void make_twelve_rr(void)
{
	hrm_publish_rate(&pub, 72, now);
	beat_at(now);
	drain();
	for (unsigned int i = 0; i < 12; i++) {
		now += 500u + 10u * i;
		beat_at(now);
		drain();
	}
	zassert_equal(pub.rr_count, 12);
}

ZTEST(hrm_publish, test_rr_carry_over_to_next_notification)
{
	struct radiant_ble_hrs_meas m;
	uint8_t buf[RADIANT_BLE_HRS_DEFAULT_PAYLOAD];
	uint16_t want[12];
	size_t sent;

	make_twelve_rr();
	memcpy(want, pub.rr, sizeof(want));

	/* The first notification at the default MTU carries the 9 oldest. */
	zassert_true(hrm_publish_ble_step(&pub, now, true, false, &m));
	zassert_true(radiant_ble_hrs_encode(&m, buf, sizeof(buf), &sent) > 0);
	zassert_equal(sent, 9);
	hrm_publish_rr_consume(&pub, sent);

	/* The other 3 wait, still oldest first, and lead the next one. */
	zassert_equal(pub.rr_count, 3);
	zassert_equal(pub.rr[0], want[9]);
	zassert_equal(pub.rr[2], want[11]);
	now += 600u;
	beat_at(now);
	drain();
	now = pub.next_notify_ms;
	zassert_true(hrm_publish_ble_step(&pub, now, true, false, &m));
	zassert_equal(m.rr_count, 4);
	zassert_true(radiant_ble_hrs_encode(&m, buf, sizeof(buf), &sent) > 0);
	zassert_equal(sent, 4);
	zassert_equal((uint16_t)(buf[2] | (buf[3] << 8)), want[9]);
	hrm_publish_rr_consume(&pub, sent);
	zassert_equal(pub.rr_count, 0);
}

ZTEST(hrm_publish, test_failed_notification_keeps_rr)
{
	struct radiant_ble_hrs_meas m;

	make_twelve_rr();
	/* main.c consumes only when hrm_ble_notify() reports an accepted
	 * notification; a failed one (rr_sent 0) leaves everything pending. */
	zassert_true(hrm_publish_ble_step(&pub, now, true, false, &m));
	hrm_publish_rr_consume(&pub, 0);
	zassert_equal(pub.rr_count, 12);
	now = pub.next_notify_ms;
	zassert_true(hrm_publish_ble_step(&pub, now, true, false, &m));
	zassert_equal(m.rr_count, 12, "offered again");
}

ZTEST(hrm_publish, test_disconnect_clears_rr)
{
	struct radiant_ble_hrs_meas m;

	hrm_publish_rate(&pub, 72, now);
	for (unsigned int i = 0; i < 3; i++) {
		now += 833u;
		beat_at(now);
		drain();
	}
	zassert_true(hrm_publish_ble_step(&pub, now, true, false, &m));
	now += 100u;
	zassert_equal(pub.rr_count, 2);

	/* The tick after a link dropped (hrm_ble_take_disconnect() true): the
	 * pending intervals are gone even though the collector - reconnected
	 * already, or another one - is subscribed. */
	(void)hrm_publish_ble_step(&pub, now, true, true, &m);
	zassert_equal(pub.rr_count, 0);

	/* And with nobody subscribed at a tick, intervals do not pile up for
	 * the next collector. */
	now += 833u;
	beat_at(now);
	drain();
	zassert_equal(pub.rr_count, 1);
	zassert_false(hrm_publish_ble_step(&pub, now, false, false, &m));
	zassert_equal(pub.rr_count, 0);

	/* The next subscriber is notified at once, with no stale RR. */
	zassert_true(hrm_publish_ble_step(&pub, now, true, false, &m));
	zassert_equal(m.rr_count, 0);
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

/* ══ the worker's pass order ═══════════════════════════════════════════════ */

static char ev_log[32];
static unsigned int ev_n;
static bool fake_level;
static bool level_after_drain;   /* the level flips during the drain */
static bool use_real;            /* ops feed the real ring and snapshot */

static void ev(char c)
{
	if (ev_n < sizeof(ev_log) - 1u) {
		ev_log[ev_n++] = c;
		ev_log[ev_n] = '\0';
	}
}

static bool op_level(void *ctx)
{
	ARG_UNUSED(ctx);
	return fake_level;
}

static void op_rate(void *ctx, uint8_t bpm)
{
	ARG_UNUSED(ctx);
	ev('R');
	if (use_real) {
		hrm_publish_rate(&pub, bpm, now);
	}
}

static void op_drain(void *ctx)
{
	ARG_UNUSED(ctx);
	ev('B');
	if (use_real) {
		drain();
	}
	fake_level = level_after_drain;
}

static void op_contact(void *ctx, bool detected)
{
	ARG_UNUSED(ctx);
	ev(detected ? '+' : '-');
	if (use_real) {
		hrm_publish_contact(&pub, detected, now);
	}
}

static const struct hrm_pass_ops fake_ops = {
	.level = op_level,
	.rate = op_rate,
	.drain = op_drain,
	.contact = op_contact,
};

static struct hrm_pass_state pass;

static void pass_reset(void *f)
{
	publish_reset(f);
	hrm_pass_init(&pass);
	ev_n = 0u;
	ev_log[0] = '\0';
	fake_level = true;
	level_after_drain = true;
	use_real = false;
}

ZTEST_SUITE(hrm_pass, NULL, NULL, pass_reset, NULL, NULL);

ZTEST(hrm_pass, test_plain_pass_is_rate_then_beats)
{
	hrm_pass_run(&pass, (1u << 8) | 72u, 0u, &fake_ops);
	zassert_str_equal(ev_log, "RB");
	/* No new report: no rate call. */
	hrm_pass_run(&pass, (1u << 8) | 72u, 0u, &fake_ops);
	zassert_str_equal(ev_log, "RBB");
}

ZTEST(hrm_pass, test_loss_goes_first_regain_after_drain)
{
	/* A loss and a regain both since the last pass: the loss before the
	 * beats, the regain after them. */
	hrm_pass_run(&pass, (1u << 8) | 72u, 1u, &fake_ops);
	zassert_str_equal(ev_log, "-RB+");

	/* A regain alone (its loss delivered by an earlier pass) still comes
	 * after the drain. */
	ev_n = 0u;
	fake_level = false;
	level_after_drain = false;
	hrm_pass_run(&pass, (1u << 8) | 72u, 2u, &fake_ops);
	zassert_str_equal(ev_log, "-B", "%s", ev_log);
	ev_n = 0u;
	fake_level = true;
	level_after_drain = true;
	hrm_pass_run(&pass, (1u << 8) | 72u, 2u, &fake_ops);
	zassert_str_equal(ev_log, "B+", "%s", ev_log);
}

ZTEST(hrm_pass, test_loss_seen_only_in_level_is_delivered)
{
	/* The level dropped during the drain, before the counter moved: the
	 * loss is delivered at once, and again (harmlessly) when the counter
	 * shows up. */
	level_after_drain = false;
	hrm_pass_run(&pass, 0u, 0u, &fake_ops);
	zassert_str_equal(ev_log, "B-");
	hrm_pass_run(&pass, 0u, 1u, &fake_ops);
	zassert_str_equal(ev_log, "B--B", "%s", ev_log);
}

ZTEST(hrm_pass, test_noise_beats_during_loss_make_no_rr)
{
	uint32_t rate_word = 0u;
	uint32_t losses = 0u;

	use_real = true;

	/* Normal beats with a rate. */
	for (unsigned int i = 0; i < 4; i++) {
		now += 833u;
		beat_at(now);
		rate_word = (((rate_word >> 8) + 1u) << 8) | 72u;
		hrm_pass_run(&pass, rate_word, losses, &fake_ops);
	}
	zassert_equal(pub.rr_count, 3);

	/* Pass N: the loss. */
	losses++;
	fake_level = false;
	level_after_drain = false;
	hrm_pass_run(&pass, rate_word, losses, &fake_ops);
	zassert_equal(pub.rr_count, 0);

	/* Off the skin: noise beats, then the regain - all before pass N+1. */
	now += 200u;
	beat_at(now);
	now += 190u;
	beat_at(now);
	now += 210u;
	beat_at(now);
	fake_level = true;
	level_after_drain = true;
	now += 50u;

	/* Pass N+1 drains with contact still lost, THEN applies the regain:
	 * the noise counted on ANT+ only - no interval, no rate refresh. */
	ev_n = 0u;
	hrm_pass_run(&pass, rate_word, losses, &fake_ops);
	zassert_str_equal(ev_log, "B+", "%s", ev_log);
	zassert_equal(pub.rr_count, 0, "noise produced no RR");
	zassert_equal(hrm_publish_bpm(&pub, now), 0, "noise refreshed nothing");
	zassert_equal(ant_beats, 4 + 3, "but every beat counts on ANT+");

	/* Real beats after the regain: the first is a baseline, the second
	 * an interval. */
	now += 833u;
	beat_at(now);
	hrm_pass_run(&pass, rate_word, losses, &fake_ops);
	zassert_equal(pub.rr_count, 0);
	now += 833u;
	beat_at(now);
	hrm_pass_run(&pass, rate_word, losses, &fake_ops);
	zassert_equal(pub.rr_count, 1);
}

ZTEST(hrm_pass, test_loss_and_regain_in_one_pass_drop_the_beats)
{
	use_real = true;
	hrm_publish_rate(&pub, 72, now);
	beat_at(now);
	hrm_pass_run(&pass, 0u, 0u, &fake_ops);
	now += 833u;
	beat_at(now);
	hrm_pass_run(&pass, 0u, 0u, &fake_ops);
	zassert_equal(pub.rr_count, 1);

	/* Beats, a loss and a regain all land before one pass: nobody can say
	 * which beats were on the skin, so none makes an interval. */
	now += 833u;
	beat_at(now);
	now += 833u;
	beat_at(now);
	ev_n = 0u;
	hrm_pass_run(&pass, 0u, 1u, &fake_ops);
	zassert_str_equal(ev_log, "-B+", "%s", ev_log);
	zassert_equal(pub.rr_count, 0);
	zassert_equal(ant_beats, 4);
}
