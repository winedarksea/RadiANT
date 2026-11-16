/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against
 * radiant/src/radiant_rx_torn.h.
 *
 * The nRF backend's late-ISR guard decides, from two CC_SYNC reads around its
 * buffer copy, a clock read after them and the frame's least ADDRESS-to-END
 * airtime, whether the snapshot is one frame's. Wrong one way, a late interrupt
 * delivers frame N's CRC verdict over frame N+1's bytes and timestamp; wrong
 * the other way, every frame whose interrupt runs on time is dropped.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "../../src/radiant_rx_torn.h"

ZTEST_SUITE(radiant_rx_torn, NULL, NULL, NULL, NULL, NULL);

/* A tracking frame: 8-byte body + CRC at 8 us a byte, less 2 us slack. */
#define AIR 78u

ZTEST(radiant_rx_torn, test_on_time_frame_is_not_torn)
{
	zassert_false(radiant_rx_torn(1000u, 1000u, 1000u + AIR, AIR));
	zassert_false(radiant_rx_torn(1000u, 1000u, 1000u + AIR + 5u, AIR));
	/* Very late, but no new ADDRESS: still one frame's (the next frame never came). */
	zassert_false(radiant_rx_torn(1000u, 1000u, 1000u + 200000u, AIR));
}

ZTEST(radiant_rx_torn, test_capture_younger_than_the_airtime_is_torn)
{
	/* The capture dates a frame still on the air: the next frame's ADDRESS. */
	zassert_true(radiant_rx_torn(1000u, 1000u, 1000u + AIR - 1u, AIR));
	zassert_true(radiant_rx_torn(1000u, 1000u, 1000u, AIR));
}

ZTEST(radiant_rx_torn, test_reads_that_differ_are_torn)
{
	/* A new ADDRESS landed during the copy, however old the first capture. */
	zassert_true(radiant_rx_torn(1000u, 1300u, 1000u + 5000u, AIR));
	zassert_true(radiant_rx_torn(1000u, 999u, 1000u + 5000u, AIR));
}

ZTEST(radiant_rx_torn, test_counter_wrap)
{
	/* Capture just before the 32-bit wrap, clock read just after. */
	zassert_false(radiant_rx_torn(0xFFFFFFF0u, 0xFFFFFFF0u,
				      0xFFFFFFF0u + AIR, AIR));
	zassert_false(radiant_rx_torn(0xFFFFFFF0u, 0xFFFFFFF0u, 100u, AIR));
	zassert_true(radiant_rx_torn(0xFFFFFFF0u, 0xFFFFFFF0u, 20u, AIR),
		     "36 us across the wrap is still under the airtime");
	/* A capture exactly at the wrap. */
	zassert_false(radiant_rx_torn(0u, 0u, AIR, AIR));
	zassert_true(radiant_rx_torn(0u, 0u, AIR - 1u, AIR));
}

ZTEST(radiant_rx_torn, test_clock_behind_the_capture_is_torn)
{
	/* The hardware cannot produce it; drop (counted) rather than deliver. */
	zassert_true(radiant_rx_torn(1000u, 1000u, 999u, AIR));
	zassert_true(radiant_rx_torn(1000u, 1000u, 1000u + 0x80000000u, AIR));
	zassert_false(radiant_rx_torn(1000u, 1000u, 1000u + 0x7FFFFFFFu, AIR));
}

ZTEST(radiant_rx_torn, test_zero_airtime_only_checks_the_reads)
{
	zassert_false(radiant_rx_torn(5u, 5u, 5u, 0u));
	zassert_true(radiant_rx_torn(5u, 6u, 5u, 0u));
}
