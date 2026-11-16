/* SPDX-License-Identifier: Apache-2.0 */
/*
 * THE LATE-ISR GUARD in the nRF END branch (plan B2). END->START keeps the
 * receiver running, so a late END interrupt can find the next frame's ADDRESS
 * capture, bytes and CRC registers in place of its own. The branch clears
 * EVENTS_ADDRESS, snapshots the buffer and registers between two CC_SYNC reads,
 * drops a torn snapshot without an event (0xF7 page 10 field 4), and carries
 * the next frame's ADDRESS so that frame is not discarded at its own END.
 *
 * The TIMER model counts only on a capture (fakes/nrfx.h). The END branch's
 * first capture is its own clock read after the snapshot, so a frame staged
 * with backend_frame_at(since) is judged at since + 1 us after its capture;
 * in the settle band each further capture is one more microsecond. CC_SYNC
 * reads are numbered: 1 before the snapshot, 2 after the copy. A hook on read
 * n, or on capture n, stages what lands at that point.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <nrfx.h>
#include <radiant/radiant_radio_hal.h>

#include "backend_probe.h"

/* backend_arm_rx(): 8-byte body, no software address bytes. */
#define BODY_LEN 8u
#define TRUE_AIR ((0u + BODY_LEN + 2u) * 8u)
/* The time test's threshold (RX_TORN_SLACK_US = 2) and the settle point (RX_TORN_SETTLE_US = 2). */
#define AIR_US   (TRUE_AIR - 2u)
#define SETTLED  (TRUE_AIR + 2u)

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
	zassert_false(fake_hw.event[NRF_RADIO_EVENT_ADDRESS], NULL);
	zassert_equal(0u, backend_late_isr_drops(), NULL);
}

/*
 * The boundary: since + 1 us at or past the threshold is not torn. That
 * capture is inside the settle band, so the branch waits to the settle point
 * (one capture per microsecond in the model) and, with no END raised,
 * delivers.
 */
ZTEST(radiant_backend_rx_snapshot, test_airtime_boundary)
{
	load(frame_n);
	backend_frame_at(true, AIR_US - 1u);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events,
		      "a frame judged exactly at the threshold was dropped as torn");
	/* Clock read at AIR_US, waited to SETTLED, then t_sync's clock read. */
	zassert_equal(1u + (SETTLED - AIR_US) + 1u, fake_hw.cc3_captures,
		      "the settle wait did not run to the settle point");
	zassert_equal(0u, backend_late_isr_drops(), NULL);

	backend_reset();
	backend_arm_rx(false);
	backend_frame_at(true, AIR_US - 2u);
	backend_dispatch();
	zassert_equal(0u, backend_probe()->rx_events,
		      "a capture younger than the airtime was taken for the "
		      "frame whose END this is");
	zassert_equal(1u, backend_late_isr_drops(), NULL);
}

/*
 * Case (a): N+1's ADDRESS landed before the interrupt ran (its flag is wiped
 * by the clear), and N+1 is still on the air. N is dropped and counted, the
 * window counts as having heard something, and N+1 is delivered at its own END
 * through the carry.
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
	zassert_equal(1u, backend_late_isr_drops(), NULL);

	load(frame_n1);
	backend_end_only(200u);
	backend_dispatch();

	zassert_equal(1u, backend_probe()->rx_events,
		      "N+1 was discarded as \"not a reception\" at its own END");
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_false(backend_addr_carry(), "the carry outlived the END it covers");
}

/*
 * Case (b): N+1's ADDRESS lands between the clear and read 1. Its flag stays
 * latched and read 1 holds its fresh capture: torn.
 */
static void hook_address_before_read_1(uint32_t n)
{
	if (n == 1u) {
		fake_hw.cc[0] = fake_hw.counter;
		fake_hw.event[NRF_RADIO_EVENT_ADDRESS] = true;
	}
}

ZTEST(radiant_backend_rx_snapshot, test_address_before_read_1_is_torn_and_latched)
{
	load(frame_n);
	fake_hw.cc0_hook = hook_address_before_read_1;
	backend_frame(true);
	fake_hw.cc0_hook = NULL;
	backend_dispatch();

	zassert_equal(0u, backend_probe()->rx_events, NULL);
	zassert_true(fake_hw.event[NRF_RADIO_EVENT_ADDRESS],
		     "the ADDRESS that landed after the clear was consumed");
	zassert_equal(1u, backend_late_isr_drops(), NULL);

	load(frame_n1);
	backend_end_only(200u);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_false(backend_addr_carry(), NULL);
	zassert_false(fake_hw.event[NRF_RADIO_EVENT_ADDRESS], NULL);
}

/* Case (c): N+1's ADDRESS lands during the copy, so the two reads differ. */
static void hook_new_capture_on_read_2(uint32_t n)
{
	if (n == 2u) {
		fake_hw.cc[0] = fake_hw.counter;
		fake_hw.event[NRF_RADIO_EVENT_ADDRESS] = true;
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
	zassert_equal(1u, backend_late_isr_drops(), NULL);

	load(frame_n1);
	backend_end_only(200u);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
}

/*
 * Case (d): N+1's ADDRESS lands after read 2, as the clock is read. N is
 * delivered from the snapshot although N+1 has moved CC_SYNC, the DMA buffer,
 * CRCSTATUS, RXCRC, RXMATCH and RSSISAMPLE: a re-read of any of them after the
 * snapshot changes the event. N+1's ADDRESS stays latched, so it is delivered
 * after, with its own registers.
 */
static void hook_everything_moves_after_read_2(uint32_t n)
{
	if (n == 1u) {
		fake_hw.cc[0] = fake_hw.counter;
		fake_hw.event[NRF_RADIO_EVENT_ADDRESS] = true;
		load(frame_n1);
		fake_hw.crc_ok = true;
		NRF_RADIO->RXCRC = 0xBEEFu;
		NRF_RADIO->RXMATCH = 3u;
		NRF_RADIO->RSSISAMPLE = 70u;
	}
}

ZTEST(radiant_backend_rx_snapshot, test_address_after_read_2_leaves_the_snapshot_whole)
{
	backend_two_filters();
	backend_set_report_crc_fail();
	NRF_RADIO->RXCRC = 0x1234u;
	NRF_RADIO->RXMATCH = 0u;
	NRF_RADIO->RSSISAMPLE = 40u;
	load(frame_n);
	fake_hw.cc3_hook = hook_everything_moves_after_read_2;
	backend_frame(false);
	fake_hw.cc3_hook = NULL;
	backend_dispatch();

	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_equal(RADIANT_RADIO_STATUS_CRC_FAIL,
		      backend_probe()->last_rx_status,
		      "CRCSTATUS was read after the snapshot");
	zassert_mem_equal(frame_n, backend_probe()->last_rx_body, BODY_LEN,
			  "the event read the live buffer after the snapshot");
	zassert_equal(0x1234u, backend_probe()->last_rx_crc_rx,
		      "crc_rx read the live RXCRC after the snapshot");
	zassert_equal(0u, backend_probe()->last_rx_filter_index,
		      "the filter came from a live RXMATCH");
	zassert_equal(-40, backend_probe()->last_rx_rssi_dbm,
		      "rssi_dbm came from a live RSSISAMPLE");
	zassert_false(backend_addr_carry(), "a latched ADDRESS was carried too");
	zassert_true(fake_hw.event[NRF_RADIO_EVENT_ADDRESS],
		     "N+1's ADDRESS was cleared after the snapshot");
	zassert_equal(0u, backend_late_isr_drops(), NULL);

	backend_end_only(200u);
	backend_dispatch();
	zassert_equal(2u, backend_probe()->rx_events,
		      "N+1 was discarded as \"not a reception\" at its own END");
	zassert_equal(RADIANT_RADIO_STATUS_OK, backend_probe()->last_rx_status, NULL);
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_equal(1u, backend_probe()->last_rx_filter_index, NULL);
}

/*
 * Case (a), N+1 ended between the snapshot's clock read and the END check:
 * the first pass is torn (N+1 was on the air at that read), END is raised
 * again, so the snapshot is retaken from N+1's END and N+1 is delivered once,
 * with nothing left pending. N+1's END lands just after capture 1.
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
	zassert_equal(1u, backend_late_isr_drops(), "N's loss was not counted");
}

/*
 * Case (a), the capture old enough to pass the time test and past the settle
 * band: it dates N+1, whose ADDRESS came before the interrupt and whose END
 * was raised after the END clear at the top of radio_isr(), here while the
 * snapshot was being read (staged on read 2). The registers read may predate
 * that END, so the snapshot is retaken and N+1 is delivered once, from the
 * retake; N, never delivered, is counted.
 */
static void hook_end_raised_on_read_2(uint32_t n)
{
	if (n == 2u) {
		load(frame_n1);
		fake_hw.crc_ok = false;
		fake_hw.event[NRF_RADIO_EVENT_END] = true;
	}
}

ZTEST(radiant_backend_rx_snapshot, test_end_raised_after_a_passing_snapshot_is_retaken)
{
	backend_set_report_crc_fail();
	load(frame_n);
	fake_hw.cc0_hook = hook_end_raised_on_read_2;
	backend_frame(true);
	fake_hw.cc0_hook = NULL;
	backend_dispatch();

	zassert_equal(3u, fake_hw.cc3_captures, "the snapshot was not retaken");
	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_equal(RADIANT_RADIO_STATUS_CRC_FAIL, backend_probe()->last_rx_status,
		      "the later frame went out with the first snapshot's CRC verdict");
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_false(backend_addr_carry(), NULL);
	zassert_equal(1u, backend_late_isr_drops(), "N's loss was not counted");
}

/*
 * The settle band (review F1): a capture that passes the time test by a
 * microsecond could be N+1's, in its last microseconds with its END not yet
 * raised. Here it is: N+1 (corrupt) ends during the settle wait. The wait lets
 * its END be seen, the snapshot is retaken, and N+1 goes out as a CRC failure,
 * not as OK under N's CRCSTATUS.
 */
static void hook_corrupt_frame_ends_in_the_wait(uint32_t n)
{
	if (n == 2u) {
		load(frame_n1);
		fake_hw.crc_ok = false;
		NRF_RADIO->RXCRC = 0x0BADu;
		fake_hw.event[NRF_RADIO_EVENT_END] = true;
	}
}

ZTEST(radiant_backend_rx_snapshot, test_frame_ending_in_the_settle_band_is_retaken)
{
	backend_set_report_crc_fail();
	NRF_RADIO->RXCRC = 0x1111u;
	load(frame_n);
	fake_hw.cc3_hook = hook_corrupt_frame_ends_in_the_wait;
	backend_frame_at(true, AIR_US - 1u);
	fake_hw.cc3_hook = NULL;
	backend_dispatch();

	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_equal(RADIANT_RADIO_STATUS_CRC_FAIL, backend_probe()->last_rx_status,
		      "a corrupt frame passed as OK with the earlier CRCSTATUS");
	zassert_mem_equal(frame_n1, backend_probe()->last_rx_body, BODY_LEN, NULL);
	zassert_equal(0x0BADu, backend_probe()->last_rx_crc_rx, NULL);
	zassert_equal(1u, backend_late_isr_drops(), NULL);
}

/*
 * A carry does not outlive its window: torn, then the window closes, then a
 * real re-arm through program_rx(). An END with no ADDRESS in the new window
 * is not a reception.
 */
ZTEST(radiant_backend_rx_snapshot, test_carry_does_not_survive_a_re_arm)
{
	load(frame_n);
	backend_frame_at(true, 10u);
	backend_dispatch();
	zassert_true(backend_addr_carry(), NULL);

	backend_window_close();
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events, "no terminal for the window");
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, backend_probe()->last_rx_status,
		      NULL);

	zassert_equal(RADIANT_RADIO_OK_RC, backend_program_rx(), NULL);
	zassert_false(backend_addr_carry(), "program_rx() kept the old carry");

	backend_end_only(200u);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events,
		      "a stale carry made an END with no address match a reception");
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
