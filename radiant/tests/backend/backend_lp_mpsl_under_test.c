/* SPDX-License-Identifier: Apache-2.0 */
/*
 * backend_lp_mpsl_under_test.c - the real nRF backend with
 * CONFIG_RADIANT_NRF_LOW_POWER on the MPSL gate's build (power plan pass B6),
 * included against register models as backend_under_test.c includes it.
 * Scenario radiant.backend.lp_mpsl (-DBACKEND_LP_MPSL=1). Its own image: the
 * symbols are the same backend's as the other two configurations'.
 *
 * What is real: radiant_radio_nrf.c with its LP_MPSL section of
 * radiant_radio_nrf_lp.c - the boot, the clock, the two hooks the gate calls
 * (radiant_nrf_gate_lp_slot_start()/_end()), the arm path inside a timeslot.
 * What is fake: the MPSL gate (a stub that grants in place, as an in-grant arm
 * sees it; radiant/tests/gate's gate.lp scenario tests the gate's side of the
 * hooks), the GRTC (its SYSCOUNTER read), the HFXO's onoff manager, and the
 * TIMER.
 *
 * The world: one clock in nanoseconds. The GRTC SYSCOUNTER reads whole world
 * microseconds + lf_off, and each read costs read_lat_ns of world time before
 * the next action (the read-to-START latency of a wake, the read-to-capture
 * latency of a release). The radio TIMER counts whole microseconds of world
 * time from its START, offset by a prescaler phase t_phase_ns (its first
 * increment comes 1000 - phase ns after START) and scaled by hf_ppm (the HFXO
 * against the LFXO; spike B1 measured -30 ppm on one nRF54L15 DK). A
 * transition that carried an error, or a timebase that took the HF rate
 * between timeslots, shows as radio time drifting from world time; the phases
 * are what a real START lands on, so a sweep of them is the anchor's error.
 * The public lmt_* API speaks world microseconds (lmt_run_ns() aside).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/onoff.h>
#include <nrfx.h>

#define CONFIG_RADIANT_SWI 1
#define CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL 1
#define CONFIG_RADIANT_BACKEND_NRF_IRQ_PRIO 0
#define CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN 0
#define CONFIG_SOC_COMPATIBLE_NRF54LX 1
#define CONFIG_RADIANT_NRF_LOW_POWER 1
#define CONFIG_RADIANT_NRF_LP_STATS 1
#define CONFIG_NRF_GRTC_TIMER 1
#define RADIANT_LP_LF_HZ 1000000
/* The ED sweep, so the preempted-arm cases cover program_ed() too. */
#define CONFIG_RADIANT_ED_SCAN 1

/* The HFXO's onoff manager, simulated below. */
int fake_onoff_request(struct onoff_manager *mgr, struct onoff_client *cli);
int fake_onoff_cancel_or_release(struct onoff_manager *mgr, struct onoff_client *cli);
#define onoff_request fake_onoff_request
#define onoff_cancel_or_release fake_onoff_cancel_or_release

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

#include "backend_lp_mpsl_probe.h"

NRF_RADIO_Type fake_radio_regs;
NRF_TIMER_Type fake_timer_regs;
NRF_TEMP_Type  fake_temp_regs;
struct fake_hw fake_hw;
nrf_clock_lfclk_t fake_lfclk_src;

static struct {
	uint64_t ns;        /* world time */
	uint64_t lf_off;    /* GRTC at world 0 */
	int32_t  hf_ppm;    /* TIMER rate against the world, parts per million */
	uint32_t read_lat_ns;  /* world time a GRTC read costs */
	uint32_t t_phase_ns;   /* the TIMER's prescaler phase at START, < 1000 */
	/* TIMER */
	bool     t_run;
	uint32_t t_base;    /* counter at t_t0 */
	uint64_t t_t0;
	uint32_t t_starts;
	/* HFXO (the boot request only) */
	struct onoff_manager mgr;
	bool     hf_on;
	uint32_t hf_requests;
	uint32_t hf_releases;
	/* GRTC */
	uint32_t grtc_reads;
} fw;

/* ---- TIMER ---- */

static uint32_t fw_timer(void)
{
	int64_t el;

	if (!fw.t_run) {
		return fw.t_base;
	}
	/* Whole counts, floored, so a short timeslot's sub-us drift shows as
	 * the 1 us quantisation the real TIMER would read. */
	el = (int64_t)(fw.ns - fw.t_t0) + (int64_t)fw.t_phase_ns;
	return fw.t_base + (uint32_t)((el * (1000000 + fw.hf_ppm)) / 1000000000);
}

static void fw_task(nrf_timer_task_t t)
{
	switch (t) {
	case NRF_TIMER_TASK_START:
		if (!fw.t_run) {
			fw.t_run = true;
			fw.t_t0 = fw.ns;
			fw.t_starts++;
		}
		break;
	case NRF_TIMER_TASK_STOP:
		fw.t_base = fw_timer();
		fw.t_run = false;
		break;
	case NRF_TIMER_TASK_CLEAR:
		fw.t_base = 0u;
		fw.t_t0 = fw.ns;
		break;
	default:
		break;
	}
}

/* ---- GRTC, HFXO, LFCLK ---- */

uint64_t z_nrf_grtc_timer_read(void)
{
	uint64_t v = fw.ns / 1000u + fw.lf_off;

	fw.grtc_reads++;
	fw.ns += fw.read_lat_ns;
	return v;
}

int fake_onoff_request(struct onoff_manager *mgr, struct onoff_client *cli)
{
	ARG_UNUSED(mgr);
	ARG_UNUSED(cli);
	fw.hf_requests++;
	fw.hf_on = true;
	lp_hfxo_cb(&fw.mgr, &lp.hf_cli, 0u, 0);
	return 0;
}

int fake_onoff_cancel_or_release(struct onoff_manager *mgr, struct onoff_client *cli)
{
	ARG_UNUSED(mgr);
	ARG_UNUSED(cli);
	fw.hf_releases++;
	fw.hf_on = false;
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

/* ---- The gate, as an in-grant arm sees it ---- */

bool gate_held(void) { return true; }
/* True while a simulated zero-latency signal runs (lmt_preempt_after_capture()). */
static bool lmt_in_signal;
bool gate_in_signal(void) { return lmt_in_signal; }
/* The grant generation, as the real gate moves it: odd from each START to the
 * hand-back, even after (lmt_slot_start() / lmt_slot_end()). */
static uint32_t lmt_gen;
uint32_t gate_grant_gen(void) { return lmt_gen; }
/* This gate always grants in place: nothing is ever pending. */
bool gate_request_pending(void) { return false; }
void gate_release(void) {}
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

/*
 * CLOCK_LOCK is this PRIMASK lock on the MPSL build, and it masks the
 * zero-latency signal: a simulated ACTION_END that falls inside it is taken at
 * the unlock, as the real one would be (lmt_preempt_after_capture()).
 */
static uint32_t zli_depth;
static void pre_at_unlock(void);

uint32_t radiant_swi_zli_lock(void)
{
	zli_depth++;
	return 0u;
}

void radiant_swi_zli_unlock(uint32_t key)
{
	ARG_UNUSED(key);
	if (--zli_depth == 0u) {
		pre_at_unlock();
	}
}
int radiant_swi_register(uint32_t bits, radiant_swi_handler_t h)
{ ARG_UNUSED(bits); ARG_UNUSED(h); return 0; }
void radiant_swi_pend(uint32_t bits) { ARG_UNUSED(bits); }

/* ---- Boot ---- */

static uint8_t lmt_body[RADIANT_RADIO_BODY_MAX];

/* Terminal callbacks delivered directly (outside a signal; inside one they
 * queue on the core trampoline, counted from core_cb_head). */
static uint32_t lmt_cb_calls;

static void cb_tx(const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
	lmt_cb_calls++;
}

static void cb_rx(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
	lmt_cb_calls++;
}

static void cb_ed(const struct radiant_ed_event *evt, void *user)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
	lmt_cb_calls++;
}

static const struct radiant_radio_cbs lmt_cbs = { .rx = cb_rx, .tx = cb_tx, .ed = cb_ed };
static const struct radiant_rx_filter lmt_filt = { .addr_len = 5u };

static void lmt_boot(nrf_clock_lfclk_t lf)
{
	memset(&fake_hw, 0, sizeof(fake_hw));
	memset(&fw, 0, sizeof(fw));
	memset(&radiant_op, 0, sizeof(radiant_op));
	memset(&radiant_staged, 0, sizeof(radiant_staged));
	memset(&lp, 0, sizeof(lp));
	lp.ch_anchor = -1;
	lp.ch_hf = -1;
	lp_slot_t0 = 0u;
	memset(&radiant_nrf_lp_stats, 0, sizeof(radiant_nrf_lp_stats));
	radiant_nrf_lp_stats.magic = 0x52504C31u;
	radiant_nrf_lp_stats.size = sizeof(radiant_nrf_lp_stats);
	memset(&fake_radio_regs, 0, sizeof(fake_radio_regs));
	reg_shadow_forget();
	arm_memo_forget();
	core_cb_head = 0u;
	core_cb_tail = 0u;
	core_cb_depth = 0u;
	lmt_cb_calls = 0u;
	lmt_in_signal = false;
	arm_marked = false;
	arm_gen = 0u;
	lmt_gen = 0u;
	arm_ed_lost = false;
	fake_lfclk_src = lf;
	fake_hw.counter_fn = fw_timer;
	fake_hw.task_hook = fw_task;
	fw.lf_off = LMT_LF_OFFSET;

	/* radiant_radio_init()'s order: the HFXO, the TIMER, then low power. */
	fw.ns = (uint64_t)(LMT_W0 - 100u) * 1000u;
	(void)lp_hfxo_boot();
	fw.ns = (uint64_t)LMT_W0 * 1000u;
	fw_task(NRF_TIMER_TASK_CLEAR);
	fw_task(NRF_TIMER_TASK_START);
	radiant_clock.base = 0u;
	radiant_clock.last = 0u;
	radiant_op.cbs = &lmt_cbs;
	radiant_op.next_id = 1u;
	radiant_op.inited = true;
	radiant_op.enabled = true;
	/* The connections radiant_radio_init() allocates (one handle in this
	 * model, so one bit of conn_on). */
	radiant_op.conn_start = 1u;
	radiant_op.conn_start_tx = 1u;
	radiant_op.conn_close = 1u;
	radiant_op.conn_rssi = 2u;
	radiant_op.conn_sync = 3u;
	fw.ns += 50000u;
	lp_init();
}

void lmt_reset(void) { lmt_boot(NRF_CLOCK_LFCLK_XTAL); }
void lmt_reset_lf_rc(void) { lmt_boot(NRF_CLOCK_LFCLK_RC); }

void lmt_stats(struct lmt_stats *o)
{
	const struct radiant_nrf_lp_stats *s = &radiant_nrf_lp_stats;

	o->size = s->size;
	o->state = s->state;
	o->sleeps = s->sleeps;
	o->wakes = s->wakes;
	o->late_anchors = s->late_anchors;
	o->late_wakes = s->late_wakes;
	o->range_errors = s->range_errors;
	o->lf_taken = s->lf_taken;
	o->timer_taken = s->timer_taken;
	o->disagreements = s->disagreements;
	o->max_disagree_us = s->max_disagree_us;
	o->last_disagree_q = s->last_disagree_q;
	o->hfxo_errors = s->hfxo_errors;
	o->refused = s->refused;
	o->slot_awake_us = s->slot_awake_us;
	o->start_err_n = s->start_err_n;
	o->start_err_min_us = s->start_err_min_us;
	o->start_err_max_us = s->start_err_max_us;
	o->start_err_last_us = s->start_err_last_us;
	o->missed_releases = s->missed_releases;
}

/* ---- The world ---- */

void lmt_run(uint64_t us) { fw.ns += us * 1000u; }
void lmt_run_ns(uint64_t ns) { fw.ns += ns; }
uint64_t lmt_world(void) { return fw.ns / 1000u; }
void lmt_set_read_latency_ns(uint32_t ns) { fw.read_lat_ns = ns; }
void lmt_set_timer_phase_ns(uint32_t ns) { fw.t_phase_ns = ns % 1000u; }
uint64_t lmt_radio_now(void) { return radiant_radio_now(); }
void lmt_set_hf_ppm(int32_t ppm) { fw.hf_ppm = ppm; }

/* ---- The gate's calls ---- */

void lmt_slot_start(uint64_t r_start)
{
	lmt_gen = (lmt_gen + 2u) | 1u;
	radiant_nrf_gate_lp_slot_start((radiant_time_t)r_start);
	/* What the staged operation's on_grant() leaves: the radio on loan. */
	radio_on_loan = true;
}

void lmt_slot_end(bool backstop)
{
	lmt_gen = (lmt_gen + 1u) & ~1u;
	radio_on_loan = false;
	radiant_nrf_gate_lp_slot_end(backstop);
}

/* ---- Inside a timeslot ---- */

uint64_t lmt_tx_lead(void)
{
	const struct radiant_pkt_format *f = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);

	return air_lead_us(f->phy, f->addr_len) + (uint32_t)T_SYNC_CAL_TX_US;
}

int lmt_tx_at(uint64_t t_sync)
{
	struct radiant_tx_req req;
	uint32_t op;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.addr_len = req.fmt->addr_len;
	req.body = lmt_body;
	req.body_len = req.fmt->body_len;
	req.t_sync_at = t_sync;
	return radiant_radio_tx(&req, &op);
}

bool lmt_tx_programmed(uint64_t *world_start)
{
	int64_t ticks;

	if (!fw.t_run || radiant_op.kind != OP_TX ||
	    (fake_hw.conn_on & (1u << (radiant_op.conn_start_tx & 31u))) == 0u) {
		return false;
	}
	/* The world microsecond in which the running TIMER reaches the compare
	 * (the inverse of fw_timer()'s floor: a ceiling, in ns). */
	ticks = (int64_t)(uint32_t)(fake_hw.cc[CC_TXSTART] - fw.t_base);
	{
		int64_t el = (ticks * 1000000000 + (1000000 + fw.hf_ppm) - 1) /
			     (1000000 + fw.hf_ppm);

		*world_start = (fw.t_t0 + (uint64_t)(el - (int64_t)fw.t_phase_ns)) / 1000u;
	}
	return true;
}

void lmt_retire_op(void)
{
	radiant_op.kind = OP_NONE;
	radiant_op.terminal_sent = true;
	nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
}

/* ---- State ---- */

int lmt_state(void) { return (int)lp.state; }
bool lmt_timer_running(void) { return fw.t_run; }
uint32_t lmt_timer_count(void) { return fw_timer(); }
uint32_t lmt_timer_starts(void) { return fw.t_starts; }
bool lmt_hfxo_on(void) { return fw.hf_on; }
uint32_t lmt_hfxo_requests(void) { return fw.hf_requests; }
uint32_t lmt_hfxo_releases(void) { return fw.hf_releases; }
bool lmt_timer_at_anchor(void) { return lp.clk.timer_at_anchor; }
uint32_t lmt_disable_tasks(void) { return fake_hw.disable_tasks; }
void lmt_radio_disable_now(void) { radio_disable_now(); }
uint32_t lmt_conn_on(void) { return fake_hw.conn_on; }
void lmt_conn_enable_all(void)
{
	nrfx_gppi_conn_enable(radiant_op.conn_start);
	nrfx_gppi_conn_enable(radiant_op.conn_rssi);
	nrfx_gppi_conn_enable(radiant_op.conn_sync);
}
uint32_t lmt_conn_bit_timer(void) { return 1u << (radiant_op.conn_start & 31u); }
uint32_t lmt_conn_bits_other(void)
{
	return (1u << (radiant_op.conn_rssi & 31u)) | (1u << (radiant_op.conn_sync & 31u));
}

/* ---- An arm preempted by the timeslot's end ---- */

static struct {
	uint32_t at;        /* fire at this outermost unlock of the arm (0: off) */
	uint32_t n;
	bool     restart;   /* and the next START too */
	bool     abort;     /* the signal aborts first (EXTEND_FAILED, ext-skip) */
	uint32_t writes_at_handback;
	NRF_RADIO_Type regs_at_handback;
	bool     fired;
	bool     busy;
} pre;

static void pre_fire(void)
{
	/* The zero-latency ACTION_END (and START) land here, between two
	 * instructions of the arm. */
	pre.busy = true;
	pre.fired = true;
	if (pre.abort) {
		lmt_in_signal = true;
		(void)radiant_radio_abort();
		lmt_in_signal = false;
	}
	lmt_slot_end(false);
	/* The RADIO is the other stack's from here. */
	pre.writes_at_handback = fake_hw.radio_writes;
	pre.regs_at_handback = fake_radio_regs;
	if (pre.restart) {
		lmt_run(5u);
		lmt_slot_start(0u);
	}
	pre.busy = false;
}

/*
 * The zero-latency signal can only land where PRIMASK is clear: at the end of
 * an outermost CLOCK_LOCK / zero-latency lock, or between two of them. Each
 * outermost unlock is one such point; the n-th of the arm takes the signal.
 */
static void pre_at_unlock(void)
{
	if (pre.at == 0u || pre.busy || pre.fired || ++pre.n != pre.at) {
		return;
	}
	pre_fire();
}

void lmt_preempt_after_capture(uint32_t n, bool restart)
{
	lmt_preempt_mode(n, restart, false);
}

void lmt_preempt_mode(uint32_t n, bool restart, bool abort)
{
	if (n == 0u) {
		/* Disarm, keeping `fired` for lmt_preempt_fired(). */
		pre.at = 0u;
		return;
	}
	memset(&pre, 0, sizeof(pre));
	pre.at = n;
	pre.restart = restart;
	pre.abort = abort;
}

bool lmt_preempt_fired(void)
{
	return pre.fired;
}

bool lmt_op_idle(void) { return radiant_op.kind == OP_NONE; }
bool lmt_tx_conn_on(void)
{
	return (fake_hw.conn_on & (1u << (radiant_op.conn_start_tx & 31u))) != 0u;
}

bool lmt_radio_written_after_handback(void)
{
	return pre.fired &&
	       (fake_hw.radio_writes != pre.writes_at_handback ||
		memcmp(&fake_radio_regs, &pre.regs_at_handback, sizeof(fake_radio_regs)) != 0);
}

uint32_t lmt_terminals(void) { return lmt_cb_calls + core_cb_head; }

int lmt_rx_at(uint64_t t_open, uint32_t len_us)
{
	struct radiant_rx_req req;
	uint32_t op;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.filters = &lmt_filt;
	req.n_filters = 1u;
	req.t_open = t_open;
	req.t_close = t_open + len_us;
	return radiant_radio_rx(&req, &op);
}

int lmt_ed_at(uint64_t t_start)
{
	struct radiant_ed_req req;
	uint32_t op;

	memset(&req, 0, sizeof(req));
	req.rf_index_lo = 10u;
	req.rf_index_hi = 11u;
	req.dwell_us = 2000u;
	req.t_start = t_start;
	return radiant_radio_ed(&req, &op);
}

bool lmt_window_conns_on(void)
{
	return (fake_hw.conn_on & ((1u << (radiant_op.conn_start & 31u)) |
				   (1u << (radiant_op.conn_start_tx & 31u)) |
				   (1u << (radiant_op.conn_close & 31u)))) != 0u;
}
