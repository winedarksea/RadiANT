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

struct backend_probe *backend_probe(void) { return &bp; }

void backend_reset(void)
{
	memset(&fake_hw, 0, sizeof(fake_hw));
	memset(&fg, 0, sizeof(fg));
	memset(&bp, 0, sizeof(bp));
	memset(&radiant_op, 0, sizeof(radiant_op));
	memset(&radiant_staged, 0, sizeof(radiant_staged));
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

void backend_frame(bool crc_ok)
{
	fake_hw.crc_ok = crc_ok;
	fake_hw.event[NRF_RADIO_EVENT_ADDRESS] = true;
	fake_hw.event[NRF_RADIO_EVENT_END] = true;
	signal_isr();
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

	radiant_op.conn_start_tx = BACKEND_CONN_TX;
	fake_hw.counter_step = step;
	backend_late_tx_req(&req, d_us);
	return program_tx(&req);
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
