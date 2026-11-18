/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pass B1 spike: can the nRF backend release the HFXO between slots and keep an
 * exact timebase by anchoring its 1 MHz TIMER to the LF counter?
 *
 *   S1  register retention across HFXO release + 250 ms sleep (RADIO, TIMER,
 *       (D)PPI endpoint registers, DPPIC CHEN, PPIB bridges)
 *   S2  LF compare channels available; GPPI LF compare -> TIMER START + fork
 *       CAPTURE5 (the Track B anchor), and the reverse TIMER COMPARE -> LF
 *       capture used here to measure it
 *   S3  anchor error over 10k cycles, wake anchor (TIMER started from the LF
 *       compare) and release anchor (fork capture of a running TIMER)
 *   S4  HFXO start-up distribution, CONSTLAT on and off
 *   S5  TIMER (HFXO) vs LF counter rate, minutes
 *
 * nRF54L15: LF = GRTC (SYSCOUNTER, 1 MHz), TIMER20, D2PPI (DPPIC + PPIB).
 * nRF52840: LF = RTC1 user channels (32768 Hz), TIMER4, PPI with FORK.
 * Throwaway: does not link radiant. Prints parseable "S<n> ..." lines.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/onoff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>

#include <hal/nrf_timer.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_clock.h>
#include <hal/nrf_power.h>
#include <helpers/nrfx_gppi.h>

#if defined(CONFIG_NRF_GRTC_TIMER)
#include <zephyr/drivers/timer/nrf_grtc_timer.h>
#define LF_GRTC 1
#define LF_PER_S 1000000ULL
#else
#include <zephyr/drivers/timer/nrf_rtc_timer.h>
#define LF_GRTC 0
#define LF_PER_S 32768ULL
#endif

#if defined(NRF_DPPIC20)
#define SOC_54L 1
#else
#define SOC_54L 0
#endif

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/uart.h>
#define CDC_CONSOLE 1
#else
#define CDC_CONSOLE 0
#endif

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#define BT_BUILD 1
#else
#define BT_BUILD 0
#endif

#define TMR ((NRF_TIMER_Type *)DT_REG_ADDR(DT_CHOSEN(spike_timer)))

/* Backend CC map (radiant_radio_nrf.c) + the Track B anchor capture. */
#define CC_SYNC    0u
#define CC_START   1u
#define CC_CLOSE   2u
#define CC_NOW     3u
#define CC_TXSTART 4u
#define CC_ANCHOR  5u

#define SENT 0xDEADBEEFu
#define BAD  INT16_MIN

/* ------------------------------------------------------------------ output */

#if CDC_CONSOLE
static char obuf[40 * 1024];
static size_t olen;
static bool olive;
#else
static const bool olive = true;
#endif

static void out(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (olive) {
		vprintk(fmt, ap);
	}
#if CDC_CONSOLE
	else if (olen + 1 < sizeof(obuf)) {
		int n = vsnprintk(obuf + olen, sizeof(obuf) - olen, fmt, ap);

		if (n > 0) {
			olen += MIN((size_t)n, sizeof(obuf) - 1 - olen);
		}
	}
#endif
	va_end(ap);
}

static uint32_t rng = 0x9E3779B9u;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* ------------------------------------------------------------------- stats */

#define NMAX 10000
static int16_t A[NMAX], B[NMAX], C[NMAX];
static int32_t scratch[NMAX];

static int cmp_i32(const void *a, const void *b)
{
	int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;

	return (x > y) - (x < y);
}

/* Exact distribution: summary percentiles and every distinct value:count. */
static void report(const char *tag, const int16_t *v, int n, int start, int step)
{
	int m = 0, bad = 0;
	int64_t sum = 0;

	for (int i = start; i < n; i += step) {
		if (v[i] == BAD) {
			bad++;
		} else {
			scratch[m++] = v[i];
			sum += v[i];
		}
	}
	if (m == 0) {
		out("%s n=0 bad=%d\n", tag, bad);
		return;
	}
	qsort(scratch, m, sizeof(scratch[0]), cmp_i32);
#define PCT(pm) scratch[(int)(((int64_t)(m - 1) * (pm)) / 1000)]
	out("%s n=%d bad=%d min=%d p0.1=%d p1=%d p50=%d p99=%d p99.9=%d max=%d "
	    "mean_milli=%lld\n",
	    tag, m, bad, scratch[0], PCT(1), PCT(10), PCT(500), PCT(990),
	    PCT(999), scratch[m - 1], (long long)(sum * 1000 / m));
#undef PCT
	out("%s hist", tag);
	int col = 0;

	for (int i = 0; i < m;) {
		int j = i;

		while (j < m && scratch[j] == scratch[i]) {
			j++;
		}
		out(" %d:%d", scratch[i], j - i);
		if (++col == 14) {
			out("\n%s hist", tag);
			col = 0;
		}
		i = j;
	}
	out("\n");
}

static int16_t clamp16(int32_t x)
{
	if (x > INT16_MAX) {
		return INT16_MAX;
	}
	if (x <= INT16_MIN) {
		return INT16_MIN + 1;
	}
	return (int16_t)x;
}

/* -------------------------------------------------------------- LF counter */

static volatile uint32_t lf_fired;

static void lf_handler(int32_t id, uint64_t expire_time, void *user_data)
{
	ARG_UNUSED(id);
	ARG_UNUSED(expire_time);
	ARG_UNUSED(user_data);
	lf_fired++;
}

#if LF_GRTC
static inline uint64_t lf_now(void) { return z_nrf_grtc_timer_read(); }
static inline int32_t lf_alloc(void) { return z_nrf_grtc_timer_chan_alloc(); }
static inline int lf_set(int32_t ch, uint64_t t)
{
	return z_nrf_grtc_timer_set(ch, t, lf_handler, NULL);
}
static inline uint32_t lf_evt(int32_t ch)
{
	return z_nrf_grtc_timer_compare_evt_address_get(ch);
}
/* LF ticks -> microseconds (GRTC is 1 MHz). */
static inline uint64_t lf_us(uint64_t t) { return t; }
#else
static inline uint64_t lf_now(void) { return z_nrf_rtc_timer_read(); }
static inline int32_t lf_alloc(void) { return z_nrf_rtc_timer_chan_alloc(); }
static inline int lf_set(int32_t ch, uint64_t t)
{
	return z_nrf_rtc_timer_exact_set(ch, t, lf_handler, NULL);
}
static inline uint32_t lf_evt(int32_t ch)
{
	return z_nrf_rtc_timer_compare_evt_address_get(ch);
}
static inline uint64_t lf_us(uint64_t t) { return (t * 15625ULL) >> 9; }
#endif

static int32_t lf_ch[8];
static int lf_n;

/* ------------------------------------------------------------------- TIMER */

static void tmr_init(void)
{
	uint32_t base = NRF_TIMER_BASE_FREQUENCY_GET(TMR);
	uint32_t presc = 0;

	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_STOP);
	nrf_timer_mode_set(TMR, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(TMR, NRF_TIMER_BIT_WIDTH_32);
	while ((base >> presc) > 1000000u) {
		presc++;
	}
	nrf_timer_prescaler_set(TMR, presc);
	nrf_timer_shorts_disable(TMR, ~0u);
	nrf_timer_int_disable(TMR, ~0u);
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_CLEAR);
	for (int i = 0; i < 6; i++) {
		nrf_timer_cc_set(TMR, (nrf_timer_cc_channel_t)i, 0xF0000000u + i);
	}
}

static uint32_t tmr_cap(uint8_t cc)
{
	nrf_timer_task_trigger(TMR, nrf_timer_capture_task_get(cc));
	return nrf_timer_cc_get(TMR, (nrf_timer_cc_channel_t)cc);
}

static inline uint32_t tmr_cc(uint8_t cc)
{
	return nrf_timer_cc_get(TMR, (nrf_timer_cc_channel_t)cc);
}

static inline void tmr_cc_set(uint8_t cc, uint32_t v)
{
	nrf_timer_cc_set(TMR, (nrf_timer_cc_channel_t)cc, v);
}

/* ------------------------------------------------------------- timestamps */

static uint32_t stamp_lf(void) { return (uint32_t)lf_now(); }
#if !LF_GRTC
static uint32_t stamp_tmr(void) { return tmr_cap(CC_TXSTART); }
#endif
static uint32_t (*stamp)(void) = stamp_lf;

/* -------------------------------------------------------------- HFXO, POWER */

static struct onoff_manager *hf_mgr;
static struct onoff_client hf_cli;
static K_SEM_DEFINE(hf_sem, 0, 1);
static volatile uint32_t hf_cb_t;
static volatile int hf_cb_res;
static bool hf_held;

static void hf_cb(struct onoff_manager *mgr, struct onoff_client *cli,
		  uint32_t state, int res)
{
	hf_cb_t = stamp();
	hf_cb_res = res;
	k_sem_give(&hf_sem);
}

static int hf_request(void)
{
	k_sem_reset(&hf_sem);
	sys_notify_init_callback(&hf_cli.notify, hf_cb);
	int r = onoff_request(hf_mgr, &hf_cli);

	if (r >= 0) {
		hf_held = true;
	}
	return r;
}

static int hf_wait(k_timeout_t to)
{
	return k_sem_take(&hf_sem, to);
}

static void hf_release(void)
{
	if (hf_held) {
		(void)onoff_cancel_or_release(hf_mgr, &hf_cli);
		hf_held = false;
	}
}

static uint32_t hf_stat_raw(void)
{
#if SOC_54L
	return NRF_CLOCK->XO.STAT;
#else
	return NRF_CLOCK->HFCLKSTAT;
#endif
}

static bool hfxo_running(void)
{
#if SOC_54L
	return ((NRF_CLOCK->XO.STAT & CLOCK_XO_STAT_STATE_Msk) >>
		CLOCK_XO_STAT_STATE_Pos) == CLOCK_XO_STAT_STATE_Running;
#else
	uint32_t s = NRF_CLOCK->HFCLKSTAT;

	return (s & CLOCK_HFCLKSTAT_STATE_Msk) && (s & CLOCK_HFCLKSTAT_SRC_Msk);
#endif
}

static uint32_t constlat_stat(void)
{
#if SOC_54L
	return NRF_POWER->CONSTLATSTAT;
#else
	return 0xFFFFFFFFu; /* nRF52840 has no status register */
#endif
}

static void constlat(bool on)
{
	nrf_power_task_trigger(NRF_POWER, on ? NRF_POWER_TASK_CONSTLAT
					     : NRF_POWER_TASK_LOWPWR);
}

static bool hf_wait_off(int max_us)
{
	for (int t = 0; t < max_us; t += 20) {
		if (!hfxo_running()) {
			return true;
		}
		k_busy_wait(20);
	}
	return !hfxo_running();
}

/* -------------------------------------------------------- GPPI connections */

enum { C_SYNC, C_START, C_START_TX, C_CLOSE, C_RSSI,
       C_ANCHOR, C_MEAS, C_CLK1, C_CLK2, C_N };

struct conn {
	const char *name;
	uint32_t eep, tep, tep2;
	nrfx_gppi_handle_t h;
	int err, err2;
	bool ok, contended;
};
static struct conn conns[C_N];

#if LF_GRTC
static int32_t ch_cmp = -1, ch_cap = -1, ch_xs = -1, ch_xt = -1;
#else
static int32_t ch_a = -1, ch_b = -1;
#endif

static uint32_t ep_reg(uint32_t ep)
{
#if defined(DPPI_PRESENT)
	return ep + NRF_SUBSCRIBE_PUBLISH_OFFSET(ep);
#else
	return 0;
#endif
}

static void conn_alloc(int i, const char *name, uint32_t eep, uint32_t tep,
		       uint32_t tep2)
{
	struct conn *c = &conns[i];

	c->name = name;
	c->eep = eep;
	c->tep = tep;
	c->tep2 = tep2;
#if defined(DPPI_PRESENT)
	if (*(volatile uint32_t *)ep_reg(eep) || *(volatile uint32_t *)ep_reg(tep)) {
		c->contended = true;
		c->err = -EBUSY;
		return;
	}
#endif
	c->err = nrfx_gppi_conn_alloc(eep, tep, &c->h);
	if (c->err == 0 && tep2) {
		c->err2 = nrfx_gppi_ep_attach(tep2, c->h);
	}
	c->ok = (c->err == 0 && c->err2 == 0);
}

static void conn_report(int i)
{
	struct conn *c = &conns[i];

	if (!c->name) {
		return;
	}
	uint32_t de = nrfx_gppi_domain_id_get(c->eep);
	uint32_t dt = nrfx_gppi_domain_id_get(c->tep);

	out("S2 CONN %-8s err=%d err2=%d contended=%d h=0x%08x dom_evt=%u "
	    "dom_task=%u ch_evt=%d ch_task=%d eep=0x%08x tep=0x%08x tep2=0x%08x\n",
	    c->name, c->err, c->err2, c->contended, (unsigned)c->h,
	    (unsigned)de, (unsigned)dt,
	    c->ok ? nrfx_gppi_domain_channel_get(c->h, de) : -1,
	    c->ok ? nrfx_gppi_domain_channel_get(c->h, dt) : -1,
	    (unsigned)c->eep, (unsigned)c->tep, (unsigned)c->tep2);
}

static void conn_enable(int i, bool en)
{
	if (conns[i].ok) {
		if (en) {
			nrfx_gppi_conn_enable(conns[i].h);
		} else {
			nrfx_gppi_conn_disable(conns[i].h);
		}
	}
}

/* How many more connections fit between two domains (allocated, then freed). */
static int gppi_headroom(uint32_t dom_p, uint32_t dom_c)
{
	nrfx_gppi_handle_t hs[48];
	int n = 0;

	while (n < (int)ARRAY_SIZE(hs) &&
	       nrfx_gppi_domain_conn_alloc(dom_p, dom_c, &hs[n]) == 0) {
		n++;
	}
	for (int i = 0; i < n; i++) {
		nrfx_gppi_domain_conn_free(hs[i]);
	}
	return n;
}

#if SOC_54L
static NRF_PPIB_Type *const ppibs[] = {
	NRF_PPIB00, NRF_PPIB01, NRF_PPIB10, NRF_PPIB11,
	NRF_PPIB20, NRF_PPIB21, NRF_PPIB22, NRF_PPIB30,
};
static const char *const ppib_names[] = {
	"PPIB00", "PPIB01", "PPIB10", "PPIB11",
	"PPIB20", "PPIB21", "PPIB22", "PPIB30",
};
static NRF_DPPIC_Type *const dppics[] = {
	NRF_DPPIC00, NRF_DPPIC10, NRF_DPPIC20, NRF_DPPIC30,
};
static const char *const dppic_names[] = { "DPPIC00", "DPPIC10", "DPPIC20", "DPPIC30" };
static uint32_t ppib_snap0[8][64];

static void ppib_snapshot(uint32_t s[8][64])
{
	for (int p = 0; p < 8; p++) {
		for (int k = 0; k < 32; k++) {
			s[p][k] = ppibs[p]->SUBSCRIBE_SEND[k];
			s[p][32 + k] = ppibs[p]->PUBLISH_RECEIVE[k];
		}
	}
}

static void ppib_diff(const char *tag, uint32_t a[8][64])
{
	static uint32_t b[8][64];

	ppib_snapshot(b);
	for (int p = 0; p < 8; p++) {
		for (int k = 0; k < 64; k++) {
			if (a[p][k] != b[p][k]) {
				out("%s %s.%s[%d] 0x%08x -> 0x%08x\n", tag, ppib_names[p],
				    k < 32 ? "SUBSCRIBE_SEND" : "PUBLISH_RECEIVE",
				    k & 31, (unsigned)a[p][k], (unsigned)b[p][k]);
			}
		}
	}
}
#endif

/* ---------------------------------------------------------------------- S2 */

static void s2(void)
{
	out("S2 begin lf=%s\n", LF_GRTC ? "GRTC" : "RTC1");
#if LF_GRTC
	out("S2 NRFX_GRTC_CONFIG_NUM_OF_CC_CHANNELS=%d ALLOWED_MASK=0x%08x "
	    "GRTC.MODE=0x%08x CLKCFG=0x%08x WAKETIME=%u TIMEOUT=%u\n",
	    NRFX_GRTC_CONFIG_NUM_OF_CC_CHANNELS,
	    (unsigned)NRFX_GRTC_CONFIG_ALLOWED_CC_CHANNELS_MASK,
	    (unsigned)NRF_GRTC->MODE, (unsigned)NRF_GRTC->CLKCFG,
	    (unsigned)NRF_GRTC->WAKETIME, (unsigned)NRF_GRTC->TIMEOUT);
#else
	out("S2 CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT=%d\n",
	    CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT);
#endif
	while (lf_n < (int)ARRAY_SIZE(lf_ch)) {
		int32_t c = lf_alloc();

		if (c < 0) {
			out("S2 lf_alloc #%d -> %d (stop)\n", lf_n + 1, (int)c);
			break;
		}
		lf_ch[lf_n++] = c;
		out("S2 lf_alloc #%d -> channel %d\n", lf_n, (int)c);
	}
	out("S2 lf_channels_available=%d\n", lf_n);

	/* The backend's five connections (radiant_radio_nrf.c init). */
	uint32_t radio_addr = nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	uint32_t radio_rxrdy = nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_RXREADY);

	conn_alloc(C_SYNC, "sync", radio_addr,
		   nrf_timer_task_address_get(TMR, nrf_timer_capture_task_get(CC_SYNC)), 0);
	conn_alloc(C_START, "start",
		   nrf_timer_event_address_get(TMR, nrf_timer_compare_event_get(CC_START)),
		   nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_RXEN), 0);
	conn_alloc(C_START_TX, "start_tx",
		   nrf_timer_event_address_get(TMR, nrf_timer_compare_event_get(CC_TXSTART)),
		   nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_TXEN), 0);
	conn_alloc(C_CLOSE, "close",
		   nrf_timer_event_address_get(TMR, nrf_timer_compare_event_get(CC_CLOSE)),
		   nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_DISABLE), 0);
	conn_alloc(C_RSSI, "rssi", radio_rxrdy,
		   nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_RSSISTART), 0);

#if SOC_54L
	ppib_snapshot(ppib_snap0);
#endif
	uint32_t t_start = nrf_timer_task_address_get(TMR, NRF_TIMER_TASK_START);
	uint32_t t_cap5 = nrf_timer_task_address_get(TMR, nrf_timer_capture_task_get(CC_ANCHOR));
	uint32_t t_cap3 = nrf_timer_task_address_get(TMR, nrf_timer_capture_task_get(CC_NOW));
	uint32_t e_cmp3 = nrf_timer_event_address_get(TMR, nrf_timer_compare_event_get(CC_NOW));

#if LF_GRTC
	if (lf_n < 2) {
		out("S2 FAIL fewer than two GRTC channels\n");
		return;
	}
	ch_cmp = lf_ch[0];
	ch_cap = lf_ch[1];
	conn_alloc(C_ANCHOR, "anchor", lf_evt(ch_cmp), t_start, t_cap5);
	conn_alloc(C_MEAS, "meas", e_cmp3, z_nrf_grtc_timer_capture_task_address_get(ch_cap), 0);
	if (lf_n >= 3) {
		ch_xs = lf_ch[2];
		conn_alloc(C_CLK1, "xostart",
			   nrf_clock_event_address_get(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED),
			   z_nrf_grtc_timer_capture_task_address_get(ch_xs), 0);
	}
	if (lf_n >= 4) {
		ch_xt = lf_ch[3];
		conn_alloc(C_CLK2, "xotuned",
			   nrf_clock_event_address_get(NRF_CLOCK, NRF_CLOCK_EVENT_XOTUNED),
			   z_nrf_grtc_timer_capture_task_address_get(ch_xt), 0);
	}
	ARG_UNUSED(t_cap3);
#else
	if (lf_n < 2) {
		out("S2 FAIL fewer than two RTC1 user channels\n");
		return;
	}
	ch_a = lf_ch[0];
	ch_b = lf_ch[1];
	conn_alloc(C_ANCHOR, "anchor", lf_evt(ch_a), t_start, t_cap5);
	conn_alloc(C_MEAS, "meas", lf_evt(ch_b), t_cap3, 0);
	conn_alloc(C_CLK1, "hfstart",
		   nrf_clock_event_address_get(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED),
		   nrf_timer_task_address_get(TMR, nrf_timer_capture_task_get(CC_CLOSE)), 0);
	ARG_UNUSED(e_cmp3);
#endif
	for (int i = 0; i < C_N; i++) {
		conn_report(i);
	}
#if SOC_54L
	ppib_diff("S2 PPIB", ppib_snap0);
#endif
	/* As the backend: sync always on, the window edges per operation. */
	conn_enable(C_SYNC, true);
	conn_enable(C_RSSI, true);
	conn_enable(C_ANCHOR, true);
	conn_enable(C_MEAS, true);
	conn_enable(C_CLK1, true);
	conn_enable(C_CLK2, true);

	uint32_t d_lf = nrfx_gppi_domain_id_get(conns[C_ANCHOR].eep);
	uint32_t d_tm = nrfx_gppi_domain_id_get(t_start);
	uint32_t d_ra = nrfx_gppi_domain_id_get(radio_addr);

	out("S2 headroom lf->timer=%d timer->lf=%d timer->radio=%d radio->timer=%d "
	    "(domains lf=%u timer=%u radio=%u)\n",
	    gppi_headroom(d_lf, d_tm), gppi_headroom(d_tm, d_lf),
	    gppi_headroom(d_tm, d_ra), gppi_headroom(d_ra, d_tm),
	    (unsigned)d_lf, (unsigned)d_tm, (unsigned)d_ra);
	out("S2 anchor_ok=%d meas_ok=%d\n", conns[C_ANCHOR].ok, conns[C_MEAS].ok);
}

/* ---------------------------------------------------------------------- S1 */

struct reg {
	const char *name;
	int16_t idx;
	uint32_t addr;
};
static struct reg regs[256];
static int nregs;
static uint32_t snapA[256], snapB[256];

static void reg_add(const char *n, int idx, volatile const void *a)
{
	if (nregs < (int)ARRAY_SIZE(regs)) {
		regs[nregs].name = n;
		regs[nregs].idx = (int16_t)idx;
		regs[nregs].addr = (uint32_t)a;
		nregs++;
	}
}

static void regs_build(void)
{
	nregs = 0;
#define RA(f) reg_add("RADIO." #f, -1, &NRF_RADIO->f)
	RA(MODE); RA(FREQUENCY); RA(PCNF0); RA(PCNF1); RA(BASE0); RA(PREFIX0);
	RA(TXADDRESS); RA(RXADDRESSES); RA(CRCCNF); RA(CRCPOLY); RA(CRCINIT);
	RA(TXPOWER); RA(SHORTS); RA(PACKETPTR); RA(TIFS);
#if SOC_54L
	RA(TIMING); RA(INTENSET00);
#else
	RA(MODECNF0); RA(INTENSET); RA(POWER);
#endif
#undef RA
#define TA(f) reg_add("TIMER." #f, -1, &TMR->f)
	TA(MODE); TA(BITMODE); TA(PRESCALER); TA(SHORTS); TA(INTENSET);
#undef TA
	for (int i = 0; i < 6; i++) {
		reg_add("TIMER.CC", i, &TMR->CC[i]);
	}
	for (int i = 0; i < C_N; i++) {
		struct conn *c = &conns[i];

		if (!c->ok) {
			continue;
		}
#if defined(DPPI_PRESENT)
		reg_add(c->name, -2, (void *)ep_reg(c->eep));
		reg_add(c->name, -3, (void *)ep_reg(c->tep));
		if (c->tep2) {
			reg_add(c->name, -4, (void *)ep_reg(c->tep2));
		}
#else
		reg_add("PPI.CH.EEP", c->h, &NRF_PPI->CH[c->h].EEP);
		reg_add("PPI.CH.TEP", c->h, &NRF_PPI->CH[c->h].TEP);
		reg_add("PPI.FORK.TEP", c->h, &NRF_PPI->FORK[c->h].TEP);
#endif
	}
#if SOC_54L
	for (int d = 0; d < 4; d++) {
		reg_add(dppic_names[d], -5, &dppics[d]->CHEN);
	}
	for (int p = 0; p < 8; p++) {
		for (int k = 0; k < 32; k++) {
			if (ppibs[p]->SUBSCRIBE_SEND[k]) {
				reg_add(ppib_names[p], k, &ppibs[p]->SUBSCRIBE_SEND[k]);
			}
			if (ppibs[p]->PUBLISH_RECEIVE[k]) {
				reg_add(ppib_names[p], 32 + k, &ppibs[p]->PUBLISH_RECEIVE[k]);
			}
		}
	}
#else
	reg_add("PPI.CHEN", -1, &NRF_PPI->CHEN);
#endif
}

static void snapshot(uint32_t *s)
{
	for (int i = 0; i < nregs; i++) {
		s[i] = *(volatile uint32_t *)regs[i].addr;
	}
}

static void reg_name(const struct reg *r, char *buf, size_t len)
{
	switch (r->idx) {
	case -1: snprintk(buf, len, "%s", r->name); break;
	case -2: snprintk(buf, len, "PUBLISH(%s.evt)", r->name); break;
	case -3: snprintk(buf, len, "SUBSCRIBE(%s.task)", r->name); break;
	case -4: snprintk(buf, len, "SUBSCRIBE(%s.fork)", r->name); break;
	case -5: snprintk(buf, len, "%s.CHEN", r->name); break;
	default:
		if (r->name[0] == 'P' && r->name[1] == 'P' && r->name[2] == 'I' &&
		    r->name[3] == 'B') {
			snprintk(buf, len, "%s.%s[%d]", r->name,
				 r->idx < 32 ? "SUBSCRIBE_SEND" : "PUBLISH_RECEIVE",
				 r->idx & 31);
		} else {
			snprintk(buf, len, "%s[%d]", r->name, r->idx);
		}
	}
}

static volatile uint32_t mid_stat, mid_cl, mid_run;

static void mid_fn(struct k_timer *t)
{
	mid_run = hfxo_running();
	mid_stat = hf_stat_raw();
	mid_cl = constlat_stat();
}
static K_TIMER_DEFINE(mid_timer, mid_fn, NULL);

static uint8_t pkt[16];

static void s1_config(void)
{
#if defined(RADIO_MODECNF0_RU_Msk) || defined(RADIO_TIMING_RU_Msk)
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);
#endif
	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
	nrf_radio_frequency_set(NRF_RADIO, 2457);
	NRF_RADIO->PCNF0 = 0x00000008u;
	NRF_RADIO->PCNF1 = 0x00040309u;
	NRF_RADIO->BASE0 = 0xA5A5C3C3u;
	NRF_RADIO->PREFIX0 = 0x000000C5u;
	NRF_RADIO->TXADDRESS = 0;
	NRF_RADIO->RXADDRESSES = 1;
	NRF_RADIO->CRCCNF = 2;
	NRF_RADIO->CRCPOLY = 0x11021u;
	NRF_RADIO->CRCINIT = 0xFFFFu;
	NRF_RADIO->TIFS = 150;
	nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK |
					NRF_RADIO_SHORT_DISABLED_TXEN_MASK);
	nrf_radio_packetptr_set(NRF_RADIO, pkt);
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_ADDRESS_MASK |
					NRF_RADIO_INT_DISABLED_MASK);

	tmr_init();
	nrf_timer_shorts_enable(TMR, NRF_TIMER_SHORT_COMPARE4_STOP_MASK);
	nrf_timer_int_enable(TMR, NRF_TIMER_INT_COMPARE4_MASK);
	for (int i = 0; i < 6; i++) {
		tmr_cc_set(i, 0x40000000u + 0x111u * i);
	}
}

static void s1(void)
{
	char nm[64];

	s1_config();
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_START);
	k_busy_wait(2000);
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_STOP);
	uint32_t cnt0 = tmr_cap(CC_NOW);

	regs_build();
	snapshot(snapA);
	out("S1 begin regs=%d timer_count_at_stop=%u hf_stat=0x%08x constlatstat=%u\n",
	    nregs, (unsigned)cnt0, (unsigned)hf_stat_raw(), (unsigned)constlat_stat());
	for (int i = 0; i < nregs; i++) {
		reg_name(&regs[i], nm, sizeof(nm));
		out("S1 REG %s @0x%08x = 0x%08x\n", nm, (unsigned)regs[i].addr,
		    (unsigned)snapA[i]);
	}

	const int nvar = SOC_54L ? 2 : 3;

	for (int v = 0; v < nvar; v++) {
		bool cl = (v == 0);
		bool shut = (v == 2);

		constlat(cl);
#if defined(TIMER_TASKS_SHUTDOWN_TASKS_SHUTDOWN_Msk)
		if (shut) {
			nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_SHUTDOWN);
		}
#endif
		hf_release();
		bool off = hf_wait_off(20000);

		mid_run = 0xFF;
		k_timer_start(&mid_timer, K_MSEC(125), K_NO_WAIT);
		k_sleep(K_MSEC(250));
		hf_request();
		int w = hf_wait(K_MSEC(100));

		snapshot(snapB);
		uint32_t cnt1 = tmr_cap(CC_NOW);

		tmr_cc_set(CC_NOW, cnt0);

		int nd = 0;

		for (int i = 0; i < nregs; i++) {
			if (snapA[i] != snapB[i]) {
				reg_name(&regs[i], nm, sizeof(nm));
				out("S1 var=%d DIFF %s before=0x%08x after=0x%08x\n", v, nm,
				    (unsigned)snapA[i], (unsigned)snapB[i]);
				nd++;
			}
		}
		out("S1 var=%d constlat=%d timer_shutdown=%d hfxo_off_seen=%d "
		    "mid_hfxo_running=%u mid_hf_stat=0x%08x mid_constlatstat=%u "
		    "rerequest=%d count_after=%u (before %u) same=%d diff=%d\n",
		    v, cl, shut, off, (unsigned)mid_run, (unsigned)mid_stat,
		    (unsigned)mid_cl, w, (unsigned)cnt1, (unsigned)cnt0,
		    nregs - nd, nd);
	}
	constlat(true);
	/* Leave the RADIO quiet and the TIMER in the backend's configuration. */
	nrf_radio_shorts_set(NRF_RADIO, 0);
	nrf_radio_int_disable(NRF_RADIO, ~0u);
	tmr_init();
	out("S1 end\n");
}

/* ---------------------------------------------------------------------- S4 */

#define N4 1000

static void s4(void)
{
	out("S4 begin n=%d per mode\n", N4);
#if !LF_GRTC
	/* Stopwatch = the TIMER, on HFINT while the HFXO is off. Calibrate HFINT
	 * against RTC1 first (1 s, TIMER captures on two RTC compares). */
	stamp = stamp_tmr;
	tmr_init();
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_START);
	hf_release();
	hf_wait_off(20000);
	for (int rep = 0; rep < 3; rep++) {
		uint64_t ta = lf_now() + 16;

		tmr_cc_set(CC_ANCHOR, SENT);
		tmr_cc_set(CC_NOW, SENT);
		lf_set(ch_a, ta);
		lf_set(ch_b, ta + 32768);
		k_sleep(K_MSEC(1100));
		uint32_t d = tmr_cc(CC_NOW) - tmr_cc(CC_ANCHOR);

		out("S4 hfint_cal rep=%d hfxo_running=%d timer_counts_per_rtc_second=%u "
		    "ppm=%d\n", rep, hfxo_running(), (unsigned)d, (int)((int32_t)d - 1000000));
	}
#endif
	for (int mode = 0; mode < 2; mode++) {
		bool cl = (mode == 0);
		int was_on = 0, tmo = 0;

		constlat(cl);
		for (int i = 0; i < N4; i++) {
			hf_release();
			hf_wait_off(20000);
			k_sleep(K_USEC(2000 + rnd() % 6000));
			if (hfxo_running()) {
				was_on++;
			}
#if LF_GRTC
			if (ch_xs >= 0) {
				z_nrf_grtc_timer_capture_prepare(ch_xs);
			}
			if (ch_xt >= 0) {
				z_nrf_grtc_timer_capture_prepare(ch_xt);
			}
#else
			tmr_cc_set(CC_CLOSE, SENT);
#endif
			uint32_t t0 = stamp();

			hf_request();
			int r = hf_wait(K_MSEC(50));

			if (r) {
				tmo++;
			}
			A[i] = r ? BAD : clamp16((int32_t)(hf_cb_t - t0));
#if LF_GRTC
			uint64_t g;

			B[i] = (ch_xs >= 0 && z_nrf_grtc_timer_capture_read(ch_xs, &g) == 0)
				       ? clamp16((int32_t)((uint32_t)g - t0)) : BAD;
			C[i] = (ch_xt >= 0 && z_nrf_grtc_timer_capture_read(ch_xt, &g) == 0)
				       ? clamp16((int32_t)((uint32_t)g - t0)) : BAD;
#else
			uint32_t c2 = tmr_cc(CC_CLOSE);

			B[i] = (c2 == SENT) ? BAD : clamp16((int32_t)(c2 - t0));
			C[i] = BAD;
#endif
		}
		out("S4 constlat=%d was_on_before_request=%d timeouts=%d constlatstat=%u\n",
		    cl, was_on, tmo, (unsigned)constlat_stat());
		char tag[40];

		snprintk(tag, sizeof(tag), "S4 cl=%d request->callback", cl);
		report(tag, A, N4, 0, 1);
		snprintk(tag, sizeof(tag), "S4 cl=%d request->XOSTARTED_hw", cl);
		report(tag, B, N4, 0, 1);
#if LF_GRTC
		snprintk(tag, sizeof(tag), "S4 cl=%d request->XOTUNED_hw", cl);
		report(tag, C, N4, 0, 1);
#endif
	}
	constlat(true);
	hf_request();
	hf_wait(K_MSEC(50));
	stamp = stamp_lf;
	tmr_init();
	out("S4 end\n");
}

/* ---------------------------------------------------------------------- S3 */

/* v1 (odd cycles): CPU asleep across both compare and GRTC capture. v2 (the
 * SPIKE_S3ONLY image): asleep across the compare, awake for the capture. */
#if defined(SPIKE_S3ONLY)
static const bool s3_v2 = true;
#else
static const bool s3_v2 = false;
#endif

static void wait_until_lf(uint64_t t, bool sleep)
{
	if (sleep) {
		uint64_t now = lf_now();

		if (t > now) {
			k_sleep(K_USEC((uint32_t)lf_us(t - now)));
		}
	}
	while (lf_now() < t) {
	}
}

#if LF_GRTC
#define X_W 1000u
#define X_R 500u
#define LEAD_SLEEP 3000u
#define LEAD_HELD  300u

/* One wake + one release anchor. Returns via A (wake err), B (release err), C (CC5 at wake). */
static void s3_cycle(int i, bool lp, bool cpu_sleep, int *late)
{
	uint64_t g;

	if (lp) {
		hf_release();
		constlat(false);
		k_sleep(K_USEC(4000 + rnd() % 8000));
	}
	/* Wake anchor: a cleared, stopped TIMER started by the GRTC compare. */
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_CLEAR);
	tmr_cc_set(CC_ANCHOR, SENT);
	tmr_cc_set(CC_NOW, X_W);
	z_nrf_grtc_timer_capture_prepare(ch_cap);
	uint64_t T = lf_now() + (lp ? LEAD_SLEEP : LEAD_HELD);

	lf_set(ch_cmp, T);
	if (lp) {
		constlat(true);
		hf_request();
		hf_wait(K_MSEC(20));
		if ((int32_t)(hf_cb_t - (uint32_t)T) >= 0) {
			(*late)++;
		}
	}
	if (s3_v2 && cpu_sleep) {
		/* v2: no IRQ at T, CPU asleep across the compare, awake for the
		 * GRTC capture (a capture while SYSCOUNTER sleeps reads stale). */
		bool key = z_nrf_grtc_timer_compare_int_lock(ch_cmp);

		wait_until_lf(T + 300, true);
		wait_until_lf(T + X_W + 200, false);
		z_nrf_grtc_timer_compare_int_unlock(ch_cmp, key);
	} else {
		wait_until_lf(T + X_W + 200, cpu_sleep);
	}
	int rc = -1;

	for (int k = 0; k < 100 && rc; k++) {
		rc = z_nrf_grtc_timer_capture_read(ch_cap, &g);
		if (rc) {
			k_busy_wait(10);
		}
	}
	A[i] = rc ? BAD : clamp16((int32_t)(g - T) - (int32_t)X_W);
	uint32_t c5 = tmr_cc(CC_ANCHOR);

	C[i] = (c5 == SENT) ? BAD : clamp16((int32_t)c5);

	/* Release anchor: fork capture of the running TIMER at GRTC T2. */
	tmr_cc_set(CC_ANCHOR, SENT);
	uint64_t T2 = lf_now() + ((s3_v2 && cpu_sleep) ? 800 : 200);

	lf_set(ch_cmp, T2);
	if (s3_v2 && cpu_sleep) {
		bool key = z_nrf_grtc_timer_compare_int_lock(ch_cmp);

		wait_until_lf(T2 + 100, true); /* asleep across the release compare */
		z_nrf_grtc_timer_compare_int_unlock(ch_cmp, key);
	}
	wait_until_lf(T2 + 30, false);
	for (int k = 0; k < 100 && tmr_cc(CC_ANCHOR) == SENT; k++) {
		k_busy_wait(5);
	}
	uint32_t c5r = tmr_cc(CC_ANCHOR);

	z_nrf_grtc_timer_capture_prepare(ch_cap);
	tmr_cc_set(CC_NOW, c5r + X_R);
	wait_until_lf(T2 + X_R + 100, cpu_sleep && !s3_v2);
	rc = -1;
	for (int k = 0; k < 100 && rc; k++) {
		rc = z_nrf_grtc_timer_capture_read(ch_cap, &g);
		if (rc) {
			k_busy_wait(10);
		}
	}
	B[i] = (rc || c5r == SENT) ? BAD : clamp16((int32_t)(g - T2) - (int32_t)X_R);
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_STOP);
}
#else
#define NB 64u           /* RTC ticks between the two compares */
#define NB_E8 15625      /* NB ticks in 1/8 us: 64 * 15625 / 64 */
#define LEADT_SLEEP 100u /* ~3.05 ms */
#define LEADT_HELD  4u

static void s3_cycle(int i, bool lp, bool cpu_sleep, int *late)
{
	if (lp) {
		hf_release();
		constlat(false);
		k_sleep(K_USEC(4000 + rnd() % 8000));
	}
	/* Wake anchor: cleared TIMER started by RTC compare A, captured at B. */
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_CLEAR);
	tmr_cc_set(CC_ANCHOR, SENT);
	tmr_cc_set(CC_NOW, SENT);
	uint64_t Ta = lf_now() + (lp ? LEADT_SLEEP : LEADT_HELD);

	lf_set(ch_a, Ta);
	lf_set(ch_b, Ta + NB);
	if (lp) {
		constlat(true);
		hf_request();
		hf_wait(K_MSEC(20));
		if ((int32_t)(hf_cb_t - (uint32_t)Ta) >= 0) {
			(*late)++;
		}
	}
	wait_until_lf(Ta + NB + 2, cpu_sleep);
	uint32_t c3 = tmr_cc(CC_NOW), c5 = tmr_cc(CC_ANCHOR);

	A[i] = (c3 == SENT) ? BAD : clamp16((int32_t)(8 * c3) - NB_E8);
	C[i] = (c5 == SENT) ? BAD : clamp16((int32_t)c5);

	/* Release anchor: the TIMER is running; fork capture at A, capture at B. */
	tmr_cc_set(CC_ANCHOR, SENT);
	tmr_cc_set(CC_NOW, SENT);
	uint64_t Tc = lf_now() + 4;

	lf_set(ch_a, Tc);
	lf_set(ch_b, Tc + NB);
	wait_until_lf(Tc + NB + 2, cpu_sleep);
	c3 = tmr_cc(CC_NOW);
	c5 = tmr_cc(CC_ANCHOR);
	B[i] = (c3 == SENT || c5 == SENT) ? BAD : clamp16((int32_t)(8 * (c3 - c5)) - NB_E8);
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_STOP);
	if (lp) {
#if defined(TIMER_TASKS_SHUTDOWN_TASKS_SHUTDOWN_Msk)
		nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_SHUTDOWN); /* anomaly 78 */
#endif
	}
}
#endif

static void s3(const char *name, bool lp, int n)
{
	int late = 0;
	uint32_t f0 = lf_fired;
	int64_t t0 = k_uptime_get();

	out("S3 %s begin n=%d lp=%d units=%s\n", name, n, lp,
	    LF_GRTC ? "us" : "1/8us");
	tmr_init();
	if (!lp) {
		constlat(true);
		hf_request();
		hf_wait(K_MSEC(50));
	}
	for (int i = 0; i < n; i++) {
		s3_cycle(i, lp, (i & 1) != 0, &late);
	}
	if (lp) {
		hf_release();
		constlat(true);
		hf_request();
		hf_wait(K_MSEC(50));
	}
	out("S3 %s cycles=%d late_hfxo=%d lf_irqs=%u elapsed_ms=%lld\n", name, n,
	    late, (unsigned)(lf_fired - f0), (long long)(k_uptime_get() - t0));
	char tag[48];

	snprintk(tag, sizeof(tag), "S3 %s wake_err", name);
	report(tag, A, n, 0, 1);
	snprintk(tag, sizeof(tag), "S3 %s wake_err cpu_busy", name);
	report(tag, A, n, 0, 2);
	snprintk(tag, sizeof(tag), "S3 %s wake_err cpu_sleep", name);
	report(tag, A, n, 1, 2);
	snprintk(tag, sizeof(tag), "S3 %s release_err", name);
	report(tag, B, n, 0, 1);
	snprintk(tag, sizeof(tag), "S3 %s release_err cpu_busy", name);
	report(tag, B, n, 0, 2);
	snprintk(tag, sizeof(tag), "S3 %s release_err cpu_sleep", name);
	report(tag, B, n, 1, 2);
	snprintk(tag, sizeof(tag), "S3 %s cc5_at_wake", name);
	report(tag, C, n, 0, 1);
	tmr_init();
}

/* ---------------------------------------------------------------------- S5 */

static void s5(const char *name, bool busy, int nsec)
{
	int32_t ch = LF_GRTC ? lf_ch[0] : lf_ch[0];
	uint32_t c0 = 0;
	int64_t sx = 0, sy = 0, sxx = 0, sxy = 0;
	int n = 0;

	out("S5 %s begin seconds=%d\n", name, nsec);
	constlat(true);
	tmr_init();
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_START);
	uint64_t T0 = lf_now() + LF_PER_S / 50;

	for (int k = 0; k <= nsec; k++) {
		uint64_t Tk = T0 + (uint64_t)k * LF_PER_S;

		tmr_cc_set(CC_ANCHOR, SENT);
		lf_set(ch, Tk);
		wait_until_lf(Tk + LF_PER_S / 500, !busy);
		for (int j = 0; j < 100 && tmr_cc(CC_ANCHOR) == SENT; j++) {
			k_busy_wait(10);
		}
		uint32_t c = tmr_cc(CC_ANCHOR);

		if (c == SENT) {
			out("S5 %s k=%d MISSED\n", name, k);
			continue;
		}
		if (k == 0) {
			c0 = c;
		}
		int64_t dt = (int64_t)k * 1000000;
		int64_t res = (int64_t)(uint32_t)(c - c0) - dt; /* us, TIMER minus LF */
		long long ppm_m = k ? (long long)(res * 1000000000LL / dt) : 0;

		out("S5 %s k=%d timer=%u resid_us=%lld ppm_milli=%lld\n", name, k,
		    (unsigned)c, (long long)res, ppm_m);
		sx += k;
		sy += res;
		sxx += (int64_t)k * k;
		sxy += (int64_t)k * res;
		n++;
	}
	nrf_timer_task_trigger(TMR, NRF_TIMER_TASK_STOP);
	int64_t den = (int64_t)n * sxx - sx * sx;

	if (den) {
		/* slope in us per s == ppm; milli-ppm */
		long long slope_m = (long long)(((int64_t)n * sxy - sx * sy) * 1000 / den);

		out("S5 %s fit n=%d slope_milli_ppm=%lld (positive: TIMER/HFXO fast vs LF)\n",
		    name, n, slope_m);
	}
}

/* --------------------------------------------------------------- USB (PM) */

#if CDC_CONSOLE
static const struct device *const con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void reboot_to_bootloader(void)
{
	NRF_POWER->GPREGRET = 0x57; /* DFU_MAGIC_UF2_RESET, Adafruit bootloader */
	sys_reboot(SYS_REBOOT_COLD);
}

static void touch_fn(struct k_timer *t)
{
	uint32_t baud = 0;

	if (uart_line_ctrl_get(con, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 &&
	    baud == 1200) {
		reboot_to_bootloader();
	}
}
static K_TIMER_DEFINE(touch_timer, touch_fn, NULL);

static void usb_up(void)
{
	struct usbd_context *ctx = NULL;

	STRUCT_SECTION_FOREACH(usbd_context, c) {
		ctx = c;
		break;
	}
	if (ctx == NULL || usbd_enable(ctx) != 0) {
		reboot_to_bootloader();
	}
	k_timer_start(&touch_timer, K_MSEC(200), K_MSEC(200));

	struct uart_config cfg;

	if (uart_config_get(con, &cfg) == 0) {
		cfg.flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS; /* poll_out blocks, never drops */
		(void)uart_configure(con, &cfg);
	}
	uint32_t dtr = 0;

	while (!dtr) {
		(void)uart_line_ctrl_get(con, UART_LINE_CTRL_DTR, &dtr);
		k_msleep(50);
	}
	k_msleep(300);
	olive = true;
	for (size_t i = 0; i < olen;) {
		size_t j = i;

		while (j < olen && obuf[j] != '\n') {
			j++;
		}
		printk("%.*s\n", (int)(j - i), &obuf[i]);
		i = j + 1;
	}
	olen = 0;
	out("USB console up; buffered text above was produced with USB disabled\n");
}
#endif

/* ------------------------------------------------------------------- main */

int main(void)
{
	hf_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	constlat(true);
	hf_request();
	int w = hf_wait(K_MSEC(100));

#if !CDC_CONSOLE
	k_msleep(1500); /* time for the host to open the VCOM after reset */
#endif
	out("\nLPSPIKE begin board=%s lf=%s bt=%d timer=0x%08x hfxo_req=%d hf_stat=0x%08x\n",
	    CONFIG_BOARD, LF_GRTC ? "GRTC" : "RTC1", BT_BUILD, (unsigned)(uintptr_t)TMR,
	    w, (unsigned)hf_stat_raw());
	out("LPSPIKE cfg sys_clock_hw_cycles=%d ticks_per_sec=%d hfint_cal=%d k32src_xtal=%d\n",
	    CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC, CONFIG_SYS_CLOCK_TICKS_PER_SEC,
	    IS_ENABLED(CONFIG_CLOCK_CONTROL_NRF_HFINT_CALIBRATION),
	    IS_ENABLED(CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL));
#if LF_GRTC
	out("LPSPIKE grtc_source lfxo=%d system_lfclk=%d lflprc=%d\n",
	    IS_ENABLED(CONFIG_NRF_GRTC_TIMER_SOURCE_LFXO),
	    IS_ENABLED(CONFIG_NRF_GRTC_TIMER_SOURCE_SYSTEM_LFCLK),
	    IS_ENABLED(CONFIG_NRF_GRTC_TIMER_SOURCE_LFLPRC));
#endif
	{
		nrf_clock_lfclk_t lfsrc = 0;
		bool lfon = nrf_clock_is_running(NRF_CLOCK, NRF_CLOCK_DOMAIN_LFCLK, &lfsrc);

		out("LPSPIKE lfclk running=%d src=%d (1 = xtal on both SoCs)\n", lfon,
		    (int)lfsrc);
	}

#if BT_BUILD
	int be = bt_enable(NULL);
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
		BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	};
	int ae = be ? -1 : bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), NULL, 0);

	out("LPSPIKE bt_enable=%d adv_start=%d\n", be, ae);
	k_msleep(500);
#endif

	s2();
	if (!conns[C_ANCHOR].ok || !conns[C_MEAS].ok) {
		out("LPSPIKE abort: anchor/measure connection missing\n");
		goto done;
	}
#if BT_BUILD
	s3("held_bt", false, 2000);
#elif defined(SPIKE_S3ONLY)
	s3("lp_v2", true, NMAX);
	s3("held_v2", false, NMAX);
#else
	s1();
	s4();
	s3("lp", true, NMAX);
#if CDC_CONSOLE
	usb_up();
#endif
	s3("held", false, NMAX);
	s5("sleep", false, 300);
	s5("busy", true, 120);
#endif
done:
	out("LPSPIKE DONE\n");
	while (1) {
#if CDC_CONSOLE
		uint8_t ch;

		if (uart_poll_in(con, &ch) == 0 && (ch == 'B' || ch == 'b')) {
			out("rebooting to UF2 bootloader\n");
			k_msleep(100);
			reboot_to_bootloader();
		}
		k_msleep(50);
#else
		k_sleep(K_FOREVER);
#endif
	}
	return 0;
}
