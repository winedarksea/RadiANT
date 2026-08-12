/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_compat.c - compat-C5's gate: the emitted stream is byte-exact
 * ANT+ apart from two added page numbers.
 *
 * ---------------------------------------------------------------------------
 * What is being claimed, and how this file and a Python file split the work
 * ---------------------------------------------------------------------------
 * The claim is not "the encoders round-trip". It is that a RadiANT heart-rate
 * strap and a RadiANT power meter put a STOCK ANT+ STREAM on the air, with
 * pages 0x70, 0x71 and 0x72 added and nothing else changed. Half of that is
 * checkable here and half of it is not:
 *
 *   HERE       the cadence and the structure. Page 80 at message 119 and page
 *              81 at message 120 of every 121; the page-change toggle stepping
 *              every four TRANSMITTED messages including the inserted ones; one
 *              beacon frame per cycle, alternating; no page number outside the
 *              union of the profile's own set and 0x70-0x72; a node with no
 *              compat layer emitting nothing but ANT+; and the attestation
 *              pages verifying at a receiver that holds the key.
 *
 *   NOT HERE   whether the BYTES of a heart-rate page are the bytes a real
 *              receiver expects. A C test asserting that against constants
 *              written by the same author in the same hour proves the author
 *              was consistent, not that the profile is right.
 *
 * So this suite also PRINTS the whole stream as .antcap lines, in the format
 * tools/ant_pages.py's write_capture()/read_capture() already define, and
 * tools/test_compat_capture.py decodes those bytes with the Python encoders written
 * in a different phase (compat-C3) and re-encodes them: byte-for-byte equality
 * between two independent implementations is the gate. The capture is committed
 * to tools/vectors/ so it is a regression test on every platform rather than a
 * thing somebody once ran on a DK.
 *
 * The prefix on those lines is "@@ <name> ", which is not part of the format -
 * ztest's console carries other traffic, and a marker is what lets the capture
 * be lifted out of it. scripts/ has no extractor because one PowerShell line
 * does it; the format's own header is written by the Python side.
 */

#include <zephyr/ztest.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include <radiant_core/radiant_sec_compat.h>

#include "profile_common.h"
#include "profile_compat.h"
#include "profile_hr.h"
#include "profile_power.h"
#include "profile_sched.h"

#define TX_CH 0u
#define RX_CH 1u

/*
 * The key, the epoch and the device number are pinned here AND in
 * tools/test_compat_capture.py, because the Python side has to derive the same
 * K_auth to check the tags in the committed capture. The root is
 * tools/ant_sim.py's DEFAULT_COMPAT_ROOT - bytes 0x00..0x0F - so a reader
 * comparing a capture from this suite against one from the simulator is
 * comparing two streams under one key rather than two unrelated ones.
 */
static const uint8_t compat_root[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

#define TEST_EPOCH  7u
#define TEST_DEVNUM 0x2C41u
#define TEST_T_S    RADIANT_SEC_COMPAT_T_DEFAULT_S
#define TEST_N      RADIANT_SEC_COMPAT_N_DEFAULT

/*
 * The default configuration: Tier I on, Tier II off. Two page numbers exist and
 * only one of them is on the air here, which is what "on by default" and "off
 * by default" mean in a capture.
 *
 * The receiver PINS, and that is not decoration either: an unpinned receiver
 * reads a stream with no attestation as CLEAR, so a test that asserted VERIFIED
 * without pinning would be asserting the default answer. Pinning is the whole
 * of downgrade protection - strip the attestation off the air and this receiver
 * says UNVERIFIED where a naive one falls back to clear and calls it normal.
 */
#define TX_SWITCHES RADIANT_SEC_COMPAT_SW_TIER_I
#define RX_SWITCHES ((uint8_t)(RADIANT_SEC_COMPAT_SW_TIER_I | \
			       RADIANT_SEC_COMPAT_SW_PIN))

/* Three cycles: enough for four Tier I intervals at the 20 s default and two
 * full passes of the beacon's two-frame set. */
#define CYCLES 3u
#define SLOTS  (CYCLES * PROFILE_TLM_CYCLE)

/* An origin above 2^32 microseconds, for the reason test_sec_compat_attest.c
 * gives: a test anchored at zero passes on an implementation that truncated
 * every instant to 32 bits. */
#define T_ORIGIN ((uint64_t)0x100000000ull + 4321u)

#define US_PER_S 1000000u

static uint64_t period_us(uint16_t counts)
{
	return ((uint64_t)counts * US_PER_S) / 32768u;
}

/* ── What a slot produced ───────────────────────────────────────────────── */

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

/*
 * One .antcap line per transmitted message, with a marker so it can be lifted
 * out of the console.
 *
 * The time column is printed as two integers rather than a float: Zephyr's
 * printk has no floating point unless CONFIG_CBPRINTF_FP_SUPPORT is on, and
 * turning that on to print a timestamp would be a build-wide change made for a
 * test's convenience. Six digits of microseconds is exactly what
 * ant_pages.format_capture_line() emits anyway.
 */
static void dump_capture(const char *name, uint8_t device_type,
			 uint16_t devnum, uint64_t period)
{
	uint32_t i;

	for (i = 0u; i < log_n && i < SLOTS; i++) {
		uint64_t       t = (uint64_t)i * period;
		const uint8_t *b = log_buf[i].body;

		printk("@@ %s %u.%06u %02X %04X "
		       "%02x%02x%02x%02x%02x%02x%02x%02x\n",
		       name, (uint32_t)(t / US_PER_S), (uint32_t)(t % US_PER_S),
		       device_type, devnum,
		       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	}
}

/* The subtype a receiver reads out of byte [0]. On heart rate the toggle bit
 * has to come off first, which is the whole reason ant_pages.decode() takes a
 * device type that looks redundant. */
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

/* ── Bringing a pair of channels up ─────────────────────────────────────── */

static void keys_up(uint8_t tx_sw, uint8_t rx_sw, uint16_t period_counts)
{
	radiant_sec_compat_reset();

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(TX_CH, compat_root, 128,
						 TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(RX_CH, compat_root, 128,
						 TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_devnum(TX_CH, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_devnum(RX_CH, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(TX_CH, tx_sw, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(RX_CH, rx_sw, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(TX_CH, TEST_EPOCH, T_ORIGIN,
						   0u, period_counts));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(RX_CH, TEST_EPOCH, T_ORIGIN,
						   0u, period_counts));
}

static void compat_up(struct profile_compat *pc)
{
	struct profile_compat_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.ch = TX_CH;
	cfg.epoch = TEST_EPOCH;
	cfg.advertise = true;
	cfg.attest_available = true;
	cfg.policy = PROFILE_COMPAT_POLICY_NEVER;
	cfg.window = TEST_N;
	cfg.mode = PROFILE_COMPAT_MODE_FIXED;

	zassert_equal(0, profile_compat_init(pc, &cfg),
		      "the compat client refused to initialise");
}

/* ── The two sensors ────────────────────────────────────────────────────── */

static void hr_cfg_fill(struct profile_hr_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->id.hw_revision = 1u;
	cfg->id.manufacturer_id = 0x00FFu;
	cfg->id.model_number = 0x0001u;
	cfg->id.sw_revision_main = 1u;
	cfg->id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	/* The page 81 privacy rule, as the default a strap should ship with:
	 * a 32-bit globally unique serial in the clear defeats identity tiers 1
	 * and 2 outright, whatever else the node does. */
	cfg->id.serial_number = PROFILE_COMMON_INVALID_U32;
	cfg->manufacturer_id_8 = 0xFFu;
	cfg->serial_upper16 = 0x4553u;
	cfg->model_number_8 = 1u;
	cfg->hw_version = 1u;
	cfg->sw_version = 1u;
}

static void power_cfg_fill(struct profile_power_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->id.hw_revision = 1u;
	cfg->id.manufacturer_id = 0x00FFu;
	cfg->id.model_number = 0x0001u;
	cfg->id.sw_revision_main = 1u;
	cfg->id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg->id.serial_number = PROFILE_COMMON_INVALID_U32;
	cfg->pages[0] = PROFILE_POWER_PAGE_STANDARD;
	cfg->n_pages = 1u;
}

/*
 * Run a heart-rate strap for `slots` messages, beating at a fixed 60 bpm so the
 * stream is reproducible: a capture whose values came from a random walk cannot
 * be committed and diffed.
 */
static void run_hr(struct profile_hr *hr, struct profile_compat *pc,
		   uint32_t slots)
{
	uint64_t period = period_us(PROFILE_HR_PERIOD);
	uint32_t i;

	log_reset();
	profile_hr_set_computed(hr, 60u);

	for (i = 0u; i < slots; i++) {
		uint8_t                body[8];
		uint64_t               now = T_ORIGIN + (uint64_t)i * period;
		enum profile_slot_kind kind;

		/* One beat a second: 1024 counts of the profile's 1/1024 s
		 * event time, and at ~4.06 Hz that is one beat every four
		 * messages. */
		if ((i % 4u) == 0u) {
			profile_hr_beat(hr, (uint16_t)((i / 4u) * 1024u));
		}
		profile_hr_set_operating_time(hr, i / 4u);

		kind = profile_hr_next(hr, now, body);
		zassert_not_equal(PROFILE_SLOT_IDLE, kind,
				  "slot %u went silent; a periodic ANT+ master "
				  "has nothing to be silent about", i);
		log_put(body, kind);

		if (pc != NULL) {
			zassert_equal(RADIANT_SEC_OK,
				      radiant_sec_compat_rx(RX_CH, sub_of(body),
							    body, 8u, now));
		}
	}
}

static void run_power(struct profile_power *pw, struct profile_compat *pc,
		      uint32_t slots)
{
	uint64_t period = period_us(PROFILE_POWER_PERIOD);
	uint32_t i;

	log_reset();

	for (i = 0u; i < slots; i++) {
		uint8_t                body[8];
		uint64_t               now = T_ORIGIN + (uint64_t)i * period;
		enum profile_slot_kind kind;

		profile_power_event(pw, 200u, 80u);

		kind = profile_power_next(pw, now, body);
		zassert_not_equal(PROFILE_SLOT_IDLE, kind, "slot %u went silent",
				  i);
		log_put(body, kind);

		if (pc != NULL) {
			zassert_equal(RADIANT_SEC_OK,
				      radiant_sec_compat_rx(RX_CH, sub_of(body),
							    body, 8u, now));
		}
	}
}

static void compat_before_each(void *f)
{
	ARG_UNUSED(f);
	radiant_sec_compat_reset();
	log_reset();
}

ZTEST_SUITE(profile_compat, NULL, NULL, compat_before_each, NULL, NULL);

/* ═══ The cadence is the cadence, with the compat pages in it ════════════ */

ZTEST(profile_compat, test_hr_keeps_the_119_120_121_interleave)
{
	struct profile_hr     hr;
	struct profile_hr_cfg cfg;
	struct profile_compat pc;
	uint32_t              i;

	keys_up(TX_SWITCHES, RX_SWITCHES,
		PROFILE_HR_PERIOD);
	hr_cfg_fill(&cfg);
	zassert_equal(0, profile_hr_init(&hr, &cfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));

	run_hr(&hr, &pc, SLOTS);

	/*
	 * NOTHING DISPLACES THE COMMON PAGES. The seam never offers message 119
	 * or 120 precisely so a client cannot, and this is the assertion that
	 * makes that a fact about the stream rather than about the header
	 * comment - a certified receiver pairs on this cadence, and
	 * tools/ant_verify.py fails a sensor whose common-page gap exceeds 121.
	 */
	for (i = 0u; i < log_n; i++) {
		uint8_t page = (uint8_t)(log_buf[i].body[0] &
					 PROFILE_COMPAT_PAGE_MASK);

		if ((i % PROFILE_TLM_CYCLE) == PROFILE_TLM_SLOT_PAGE_80) {
			zassert_equal(PROFILE_COMMON_PAGE_80, page,
				      "message %u should be page 80", i);
		} else if ((i % PROFILE_TLM_CYCLE) == PROFILE_TLM_SLOT_PAGE_81) {
			zassert_equal(PROFILE_COMMON_PAGE_81, page,
				      "message %u should be page 81", i);
		} else {
			zassert_true(page != PROFILE_COMMON_PAGE_80 &&
					     page != PROFILE_COMMON_PAGE_81,
				     "a common page turned up at message %u", i);
		}
	}
}

ZTEST(profile_compat, test_hr_emits_no_page_outside_the_allocation)
{
	struct profile_hr     hr;
	struct profile_hr_cfg cfg;
	struct profile_compat pc;
	uint32_t              i;

	keys_up(TX_SWITCHES, RX_SWITCHES,
		PROFILE_HR_PERIOD);
	hr_cfg_fill(&cfg);
	zassert_equal(0, profile_hr_init(&hr, &cfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));

	run_hr(&hr, &pc, SLOTS);

	for (i = 0u; i < log_n; i++) {
		uint8_t page = (uint8_t)(log_buf[i].body[0] &
					 PROFILE_COMPAT_PAGE_MASK);
		bool    ant_plus = page <= PROFILE_HR_PAGE_PREVIOUS_BEAT ||
				   page == PROFILE_COMMON_PAGE_80 ||
				   page == PROFILE_COMMON_PAGE_81 ||
				   page == PROFILE_COMMON_PAGE_82;

		zassert_true(ant_plus || profile_compat_is_compat_page(page),
			     "message %u carries page 0x%02X, which is neither "
			     "an ANT+ heart-rate page nor one of the two "
			     "allocated compat numbers", i, page);
	}

	/* Every one of the profile's own pages appeared: a rotation that had
	 * silently stopped walking its background set would otherwise pass
	 * every assertion above. */
	zassert_true(count_page(PROFILE_HR_PAGE_PREVIOUS_BEAT) > 0u);
	zassert_true(count_page(PROFILE_HR_PAGE_DEFAULT) > 0u);
	zassert_true(count_page(PROFILE_HR_PAGE_CUMULATIVE) > 0u);
	zassert_true(count_page(PROFILE_HR_PAGE_MANUFACTURER) > 0u);
	zassert_true(count_page(PROFILE_HR_PAGE_PRODUCT) > 0u);
}

ZTEST(profile_compat, test_the_toggle_counts_transmitted_messages)
{
	struct profile_hr     hr;
	struct profile_hr_cfg cfg;
	struct profile_compat pc;
	uint32_t              i;

	keys_up(TX_SWITCHES, RX_SWITCHES,
		PROFILE_HR_PERIOD);
	hr_cfg_fill(&cfg);
	zassert_equal(0, profile_hr_init(&hr, &cfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));

	run_hr(&hr, &pc, SLOTS);

	/*
	 * The toggle steps every four TRANSMITTED messages, and the common
	 * pages and the compat pages are messages. This is the assertion behind
	 * the bench question docs/decisions/0008 records as open: inserted pages
	 * must not disturb the toggle sequence, and the cheapest way for them to
	 * disturb it is for the sensor to leave them out of its own count.
	 */
	for (i = 0u; i < log_n; i++) {
		bool want = ((i / PROFILE_HR_TOGGLE_INTERVAL) & 1u) != 0u;
		bool got = (log_buf[i].body[0] & PROFILE_HR_PAGE_TOGGLE) != 0u;

		zassert_equal(want, got,
			      "message %u carries toggle %d, expected %d", i,
			      (int)got, (int)want);
	}
}

ZTEST(profile_compat, test_the_beacon_is_one_frame_per_cycle_and_alternates)
{
	struct profile_hr     hr;
	struct profile_hr_cfg cfg;
	struct profile_compat pc;
	uint32_t              i;
	uint32_t              seen = 0u;
	uint8_t               expect_index = 0u;

	keys_up(TX_SWITCHES, RX_SWITCHES,
		PROFILE_HR_PERIOD);
	hr_cfg_fill(&cfg);
	zassert_equal(0, profile_hr_init(&hr, &cfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));

	run_hr(&hr, &pc, SLOTS);

	for (i = 0u; i < log_n; i++) {
		const uint8_t *b = log_buf[i].body;
		uint8_t        page = (uint8_t)(b[0] & PROFILE_COMPAT_PAGE_MASK);
		uint8_t        index;
		uint8_t        count;

		if (page != PROFILE_COMPAT_PAGE_BEACON) {
			continue;
		}
		index = (uint8_t)((b[1] >> 4) & 0x0Fu);
		count = (uint8_t)((b[1] & 0x0Fu) + 1u);

		zassert_equal(PROFILE_COMPAT_BEACON_FRAMES, count,
			      "the beacon set is two frames in the steady "
			      "state");
		zassert_equal(expect_index, index,
			      "beacon frames must alternate; message %u carries "
			      "frame %u", i, index);
		zassert_equal(0u, b[7], "beacon byte [7] is reserved");
		expect_index = (uint8_t)((expect_index + 1u) %
					 PROFILE_COMPAT_BEACON_FRAMES);
		seen++;
	}

	/*
	 * EXACTLY one per 121-message cycle: 0.8% of slots, which is the
	 * beacon's half of the 2.0% the compatibility claim rests on.
	 *
	 * Exactly, and not "at least", because both ways of missing are real
	 * and both are bugs. A node owes its first Tier I page on the first
	 * slot it is offered and that slot is the beacon's, so a client that
	 * treated a displaced frame as a lost one would emit two frames in
	 * three cycles here - and the first run of this suite did, which is why
	 * profile_compat.c owes the frame instead of dropping it.
	 */
	zassert_equal(CYCLES, seen,
		      "expected one beacon frame per cycle, got %u in %u cycles",
		      seen, CYCLES);
}

/* ═══ The attestation half ═══════════════════════════════════════════════ */

ZTEST(profile_compat, test_tier1_pages_ride_the_stream_and_verify)
{
	struct profile_hr               hr;
	struct profile_hr_cfg           cfg;
	struct profile_compat           pc;
	struct radiant_sec_compat_stats st;
	uint32_t                        emitted;

	keys_up(TX_SWITCHES, RX_SWITCHES,
		PROFILE_HR_PERIOD);
	hr_cfg_fill(&cfg);
	zassert_equal(0, profile_hr_init(&hr, &cfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));

	run_hr(&hr, &pc, SLOTS);

	emitted = count_page(PROFILE_COMPAT_PAGE_ATTEST_I);

	/*
	 * T = 20 s at ~4.06 Hz is one page in ~81, so three cycles of 121 is
	 * four intervals plus the one served at t = 0. The exact number is the
	 * point rather than "more than zero": a Tier I page every slot would
	 * also verify, and would cost 100% of the airtime this tier exists to
	 * spend 1.2% of.
	 */
	zassert_true(emitted >= 4u && emitted <= 6u,
		     "expected ~5 Tier I pages in %u slots, got %u", SLOTS,
		     emitted);

	radiant_sec_compat_get_stats(RX_CH, &st);
	zassert_equal(emitted, st.tier1_verified,
		      "%u Tier I pages went out and %u verified", emitted,
		      st.tier1_verified);
	zassert_equal(0u, st.tier1_unverified);
	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
		      radiant_sec_compat_stream_verdict(
			      RX_CH, T_ORIGIN + (uint64_t)(SLOTS - 1u) *
						       period_us(PROFILE_HR_PERIOD)));
}

ZTEST(profile_compat, test_a_strap_with_no_compat_layer_is_stock_ant_plus)
{
	struct profile_hr     hr;
	struct profile_hr_cfg cfg;
	uint32_t              i;

	/*
	 * The configuration most straps should ship in, and it is not a
	 * degraded mode: no client is registered at all, so the rotation never
	 * learns one existed. This is also the assertion that a build without
	 * compat attestation is a plain ANT+ sensor rather than a broken one.
	 */
	radiant_sec_compat_reset();
	hr_cfg_fill(&cfg);
	zassert_equal(0, profile_hr_init(&hr, &cfg));

	run_hr(&hr, NULL, PROFILE_TLM_CYCLE);

	for (i = 0u; i < log_n; i++) {
		uint8_t page = (uint8_t)(log_buf[i].body[0] &
					 PROFILE_COMPAT_PAGE_MASK);

		zassert_false(profile_compat_is_compat_page(page),
			      "message %u carries compat page 0x%02X on a node "
			      "that has no compat layer", i, page);
	}
}

/* ═══ Bicycle power, the other compat target ═════════════════════════════ */

ZTEST(profile_compat, test_power_keeps_the_interleave_and_its_own_pages)
{
	struct profile_power     pw;
	struct profile_power_cfg cfg;
	struct profile_compat    pc;
	uint32_t                 i;

	keys_up(TX_SWITCHES, RX_SWITCHES,
		PROFILE_POWER_PERIOD);
	power_cfg_fill(&cfg);
	zassert_equal(0, profile_power_init(&pw, &cfg));
	compat_up(&pc);
	zassert_equal(0, profile_power_set_compat(&pw, &pc));

	run_power(&pw, &pc, SLOTS);

	for (i = 0u; i < log_n; i++) {
		uint8_t page = log_buf[i].body[0];
		bool    ant_plus = page == PROFILE_POWER_PAGE_STANDARD ||
				   page == PROFILE_COMMON_PAGE_80 ||
				   page == PROFILE_COMMON_PAGE_81 ||
				   page == PROFILE_COMMON_PAGE_82;

		/* NO TOGGLE ON THIS DEVICE TYPE. Byte [0] is a whole page
		 * number, so it is compared unmasked - a stray toggle bit would
		 * make page 0x10 into page 0x90 and this is where that shows. */
		zassert_true(ant_plus || profile_compat_is_compat_page(page),
			     "message %u carries page 0x%02X", i, page);
		zassert_false((page & PROFILE_COMPAT_PAGE_TOGGLE) != 0u,
			      "message %u has bit 7 set on a device type with "
			      "no page-change toggle", i);

		if ((i % PROFILE_TLM_CYCLE) == PROFILE_TLM_SLOT_PAGE_80) {
			zassert_equal(PROFILE_COMMON_PAGE_80, page);
		} else if ((i % PROFILE_TLM_CYCLE) == PROFILE_TLM_SLOT_PAGE_81) {
			zassert_equal(PROFILE_COMMON_PAGE_81, page);
		}
	}

	zassert_equal(CYCLES, count_page(PROFILE_COMPAT_PAGE_BEACON));
	zassert_true(count_page(PROFILE_COMPAT_PAGE_ATTEST_I) >= 4u);
}

/* ═══ The captures the Python gate reads ═════════════════════════════════ */

ZTEST(profile_compat, test_emit_captures_for_the_python_cross_check)
{
	struct profile_hr        hr;
	struct profile_hr_cfg    hcfg;
	struct profile_power     pw;
	struct profile_power_cfg pcfg;
	struct profile_compat    pc;

	keys_up(TX_SWITCHES, RX_SWITCHES,
		PROFILE_HR_PERIOD);
	hr_cfg_fill(&hcfg);
	zassert_equal(0, profile_hr_init(&hr, &hcfg));
	compat_up(&pc);
	zassert_equal(0, profile_hr_set_compat(&hr, &pc));
	run_hr(&hr, &pc, SLOTS);
	dump_capture("compat-hr", PROFILE_HR_DEVICE_TYPE, TEST_DEVNUM,
		     period_us(PROFILE_HR_PERIOD));

	keys_up(TX_SWITCHES, RX_SWITCHES,
		PROFILE_POWER_PERIOD);
	power_cfg_fill(&pcfg);
	zassert_equal(0, profile_power_init(&pw, &pcfg));
	compat_up(&pc);
	zassert_equal(0, profile_power_set_compat(&pw, &pc));
	run_power(&pw, &pc, SLOTS);
	dump_capture("compat-power", PROFILE_POWER_DEVICE_TYPE, TEST_DEVNUM,
		     period_us(PROFILE_POWER_PERIOD));
}
