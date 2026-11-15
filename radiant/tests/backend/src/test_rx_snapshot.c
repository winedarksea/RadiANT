/* SPDX-License-Identifier: Apache-2.0 */
/*
 * THE LATE-ISR GUARD in the nRF END branch (plan B2). END->START keeps the
 * receiver running, so a late END interrupt can find the next frame's ADDRESS
 * capture, bytes and CRC registers in place of its own. The branch snapshots
 * the buffer and registers between two CC_SYNC reads, drops a torn snapshot
 * without an event, and carries the next frame's ADDRESS so that frame is not
 * discarded at its own END.
 *
 * The TIMER model counts only on a capture (fakes/nrfx.h): the END branch's one
 * capture before the torn test is its own `now`, so a frame staged with
 * backend_frame_at(since) is judged at since + 1 us after its capture. The
 * CC_SYNC reads are numbered: 1 before the snapshot, 2 after the copy, 3 after
 * the ADDRESS clear. A hook on read n stages what lands at that point.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <nrfx.h>
#include <radiant/radiant_radio_hal.h>

#include "backend_probe.h"

/* backend_arm_rx(): 8-byte body, no software address bytes. */
#define BODY_LEN 8u
#define AIR_US   ((0u + BODY_LEN + 2u) * 8u - 2u)

static const uint8_t frame_n[BODY_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint8_t frame_n1[BODY_LEN] = { 0xE1, 0xE2, 0xE3, 0xE4,
					    0xE5, 0xE6, 0xE7, 0xE8 };

static void load(const uint8_t *bytes)
{
	memcpy(backend_rx_dma_buf(), bytes, BODY_LEN);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	backend_reset();
	backend_arm_rx(false);
}

ZTEST_SUITE(radiant_backend_rx_snapshot, NULL, NULL, before, NULL, NULL);

ZTEST(radiant_backend_rx_snapshot, test_airtime_is_body_and_crc_less_slack)
{
	zassert_equal(AIR_US, backend_torn_air_us(), NULL);
	/* Software address bytes are on the air after the capture too. */
	backend_set_sw_addr_len(2u);
	zassert_equal(AIR_US + 16u, backend_torn_air_us(), NULL);
}

ZTEST(radiant_backend_rx_snapshot, test_on_time_frame_is_delivered_from_the_snapshot)
{
	load(frame_n);
	backend_frame(true);
	backend_dispatch();

	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_mem_equal(frame_n, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_false(backend_addr_carry(), "an on-time frame carried an ADDRESS");
}

/* The boundary: since + 1 us at or past the airtime is the frame's own END. */
ZTEST(radiant_backend_rx_snapshot, test_airtime_boundary)
{
	load(frame_n);
	backend_frame_at(true, AIR_US - 1u);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events,
		      "a frame judged exactly one airtime after its capture was "
		      "dropped as torn");

	backend_reset();
	backend_arm_rx(false);
	backend_frame_at(true, AIR_US - 2u);
	backend_dispatch();
	zassert_equal(0u, backend_probe()->rx_events,
		      "a capture younger than the airtime was taken for the "
		      "frame whose END this is");
}

/*
 * Case (a): N+1's ADDRESS landed before the interrupt ran, and N+1 is still on
 * the air. N is dropped, the window counts as having heard something, and N+1
 * is delivered at its own END although EVENTS_ADDRESS was consumed.
 */
ZTEST(radiant_backend_rx_snapshot, test_late_isr_with_next_frame_on_air_is_torn)
{
	load(frame_n);
	backend_frame_at(true, 10u);
	backend_dispatch();

	zassert_equal(0u, backend_probe()->rx_events,
		      "a torn snapshot was delivered");
	zassert_true(backend_rx_any(), "a torn window would give a noise sample");
	zassert_true(backend_addr_carry(), "N+1's ADDRESS was not carried");

	load(frame_n1);
	backend_end_only(200u);
	backend_dispatch();

	zassert_equal(1u, backend_probe()->rx_events,
		      "N+1 was discarded as \"not a reception\" at its own END");
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_false(backend_addr_carry(), "the carry outlived the END it covers");
}

/* Case (b): N+1's ADDRESS lands during the copy, so the two reads differ. */
static void hook_new_capture_on_read_2(uint32_t n)
{
	if (n == 2u) {
		fake_hw.cc[0] = fake_hw.counter;
		memcpy(backend_rx_dma_buf(), frame_n1, 3u);
	}
}

ZTEST(radiant_backend_rx_snapshot, test_address_during_the_copy_is_torn)
{
	load(frame_n);
	fake_hw.cc0_hook = hook_new_capture_on_read_2;
	backend_frame(true);
	fake_hw.cc0_hook = NULL;
	backend_dispatch();

	zassert_equal(0u, backend_probe()->rx_events, NULL);
	zassert_true(backend_addr_carry(), NULL);

	load(frame_n1);
	backend_end_only(200u);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
}

/*
 * Case (c): N+1's ADDRESS lands after the copy, before the clear wipes it. N
 * is delivered from the snapshot although N+1's bytes are already landing in
 * the DMA buffer and N+1 has moved RXCRC, and N+1 is still delivered after.
 */
static void hook_address_before_clear(uint32_t n)
{
	if (n == 3u) {
		fake_hw.cc[0] = fake_hw.counter;
		load(frame_n1);
		NRF_RADIO->RXCRC = 0xBEEFu;
	}
}

ZTEST(radiant_backend_rx_snapshot, test_address_before_the_clear_is_carried)
{
	backend_set_report_crc_fail();
	NRF_RADIO->RXCRC = 0x1234u;
	load(frame_n);
	fake_hw.cc0_hook = hook_address_before_clear;
	backend_frame(false);
	fake_hw.cc0_hook = NULL;
	backend_dispatch();

	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_equal(RADIANT_RADIO_STATUS_CRC_FAIL,
		      backend_probe()->last_rx_status, NULL);
	zassert_mem_equal(frame_n, backend_probe()->last_rx_body, BODY_LEN,
			  "the event read the live buffer after the snapshot");
	zassert_equal(0x1234u, backend_probe()->last_rx_crc_rx,
		      "crc_rx read the live RXCRC after the snapshot");
	zassert_true(backend_addr_carry(),
		     "the clear wiped N+1's ADDRESS and nothing carried it");

	fake_hw.crc_ok = true;
	backend_end_only(200u);
	backend_dispatch();
	zassert_equal(2u, backend_probe()->rx_events,
		      "N+1 was discarded as \"not a reception\" at its own END");
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
}

/*
 * Case (a), N+1 ended between the snapshot's clock read and the END check
 * after it: the first pass is torn (N+1 was on the air at that read), END is
 * raised again, so the snapshot is retaken from N+1's END and N+1 is delivered
 * once, with nothing left pending. N+1's END lands just after capture 1, the
 * first pass's `now`.
 */
static void hook_next_frame_ends_after_now(uint32_t n)
{
	if (n == 1u) {
		fake_hw.counter += 200u;
		load(frame_n1);
		fake_hw.event[NRF_RADIO_EVENT_END] = true;
	}
}

ZTEST(radiant_backend_rx_snapshot, test_next_frame_ending_during_the_snapshot_is_retaken)
{
	load(frame_n);
	fake_hw.cc3_hook = hook_next_frame_ends_after_now;
	backend_frame_at(true, 10u);
	fake_hw.cc3_hook = NULL;
	backend_dispatch();

	/* Two snapshot clock reads, then clock_absolute() for the delivered t_sync. */
	zassert_equal(3u, fake_hw.cc3_captures, "the snapshot was not retaken");
	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_false(fake_hw.event[NRF_RADIO_EVENT_END], NULL);
	zassert_false(backend_addr_carry(), NULL);
}

/*
 * The same raised END after a snapshot that passed the time test (the residual
 * band: the capture was N+1's, inside its last RX_TORN_SLACK_US). The registers
 * may be N's, so the snapshot is retaken and the frame delivered once, from
 * the retake.
 */
static void hook_end_raised_on_read_2(uint32_t n)
{
	if (n == 2u) {
		load(frame_n1);
		fake_hw.event[NRF_RADIO_EVENT_END] = true;
	}
}

ZTEST(radiant_backend_rx_snapshot, test_end_raised_after_a_good_snapshot_delivers_once)
{
	load(frame_n);
	fake_hw.cc0_hook = hook_end_raised_on_read_2;
	backend_frame(true);
	fake_hw.cc0_hook = NULL;
	backend_dispatch();

	/* Two snapshot clock reads, then clock_absolute() for the delivered t_sync. */
	zassert_equal(3u, fake_hw.cc3_captures, "the snapshot was not retaken");
	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_false(backend_addr_carry(), NULL);
}

/* The old guard still holds: an END with no ADDRESS and no carry is not a reception. */
ZTEST(radiant_backend_rx_snapshot, test_end_without_address_or_carry_is_not_a_reception)
{
	load(frame_n);
	backend_frame(true);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events, NULL);

	backend_end_only(200u);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events,
		      "an END with no address match replayed the last frame");
}
