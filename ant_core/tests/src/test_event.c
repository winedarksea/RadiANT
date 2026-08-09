/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original work. Every expected byte in this file was written from
 * src/ant_wire.h (generated from protocol/ant_wire.yaml), from
 * tools/ant_verify.py's extended_fields() - which this module has to be the
 * exact inverse of - and from the t_sync contract in
 * ant_core/include/ant_radio_hal.h, not from running ant_event.c and writing
 * down what it did. Nothing here derives from sdk-ant, from libant.a, from
 * disassembly of any binary, or from any adopter-gated ANT+ device profile
 * document.
 *
 * ---------------------------------------------------------------------------
 * The suite for ant_core/src/ant_event.c
 * ---------------------------------------------------------------------------
 * Three things in this module have no functional symptom when they are wrong,
 * and most of the file is about them.
 *
 *   THE RX TIMESTAMP. Reading a clock at drain time instead of using the
 *   backend's t_sync passes every functional test in existence and destroys
 *   the only measurement the field exists for - the 0.009 ms radio-clock
 *   figure the A/B `timing` gate is read against, and the basis of the
 *   sub-millisecond fusion claim. test_rx_timestamp_survives_the_clock_moving
 *   advances the virtual clock by more than a second between the frame
 *   arriving and the queue draining, and asserts the reported ticks did not
 *   move. It is the single most important test here.
 *
 *   THE EXTENDED FIELD LAYOUT. Field order and the flag byte are a Tier 1
 *   byte-diff, so they are asserted byte by byte rather than round-tripped.
 *   test_ext_offsets_move_with_the_flags is the one that would catch a
 *   decoder-shaped bug: it omits the channel id and checks that RSSI moved up
 *   to where the channel id used to be, which is exactly the mistake
 *   extended_fields() is written to avoid.
 *
 *   THE CRC GATE. With eight filters armed the bench sees ~1.4 CRC failures a
 *   second on a quiet room, and Spike A measured a 3-byte address triggering
 *   the matcher on noise with zero valid frames behind it. A CRC-failed frame
 *   that becomes an event is a phantom sensor, and phantom sensors are hard to
 *   attribute later.
 *
 * These run only in CI on Linux: native_sim does not build on Windows, so no
 * amount of local work executes them. See ant_core/tests/testcase.yaml.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "ant_event.h"
#include "ant_frame.h"
#include "fake_radio.h"

/* ---------------------------------------------------------------------------
 * The port hooks, and the one definition of antr_on_message() in this binary
 *
 * ant_event.c deliberately includes no Zephyr header, so the two things it
 * needs from an RTOS are symbols the port supplies. Here the mock radio is
 * single-threaded and its clock only moves when a test moves it, so the
 * critical section is a no-op and the wakeup is a counter - which is also the
 * cheapest way to assert that a post really did try to wake the drain.
 * ---------------------------------------------------------------------------
 */

static uint32_t n_wakeups;

unsigned int ant_event_crit_enter(void)
{
	return 0u;
}

void ant_event_crit_exit(unsigned int key)
{
	ARG_UNUSED(key);
}

void ant_event_wakeup(void)
{
	n_wakeups++;
}

/*
 * The bridge's side of the inverted event path. In the firmware this is
 * src/ant_serial_bridge.c; in this binary it is here, and it is the only
 * definition, so any future suite that wants delivered messages should read
 * this recorder rather than define a second one.
 */
#define REC_MAX 192

struct rec {
	uint8_t id;
	uint8_t len;
	uint8_t data[ANT_EVENT_MSG_MAX];
};

static struct rec recs[REC_MAX];
static uint32_t   n_recs;

void antr_on_message(const struct antr_msg *msg)
{
	uint8_t n;

	zassert_not_null(msg, "antr_on_message() called with a null message");
	zassert_not_null(msg->data, "antr_msg.data must never be null");
	zassert_true(msg->len <= ANT_EVENT_MSG_MAX,
		     "message longer than this module can build: %u", msg->len);

	if (n_recs < REC_MAX) {
		n = msg->len;
		recs[n_recs].id = msg->id;
		recs[n_recs].len = n;
		if (n > 0u) {
			memcpy(recs[n_recs].data, msg->data, n);
		}
	}
	n_recs++;
}

/* ---------------------------------------------------------------------------
 * Ground truth
 * ---------------------------------------------------------------------------
 */

/*
 * The frame Spike A pulled off the air (docs/spike-a-results.md), which is
 * also test_frame.c's strongest vector: a real ANT+ power meter with an
 * independently established channel ID.
 */
#define DEVNUM  14871u   /* 0x3A17 */
#define DTYPE   0x0Bu
#define TTYPE   5u

static const uint8_t payload8[ANT_FRAME_PAYLOAD_STD] = {
	0x10, 0xBD, 0xFF, 0x50, 0xDE, 0x11, 0x64, 0x00,
};

/* Where the extended tail starts in a received data message with a standard
 * 8-byte payload: one channel byte plus eight payload bytes. This is the
 * offset tools/ant_verify.py and tools/ant_scan.py both read the flag byte
 * from, spelled here as arithmetic so a reader can check it. */
#define FLAG_OFFSET  (1u + ANT_FRAME_PAYLOAD_STD)

/* ---------------------------------------------------------------------------
 * Suite plumbing
 * ---------------------------------------------------------------------------
 */

static void before_each(void *f)
{
	ARG_UNUSED(f);
	fake_radio_reset();
	ant_event_init();
	n_recs = 0u;
	n_wakeups = 0u;
}

/*
 * The two habits fake_radio.h asks every suite to adopt: an operation left
 * armed, and anything a callback did that would deadlock a real radio ISR.
 * Both are end-of-test conditions, so they belong in the after hook rather
 * than repeated at the bottom of a dozen tests.
 */
static void after_each(void *f)
{
	ARG_UNUSED(f);
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

ZTEST_SUITE(ant_event, NULL, NULL, before_each, after_each, NULL);

/* ---------------------------------------------------------------------------
 * A synthetic HAL event, for the tests that are about the queue rather than
 * about the radio
 * ---------------------------------------------------------------------------
 */

static struct ant_rx_event mk_hal(enum ant_radio_status status,
				  ant_time_t t_sync, bool exact,
				  bool has_rssi, int8_t rssi_dbm)
{
	struct ant_rx_event e;

	memset(&e, 0, sizeof(e));
	e.op = 1u;
	e.status = status;
	e.t_sync = t_sync;
	e.t_sync_exact = exact;
	e.has_rssi = has_rssi;
	e.rssi_dbm = rssi_dbm;
	e.body = payload8;
	e.body_len = (uint8_t)sizeof(payload8);
	return e;
}

static struct ant_event_rx mk_rx(const struct ant_rx_event *hal,
				 uint8_t channel, const uint8_t *payload)
{
	struct ant_event_rx rx;

	memset(&rx, 0, sizeof(rx));
	rx.hal = hal;
	rx.msg_id = (uint8_t)ANTW_MESG_BROADCAST_DATA_ID;
	rx.channel = channel;
	rx.id.device_number = DEVNUM;
	rx.id.device_type = DTYPE;
	rx.id.trans_type = TTYPE;
	rx.payload = payload;
	rx.payload_len = (uint8_t)ANT_FRAME_PAYLOAD_STD;
	return rx;
}

/* ---------------------------------------------------------------------------
 * Driving a real receive through the mock radio
 * ---------------------------------------------------------------------------
 */

static int        cb_rc;        /* what ant_event_post_rx() returned */
static ant_time_t cb_t_sync;    /* the t_sync the callback was handed */
static uint32_t   cb_calls;

static void rx_cb(const struct ant_rx_event *e, void *user)
{
	struct ant_event_rx rx;

	ARG_UNUSED(user);

	if (e->status != ANT_RADIO_STATUS_OK &&
	    e->status != ANT_RADIO_STATUS_CRC_FAIL) {
		/* The window's terminal timeout. Not a frame. */
		return;
	}
	if (e->body_len <= ANT_FRAME_HDR_LEN_TRACKING) {
		return;
	}

	cb_calls++;
	cb_t_sync = e->t_sync;

	memset(&rx, 0, sizeof(rx));
	rx.hal = e;
	rx.msg_id = (uint8_t)ANTW_MESG_BROADCAST_DATA_ID;
	rx.channel = 0u;
	rx.id.device_number = DEVNUM;
	rx.id.device_type = DTYPE;
	rx.id.trans_type = TTYPE;
	/* A tracking body is [ttype][ctrl][d0..d7]: the payload starts after
	 * the two header bytes. The pointer is the mock's own buffer, which it
	 * overwrites with 0xDD the instant this callback returns - so passing
	 * it here is exactly the case the copy-in-the-ISR rule exists for. */
	rx.payload = e->body + ANT_FRAME_HDR_LEN_TRACKING;
	rx.payload_len = (uint8_t)(e->body_len - ANT_FRAME_HDR_LEN_TRACKING);

	cb_rc = ant_event_post_rx(&rx);
}

static const struct ant_radio_cbs cbs = { .rx = rx_cb, .tx = NULL };

/* One ANT+ broadcast, heard by one tracked window. Returns the t_sync the
 * frame was put on the air with, so a test can check the reported timestamp
 * against the transmitter's instant and not only against the receiver's. */
static ant_time_t run_one_rx(bool crc_bad, uint32_t flags)
{
	uint8_t               frame[FAKE_RADIO_AIR_FRAME_MAX];
	struct fake_radio_air air;
	struct ant_rx_filter  filt;
	struct ant_rx_req     req;
	uint32_t              op = 0u;
	uint8_t               n;
	ant_time_t            t;

	cb_rc = ANT_EVENT_ESTATE;
	cb_calls = 0u;

	n = fake_radio_build_ant_frame(frame, (uint16_t)DEVNUM, (uint8_t)DTYPE,
				       (uint8_t)TTYPE, payload8);

	zassert_ok(ant_radio_init(&cbs, NULL));
	zassert_ok(ant_radio_enable());

	t = ant_radio_now() + 100000u;

	fake_radio_air_init(&air);
	air.t_sync = t;
	memcpy(air.bytes, frame, n);
	air.len = n;
	air.crc_bad = crc_bad;
	zassert_equal(ANT_RADIO_OK_RC, fake_radio_air_add(&air));

	memset(&filt, 0, sizeof(filt));
	memcpy(filt.addr, frame, ANT_FRAME_ADDR_LEN_TRACKING);
	filt.addr_len = (uint8_t)ANT_FRAME_ADDR_LEN_TRACKING;

	memset(&req, 0, sizeof(req));
	req.fmt = ant_frame_format(ANT_FRAME_CFG_TRACKING);
	req.rf_index = (uint8_t)ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt;
	req.n_filters = 1u;
	req.t_open = t - 500u;
	req.t_close = t + 500u;
	req.flags = flags;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op));

	fake_radio_advance_to(req.t_close + 1u);
	zassert_equal(1u, cb_calls, "the window did not hear the frame");

	return t;
}

/* ---------------------------------------------------------------------------
 * The tick conversion, pinned against hand-computed values
 *
 * Independent of the implementation: ticks = floor(us * 32768 / 1000000), and
 * the field is 16 bits of a 32768 Hz counter so it wraps every two seconds.
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_event, test_rx_ticks_is_the_32768_hz_counter)
{
	zassert_equal(0u, ant_event_rx_ticks(0u));

	/* 15625 us is exactly 512 ticks - the reduced ratio itself. */
	zassert_equal(512u, ant_event_rx_ticks(15625u));

	/* One second is half the field. */
	zassert_equal(32768u, ant_event_rx_ticks(1000000u));

	/* The last microsecond before the wrap, and the wrap. */
	zassert_equal(65535u, ant_event_rx_ticks(1999999u));
	zassert_equal(0u, ant_event_rx_ticks(2000000u));
	zassert_equal(32768u, ant_event_rx_ticks(5000000u));
}

/*
 * The mock's clock starts one second below 2^32 microseconds precisely so that
 * a module storing a timestamp in a uint32_t fails in its first virtual
 * second. 2^32 us is 967,296 us past a two-second boundary, which is 31,696
 * ticks; a 32-bit truncation of the input would give 0.
 */
ZTEST(ant_event, test_rx_ticks_is_not_a_uint32)
{
	zassert_equal(31696u, ant_event_rx_ticks((ant_time_t)0x100000000ULL));
	zassert_not_equal(ant_event_rx_ticks((ant_time_t)0x100000000ULL),
			  ant_event_rx_ticks(0u));
}

/* ---------------------------------------------------------------------------
 * The extended tail, byte by byte
 * ---------------------------------------------------------------------------
 */

static struct ant_event_ext mk_ext(void)
{
	struct ant_event_ext ext;

	memset(&ext, 0, sizeof(ext));
	ext.id.device_number = (uint16_t)DEVNUM;
	/* Pairing bit set, so a test can see that it survives. */
	ext.id.device_type = (uint8_t)(DTYPE | ANT_DEVICE_TYPE_PAIRING_BIT);
	ext.id.trans_type = (uint8_t)TTYPE;
	ext.has_rssi = true;
	ext.rssi_dbm = FAKE_RADIO_RSSI_BENCH_DBM;   /* -17 */
	ext.has_t_sync = true;
	ext.t_sync_exact = true;
	ext.t_sync = 1000000u;                      /* exactly 0x8000 ticks */
	return ext;
}

ZTEST(ant_event, test_ext_all_three_fields_in_order)
{
	struct ant_event_ext ext = mk_ext();
	uint8_t              out[ANT_EVENT_EXT_MAX];
	int                  n;

	n = ant_event_build_ext((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
				out, sizeof(out));

	/* flag + 4 + 3 + 2 */
	zassert_equal(10, n, "0xE0 must produce ten bytes, got %d", n);

	zassert_equal(0xE0u, out[0], "flag byte");
	/* Channel id: device number little endian, then type, then trans. */
	zassert_equal(0x17u, out[1]);
	zassert_equal(0x3Au, out[2]);
	zassert_equal(0x8Bu, out[3], "the pairing bit must not be masked off");
	zassert_equal(0x05u, out[4]);
	/* RSSI: measurement type, value, threshold configuration. */
	zassert_equal((uint8_t)ANTW_RSSI_MEASUREMENT_TYPE_DBM, out[5]);
	zassert_equal(0xEFu, out[6], "-17 dBm as a signed byte");
	zassert_equal(0x00u, out[7]);
	/* Receive timestamp, little endian: 1,000,000 us is 0x8000 ticks. */
	zassert_equal(0x00u, out[8]);
	zassert_equal(0x80u, out[9]);
}

ZTEST(ant_event, test_ext_device_id_only)
{
	struct ant_event_ext ext = mk_ext();
	uint8_t              out[ANT_EVENT_EXT_MAX];
	int                  n;

	n = ant_event_build_ext(
		(uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID, &ext, out,
		sizeof(out));

	zassert_equal(5, n);
	zassert_equal((uint8_t)ANTW_EXT_FLAG_CHANNEL_ID, out[0]);
	zassert_equal(0x17u, out[1]);
	zassert_equal(0x3Au, out[2]);
}

/*
 * The decoder-shaped bug, made reproducible. Ask for RSSI and the timestamp
 * but not the channel id: every field must shift up by four, and the flag byte
 * must say so. A module that emitted a placeholder for the channel id, or that
 * wrote fields at fixed offsets, passes the 0xE0 test above and fails this
 * one - which is the same trap tools/ant_verify.py's extended_fields() is
 * written to avoid on the other side of the wire.
 */
ZTEST(ant_event, test_ext_offsets_move_with_the_flags)
{
	struct ant_event_ext ext = mk_ext();
	uint8_t              out[ANT_EVENT_EXT_MAX];
	int                  n;
	uint8_t              cfg;

	cfg = (uint8_t)(ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI |
			ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP);

	n = ant_event_build_ext(cfg, &ext, out, sizeof(out));

	zassert_equal(6, n);
	zassert_equal((uint8_t)(ANTW_EXT_FLAG_RSSI | ANTW_EXT_FLAG_RX_TIMESTAMP),
		      out[0]);
	zassert_equal((uint8_t)ANTW_RSSI_MEASUREMENT_TYPE_DBM, out[1],
		      "RSSI must start where the channel id would have been");
	zassert_equal(0xEFu, out[2]);
	zassert_equal(0x00u, out[3]);
	zassert_equal(0x00u, out[4]);
	zassert_equal(0x80u, out[5]);
}

ZTEST(ant_event, test_ext_absent_when_nothing_is_configured)
{
	struct ant_event_ext ext = mk_ext();
	uint8_t              out[ANT_EVENT_EXT_MAX];

	/* Not a flag byte of zero - no flag byte at all. That is what keeps a
	 * plain broadcast nine bytes long on the wire. */
	zassert_equal(0, ant_event_build_ext(0u, &ext, out, sizeof(out)));
}

/* ---------------------------------------------------------------------------
 * Degrading rather than lying
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_event, test_inexact_timestamp_downgrades_rather_than_lies)
{
	struct ant_event_ext ext = mk_ext();
	uint8_t              out[ANT_EVENT_EXT_MAX];
	int                  n;

	ext.t_sync_exact = false;

	n = ant_event_build_ext((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
				out, sizeof(out));

	/* The field is gone and the flag bit went with it: the host is told
	 * there is no timestamp, rather than handed a worse one in a field it
	 * would read as radio-clock accurate. */
	zassert_equal(8, n);
	zassert_equal((uint8_t)(ANTW_EXT_FLAG_CHANNEL_ID | ANTW_EXT_FLAG_RSSI),
		      out[0]);
	zassert_equal(0u, out[0] & (uint8_t)ANTW_EXT_FLAG_RX_TIMESTAMP);
	zassert_equal(1u, ant_event_stats_get()->ts_suppressed,
		      "a downgrade must be counted, not silent");

	/* A bench build may opt into the approximate value explicitly. */
	ant_event_set_ts_policy(ANT_EVENT_TS_BEST_EFFORT);
	n = ant_event_build_ext((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
				out, sizeof(out));
	zassert_equal(10, n);
	zassert_equal(0xE0u, out[0]);
	zassert_equal(1u, ant_event_stats_get()->ts_suppressed,
		      "the opt-in must not count as a downgrade");
}

ZTEST(ant_event, test_missing_rssi_downgrades_rather_than_reporting_zero)
{
	struct ant_event_ext ext = mk_ext();
	uint8_t              out[ANT_EVENT_EXT_MAX];
	int                  n;

	ext.has_rssi = false;

	n = ant_event_build_ext((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
				out, sizeof(out));

	/* 0 dBm is a physically implausible reading that a host would
	 * nonetheless believe, so the field is dropped instead. */
	zassert_equal(7, n);
	zassert_equal((uint8_t)(ANTW_EXT_FLAG_CHANNEL_ID |
				ANTW_EXT_FLAG_RX_TIMESTAMP),
		      out[0]);
	zassert_equal(1u, ant_event_stats_get()->rssi_suppressed);
}

/* ---------------------------------------------------------------------------
 * The test this module exists to pass
 * ---------------------------------------------------------------------------
 */

/*
 * The timestamp must be the instant the frame's address was at the antenna,
 * not whatever the clock says when the queue is drained. The mock's clock only
 * moves when a test moves it, so moving it by more than a second between the
 * two makes the difference unmissable: a drain-time reading would be about
 * 40,448 ticks further on, and a reading of the callback's own arrival time
 * would still be right, which is why the assertion is against the
 * transmitter's t_sync rather than against anything the receiver produced.
 */
ZTEST(ant_event, test_rx_timestamp_survives_the_clock_moving)
{
	ant_time_t t_air;
	uint16_t   reported;
	uint16_t   at_drain;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_set(
			      (uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));

	t_air = run_one_rx(false, 0u);
	zassert_equal(ANT_EVENT_OK, cb_rc);
	zassert_equal(1u, ant_event_queued());
	zassert_equal(1u, n_wakeups, "a queued message must wake the drain");
	zassert_equal(0u, n_recs, "nothing may be delivered from the ISR");

	/* Between arrival and drain, a great deal of time. */
	fake_radio_advance(1234567u);
	at_drain = ant_event_rx_ticks(ant_radio_now());

	zassert_equal(1u, ant_event_drain(0u));
	zassert_equal(1u, n_recs);

	zassert_equal((uint8_t)ANTW_MESG_BROADCAST_DATA_ID, recs[0].id);
	/* channel + 8 payload + flag + 4 + 3 + 2 */
	zassert_equal(19u, recs[0].len);
	zassert_equal(0xE0u, recs[0].data[FLAG_OFFSET]);

	reported = (uint16_t)(recs[0].data[17] |
			      ((uint16_t)recs[0].data[18] << 8));

	zassert_equal(ant_event_rx_ticks(t_air), reported,
		      "the reported timestamp is not the frame's t_sync");
	zassert_equal(ant_event_rx_ticks(cb_t_sync), reported,
		      "the reported timestamp is not the one the HAL gave");
	zassert_not_equal(at_drain, reported,
			  "the timestamp moved with the clock - it was read at "
			  "drain time instead of taken from t_sync");
}

/*
 * The body is a DMA buffer the backend reuses the instant the callback
 * returns; the mock overwrites it with 0xDD to make that reproducible. So the
 * payload delivered after the drain must be the eight bytes that were on the
 * air, not eight copies of the poison.
 */
ZTEST(ant_event, test_body_is_copied_in_the_callback_not_retained)
{
	uint8_t i;

	(void)run_one_rx(false, 0u);
	zassert_equal(ANT_EVENT_OK, cb_rc);

	fake_radio_advance(50000u);
	zassert_equal(1u, ant_event_drain(0u));
	zassert_equal(1u, n_recs);

	zassert_equal(9u, recs[0].len, "no lib config, so no extended tail");
	zassert_equal(0u, recs[0].data[0], "channel byte");
	for (i = 0u; i < (uint8_t)ANT_FRAME_PAYLOAD_STD; i++) {
		zassert_equal(payload8[i], recs[0].data[1u + i],
			      "payload byte %u is 0x%02X, not 0x%02X - the "
			      "body was retained rather than copied",
			      i, recs[0].data[1u + i], payload8[i]);
		zassert_not_equal(FAKE_RADIO_BODY_POISON, recs[0].data[1u + i]);
	}
}

/* A CRC-failed frame must never become an event. It is refused at the last
 * gate before the host as well as by whatever decided to offer it. */
ZTEST(ant_event, test_crc_failed_frame_produces_no_event)
{
	(void)run_one_rx(true, ANT_RX_REPORT_CRC_FAIL);

	zassert_equal(ANT_EVENT_ECRC, cb_rc,
		      "a CRC_FAIL event must be refused, not queued");
	zassert_equal(0u, ant_event_queued());
	zassert_false(ant_event_pending());
	zassert_equal(0u, ant_event_drain(0u));
	zassert_equal(0u, n_recs);
	zassert_equal(0u, n_wakeups, "a refusal must not wake the drain");
	zassert_equal(1u, ant_event_stats_get()->rejected_crc);
	zassert_equal(0u, ant_event_stats_get()->posted);
}

/* The same refusal for a synthetic event, and for the statuses that carry no
 * frame at all - so that a caller which forwards a terminal event by mistake
 * cannot manufacture a message out of a zeroed timestamp and a NULL body. */
ZTEST(ant_event, test_non_ok_statuses_are_all_refused)
{
	struct ant_rx_event  hal;
	struct ant_event_rx  rx;

	hal = mk_hal(ANT_RADIO_STATUS_CRC_FAIL, 1000000u, true, true, -50);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(ANT_EVENT_ECRC, ant_event_post_rx(&rx));

	hal = mk_hal(ANT_RADIO_STATUS_TIMEOUT, 0u, false, false, 0);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	hal = mk_hal(ANT_RADIO_STATUS_ABORTED, 0u, false, false, 0);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	hal = mk_hal(ANT_RADIO_STATUS_FAILED, 0u, false, false, 0);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	zassert_equal(0u, ant_event_queued());
	zassert_equal(0u, ant_event_stats_get()->posted);
}

/*
 * A backend that cannot capture the address event clears t_sync_exact on every
 * event, and the whole path - not just ant_event_build_ext() - has to degrade.
 */
ZTEST(ant_event, test_inexact_backend_degrades_the_whole_path)
{
	fake_radio_caps_mut()->has_sync_timestamp = false;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_set(
			      (uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));

	(void)run_one_rx(false, 0u);
	zassert_equal(ANT_EVENT_OK, cb_rc);

	zassert_equal(1u, ant_event_drain(0u));
	/* channel + 8 payload + flag + 4 + 3, and no timestamp. */
	zassert_equal(17u, recs[0].len);
	zassert_equal((uint8_t)(ANTW_EXT_FLAG_CHANNEL_ID | ANTW_EXT_FLAG_RSSI),
		      recs[0].data[FLAG_OFFSET]);
	zassert_equal(1u, ant_event_stats_get()->ts_suppressed);
}

/* ---------------------------------------------------------------------------
 * The queue at 32 channels
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_event, test_thirty_two_channels_neither_overflow_nor_reorder)
{
	struct ant_rx_event hal;
	struct ant_event_rx rx;
	uint8_t             payload[ANT_FRAME_PAYLOAD_STD];
	uint8_t             ch;
	uint32_t            i;

	zassert_true(ANT_EVENT_CHANNEL_MAX >= 31u);

	memcpy(payload, payload8, sizeof(payload));

	for (ch = 0u; ch <= 31u; ch++) {
		/* A distinct t_sync and a distinct first payload byte per
		 * channel, so a reordering is visible in two independent
		 * ways. */
		hal = mk_hal(ANT_RADIO_STATUS_OK,
			     (ant_time_t)1000000u + (ant_time_t)ch, true, true,
			     (int8_t)(-40 - (int8_t)ch));
		payload[0] = ch;
		rx = mk_rx(&hal, ch, payload);
		zassert_equal(ANT_EVENT_OK, ant_event_post_rx(&rx),
			      "channel %u was refused", ch);
	}

	zassert_equal(32u, ant_event_queued());
	zassert_equal(32u, n_wakeups);
	zassert_equal(0u, ant_event_stats_get()->dropped_full);

	zassert_equal(32u, ant_event_drain(0u));
	zassert_equal(32u, n_recs);
	zassert_false(ant_event_pending());

	for (i = 0u; i < 32u; i++) {
		zassert_equal((uint8_t)ANTW_MESG_BROADCAST_DATA_ID,
			      recs[i].id);
		zassert_equal(9u, recs[i].len);
		zassert_equal((uint8_t)i, recs[i].data[0],
			      "message %u came out of order", i);
		zassert_equal((uint8_t)i, recs[i].data[1]);
	}

	zassert_equal(32u, ant_event_stats_get()->delivered);
	zassert_equal(32u, ant_event_stats_get()->high_water);
}

/*
 * A full ring drops the newest message - the queued ones are already in order
 * and losing the head of a burst is worse than losing its tail - and says so
 * with ANTW_EVENT_QUE_OVERFLOW. Never a silent loss: "unexplained loss must be
 * 0" is one of the A/B gates, and a message that vanished without an event is
 * exactly what makes that figure meaningless.
 */
ZTEST(ant_event, test_overflow_is_reported_never_silent)
{
	struct ant_rx_event hal;
	struct ant_event_rx rx;
	uint8_t             payload[ANT_FRAME_PAYLOAD_STD];
	uint32_t            i;

	memcpy(payload, payload8, sizeof(payload));
	hal = mk_hal(ANT_RADIO_STATUS_OK, 1000000u, true, true, -40);

	for (i = 0u; i < (uint32_t)ANT_EVENT_QUEUE_DEPTH; i++) {
		payload[0] = (uint8_t)i;
		rx = mk_rx(&hal, (uint8_t)(i & ANT_EVENT_CHANNEL_MAX), payload);
		zassert_equal(ANT_EVENT_OK, ant_event_post_rx(&rx));
	}

	for (i = 0u; i < 3u; i++) {
		payload[0] = 0xFFu;
		rx = mk_rx(&hal, 0u, payload);
		zassert_equal(ANT_EVENT_ENOSPC, ant_event_post_rx(&rx));
	}

	zassert_equal(3u, ant_event_stats_get()->dropped_full);
	zassert_equal((uint32_t)ANT_EVENT_QUEUE_DEPTH, n_wakeups,
		      "a dropped message must not wake the drain");
	zassert_true(ant_event_pending());

	zassert_equal((uint32_t)ANT_EVENT_QUEUE_DEPTH + 1u,
		      ant_event_drain(0u));

	/* The marker comes first and is a channel event on channel 0. */
	zassert_equal((uint8_t)ANTW_MESG_RESPONSE_EVENT_ID, recs[0].id);
	zassert_equal(3u, recs[0].len);
	zassert_equal(0u, recs[0].data[0]);
	zassert_equal((uint8_t)ANTW_MESG_EVENT_ID, recs[0].data[1]);
	zassert_equal((uint8_t)ANTW_EVENT_QUE_OVERFLOW, recs[0].data[2]);
	zassert_equal(1u, ant_event_stats_get()->overflow_marks);

	/* Everything that was accepted still came out in order, and the three
	 * that were refused are not among them. */
	for (i = 0u; i < (uint32_t)ANT_EVENT_QUEUE_DEPTH; i++) {
		zassert_equal((uint8_t)i, recs[i + 1u].data[1],
		              "queued message %u is missing or out of order", i);
	}

	zassert_false(ant_event_pending());
}

/* ---------------------------------------------------------------------------
 * Channel events, including the three that release the bridge's burst block
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_event, test_channel_event_shape)
{
	zassert_equal(ANT_EVENT_OK,
		      ant_event_post_channel_event(
			      7u, (uint8_t)ANTW_EVENT_CHANNEL_CLOSED));
	zassert_equal(1u, ant_event_drain(0u));

	/* [channel][MESG_EVENT_ID][code] under MESG_RESPONSE_EVENT_ID. Byte 1
	 * being 0x01 is what tells the bridge this is an unsolicited event and
	 * not a reply to a command. */
	zassert_equal((uint8_t)ANTW_MESG_RESPONSE_EVENT_ID, recs[0].id);
	zassert_equal((uint8_t)ANTW_MESG_RESPONSE_EVENT_SIZE, recs[0].len);
	zassert_equal(7u, recs[0].data[0]);
	zassert_equal((uint8_t)ANTW_MESG_EVENT_ID, recs[0].data[1]);
	zassert_equal((uint8_t)ANTW_EVENT_CHANNEL_CLOSED, recs[0].data[2]);

	/* A channel event carries no extended fields whatever the library
	 * configuration says - they belong to received data only. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_set(
			      (uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));
	zassert_equal(ANT_EVENT_OK,
		      ant_event_post_channel_event(
			      7u, (uint8_t)ANTW_EVENT_RX_FAIL));
	zassert_equal(1u, ant_event_drain(0u));
	zassert_equal(3u, recs[1].len);
}

/*
 * The three release codes. If NEXT_DATA_BLOCK is not raised exactly once per
 * accepted block the bridge's k_sem_take() waits its full 1000 ms and then
 * answers ANTW_TRANSFER_IN_PROGRESS, so a burst stalls a second a packet and
 * reports a plausible-looking error. Counting them here is what lets
 * ant_burst.c's own suite assert release-count == accepted-block-count against
 * a number instead of a log.
 */
ZTEST(ant_event, test_burst_release_codes_are_the_wire_values)
{
	const struct ant_event_stats *st;

	zassert_equal(ANT_EVENT_OK, ant_event_post_transfer_next_block(4u));
	zassert_equal(ANT_EVENT_OK, ant_event_post_transfer_next_block(4u));
	zassert_equal(ANT_EVENT_OK, ant_event_post_transfer_tx_completed(4u));
	zassert_equal(ANT_EVENT_OK, ant_event_post_transfer_tx_failed(5u));

	zassert_equal(4u, ant_event_drain(0u));

	zassert_equal((uint8_t)ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK,
		      recs[0].data[2]);
	zassert_equal((uint8_t)ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK,
		      recs[1].data[2]);
	zassert_equal((uint8_t)ANTW_EVENT_TRANSFER_TX_COMPLETED,
		      recs[2].data[2]);
	zassert_equal((uint8_t)ANTW_EVENT_TRANSFER_TX_FAILED, recs[3].data[2]);
	zassert_equal(5u, recs[3].data[0]);

	st = ant_event_stats_get();
	zassert_equal(2u, st->next_data_block);
	zassert_equal(1u, st->tx_completed);
	zassert_equal(1u, st->tx_failed);
}

ZTEST(ant_event, test_burst_data_uses_the_serial_burst_header)
{
	struct ant_rx_event hal;
	struct ant_event_rx rx;

	hal = mk_hal(ANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 9u, payload8);
	rx.msg_id = (uint8_t)ANTW_MESG_BURST_DATA_ID;
	rx.burst_seq = 2u;
	rx.burst_last = true;

	zassert_equal(ANT_EVENT_OK, ant_event_post_rx(&rx));
	zassert_equal(1u, ant_event_drain(0u));

	/* [last<<7 | seq<<5 | channel] = 0x80 | 0x40 | 0x09. */
	zassert_equal((uint8_t)ANTW_MESG_BURST_DATA_ID, recs[0].id);
	zassert_equal(0xC9u, recs[0].data[0]);

	/* A sequence number wider than the header's two bits is a caller bug,
	 * not something to truncate into the channel field. */
	rx.burst_seq = 4u;
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));
}

/* ---------------------------------------------------------------------------
 * Library configuration and the reset path
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_event, test_lib_config_is_additive_and_refuses_unknown_bits)
{
	uint8_t cfg = 0xFFu;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_get(&cfg));
	zassert_equal(0u, cfg, "library configuration starts cleared");

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_set(
			      (uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_set(
			      (uint8_t)ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_get(&cfg));
	zassert_equal(0xC0u, cfg, "setting one bit must not clear another");

	/* A bit this module does not implement is refused outright, and
	 * nothing is applied - a half-applied configuration is worse than an
	 * error, because the host cannot tell which half took. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      ant_event_lib_config_set(0x02u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_get(&cfg));
	zassert_equal(0xC0u, cfg);

	/* 0xE0 - what every tool asks for - must be accepted whole. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_set(
			      (uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_get(&cfg));
	zassert_equal(0xE0u, cfg);

	/* The clear path takes the mask the bridge sends for "set it to
	 * zero"; there is no separate clear message on the wire. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_clear(
			      (uint8_t)ANTW_LIB_CONFIG_MASK_ALL));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_get(&cfg));
	zassert_equal(0u, cfg);
}

ZTEST(ant_event, test_event_filter_round_trips_and_filters_nothing)
{
	struct ant_rx_event hal;
	struct ant_event_rx rx;
	uint16_t            f = 0u;

	/* tools/ant_features.py round-trips this, so it must be stored... */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_filter_set(0xFFFFu));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_filter_get(&f));
	zassert_equal(0xFFFFu, f);

	/* ...and it must not actually suppress anything, because loss
	 * accounting depends on RX_FAIL events arriving and "unexplained loss
	 * must be 0" is an A/B gate. */
	zassert_equal(ANT_EVENT_OK,
		      ant_event_post_channel_event(
			      0u, (uint8_t)ANTW_EVENT_RX_FAIL));
	hal = mk_hal(ANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(ANT_EVENT_OK, ant_event_post_rx(&rx));

	zassert_equal(2u, ant_event_drain(0u));
}

/*
 * antr_stack_reset() discards rather than drains. Draining would call
 * antr_on_message() recursively from inside an antr_* call, which
 * docs/sdk-ant-contract.md forbids, and a real stick emits nothing between the
 * reset command and the startup message anyway.
 */
ZTEST(ant_event, test_flush_discards_without_delivering)
{
	struct ant_rx_event hal;
	struct ant_event_rx rx;
	uint8_t             cfg = 0xFFu;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_set(
			      (uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));

	hal = mk_hal(ANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(ANT_EVENT_OK, ant_event_post_rx(&rx));
	zassert_equal(ANT_EVENT_OK, ant_event_post_rx(&rx));

	ant_event_flush();

	zassert_equal(0u, ant_event_queued());
	zassert_false(ant_event_pending());
	zassert_equal(0u, ant_event_drain(0u));
	zassert_equal(0u, n_recs, "a flush must deliver nothing");
	zassert_equal(2u, ant_event_stats_get()->dropped_flush);

	/* Library configuration goes back to zero with it. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_get(&cfg));
	zassert_equal(0u, cfg);
}

ZTEST(ant_event, test_drain_honours_its_bound)
{
	struct ant_rx_event hal;
	struct ant_event_rx rx;
	uint8_t             payload[ANT_FRAME_PAYLOAD_STD];
	uint8_t             i;

	memcpy(payload, payload8, sizeof(payload));
	hal = mk_hal(ANT_RADIO_STATUS_OK, 1000000u, true, true, -40);

	for (i = 0u; i < 5u; i++) {
		payload[0] = i;
		rx = mk_rx(&hal, i, payload);
		zassert_equal(ANT_EVENT_OK, ant_event_post_rx(&rx));
	}

	zassert_equal(2u, ant_event_drain(2u));
	zassert_equal(3u, ant_event_queued());
	zassert_equal(0u, recs[0].data[0]);
	zassert_equal(1u, recs[1].data[0]);

	zassert_equal(3u, ant_event_drain(0u));
	zassert_equal(2u, recs[2].data[0]);
	zassert_equal(4u, recs[4].data[0]);
	zassert_false(ant_event_pending());
}

/*
 * antr_init() must leave nothing behind from a previous run of the stack. A
 * message surviving a re-init would reach the host between the reset command
 * and the startup message, which is the one window a real stick is silent in.
 */
ZTEST(ant_event, test_init_empties_the_ring)
{
	struct ant_rx_event hal;
	struct ant_event_rx rx;
	uint8_t             cfg = 0xFFu;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_set(
			      (uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));

	hal = mk_hal(ANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(ANT_EVENT_OK, ant_event_post_rx(&rx));
	zassert_equal(1u, ant_event_queued());

	ant_event_init();

	zassert_equal(0u, ant_event_queued());
	zassert_false(ant_event_pending());
	zassert_equal(0u, ant_event_drain(0u));
	zassert_equal(0u, n_recs);
	zassert_equal(0u, ant_event_stats_get()->posted,
		      "init clears the statistics as well as the ring");
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      ant_event_lib_config_get(&cfg));
	zassert_equal(0u, cfg);
}

/* ---------------------------------------------------------------------------
 * Argument checking
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_event, test_bad_arguments_are_refused_not_truncated)
{
	struct ant_rx_event  hal;
	struct ant_event_rx  rx;
	struct ant_event_ext ext = mk_ext();
	uint8_t              out[ANT_EVENT_EXT_MAX];

	hal = mk_hal(ANT_RADIO_STATUS_OK, 1000000u, true, true, -40);

	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(NULL));

	rx = mk_rx(&hal, 0u, payload8);
	rx.hal = NULL;
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	rx = mk_rx(&hal, 0u, NULL);
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	/* One past the highest channel the burst header can name. */
	rx = mk_rx(&hal, (uint8_t)(ANT_EVENT_CHANNEL_MAX + 1u), payload8);
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	rx = mk_rx(&hal, 0u, payload8);
	rx.payload_len = 0u;
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	rx = mk_rx(&hal, 0u, payload8);
	rx.payload_len = (uint8_t)(ANT_EVENT_PAYLOAD_MAX + 1u);
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	/* Only the three received-data message IDs are data messages. */
	rx = mk_rx(&hal, 0u, payload8);
	rx.msg_id = (uint8_t)ANTW_MESG_RESPONSE_EVENT_ID;
	zassert_equal(ANT_EVENT_EINVAL, ant_event_post_rx(&rx));

	zassert_equal(ANT_EVENT_EINVAL,
		      ant_event_post_channel_event(
			      (uint8_t)(ANT_EVENT_CHANNEL_MAX + 1u), 0x07u));

	zassert_equal(ANT_EVENT_EINVAL,
		      ant_event_post_raw(0x4Eu, payload8,
					 (uint8_t)(ANT_EVENT_BODY_MAX + 1u)));

	/* A short output buffer is refused rather than filled with a
	 * truncated tail, which would be a valid-looking message with the
	 * wrong fields in it. */
	zassert_equal(ANT_EVENT_EINVAL,
		      ant_event_build_ext(
			      (uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
			      out, ANT_EVENT_EXT_MAX - 1u));
	zassert_equal(ANT_EVENT_EINVAL,
		      ant_event_build_ext(
			      (uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS, NULL,
			      out, sizeof(out)));

	zassert_equal(0u, ant_event_queued());
	zassert_equal(0u, n_recs);
}
