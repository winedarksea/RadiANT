/* SPDX-License-Identifier: Apache-2.0 */
/*
 * backend_lp_under_test.c - the real nRF backend with
 * CONFIG_RADIANT_NRF_LOW_POWER (power plan pass B3), included against register
 * models as backend_under_test.c includes it for the MPSL gate. Its own
 * scenario (radiant.backend.lp, -DBACKEND_LP=1): the two configurations define
 * the same backend symbols and cannot share an image.
 *
 * The world. One clock, `fw.now`, in microseconds: it IS the GRTC SYSCOUNTER
 * (1 MHz), and the radio TIMER counts it while running (the two crystals at
 * the same rate, so radio time t is world LPT_W0 + t exactly, and any error a
 * transition makes shows as a non-zero difference). GRTC compares fire when
 * the world reaches them - a value written behind the counter fires at once,
 * as the GRTC's greater-or-equal compare does - and the anchor channel's DPPI
 * (TIMER START + CAPTURE[5]) acts before its handler runs. The HFXO starts
 * fw.hf_startup after its request and then calls the backend's callback, as
 * the clock interrupt does.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/onoff.h>
#include <nrfx.h>

#define CONFIG_RADIANT_BACKEND_NRF_IRQ_PRIO 1
#define CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN 0
#define CONFIG_SOC_COMPATIBLE_NRF54LX 1
#define CONFIG_RADIANT_NRF_LOW_POWER 1
#define CONFIG_RADIANT_NRF_LP_HFXO_STARTUP_US 800
#define CONFIG_RADIANT_NRF_LP_KEEP_ON_US 3000
#define CONFIG_RADIANT_NRF_LP_LINGER_MS 1000
#define CONFIG_RADIANT_NRF_LP_STATS 1
#define CONFIG_NRF_GRTC_TIMER 1
#define RADIANT_LP_LF_HZ 1000000
/* nRF54L errata 20: CONSTLAT through nrfx's count. */
#define NRF54L_ERRATA_20_PRESENT 1
static inline bool nrf54l_errata_20(void) { return true; }

/* The HFXO's onoff manager, simulated below. */
int fake_onoff_request(struct onoff_manager *mgr, struct onoff_client *cli);
int fake_onoff_cancel_or_release(struct onoff_manager *mgr, struct onoff_client *cli);
#define onoff_request fake_onoff_request
#define onoff_cancel_or_release fake_onoff_cancel_or_release
/* The always-on fallback after an HFXO failure. */
int fake_hf_fallback(void);
#define LP_HF_FALLBACK() fake_hf_fallback()

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

#include "backend_lp_probe.h"

NRF_RADIO_Type fake_radio_regs;
NRF_TIMER_Type fake_timer_regs;
NRF_TEMP_Type  fake_temp_regs;
struct fake_hw fake_hw;
nrf_clock_lfclk_t fake_lfclk_src;

#define FW_CHANNELS 4

static struct {
	uint64_t now;
	/* TIMER */
	bool     t_run;
	uint32_t t_base;   /* counter at t_t0 */
	uint64_t t_t0;
	/* GRTC */
	struct {
		bool     armed;
		bool     pending;   /* fired, handler not yet run */
		uint64_t cc;
		z_nrf_grtc_timer_compare_handler_t h;
		void    *u;
	} ch[FW_CHANNELS];
	/* The GRTC interrupt is held off until this time (a long irq_lock or a
	 * higher-priority interrupt); then nrfx services the pending channels in
	 * ascending index, as its CTZ loop does. */
	uint64_t isr_hold_until;
	int32_t  n_alloc;
	bool     break_dppi;
	uint32_t delay_anchor_set;
	/* HFXO */
	struct onoff_manager mgr;
	bool     hf_on;
	bool     hf_starting;
	bool     hf_sync;
	uint64_t hf_ready_at;
	uint32_t hf_startup;
	uint32_t hf_requests;
	bool     hf_req_error;  /* onoff_request() fails */
	bool     hf_cb_error;   /* the start completes with an error */
	bool     hf_shared;     /* another client holds the HFXO: start is synchronous */
	bool     fallback;      /* LP_HF_FALLBACK() ran */
	int      fallback_rc;   /* what it returns */
	uint64_t hf_on_since;
	uint64_t hf_on_us;
	int      constlat;
} fw;

static struct lpt_probe lpp;

/* ---- TIMER ---- */

static uint32_t fw_timer(void)
{
	return fw.t_run ? fw.t_base + (uint32_t)(fw.now - fw.t_t0) : fw.t_base;
}

static void fw_task(nrf_timer_task_t t)
{
	switch (t) {
	case NRF_TIMER_TASK_START:
		if (!fw.t_run) {
			fw.t_run = true;
			fw.t_t0 = fw.now;
		}
		break;
	case NRF_TIMER_TASK_STOP:
		fw.t_base = fw_timer();
		fw.t_run = false;
		break;
	case NRF_TIMER_TASK_CLEAR:
		fw.t_base = 0u;
		fw.t_t0 = fw.now;
		break;
	default:
		break;
	}
}

/* ---- GRTC ---- */

int32_t z_nrf_grtc_timer_chan_alloc(void)
{
	return (fw.n_alloc < FW_CHANNELS) ? fw.n_alloc++ : -ENOMEM;
}

void z_nrf_grtc_timer_chan_free(int32_t chan)
{
	ARG_UNUSED(chan);
}

uint64_t z_nrf_grtc_timer_read(void)
{
	return fw.now;
}

uint32_t z_nrf_grtc_timer_compare_evt_address_get(int32_t chan)
{
	return 0x5000u + (uint32_t)chan;
}

int z_nrf_grtc_timer_set(int32_t chan, uint64_t target,
			 z_nrf_grtc_timer_compare_handler_t h, void *u)
{
	fw.ch[chan].armed = true;
	fw.ch[chan].pending = false;   /* nrfx clears the event with the write */
	fw.ch[chan].cc = target;
	fw.ch[chan].h = h;
	fw.ch[chan].u = u;
	if (chan == lp.ch_anchor && fw.delay_anchor_set != 0u) {
		/* The write was preempted this long. */
		fw.now += fw.delay_anchor_set;
		fw.delay_anchor_set = 0u;
	}
	return 0;
}

void z_nrf_grtc_timer_abort(int32_t chan)
{
	/* An event already latched still reaches the handler (nrfx reads it). */
	fw.ch[chan].armed = false;
}

/* ---- HFXO, CONSTLAT, LFCLK ---- */

static void fw_hf_up(void)
{
	fw.hf_starting = false;
	fw.hf_on = true;
	fw.hf_on_since = fw.now;
}

int fake_hf_fallback(void)
{
	fw.fallback = true;
	if (fw.fallback_rc == 0) {
		fw_hf_up();
	}
	return fw.fallback_rc;
}

int fake_onoff_request(struct onoff_manager *mgr, struct onoff_client *cli)
{
	ARG_UNUSED(mgr);
	ARG_UNUSED(cli);
	fw.hf_requests++;
	if (fw.hf_req_error) {
		return -EIO;
	}
	if (fw.hf_sync || fw.hf_shared) {
		fw_hf_up();
		lp_hfxo_cb(&fw.mgr, &lp.hf_cli, 0u, 0);
	} else {
		fw.hf_starting = true;
		fw.hf_ready_at = fw.now + fw.hf_startup;
	}
	return 0;
}

int fake_onoff_cancel_or_release(struct onoff_manager *mgr, struct onoff_client *cli)
{
	ARG_UNUSED(mgr);
	ARG_UNUSED(cli);
	if (fw.hf_on) {
		fw.hf_on_us += fw.now - fw.hf_on_since;
	}
	fw.hf_on = false;
	fw.hf_starting = false;
	return 0;
}

struct onoff_manager *z_nrf_clock_control_get_onoff(clock_control_subsys_t sys)
{
	ARG_UNUSED(sys);
	return &fw.mgr;
}

void z_nrf_clock_control_lf_on(enum nrf_lfclk_start_mode start_mode)
{
	ARG_UNUSED(start_mode);
}

int nrfx_power_constlat_mode_request(void)
{
	return (++fw.constlat == 1) ? 0 : -EALREADY;
}

int nrfx_power_constlat_mode_free(void)
{
	return (--fw.constlat == 0) ? 0 : -EBUSY;
}

/* ---- The world ---- */

/*
 * Fire the earliest event due at or before `until`. False if none is. A
 * compare's hardware part (the anchor DPPI) acts at its tick; its handler
 * runs then, or at the end of an interrupt hold, lowest channel first.
 */
static bool fw_step(uint64_t until)
{
	uint64_t best = UINT64_MAX;
	int      which = -1;
	bool     service = false;

	for (int c = 0; c < FW_CHANNELS; c++) {
		if (fw.ch[c].armed && fw.ch[c].cc < best) {
			best = fw.ch[c].cc;
			which = c;
		}
	}
	for (int c = 0; c < FW_CHANNELS; c++) {
		if (fw.ch[c].pending) {
			uint64_t at = (fw.isr_hold_until > fw.now) ? fw.isr_hold_until : fw.now;

			if (at < best || (at == best && !service)) {
				best = at;
				which = c;
				service = true;
			}
			break;   /* the lowest pending channel is serviced first */
		}
	}
	if (fw.hf_starting && fw.hf_ready_at < best) {
		best = fw.hf_ready_at;
		which = FW_CHANNELS;
		service = false;
	}
	if (which < 0 || best > until) {
		return false;
	}
	if (best > fw.now) {
		fw.now = best;
	}
	if (which == FW_CHANNELS) {
		fw_hf_up();
		if (fw.hf_cb_error) {
			fw.hf_on = false;
			lp_hfxo_cb(&fw.mgr, &lp.hf_cli, 0u, -EIO);
		} else {
			lp_hfxo_cb(&fw.mgr, &lp.hf_cli, 0u, 0);
		}
		return true;
	}
	if (!service) {
		/* The compare event: hardware first. */
		fw.ch[which].armed = false;
		fw.ch[which].pending = true;
		if (which == lp.ch_anchor &&
		    (fake_hw.conn_on & (1u << (lp.conn_anchor & 31u))) != 0u) {
			if (fw.break_dppi) {
				fw.break_dppi = false;
			} else {
				fw_task(NRF_TIMER_TASK_START);
				fake_hw.cc[LP_CC_ANCHOR] = fw_timer();
			}
		}
		if (fw.isr_hold_until > fw.now) {
			return true;
		}
		/* Lower channels still pending go first. */
		for (int c = 0; c < which; c++) {
			if (fw.ch[c].pending) {
				return true;
			}
		}
	}
	fw.ch[which].pending = false;
	fw.ch[which].h(which, fw.ch[which].cc, fw.ch[which].u);
	return true;
}

void lpt_run_to(uint64_t world)
{
	while (fw_step(world)) {
	}
	if (fw.now < world) {
		fw.now = world;
	}
}

void lpt_run(uint64_t us)
{
	lpt_run_to(fw.now + us);
}

/* ---- The core's callbacks ---- */

static uint8_t lpt_body[RADIANT_RADIO_BODY_MAX];

static void lpt_make_tx(struct radiant_tx_req *req, uint64_t t_sync)
{
	memset(req, 0, sizeof(*req));
	req->fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req->addr_len = req->fmt->addr_len;
	req->body = lpt_body;
	req->body_len = req->fmt->body_len;
	req->t_sync_at = t_sync;
}

static void cb_tx(const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(user);
	lpp.tx_events++;
	lpp.last_tx_status = evt->status;
	if (evt->status == RADIANT_RADIO_STATUS_FAILED) {
		lpp.failed_events++;
		if (lpp.on_failed_arm_us != 0u) {
			lpp.nested_arms++;
			lpp.nested_rc = lpt_tx_at(radiant_radio_now() + lpp.on_failed_arm_us);
		}
	} else if (evt->status == RADIANT_RADIO_STATUS_OK && lpp.period_us != 0u) {
		lpp.next_sync += lpp.period_us;
		lpp.nested_arms++;
		lpp.nested_rc = lpt_tx_at(lpp.next_sync);
	}
}

static void cb_rx(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);
	lpp.rx_events++;
	lpp.last_rx_status = evt->status;
}

static const struct radiant_radio_cbs lpt_cbs = { .rx = cb_rx, .tx = cb_tx };
static const struct radiant_rx_filter lpt_filt = { .addr_len = 5u };

/* ---- Boot ---- */

static void lpt_boot(nrf_clock_lfclk_t lf)
{
	memset(&fake_hw, 0, sizeof(fake_hw));
	memset(&fw, 0, sizeof(fw));
	memset(&lpp, 0, sizeof(lpp));
	memset(&radiant_op, 0, sizeof(radiant_op));
	memset(&radiant_staged, 0, sizeof(radiant_staged));
	memset(&lp, 0, sizeof(lp));
	lp.ch_anchor = -1;
	lp.ch_hf = -1;
	memset(&radiant_nrf_lp_stats, 0, sizeof(radiant_nrf_lp_stats));
	radiant_nrf_lp_stats.magic = 0x52504C31u;
	(void)k_work_cancel(&lp_fail_work);
	radiant_nrf_lp_stats.size = sizeof(radiant_nrf_lp_stats);
	memset(&fake_radio_regs, 0, sizeof(fake_radio_regs));
	reg_shadow_forget();
	arm_memo_forget();
	fake_lfclk_src = lf;
	fake_hw.counter_fn = fw_timer;
	fake_hw.task_hook = fw_task;
	fw.hf_startup = 680u;   /* spike B1 S4: p50 655, max 680 */

	/* radiant_radio_init()'s order: HFXO, CONSTLAT, TIMER, low power. */
	fw.now = LPT_W0 - 100u;
	fw.hf_sync = true;
	(void)lp_hfxo_boot();
	fw.hf_sync = false;
	lp_constlat(true);
	fw.now = LPT_W0;
	fw_task(NRF_TIMER_TASK_CLEAR);
	fw_task(NRF_TIMER_TASK_START);
	radiant_clock.base = 0u;
	radiant_clock.last = 0u;
	radiant_op.cbs = &lpt_cbs;
	radiant_op.next_id = 1u;
	radiant_op.inited = true;
	radiant_op.enabled = true;
	lp_init();
	/* The boot re-anchor. */
	lpt_run(500u);
}

void lpt_reset(void) { lpt_boot(NRF_CLOCK_LFCLK_XTAL); }
void lpt_reset_lf_rc(void) { lpt_boot(NRF_CLOCK_LFCLK_RC); }
struct lpt_probe *lpt_probe(void) { return &lpp; }

void lpt_stats(struct lpt_stats *o)
{
	const struct radiant_nrf_lp_stats *s = &radiant_nrf_lp_stats;

	o->state = s->state;
	o->sleeps = s->sleeps;
	o->wakes = s->wakes;
	o->late_anchors = s->late_anchors;
	o->late_wakes = s->late_wakes;
	o->hfxo_late_edge2 = s->hfxo_late_edge2;
	o->reanchors = s->reanchors;
	o->range_errors = s->range_errors;
	o->lf_taken = s->lf_taken;
	o->timer_taken = s->timer_taken;
	o->disagreements = s->disagreements;
	o->max_disagree_us = s->max_disagree_us;
	o->max_wake_slack_us = s->max_wake_slack_us;
	o->max_wake_late_us = s->max_wake_late_us;
	o->asap_wakes = s->asap_wakes;
	o->release_cancels = s->release_cancels;
	o->spurious = s->spurious;
	o->hfxo_errors = s->hfxo_errors;
	o->refused = s->refused;
	o->edge_order = s->edge_order;
}

uint64_t lpt_world(void) { return fw.now; }
uint64_t lpt_radio_now(void) { return radiant_radio_now(); }

/* ---- Operations ---- */

uint64_t lpt_tx_lead(void)
{
	const struct radiant_pkt_format *f = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);

	return air_lead_us(f->phy, f->addr_len) + (uint32_t)T_SYNC_CAL_TX_US;
}

uint64_t lpt_rx_lead(void)
{
	const struct radiant_pkt_format *f = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);

	return air_lead_us(f->phy, f->addr_len);
}

int lpt_tx_at(uint64_t t_sync)
{
	struct radiant_tx_req req;
	uint32_t op;

	lpt_make_tx(&req, t_sync);
	return radiant_radio_tx(&req, &op);
}

static bool lpt_cc_world(uint8_t cc, uint64_t *world)
{
	if (!fw.t_run || radiant_op.kind == OP_NONE || radiant_staged.pending ||
	    (fake_hw.conn_on & 1u) == 0u) {
		return false;
	}
	*world = fw.t_t0 + (uint32_t)(fake_hw.cc[cc] - fw.t_base);
	return true;
}

bool lpt_tx_programmed(uint64_t *world_start)
{
	return radiant_op.kind == OP_TX && lpt_cc_world(CC_TXSTART, world_start);
}

bool lpt_rx_programmed(uint64_t *world_start)
{
	return radiant_op.kind == OP_RX && lpt_cc_world(CC_START, world_start);
}

void lpt_tx_end(void)
{
	uint64_t start;

	if (!lpt_tx_programmed(&start)) {
		return;
	}
	lpt_run_to(start + 400u);
	fake_hw.cc[CC_SYNC] = fw_timer() - 300u;
	fake_hw.event[NRF_RADIO_EVENT_END] = true;
	radio_isr(NULL);
}

int lpt_rx_at(uint64_t t_open, uint32_t len_us)
{
	struct radiant_rx_req req;
	uint32_t op;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.filters = &lpt_filt;
	req.n_filters = 1u;
	req.t_open = t_open;
	req.t_close = t_open + len_us;
	return radiant_radio_rx(&req, &op);
}

int lpt_abort(void) { return radiant_radio_abort(); }
int lpt_disable(void) { return radiant_radio_disable(); }
int lpt_enable(void)
{
	/* radiant_radio_enable() returns early on an enabled radio. */
	radiant_op.enabled = false;
	return radiant_radio_enable();
}
void lpt_radio_disable_now(void) { radio_disable_now(); }

int lpt_acquire(uint64_t t_from, bool prio_normal)
{
	return (int)gate_acquire(GATE_OP_RX, t_from, t_from + 1000u, 0u,
				 prio_normal ? RADIANT_GATE_PRIO_NORMAL
					     : RADIANT_GATE_PRIO_HIGH);
}

void lpt_stage_tx_late(uint64_t t_sync)
{
	/* radiant_radio_tx()'s PENDING branch, finishing after the wake. */
	lpt_make_tx(&radiant_staged.tx, t_sync);
	radiant_staged.tx_fmt = *radiant_staged.tx.fmt;
	radiant_staged.tx.fmt = &radiant_staged.tx_fmt;
	(void)format_prepare_memo(radiant_staged.tx.fmt, radiant_staged.tx.rf_index, 0u,
				  &radiant_staged.tx_regs);
	radiant_op.kind = OP_TX;
	radiant_op.terminal_sent = false;
	radiant_op.id = radiant_op.next_id++;
	radiant_staged.pending = true;
	lp_staged();
}

/* ---- State ---- */

int lpt_state(void) { return (int)lp.state; }
bool lpt_staged(void) { return radiant_staged.pending; }
bool lpt_op_idle(void) { return radiant_op.kind == OP_NONE; }
bool lpt_hfxo_on(void) { return fw.hf_on || fw.hf_starting; }
bool lpt_timer_running(void) { return fw.t_run; }
int lpt_constlat_refs(void) { return fw.constlat; }
uint32_t lpt_hfxo_requests(void) { return fw.hf_requests; }
uint32_t lpt_disable_tasks(void) { return fake_hw.disable_tasks; }
bool lpt_timer_at_anchor(void) { return lp.clk.timer_at_anchor; }

uint64_t lpt_hfxo_on_us(void)
{
	return fw.hf_on_us + (fw.hf_on ? fw.now - fw.hf_on_since : 0u);
}

void lpt_set_hfxo_startup(uint32_t us) { fw.hf_startup = us; }
void lpt_break_next_dppi(void) { fw.break_dppi = true; }
void lpt_delay_next_anchor_set(uint32_t ticks) { fw.delay_anchor_set = ticks; }
void lpt_set_disable_hangs(bool on) { fake_hw.disable_hangs = on; }
void lpt_hold_isr_until(uint64_t world) { fw.isr_hold_until = world; }
void lpt_hfxo_errors(bool on_request, bool on_start)
{
	fw.hf_req_error = on_request;
	fw.hf_cb_error = on_start;
}
void lpt_hfxo_shared(bool on) { fw.hf_shared = on; }
bool lpt_hf_fallback(void) { return fw.fallback; }
void lpt_set_fallback_rc(int rc) { fw.fallback_rc = rc; }
void lpt_run_work(void) { k_msleep(2); }

int lpt_boot_hfxo_timeout(bool *still_queued)
{
	int rc;

	memset(&lp, 0, sizeof(lp));
	lp.ch_anchor = -1;
	lp.ch_hf = -1;
	fw.hf_sync = false;
	fw.hf_startup = 1000000u;   /* never within the boot wait */
	rc = lp_hfxo_boot();
	*still_queued = fw.hf_starting || lp.hfxo_req;
	return rc;
}

