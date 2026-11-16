/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The nRF backend's arm cache (perf plan 2026-11-15, B4).
 *
 * An arm call validates its request once (format_prepare()/filters_prepare(),
 * answered from a content-keyed memo when the same request was accepted
 * before) and programs from the result. The register writes are skipped when a
 * shadow says the RADIO already holds the values. A stale shadow is a deaf
 * radio with no error, so these tests poison the register model after an arm
 * and check which writers make the next arm write again.
 *
 * backend_under_test.c forces RADIANT_NRF_REG_SHADOW on; production MPSL
 * builds compile it out (see the comment above RADIANT_NRF_REG_SHADOW).
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <nrfx.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_frame.h>

#include "backend_probe.h"

#define POISON 0xA5A5A5A5u
#define RF_ANT 57u

static struct radiant_rx_filter filt_a[2];
static struct radiant_rx_filter filt_b[1];
static struct radiant_pkt_format fmt_copy;
static uint8_t tx_body[RADIANT_RADIO_BODY_MAX];

static void set_addr(struct radiant_rx_filter *f, uint8_t b0, uint8_t b4)
{
	memset(f, 0, sizeof(*f));
	f->addr_len = 5u;
	f->addr[0] = b0;
	f->addr[1] = 0x11u;
	f->addr[2] = 0x22u;
	f->addr[3] = 0x33u;
	f->addr[4] = b4;
}

static void track_req(struct radiant_rx_req *req, const struct radiant_rx_filter *f,
		      uint8_t n)
{
	memset(req, 0, sizeof(*req));
	req->fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req->rf_index = RF_ANT;
	req->filters = f;
	req->n_filters = n;
	req->t_open = radiant_radio_now() + 10000u;
	req->t_close = req->t_open + 1000u;
}

/* Every register the cache may skip, overwritten with a value no arm writes. */
static void poison(void)
{
	fake_radio_regs.MODE = POISON;
	fake_radio_regs.FREQUENCY = POISON;
	fake_radio_regs.PCNF0 = POISON;
	fake_radio_regs.PCNF1 = POISON;
	fake_radio_regs.CRCCNF = POISON;
	fake_radio_regs.CRCPOLY = POISON;
	fake_radio_regs.CRCINIT = POISON;
	fake_radio_regs.BASE0 = POISON;
	fake_radio_regs.BASE1 = POISON;
	fake_radio_regs.PREFIX0 = POISON;
	fake_radio_regs.PREFIX1 = POISON;
	fake_radio_regs.RXADDRESSES = POISON;
}

static bool format_poisoned(void)
{
	return fake_radio_regs.MODE == POISON && fake_radio_regs.FREQUENCY == POISON &&
	       fake_radio_regs.PCNF0 == POISON && fake_radio_regs.PCNF1 == POISON &&
	       fake_radio_regs.CRCCNF == POISON && fake_radio_regs.CRCPOLY == POISON &&
	       fake_radio_regs.CRCINIT == POISON;
}

static bool format_rewritten(void)
{
	return fake_radio_regs.MODE != POISON && fake_radio_regs.FREQUENCY != POISON &&
	       fake_radio_regs.PCNF0 != POISON && fake_radio_regs.PCNF1 != POISON &&
	       fake_radio_regs.CRCCNF != POISON && fake_radio_regs.CRCPOLY != POISON &&
	       fake_radio_regs.CRCINIT != POISON;
}

static bool filters_poisoned(void)
{
	return fake_radio_regs.BASE0 == POISON && fake_radio_regs.BASE1 == POISON &&
	       fake_radio_regs.PREFIX0 == POISON && fake_radio_regs.PREFIX1 == POISON &&
	       fake_radio_regs.RXADDRESSES == POISON;
}

static bool filters_rewritten(void)
{
	return fake_radio_regs.BASE0 != POISON && fake_radio_regs.BASE1 != POISON &&
	       fake_radio_regs.PREFIX0 != POISON && fake_radio_regs.PREFIX1 != POISON &&
	       fake_radio_regs.RXADDRESSES != POISON;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	backend_reset();
	set_addr(&filt_a[0], 0x10u, 0x01u);
	set_addr(&filt_a[1], 0x20u, 0x02u);   /* another base: logical 0 */
	set_addr(&filt_b[0], 0x30u, 0x03u);
	fmt_copy = *radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
}

ZTEST_SUITE(radiant_backend_arm_cache, NULL, NULL, before, NULL, NULL);

ZTEST(radiant_backend_arm_cache, test_the_first_receive_writes_every_register)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	poison();
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_true(format_rewritten(), "the first arm skipped a format register");
	zassert_true(filters_rewritten(), "the first arm skipped an address register");
	zassert_equal(2400u + RF_ANT, fake_radio_regs.FREQUENCY);
}

ZTEST(radiant_backend_arm_cache, test_an_unchanged_receive_rewrites_nothing)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	poison();
	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_true(format_poisoned(), "an unchanged format was written again");
	zassert_true(filters_poisoned(), "an unchanged filter set was written again");
}

ZTEST(radiant_backend_arm_cache, test_a_new_channel_index_rewrites_the_format_only)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	poison();
	track_req(&req, filt_b, 1u);
	req.rf_index = 66u;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_true(format_rewritten(), "a format change left a register stale");
	zassert_equal(2466u, fake_radio_regs.FREQUENCY);
	zassert_true(filters_poisoned(), "an unchanged filter set was written again");
}

ZTEST(radiant_backend_arm_cache, test_a_new_filter_set_rewrites_the_addresses_only)
{
	struct radiant_rx_req req;
	uint32_t base1_b;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	base1_b = fake_radio_regs.BASE1;
	poison();
	track_req(&req, filt_a, 2u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_true(filters_rewritten(), "a filter change left a register stale");
	zassert_not_equal(base1_b, fake_radio_regs.BASE1);
	zassert_true(format_poisoned(), "an unchanged format was written again");
}

/* The memo is keyed on content: a filter array rewritten in place is a new set. */
ZTEST(radiant_backend_arm_cache, test_a_filter_rewritten_in_place_is_a_new_set)
{
	struct radiant_rx_req req;
	uint32_t prefix0;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	prefix0 = fake_radio_regs.PREFIX0;
	filt_b[0].addr[4] = 0x7Eu;   /* the prefix byte */
	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_not_equal(prefix0, fake_radio_regs.PREFIX0,
			  "a filter rewritten in place kept the old address");

	/* And one rewritten into an invalid set is refused, not served from the memo. */
	filt_b[0].addr_len = 4u;
	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_EINVAL, backend_rx(&req),
		      "a filter set made invalid in place was accepted");
}

/* The same for a format: a pointer match must never skip a refusal. */
ZTEST(radiant_backend_arm_cache, test_a_format_rewritten_in_place_is_checked_again)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	req.fmt = &fmt_copy;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));

	fmt_copy.crc.init = 0x1234u;
	poison();
	track_req(&req, filt_b, 1u);
	req.fmt = &fmt_copy;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_equal(0x1234u, fake_radio_regs.CRCINIT,
		      "a format rewritten in place kept the old CRC init");

	fmt_copy.crc.poly = 0x8005u;
	track_req(&req, filt_b, 1u);
	req.fmt = &fmt_copy;
	zassert_equal(RADIANT_RADIO_ENOTSUP, backend_rx(&req),
		      "a format made invalid in place was accepted");

	/* The refusal order is unchanged: an invalid format wins over bad filters. */
	filt_b[0].addr_len = 4u;
	zassert_equal(RADIANT_RADIO_ENOTSUP, backend_rx(&req));
}

/* radiant_filter_slot[] comes from the memo with the registers, never from the last set. */
ZTEST(radiant_backend_arm_cache, test_the_slot_map_follows_the_filter_set_on_a_hit)
{
	struct radiant_rx_req req;

	track_req(&req, filt_a, 2u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_equal(0u, backend_filter_slot(1u));
	zassert_equal(1u, backend_filter_slot(0u));

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_equal(0u, backend_filter_slot(1u));
	zassert_equal(0u, backend_filter_slot(0u), "a slot outside the set kept the old index");

	track_req(&req, filt_a, 2u);   /* a memo hit */
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_equal(1u, backend_filter_slot(0u), "a memo hit left the slot map stale");
}

/* A transmit writes BASE0 and PREFIX0: the next receive must put its own back. */
ZTEST(radiant_backend_arm_cache, test_a_transmit_makes_the_next_receive_rewrite_its_addresses)
{
	struct radiant_rx_req req;
	struct radiant_tx_req tx;
	uint32_t base0_rx, prefix0_rx;

	track_req(&req, filt_a, 2u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	base0_rx = fake_radio_regs.BASE0;
	prefix0_rx = fake_radio_regs.PREFIX0;

	memset(&tx, 0, sizeof(tx));
	tx.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	tx.rf_index = RF_ANT;
	tx.addr_len = tx.fmt->addr_len;
	memcpy(tx.addr, filt_b[0].addr, sizeof(tx.addr));
	tx.body = tx_body;
	tx.body_len = tx.fmt->body_len;
	tx.t_sync_at = radiant_radio_now() + 10000u;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_tx(&tx));
	zassert_not_equal(base0_rx, fake_radio_regs.BASE0, "the transmit wrote no address");

	track_req(&req, filt_a, 2u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_equal(base0_rx, fake_radio_regs.BASE0,
		      "the receive after a transmit kept the transmit's BASE0");
	zassert_equal(prefix0_rx, fake_radio_regs.PREFIX0,
		      "the receive after a transmit kept the transmit's PREFIX0");
}

/* Transmit and receive formats differ (sw_addr_len): both directions stay right. */
ZTEST(radiant_backend_arm_cache, test_a_transmit_format_is_written_when_it_differs)
{
	struct radiant_tx_req tx;

	memset(&tx, 0, sizeof(tx));
	tx.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	tx.rf_index = 66u;
	tx.addr_len = tx.fmt->addr_len;
	memcpy(tx.addr, filt_b[0].addr, sizeof(tx.addr));
	tx.body = tx_body;
	tx.body_len = tx.fmt->body_len;
	tx.t_sync_at = radiant_radio_now() + 10000u;
	poison();
	zassert_equal(RADIANT_RADIO_OK_RC, backend_tx(&tx));
	zassert_true(format_rewritten(), "a transmit skipped a format register");
	zassert_equal(2466u, fake_radio_regs.FREQUENCY);
	zassert_equal(0u, fake_radio_regs.TXADDRESS);

	/* A second identical transmit writes its address again, and nothing else. */
	poison();
	tx.t_sync_at = radiant_radio_now() + 10000u;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_tx(&tx));
	zassert_true(format_poisoned(), "an unchanged transmit format was written again");
	zassert_not_equal(POISON, fake_radio_regs.BASE0, "a transmit skipped its address");
}

/* A grant starts with the other stack's registers: nothing is skipped. */
ZTEST(radiant_backend_arm_cache, test_a_grant_rewrites_everything)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	poison();
	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_grant_rx(&req));
	zassert_true(format_rewritten(), "a grant trusted the registers before it");
	zassert_true(filters_rewritten(), "a grant trusted the addresses before it");
}

ZTEST(radiant_backend_arm_cache, test_a_grant_end_forgets_the_registers)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	backend_grant_end();
	poison();
	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_true(format_rewritten(), "an arm after a grant end trusted the registers");
	zassert_true(filters_rewritten(), "an arm after a grant end trusted the addresses");
}

ZTEST(radiant_backend_arm_cache, test_disable_and_enable_forget_the_registers)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	backend_disable_enable();
	poison();
	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_true(format_rewritten(), "an arm after a disable trusted the registers");
	zassert_true(filters_rewritten(), "an arm after a disable trusted the addresses");
}

/* A request refused by validation is refused before any register is written. */
ZTEST(radiant_backend_arm_cache, test_a_refused_request_writes_nothing)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	req.n_filters = 0u;
	poison();
	zassert_equal(RADIANT_RADIO_EINVAL, backend_rx(&req));
	zassert_true(format_poisoned(), "a refused arm wrote the format");
	zassert_true(filters_poisoned(), "a refused arm wrote the addresses");
}

/*
 * A deferred (MPSL) arm through the real rx_arm() branch: the caller rewrites
 * its format and filters between the arm and the grant. The grant programs the
 * validated, arm-time request, and the ISR-facing fields (radiant_op.filters,
 * crc_init) describe the same request as the registers.
 */
ZTEST(radiant_backend_arm_cache, test_a_staged_arm_programs_the_arm_time_request)
{
	struct radiant_rx_req req;
	uint32_t prefix0, crcinit;
	const uint8_t prefix_byte = filt_b[0].addr[4];

	fmt_copy.crc.init = 0x4321u;
	track_req(&req, filt_b, 1u);
	req.fmt = &fmt_copy;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	prefix0 = fake_radio_regs.PREFIX0;
	crcinit = fake_radio_regs.CRCINIT;
	backend_grant_end();   /* forget the shadow: the grant must write */

	track_req(&req, filt_b, 1u);
	req.fmt = &fmt_copy;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx_staged(&req));
	poison();
	filt_b[0].addr[4] = 0x7Eu;
	fmt_copy.crc.init = 0x1111u;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_grant_staged());

	zassert_equal(prefix0, fake_radio_regs.PREFIX0,
		      "the grant programmed an address rewritten after the arm");
	zassert_equal(crcinit, fake_radio_regs.CRCINIT,
		      "the grant programmed a CRC init rewritten after the arm");
	zassert_equal(1u, backend_op_n_filters());
	zassert_equal(prefix_byte, backend_op_filter_byte(0u, 4u),
		      "the ISR would match against a filter rewritten after the arm");
	zassert_equal(0x4321u, backend_op_crc_init(),
		      "the ISR would repair with a CRC init rewritten after the arm");
}

/* A filter-memo hit must not let an invalid format through: the format's code wins. */
ZTEST(radiant_backend_arm_cache, test_a_filter_memo_hit_with_an_invalid_format_is_refused)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_true(backend_filt_memo_holds(&req));

	fmt_copy.crc.poly = 0x8005u;   /* same addr_len and PHY, so the filter memo hits */
	track_req(&req, filt_b, 1u);
	req.fmt = &fmt_copy;
	poison();
	zassert_equal(RADIANT_RADIO_ENOTSUP, backend_rx(&req),
		      "a memo hit on the filters let an invalid format through");
	zassert_true(format_poisoned() && filters_poisoned(),
		     "a refused arm wrote registers");
}

/*
 * The Zwift-style stream: a constant scan set S between tracked sets that each
 * arm once. LRU keeps S hot with up to two tracked sets between chunks; the
 * round-robin it replaced evicted S on the third new set.
 */
ZTEST(radiant_backend_arm_cache, test_the_alternating_set_stays_in_the_filter_memo)
{
	static struct radiant_rx_filter tracked[6][1];
	struct radiant_rx_req scan, req;

	track_req(&scan, filt_a, 2u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&scan));
	for (uint8_t i = 0u; i < 6u; i++) {
		set_addr(&tracked[i][0], (uint8_t)(0x40u + i), (uint8_t)(0x10u + i));
		track_req(&req, tracked[i], 1u);
		zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
		if ((i & 1u) == 1u) {
			/* Two tracked sets back to back, then the scan again. */
			zassert_true(backend_filt_memo_holds(&scan),
				     "the scan set was evicted after tracked set %u", i);
			track_req(&scan, filt_a, 2u);
			zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&scan));
		}
	}
	zassert_true(backend_filt_memo_holds(&scan), "the scan set was evicted");
	track_req(&req, tracked[0], 1u);
	zassert_false(backend_filt_memo_holds(&req), "the oldest tracked set was not evicted");
}

/* An ED chunk rewrites MODE, RXADDRESSES and FREQUENCY: the next receive writes everything. */
ZTEST(radiant_backend_arm_cache, test_an_ed_sweep_makes_the_next_receive_rewrite_everything)
{
	struct radiant_rx_req req;

	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	backend_program_ed();
	poison();
	track_req(&req, filt_b, 1u);
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx(&req));
	zassert_true(format_rewritten(), "a receive after an ED sweep trusted the format");
	zassert_true(filters_rewritten(), "a receive after an ED sweep trusted the addresses");
	zassert_equal(2400u + RF_ANT, fake_radio_regs.FREQUENCY);
}
