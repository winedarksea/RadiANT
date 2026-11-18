/* SPDX-License-Identifier: Apache-2.0 */
/*
 * backend_lp_under_test.c - the real nRF backend with
 * CONFIG_RADIANT_NRF_LOW_POWER, included against register models as
 * backend_under_test.c includes it for the MPSL gate. Two scenarios, one
 * file: the two configurations define the same backend symbols and cannot
 * share an image.
 *
 *   radiant.backend.lp      (-DBACKEND_LP=1, pass B3): the nRF54L15 shape -
 *                           the GRTC SYSCOUNTER (1 MHz), DPPI, CONSTLAT
 *                           (errata 20), the TIMER stopped and cleared.
 *   radiant.backend.lp_rtc  (-DBACKEND_LP_RTC=1, which defines LPT_RTC,
 *                           pass B5): the nRF52840 shape - RTC1 (32768 Hz,
 *                           24-bit, behind Zephyr's nrf_rtc_timer API and
 *                           its rules), PPI, no CONSTLAT, and the TIMER
 *                           stopped and SHUT DOWN (anomaly 78).
 *
 * The world. One clock, `fw.q`, in q units of 1/512 us: the radio TIMER
 * counts whole microseconds of it while running (the two crystals at the
 * same rate, so radio time t is world LPT_W0 + t exactly, and any error a
 * transition makes shows as a non-zero difference), and the LF counter
 * counts its ticks - 512 q (1 us) on the GRTC, which therefore IS world time
 * in microseconds, and 15625 q (30.517578125 us) on RTC1, whose ticks fall
 * at no fixed phase to the TIMER's. LF compares fire when the world reaches
 * them, and the anchor channel's (D)PPI (TIMER START + CAPTURE[5]) acts
 * before its handler runs. The HFXO starts fw.hf_startup us after its
 * request and then calls the backend's callback, as the clock interrupt
 * does. The public lpt_* API speaks microseconds of world time throughout.
 *
 * The two counters' drivers, as modelled:
 *   GRTC  z_nrf_grtc_timer_set() writes any value unchanged; a value behind
 *         the counter fires at once (the GRTC compare is greater-or-equal);
 *         an abort leaves an already-latched event for the handler (nrfx
 *         reads it).
 *   RTC1  z_nrf_rtc_timer_exact_set() refuses a target in the past or fewer
 *         than 3 ticks ahead (MIN_CYCLES_FROM_NOW) - leaving the CC written
 *         and its event routed, as set_alarm() does, until an abort - and
 *         z_nrf_rtc_timer_set() moves a too-close one to 3 ticks ahead or,
 *         for a past one, forces the interrupt with no compare event (so no
 *         PPI); both refuse a target more than 2^23 ticks ahead; an abort
 *         clears a latched event, so its handler never runs.
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
/*
 * The rest of the backend keeps the nRF54L shape in both scenarios (its
 * RADIO interrupt lines, register models); only the low-power path's LF
 * counter, CONSTLAT and TIMER halt differ, and those are what the RTC
 * scenario exists to test.
 */
#define CONFIG_SOC_COMPATIBLE_NRF54LX 1
#define CONFIG_RADIANT_NRF_LOW_POWER 1
#define CONFIG_RADIANT_NRF_LP_KEEP_ON_US 3000
#define CONFIG_RADIANT_NRF_LP_LINGER_MS 1000
#define CONFIG_RADIANT_NRF_LP_STATS 1
#if defined(LPT_RTC)
#define CONFIG_NRF_RTC_TIMER 1
#define CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT 2
#define RADIANT_LP_LF_HZ 32768
#define CONFIG_RADIANT_NRF_LP_HFXO_STARTUP_US 500
/* The software start's wait for the counter's step, simulated below. */
uint64_t fake_rtc_wait_edge(void);
#define LP_CNT_WAIT_EDGE() fake_rtc_wait_edge()
#else
#define CONFIG_NRF_GRTC_TIMER 1
#define RADIANT_LP_LF_HZ 1000000
#define CONFIG_RADIANT_NRF_LP_HFXO_STARTUP_US 800
/* nRF54L errata 20: CONSTLAT through nrfx's count. */
#define NRF54L_ERRATA_20_PRESENT 1
static inline bool nrf54l_errata_20(void) { return true; }
#endif

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
/* World q units per microsecond, and per LF tick. */
#define FW_Q 512u
#if defined(LPT_RTC)
#define FW_TICK_Q 15625u
/* The driver's scheduling rules (RTC1's COUNTER is 24 bits). */
#define FW_RTC_SPAN     (1ull << 23)
#define FW_RTC_MIN_LEAD 3u
#else
#define FW_TICK_Q 512u
#endif
/* What SHUTDOWN leaves in CC5 in this model (see fw_task()). */
#define FW_CC5_POISON 0xA5A5A5A5u

static struct {
	uint64_t q;        /* world time, 1/512 us */
	uint64_t lf_off;   /* LF counter at world 0 */
	/* TIMER */
	bool     t_run;
	uint32_t t_base;   /* counter at t_t0q */
	uint64_t t_t0q;
	uint32_t t_shutdowns;
	uint32_t t_stop_then_shutdown;
	int      t_last_task;
	/* LF compare channels, by channel number */
	struct {
		bool     armed;     /* the hardware compare will fire at cc */
		bool     hw_only;   /* ... but no handler follows (a refused RTC write) */
		bool     pending;   /* fired (or forced), handler not yet run */
		uint64_t cc;        /* the compare's tick */
		uint64_t target;    /* what the handler is told (the driver's target) */
		lp_cnt_handler_t h;
		void    *u;
	} ch[FW_CHANNELS];
	/* The LF interrupt is held off until this world time, q (a long
	 * irq_lock or a higher-priority interrupt); then the driver services the
	 * pending channels in ascending index, as both drivers' loops do. */
	uint64_t isr_hold_until;
	int32_t  n_alloc;
	bool     break_dppi;
	uint32_t delay_anchor_set;   /* us */
	/* RTC driver bookkeeping, for the RTC-specific cases. */
	uint32_t exact_refusals;
	uint32_t span_refusals;
	uint32_t min_exact_ahead;
	uint32_t wait_edges;
	uint32_t hf_sets;
	/* HFXO */
	struct onoff_manager mgr;
	bool     hf_on;
	bool     hf_starting;
	bool     hf_sync;
	uint64_t hf_ready_at;   /* q */
	uint32_t hf_startup;    /* us */
	uint32_t hf_requests;
	bool     hf_req_error;  /* onoff_request() fails */
	bool     hf_cb_error;   /* the start completes with an error */
	bool     hf_shared;     /* another client holds the HFXO: start is synchronous */
	bool     fallback;      /* LP_HF_FALLBACK() ran */
	int      fallback_rc;   /* what it returns */
	uint64_t hf_on_since;   /* q */
	uint64_t hf_on_q;
	int      constlat;
} fw;

static struct lpt_probe lpp;

/* ---- TIMER ---- */

static uint32_t fw_timer(void)
{
	return fw.t_run ? fw.t_base + (uint32_t)((fw.q - fw.t_t0q) / FW_Q) : fw.t_base;
}

static void fw_task(nrf_timer_task_t t)
{
	switch (t) {
	case NRF_TIMER_TASK_START:
		if (!fw.t_run) {
			fw.t_run = true;
			fw.t_t0q = fw.q;
		}
		break;
	case NRF_TIMER_TASK_STOP:
		fw.t_base = fw_timer();
		fw.t_run = false;
		break;
	case NRF_TIMER_TASK_CLEAR:
		fw.t_base = 0u;
		fw.t_t0q = fw.q;
		break;
	case NRF_TIMER_TASK_SHUTDOWN:
		/*
		 * Spike B1 S1 on the nRF52840: SHUTDOWN zeroes the counter and
		 * keeps the configuration and CC registers. This model is
		 * stricter for CC5 alone, which it overwrites, so a release
		 * that read its capture after the halt would read garbage.
		 */
		if (fw.t_last_task == (int)NRF_TIMER_TASK_STOP) {
			fw.t_stop_then_shutdown++;
		}
		fw.t_shutdowns++;
		fw.t_base = 0u;
		fw.t_run = false;
		fake_hw.cc[LP_CC_ANCHOR] = FW_CC5_POISON;
		break;
	default:
		break;
	}
	fw.t_last_task = (int)t;
}

/* ---- The LF counter ---- */

static uint64_t fw_lf(void)
{
	return fw.q / FW_TICK_Q + fw.lf_off;
}

/* World time, q, at which LF tick `tick` begins (at once if passed). */
static uint64_t fw_tick_q(uint64_t tick)
{
	return (tick <= fw.lf_off) ? 0u : (tick - fw.lf_off) * FW_TICK_Q;
}

static void fw_arm(int32_t chan, uint64_t cc, uint64_t target,
		   lp_cnt_handler_t h, void *u, bool hw_only)
{
	fw.ch[chan].armed = true;
	fw.ch[chan].hw_only = hw_only;
	fw.ch[chan].pending = false;   /* the driver clears the event with the write */
	fw.ch[chan].cc = cc;
	fw.ch[chan].target = target;
	if (!hw_only) {
		fw.ch[chan].h = h;
		fw.ch[chan].u = u;
	}
}

/* The anchor channel's write was preempted this long. */
static void fw_preempt_anchor_write(int32_t chan)
{
	if (chan == lp.ch_anchor && fw.delay_anchor_set != 0u) {
		fw.q += (uint64_t)fw.delay_anchor_set * FW_Q;
		fw.delay_anchor_set = 0u;
	}
}

#if defined(LPT_RTC)

int32_t z_nrf_rtc_timer_chan_alloc(void)
{
	/* The driver hands out the highest free channel first: 2, then 1. */
	if (fw.n_alloc >= CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT) {
		return -ENOMEM;
	}
	return CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT - fw.n_alloc++;
}

void z_nrf_rtc_timer_chan_free(int32_t chan)
{
	ARG_UNUSED(chan);
}

uint64_t z_nrf_rtc_timer_read(void)
{
	/* The driver's extension of the 24-bit COUNTER (overflow count << 24
	 * plus COUNTER): the world's tick count, never wrapping. */
	return fw_lf();
}

uint32_t z_nrf_rtc_timer_compare_evt_address_get(int32_t chan)
{
	return 0x4001100u + 4u * (uint32_t)chan;
}

static int fw_rtc_set(int32_t chan, uint64_t target,
		      z_nrf_rtc_timer_compare_handler_t h, void *u, bool exact)
{
	uint64_t now;

	fw_preempt_anchor_write(chan);
	now = z_nrf_rtc_timer_read();
	if (chan != lp.ch_anchor) {
		fw.hf_sets++;
	}
	if (target > now && target - now > FW_RTC_SPAN) {
		fw.span_refusals++;
		return -EINVAL;
	}
	if (target <= now) {
		if (exact) {
			fw.exact_refusals++;
			return -EINVAL;
		}
		/* Forced interrupt: the handler runs, no compare event, no PPI. */
		fw.ch[chan].armed = false;
		fw.ch[chan].hw_only = false;
		fw.ch[chan].pending = true;
		fw.ch[chan].target = target;
		fw.ch[chan].h = h;
		fw.ch[chan].u = u;
		return 0;
	}
	if (target - now < FW_RTC_MIN_LEAD) {
		if (exact) {
			/* set_alarm() wrote the CC and routed its event first. */
			fw.exact_refusals++;
			fw_arm(chan, target, target, h, u, true);
			return -EINVAL;
		}
		fw_arm(chan, now + FW_RTC_MIN_LEAD, target, h, u, false);
		return 0;
	}
	if (exact && (fw.min_exact_ahead == 0u || target - now < fw.min_exact_ahead)) {
		fw.min_exact_ahead = (uint32_t)(target - now);
	}
	fw_arm(chan, target, target, h, u, false);
	return 0;
}

int z_nrf_rtc_timer_set(int32_t chan, uint64_t target,
			z_nrf_rtc_timer_compare_handler_t h, void *u)
{
	return fw_rtc_set(chan, target, h, u, false);
}

int z_nrf_rtc_timer_exact_set(int32_t chan, uint64_t target,
			      z_nrf_rtc_timer_compare_handler_t h, void *u)
{
	return fw_rtc_set(chan, target, h, u, true);
}

void z_nrf_rtc_timer_abort(int32_t chan)
{
	/* The driver clears the event and its routing: a latched event is gone. */
	fw.ch[chan].armed = false;
	fw.ch[chan].pending = false;
}

uint64_t fake_rtc_wait_edge(void)
{
	/* Busy-wait for the counter to step: the world moves to the next tick. */
	fw.wait_edges++;
	fw.q = (fw.q / FW_TICK_Q + 1u) * FW_TICK_Q;
	return z_nrf_rtc_timer_read();
}

#else /* GRTC */

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
	return fw_lf();
}

uint32_t z_nrf_grtc_timer_compare_evt_address_get(int32_t chan)
{
	return 0x5000u + (uint32_t)chan;
}

int z_nrf_grtc_timer_set(int32_t chan, uint64_t target,
			 z_nrf_grtc_timer_compare_handler_t h, void *u)
{
	if (chan != lp.ch_anchor) {
		fw.hf_sets++;
	}
	fw_arm(chan, target, target, h, u, false);
	/* The write lands, then the caller is preempted before its re-read. */
	fw_preempt_anchor_write(chan);
	return 0;
}

void z_nrf_grtc_timer_abort(int32_t chan)
{
	/* An event already latched still reaches the handler (nrfx reads it). */
	fw.ch[chan].armed = false;
}

#endif

/* ---- HFXO, CONSTLAT, LFCLK ---- */

static void fw_hf_up(void)
{
	fw.hf_starting = false;
	fw.hf_on = true;
	fw.hf_on_since = fw.q;
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
		fw.hf_ready_at = fw.q + (uint64_t)fw.hf_startup * FW_Q;
	}
	return 0;
}

int fake_onoff_cancel_or_release(struct onoff_manager *mgr, struct onoff_client *cli)
{
	ARG_UNUSED(mgr);
	ARG_UNUSED(cli);
	if (fw.hf_on) {
		fw.hf_on_q += fw.q - fw.hf_on_since;
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

#if defined(NRF54L_ERRATA_20_PRESENT)
int nrfx_power_constlat_mode_request(void)
{
	return (++fw.constlat == 1) ? 0 : -EALREADY;
}

int nrfx_power_constlat_mode_free(void)
{
	return (--fw.constlat == 0) ? 0 : -EBUSY;
}
#endif

/* ---- The world ---- */

/*
 * Fire the earliest event due at or before `until` (q). False if none is. A
 * compare's hardware part (the anchor (D)PPI) acts at its tick; its handler
 * runs then, or at the end of an interrupt hold, lowest channel first.
 */
static bool fw_step(uint64_t until)
{
	uint64_t best = UINT64_MAX;
	int      which = -1;
	bool     service = false;

	for (int c = 0; c < FW_CHANNELS; c++) {
		if (fw.ch[c].armed) {
			uint64_t at = fw_tick_q(fw.ch[c].cc);

			if (at < best) {
				best = at;
				which = c;
			}
		}
	}
	for (int c = 0; c < FW_CHANNELS; c++) {
		if (fw.ch[c].pending) {
			uint64_t at = (fw.isr_hold_until > fw.q) ? fw.isr_hold_until : fw.q;

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
	if (best > fw.q) {
		fw.q = best;
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
		fw.ch[which].pending = !fw.ch[which].hw_only;
		if (which == lp.ch_anchor &&
		    (fake_hw.conn_on & (1u << (lp.conn_anchor & 31u))) != 0u) {
			if (fw.break_dppi) {
				fw.break_dppi = false;
			} else {
				fw_task(NRF_TIMER_TASK_START);
				fake_hw.cc[LP_CC_ANCHOR] = fw_timer();
			}
		}
		if (!fw.ch[which].pending || fw.isr_hold_until > fw.q) {
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
	fw.ch[which].h(which, fw.ch[which].target, fw.ch[which].u);
	return true;
}

static void fw_run_to_q(uint64_t q)
{
	while (fw_step(q)) {
	}
	if (fw.q < q) {
		fw.q = q;
	}
}

void lpt_run_to(uint64_t world)
{
	fw_run_to_q(world * FW_Q);
}

void lpt_run(uint64_t us)
{
	fw_run_to_q(fw.q + us * FW_Q);
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

static void lpt_boot(nrf_clock_lfclk_t lf, uint64_t lf_off)
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
	fw.lf_off = lf_off;
	fw.t_last_task = -1;
	fw.hf_startup = LPT_HFXO_STARTUP;

	/* radiant_radio_init()'s order: HFXO, CONSTLAT, TIMER, low power. */
	fw.q = (uint64_t)(LPT_W0 - 100u) * FW_Q;
	fw.hf_sync = true;
	(void)lp_hfxo_boot();
	fw.hf_sync = false;
	lp_constlat(true);
	fw.q = (uint64_t)LPT_W0 * FW_Q;
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

void lpt_reset(void) { lpt_boot(NRF_CLOCK_LFCLK_XTAL, LPT_LF_OFFSET); }
void lpt_reset_lf_rc(void) { lpt_boot(NRF_CLOCK_LFCLK_RC, LPT_LF_OFFSET); }
void lpt_reset_lf_offset(uint64_t lf_off) { lpt_boot(NRF_CLOCK_LFCLK_XTAL, lf_off); }
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

uint64_t lpt_world(void) { return fw.q / FW_Q; }
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
	*world = (fw.t_t0q + (uint64_t)(uint32_t)(fake_hw.cc[cc] - fw.t_base) * FW_Q) / FW_Q;
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
	return (fw.hf_on_q + (fw.hf_on ? fw.q - fw.hf_on_since : 0u)) / FW_Q;
}

bool lpt_anchor_world(uint64_t *world)
{
	if (lp.ch_anchor < 0 || !fw.ch[lp.ch_anchor].armed) {
		return false;
	}
	*world = fw_tick_q(fw.ch[lp.ch_anchor].cc) / FW_Q;
	return true;
}

void lpt_run_to_anchor_minus(uint32_t us)
{
	uint64_t at;

	if (lpt_anchor_world(&at) && at > us) {
		lpt_run_to(at - us);
	}
}

void lpt_run_past_anchor(void)
{
	if (lp.ch_anchor >= 0 && fw.ch[lp.ch_anchor].armed) {
		fw_run_to_q(fw_tick_q(fw.ch[lp.ch_anchor].cc) + FW_Q);
	}
}

uint64_t lpt_lf_ticks(void) { return fw_lf(); }

void lpt_run_to_next_lf_tick(uint32_t plus_us)
{
	uint64_t next = (fw.q / FW_TICK_Q + 1u) * FW_TICK_Q;

	fw_run_to_q(next + (uint64_t)plus_us * FW_Q);
}

void lpt_hw(struct lpt_hw *o)
{
	o->timer_count = fw_timer();
	o->shutdowns = fw.t_shutdowns;
	o->stop_then_shutdown = fw.t_stop_then_shutdown;
	o->cc5 = fake_hw.cc[LP_CC_ANCHOR];
	o->cc5_poisoned = (fake_hw.cc[LP_CC_ANCHOR] == FW_CC5_POISON);
	o->timer_t0_world_q = fw.t_t0q;
	o->exact_refusals = fw.exact_refusals;
	o->span_refusals = fw.span_refusals;
	o->min_exact_ahead = fw.min_exact_ahead;
	o->wait_edges = fw.wait_edges;
	o->hf_sets = fw.hf_sets;
}

void lpt_set_hfxo_startup(uint32_t us) { fw.hf_startup = us; }
void lpt_break_next_dppi(void) { fw.break_dppi = true; }
void lpt_delay_next_anchor_set(uint32_t us) { fw.delay_anchor_set = us; }
void lpt_set_disable_hangs(bool on) { fake_hw.disable_hangs = on; }
void lpt_hold_isr_until(uint64_t world) { fw.isr_hold_until = world * FW_Q; }
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
