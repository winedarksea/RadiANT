/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_enrol.c - compat-C7's gate: a node gains a SECOND receiver
 * without the first one noticing.
 *
 * ---------------------------------------------------------------------------
 * The load-bearing claim, and what this file can and cannot assert today
 * ---------------------------------------------------------------------------
 * The plan's sentence is: "a node in private mode gains a second receiver, the
 * first receiver's stream is uninterrupted across the whole window, and the new
 * receiver reaches the node by deriving the locator rather than by any
 * announcement."
 *
 * Three clauses, and they do not all belong to this phase:
 *
 *   GAINS A SECOND RECEIVER              asserted here, end to end, with the
 *                                        node's public key read back OFF THE
 *                                        AIR and the peer's answer arriving as
 *                                        eight-byte acknowledged-data payloads
 *   UNINTERRUPTED ACROSS THE WHOLE       asserted here, over a full 60-second
 *   WINDOW                               window of a real heart-rate stream:
 *                                        no silent slot, no channel change, no
 *                                        page number outside the profile's own
 *                                        union, a monotone event counter and
 *                                        EVERY Tier I window still verifying at
 *                                        the receiver that was already there
 *   IN PRIVATE MODE, BY DERIVING THE     asserted here since compat-C8, in
 *   LOCATOR                              test_a_private_node_gains_a_receiver_
 *                                        that_derives_it, against a node that
 *                                        is genuinely private and genuinely
 *                                        silent
 *
 * The third clause was deferred by C7 rather than faked, and the reason was a
 * boundary rather than effort: the locator is trunc16(CMAC(K_id, "priv" ||
 * epoch)) and K_id is reachable only from the layer that holds the root key.
 * Deriving it from a test would have meant inventing C8's entry point from the
 * outside and then having C8 either adopt it or contradict it. What C7 owed and
 * paid instead was the property that makes the third clause POSSIBLE: enrolment
 * does not depend on the `announce` setting, does not read or write the
 * beacon's locator fields, and does not need a compat instance at all - so it
 * works on whichever channel the node happens to be on, including one that
 * phase could not yet build. test_enrolment_needs_no_beacon_at_all is that
 * assertion, and it is what the C8 test at the bottom of this file stands on.
 *
 * The other half of the deferral stands, recorded so it is a decision: on
 * device type 0x60 the enrolment frames would need a page number in the
 * telemetry envelope's own namespace, and allocating one is an envelope change.
 * That belongs with the phase that creates a reason for a node to be there.
 * C8's RETURN frames inherit exactly the same deferral, which is why the C8
 * test picks a page number locally rather than either module naming one.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant_core/radiant_sec.h>
#include <radiant_core/radiant_sec_compat.h>
#include <radiant_core/radiant_channel.h>

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
 * receiver that was already there. That second half is the point: the watcher
 * is a PINNED receiver, so a stream that lost its attestation reads UNVERIFIED
 * rather than falling back to clear and looking normal.
 */
static void run_hr(struct profile_hr *hr, uint32_t first_slot, uint32_t slots)
{
	uint64_t period = period_us(PROFILE_HR_PERIOD);
	uint32_t i;

	/*
	 * `first_slot` rather than always starting at zero, so a test can run
	 * the strap in several stretches without the clock going backwards
	 * under the attestation counter - which would be a replay to the
	 * receiver, and a test failure that meant nothing.
	 */
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
		zassert_not_equal(PROFILE_SLOT_IDLE, kind,
				  "slot %u went silent while a pairing window "
				  "was open; the first receiver's stream is not "
				  "uninterrupted if the node stops talking", n);
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
 * Reassemble the node's public key from the LOGGED STREAM - which is the only
 * thing a new receiver has. Nothing is read out of the node's own state, so a
 * pass here means the key genuinely went on the air in frames a stranger can
 * find.
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
	uint8_t                  peer_pub[32];
	uint8_t                  frames[PROFILE_ENROL_FRAMES][8];
	uint32_t                 node_fp = 0u;
	uint32_t                 peer_fp = 0u;
	uint32_t                 tier1;
	uint32_t                 data_pages = 0u;
	uint32_t                 i;
	int                      rc;
	uint8_t                  k;

	/*
	 * THE WHOLE OF LAYER D IN ONE TEST.
	 *
	 * A strap that is already running, with a keyholder already listening
	 * and verifying. The owner presses the button. Sixty seconds later the
	 * strap has a second keyholder, and the first one saw a capability bit
	 * go up and back down and nothing else: no channel change, no re-key,
	 * no epoch move, no silent slot and no unverified window.
	 */
	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, PEER_DEVNUM);
	keys_up(PROFILE_HR_PERIOD);

	hr_cfg_fill(&hcfg);
	zassert_equal(0, profile_hr_init(&hr, &hcfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);
	zassert_equal(0, profile_enrol_attach(&pe, &pc));

	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(0u, sstats.enrolments,
		      "the enrolment counter started non-zero");

	/* The physical action. */
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_true(profile_enrol_is_open(&pe));

	run_hr(&hr, 0u, SLOTS);

	/* ── The window ran to the end of the run ───────────────────────── */
	zassert_true(profile_enrol_is_open(&pe),
		     "the window closed inside the run; the assertions below "
		     "would then be about a node that had stopped pairing");

	/* ── The key went on the air, in frames a stranger can assemble ── */
	zassert_equal(0, pubkey_from_air(air),
		      "the node's public key never completed on the air in %u "
		      "slots; a 60-second window that cannot transmit 32 bytes "
		      "is a window that cannot enrol anybody", SLOTS);
	check_of(air, check);
	zassert_mem_equal(check, &air[PROFILE_ENROL_PUBKEY_BYTES],
			  PROFILE_ENROL_CHECK_BYTES,
			  "the set check on the air does not match the key it "
			  "covers");

	/* ── The peer answers over acknowledged data ────────────────────── */
	peer_up(peer_scalar, peer_pub);
	peer_set(peer_pub, frames);

	for (k = 0u; k < PROFILE_ENROL_FRAMES; k++) {
		rc = profile_enrol_on_ack_data(&pe, frames[k], 8u);
		if (k + 1u < PROFILE_ENROL_FRAMES) {
			zassert_equal(0, rc,
				      "frame %u completed the set early", k);
		} else {
			zassert_equal(1, rc,
				      "the sixth frame did not complete the "
				      "enrolment (rc %d)", rc);
		}
	}

	/* ── Both ends reach the same six digits ────────────────────────── */
	zassert_equal(0, profile_enrol_fingerprint(&pe, &node_fp));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_peer(PEER_CH, air, &peer_fp));
	zassert_equal(node_fp, peer_fp,
		      "the two ends computed different fingerprints, %u vs %u; "
		      "the one-sided comparison this design admits to is at "
		      "least supposed to be a comparison", node_fp, peer_fp);
	zassert_true(node_fp < 1000000u, "the fingerprint is not six digits");

	/* ── The counter moved exactly once ─────────────────────────────── */
	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(1u, sstats.enrolments,
		      "expected exactly one enrolment on the node's channel, "
		      "got %u", sstats.enrolments);

	/* ── ONE PAIRING PER WINDOW ─────────────────────────────────────── */
	zassert_false(profile_enrol_is_open(&pe),
		      "the window survived a completed enrolment; one pairing "
		      "per window is the mitigation, not a preference");
	zassert_equal(-EINVAL, profile_enrol_on_ack_data(&pe, frames[0], 8u));
	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(1u, sstats.enrolments, "a second answer moved the "
		      "counter a second time");

	/* ── AND THE FIRST RECEIVER'S STREAM WAS UNINTERRUPTED ──────────── */

	/*
	 * Not "mostly". Every Tier I window across the whole run verified at
	 * the receiver that was already there, and none failed. A node that had
	 * re-keyed, moved its epoch, changed its device number or dropped its
	 * channel would fail this line first.
	 */
	tier1 = count_page(PROFILE_COMPAT_PAGE_ATTEST_I);
	radiant_sec_compat_get_stats(WATCHER_CH, &st);
	zassert_equal(tier1, st.tier1_verified,
		      "%u Tier I pages went out during the window and %u "
		      "verified", tier1, st.tier1_verified);
	zassert_equal(0u, st.tier1_unverified,
		      "%u Tier I windows failed to verify at the receiver that "
		      "was already listening", st.tier1_unverified);
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
			     "slot %u carried page 0x%02x, which belongs to "
			     "neither the profile nor the compat allocation",
			     i, page);
		if (own) {
			data_pages++;
		}
	}
	/*
	 * The enrolment frames DO cost data slots, and the honest assertion is
	 * that the cost is bounded rather than absent. The promoted beacon rate
	 * is one message in eight, so at worst 7/8 of the run stays the
	 * sensor's own pages once the common-page pair and the Tier I interval
	 * are taken out. Below 3/4 would mean the window had displaced more
	 * than it is allowed to.
	 */
	zassert_true(data_pages * 4u >= SLOTS * 3u,
		     "only %u of %u slots carried heart-rate data during the "
		     "window; the promotion displaced more than 1 in 8",
		     data_pages, SLOTS);
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

	/*
	 * An enrolment the owner did not perform is the whole attack, so making
	 * it silent would be the mistake. The bit is in the beacon and every
	 * keyholder already reads the beacon.
	 */
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
			zassert_equal(0u,
				      b[2] & PROFILE_COMPAT_CAP_PAIRING_OPEN,
				      "the pairing-open bit was set with no "
				      "window open");
			seen_shut++;
		}
		zassert_equal((uint8_t)(PROFILE_COMPAT_BEACON_FRAMES - 1u),
			      (uint8_t)(b[1] & 0x0Fu),
			      "the steady-state set is not two frames");
	}
	zassert_true(seen_shut > 0u, "no beacon frame 0 was emitted at all");

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
			      "frame %u of the set did not restate the new "
			      "count; a receiver that heard only frames 0 and "
			      "1 would think no window was open",
			      (unsigned)(b[1] >> 4));
		seen_count8++;
		if ((b[1] >> 4) == 0u) {
			zassert_equal(PROFILE_COMPAT_CAP_PAIRING_OPEN,
				      b[2] & PROFILE_COMPAT_CAP_PAIRING_OPEN,
				      "the pairing-open bit was clear with a "
				      "window open");
			seen_open++;
		}
	}
	zassert_true(seen_open > 0u, "beacon frame 0 never went out during the "
		     "window, so the bit reached nobody");
	zassert_true(seen_count8 >= PROFILE_ENROL_FRAMES,
		     "only %u frames of the eight-frame set went out in one "
		     "cycle; the promoted cadence is not promoting",
		     seen_count8);

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
			      (uint8_t)(b[1] & 0x0Fu),
			      "the set did not shrink back after the window "
			      "closed");
		if ((b[1] >> 4) == 0u) {
			zassert_equal(0u,
				      b[2] & PROFILE_COMPAT_CAP_PAIRING_OPEN,
				      "the pairing-open bit outlived the "
				      "window");
		}
	}
}

/* ═══ The three settings ═════════════════════════════════════════════════ */

ZTEST(profile_enrol, test_closed_refuses_both_ways_in)
{
	struct profile_enrol pe;

	/*
	 * `closed` means no over-air enrolment EVER. Not "unless somebody holds
	 * the button", not "unless a keyholder asks". It is the posture
	 * radiant_sec.h already recommends for anything that matters and
	 * nothing in Layer D weakens it.
	 */
	give_id(NODE_CH, NODE_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_CLOSED, node_scalar);

	zassert_equal(-EPERM, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_equal(-EPERM, profile_enrol_keyholder_request(&pe, T_ORIGIN));
	zassert_false(profile_enrol_is_open(&pe));
	zassert_equal(0u, profile_enrol_frame_count(&pe, T_ORIGIN),
		      "a closed node offered enrolment frames");
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

	/*
	 * `physical` is not a superset of `open-window`. A node whose owner
	 * chose "only a button opens this" must not have a second door that a
	 * keyholder - or anything that can forge a command - can walk through.
	 */
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
	zassert_false(radiant_sec_pair_is_open(NODE_CH),
		      "the window closed but the node stayed in pairing mode");
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

	/*
	 * The failure the framing convention cannot catch on its own. Byte [1]
	 * says WHICH FRAME this is and never WHICH KEY it belongs to, so a
	 * receiver that joined as one peer's window closed and another opened
	 * would otherwise splice two halves into a key neither end holds and
	 * derive a shared secret that is simply wrong, with no error anywhere.
	 */
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
			      &pe, set_b[PROFILE_ENROL_FRAMES - 1u], 8u),
		      "a key spliced from two windows was accepted");

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

	/*
	 * A small-order point makes the shared secret all zeros whatever our
	 * scalar was, and refusing it is MANDATORY here rather than optional:
	 * the result becomes a root key, so accepting it would let anyone able
	 * to inject one packet fix the group key to a value they already know.
	 *
	 * And refusing it must not cost the window. One injected packet that
	 * ends a window the user opened is a denial of service with a one-frame
	 * price tag.
	 */
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
	zassert_true(profile_enrol_is_open(&pe),
		     "one injected packet closed the window");

	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(0u, sstats.enrolments,
		      "a refused key was counted as an enrolment");

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
	 * RULE ONE OF SECTION 11.7, AS AN API PROPERTY: the window opens on
	 * whichever channel the node is currently on.
	 *
	 * This instance has no compat client, no beacon, no page number and no
	 * locator - it is the shape a node in private mode would use, and it is
	 * the hook C8 wires into. Nothing here reads or writes the beacon's
	 * locator fields or asks what `announce` is set to, which is why
	 * enrolment does not depend on that setting at all.
	 */
	give_id(NODE_CH, NODE_DEVNUM);
	give_id(PEER_CH, PEER_DEVNUM);
	enrol_up(&pe, PROFILE_ENROL_PHYSICAL, node_scalar);

	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_equal(PROFILE_ENROL_FRAMES,
		      profile_enrol_frame_count(&pe, T_ORIGIN));
	for (k = 0u; k < PROFILE_ENROL_FRAMES; k++) {
		zassert_true(profile_enrol_frame(&pe, k, payload),
			     "frame %u was not produced without a beacon", k);
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
	 * The seam wired to what a strap actually runs (ADR 0009), so that the
	 * one rule most likely to be implemented backwards is exercised through
	 * Layer D rather than only in node_ident's own suite: pair_counter
	 * advances and is DURABLY WRITTEN before the derived public key leaves
	 * the radio, never on completion. An abandoned window never completes,
	 * windows are abandoned all the time, and advance-on-completion would
	 * therefore reuse the scalar on the very next attempt - a repeated
	 * private key and an invariant 32-byte name on the air.
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
	zassert_equal(before + 1u, after,
		      "the pair counter did not advance before the public key "
		      "was available to transmit");

	/* Abandoned - and the counter does NOT roll back. A burnt counter value
	 * is the cheap half of the trade; reusing one is the expensive half. */
	profile_enrol_close(&pe);
	zassert_equal(0, profile_enrol_physical_action(&pe,
						       T_ORIGIN + 120ull * US_PER_S));
	zassert_equal(NODE_IDENT_OK, node_ident_pair_counter(&second));
	zassert_equal(after + 1u, second,
		      "a second window reused the first window's counter, "
		      "which is a repeated X25519 private key");

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

/* ═══ compat-C8: the clause C7 deferred ══════════════════════════════════ */

#if defined(CONFIG_RADIANT_SEC_COMPAT)

/*
 * "A NODE IN PRIVATE MODE GAINS A SECOND RECEIVER, the first receiver's stream
 * is uninterrupted across the whole window, and the new receiver reaches the
 * node by deriving the locator rather than by any announcement."
 *
 * The first two clauses were asserted when this file was written. The third was
 * deferred, and the reason was a boundary rather than effort: the locator is
 * trunc16(CMAC(K_id, "priv" || epoch)) and K_id is reachable only from the layer
 * that holds the root key, so deriving it from a test would have meant inventing
 * C8's entry point from the outside and then having C8 either adopt it or
 * contradict it. C8 has landed, the entry point is
 * radiant_sec_compat_locator(), and this is the deferral paid.
 *
 * The node here is `physical` + `silent`, which makes the third clause
 * unambiguous rather than merely true: there is no announcement anywhere on the
 * air to have reached the new receiver, so derivation is the only thing that
 * can have.
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
	struct radiant_sec_stats   sstats;
	uint8_t  air[PROFILE_ENROL_SET_BYTES];
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
	give_id(PEER_CH, PEER_DEVNUM);
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

		/*
		 * A device type 0x60 master's own rotation, driving the two
		 * client entry points directly - which is exactly what
		 * profile_enrol.h says a node with no beacon page does. The
		 * page number for these frames on 0x60 is still deferred; this
		 * loop picks one so there is a stream to assert about.
		 */
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
			 * counter: this is the FIRST RECEIVER'S STREAM and the
			 * whole claim is that nothing here stops. */
			body[0] = 0x01u;
			body[1] = (uint8_t)(data_msgs & 0xFFu);
			data_msgs++;
		}

		log_put(body, PROFILE_SLOT_DATA);

		/* The watcher hears every slot: no channel opened, none closed,
		 * none retuned, so there is nothing for it to notice. */
		watcher_msgs++;
		if (!is_frame) {
			zassert_true(data_msgs > last_counter,
				     "the first receiver's data counter went "
				     "backwards at slot %u", i);
			last_counter = data_msgs;
		}
	}

	zassert_equal(PRIVATE_SLOTS, watcher_msgs,
		      "the first receiver missed a slot across the window");
	zassert_true(profile_enrol_is_open(&pe),
		     "the window closed inside the run");
	zassert_equal(1u, g_enter_calls,
		      "the node changed channel during the enrolment; the whole "
		      "claim is that a pairing window is ADDITIVE - no channel "
		      "opened, closed or retuned");
	zassert_true(profile_private_is_private(&pp),
		     "the node left private mode to gain a receiver, which is "
		     "the case section 11.7 rule one exists to make impossible");

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
	zassert_equal((uint8_t)((1u << PROFILE_ENROL_FRAMES) - 1u), have,
		      "the public key never completed on the private channel");

	/* ── NOTHING ON THE AIR SAYS WHERE THE NODE IS ──────────────────── */
	for (i = 0u; i < log_n && i < SLOTS; i++) {
		const uint8_t *b = log_buf[i].body;

		/*
		 * No beacon page at all, therefore no announcement frame, no
		 * locator field and no pending-switch bit - there is no beacon
		 * on device type 0x60 to carry any of them, and a silent node
		 * would not have set them if there were. The new receiver's
		 * derivation below is therefore the ONLY thing that can have
		 * told it where the node is.
		 */
		zassert_not_equal(PROFILE_COMPAT_PAGE_BEACON,
				  (uint8_t)(b[0] & PROFILE_COMPAT_PAGE_MASK),
				  "a beacon page appeared on a private node");
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
		      radiant_sec_pair_peer(PEER_CH, air, &peer_fp));
	zassert_equal(node_fp, peer_fp,
		      "the two ends derived different shared secrets");

	radiant_sec_get_stats(NODE_CH, &sstats);
	zassert_equal(1u, sstats.enrolments);

	/* ── ...AND THE NEW RECEIVER FINDS THE NODE BY DERIVING IT ──────── */
	/*
	 * A completed enrolment is exactly "this receiver now holds the group
	 * root", so its compat context is keyed with that root - the same
	 * operation a host-provisioned receiver performs, and the reason
	 * enrolment is additive at all.
	 *
	 * From there it computes where the node is with no announcement to have
	 * missed, no beacon to read and nothing on the air above that says
	 * anything about it. THAT is the clause this test was written to pay.
	 */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(PEER_CH, compat_root, 128,
						 NODE_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_locator(PEER_CH, TEST_EPOCH, 0u,
						 &derived));
	zassert_equal(g_private_devnum, derived,
		      "the newly enrolled receiver derived 0x%04X and the node "
		      "is on 0x%04X", derived, g_private_devnum);
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
	 * THE INTERLOCK, from Layer D's side and against the real Layer C.
	 * profile_compat.h states the rule - a countdown and an enrolment window
	 * must not run at once, and whichever arrives second is refused - and
	 * both halves of it are load-bearing: the frames share indices 2 and 3,
	 * and a switch closes the channel the window is running on.
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
	zassert_equal(0, profile_enrol_attach(&pe, &pc),
		      "the beacon page must hold both clients: the exclusion is "
		      "between two activities, not two registrations");

	/* A window opens; the countdown is second and is refused. */
	zassert_equal(0, profile_enrol_physical_action(&pe, T_ORIGIN));
	zassert_true(profile_compat_client_busy(&pc, T_ORIGIN, &pp),
		     "an open window does not show as busy, so a countdown "
		     "would start straight into it and tear a public key in "
		     "half");
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

	zassert_equal(-EBUSY, profile_enrol_physical_action(&pe, T_ORIGIN),
		      "a pairing window opened during a countdown; it would "
		      "have burnt a pair counter, entered pairing mode and "
		      "transmitted nothing");
	zassert_false(profile_enrol_is_open(&pe));
}

#endif /* CONFIG_RADIANT_SEC_COMPAT */

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
