/* SPDX-License-Identifier: Apache-2.0 */
/*
 * backend_under_test.c - the real nRF backend, included against register
 * models, as gate_under_test.c includes the gate. Only this file may include
 * it. The arbitrated configuration is switched on here rather than in Kconfig
 * because the module's BACKEND_NRF symbols depend on a real SoC.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <nrfx.h>

#define CONFIG_RADIANT_SWI 1
#define CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL 1
#define CONFIG_RADIANT_BACKEND_NRF_IRQ_PRIO 0
#define CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN 0
#define CONFIG_SOC_COMPATIBLE_NRF54LX 1
/* The 0xF7 counters, so a test can read page 10 field 4. Nothing here calls
 * radiant_radio_init(), so the cycle-clock helpers it names are not needed. */
#define CONFIG_RADIANT_DIAG_COUNTERS 1
/* radiant_radio_init() names the core clock; nothing here calls it. */
static const uint32_t SystemCoreClock = 64000000u;
/*
 * The register shadow (B4) is compiled out of every MPSL build, this one
 * included. Forced on so test_arm_cache.c can check its skip decision and every
 * invalidation; with it off every arm writes, which the other suites cover.
 */
#define RADIANT_NRF_REG_SHADOW 1

#undef DT_CHOSEN
#undef DT_NODE_EXISTS
#undef DT_NODE_HAS_STATUS
#undef DT_REG_ADDR
#undef DT_PROP_OR
#undef DEVICE_DT_GET_ONE
#undef IRQ_CONNECT
#define DT_CHOSEN(x)              0
#define DT_NODE_EXISTS(x)         1
#define DT_NODE_HAS_STATUS(x, y)  1
#define DT_REG_ADDR(x)            (&fake_timer_regs)
#define DT_PROP_OR(x, p, d)       6
#define DEVICE_DT_GET_ONE(x)      NULL
#define IRQ_CONNECT(...)

#include "../../src/radiant_radio_nrf.c"

/* The scheduler's pass stamp (radiant_sched.c, not in this image). */
uint32_t radiant_diag_pass_now;

#include <radiant/radiant_frame.h>

#include "backend_probe.h"

NRF_RADIO_Type fake_radio_regs;
NRF_TIMER_Type fake_timer_regs;
NRF_TEMP_Type  fake_temp_regs;
struct fake_hw fake_hw;

/* The gate, as far as the completion path can tell. */
static struct {
	bool     held;
	bool     in_signal;
	uint32_t releases;
} fg;

bool gate_held(void) { return fg.held; }
bool gate_in_signal(void) { return fg.in_signal; }
void gate_release(void) { fg.releases++; fg.held = false; }
void gate_forget_pending(void) {}
bool gate_extend(uint32_t us) { ARG_UNUSED(us); return false; }
int gate_init(void) { return RADIANT_RADIO_OK_RC; }
void gate_shutdown(void) {}
uint32_t gate_max_window_us(void) { return 0u; }
uint32_t gate_min_arm_lead_us(void) { return 0u; }
enum gate_rc gate_acquire(enum gate_op op, radiant_time_t t_from, radiant_time_t t_to,
			  uint16_t follow_on_us, uint8_t prio)
{
	ARG_UNUSED(op); ARG_UNUSED(t_from); ARG_UNUSED(t_to);
	ARG_UNUSED(follow_on_us); ARG_UNUSED(prio);
	return GATE_GRANTED;
}

uint32_t radiant_swi_zli_lock(void) { return 0u; }
void radiant_swi_zli_unlock(uint32_t key) { ARG_UNUSED(key); }
int radiant_swi_register(uint32_t bits, radiant_swi_handler_t h)
{ ARG_UNUSED(bits); ARG_UNUSED(h); return 0; }
void radiant_swi_pend(uint32_t bits) { ARG_UNUSED(bits); }

/* The core's callback: records, and arms a follow-on if the test asked. */
static struct backend_probe bp;

static void arm_follow_on_if_asked(void)
{
	if (bp.arm_follow_on) {
		/* What a programmed follow-on leaves behind: an operation in
		 * the slot, staged behind nothing. */
		radiant_op.kind = OP_TX;
		radiant_op.terminal_sent = false;
		radiant_staged.pending = false;
	}
}

static void cb_rx(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);
	bp.rx_events++;
	bp.last_rx_status = evt->status;
	bp.last_rx_body_len = evt->body_len;
	memset(bp.last_rx_body, 0, sizeof(bp.last_rx_body));
	if (evt->body != NULL) {
		memcpy(bp.last_rx_body, evt->body,
		       MIN((size_t)evt->body_len, sizeof(bp.last_rx_body)));
	}
	bp.last_rx_crc_rx = evt->crc_rx;
	bp.last_rx_filter_index = evt->filter_index;
	bp.last_rx_rssi_dbm = evt->rssi_dbm;
	arm_follow_on_if_asked();
}

static void cb_tx(const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(user);
	bp.tx_events++;
	bp.last_tx_status = evt->status;
	arm_follow_on_if_asked();
}

static const struct radiant_radio_cbs bcbs = { .rx = cb_rx, .tx = cb_tx };
static const struct radiant_rx_filter bfilt = { .addr_len = 5u };
static const struct radiant_rx_filter bfilt2[2] = { { .addr_len = 5u }, { .addr_len = 5u } };

struct backend_probe *backend_probe(void) { return &bp; }

void backend_reset(void)
{
	memset(&fake_hw, 0, sizeof(fake_hw));
	memset(&fg, 0, sizeof(fg));
	memset(&bp, 0, sizeof(bp));
	memset(&radiant_op, 0, sizeof(radiant_op));
	memset(&radiant_staged, 0, sizeof(radiant_staged));
	memset(radiant_rx_buf, 0, sizeof(radiant_rx_buf));
	memset(radiant_rx_snap, 0, sizeof(radiant_rx_snap));
	memset(&nrf_diag, 0, sizeof(nrf_diag));
	memset(radiant_filter_slot, 0, sizeof(radiant_filter_slot));
	memset(&fake_radio_regs, 0, sizeof(fake_radio_regs));
	reg_shadow_forget();
	arm_memo_forget();
	grant_release_owed = false;
	core_cb_head = 0u;
	core_cb_tail = 0u;
	core_cb_depth = 0u;
	radiant_op.cbs = &bcbs;
	radiant_op.id = 7u;
	radio_on_loan = true;
	fg.held = true;
}

/* The operation is live inside a held grant, and the RADIO signal is running. */
void backend_arm_rx(bool stop_on_first)
{
	radiant_op.kind = OP_RX;
	radiant_op.stop_on_first = stop_on_first;
	radiant_op.filters = &bfilt;
	radiant_op.n_filters = 1u;
	radiant_op.addr_len = 5u;
	radiant_op.body_len = 8u;
}

void backend_arm_tx(void)
{
	radiant_op.kind = OP_TX;
}

static void signal_isr(void)
{
	fg.in_signal = true;
	radio_isr(NULL);
	fg.in_signal = false;
}

/*
 * A frame whose END interrupt runs on time: its ADDRESS captured CC_SYNC
 * 1000 us ago, long enough for any frame to have ended, so the late-ISR guard
 * sees one consistent snapshot.
 */
void backend_frame(bool crc_ok)
{
	backend_frame_at(crc_ok, 1000u);
}

/*
 * A frame whose ADDRESS captured CC_SYNC `since_address_us` before the END
 * interrupt runs. Below the frame's ADDRESS-to-END airtime, that capture can
 * only be the next frame's: the interrupt ran late.
 */
void backend_frame_at(bool crc_ok, uint32_t since_address_us)
{
	fake_hw.crc_ok = crc_ok;
	fake_hw.cc[CC_SYNC] = fake_hw.counter;
	fake_hw.counter += since_address_us;
	fake_hw.event[NRF_RADIO_EVENT_ADDRESS] = true;
	fake_hw.event[NRF_RADIO_EVENT_END] = true;
	signal_isr();
}

/* An END with EVENTS_ADDRESS left as it is, `advance_us` later. */
void backend_end_only(uint32_t advance_us)
{
	fake_hw.counter += advance_us;
	fake_hw.event[NRF_RADIO_EVENT_END] = true;
	signal_isr();
}

uint8_t *backend_rx_dma_buf(void) { return radiant_rx_buf; }
bool backend_addr_carry(void) { return radiant_op.addr_carry; }
bool backend_rx_any(void) { return radiant_op.rx_any; }
uint32_t backend_torn_air_us(void) { return rx_torn_air_us(); }
void backend_set_sw_addr_len(uint8_t n) { radiant_op.sw_addr_len = n; }
void backend_set_report_crc_fail(void) { radiant_op.report_crc_fail = true; }
uint32_t backend_late_isr_drops(void) { return nrf_diag.late_isr_drops; }

/* Two filters: logical address 0 is filter 0, logical address 3 is filter 1. */
void backend_two_filters(void)
{
	radiant_op.filters = bfilt2;
	radiant_op.n_filters = 2u;
	memset(radiant_filter_slot, 0, sizeof(radiant_filter_slot));
	radiant_filter_slot[3] = 1u;
}

/*
 * A real re-arm: program_rx() on the tracking format, opening well ahead of
 * the counter, then the slot taken as the arm call would (CLAIM_TERMINAL()).
 */
int backend_program_rx(void)
{
	struct radiant_rx_req req;
	int rc;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.filters = &bfilt;
	req.n_filters = 1u;
	req.t_open = radiant_radio_now() + 10000u;
	req.t_close = req.t_open + 1000u;
	{
		struct nrf_rx_prep prep;

		rc = rx_prepare(&req, &prep);
		if (rc == RADIANT_RADIO_OK_RC) {
			rc = program_rx(&req, &prep);
		}
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		radiant_op.kind = OP_RX;
		CLAIM_TERMINAL();
	}
	return rc;
}

void backend_tx_end(void)
{
	fake_hw.event[NRF_RADIO_EVENT_END] = true;
	signal_isr();
}

/* The window closed on its own compare: the ordinary terminal. */
void backend_window_close(void)
{
	fake_hw.event[NRF_RADIO_EVENT_DISABLED] = true;
	signal_isr();
}

void backend_dispatch(void)
{
	core_cb_swi(0u);
}

uint32_t backend_releases(void) { return fg.releases; }

/* ---- Arm-time and in-grant failure paths (TODO "Left open by the 2026-11-08 clearance") ---- */

#define BACKEND_CONN_TX 3u

static uint8_t btx_body[RADIANT_RADIO_BODY_MAX];

static void backend_late_tx_req(struct radiant_tx_req *req, uint32_t d_us)
{
	uint32_t lead;

	memset(req, 0, sizeof(*req));
	req->fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req->addr_len = req->fmt->addr_len;
	req->body = btx_body;
	req->body_len = req->fmt->body_len;
	lead = air_lead_us(req->fmt->phy, req->addr_len) + (uint32_t)T_SYNC_CAL_TX_US;
	req->t_sync_at = radiant_radio_now() + (radiant_time_t)lead + d_us;
}

int backend_program_tx(uint32_t d_us, uint32_t step)
{
	struct radiant_tx_req req;
	struct nrf_fmt_regs fmt_regs;
	int rc;

	radiant_op.conn_start_tx = BACKEND_CONN_TX;
	fake_hw.counter_step = step;
	backend_late_tx_req(&req, d_us);
	rc = format_prepare_memo(req.fmt, req.rf_index, 0u, &fmt_regs);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}
	return program_tx(&req, &fmt_regs);
}

bool backend_tx_conn_live(void) { return (fake_hw.conn_on & (1u << BACKEND_CONN_TX)) != 0u; }
uint32_t backend_shorts(void) { return fake_hw.shorts; }

void backend_grant_late_tx(void)
{
	/* A transmit staged behind a grant that arrives after its t_sync. */
	radiant_op.inited = true;
	radiant_op.kind = OP_TX;
	radiant_op.conn_start_tx = BACKEND_CONN_TX;
	backend_late_tx_req(&radiant_staged.tx, 0u);
	radiant_staged.tx.t_sync_at -= 1000u;
	(void)format_prepare_memo(radiant_staged.tx.fmt,
				  radiant_staged.tx.rf_index, 0u,
				  &radiant_staged.tx_fmt);
	radiant_staged.pending = true;
	radiant_nrf_gate_on_grant();
}

void backend_set_gate_failed(void) { radiant_op.gate_failed = true; }
bool backend_gate_failed(void) { return radiant_op.gate_failed; }
void backend_finish_failed(void) { radiant_nrf_gate_finish_failed(); }

int backend_abort(void)
{
	radiant_op.inited = true;
	return radiant_radio_abort();
}

/* The next operation takes the slot, as an arm would, but without
 * CLAIM_TERMINAL(), so a test can tell which of the two cleared the flag. */
void backend_take_slot_unclaimed(void)
{
	radiant_op.kind = OP_RX;
	radiant_op.terminal_sent = false;
}

void backend_claim_terminal(void) { CLAIM_TERMINAL(); }
bool     backend_grant_held(void) { return fg.held; }
bool     backend_op_retired(void) { return radiant_op.kind == OP_NONE; }

/* ---- The arm cache (B4): test_arm_cache.c ---- */

/* An arm call as the scheduler makes it, then the slot freed for the next one. */
int backend_rx(const struct radiant_rx_req *req)
{
	uint32_t op;
	int rc;

	radiant_op.inited = true;
	radiant_op.enabled = true;
	radiant_op.kind = OP_NONE;
	rc = radiant_radio_rx(req, &op);
	radiant_op.kind = OP_NONE;
	return rc;
}

int backend_tx(const struct radiant_tx_req *req)
{
	uint32_t op;
	int rc;

	radiant_op.inited = true;
	radiant_op.enabled = true;
	radiant_op.kind = OP_NONE;
	rc = radiant_radio_tx(req, &op);
	radiant_op.kind = OP_NONE;
	return rc;
}

/* A receive staged behind the gate and programmed by its grant. */
int backend_grant_rx(const struct radiant_rx_req *req)
{
	int rc;

	radiant_op.inited = true;
	radiant_op.enabled = true;
	rc = rx_prepare(req, &radiant_staged.rx_prep);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}
	radiant_staged.rx = *req;
	radiant_staged.pending = true;
	radiant_op.kind = OP_RX;
	radiant_nrf_gate_on_grant();
	rc = radiant_op.gate_failed ? RADIANT_RADIO_ETIME : RADIANT_RADIO_OK_RC;
	radiant_op.kind = OP_NONE;
	return rc;
}

/* The grant-end hook, as the gate calls it with the RADIO on loan. */
void backend_grant_end(void)
{
	radio_on_loan = true;
	radiant_nrf_gate_on_grant_end();
	radio_on_loan = true;
	fg.held = true;
}

/* Disable then enable, as a host reset does. */
void backend_disable_enable(void)
{
	radiant_op.inited = true;
	radiant_op.enabled = true;
	radiant_op.kind = OP_NONE;
	(void)radiant_radio_disable();
	(void)radiant_radio_enable();
	radio_on_loan = true;
	fg.held = true;
}

uint8_t backend_filter_slot(uint8_t logical) { return radiant_filter_slot[logical]; }
