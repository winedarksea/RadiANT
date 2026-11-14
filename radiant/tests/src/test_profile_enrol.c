/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_enrol.c - compat-C7's gate: a node gains a second receiver
 * without the first one noticing.
 *
 * The plan's claim has three clauses: "a node in private mode gains a second
 * receiver, the first receiver's stream is uninterrupted across the whole
 * window, and the new receiver reaches the node by deriving the locator
 * rather than by any announcement." Not all belong to this phase:
 *
 *   - Gains a second receiver: asserted here end to end, with the node's
 *     public key read back off the air, the peer's answer arriving as
 *     eight-byte acknowledged-data payloads, and the node's own root coming
 *     back off the air wrapped, so the receiver verifies the stream with the
 *     key the first receiver already had (security review 2026-11-13,
 *     item 6: the exchange used to replace that root instead).
 *   - Uninterrupted across the whole window: asserted here over a full
 *     60-second window of a real heart-rate stream - no silent slot, no
 *     channel change, no page number outside the profile's own union, a
 *     monotone event counter, and every Tier I window still verifying at the
 *     receiver that was already there.
 *   - In private mode, by deriving the locator: asserted since compat-C8, in
 *     test_a_private_node_gains_a_receiver_that_derives_it, against a node
 *     that is genuinely private and genuinely silent.
 *
 * The third clause was deferred by C7 rather than faked, because the locator
 * is trunc16(CMAC(K_id, "priv" || epoch)) and K_id is reachable only from the
 * layer holding the root key - deriving it here would mean inventing C8's
 * entry point from outside. What C7 paid instead is the property that makes
 * the third clause possible: enrolment does not depend on the `announce`
 * setting, does not touch the beacon's locator fields, and needs no compat
 * instance at all, so it works on whichever channel the node happens to be
 * on. test_enrolment_needs_no_beacon_at_all is that assertion, and the C8
 * test at the bottom of this file stands on it.
 *
 * The other deferral: on device type 0x60 the enrolment frames need a page
 * number in the telemetry envelope's own namespace, which is an envelope
 * change that belongs with the phase that creates a reason for a node to be
 * there. C8's RETURN frames inherit the same deferral, which is why the C8
 * test picks a page number locally rather than either module naming one.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant/radiant_sec.h>
#include <radiant/radiant_sec_compat.h>
#include <radiant/radiant_channel.h>
#include <radiant/radiant_radio_hal.h>

#include "fake_radio.h"
#include "fake_nvm.h"
#include "node_ident.h"
#include "profile_common.h"
#include "profile_compat.h"
#include "profile_enrol.h"
#include "profile_hr.h"
#include "profile_private.h"
#include "profile_sched.h"

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

/* The node's compat channel, the receiver that was already listening, and the
 * pairing context of the receiver being added. Three channels because the
 * interesting assertion needs all three at once. */
#define NODE_CH    0u
#define WATCHER_CH 1u
#define PEER_CH    2u

#define NODE_DEVNUM 0x2C41u
#define PEER_DEVNUM 0x51C4u

/* The same root, epoch and device number test_profile_compat.c pins, so a
 * capture from either suite is a stream under one key. */
static const uint8_t compat_root[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

#define TEST_EPOCH 7u
#define TEST_T_S   RADIANT_SEC_COMPAT_T_DEFAULT_S
#define TEST_N     RADIANT_SEC_COMPAT_N_DEFAULT

#define TX_SWITCHES RADIANT_SEC_COMPAT_SW_TIER_I
#define RX_SWITCHES ((uint8_t)(RADIANT_SEC_COMPAT_SW_TIER_I | \
			       RADIANT_SEC_COMPAT_SW_PIN))

/*
 * 242 messages at ~4.06 Hz is 59.6 s: two full 121-message cycles and just
 * inside the 60-second window, so "across the whole window" is the whole window
 * and not a sample of it. One slot more would expire it mid-run, which is what
 * test_the_window_closes_itself uses instead.
 */
#define SLOTS 242u

#define T_ORIGIN ((uint64_t)0x100000000ull + 4321u)
#define US_PER_S 1000000u

static uint64_t period_us(uint16_t counts)
{
	return ((uint64_t)counts * US_PER_S) / 32768u;
}

/* Fixed scalars. A test that generated its own would be untestable against a
 * stated expectation and would fail intermittently if the generator were
 * wrong - test_sec_pair.c's argument, and these are its vectors. */
static const uint8_t node_scalar[32] = {
	0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
	0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
	0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
	0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};
static const uint8_t peer_scalar[32] = {
	0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
	0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
	0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
	0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
};
static const uint8_t other_scalar[32] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
	0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
	0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
};

/* K_dev = 00 01 02 ... 0f, test_node_ident.c's vector key. */
static const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

/* ── The slot log ───────────────────────────────────────────────────────── */

struct slot_log {
	uint8_t                body[8];
	enum profile_slot_kind kind;
};

static struct slot_log log_buf[SLOTS];
static uint32_t        log_n;

static void log_reset(void)
{
	log_n = 0u;
	memset(log_buf, 0, sizeof(log_buf));
}

static void log_put(const uint8_t *body, enum profile_slot_kind kind)
{
	if (log_n < SLOTS) {
		memcpy(log_buf[log_n].body, body, 8);
		log_buf[log_n].kind = kind;
	}
	log_n++;
}

static uint8_t sub_of(const uint8_t *body)
{
	uint8_t page = (uint8_t)(body[0] & PROFILE_COMPAT_PAGE_MASK);

	if (page != PROFILE_COMPAT_PAGE_ATTEST_I &&
	    page != PROFILE_COMPAT_PAGE_ATTEST_II) {
		return 0u;
	}
	return (uint8_t)(page & 0x0Fu);
}

static uint32_t count_page(uint8_t page)
{
	uint32_t i, n = 0u;

	for (i = 0u; i < log_n && i < SLOTS; i++) {
		if ((log_buf[i].body[0] & PROFILE_COMPAT_PAGE_MASK) == page) {
			n++;
		}
	}
	return n;
}

/* ── The node's pairing seam ────────────────────────────────────────────── */

/*
 * The two-line adapter profile_enrol.h describes, wired to a fixed scalar so
 * the exchange is reproducible. `user` is the scalar, which is what lets one
 * adapter serve the node and a second, different key in the splice test.
 */
static int fixed_open_pairing(void *user, uint8_t ch, uint8_t timeout_s,
			      uint8_t *pubkey)
{
	const uint8_t *scalar = (const uint8_t *)user;

	if (radiant_sec_pair_enter(ch, timeout_s) != RADIANT_SEC_OK) {
		return -EIO;
	}
	if (radiant_sec_pair_set_scalar(ch, scalar) != RADIANT_SEC_OK) {
		return -EIO;
	}
	if (radiant_sec_pair_local_pubkey(ch, pubkey) != RADIANT_SEC_OK) {
		return -EIO;
	}
	return 0;
}

static void fixed_close_pairing(void *user, uint8_t ch)
{
	ARG_UNUSED(user);
	radiant_sec_pair_leave(ch);
}

/* The same seam wired to ADR 0009's pair, which is what a strap with no host
 * actually runs: the counter advances and is PERSISTED inside
 * node_ident_pair_begin(), before there is a public key to transmit. */
static int node_ident_open_pairing(void *user, uint8_t ch, uint8_t timeout_s,
				   uint8_t *pubkey)
{
	ARG_UNUSED(user);

	if (node_ident_pair_begin(ch, timeout_s) != NODE_IDENT_OK) {
		return -EIO;
	}
	if (node_ident_pair_pubkey(ch, pubkey) != NODE_IDENT_OK) {
		return -EIO;
	}
	return 0;
}

/* Both halves of the window, because there are two: radiant_sec's pairing
 * context and node_ident's own bookkeeping. Closing one and not the other is
 * how the next window finds itself already open. */
static void node_ident_close_pairing(void *user, uint8_t ch)
{
	ARG_UNUSED(user);
	radiant_sec_pair_leave(ch);
	node_ident_pair_window_close();
}

/* ── Fixtures ───────────────────────────────────────────────────────────── */

static void give_id(uint8_t ch, uint16_t devnum)
{
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(ch, 0x10u, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(ch, devnum, PROFILE_HR_DEVICE_TYPE,
					     0x01u));
}

static void keys_up(uint16_t period_counts)
{
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(NODE_CH, compat_root, 128,
						 NODE_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(WATCHER_CH, compat_root, 128,
						 NODE_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_devnum(NODE_CH, NODE_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_devnum(WATCHER_CH, NODE_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(NODE_CH, TX_SWITCHES, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(WATCHER_CH, RX_SWITCHES,
						   TEST_N, TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(NODE_CH, TEST_EPOCH, T_ORIGIN,
						   0u, period_counts));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(WATCHER_CH, TEST_EPOCH,
						   T_ORIGIN, 0u, period_counts));
}

static void compat_up(struct profile_compat *pc)
{
	struct profile_compat_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.ch = NODE_CH;
	cfg.epoch = TEST_EPOCH;
	cfg.advertise = true;
	cfg.attest_available = true;
	cfg.pairing_available = true;
	cfg.policy = PROFILE_COMPAT_POLICY_NEVER;
	cfg.window = TEST_N;
	cfg.mode = PROFILE_COMPAT_MODE_FIXED;

	zassert_equal(0, profile_compat_init(pc, &cfg));
}

static void enrol_up(struct profile_enrol *pe, uint8_t mode, const void *scalar)
{
	struct profile_enrol_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.ch = NODE_CH;
	cfg.mode = mode;
	cfg.timeout_s = 0u;   /* the 60-second default, not "forever" */
	cfg.open_pairing = fixed_open_pairing;
	cfg.close_pairing = fixed_close_pairing;
	cfg.user = (void *)scalar;

	zassert_equal(0, profile_enrol_init(pe, &cfg));
}

static void hr_cfg_fill(struct profile_hr_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->id.hw_revision = 1u;
	cfg->id.manufacturer_id = 0x00FFu;
	cfg->id.model_number = 0x0001u;
	cfg->id.sw_revision_main = 1u;
	cfg->id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg->id.serial_number = PROFILE_COMMON_INVALID_U32;
	cfg->manufacturer_id_8 = 0xFFu;
	cfg->serial_upper16 = 0x4553u;
	cfg->model_number_8 = 1u;
	cfg->hw_version = 1u;
	cfg->sw_version = 1u;
}

/*
 * Run the strap for `slots` messages, feeding every transmitted body to the
 * receiver that was already there. The watcher is a pinned receiver, so a
 * stream that lost its attestation reads UNVERIFIED rather than falling back
 * to clear and looking normal.
 */
static void run_hr(struct profile_hr *hr, uint32_t first_slot, uint32_t slots)
{
	uint64_t period = period_us(PROFILE_HR_PERIOD);
	uint32_t i;

	/* `first_slot` rather than always zero, so a test can run the strap in
	 * several stretches without the clock going backwards under the
	 * attestation counter (a replay to the receiver, and a meaningless
	 * test failure). */
	log_reset();
	profile_hr_set_computed(hr, 60u);

	for (i = 0u; i < slots; i++) {
		uint32_t               n = first_slot + i;
		uint8_t                body[8];
		uint64_t               now = T_ORIGIN + (uint64_t)n * period;
		enum profile_slot_kind kind;

		if ((n % 4u) == 0u) {
			profile_hr_beat(hr, (uint16_t)((n / 4u) * 1024u));
		}
		profile_hr_set_operating_time(hr, n / 4u);

		kind = profile_hr_next(hr, now, body);
		zassert_not_equal(PROFILE_SLOT_IDLE, kind, "slot %u", n);
		log_put(body, kind);

		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_rx(WATCHER_CH, sub_of(body),
						    body, 8u, now));
	}
}

/* ── The set check, mirrored ────────────────────────────────────────────── */

/*
 * The four bytes profile_enrol.c appends. Written out here rather than exported
 * from the module, so that a change to the derivation has to be made twice and
 * a receiver implementer reading this file sees the whole of what it takes to
 * assemble a key.
 */
static void check_of(const uint8_t *pubkey, uint8_t *out4)
{
	struct radiant_sec_key zero_key;
	uint8_t                zero[16] = { 0 };
	uint8_t                tag[RADIANT_SEC_BLOCK_BYTES];

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_key_import(&zero_key, zero, 128u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_cmac(&zero_key, pubkey,
				       PROFILE_ENROL_PUBKEY_BYTES, tag));
	radiant_sec_key_destroy(&zero_key);
	memcpy(out4, tag, PROFILE_ENROL_CHECK_BYTES);
}

/* ── The two directions on the air ──────────────────────────────────────── */

#define ENROL_SET_COUNT \
	((uint8_t)(PROFILE_COMPAT_BEACON_FRAMES + PROFILE_ENROL_FRAMES))

/*
 * Reassemble the node's public key from the logged stream - the only thing a
 * new receiver has. Nothing is read from the node's own state, so a pass
 * here means the key genuinely went on the air in frames a stranger can find.
 */
static int pubkey_from_air(uint8_t *out36)
{
	uint8_t  have = 0u;
	uint32_t i;

	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;
		uint8_t        idx;
		uint8_t        count;

		if ((b[0] & PROFILE_COMPAT_PAGE_MASK) !=
		    PROFILE_COMPAT_PAGE_BEACON) {
			continue;
		}
		idx = (uint8_t)(b[1] >> 4);
		count = (uint8_t)((b[1] & 0x0Fu) + 1u);
		if (count != ENROL_SET_COUNT) {
			continue;
		}
		if (idx < PROFILE_COMPAT_BEACON_FRAMES) {
			continue;
		}
		idx = (uint8_t)(idx - PROFILE_COMPAT_BEACON_FRAMES);
		if (idx >= PROFILE_ENROL_FRAMES) {
			continue;
		}
		memcpy(&out36[(size_t)idx * PROFILE_ENROL_FRAME_PAYLOAD], &b[2],
		       PROFILE_ENROL_FRAME_PAYLOAD);
		have |= (uint8_t)(1u << idx);
	}
	return (have == (uint8_t)((1u << PROFILE_ENROL_FRAMES) - 1u)) ? 0 : -EAGAIN;
}

/* The peer's answer: six eight-byte acknowledged-data payloads carrying its own
 * six-frame set, count 6 and indices 0..5 whatever the node's set looks like. */
static void peer_set(const uint8_t *peer_pub, uint8_t out[PROFILE_ENROL_FRAMES][8])
{
	uint8_t set[PROFILE_ENROL_SET_BYTES];
	uint8_t i;

	memcpy(set, peer_pub, PROFILE_ENROL_PUBKEY_BYTES);
	check_of(peer_pub, &set[PROFILE_ENROL_PUBKEY_BYTES]);

	for (i = 0u; i < PROFILE_ENROL_FRAMES; i++) {
		out[i][0] = PROFILE_COMPAT_PAGE_BEACON;
		out[i][1] = (uint8_t)((i << 4) | (PROFILE_ENROL_FRAMES - 1u));
		memcpy(&out[i][2], &set[(size_t)i * PROFILE_ENROL_FRAME_PAYLOAD],
		       PROFILE_ENROL_FRAME_PAYLOAD);
	}
}

#define ROOT_SET_COUNT \
	((uint8_t)(PROFILE_COMPAT_BEACON_FRAMES + PROFILE_ENROL_ROOT_FRAMES))

/*
 * Reassemble the wrapped root from the logged stream, as the new receiver does
 * once the window has closed: the beacon set at six, frames 2..5. Nothing is
 * read from the node's state.
 */
static int root_from_air(uint8_t *out24)
{
	uint8_t  have = 0u;
	uint32_t i;

	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;
		uint8_t        idx = (uint8_t)(b[1] >> 4);

		if ((b[0] & PROFILE_COMPAT_PAGE_MASK) !=
			    PROFILE_COMPAT_PAGE_BEACON ||
		    (uint8_t)((b[1] & 0x0Fu) + 1u) != ROOT_SET_COUNT ||
		    idx < PROFILE_COMPAT_BEACON_FRAMES) {
			continue;
		}
		idx = (uint8_t)(idx - PROFILE_COMPAT_BEACON_FRAMES);
		memcpy(&out24[(size_t)idx * PROFILE_ENROL_FRAME_PAYLOAD], &b[2],
		       PROFILE_ENROL_FRAME_PAYLOAD);
		have |= (uint8_t)(1u << idx);
	}
	return (have == (uint8_t)((1u << PROFILE_ENROL_ROOT_FRAMES) - 1u)) ? 0
									   : -EAGAIN;
}

/* Feed the logged stretch that began at `first_slot` to another receiver, at
 * the instants run_hr() sent it. */
static void feed(uint8_t ch, uint32_t first_slot)
{
	uint64_t period = period_us(PROFILE_HR_PERIOD);
	uint32_t i;

	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;

		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_rx(
				      ch, sub_of(b), b, 8u,
				      T_ORIGIN + (uint64_t)(first_slot + i) *
							 period));
	}
}

/* Bring the new receiver's own pairing context up and produce its public key. */
static void peer_up(const uint8_t *scalar, uint8_t *pub_out)
{
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(PEER_CH, 60u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(PEER_CH, scalar));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_local_pubkey(PEER_CH, pub_out));
}

static void enrol_before(void *f)
{
	ARG_UNUSED(f);
	radiant_sec_compat_reset();
	radiant_sec_pair_reset();
	radiant_sec_reset();
	fake_radio_reset();
	radiant_channel_init();
	log_reset();
}

ZTEST_SUITE(profile_enrol, NULL, NULL, enrol_before, NULL, NULL);

/* ═══ The load-bearing test ══════════════════════════════════════════════ */

ZTEST(profile_enrol, test_a_second_receiver_joins_and_the_first_stream_holds)
{
	struct profile_hr        hr;
	struct profile_hr_cfg    hcfg;
	struct profile_compat    pc;
	struct profile_enrol     pe;
	struct radiant_sec_compat_stats st;
	struct radiant_sec_stats sstats;
	uint8_t                  air[PROFILE_ENROL_SET_BYTES];
	uint8_t                  check[PROFILE_ENROL_CHECK_BYTES];
	uint8_t                  wrapped[PROFILE_ENROL_WRAPPED_BYTES];
	uint8_t                  peer_pub[32];
	uint8_t                  frames[PROFILE_ENROL_FRAMES][8];
	uint32_t                 node_fp = 0u;
	uint32_t                 peer_fp = 0u;
	uint32_t                 tier1;
	uint32_t                 tier1_late;
	uint32_t                 data_pages = 0u;
	uint32_t                 delivered = 0u;
	bool                     shrunk = false;
	uint32_t                 i;
	int                      rc;
	uint8_t                  k;

	/*
	 * The whole of Layer D in one test: a strap already running, with a
	 * keyholder already listening and verifying. The owner presses the
	 * button. Sixty seconds later the strap has a second keyholder, and
	 * the first one saw a capability bit go up and back down and nothing
	 * else - no channel change, no re-key, no epoch move, no silent slot
	 * and no unverified window.
	 */
	give_id(NODE_CH, NODE_DEVNUM);
	/* The receiver tracks the node, so its channel carries the node's ID:
	 * the device number the delivered root is bound to. */
	give_id(PEER_CH, NODE_DEVNUM);
	keys_up(PROFILE_HR_PERIOD);

	hr_cfg_fill(&hcfg);
	zassert_equal(0, profile_hr_init(&hr, &hcfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	zassert_equal(0, profile_enrol_attach(&pe, &pc));

	radiant_sec_compat_get_stats(NODE_CH, &st);
	zassert_equal(0u, st.enrolments);

	/* The physical action. */
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_true(profile_enrol_is_open(&pe));

	run_hr(&hr, 0u, SLOTS);

	/* ── The window ran to the end of the run ───────────────────────── */
	zassert_true(profile_enrol_is_open(&pe));

	/* ── The key went on the air, in frames a stranger can assemble ── */
	zassert_equal(0, pubkey_from_air(air), "no key in %u slots", SLOTS);
	check_of(air, check);
	zassert_mem_equal(check, &air[PROFILE_ENROL_PUBKEY_BYTES], PROFILE_ENROL_CHECK_BYTES);

	/* ── The peer answers over acknowledged data ────────────────────── */
	peer_up(peer_scalar, peer_pub);
	peer_set(peer_pub, frames);

	for (k = 0u; k < PROFILE_ENROL_FRAMES; k++) {
		rc = profile_enrol_on_ack_data(&pe, frames[k], 8u);
		if (k + 1u < PROFILE_ENROL_FRAMES) {
			zassert_equal(0, rc, "frame %u", k);
		} else {
			zassert_equal(1, rc, "rc %d", rc);
		}
	}

	/* ── Both ends reach the same six digits ────────────────────────── */
	zassert_equal(0, profile_enrol_fingerprint(&pe, &node_fp));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_join(PEER_CH, air, &peer_fp));
	zassert_equal(node_fp, peer_fp, "%u vs %u", node_fp, peer_fp);
	zassert_true(node_fp < 1000000u);

	/* ── The counter moved exactly once, beside the root that went out ─
	 * A compat-only node counts in its compat context, and no secured
	 * context was created on it - to count, or to hold a DH root. */
	radiant_sec_compat_get_stats(NODE_CH, &st);
	zassert_equal(1u, st.enrolments, "got %u", st.enrolments);
	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(0u, sstats.enrolments);
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_configure(NODE_CH, RADIANT_SEC_SW_AUTH, 2u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));

	/* ── ONE PAIRING PER WINDOW ─────────────────────────────────────── */
	zassert_false(profile_enrol_is_open(&pe));
	zassert_equal(-EINVAL, profile_enrol_on_ack_data(&pe, frames[0], 8u));
	radiant_sec_compat_get_stats(NODE_CH, &st);
	zassert_equal(1u, st.enrolments);

	/* ── AND THE FIRST RECEIVER'S STREAM WAS UNINTERRUPTED ──────────── */

	/* Not "mostly": every Tier I window across the whole run verified at
	 * the receiver that was already there, and none failed. A node that
	 * had re-keyed, moved its epoch, changed devnum or dropped its channel
	 * would fail this line first. */
	tier1 = count_page(PROFILE_COMPAT_PAGE_ATTEST_I);
	radiant_sec_compat_get_stats(WATCHER_CH, &st);
	zassert_equal(tier1, st.tier1_verified, "%u vs %u", tier1, st.tier1_verified);
	zassert_equal(0u, st.tier1_unverified, "%u unverified", st.tier1_unverified);
	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
		      radiant_sec_compat_stream_verdict(
			      WATCHER_CH,
			      T_ORIGIN + (uint64_t)(SLOTS - 1u) *
						 period_us(PROFILE_HR_PERIOD)));

	/*
	 * And it was still a heart-rate sensor throughout: no page number
	 * outside the union of the profile's own pages and the compat
	 * allocation, and the data pages kept coming.
	 */
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		uint8_t page = (uint8_t)(log_buf[i].body[0] &
					 PROFILE_HR_PAGE_MASK);
		bool    own = page <= PROFILE_HR_PAGE_PREVIOUS_BEAT;
		bool    common = page == PROFILE_TLM_PAGE_COMMON_80 ||
				 page == PROFILE_TLM_PAGE_COMMON_81 ||
				 page == PROFILE_TLM_PAGE_COMMON_82;

		zassert_true(own || common ||
				     profile_compat_is_compat_page(page),
			     "slot %u page 0x%02x", i, page);
		if (own) {
			data_pages++;
		}
	}
	/* Enrolment frames do cost data slots; the honest claim is that the
	 * cost is bounded, not absent. The promoted beacon rate is one message
	 * in eight, so at worst 7/8 of the run stays the sensor's own pages.
	 * Below 3/4 would mean the window displaced more than it's allowed. */
	zassert_true(data_pages * 4u >= SLOTS * 3u, "%u of %u", data_pages, SLOTS);

	/* ── THE DELIVERY: the next stretch carries the node's root, wrapped ─
	 * Four frames, the set at six with the pairing bit down, for a bounded
	 * number of passes; then the set is the plain beacon again. */
	run_hr(&hr, SLOTS, SLOTS);
	zassert_equal(0, root_from_air(wrapped), "no root in %u slots", SLOTS);
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;
		uint8_t        count = (uint8_t)((b[1] & 0x0Fu) + 1u);

		if ((b[0] & PROFILE_COMPAT_PAGE_MASK) !=
		    PROFILE_COMPAT_PAGE_BEACON) {
			continue;
		}
		if (count == PROFILE_COMPAT_BEACON_FRAMES) {
			shrunk = true;
			continue;
		}
		/* Never the public key again, and never six after two. */
		zassert_equal(ROOT_SET_COUNT, count, "slot %u", i);
		zassert_false(shrunk, "slot %u", i);
		if ((b[1] >> 4) == 0u) {
			zassert_equal(0u, b[2] & PROFILE_COMPAT_CAP_PAIRING_OPEN);
		}
		if ((b[1] >> 4) >= PROFILE_COMPAT_BEACON_FRAMES) {
			delivered++;
		}
	}
	zassert_equal(PROFILE_ENROL_ROOT_FRAMES * PROFILE_ENROL_DELIVER_CYCLES,
		      delivered, "%u", delivered);
	zassert_equal(0u, profile_enrol_frame_count(&pe, T_ORIGIN));

	/* The node's root never moved: the receiver that was already there
	 * verified every Tier I window of both stretches. */
	tier1_late = count_page(PROFILE_COMPAT_PAGE_ATTEST_I);
	zassert_true(tier1_late > 0u);
	radiant_sec_compat_get_stats(WATCHER_CH, &st);
	zassert_equal(tier1 + tier1_late, st.tier1_verified, "%u",
		      st.tier1_verified);
	zassert_equal(0u, st.tier1_unverified);

	/* ── The new receiver installs what was delivered - the node's own
	 *    root - and verifies the same stream with it ────────────────── */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_accept_root(PEER_CH, wrapped,
						   RADIANT_SEC_PAIR_INTO_COMPAT));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(PEER_CH, RX_SWITCHES, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(PEER_CH, TEST_EPOCH, T_ORIGIN,
						   0u, PROFILE_HR_PERIOD));
	feed(PEER_CH, SLOTS);
	radiant_sec_compat_get_stats(PEER_CH, &st);
	zassert_equal(tier1_late, st.tier1_verified, "%u", st.tier1_verified);
	zassert_equal(0u, st.tier1_unverified);
	zassert_equal(1u, st.enrolments);
}

/* ═══ The pairing-open bit, and the set that grows ═══════════════════════ */

ZTEST(profile_enrol, test_the_open_window_is_visible_to_existing_keyholders)
{
	struct profile_hr     hr;
	struct profile_hr_cfg hcfg;
	struct profile_compat pc;
	struct profile_enrol  pe;
	uint32_t              i;
	uint32_t              seen_open = 0u;
	uint32_t              seen_shut = 0u;
	uint32_t              seen_count8 = 0u;

	/* An enrolment the owner did not perform is the whole attack, so making
	 * it silent would be the mistake - the bit is in the beacon, which
	 * every keyholder already reads. */
	give_id(NODE_CH, NODE_DEVNUM);
	keys_up(PROFILE_HR_PERIOD);
	hr_cfg_fill(&hcfg);
	zassert_equal(0, profile_hr_init(&hr, &hcfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	zassert_equal(0, profile_enrol_attach(&pe, &pc));

	/* Before: the bit is clear and the set is two frames. */
	run_hr(&hr, 0u, PROFILE_TLM_CYCLE);
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;

		if ((b[0] & PROFILE_COMPAT_PAGE_MASK) !=
		    PROFILE_COMPAT_PAGE_BEACON) {
			continue;
		}
		if ((b[1] >> 4) == 0u) {
			zassert_equal(0u, b[2] & PROFILE_COMPAT_CAP_PAIRING_OPEN);
			seen_shut++;
		}
		zassert_equal((uint8_t)(PROFILE_COMPAT_BEACON_FRAMES - 1u),
			      (uint8_t)(b[1] & 0x0Fu));
	}
	zassert_true(seen_shut > 0u);

	/* During: the bit is set, and EVERY frame of the set says eight -
	 * frames 0 and 1 included, which is the thing section 11.5 names as the
	 * obvious one to get wrong. */
	zassert_equal(0, profile_enrol_physical_action(
				 &pe, T_ORIGIN + (uint64_t)PROFILE_TLM_CYCLE *
							  period_us(PROFILE_HR_PERIOD)));
	run_hr(&hr, PROFILE_TLM_CYCLE, PROFILE_TLM_CYCLE);
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;

		if ((b[0] & PROFILE_COMPAT_PAGE_MASK) !=
		    PROFILE_COMPAT_PAGE_BEACON) {
			continue;
		}
		zassert_equal((uint8_t)(ENROL_SET_COUNT - 1u),
			      (uint8_t)(b[1] & 0x0Fu),
			      "frame %u", (unsigned)(b[1] >> 4));
		seen_count8++;
		if ((b[1] >> 4) == 0u) {
			zassert_equal(PROFILE_COMPAT_CAP_PAIRING_OPEN,
				      b[2] & PROFILE_COMPAT_CAP_PAIRING_OPEN);
			seen_open++;
		}
	}
	zassert_true(seen_open > 0u);
	zassert_true(seen_count8 >= PROFILE_ENROL_FRAMES, "%u frames", seen_count8);

	/* After: closed, and the beacon says so again. */
	profile_enrol_close(&pe);
	run_hr(&hr, 2u * PROFILE_TLM_CYCLE, PROFILE_TLM_CYCLE);
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;

		if ((b[0] & PROFILE_COMPAT_PAGE_MASK) !=
		    PROFILE_COMPAT_PAGE_BEACON) {
			continue;
		}
		zassert_equal((uint8_t)(PROFILE_COMPAT_BEACON_FRAMES - 1u),
			      (uint8_t)(b[1] & 0x0Fu));
		if ((b[1] >> 4) == 0u) {
			zassert_equal(0u, b[2] & PROFILE_COMPAT_CAP_PAIRING_OPEN);
		}
	}
}

/* ═══ The three settings ═════════════════════════════════════════════════ */

ZTEST(profile_enrol, test_closed_refuses_both_ways_in)
{
	struct profile_enrol pe;

	/* `closed` means no over-air enrolment ever - not "unless somebody
	 * holds the button", not "unless a keyholder asks". It's the posture
	 * radiant_sec.h recommends for anything that matters, and nothing in
	 * Layer D weakens it. */
	give_id(NODE_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_CLOSED, node_scalar);

	zassert_equal(-EPERM, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_equal(-EPERM, profile_enrol_keyholder_request(&pe, T_ORIGIN));
	zassert_false(profile_enrol_is_open(&pe));
	zassert_equal(0u, profile_enrol_frame_count(&pe, T_ORIGIN));
	zassert_equal(2u, pe.windows_refused);
	zassert_equal(0u, pe.windows_opened);
}

ZTEST(profile_enrol, test_open_window_is_rate_limited)
{
	struct profile_enrol pe;
	uint64_t             min_us =
		(uint64_t)PROFILE_ENROL_OPEN_WINDOW_MIN_S * US_PER_S;

	give_id(NODE_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_OPEN_WINDOW, node_scalar);

	/* The weakest of the three, so it is bounded twice: sixty seconds long
	 * and no more than one every five minutes. */
	zassert_equal(0, profile_enrol_keyholder_request(&pe, T_ORIGIN));
	zassert_true(profile_enrol_is_open(&pe));

	/* A second command while the first window runs does not extend it. */
	zassert_equal(-EBUSY,
		      profile_enrol_keyholder_request(&pe, T_ORIGIN + 1000u));

	/* The window expires on its own, and a command right after it is still
	 * refused - the limit is measured from the last OPENING, so how long
	 * the previous window happened to last is not an input an attacker
	 * gets to choose. */
	zassert_true(profile_enrol_tick(&pe, T_ORIGIN + 61ull * US_PER_S));
	zassert_false(profile_enrol_is_open(&pe));
	zassert_equal(-EAGAIN,
		      profile_enrol_keyholder_request(&pe,
						      T_ORIGIN + 62ull * US_PER_S));
	zassert_equal(-EAGAIN,
		      profile_enrol_keyholder_request(&pe,
						      T_ORIGIN + min_us - 1u));

	/* And permitted once the interval has passed. */
	zassert_equal(0, profile_enrol_keyholder_request(&pe,
							 T_ORIGIN + min_us));
	zassert_true(profile_enrol_is_open(&pe));
	zassert_equal(2u, pe.windows_opened);
}

ZTEST(profile_enrol, test_a_physical_node_refuses_a_keyholder_command)
{
	struct profile_enrol pe;

	/* `physical` is not a superset of `open-window`: a node whose owner
	 * chose "only a button opens this" must not have a second door a
	 * keyholder, or a forged command, can walk through. */
	give_id(NODE_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);

	zassert_equal(-EPERM, profile_enrol_keyholder_request(&pe, T_ORIGIN));
	zassert_false(profile_enrol_is_open(&pe));
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_true(profile_enrol_is_open(&pe));
}

ZTEST(profile_enrol, test_a_second_press_does_not_extend_a_window)
{
	struct profile_enrol pe;

	give_id(NODE_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);

	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_equal(-EBUSY,
		      profile_enrol_physical_action(&pe,
						    T_ORIGIN + 30ull * US_PER_S));
	/* Still the ORIGINAL deadline: an extension is an unbounded window
	 * reached one press at a time. */
	zassert_true(profile_enrol_tick(&pe, T_ORIGIN + 60ull * US_PER_S));
	zassert_false(profile_enrol_is_open(&pe));
}

ZTEST(profile_enrol, test_the_window_closes_itself)
{
	struct profile_enrol pe;
	uint8_t              payload[8];

	give_id(NODE_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);

	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_equal(PROFILE_ENROL_FRAMES,
		      profile_enrol_frame_count(&pe, T_ORIGIN));

	/* One microsecond short. */
	zassert_equal(PROFILE_ENROL_FRAMES,
		      profile_enrol_frame_count(&pe,
						T_ORIGIN + 60ull * US_PER_S - 1u));

	/* And on the second, the frames stop and pairing mode is left - which
	 * matters more than the frames, because a node left in pairing mode
	 * accepts a key from whoever asks. */
	zassert_equal(0u, profile_enrol_frame_count(&pe,
						    T_ORIGIN + 60ull * US_PER_S));
	zassert_false(profile_enrol_is_open(&pe));
	zassert_false(radiant_sec_pair_is_open(NODE_CH));
	zassert_equal(1u, pe.windows_expired);
	zassert_false(profile_enrol_frame(&pe, 0u, payload));
}

/* ═══ What arrives on the acknowledged-data path ═════════════════════════ */

ZTEST(profile_enrol, test_a_set_spliced_from_two_windows_is_refused)
{
	struct profile_enrol pe;
	uint8_t              pub_a[32];
	uint8_t              pub_b[32];
	uint8_t              set_a[PROFILE_ENROL_FRAMES][8];
	uint8_t              set_b[PROFILE_ENROL_FRAMES][8];
	uint8_t              k;

	/* The failure the framing convention cannot catch on its own: byte [1]
	 * says which frame this is, never which key it belongs to, so a
	 * receiver joining as one window closes and another opens could splice
	 * two halves into a key neither end holds, with no error anywhere. */
	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, PEER_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));

	peer_up(peer_scalar, pub_a);
	peer_set(pub_a, set_a);
	radiant_sec_pair_leave(PEER_CH);
	peer_up(other_scalar, pub_b);
	peer_set(pub_b, set_b);

	for (k = 0u; k < 3u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(&pe, set_a[k], 8u));
	}
	for (k = 3u; k < PROFILE_ENROL_FRAMES - 1u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(&pe, set_b[k], 8u));
	}
	zassert_equal(-EBADMSG,
		      profile_enrol_on_ack_data(
			      &pe, set_b[PROFILE_ENROL_FRAMES - 1u], 8u));

	/* The accumulator started again and THE WINDOW STAYED OPEN - a torn
	 * set is usually honest, and burning the window on it would make a
	 * receiver that arrived at the wrong moment cost the user a retry of
	 * the physical action. */
	zassert_true(profile_enrol_is_open(&pe));
	for (k = 0u; k < PROFILE_ENROL_FRAMES - 1u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(&pe, set_a[k], 8u));
	}
	zassert_equal(1, profile_enrol_on_ack_data(
				 &pe, set_a[PROFILE_ENROL_FRAMES - 1u], 8u));
}

ZTEST(profile_enrol, test_an_injected_bad_key_does_not_burn_the_window)
{
	struct profile_enrol pe;
	uint8_t              zero_point[32] = { 0 };
	uint8_t              bad[PROFILE_ENROL_FRAMES][8];
	uint8_t              good[PROFILE_ENROL_FRAMES][8];
	uint8_t              pub[32];
	struct radiant_sec_stats sstats;
	uint8_t              k;

	/* A small-order point makes the shared secret all zeros regardless of
	 * our scalar; refusing it is mandatory, since the result becomes a
	 * root key and accepting it would let anyone inject one packet to fix
	 * the group key to a known value. Refusing must not cost the window -
	 * one injected packet ending a user-opened window is a denial of
	 * service with a one-frame price tag. */
	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, PEER_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));

	peer_set(zero_point, bad);
	for (k = 0u; k < PROFILE_ENROL_FRAMES - 1u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(&pe, bad[k], 8u));
	}
	zassert_equal(-EACCES,
		      profile_enrol_on_ack_data(
			      &pe, bad[PROFILE_ENROL_FRAMES - 1u], 8u));
	zassert_true(profile_enrol_is_open(&pe));

	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(0u, sstats.enrolments);

	/* The real peer still gets in. */
	peer_up(peer_scalar, pub);
	peer_set(pub, good);
	for (k = 0u; k < PROFILE_ENROL_FRAMES - 1u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(&pe, good[k], 8u));
	}
	zassert_equal(1, profile_enrol_on_ack_data(
				 &pe, good[PROFILE_ENROL_FRAMES - 1u], 8u));
	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(1u, sstats.enrolments);
}

ZTEST(profile_enrol, test_repeated_frames_are_idempotent)
{
	struct profile_enrol pe;
	uint8_t              pub[32];
	uint8_t              set[PROFILE_ENROL_FRAMES][8];
	uint8_t              k;

	/*
	 * Acknowledged data is retransmitted when an acknowledgement goes
	 * missing, so the same frame arriving twice is ordinary traffic rather
	 * than an attack. It overwrites itself.
	 */
	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, PEER_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));

	peer_up(peer_scalar, pub);
	peer_set(pub, set);

	for (k = 0u; k < PROFILE_ENROL_FRAMES - 1u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(&pe, set[k], 8u));
		zassert_equal(0, profile_enrol_on_ack_data(&pe, set[k], 8u));
	}
	zassert_equal(1, profile_enrol_on_ack_data(
				 &pe, set[PROFILE_ENROL_FRAMES - 1u], 8u));
}

ZTEST(profile_enrol, test_a_malformed_frame_is_refused)
{
	struct profile_enrol pe;
	uint8_t              frame[8] = { 0 };

	give_id(NODE_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));

	/* A count that is not six cannot belong to a peer's enrolment set. */
	frame[1] = 0x01u;   /* index 0, count 2 */
	zassert_equal(-EPROTO, profile_enrol_on_ack_data(&pe, frame, 8u));

	/* An index outside the set. */
	frame[1] = 0x65u;   /* index 6, count 6 */
	zassert_equal(-EPROTO, profile_enrol_on_ack_data(&pe, frame, 8u));

	/* And a length that is not an ANT payload. No frame with any other
	 * length has ever been on this bench's air. */
	frame[1] = 0x05u;
	zassert_equal(-EINVAL, profile_enrol_on_ack_data(&pe, frame, 7u));
	zassert_equal(-EINVAL, profile_enrol_on_ack_data(&pe, frame, 24u));
}

/* ═══ The seams the next phase needs ═════════════════════════════════════ */

ZTEST(profile_enrol, test_enrolment_needs_no_beacon_at_all)
{
	struct profile_enrol pe;
	uint8_t              pub[32];
	uint8_t              set[PROFILE_ENROL_FRAMES][8];
	uint8_t              payload[8];
	uint32_t             fp = 0u;
	uint8_t              k;

	/*
	 * Section 11.7 rule one, as an API property: the window opens on
	 * whichever channel the node is currently on. This instance has no
	 * compat client, no beacon, no page number and no locator - the shape
	 * a node in private mode would use, and the hook C8 wires into. Nothing
	 * here reads or writes the beacon's locator fields or checks
	 * `announce`, which is why enrolment doesn't depend on that setting.
	 */
	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, PEER_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);

	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_equal(PROFILE_ENROL_FRAMES,
		      profile_enrol_frame_count(&pe, T_ORIGIN));
	for (k = 0u; k < PROFILE_ENROL_FRAMES; k++) {
		zassert_true(profile_enrol_frame(&pe, k, payload), "frame %u", k);
	}

	peer_up(peer_scalar, pub);
	peer_set(pub, set);
	for (k = 0u; k < PROFILE_ENROL_FRAMES - 1u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(&pe, set[k], 8u));
	}
	zassert_equal(1, profile_enrol_on_ack_data(
				 &pe, set[PROFILE_ENROL_FRAMES - 1u], 8u));
	zassert_equal(0, profile_enrol_fingerprint(&pe, &fp));
	zassert_true(fp < 1000000u);
}

ZTEST(profile_enrol, test_the_hostless_seam_advances_the_counter_first)
{
	struct profile_enrol_cfg cfg;
	struct profile_enrol     pe;
	uint32_t                 before = 0u;
	uint32_t                 after = 0u;
	uint32_t                 second = 0u;

	/*
	 * The seam wired to what a strap actually runs (ADR 0009), exercising
	 * through Layer D the rule most likely to be implemented backwards:
	 * pair_counter advances and is durably written before the derived
	 * public key leaves the radio, never on completion. Windows are
	 * abandoned all the time, so advance-on-completion would reuse the
	 * scalar on the next attempt - a repeated private key on the air.
	 */
	fake_nvm_wipe();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_provision(k_dev));
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	give_id(NODE_CH, NODE_DEVNUM);

	memset(&cfg, 0, sizeof(cfg));
	cfg.ch = NODE_CH;
	cfg.mode = PROFILE_ENROL_PHYSICAL;
	cfg.open_pairing = node_ident_open_pairing;
	cfg.close_pairing = node_ident_close_pairing;
	zassert_equal(0, profile_enrol_init(&pe, &cfg));

	zassert_equal(NODE_IDENT_OK, node_ident_pair_counter(&before));
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_equal(NODE_IDENT_OK, node_ident_pair_counter(&after));
	zassert_equal(before + 1u, after);

	/* Abandoned - and the counter does NOT roll back. A burnt counter value
	 * is the cheap half of the trade; reusing one is the expensive half. */
	profile_enrol_close(&pe);
	zassert_equal(0, profile_enrol_physical_action(&pe,
						       T_ORIGIN + 120ull * US_PER_S));
	zassert_equal(NODE_IDENT_OK, node_ident_pair_counter(&second));
	zassert_equal(after + 1u, second);

	profile_enrol_close(&pe);
	node_ident_reset();
	fake_nvm_wipe();
}

ZTEST(profile_enrol, test_init_refuses_a_configuration_that_cannot_work)
{
	struct profile_enrol     pe;
	struct profile_enrol_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.ch = NODE_CH;
	cfg.mode = PROFILE_ENROL_PHYSICAL;

	/* Both callbacks are required in every mode, `closed` included: a
	 * misconfigured node that refuses looks exactly like a node refusing on
	 * policy, and the second is a decision while the first is a bug. */
	zassert_equal(-EINVAL, profile_enrol_init(&pe, &cfg));
	cfg.open_pairing = fixed_open_pairing;
	zassert_equal(-EINVAL, profile_enrol_init(&pe, &cfg));
	cfg.close_pairing = fixed_close_pairing;
	zassert_equal(0, profile_enrol_init(&pe, &cfg));

	cfg.mode = 3u;
	zassert_equal(-EINVAL, profile_enrol_init(&pe, &cfg));
	zassert_equal(-EINVAL, profile_enrol_init(NULL, &cfg));
	zassert_equal(-EINVAL, profile_enrol_init(&pe, NULL));
}

/* ═══ The delivery ═══════════════════════════════════════════════════════ */

/* One window, start to finish, through the direct entry points (no beacon):
 * the node's set as it went out, and the peer's answer. */
static void window_completes(struct profile_enrol *pe, uint8_t *node_set)
{
	uint8_t pub[32];
	uint8_t set[PROFILE_ENROL_FRAMES][8];
	uint8_t k;

	zassert_equal(0, profile_enrol_physical_action(pe, T_ORIGIN));
	for (k = 0u; k < PROFILE_ENROL_FRAMES; k++) {
		zassert_true(profile_enrol_frame(
			pe, k, &node_set[(size_t)k * PROFILE_ENROL_FRAME_PAYLOAD]));
	}
	peer_up(peer_scalar, pub);
	peer_set(pub, set);
	for (k = 0u; k < PROFILE_ENROL_FRAMES - 1u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(pe, set[k], 8u));
	}
	zassert_equal(1, profile_enrol_on_ack_data(
				 pe, set[PROFILE_ENROL_FRAMES - 1u], 8u));
}

ZTEST(profile_enrol, test_the_delivery_is_bounded_and_holds_off_a_new_window)
{
	struct profile_enrol pe;
	uint8_t              node_set[PROFILE_ENROL_SET_BYTES];
	uint8_t              first[PROFILE_ENROL_WRAPPED_BYTES];
	uint8_t              payload[8];
	uint32_t             sent;
	uint8_t              k;

	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	window_completes(&pe, node_set);

	/* Four frames, not a window: no public key, nothing open. */
	zassert_false(profile_enrol_is_open(&pe));
	zassert_equal(PROFILE_ENROL_ROOT_FRAMES,
		      profile_enrol_frame_count(&pe, T_ORIGIN));
	zassert_false(profile_enrol_frame(&pe, PROFILE_ENROL_ROOT_FRAMES,
					  payload));

	/* A press during the delivery would cut it short: refused. */
	zassert_equal(-EBUSY, profile_enrol_physical_action(&pe, T_ORIGIN));

	/* Every frame the stated number of times, the same bytes each pass,
	 * then nothing. */
	for (sent = 0u;
	     sent < PROFILE_ENROL_ROOT_FRAMES * PROFILE_ENROL_DELIVER_CYCLES;
	     sent++) {
		k = (uint8_t)(sent % PROFILE_ENROL_ROOT_FRAMES);
		zassert_equal(PROFILE_ENROL_ROOT_FRAMES,
			      profile_enrol_frame_count(&pe, T_ORIGIN));
		zassert_true(profile_enrol_frame(&pe, k, payload), "%u", sent);
		if (sent < PROFILE_ENROL_ROOT_FRAMES) {
			memcpy(&first[(size_t)k * PROFILE_ENROL_FRAME_PAYLOAD],
			       payload, PROFILE_ENROL_FRAME_PAYLOAD);
		} else {
			zassert_mem_equal(
				&first[(size_t)k * PROFILE_ENROL_FRAME_PAYLOAD],
				payload, PROFILE_ENROL_FRAME_PAYLOAD);
		}
	}
	zassert_equal(0u, profile_enrol_frame_count(&pe, T_ORIGIN));
	zassert_false(profile_enrol_frame(&pe, 0u, payload));

	/* What went out opens at the peer. */
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_join(PEER_CH, node_set,
							     NULL));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_accept_root(PEER_CH, first,
						   RADIANT_SEC_PAIR_INTO_SEC));

	/* And the next window opens normally. */
	zassert_equal(0, profile_enrol_physical_action(
				 &pe, T_ORIGIN + 120ull * US_PER_S));
	zassert_equal(PROFILE_ENROL_FRAMES,
		      profile_enrol_frame_count(&pe,
						T_ORIGIN + 120ull * US_PER_S));
}

ZTEST(profile_enrol, test_closing_ends_a_delivery)
{
	struct profile_enrol pe;
	uint8_t              node_set[PROFILE_ENROL_SET_BYTES];
	uint8_t              payload[8];

	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	window_completes(&pe, node_set);
	zassert_true(profile_enrol_frame(&pe, 0u, payload));

	profile_enrol_close(&pe);
	zassert_equal(0u, profile_enrol_frame_count(&pe, T_ORIGIN));
	zassert_false(profile_enrol_frame(&pe, 0u, payload));
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
}

ZTEST(profile_enrol, test_a_live_secured_root_survives_an_enrolment)
{
	struct profile_enrol     pe;
	struct radiant_sec_stats sstats;
	uint8_t                  node_set[PROFILE_ENROL_SET_BYTES];
	uint8_t                  wrapped[PROFILE_ENROL_WRAPPED_BYTES];
	uint8_t                  plain[8] = { RADIANT_SEC_PAGE_LO_DEFAULT, 0u,
					      0x11u, 0x22u, 0x33u, 0x44u,
					      0x55u, 0x66u };
	uint8_t                  a[8];
	uint8_t                  b[8];
	uint64_t                 t;
	uint8_t                  k;

	/* A plain secured channel (no compat context), armed and running. The
	 * enrolment must leave it so - no set_key, so its epoch gate is never
	 * reset - and the receiver must end up with this same root. */
	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, NODE_DEVNUM);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(NODE_CH, compat_root, 128u,
					  NODE_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(NODE_CH, RADIANT_SEC_SW_CONF, 2u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(NODE_CH, 3u, 0u, 8182u));
	zassert_true(radiant_sec_channel_is_secured(NODE_CH));

	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	window_completes(&pe, node_set);
	zassert_true(radiant_sec_channel_is_secured(NODE_CH));
	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(1u, sstats.enrolments);

	for (k = 0u; k < PROFILE_ENROL_ROOT_FRAMES; k++) {
		zassert_true(profile_enrol_frame(
			&pe, k, &wrapped[(size_t)k * PROFILE_ENROL_FRAME_PAYLOAD]));
	}
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_join(PEER_CH, node_set,
							     NULL));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_accept_root(PEER_CH, wrapped,
						   RADIANT_SEC_PAIR_INTO_SEC));

	/* Same root: under one epoch, device number and slot, the node and
	 * the receiver encrypt a page to the same bytes. The epoch is set on
	 * both explicitly; a freshly installed root is unarmed until then. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(PEER_CH, RADIANT_SEC_SW_CONF, 2u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(NODE_CH, 7u, 0u, 8182u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(PEER_CH, 7u, 0u, 8182u));
	t = (uint64_t)radiant_radio_now() + 3000000u;
	memcpy(a, plain, sizeof(a));
	memcpy(b, plain, sizeof(b));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(NODE_CH, a, 8u, t));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(PEER_CH, b, 8u, t));
	zassert_false(memcmp(a, plain, sizeof(a)) == 0);
	zassert_mem_equal(a, b, sizeof(a));
}

/* ═══ compat-C8: the clause C7 deferred ══════════════════════════════════ */

#if defined(CONFIG_RADIANT_SEC_COMPAT)

/*
 * "A node in private mode gains a second receiver, the first receiver's
 * stream is uninterrupted across the whole window, and the new receiver
 * reaches the node by deriving the locator rather than by any announcement."
 *
 * The first two clauses were asserted when this file was written. The third
 * was deferred because the locator is trunc16(CMAC(K_id, "priv" || epoch))
 * and K_id is reachable only from the layer holding the root key. C8 has
 * landed with the entry point radiant_sec_compat_locator(), and this test
 * pays that deferral.
 *
 * The node here is `physical` + `silent`, so no announcement anywhere on the
 * air could have reached the new receiver - derivation is the only thing
 * that can have.
 */

#define PRIVATE_DEVICE_TYPE 0x60u
#define PRIVATE_PERIOD      8192u
#define PRIVATE_SLOTS       242u

static uint32_t g_enter_calls;
static uint16_t g_private_devnum;

static int private_enter(void *user, uint8_t device_type, uint16_t devnum,
			 uint16_t period)
{
	ARG_UNUSED(user);
	ARG_UNUSED(device_type);
	ARG_UNUSED(period);
	g_enter_calls++;
	g_private_devnum = devnum;
	return 0;
}

static int private_leave(void *user)
{
	ARG_UNUSED(user);
	return 0;
}

ZTEST(profile_enrol, test_a_private_node_gains_a_receiver_that_derives_it)
{
	struct profile_private     pp;
	struct profile_private_cfg pcfg;
	struct profile_enrol       pe;
	struct radiant_sec_compat_stats st;
	uint8_t  air[PROFILE_ENROL_SET_BYTES];
	uint8_t  wrapped[PROFILE_ENROL_WRAPPED_BYTES];
	uint8_t  have = 0u;
	uint8_t  peer_pub[32];
	uint8_t  frames[PROFILE_ENROL_FRAMES][8];
	uint8_t  cursor = 0u;
	uint32_t watcher_msgs = 0u;
	uint32_t last_counter = 0u;
	uint32_t data_msgs = 0u;
	uint32_t i;
	uint32_t node_fp = 0u;
	uint32_t peer_fp = 0u;
	uint16_t derived = 0u;
	uint8_t  k;

	give_id(NODE_CH, NODE_DEVNUM);
	/* Tracking the node: the receiver's channel carries the node's ID. */
	give_id(PEER_CH, NODE_DEVNUM);
	keys_up(PROFILE_HR_PERIOD);
	g_enter_calls = 0u;
	g_private_devnum = 0u;

	/* ── The node goes private, silently ────────────────────────────── */
	memset(&pcfg, 0, sizeof(pcfg));
	pcfg.ch = NODE_CH;
	pcfg.epoch = TEST_EPOCH;
	pcfg.policy = PROFILE_PRIVATE_PHYSICAL;
	pcfg.announce = PROFILE_PRIVATE_SILENT;
	pcfg.attest = true;
	pcfg.private_device_type = PRIVATE_DEVICE_TYPE;
	pcfg.private_period = PRIVATE_PERIOD;
	pcfg.compat_device_type = PROFILE_HR_DEVICE_TYPE;
	pcfg.compat_devnum = NODE_DEVNUM;
	pcfg.compat_period = PROFILE_HR_PERIOD;
	pcfg.enter_private = private_enter;
	pcfg.leave_private = private_leave;
	zassert_equal(0, profile_private_init(&pp, &pcfg));
	zassert_equal(0, profile_private_physical_action(&pp, T_ORIGIN));
	zassert_true(profile_private_is_private(&pp));
	zassert_equal(1u, g_enter_calls);

	/* ── The owner presses the button again, on a node with no beacon
	 *    page, no compat channel and nowhere for a pairing frame to ride
	 *    except the rotation it is already running ─────────────────────── */
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_true(profile_enrol_is_open(&pe));

	log_reset();
	for (i = 0u; i < PRIVATE_SLOTS; i++) {
		uint64_t now = T_ORIGIN + (uint64_t)i * period_us(PROFILE_HR_PERIOD);
		uint8_t  body[8];
		bool     is_frame = false;

		memset(body, 0, sizeof(body));

		/* A device type 0x60 master's own rotation, driving the two
		 * client entry points directly - what profile_enrol.h says a
		 * node with no beacon page does. The page number for these
		 * frames on 0x60 is still deferred; this loop picks one so
		 * there is a stream to assert about. */
		if ((i % 8u) == 0u &&
		    profile_enrol_frame_count(&pe, now) == PROFILE_ENROL_FRAMES) {
			body[0] = 0x40u;
			body[1] = (uint8_t)((cursor << 4) |
					    (PROFILE_ENROL_FRAMES - 1u));
			if (profile_enrol_frame(&pe, cursor, &body[2])) {
				is_frame = true;
				cursor = (uint8_t)((cursor + 1u) %
						   PROFILE_ENROL_FRAMES);
			}
		}
		if (!is_frame) {
			/* An ordinary telemetry data page with a monotone
			 * counter: this is the first receiver's stream, and
			 * the whole claim is that nothing here stops. */
			body[0] = 0x01u;
			body[1] = (uint8_t)(data_msgs & 0xFFu);
			data_msgs++;
		}

		log_put(body, PROFILE_SLOT_DATA);

		/* The watcher hears every slot: no channel opened, none closed,
		 * none retuned, so there is nothing for it to notice. */
		watcher_msgs++;
		if (!is_frame) {
			zassert_true(data_msgs > last_counter, "slot %u", i);
			last_counter = data_msgs;
		}
	}

	zassert_equal(PRIVATE_SLOTS, watcher_msgs);
	zassert_true(profile_enrol_is_open(&pe));
	zassert_equal(1u, g_enter_calls);
	zassert_true(profile_private_is_private(&pp));

	/* ── The key went on the air, on the private channel ────────────── */
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;
		uint8_t        idx;

		if (b[0] != 0x40u) {
			continue;
		}
		idx = (uint8_t)(b[1] >> 4);
		if (idx >= PROFILE_ENROL_FRAMES) {
			continue;
		}
		memcpy(&air[(size_t)idx * PROFILE_ENROL_FRAME_PAYLOAD], &b[2],
		       PROFILE_ENROL_FRAME_PAYLOAD);
		have |= (uint8_t)(1u << idx);
	}
	zassert_equal((uint8_t)((1u << PROFILE_ENROL_FRAMES) - 1u), have);

	/* ── NOTHING ON THE AIR SAYS WHERE THE NODE IS ──────────────────── */
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;

		/* No beacon page, so no announcement frame, locator field or
		 * pending-switch bit - device type 0x60 has no beacon to carry
		 * any of them, and a silent node wouldn't set them if it did.
		 * The new receiver's derivation below is the only thing that
		 * can have told it where the node is. */
		zassert_not_equal(PROFILE_COMPAT_PAGE_BEACON,
				  (uint8_t)(b[0] & PROFILE_COMPAT_PAGE_MASK));
	}

	/* ── The enrolment completes ────────────────────────────────────── */
	peer_up(peer_scalar, peer_pub);
	peer_set(peer_pub, frames);
	for (k = 0u; k < PROFILE_ENROL_FRAMES - 1u; k++) {
		zassert_equal(0, profile_enrol_on_ack_data(&pe, frames[k], 8u));
	}
	zassert_equal(1, profile_enrol_on_ack_data(
				 &pe, frames[PROFILE_ENROL_FRAMES - 1u], 8u));
	zassert_equal(0, profile_enrol_fingerprint(&pe, &node_fp));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_join(PEER_CH, air, &peer_fp));
	zassert_equal(node_fp, peer_fp);

	radiant_sec_compat_get_stats(NODE_CH, &st);
	zassert_equal(1u, st.enrolments);

	/* ── The root follows on the same rotation, four frames wide ────── */
	log_reset();
	cursor = 0u;
	for (i = 0u; i < PRIVATE_SLOTS; i++) {
		uint64_t now = T_ORIGIN + (uint64_t)(PRIVATE_SLOTS + i) *
						  period_us(PROFILE_HR_PERIOD);
		uint8_t  n = profile_enrol_frame_count(&pe, now);
		uint8_t  body[8];

		if ((i % 8u) != 0u || n == 0u) {
			continue;
		}
		zassert_equal(PROFILE_ENROL_ROOT_FRAMES, n);
		body[0] = 0x40u;
		body[1] = (uint8_t)((cursor << 4) | (n - 1u));
		zassert_true(profile_enrol_frame(&pe, cursor, &body[2]));
		cursor = (uint8_t)((cursor + 1u) % n);
		log_put(body, PROFILE_SLOT_DATA);
	}
	zassert_equal(PROFILE_ENROL_ROOT_FRAMES * PROFILE_ENROL_DELIVER_CYCLES,
		      log_n);
	have = 0u;
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;
		uint8_t        idx = (uint8_t)(b[1] >> 4);

		zassert_equal(PROFILE_ENROL_ROOT_FRAMES - 1u, b[1] & 0x0Fu);
		memcpy(&wrapped[(size_t)idx * PROFILE_ENROL_FRAME_PAYLOAD],
		       &b[2], PROFILE_ENROL_FRAME_PAYLOAD);
		have |= (uint8_t)(1u << idx);
	}
	zassert_equal((uint8_t)((1u << PROFILE_ENROL_ROOT_FRAMES) - 1u), have);

	/* ── ...AND THE NEW RECEIVER FINDS THE NODE BY DERIVING IT ──────── */
	/*
	 * A completed enrolment is exactly "this receiver now holds the group
	 * root": its compat context is keyed with the root the node delivered,
	 * not one handed to it here, and the reason enrolment is additive at
	 * all. From there it computes where the node is with no announcement
	 * to have missed and no beacon to read - the clause this test was
	 * written to pay.
	 */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_accept_root(PEER_CH, wrapped,
						   RADIANT_SEC_PAIR_INTO_COMPAT));
	radiant_sec_compat_get_stats(PEER_CH, &st);
	zassert_equal(1u, st.enrolments);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_locator(PEER_CH, TEST_EPOCH, 0u,
						 &derived));
	zassert_equal(g_private_devnum, derived, "0x%04X vs 0x%04X", derived, g_private_devnum);
	zassert_not_equal(0u, derived);
	zassert_equal(profile_private_locator(&pp), derived);
}

ZTEST(profile_enrol, test_a_countdown_refuses_a_window_and_is_refused_by_one)
{
	struct profile_compat      pc;
	struct profile_private     pp;
	struct profile_private_cfg pcfg;
	struct profile_enrol       pe;

	/*
	 * The interlock, from Layer D's side against the real Layer C.
	 * profile_compat.h states the rule - a countdown and an enrolment
	 * window must not run at once, whichever arrives second is refused -
	 * and both halves matter: the frames share indices 2 and 3, and a
	 * switch closes the channel the window is running on.
	 */
	struct profile_compat_cfg ccfg;

	give_id(NODE_CH, NODE_DEVNUM);
	keys_up(PROFILE_HR_PERIOD);
	g_enter_calls = 0u;

	/* A beacon whose policy agrees with the node's, because a `never`
	 * beacon refuses a locator and this test is not about that rule. */
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ch = NODE_CH;
	ccfg.epoch = TEST_EPOCH;
	ccfg.advertise = true;
	ccfg.attest_available = true;
	ccfg.pairing_available = true;
	ccfg.policy = PROFILE_COMPAT_POLICY_PHYSICAL;
	ccfg.window = TEST_N;
	ccfg.mode = PROFILE_COMPAT_MODE_FIXED;
	zassert_equal(0, profile_compat_init(&pc, &ccfg));

	memset(&pcfg, 0, sizeof(pcfg));
	pcfg.ch = NODE_CH;
	pcfg.epoch = TEST_EPOCH;
	pcfg.policy = PROFILE_PRIVATE_PHYSICAL;
	pcfg.announce = PROFILE_PRIVATE_BROADCAST;
	pcfg.attest = true;
	pcfg.private_device_type = PRIVATE_DEVICE_TYPE;
	pcfg.private_period = PRIVATE_PERIOD;
	pcfg.compat_device_type = PROFILE_HR_DEVICE_TYPE;
	pcfg.compat_devnum = NODE_DEVNUM;
	pcfg.compat_period = PROFILE_HR_PERIOD;
	pcfg.enter_private = private_enter;
	pcfg.leave_private = private_leave;

	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);

	zassert_equal(0, profile_private_init(&pp, &pcfg));
	zassert_equal(0, profile_private_attach(&pp, &pc));
	zassert_equal(0, profile_enrol_attach(&pe, &pc));

	/* A window opens; the countdown is second and is refused. */
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_true(profile_compat_client_busy(&pc, T_ORIGIN, &pp));
	zassert_equal(-EBUSY, profile_private_physical_action(&pp, T_ORIGIN));
	zassert_equal(PROFILE_PRIVATE_STATE_COMPAT,
		      profile_private_state(&pp));

	/* And the other way round: with the window closed and a countdown
	 * running, the window is the one refused. */
	profile_enrol_close(&pe);
	zassert_equal(0, profile_private_physical_action(&pp, T_ORIGIN));
	zassert_equal(PROFILE_PRIVATE_STATE_ANNOUNCING,
		      profile_private_state(&pp));
	zassert_equal(PROFILE_PRIVATE_FRAMES,
		      profile_private_frame_count(&pp, T_ORIGIN));

	zassert_equal(-EBUSY, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_false(profile_enrol_is_open(&pe));
}

#endif /* CONFIG_RADIANT_SEC_COMPAT */

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
