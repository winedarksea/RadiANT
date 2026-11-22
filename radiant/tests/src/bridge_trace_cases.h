/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The host trace (CONFIG_ANT_DONGLE_HOST_TRACE) and the parser-buffer wipe in
 * apps/common/ant_serial_bridge.c: no key byte reaches a trace line or
 * outlives its frame in the parser buffer, whatever the security
 * configuration.
 *
 * Included by test_bridge_trace.c (security compiled out) and
 * test_bridge_trace_sec.c (host security messages and pairing compiled in),
 * after each has set its configuration. Each of those files defines its own
 * suite and one ZTEST per check below: the checks are the same in both,
 * because the redaction must not depend on the configuration.
 *
 * The bridge is included the way test_bridge_parser.c includes it, with
 * ANT_BRIDGE_PARSER_ONLY, plus the trace compiled in and its lines captured
 * through ANT_BRIDGE_TRACE_EMIT instead of logged.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/logging/log.h>

/* test_bridge_parser.c includes the bridge too and registers its log module;
 * a second registration would be a duplicate symbol. */
#undef LOG_MODULE_REGISTER
#define LOG_MODULE_REGISTER(...) LOG_MODULE_DECLARE(__VA_ARGS__)

#define ANT_BRIDGE_PARSER_ONLY 1
#define CONFIG_ANT_DONGLE_HOST_TRACE 1
#define ANT_BRIDGE_TRACE_EMIT(dir, line) trace_emit((dir), (line))

static void trace_emit(const char *dir, const char *line);
static void dispatch(uint8_t msg_id, const uint8_t *body, uint8_t len);

#include "../../../apps/common/ant_serial_bridge.c"

static char trace_line[4 * RADIANT_WIRE_MAX_FRAME_SIZE + 8];
static unsigned int n_trace;
static unsigned int n_dispatched;

static void trace_emit(const char *dir, const char *line)
{
	n_trace++;
	snprintf(trace_line, sizeof(trace_line), "%s %s", dir, line);
}

static void dispatch(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	ARG_UNUSED(msg_id);
	ARG_UNUSED(body);
	ARG_UNUSED(len);
	n_dispatched++;
}

static void trace_before(void *f)
{
	ARG_UNUSED(f);
	parser_reset();
	n_trace = 0;
	n_dispatched = 0;
	trace_line[0] = '\0';
}

/* A4 len id body... xor */
static size_t trace_frame_build(uint8_t *out, uint8_t id, const uint8_t *body,
				uint8_t len)
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

static void trace_feed(const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		process_byte(b[i]);
	}
}

/* "a4 13 46 00" then n_masked "xx" (the checksum among them). */
static void expect_line(const char *dir, const uint8_t *f, size_t keep,
			size_t n_masked)
{
	char want[sizeof(trace_line)];
	size_t o = (size_t)snprintf(want, sizeof(want), "%s", dir);

	for (size_t i = 0; i < keep; i++) {
		o += (size_t)snprintf(want + o, sizeof(want) - o, " %02x", f[i]);
	}
	for (size_t i = 0; i < n_masked; i++) {
		o += (size_t)snprintf(want + o, sizeof(want) - o, " xx");
	}
	zassert_str_equal(want, trace_line, "want \"%s\"", want);
}

/* A key whose every byte is recognisable in hex and none is 0x00 or 0xA4. */
static void fill_key(uint8_t *k, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		k[i] = (uint8_t)(0xC1 + i);
	}
}

static bool body_is_zero(void)
{
	for (size_t i = 0; i < sizeof(parser.msg_body); i++) {
		if (parser.msg_body[i] != 0) {
			return false;
		}
	}
	return true;
}

/* 0x46 [net, key 8]: the network number shows; the key and the checksum
 * (which is the XOR of the key bytes once the header is known) do not. */
static void check_network_key_masked(void)
{
	uint8_t body[9], f[16];
	size_t n;

	body[0] = 0x00;
	fill_key(&body[1], 8);
	n = trace_frame_build(f, RADIANT_WIRE_MESG_NETWORK_KEY_ID, body, sizeof(body));
	trace_feed(f, n);
	zassert_equal(1u, n_trace);
	expect_line("H>", f, 4, 8 + 1);
}

/* 0x7E [key number, key 16]: the key number shows. */
static void check_encrypt_key_masked(void)
{
	uint8_t body[17], f[24];
	size_t n;

	body[0] = 0x03;
	fill_key(&body[1], 16);
	n = trace_frame_build(f, RADIANT_WIRE_MESG_SET_ENCRYPT_KEY_ID, body, sizeof(body));
	trace_feed(f, n);
	zassert_equal(1u, n_trace);
	expect_line("H>", f, 4, 16 + 1);
}

/* 0xF2 [channel, key bits, key 16]: channel and key length show. */
static void check_radiant_key_masked(void)
{
	uint8_t body[18], f[24];
	size_t n;

	body[0] = 0x01;
	body[1] = 128;
	fill_key(&body[2], 16);
	n = trace_frame_build(f, RADIANT_WIRE_MESG_RADIANT_SET_KEY_ID, body, sizeof(body));
	trace_feed(f, n);
	zassert_equal(1u, n_trace);
	expect_line("H>", f, 5, 16 + 1);
}

/* 0xF5 sub-command 0x02 [channel, 0x02, scalar 32]: nothing past the id
 * shows, not even the sub-command, so no sub-command can leak by being
 * classified wrongly. */
static void check_pairing_masked(void)
{
	uint8_t body[34], f[40];
	size_t n;

	body[0] = 0x00;
	body[1] = 0x02;
	fill_key(&body[2], 32);
	n = trace_frame_build(f, RADIANT_WIRE_MESG_RADIANT_PAIRING_ID, body, sizeof(body));
	trace_feed(f, n);
	zassert_equal(1u, n_trace);
	expect_line("H>", f, 3, 34 + 1);
}

/* The other direction goes through the same masking: a 0xF5 reply. */
static void check_outbound_pairing_masked(void)
{
	uint8_t body[34], f[40];
	size_t n;

	body[0] = 0x00;
	body[1] = 0x03;
	fill_key(&body[2], 32);
	n = trace_frame_build(f, RADIANT_WIRE_MESG_RADIANT_PAIRING_ID, body, sizeof(body));
	TRACE_TX(f, n);
	zassert_equal(1u, n_trace);
	expect_line("<D", f, 3, 34 + 1);
}

/* A frame with no key in it is traced whole, checksum included. */
static void check_plain_frame_whole(void)
{
	const uint8_t body[] = {0x00, 0x39};
	uint8_t f[8];
	size_t n = trace_frame_build(f, RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, body,
				     sizeof(body));

	trace_feed(f, n);
	zassert_equal(1u, n_trace);
	expect_line("H>", f, n, 0);
	zassert_equal(0x39, parser.msg_body[1], "a plain body was wiped");
}

/* Every key-carrying message leaves the parser buffer zeroed once its frame
 * ends, the ANT ones as well as RadiANT's. */
static void check_key_bodies_wiped(void)
{
	static const uint8_t ids[] = {
		RADIANT_WIRE_MESG_NETWORK_KEY_ID,
		RADIANT_WIRE_MESG_SET_ENCRYPT_KEY_ID,
		RADIANT_WIRE_MESG_RADIANT_SET_KEY_ID,
		RADIANT_WIRE_MESG_RADIANT_PAIRING_ID,
	};

	for (size_t k = 0; k < ARRAY_SIZE(ids); k++) {
		uint8_t body[18], f[24];
		size_t n;

		body[0] = 0x01;
		fill_key(&body[1], sizeof(body) - 1);
		n = trace_frame_build(f, ids[k], body, sizeof(body));
		trace_feed(f, n);
		zassert_equal(k + 1, n_dispatched);
		zassert_true(body_is_zero(), "0x%02x body kept", ids[k]);
	}
}

/* A key frame that fails its checksum is wiped too: it was still a key. */
static void check_bad_checksum_key_wiped(void)
{
	uint8_t body[17], f[24];
	size_t n;

	body[0] = 0x00;
	fill_key(&body[1], 16);
	n = trace_frame_build(f, RADIANT_WIRE_MESG_SET_ENCRYPT_KEY_ID, body, sizeof(body));
	f[n - 1] ^= 0x01;
	zassert_not_equal(RADIANT_WIRE_SYNC_TX, f[n - 1]);
	trace_feed(f, n);
	zassert_equal(0u, n_dispatched);
	zassert_true(body_is_zero(), "a bad-checksum key frame was kept");
}

/* A frame that fails its checksum is wiped whatever its id: a UART overrun
 * that drops the id byte of a 0x7E makes its key number the id and leaves
 * the key in the body under an id that carries none. */
static void check_bad_checksum_any_id_wiped(void)
{
	uint8_t body[17], f[24];
	size_t n;

	body[0] = 0x03;
	fill_key(&body[1], 16);
	n = trace_frame_build(f, RADIANT_WIRE_MESG_SET_ENCRYPT_KEY_ID, body, sizeof(body));
	/* Drop the id byte: LEN 0x11, then the key number read as the id. */
	memmove(&f[2], &f[3], n - 3);
	trace_feed(f, n - 1);
	trace_feed((const uint8_t[]){0x00}, 1);	/* the byte taken as checksum */
	zassert_equal(0u, n_dispatched);
	zassert_equal(0u, n_trace);
	zassert_true(body_is_zero(), "a failed frame's body was kept");
}

/* 0x7F type 0x02 is an RNG seed: the type shows, the seed does not, and it is
 * wiped. */
static void check_rng_seed_masked_and_wiped(void)
{
	uint8_t body[17], f[24];
	size_t n;

	body[0] = RADIANT_WIRE_ENCRYPTION_INFO_SET_RNG_SEED;
	fill_key(&body[1], 16);
	n = trace_frame_build(f, RADIANT_WIRE_MESG_SET_ENCRYPT_INFO_ID, body, sizeof(body));
	trace_feed(f, n);
	zassert_equal(1u, n_trace);
	expect_line("H>", f, 4, 16 + 1);
	zassert_true(body_is_zero(), "an RNG seed was kept");
}

/* Other 0x7F types are not secret: traced whole, not wiped. */
static void check_crypto_id_info_whole(void)
{
	const uint8_t body[] = {RADIANT_WIRE_ENCRYPTION_INFO_SET_CRYPTO_ID,
				0x11, 0x22, 0x33, 0x44};
	uint8_t f[12];
	size_t n = trace_frame_build(f, RADIANT_WIRE_MESG_SET_ENCRYPT_INFO_ID, body,
				     sizeof(body));

	trace_feed(f, n);
	zassert_equal(1u, n_trace);
	expect_line("H>", f, n, 0);
	zassert_equal(0x44, parser.msg_body[4]);
}
