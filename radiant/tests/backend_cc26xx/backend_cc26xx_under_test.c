/* SPDX-License-Identifier: Apache-2.0 */
/*
 * backend_cc26xx_under_test.c - the real TI backend, included against the
 * fake RF driver (fakes/ti/drivers/rf/RF.h, fake_rf.c) and hal_ti's real
 * driverlib command headers. Only this file may include it.
 *
 * The module's CC26XX symbols depend on a TI SoC, so the backend's knobs are
 * defined here at their Kconfig defaults (radiant/Kconfig), as
 * radiant/tests/backend does for the nRF backend. -DBACKEND_COEX=1 (scenario
 * radiant.backend_cc26xx.coex) adds CONFIG_RADIANT_BACKEND_CC26XX_COEX and the
 * MULTI_PATCH it selects; radiant_radio_cc26xx_arb.c is then compiled too.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>

#define CONFIG_RADIANT_CC26XX_RX_BW_CODE       98
#define CONFIG_RADIANT_CC26XX_INT_FREQ         0x1000
#define CONFIG_RADIANT_CC26XX_FREQ_TRIM_KHZ    0
#define CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US   100
#define CONFIG_RADIANT_CC26XX_AA_FILTER        0x5
#define CONFIG_RADIANT_CC26XX_AGC_REF          0x0034
#define CONFIG_RADIANT_CC26XX_RX_END_SLOP_US   500

#if defined(BACKEND_COEX)
#define CONFIG_RADIANT_BACKEND_CC26XX_COEX       1
#define CONFIG_RADIANT_BACKEND_CC26XX_MULTI_PATCH 1
#endif

#include "../../src/radiant_radio_cc26xx.c"

#include <radiant/radiant_frame.h>

#include "backend_cc26xx_probe.h"

/* ---- The core's callbacks ---- */

static struct cc_probe probe;

struct cc_probe *cc_probe(void) { return &probe; }

static void order_push(char c)
{
	const size_t n = strlen(probe.order);

	if (n + 1u < sizeof(probe.order)) {
		probe.order[n] = c;
		probe.order[n + 1u] = '\0';
	}
}

static void core_rx(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);
	if (probe.rx_events < CC_PROBE_LOG) {
		struct cc_rx_rec *r = &probe.rx[probe.rx_events];

		r->op = evt->op;
		r->status = evt->status;
		r->t_sync = evt->t_sync;
		r->t_sync_exact = evt->t_sync_exact;
		r->filter_index = evt->filter_index;
		r->wild_value = evt->wild_value;
		r->body_len = evt->body_len;
		if (evt->body != NULL) {
			memcpy(r->body, evt->body,
			       MIN((size_t)evt->body_len, sizeof(r->body)));
		}
		r->has_rssi = evt->has_rssi;
		r->rssi_dbm = evt->rssi_dbm;
		r->has_crc_rx = evt->has_crc_rx;
		r->crc_rx = evt->crc_rx;
		r->has_noise = evt->has_noise;
		r->noise_dbm = evt->noise_dbm;
		r->busy_in_cb = st.kind != OP_NONE;
	}
	probe.rx_events++;
	order_push('R');
	if (probe.on_rx != NULL) {
		probe.on_rx(evt);
	}
}

static void core_tx(const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(user);
	if (probe.tx_events < CC_PROBE_LOG) {
		struct cc_tx_rec *r = &probe.tx[probe.tx_events];

		r->op = evt->op;
		r->status = evt->status;
		r->t_sync = evt->t_sync;
		r->t_sync_exact = evt->t_sync_exact;
		r->busy_in_cb = st.kind != OP_NONE;
	}
	probe.tx_events++;
	order_push('T');
	if (probe.on_tx != NULL) {
		probe.on_tx(evt);
	}
}

static const struct radiant_radio_cbs core_cbs = { .rx = core_rx, .tx = core_tx };

void cc_reset(bool enable)
{
	/* radiant_radio_init() re-initialises the keepalive timer; stop the
	 * last one first so a running timer is never re-initialised. */
	if (st.inited) {
		k_timer_stop(&rat_keepalive);
	}
	/* A fresh boot: a second init without one is ESTATE (HAL item 5). */
	st.inited = false;
	st.enabled = false;
	fake_rf_reset();
	fake_rf_set_rat(CC_RAT_START);
	memset(&probe, 0, sizeof(probe));
	radiant_cc26xx_sync_frames = 0u;
	radiant_cc26xx_sw_addr_mismatch = 0u;
	(void)radiant_radio_init(&core_cbs, NULL);
	if (enable) {
		(void)radiant_radio_enable();
	}
}

/* ---- Private state ---- */

int      cc_kind(void) { return (int)st.kind; }
uint32_t cc_op_id(void) { return st.op_id; }
int16_t  cc_cmd_handle(void) { return st.cmd_handle; }
/* The newest entry of st.retired (NO_HANDLE once its callback came). */
int16_t  cc_retired_handle(void)
{
	return st.retired[(st.retired_next + RETIRED_MAX - 1u) % RETIRED_MAX];
}
uint8_t  cc_hw_addr_len(void) { return st.hw_addr_len; }
uint8_t  cc_sw_addr_len(void) { return st.sw_addr_len; }
bool     cc_delivered_terminal(void) { return st.delivered_terminal; }
void     cc_set_next_op_id(uint32_t v) { st.next_op_id = v; }
uint32_t cc_sw_addr_mismatch(void) { return radiant_cc26xx_sw_addr_mismatch; }
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
bool     cc_diag_built(void) { return true; }
uint32_t cc_status_err(void) { return cc_diag.status_err; }
uint32_t cc_window_drops(void) { return cc_diag.window_drops; }
#else
bool     cc_diag_built(void) { return false; }
uint32_t cc_status_err(void) { return 0u; }
uint32_t cc_window_drops(void) { return 0u; }
#endif

void cc_forget_init(void)
{
	if (st.inited) {
		k_timer_stop(&rat_keepalive);
	}
	st.inited = false;
	st.enabled = false;
}

bool cc_rf_open(void) { return st.rf_handle != NULL; }

int cc_init_call(int which)
{
	static const struct radiant_radio_cbs no_rx = { .rx = NULL, .tx = core_tx };
	static const struct radiant_radio_cbs no_tx = { .rx = core_rx, .tx = NULL };

	/* An init that wrongly succeeds re-initialises the keepalive timer;
	 * never let it do so to a running one (a harness hazard, not the
	 * backend's). cc_reset() stops it again whatever happened. */
	if (st.inited) {
		k_timer_stop(&rat_keepalive);
	}
	switch (which) {
	case 0:  return radiant_radio_init(NULL, NULL);
	case 1:  return radiant_radio_init(&no_rx, NULL);
	case 2:  return radiant_radio_init(&no_tx, NULL);
	default: return radiant_radio_init(&core_cbs, NULL);
	}
}

/* The same macros radiant_radio_tx() and the caps use, at any calibration. */
int32_t cc_tx_cal_us(void) { return CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US; }

radiant_time_t cc_tx_start_us(radiant_time_t t, uint8_t addr_len, int32_t cal)
{
	return tx_start_us(t, addr_len, cal);
}

uint32_t cc_tx_arm_floor_us(uint8_t addr_len, bool retune, int32_t cal)
{
	return TX_ARM_FLOOR_US(addr_len, retune, cal);
}

uint32_t cc_min_arm_lead_us(int32_t cal)
{
#if defined(CONFIG_RADIANT_BACKEND_CC26XX_COEX)
	return CC26XX_MIN_ARM_LEAD_US(cal) + CC26XX_COEX_PHY_SWITCH_LEAD_US;
#else
	return CC26XX_MIN_ARM_LEAD_US(cal);
#endif
}

uint32_t cc_in_grant_lead_us(int32_t cal)
{
#if defined(CONFIG_RADIANT_BACKEND_CC26XX_COEX)
	return CC26XX_IN_GRANT_LEAD_US(600, cal);
#else
	return CC26XX_IN_GRANT_LEAD_US(IN_GRANT_LEAD_BASE_US, cal);
#endif
}

/* ---- Requests and payloads ---- */

const uint8_t cc_addr[RADIANT_RADIO_ADDR_MAX] = { 0xA6, 0xC5, 0x34, 0x12, 0x78 };

const struct radiant_pkt_format *cc_fmt(void)
{
	return radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
}

void cc_rx_req(struct radiant_rx_req *req, struct radiant_rx_filter *filt,
	       radiant_time_t t_open, uint32_t len_us)
{
	memset(filt, 0, sizeof(*filt));
	memcpy(filt->addr, cc_addr, sizeof(cc_addr));
	filt->addr_len = cc_fmt()->addr_len;

	memset(req, 0, sizeof(*req));
	req->fmt = cc_fmt();
	req->rf_index = ANT_RF_INDEX_DEFAULT;
	req->filters = filt;
	req->n_filters = 1u;
	req->t_open = t_open;
	req->t_close = t_open + len_us;
}

void cc_tx_req(struct radiant_tx_req *req, const uint8_t *body,
	       radiant_time_t t_sync)
{
	memset(req, 0, sizeof(*req));
	req->fmt = cc_fmt();
	req->rf_index = ANT_RF_INDEX_DEFAULT;
	memcpy(req->addr, cc_addr, sizeof(cc_addr));
	req->addr_len = req->fmt->addr_len;
	req->body = body;
	req->body_len = req->fmt->body_len;
	req->t_sync_at = t_sync;
}

uint8_t cc_payload(const uint8_t *addr, uint8_t hw_len, const uint8_t *body,
		   bool corrupt_crc, uint8_t *out)
{
	const struct radiant_pkt_format *f = cc_fmt();
	uint8_t covered[RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX];
	uint8_t n = 0u;
	uint16_t crc;

	memcpy(covered, addr, f->addr_len);
	memcpy(&covered[f->addr_len], body, f->body_len);
	crc = crc16_itu_t((uint16_t)f->crc.init, covered,
			  (size_t)f->addr_len + f->body_len) ^ (uint16_t)f->crc.xor_out;
	if (corrupt_crc) {
		crc ^= 1u;
	}

	memcpy(&out[n], &addr[hw_len], (size_t)(f->addr_len - hw_len));
	n += (uint8_t)(f->addr_len - hw_len);
	memcpy(&out[n], body, f->body_len);
	n += f->body_len;
	out[n++] = (uint8_t)(crc >> 8);
	out[n++] = (uint8_t)(crc & 0xFFu);
	return n;
}

uint32_t cc_rat_for_t_sync(radiant_time_t t)
{
	return (uint32_t)((t - (radiant_time_t)radiant_addr_split_t_sync_us(st.sw_addr_len)) *
			  RAT_TICKS_PER_US);
}
