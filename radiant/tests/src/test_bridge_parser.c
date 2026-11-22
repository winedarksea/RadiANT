/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The host-frame parser in apps/common/ant_serial_bridge.c, on its own.
 *
 * The bridge is built with ANT_BRIDGE_PARSER_ONLY, which compiles only
 * process_byte() and its state; dispatch() and the health counter are this
 * file's. The cases are the two ways one lost host byte used to cost more
 * than one frame (archive/benchmarks/2026-11-16-ti-cmd-stall/): a lost LEN
 * byte that made the message id a length, and a lost last byte that made the
 * next frame's SYNC a checksum.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#define ANT_BRIDGE_PARSER_ONLY 1
#define CONFIG_ANT_DONGLE_HEALTH_COUNTERS 1
#define ant_health_note test_parser_health_note

#include "../../../apps/common/ant_health.h"

static void dispatch(uint8_t msg_id, const uint8_t *body, uint8_t len);

#include "../../../apps/common/ant_serial_bridge.c"

static unsigned int n_dispatched;
static uint8_t last_id;
static uint8_t last_body[RADIANT_WIRE_MAX_SIZE_VALUE];
static uint8_t last_len;
static unsigned int n_desync;
static unsigned int n_csum;

void test_parser_health_note(enum ant_health_counter c)
{
	if (c == ANT_HEALTH_HOST_DESYNC) {
		n_desync++;
	} else if (c == ANT_HEALTH_HOST_CSUM) {
		n_csum++;
	}
}

static void dispatch(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	n_dispatched++;
	last_id = msg_id;
	last_len = len;
	memcpy(last_body, body, MIN(len, sizeof(last_body)));
}

static void feed(const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		process_byte(b[i]);
	}
}

/* A4 len id body... xor */
static size_t frame(uint8_t *out, uint8_t id, const uint8_t *body, uint8_t len)
{
	uint8_t x = RADIANT_WIRE_SYNC_TX ^ len ^ id;

	out[0] = RADIANT_WIRE_SYNC_TX;
	out[1] = len;
	out[2] = id;
	for (uint8_t i = 0; i < len; i++) {
		out[3 + i] = body[i];
		x ^= body[i];
	}
	out[3 + len] = x;
	return 4u + len;
}

static void parser_before(void *f)
{
	ARG_UNUSED(f);
	parser_reset();
	n_dispatched = 0;
	n_desync = 0;
	n_csum = 0;
	last_id = 0;
	last_len = 0;
}

ZTEST_SUITE(bridge_parser, NULL, NULL, parser_before, NULL, NULL);

ZTEST(bridge_parser, test_a_valid_frame_dispatches)
{
	uint8_t f[16];
	const uint8_t body[] = {0x00, 0x39};

	feed(f, frame(f, RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, body, sizeof(body)));
	zassert_equal(1u, n_dispatched);
	zassert_equal(RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, last_id);
	zassert_equal(2u, last_len);
	zassert_equal(0u, n_desync + n_csum);
}

/* ASSIGN with its LEN byte lost: the id 0x42 would have been a 66-byte
 * length and swallowed the next frame. It is above RADIANT_WIRE_MAX_SIZE_VALUE, so
 * the parser hunts instead and the following frame gets through. */
ZTEST(bridge_parser, test_a_lost_len_byte_costs_only_its_own_frame)
{
	uint8_t a[16], b[16];
	const uint8_t assign[] = {0x00, 0x00, 0x00, 0x01};
	const uint8_t freq[] = {0x00, 0x39};
	size_t na = frame(a, RADIANT_WIRE_MESG_ASSIGN_CHANNEL_ID, assign, sizeof(assign));
	size_t nb = frame(b, RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, freq, sizeof(freq));

	zassert_true(RADIANT_WIRE_MESG_ASSIGN_CHANNEL_ID > RADIANT_WIRE_MAX_SIZE_VALUE);

	/* Drop a[1], the LEN. */
	process_byte(a[0]);
	feed(&a[2], na - 2);
	feed(b, nb);

	zassert_equal(1u, n_dispatched, "the frame after the damaged one was lost");
	zassert_equal(RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, last_id);
	zassert_true(n_desync > 0u, "the damage was not counted");
	zassert_equal(0u, n_csum);
}

/* UNASSIGN with its last byte (the checksum) lost: the next frame's SYNC
 * arrives in the checksum position. It fails the checksum and starts the next
 * frame, which dispatches. */
ZTEST(bridge_parser, test_a_sync_in_the_checksum_slot_starts_the_next_frame)
{
	uint8_t a[16], b[16];
	const uint8_t unassign[] = {0x00};
	const uint8_t assign[] = {0x00, 0x00, 0x00, 0x01};
	size_t na = frame(a, RADIANT_WIRE_MESG_UNASSIGN_CHANNEL_ID, unassign,
			  sizeof(unassign));
	size_t nb = frame(b, RADIANT_WIRE_MESG_ASSIGN_CHANNEL_ID, assign, sizeof(assign));

	zassert_not_equal(RADIANT_WIRE_SYNC_TX, a[na - 1],
			  "the test needs a checksum that is not SYNC");

	feed(a, na - 1);
	feed(b, nb);

	zassert_equal(1u, n_dispatched);
	zassert_equal(RADIANT_WIRE_MESG_ASSIGN_CHANNEL_ID, last_id);
	zassert_equal(1u, n_csum);
	zassert_equal(0u, n_desync, "the next frame was hunted past");
}

/* The other reading of the same byte: a frame whose real, corrupt checksum
 * happens to be 0xA4. The re-seed takes it as a SYNC, the real next SYNC
 * then arrives as a LEN, and S_LEN re-seeds on that - one desync count, and
 * the next frame still dispatches. */
ZTEST(bridge_parser, test_a_corrupt_checksum_of_a4_does_not_cost_the_next_frame)
{
	uint8_t a[16], b[16];
	const uint8_t unassign[] = {0x00};
	const uint8_t freq[] = {0x00, 0x39};
	size_t na = frame(a, RADIANT_WIRE_MESG_UNASSIGN_CHANNEL_ID, unassign,
			  sizeof(unassign));
	size_t nb = frame(b, RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, freq, sizeof(freq));

	zassert_not_equal(RADIANT_WIRE_SYNC_TX, a[na - 1]);
	a[na - 1] = RADIANT_WIRE_SYNC_TX;

	feed(a, na);
	feed(b, nb);

	zassert_equal(1u, n_dispatched);
	zassert_equal(RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, last_id);
	zassert_equal(1u, n_csum);
	zassert_equal(1u, n_desync);
}

/* A LEN above the maximum is refused outright: nothing dispatches, even a
 * frame with a correct checksum for its length (the conformance tool's
 * frame/malformed-oversize, LEN 42). */
ZTEST(bridge_parser, test_an_oversize_len_is_dropped_silently)
{
	uint8_t f[64];
	uint8_t body[RADIANT_WIRE_MAX_SIZE_VALUE + 4] = {0};

	feed(f, frame(f, RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, body, sizeof(body)));
	zassert_equal(0u, n_dispatched);
	zassert_true(n_desync > 0u);
}
