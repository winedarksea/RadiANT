/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_radio.c - an in-memory backend for radiant_core/include/radiant_core/radiant_radio_hal.h.
 *
 * Provenance: original clean-room work. Written against
 * radiant_core/include/radiant_core/radiant_radio_hal.h (keystone K3), docs/backends.md,
 * docs/ant-radio-link.md and the measurements in docs/spike-a-results.md, plus
 * public nRF52840/nRF54L15 datasheet figures and public Silicon Labs RAIL
 * documentation for the capability presets. Nothing here derives from sdk-ant,
 * from libant.a, or from any adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * The shape of it
 * ---------------------------------------------------------------------------
 * This file defines every function radiant_radio_hal.h declares, so the unit-test
 * binary links it in place of a backend. See fake_radio.h for the control
 * surface a test drives it with and for why the mock is deliberately strict.
 *
 * Three design choices are worth stating here, because they are what make the
 * mock worth trusting rather than merely convenient.
 *
 * 1. IT MODELS THE AIR, NOT THE API. A queued frame is a byte stream that
 *    exists at one instant. Whether the core hears it depends on whether a
 *    window was open, whether one of the window's filters matched its leading
 *    bytes, and whether the body that remains after the address agrees with
 *    the window's length rule. Nothing is "handed to the next rx call". The
 *    consequence is that "the window closed with nothing in it" is the default
 *    outcome of a mistake rather than something a test has to arrange, which
 *    is the same way the bench behaves.
 *
 * 2. THE ADDRESS/BODY SPLIT BELONGS TO THE RECEIVER. Spike A measured the same
 *    frame arriving as a 5-byte address plus a 10-byte body under the tracking
 *    configuration and as a 3-byte address plus a 12-byte body under the
 *    search configuration. Storing one byte stream and cutting it at
 *    fmt->addr_len reproduces both from one queued frame, which is exactly the
 *    behaviour radiant_search.c depends on when it reads devnum_hi and the device
 *    type out of the body of a frame whose devnum_lo it recovered from the
 *    filter index.
 *
 * 3. NO WALL CLOCK, ANYWHERE. radiant_radio_now() reads a variable. It changes
 *    only inside fake_radio_advance*() / fake_radio_step(), and while a
 *    callback runs it reads that event's own instant. A test that has to sleep
 *    to make something happen is a test that will flake in CI.
 *
 * There is no dynamic allocation and no synchronisation: the test thread is
 * the only thread, and "interrupt context" is modelled by a flag that the
 * entry points check. That flag is not decoration - the HAL's callback
 * contract lists things a callback must not do, and a mock that allows them
 * trains six core modules to write code that deadlocks on real silicon.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_radio_hal.h>
#include "fake_radio.h"

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

enum fake_lifecycle {
	LC_DOWN = 0,   /* before radiant_radio_init() */
	LC_INIT,       /* initialised, not powered */
	LC_ENABLED     /* arm calls are legal */
};

enum due_kind {
	DUE_NONE = 0,
	DUE_AIR,      /* a frame is at the antenna */
	DUE_CLOSE,    /* the receive window's t_close */
	DUE_TX        /* the transmit's t_sync */
};

struct air_slot {
	struct fake_radio_air f;
	bool                  used;
	bool                  done;  /* delivered, dropped or missed */
};

struct cur_op {
	enum fake_radio_arm_kind kind;
	uint32_t                 op;
	int32_t                  arm_idx;  /* -1 when the arm log overflowed */

	struct radiant_pkt_format fmt;

	/* receive */
	struct radiant_rx_filter filters[FAKE_RADIO_MAX_FILTERS];
	uint8_t              n_filters;
	uint32_t             flags;
	radiant_time_t           t_open;
	radiant_time_t           t_close;

	/* transmit */
	radiant_time_t t_sync_req;
	radiant_time_t t_fire;

	bool                  force_terminal_set;
	enum radiant_radio_status force_terminal;
};

struct fake_state {
	enum fake_lifecycle  lc;
	struct radiant_radio_cbs cbs;
	void                *user;

	radiant_time_t now;
	uint32_t   next_op;

	struct radiant_radio_caps caps;

	struct cur_op cur;

	/* deferred abort: a callback may abort, but callbacks never nest, so
	 * the terminal event waits until the current one returns */
	uint32_t                 defer_abort_op;
	enum fake_radio_arm_kind defer_abort_kind;
	bool                     draining;

	bool     in_cb;
	uint32_t isr_calls;
	uint16_t isr_budget;
	bool     isr_work_flagged;

	int      force_arm_rc;
	bool     force_arm_set;
	int32_t  tx_offset_us;
	int8_t   default_rssi;

	/* A forced terminal status waiting for the next operation to be armed. */
	bool                  pending_force_set;
	enum radiant_radio_status pending_force;

	struct air_slot air[FAKE_RADIO_AIR_MAX];

	struct fake_radio_arm arms[FAKE_RADIO_ARM_LOG_MAX];
	uint32_t              n_arms;        /* stored */
	uint32_t              n_arms_total;  /* attempted */
	struct fake_radio_arm arm_scratch;   /* written when the log is full */

	struct fake_radio_event events[FAKE_RADIO_EVENT_LOG_MAX];
	uint32_t                n_events;
	uint32_t                n_events_total;

	struct fake_radio_viol_rec viols[FAKE_RADIO_VIOL_MAX];
	uint32_t                   n_viols;
	uint32_t                   n_viols_total;

	struct fake_radio_stats stats;

	/* The event body the callback sees. Poisoned the moment the callback
	 * returns, so retaining the pointer - which the HAL forbids, because on
	 * hardware it is a DMA buffer - produces visible garbage rather than a
	 * test that passes by luck. */
	uint8_t body_buf[RADIANT_RADIO_BODY_MAX];
};

static struct fake_state g;

/* Only the 1 Mbps GFSK PHY is modelled: it is the only one an ANT+
 * compatibility channel may use, and the Phase 7 long-range entry has no
 * defined behaviour to fake yet. A test that needs a backend advertising both
 * calls fake_radio_set_phys() with its own static array. */
static const enum radiant_phy fake_phys_1m[] = { RADIANT_PHY_1M_GFSK };

static const struct radiant_radio_cbs fake_cbs_none = { NULL, NULL };

static const struct fake_radio_viol_rec viol_none = {
	FAKE_RADIO_VIOL_NONE, "none", 0, 0
};

/* ---------------------------------------------------------------------------
 * Violations
 * ---------------------------------------------------------------------------
 */

static void record_viol(enum fake_radio_viol code, const char *detail)
{
	g.n_viols_total++;
	if (g.n_viols >= FAKE_RADIO_VIOL_MAX) {
		return;
	}
	g.viols[g.n_viols].code = code;
	g.viols[g.n_viols].detail = detail;
	g.viols[g.n_viols].when = g.now;
	g.viols[g.n_viols].op = g.cur.op;
	g.n_viols++;
}

const char *fake_radio_viol_name(enum fake_radio_viol v)
{
	switch (v) {
	case FAKE_RADIO_VIOL_NONE:
		return "none";
	case FAKE_RADIO_VIOL_ISR_LIFECYCLE:
		return "init/enable/disable called from a callback";
	case FAKE_RADIO_VIOL_ISR_HARNESS:
		return "the test harness was driven from a callback";
	case FAKE_RADIO_VIOL_ISR_WORK:
		return "callback exceeded the HAL-call budget";
	case FAKE_RADIO_VIOL_CB_NESTED:
		return "a callback was about to nest";
	case FAKE_RADIO_VIOL_DOUBLE_TERMINAL:
		return "a second terminal event for one operation";
	case FAKE_RADIO_VIOL_EVENT_AFTER_END:
		return "an event for an operation that had ended";
	case FAKE_RADIO_VIOL_CLOCK_BACKWARDS:
		return "the virtual clock was moved backwards";
	case FAKE_RADIO_VIOL_ADVANCE_BUDGET:
		return "advance budget exhausted: the core is livelocking";
	case FAKE_RADIO_VIOL_CAPS_UNMODELLABLE:
		return "caps set to something the mock cannot model";
	case FAKE_RADIO_VIOL_QUEUE_FULL:
		return "the air queue is full";
	case FAKE_RADIO_VIOL_LOG_FULL:
		return "a record log is full";
	case FAKE_RADIO_VIOL_COUNT:
	default:
		return "unknown";
	}
}

uint32_t fake_radio_viol_count(void)
{
	return g.n_viols_total;
}

const struct fake_radio_viol_rec *fake_radio_viol(uint32_t index)
{
	if (index >= g.n_viols) {
		return &viol_none;
	}
	return &g.viols[index];
}

/* ---------------------------------------------------------------------------
 * Interrupt-context accounting
 * ---------------------------------------------------------------------------
 */

/*
 * Every HAL entry point calls this first. Inside a callback it counts, and
 * past the budget it records a violation once.
 *
 * The budget is a heuristic and is documented as one: "do work proportional to
 * anything - queue it and return" is not mechanically checkable, but a
 * callback that loops over 32 channels calling radiant_radio_now() is doing
 * precisely that, and the count catches it. Blocking, sleeping and taking a
 * mutex are not checkable here at all - there is nothing to block on in a
 * single-threaded test - which is why fake_radio_in_callback() is public: core
 * code that has any doubt can assert on it directly.
 */
static void isr_tick(void)
{
	if (!g.in_cb) {
		return;
	}
	g.isr_calls++;
	if (g.isr_budget != 0u && g.isr_calls > g.isr_budget &&
	    !g.isr_work_flagged) {
		g.isr_work_flagged = true;
		record_viol(FAKE_RADIO_VIOL_ISR_WORK,
			    "callback made more HAL calls than the budget");
	}
}

static bool reject_from_callback(void)
{
	if (!g.in_cb) {
		return false;
	}
	record_viol(FAKE_RADIO_VIOL_ISR_HARNESS,
		    "the harness control surface is not callable from a callback");
	return true;
}

bool fake_radio_in_callback(void)
{
	return g.in_cb;
}

void fake_radio_set_isr_call_budget(uint16_t calls)
{
	g.isr_budget = calls;
}

/* ---------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------------
 */

/*
 * The presets.
 *
 * Only three numbers in each are documented facts rather than plausible
 * placeholders, and they are the three the portability argument rests on:
 * max_filters, addr_len_hw_max and crc_in_hw (docs/backends.md, "the caps
 * table"). The nRF row is 8 / 5 / true - one base address plus eight prefixes,
 * a five-byte matcher, a CRC engine that can be made to cover the address. The
 * RAIL row is 2 / 4 / false - two runtime sync words, a four-byte matcher, and
 * a CRC engine that cannot fold in a sync word that varies per channel, so the
 * backend verifies in software.
 *
 * The timing figures are deliberately non-zero and deliberately different
 * between the two presets. They are not measurements and must not be quoted as
 * any part's real behaviour - Spike A did not timestamp anything and the
 * t_sync calibration constant remains unmeasured. Their only job is to make a
 * core module that hardcoded one backend's constant fail under the other.
 */
void fake_radio_caps_preset_nrf(void)
{
	struct radiant_radio_caps *c = &g.caps;

	memset(c, 0, sizeof(*c));
	c->name = "fake:nrf";
	c->max_filters = 8u;
	c->filter_wildcard_dev = false;
	c->addr_len_hw_max = 5u;
	c->max_body_len = RADIANT_RADIO_BODY_MAX;
	c->phys = fake_phys_1m;
	c->n_phys = 1u;
	c->phy_switch_us = 0u;
	c->ramp_up_us = 40u;
	c->rx_to_tx_us = 130u;
	c->tx_to_rx_us = 130u;
	c->min_arm_lead_us = 200u;
	c->time_resolution_ns = 1000u;
	c->has_sync_timestamp = true;
	c->has_rssi = true;
	c->crc_in_hw = true;
	c->tx_power_min_dbm = -40;
	c->tx_power_max_dbm = 8;
}

void fake_radio_caps_preset_rail(void)
{
	struct radiant_radio_caps *c = &g.caps;

	memset(c, 0, sizeof(*c));
	c->name = "fake:rail";
	c->max_filters = 2u;
	c->filter_wildcard_dev = false;
	c->addr_len_hw_max = 4u;
	c->max_body_len = RADIANT_RADIO_BODY_MAX;
	c->phys = fake_phys_1m;
	c->n_phys = 1u;
	/* Non-zero because a RAIL PHY comes out of a generated configuration
	 * and switching means reloading it; the scheduler has to budget for
	 * that where the nRF backend pays nothing. */
	c->phy_switch_us = 500u;
	c->ramp_up_us = 100u;
	c->rx_to_tx_us = 200u;
	c->tx_to_rx_us = 200u;
	c->min_arm_lead_us = 400u;
	c->time_resolution_ns = 1000u;
	c->has_sync_timestamp = true;
	c->has_rssi = true;
	c->crc_in_hw = false;
	c->tx_power_min_dbm = -26;
	c->tx_power_max_dbm = 20;
}

struct radiant_radio_caps *fake_radio_caps_mut(void)
{
	return &g.caps;
}

void fake_radio_set_phys(const enum radiant_phy *phys, uint8_t n_phys)
{
	g.caps.phys = phys;
	g.caps.n_phys = n_phys;
}

const struct radiant_radio_caps *radiant_radio_caps_get(void)
{
	isr_tick();
	return &g.caps;
}

static bool phy_supported(enum radiant_phy phy)
{
	uint8_t i;

	if (g.caps.phys == NULL) {
		return false;
	}
	for (i = 0u; i < g.caps.n_phys; i++) {
		if (g.caps.phys[i] == phy) {
			return true;
		}
	}
	return false;
}

/* Caps a test set to something with no defined meaning here. Checked on every
 * arm rather than in the setter, because the setter is a raw pointer by
 * design. */
static bool caps_modellable(void)
{
	if (g.caps.max_filters == 0u ||
	    g.caps.max_filters > FAKE_RADIO_MAX_FILTERS) {
		record_viol(FAKE_RADIO_VIOL_CAPS_UNMODELLABLE,
			    "max_filters outside 1..FAKE_RADIO_MAX_FILTERS");
		return false;
	}
	if (g.caps.phys == NULL || g.caps.n_phys == 0u) {
		record_viol(FAKE_RADIO_VIOL_CAPS_UNMODELLABLE,
			    "caps advertises no PHY");
		return false;
	}
	if (g.caps.max_body_len == 0u ||
	    g.caps.max_body_len > RADIANT_RADIO_BODY_MAX) {
		record_viol(FAKE_RADIO_VIOL_CAPS_UNMODELLABLE,
			    "max_body_len outside 1..RADIANT_RADIO_BODY_MAX");
		return false;
	}
	return true;
}

/* ---------------------------------------------------------------------------
 * Logs
 * ---------------------------------------------------------------------------
 */

static struct fake_radio_arm *new_arm_record(enum fake_radio_arm_kind kind,
					     int32_t *idx_out)
{
	struct fake_radio_arm *a;

	g.n_arms_total++;
	if (g.n_arms >= FAKE_RADIO_ARM_LOG_MAX) {
		record_viol(FAKE_RADIO_VIOL_LOG_FULL, "arm log full");
		a = &g.arm_scratch;
		*idx_out = -1;
	} else {
		a = &g.arms[g.n_arms];
		*idx_out = (int32_t)g.n_arms;
		g.n_arms++;
	}
	memset(a, 0, sizeof(*a));
	a->kind = kind;
	a->t_arm = g.now;
	a->from_callback = g.in_cb;
	return a;
}

static struct fake_radio_arm *arm_at(int32_t idx)
{
	if (idx < 0 || (uint32_t)idx >= g.n_arms) {
		return NULL;
	}
	return &g.arms[idx];
}

uint32_t fake_radio_arm_count(void)
{
	return g.n_arms_total;
}

const struct fake_radio_arm *fake_radio_arm(uint32_t index)
{
	if (index >= g.n_arms) {
		return NULL;
	}
	return &g.arms[index];
}

const struct fake_radio_arm *fake_radio_arm_for_op(uint32_t op)
{
	uint32_t i;

	if (op == 0u) {
		return NULL;
	}
	for (i = 0u; i < g.n_arms; i++) {
		if (g.arms[i].op == op) {
			return &g.arms[i];
		}
	}
	return NULL;
}

const struct fake_radio_arm *fake_radio_last_arm(void)
{
	if (g.n_arms == 0u) {
		return NULL;
	}
	return &g.arms[g.n_arms - 1u];
}

uint32_t fake_radio_event_count(void)
{
	return g.n_events_total;
}

const struct fake_radio_event *fake_radio_event(uint32_t index)
{
	if (index >= g.n_events) {
		return NULL;
	}
	return &g.events[index];
}

const struct fake_radio_event *fake_radio_last_event(void)
{
	if (g.n_events == 0u) {
		return NULL;
	}
	return &g.events[g.n_events - 1u];
}

const struct fake_radio_stats *fake_radio_stats(void)
{
	return &g.stats;
}

static void count_status(enum radiant_radio_status st, bool is_tx)
{
	if (is_tx) {
		g.stats.ev_tx++;
	}
	switch (st) {
	case RADIANT_RADIO_STATUS_OK:
		if (!is_tx) {
			g.stats.ev_ok++;
		}
		break;
	case RADIANT_RADIO_STATUS_CRC_FAIL:
		g.stats.ev_crc_fail++;
		break;
	case RADIANT_RADIO_STATUS_TIMEOUT:
		g.stats.ev_timeout++;
		break;
	case RADIANT_RADIO_STATUS_ABORTED:
		g.stats.ev_aborted++;
		break;
	case RADIANT_RADIO_STATUS_FAILED:
		g.stats.ev_failed++;
		break;
	default:
		break;
	}
}

static struct fake_radio_event *new_event_record(void)
{
	struct fake_radio_event *e;

	g.n_events_total++;
	if (g.n_events >= FAKE_RADIO_EVENT_LOG_MAX) {
		record_viol(FAKE_RADIO_VIOL_LOG_FULL, "event log full");
		return NULL;
	}
	e = &g.events[g.n_events];
	g.n_events++;
	memset(e, 0, sizeof(*e));
	return e;
}

/* ---------------------------------------------------------------------------
 * Delivering events
 * ---------------------------------------------------------------------------
 */

static void run_deferred(void);

static bool has_frame_fields(enum radiant_radio_status st)
{
	return st == RADIANT_RADIO_STATUS_OK || st == RADIANT_RADIO_STATUS_CRC_FAIL;
}

static void deliver_rx(uint32_t op, enum radiant_radio_status status, bool terminal,
		       radiant_time_t t_sync, uint8_t filter_index,
		       const uint8_t *body, uint8_t body_len, int8_t rssi_dbm,
		       bool late)
{
	struct radiant_rx_event evt;
	struct fake_radio_event *rec;
	bool frame = has_frame_fields(status);

	if (g.in_cb) {
		record_viol(FAKE_RADIO_VIOL_CB_NESTED, "rx callback would nest");
		return;
	}

	memset(&evt, 0, sizeof(evt));
	memset(g.body_buf, 0, sizeof(g.body_buf));
	if (frame && body != NULL && body_len > 0u) {
		memcpy(g.body_buf, body,
		       body_len > sizeof(g.body_buf) ? sizeof(g.body_buf)
						     : body_len);
	}

	evt.op = op;
	evt.status = status;
	/* The HAL says t_sync, filter_index, body and rssi are meaningful only
	 * for OK and CRC_FAIL. Leaving them as a NULL body and a zero
	 * timestamp on the other statuses makes a core module that reads them
	 * anyway fault in CI instead of quietly using a stale value. */
	evt.t_sync = frame ? t_sync : 0u;
	evt.t_sync_exact = frame ? g.caps.has_sync_timestamp : false;
	evt.filter_index = frame ? filter_index : 0u;
	evt.body = frame ? g.body_buf : NULL;
	evt.body_len = frame ? body_len : 0u;
	evt.has_rssi = frame && g.caps.has_rssi;
	evt.rssi_dbm = evt.has_rssi ? rssi_dbm : 0;

	rec = new_event_record();
	if (rec != NULL) {
		rec->op = op;
		rec->kind = FAKE_RADIO_ARM_RX;
		rec->status = status;
		rec->terminal = terminal;
		rec->t = g.now;
		rec->t_sync = evt.t_sync;
		rec->t_sync_exact = evt.t_sync_exact;
		rec->filter_index = evt.filter_index;
		rec->body_len = evt.body_len;
		if (evt.body_len > 0u) {
			memcpy(rec->body, g.body_buf, evt.body_len);
		}
		rec->has_rssi = evt.has_rssi;
		rec->rssi_dbm = evt.rssi_dbm;
		rec->late = late;
	}
	count_status(status, false);
	if (late) {
		g.stats.ev_late++;
	}

	g.in_cb = true;
	g.isr_calls = 0u;
	g.isr_work_flagged = false;
	g.stats.callbacks++;
	if (g.cbs.rx != NULL) {
		g.cbs.rx(&evt, g.user);
	}
	g.in_cb = false;
	memset(g.body_buf, FAKE_RADIO_BODY_POISON, sizeof(g.body_buf));

	run_deferred();
}

static void deliver_tx(uint32_t op, enum radiant_radio_status status,
		       radiant_time_t t_sync, bool late)
{
	struct radiant_tx_event evt;
	struct fake_radio_event *rec;

	if (g.in_cb) {
		record_viol(FAKE_RADIO_VIOL_CB_NESTED, "tx callback would nest");
		return;
	}

	memset(&evt, 0, sizeof(evt));
	evt.op = op;
	evt.status = status;
	evt.t_sync = (status == RADIANT_RADIO_STATUS_OK) ? t_sync : 0u;
	evt.t_sync_exact = (status == RADIANT_RADIO_STATUS_OK) &&
			   g.caps.has_sync_timestamp;

	rec = new_event_record();
	if (rec != NULL) {
		rec->op = op;
		rec->kind = FAKE_RADIO_ARM_TX;
		rec->status = status;
		rec->terminal = true;
		rec->t = g.now;
		rec->t_sync = evt.t_sync;
		rec->t_sync_exact = evt.t_sync_exact;
		rec->late = late;
	}
	count_status(status, true);
	if (late) {
		g.stats.ev_late++;
	}

	g.in_cb = true;
	g.isr_calls = 0u;
	g.isr_work_flagged = false;
	g.stats.callbacks++;
	if (g.cbs.tx != NULL) {
		g.cbs.tx(&evt, g.user);
	}
	g.in_cb = false;

	run_deferred();
}

/* ---------------------------------------------------------------------------
 * Ending an operation
 * ---------------------------------------------------------------------------
 */

static void mark_terminal(int32_t arm_idx, enum radiant_radio_status st)
{
	struct fake_radio_arm *a = arm_at(arm_idx);

	if (a == NULL) {
		return;
	}
	if (a->terminated) {
		record_viol(FAKE_RADIO_VIOL_DOUBLE_TERMINAL,
			    "operation already had a terminal event");
		return;
	}
	a->terminated = true;
	a->terminal_status = st;
	a->t_terminal = g.now;
}

static void clear_cur(void)
{
	memset(&g.cur, 0, sizeof(g.cur));
	g.cur.kind = FAKE_RADIO_ARM_NONE;
	g.cur.arm_idx = -1;
}

/*
 * The slot is freed *before* the terminal callback runs. That is not a
 * shortcut: the HAL says chaining is done by arming the next operation from
 * inside the completion callback, so a mock that still held the slot would
 * return RADIANT_RADIO_EBUSY for the one call pattern the contract exists to
 * permit.
 */
static void end_op(enum radiant_radio_status status, radiant_time_t t_sync)
{
	enum fake_radio_arm_kind kind = g.cur.kind;
	uint32_t op = g.cur.op;
	int32_t arm_idx = g.cur.arm_idx;

	if (kind == FAKE_RADIO_ARM_NONE) {
		return;
	}
	mark_terminal(arm_idx, status);
	clear_cur();

	if (kind == FAKE_RADIO_ARM_TX) {
		deliver_tx(op, status, t_sync, false);
	} else {
		deliver_rx(op, status, true, 0u, 0u, NULL, 0u, 0, false);
	}
}

static void run_deferred(void)
{
	if (g.draining) {
		return;
	}
	g.draining = true;
	while (g.defer_abort_op != 0u) {
		uint32_t op = g.defer_abort_op;
		enum fake_radio_arm_kind kind = g.defer_abort_kind;

		g.defer_abort_op = 0u;
		g.defer_abort_kind = FAKE_RADIO_ARM_NONE;
		if (kind == FAKE_RADIO_ARM_TX) {
			deliver_tx(op, RADIANT_RADIO_STATUS_ABORTED, 0u, false);
		} else {
			deliver_rx(op, RADIANT_RADIO_STATUS_ABORTED, true, 0u, 0u,
				   NULL, 0u, 0, false);
		}
	}
	g.draining = false;
}

/* ---------------------------------------------------------------------------
 * The air
 * ---------------------------------------------------------------------------
 */

void fake_radio_air_init(struct fake_radio_air *f)
{
	if (f == NULL) {
		return;
	}
	memset(f, 0, sizeof(*f));
	f->match = FAKE_RADIO_MATCH_ADDR;
	f->crc_bad = false;
	f->rssi_set = true;
	f->rssi_dbm = g.default_rssi;
}

int fake_radio_air_add(const struct fake_radio_air *f)
{
	uint32_t i;

	if (reject_from_callback()) {
		return RADIANT_RADIO_ESTATE;
	}
	if (f == NULL || f->len == 0u || f->len > FAKE_RADIO_AIR_FRAME_MAX) {
		return RADIANT_RADIO_EINVAL;
	}
	for (i = 0u; i < FAKE_RADIO_AIR_MAX; i++) {
		if (!g.air[i].used) {
			g.air[i].used = true;
			g.air[i].done = false;
			g.air[i].f = *f;
			if (!g.air[i].f.rssi_set) {
				g.air[i].f.rssi_set = true;
				g.air[i].f.rssi_dbm = g.default_rssi;
			}
			return RADIANT_RADIO_OK_RC;
		}
	}
	record_viol(FAKE_RADIO_VIOL_QUEUE_FULL, "air queue full");
	return RADIANT_RADIO_EBUSY;
}

int fake_radio_air_frame(radiant_time_t t_sync, const uint8_t *bytes, uint8_t len)
{
	struct fake_radio_air f;

	if (bytes == NULL || len == 0u || len > FAKE_RADIO_AIR_FRAME_MAX) {
		return RADIANT_RADIO_EINVAL;
	}
	fake_radio_air_init(&f);
	f.t_sync = t_sync;
	f.len = len;
	memcpy(f.bytes, bytes, len);
	return fake_radio_air_add(&f);
}

uint32_t fake_radio_air_master(radiant_time_t t_first, uint32_t period_us,
			       uint32_t count, const uint8_t *bytes,
			       uint8_t len)
{
	uint32_t queued = 0u;
	uint32_t i;

	for (i = 0u; i < count; i++) {
		radiant_time_t t = t_first + (radiant_time_t)period_us * (radiant_time_t)i;

		if (fake_radio_air_frame(t, bytes, len) != RADIANT_RADIO_OK_RC) {
			break;
		}
		queued++;
	}
	return queued;
}

/* Deterministic, so a failing noise test reproduces exactly. */
static uint32_t noise_rand(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

uint32_t fake_radio_air_noise(radiant_time_t t0, radiant_time_t t1, uint32_t count,
			      int8_t rssi_dbm)
{
	uint32_t state = 0x5EED0001u;
	uint32_t queued = 0u;
	uint32_t i;
	uint64_t span = (t1 > t0) ? (uint64_t)(t1 - t0) : 0u;

	for (i = 0u; i < count; i++) {
		struct fake_radio_air f;
		uint8_t b;

		fake_radio_air_init(&f);
		f.t_sync = t0;
		if (count > 1u && span > 0u) {
			f.t_sync = t0 + (radiant_time_t)((span * (uint64_t)i) /
						     (uint64_t)(count - 1u));
		}
		f.crc_bad = true;
		f.rssi_set = true;
		f.rssi_dbm = rssi_dbm;
		f.match = FAKE_RADIO_MATCH_NOISE;
		f.filter_index = (uint8_t)(i & 0xFFu);
		/* 12 bytes: the STATLEN Spike A ran the search configuration
		 * with, so a noise trigger is the same size as the real thing
		 * it is being mistaken for. */
		f.len = 12u;
		for (b = 0u; b < f.len; b++) {
			f.bytes[b] = (uint8_t)(noise_rand(&state) & 0xFFu);
		}
		if (fake_radio_air_add(&f) != RADIANT_RADIO_OK_RC) {
			break;
		}
		queued++;
	}
	return queued;
}

void fake_radio_air_clear(void)
{
	if (reject_from_callback()) {
		return;
	}
	memset(g.air, 0, sizeof(g.air));
}

uint32_t fake_radio_air_pending(void)
{
	uint32_t i;
	uint32_t n = 0u;

	for (i = 0u; i < FAKE_RADIO_AIR_MAX; i++) {
		if (g.air[i].used && !g.air[i].done) {
			n++;
		}
	}
	return n;
}

void fake_radio_set_default_rssi(int8_t dbm)
{
	g.default_rssi = dbm;
}

uint8_t fake_radio_build_ant_frame(uint8_t *out, uint16_t devnum,
				   uint8_t dev_type, uint8_t trans_type,
				   const uint8_t *payload8)
{
	uint8_t i;
	uint8_t n = 0u;

	if (out == NULL) {
		return 0u;
	}
	/* docs/ant-radio-link.md, confirmed byte for byte in
	 * docs/spike-a-results.md: A6 C5 | dl dh | dtype | ttype | 0x0A | 8 */
	out[n++] = 0xA6u;
	out[n++] = 0xC5u;
	out[n++] = (uint8_t)(devnum & 0xFFu);
	out[n++] = (uint8_t)((devnum >> 8) & 0xFFu);
	out[n++] = dev_type;
	out[n++] = trans_type;
	out[n++] = 0x0Au;
	for (i = 0u; i < 8u; i++) {
		out[n++] = (payload8 != NULL) ? payload8[i] : 0u;
	}
	return n;
}

/* ---------------------------------------------------------------------------
 * Matching a frame against the armed window
 * ---------------------------------------------------------------------------
 */

static bool air_matches(const struct fake_radio_air *f, uint8_t *filter_index)
{
	uint8_t i;

	if (g.cur.kind != FAKE_RADIO_ARM_RX || g.cur.n_filters == 0u) {
		return false;
	}

	switch (f->match) {
	case FAKE_RADIO_MATCH_ADDR:
		if (f->len < g.cur.fmt.addr_len) {
			return false;
		}
		for (i = 0u; i < g.cur.n_filters; i++) {
			if (g.cur.filters[i].addr_len != g.cur.fmt.addr_len) {
				continue;
			}
			if (memcmp(f->bytes, g.cur.filters[i].addr,
				   g.cur.fmt.addr_len) == 0) {
				*filter_index = i;
				return true;
			}
		}
		return false;
	case FAKE_RADIO_MATCH_INDEX:
		if (f->filter_index >= g.cur.n_filters) {
			return false;
		}
		*filter_index = f->filter_index;
		return true;
	case FAKE_RADIO_MATCH_NOISE:
		*filter_index = (uint8_t)(f->filter_index % g.cur.n_filters);
		return true;
	default:
		return false;
	}
}

enum air_fit {
	AIR_FIT_OK = 0,
	AIR_FIT_BADLEN,   /* the receiver would read the wrong number of bytes */
	AIR_FIT_OVERSIZE  /* longer than fmt->max_body_len: never delivered */
};

/*
 * Cut the queued byte stream at fmt->addr_len and check what remains against
 * the window's length rule.
 *
 * A mismatch is a CRC failure rather than an error: a receiver configured for
 * the wrong body length reads the wrong bytes as its CRC, and that is what
 * comes out. Modelling it that way is what makes a len_bias mistake in the
 * frame layer show up as loss in a scheduler test rather than as nothing at
 * all.
 */
static enum air_fit air_body(const struct fake_radio_air *f,
			     const uint8_t **body, uint8_t *body_len)
{
	const struct radiant_pkt_format *fmt = &g.cur.fmt;
	uint8_t avail;
	int32_t need;

	if (f->match == FAKE_RADIO_MATCH_NOISE) {
		/* A demodulator that locked onto noise produces bytes, not a
		 * well-formed body. Hand over what there is, bounded. */
		*body = f->bytes;
		*body_len = (f->len > fmt->max_body_len) ? fmt->max_body_len
							 : f->len;
		return AIR_FIT_OK;
	}

	if (f->len < fmt->addr_len) {
		return AIR_FIT_BADLEN;
	}
	avail = (uint8_t)(f->len - fmt->addr_len);
	if (avail > fmt->max_body_len) {
		return AIR_FIT_OVERSIZE;
	}

	*body = f->bytes + fmt->addr_len;
	*body_len = avail;

	if (fmt->len_mode == RADIANT_LEN_FIXED) {
		need = (int32_t)fmt->body_len;
	} else {
		if (fmt->len_offset >= avail) {
			return AIR_FIT_BADLEN;
		}
		need = (int32_t)(*body)[fmt->len_offset] +
		       (int32_t)fmt->len_bias;
	}
	if (need < 0 || need != (int32_t)avail) {
		return AIR_FIT_BADLEN;
	}
	return AIR_FIT_OK;
}

/* ---------------------------------------------------------------------------
 * The clock and the event pump
 * ---------------------------------------------------------------------------
 */

static void mark_missed(radiant_time_t before)
{
	uint32_t i;

	for (i = 0u; i < FAKE_RADIO_AIR_MAX; i++) {
		if (g.air[i].used && !g.air[i].done &&
		    g.air[i].f.t_sync < before) {
			g.air[i].done = true;
			g.stats.air_missed++;
		}
	}
}

static void clock_to(radiant_time_t t)
{
	if (t <= g.now) {
		return;
	}
	g.now = t;
	mark_missed(g.now);
}

static bool next_due(radiant_time_t *t_out, enum due_kind *kind_out,
		     int32_t *air_out)
{
	uint32_t i;
	radiant_time_t best = RADIANT_TIME_NEVER;
	int32_t best_i = -1;

	*air_out = -1;

	if (g.cur.kind == FAKE_RADIO_ARM_TX) {
		*t_out = g.cur.t_fire;
		*kind_out = DUE_TX;
		return true;
	}
	if (g.cur.kind != FAKE_RADIO_ARM_RX) {
		*kind_out = DUE_NONE;
		return false;
	}

	for (i = 0u; i < FAKE_RADIO_AIR_MAX; i++) {
		const struct fake_radio_air *f = &g.air[i].f;
		uint8_t fidx = 0u;

		if (!g.air[i].used || g.air[i].done) {
			continue;
		}
		if (f->t_sync < g.now || f->t_sync < g.cur.t_open ||
		    f->t_sync > g.cur.t_close) {
			continue;
		}
		if (!air_matches(f, &fidx)) {
			continue;
		}
		if (f->t_sync < best) {
			best = f->t_sync;
			best_i = (int32_t)i;
		}
	}

	/* A frame whose t_sync is exactly t_close is inside the window, so it
	 * is delivered before the window closes. */
	if (best_i >= 0) {
		*t_out = best;
		*kind_out = DUE_AIR;
		*air_out = best_i;
		return true;
	}
	*t_out = g.cur.t_close;
	*kind_out = DUE_CLOSE;
	return true;
}

static void dispatch_air(int32_t idx)
{
	struct air_slot *s = &g.air[idx];
	const uint8_t *body = NULL;
	uint8_t body_len = 0u;
	uint8_t fidx = 0u;
	enum air_fit fit;
	bool crc_ok;
	bool terminal;
	enum radiant_radio_status status;
	uint32_t op = g.cur.op;
	int32_t arm_idx = g.cur.arm_idx;
	int8_t rssi;

	s->done = true;

	if (!air_matches(&s->f, &fidx)) {
		g.stats.air_missed++;
		return;
	}

	fit = air_body(&s->f, &body, &body_len);
	if (fit == AIR_FIT_OVERSIZE) {
		/* The HAL says max_body_len is an RX bound and anything longer
		 * is rejected, so the core never sees this at all. */
		g.stats.air_dropped_oversize++;
		return;
	}
	if (fit == AIR_FIT_BADLEN) {
		g.stats.air_dropped_len++;
	}

	crc_ok = !s->f.crc_bad && (fit == AIR_FIT_OK) &&
		 (s->f.match != FAKE_RADIO_MATCH_NOISE);

	if (!crc_ok && (g.cur.flags & RADIANT_RX_REPORT_CRC_FAIL) == 0u) {
		/* Off by default. The count is still kept, because it is what
		 * makes "unexplained loss" a number rather than a shrug. */
		g.stats.crc_fail_suppressed++;
		return;
	}

	status = crc_ok ? RADIANT_RADIO_STATUS_OK : RADIANT_RADIO_STATUS_CRC_FAIL;
	rssi = s->f.rssi_set ? s->f.rssi_dbm : g.default_rssi;

	/*
	 * RADIANT_RX_STOP_ON_FIRST closes the window on the first *accepted* frame,
	 * and a CRC failure is not an accepted frame. Spike A measured a
	 * 3-byte search address triggering the matcher on noise several times a
	 * second; a mock that let a noise trigger close a tracked window would
	 * make every tracking test pass for the wrong reason.
	 */
	terminal = crc_ok && ((g.cur.flags & RADIANT_RX_STOP_ON_FIRST) != 0u);

	g.stats.air_delivered++;
	if (terminal) {
		mark_terminal(arm_idx, status);
		clear_cur();
	} else {
		struct fake_radio_arm *a = arm_at(arm_idx);

		if (a != NULL) {
			a->n_rx_events++;
		}
	}
	deliver_rx(op, status, terminal, s->f.t_sync, fidx, body, body_len,
		   rssi, false);
}

static void dispatch(enum due_kind kind, int32_t air_idx)
{
	switch (kind) {
	case DUE_AIR:
		dispatch_air(air_idx);
		break;
	case DUE_CLOSE: {
		enum radiant_radio_status st = g.cur.force_terminal_set
						   ? g.cur.force_terminal
						   : RADIANT_RADIO_STATUS_TIMEOUT;
		end_op(st, 0u);
		break;
	}
	case DUE_TX: {
		enum radiant_radio_status st = g.cur.force_terminal_set
						   ? g.cur.force_terminal
						   : RADIANT_RADIO_STATUS_OK;
		end_op(st, g.cur.t_fire);
		break;
	}
	case DUE_NONE:
	default:
		break;
	}
}

static void run_until(radiant_time_t target)
{
	uint32_t guard = 0u;

	for (;;) {
		radiant_time_t due = RADIANT_TIME_NEVER;
		enum due_kind kind = DUE_NONE;
		int32_t air_idx = -1;

		if (!next_due(&due, &kind, &air_idx)) {
			break;
		}
		if (due > target) {
			break;
		}
		guard++;
		if (guard > FAKE_RADIO_ADVANCE_BUDGET) {
			record_viol(FAKE_RADIO_VIOL_ADVANCE_BUDGET,
				    "too many events in one advance");
			break;
		}
		clock_to(due);
		dispatch(kind, air_idx);
	}
	clock_to(target);
}

void fake_radio_set_origin(radiant_time_t t)
{
	if (reject_from_callback()) {
		return;
	}
	if (g.cur.kind != FAKE_RADIO_ARM_NONE || fake_radio_air_pending() > 0u) {
		record_viol(FAKE_RADIO_VIOL_ISR_HARNESS,
			    "set_origin with the radio busy or frames queued");
		return;
	}
	g.now = t;
}

void fake_radio_advance_to(radiant_time_t t)
{
	if (reject_from_callback()) {
		return;
	}
	if (t < g.now) {
		record_viol(FAKE_RADIO_VIOL_CLOCK_BACKWARDS,
			    "advance_to an instant already passed");
		return;
	}
	run_until(t);
}

void fake_radio_advance(uint64_t us)
{
	fake_radio_advance_to(g.now + (radiant_time_t)us);
}

radiant_time_t fake_radio_next_due(void)
{
	radiant_time_t due = RADIANT_TIME_NEVER;
	enum due_kind kind = DUE_NONE;
	int32_t air_idx = -1;

	if (!next_due(&due, &kind, &air_idx)) {
		return RADIANT_TIME_NEVER;
	}
	return due;
}

bool fake_radio_step(void)
{
	radiant_time_t due = RADIANT_TIME_NEVER;
	enum due_kind kind = DUE_NONE;
	int32_t air_idx = -1;

	if (reject_from_callback()) {
		return false;
	}
	if (!next_due(&due, &kind, &air_idx)) {
		return false;
	}
	clock_to(due);
	dispatch(kind, air_idx);
	return true;
}

/* ---------------------------------------------------------------------------
 * Validation of an arm request
 * ---------------------------------------------------------------------------
 */

static void count_rc(int rc)
{
	switch (rc) {
	case RADIANT_RADIO_EINVAL:
		g.stats.rc_einval++;
		break;
	case RADIANT_RADIO_ENOTSUP:
		g.stats.rc_enotsup++;
		break;
	case RADIANT_RADIO_EBUSY:
		g.stats.rc_ebusy++;
		break;
	case RADIANT_RADIO_ETIME:
		g.stats.rc_etime++;
		break;
	case RADIANT_RADIO_ESTATE:
		g.stats.rc_estate++;
		break;
	default:
		break;
	}
	if (rc != RADIANT_RADIO_OK_RC) {
		g.stats.arms_rejected++;
	}
}

/*
 * Per-operation configuration is validated in full on every arm call, and this
 * is the single most useful thing the mock does for the core agents.
 *
 * The HAL makes configuration per-operation precisely so that tracking, search
 * and the extension formats cannot contaminate each other. The failure that
 * rule exists to prevent is a module that fills in half a struct and inherits
 * the rest - and on hardware that produces a window that is subtly wrong
 * rather than a window that fails. So a zero addr_len, a zero max_body_len or
 * a length rule that does not describe the body are all rejected here, loudly,
 * at the arm call.
 */
static int check_fmt(const struct radiant_pkt_format *fmt)
{
	if (fmt == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if ((unsigned int)fmt->phy >= (unsigned int)RADIANT_PHY_COUNT) {
		return RADIANT_RADIO_EINVAL;
	}
	if (!phy_supported(fmt->phy)) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (fmt->addr_len < 2u || fmt->addr_len > RADIANT_RADIO_ADDR_MAX) {
		return RADIANT_RADIO_EINVAL;
	}
	if (fmt->max_body_len == 0u || fmt->max_body_len > RADIANT_RADIO_BODY_MAX) {
		return RADIANT_RADIO_EINVAL;
	}
	if (fmt->max_body_len > g.caps.max_body_len) {
		return RADIANT_RADIO_ENOTSUP;
	}
	switch (fmt->len_mode) {
	case RADIANT_LEN_FIXED:
		if (fmt->body_len == 0u || fmt->body_len > fmt->max_body_len) {
			return RADIANT_RADIO_EINVAL;
		}
		break;
	case RADIANT_LEN_FROM_BODY:
		if (fmt->len_offset >= fmt->max_body_len) {
			return RADIANT_RADIO_EINVAL;
		}
		break;
	default:
		return RADIANT_RADIO_EINVAL;
	}
	/* The mock computes no CRC - a queued frame simply says whether its CRC
	 * was good - but it still refuses a width it could not have produced,
	 * so an unset crc block is caught here rather than believed. */
	if (fmt->crc.width_bits != 0u && fmt->crc.width_bits != 16u) {
		return RADIANT_RADIO_ENOTSUP;
	}
	return RADIANT_RADIO_OK_RC;
}

static int8_t clamp_dbm(int8_t dbm)
{
	if (dbm < g.caps.tx_power_min_dbm) {
		return g.caps.tx_power_min_dbm;
	}
	if (dbm > g.caps.tx_power_max_dbm) {
		return g.caps.tx_power_max_dbm;
	}
	return dbm;
}

static bool too_late(radiant_time_t t)
{
	return t < g.now + (radiant_time_t)g.caps.min_arm_lead_us;
}

/* ---------------------------------------------------------------------------
 * HAL: lifecycle
 * ---------------------------------------------------------------------------
 */

int radiant_radio_init(const struct radiant_radio_cbs *cbs, void *user)
{
	isr_tick();
	if (g.in_cb) {
		record_viol(FAKE_RADIO_VIOL_ISR_LIFECYCLE,
			    "radiant_radio_init() from a callback");
		return RADIANT_RADIO_ESTATE;
	}
	if (cbs == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (g.lc == LC_ENABLED) {
		return RADIANT_RADIO_ESTATE;
	}
	g.cbs = *cbs;
	g.user = user;
	clear_cur();
	g.defer_abort_op = 0u;
	g.defer_abort_kind = FAKE_RADIO_ARM_NONE;
	g.lc = LC_INIT;
	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_enable(void)
{
	isr_tick();
	if (g.in_cb) {
		record_viol(FAKE_RADIO_VIOL_ISR_LIFECYCLE,
			    "radiant_radio_enable() from a callback");
		return RADIANT_RADIO_ESTATE;
	}
	if (g.lc == LC_DOWN) {
		return RADIANT_RADIO_ESTATE;
	}
	g.lc = LC_ENABLED;
	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_disable(void)
{
	isr_tick();
	if (g.in_cb) {
		record_viol(FAKE_RADIO_VIOL_ISR_LIFECYCLE,
			    "radiant_radio_disable() from a callback");
		return RADIANT_RADIO_ESTATE;
	}
	if (g.lc == LC_DOWN) {
		return RADIANT_RADIO_ESTATE;
	}
	if (g.cur.kind != FAKE_RADIO_ARM_NONE) {
		end_op(RADIANT_RADIO_STATUS_ABORTED, 0u);
	}
	g.lc = LC_INIT;
	return RADIANT_RADIO_OK_RC;
}

radiant_time_t radiant_radio_now(void)
{
	isr_tick();
	return g.now;
}

/* ---------------------------------------------------------------------------
 * HAL: arming
 * ---------------------------------------------------------------------------
 */

static void take_pending_force(void);

static int arm_preflight(void)
{
	if (g.lc != LC_ENABLED) {
		return RADIANT_RADIO_ESTATE;
	}
	if (g.force_arm_set) {
		int rc = g.force_arm_rc;

		g.force_arm_set = false;
		g.force_arm_rc = RADIANT_RADIO_OK_RC;
		if (rc != RADIANT_RADIO_OK_RC) {
			g.stats.rc_forced++;
			return rc;
		}
	}
	if (g.cur.kind != FAKE_RADIO_ARM_NONE) {
		return RADIANT_RADIO_EBUSY;
	}
	if (!caps_modellable()) {
		return RADIANT_RADIO_ENOTSUP;
	}
	return RADIANT_RADIO_OK_RC;
}

static uint32_t alloc_op(void)
{
	g.next_op++;
	if (g.next_op == 0u) {
		/* 0 is "no operation" in every event comparison the core will
		 * write, so it is never handed out. */
		g.next_op = 1u;
	}
	return g.next_op;
}

int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op)
{
	struct fake_radio_arm *rec;
	int32_t rec_idx;
	int rc;

	isr_tick();

	rec = new_arm_record(FAKE_RADIO_ARM_TX, &rec_idx);

	if (req == NULL || op == NULL) {
		rec->rc = RADIANT_RADIO_EINVAL;
		count_rc(rec->rc);
		return rec->rc;
	}

	rec->rf_index = req->rf_index;
	rec->power = req->power;
	rec->t_sync_at = req->t_sync_at;
	if (req->fmt != NULL) {
		rec->fmt = *req->fmt;
	}
	rec->body_len = req->body_len;
	if (req->body != NULL && req->body_len > 0u &&
	    req->body_len <= RADIANT_RADIO_BODY_MAX) {
		memcpy(rec->body, req->body, req->body_len);
	}

	rc = arm_preflight();
	if (rc == RADIANT_RADIO_OK_RC) {
		rc = check_fmt(req->fmt);
	}
	if (rc == RADIANT_RADIO_OK_RC && req->rf_index > RADIANT_RF_INDEX_MAX) {
		rc = RADIANT_RADIO_EINVAL;
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		if (req->body == NULL || req->body_len == 0u ||
		    req->body_len > req->fmt->max_body_len ||
		    req->body_len > RADIANT_RADIO_BODY_MAX) {
			rc = RADIANT_RADIO_EINVAL;
		}
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		/* The body the core hands over must already agree with the
		 * format's own length rule. A backend cannot fix this up: it
		 * DMAs the bytes it was given. */
		if (req->fmt->len_mode == RADIANT_LEN_FIXED) {
			if (req->body_len != req->fmt->body_len) {
				rc = RADIANT_RADIO_EINVAL;
			}
		} else {
			int32_t declared;

			if (req->fmt->len_offset >= req->body_len) {
				rc = RADIANT_RADIO_EINVAL;
			} else {
				declared =
					(int32_t)req->body[req->fmt->len_offset] +
					(int32_t)req->fmt->len_bias;
				if (declared != (int32_t)req->body_len) {
					rc = RADIANT_RADIO_EINVAL;
				}
			}
		}
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		if (req->t_sync_at == RADIANT_TIME_NEVER) {
			rc = RADIANT_RADIO_EINVAL;
		} else if (too_late(req->t_sync_at)) {
			/* A backend never silently transmits late: a late
			 * master frame lands in the next slot and is worse
			 * than no frame at all. */
			rc = RADIANT_RADIO_ETIME;
		}
	}

	rec->rc = rc;
	count_rc(rc);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}

	rec->power_dbm_eff = req->power.use_raw ? req->power.dbm
						: clamp_dbm(req->power.dbm);
	if (req->fmt->addr_len > g.caps.addr_len_hw_max) {
		g.stats.arms_addr_len_over_hw++;
	}

	clear_cur();
	g.cur.kind = FAKE_RADIO_ARM_TX;
	g.cur.op = alloc_op();
	g.cur.arm_idx = rec_idx;
	g.cur.fmt = *req->fmt;
	g.cur.t_sync_req = req->t_sync_at;
	g.cur.t_fire = req->t_sync_at;
	if (g.tx_offset_us > 0) {
		g.cur.t_fire += (radiant_time_t)g.tx_offset_us;
	} else if (g.tx_offset_us < 0) {
		radiant_time_t back = (radiant_time_t)(-(int64_t)g.tx_offset_us);

		g.cur.t_fire = (g.cur.t_fire > g.now + back) ? g.cur.t_fire - back
							     : g.now;
	}

	take_pending_force();

	rec->op = g.cur.op;
	*op = g.cur.op;
	g.stats.arms_tx++;
	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_rx(const struct radiant_rx_req *req, uint32_t *op)
{
	struct fake_radio_arm *rec;
	int32_t rec_idx;
	int rc;
	uint8_t i;

	isr_tick();

	rec = new_arm_record(FAKE_RADIO_ARM_RX, &rec_idx);

	if (req == NULL || op == NULL) {
		rec->rc = RADIANT_RADIO_EINVAL;
		count_rc(rec->rc);
		return rec->rc;
	}

	rec->rf_index = req->rf_index;
	rec->t_open = req->t_open;
	rec->t_close = req->t_close;
	rec->flags = req->flags;
	rec->n_filters = req->n_filters;
	if (req->fmt != NULL) {
		rec->fmt = *req->fmt;
	}
	if (req->filters != NULL && req->n_filters > 0u &&
	    req->n_filters <= FAKE_RADIO_MAX_FILTERS) {
		memcpy(rec->filters, req->filters,
		       sizeof(struct radiant_rx_filter) * req->n_filters);
	}

	rc = arm_preflight();
	if (rc == RADIANT_RADIO_OK_RC) {
		rc = check_fmt(req->fmt);
	}
	if (rc == RADIANT_RADIO_OK_RC && req->rf_index > RADIANT_RF_INDEX_MAX) {
		rc = RADIANT_RADIO_EINVAL;
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		/* The capability gate. A scheduler that hardcoded 8 fails here
		 * the moment a suite sets caps.max_filters = 2, which is the
		 * only way anyone finds out before an EFR32 exists. */
		if (req->filters == NULL || req->n_filters == 0u ||
		    req->n_filters > g.caps.max_filters ||
		    req->n_filters > FAKE_RADIO_MAX_FILTERS) {
			rc = RADIANT_RADIO_EINVAL;
		}
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		for (i = 0u; i < req->n_filters; i++) {
			if (req->filters[i].addr_len != req->fmt->addr_len) {
				rc = RADIANT_RADIO_EINVAL;
				break;
			}
		}
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		const uint32_t known = RADIANT_RX_STOP_ON_FIRST |
				       RADIANT_RX_REPORT_CRC_FAIL;

		if ((req->flags & ~known) != 0u) {
			rc = RADIANT_RADIO_EINVAL;
		} else if ((req->flags & RADIANT_RX_STOP_ON_FIRST) != 0u &&
			   req->n_filters > 1u) {
			/* The HAL says STOP_ON_FIRST must not be set on a
			 * merged or search window, where more than one master
			 * may transmit inside one window. More than one filter
			 * is what a merged or search window is. */
			rc = RADIANT_RADIO_EINVAL;
		}
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		if (req->t_open == RADIANT_TIME_NEVER ||
		    req->t_close == RADIANT_TIME_NEVER) {
			/* RADIANT_TIME_NEVER is never a valid arm time, so an
			 * open-ended window is not expressible: background
			 * scan re-arms bounded windows. */
			rc = RADIANT_RADIO_EINVAL;
		} else if (req->t_close < req->t_open) {
			rc = RADIANT_RADIO_EINVAL;
		} else if (too_late(req->t_open)) {
			rc = RADIANT_RADIO_ETIME;
		}
	}

	rec->rc = rc;
	count_rc(rc);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}

	if (req->fmt->addr_len > g.caps.addr_len_hw_max) {
		g.stats.arms_addr_len_over_hw++;
	}

	clear_cur();
	g.cur.kind = FAKE_RADIO_ARM_RX;
	g.cur.op = alloc_op();
	g.cur.arm_idx = rec_idx;
	g.cur.fmt = *req->fmt;
	g.cur.n_filters = req->n_filters;
	memcpy(g.cur.filters, req->filters,
	       sizeof(struct radiant_rx_filter) * req->n_filters);
	g.cur.flags = req->flags;
	g.cur.t_open = req->t_open;
	g.cur.t_close = req->t_close;

	take_pending_force();

	rec->op = g.cur.op;
	*op = g.cur.op;
	g.stats.arms_rx++;
	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_abort(void)
{
	isr_tick();

	if (g.lc == LC_DOWN) {
		return RADIANT_RADIO_ESTATE;
	}
	if (g.cur.kind == FAKE_RADIO_ARM_NONE) {
		return RADIANT_RADIO_OK_RC;
	}

	if (g.in_cb) {
		/*
		 * A callback may abort, but callbacks never nest. Free the slot
		 * immediately - so the caller does observe it as free, and may
		 * re-arm before returning - and deliver the ABORTED terminal
		 * the moment the current callback returns. The core therefore
		 * sees a terminal event for an operation it has already
		 * replaced, which is exactly the case the op id exists to make
		 * unambiguous.
		 */
		mark_terminal(g.cur.arm_idx, RADIANT_RADIO_STATUS_ABORTED);
		g.defer_abort_op = g.cur.op;
		g.defer_abort_kind = g.cur.kind;
		clear_cur();
		return RADIANT_RADIO_OK_RC;
	}

	end_op(RADIANT_RADIO_STATUS_ABORTED, 0u);
	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * Fault injection
 * ---------------------------------------------------------------------------
 */

void fake_radio_force_next_arm(int rc)
{
	g.force_arm_rc = rc;
	g.force_arm_set = (rc != RADIANT_RADIO_OK_RC);
}

void fake_radio_force_next_terminal(enum radiant_radio_status status)
{
	/*
	 * Applied to the operation that is armed next, and only to its natural
	 * terminal event (window close or transmit completion). An operation
	 * ended by RADIANT_RX_STOP_ON_FIRST or by abort keeps the status that
	 * genuinely describes what happened.
	 */
	if (g.cur.kind != FAKE_RADIO_ARM_NONE) {
		g.cur.force_terminal_set = true;
		g.cur.force_terminal = status;
		g.pending_force_set = false;
		return;
	}
	/* clear_cur() wipes g.cur on the way into the next arm call, so the
	 * request waits in its own field until there is an operation to put it
	 * on. */
	g.pending_force_set = true;
	g.pending_force = status;
}

/* Called by both arm paths once the new operation is installed. */
static void take_pending_force(void)
{
	if (!g.pending_force_set) {
		return;
	}
	g.cur.force_terminal_set = true;
	g.cur.force_terminal = g.pending_force;
	g.pending_force_set = false;
}

void fake_radio_set_tx_offset_us(int32_t us)
{
	g.tx_offset_us = us;
}

int fake_radio_inject_late_rx(uint32_t op, const struct fake_radio_air *f,
			      uint8_t filter_index)
{
	const uint8_t *body;
	uint8_t body_len;

	if (reject_from_callback()) {
		return RADIANT_RADIO_ESTATE;
	}
	if (f == NULL || f->len == 0u) {
		return RADIANT_RADIO_EINVAL;
	}
	body = f->bytes;
	body_len = f->len;
	if (body_len > RADIANT_RADIO_BODY_MAX) {
		body_len = RADIANT_RADIO_BODY_MAX;
	}
	deliver_rx(op,
		   f->crc_bad ? RADIANT_RADIO_STATUS_CRC_FAIL : RADIANT_RADIO_STATUS_OK,
		   false, f->t_sync, filter_index, body, body_len,
		   f->rssi_set ? f->rssi_dbm : g.default_rssi, true);
	return RADIANT_RADIO_OK_RC;
}

int fake_radio_inject_late_tx(uint32_t op, enum radiant_radio_status status)
{
	if (reject_from_callback()) {
		return RADIANT_RADIO_ESTATE;
	}
	deliver_tx(op, status, g.now, true);
	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * Idle
 * ---------------------------------------------------------------------------
 */

const char *fake_radio_busy_reason(void)
{
	if (g.cur.kind == FAKE_RADIO_ARM_TX) {
		return "a transmit is still armed";
	}
	if (g.cur.kind == FAKE_RADIO_ARM_RX) {
		return "a receive window is still armed";
	}
	if (g.defer_abort_op != 0u) {
		return "an aborted operation's terminal event is still pending";
	}
	return NULL;
}

bool fake_radio_is_idle(void)
{
	return fake_radio_busy_reason() == NULL;
}

/* ---------------------------------------------------------------------------
 * Reset
 * ---------------------------------------------------------------------------
 */

void fake_radio_reset(void)
{
	if (g.in_cb) {
		record_viol(FAKE_RADIO_VIOL_ISR_HARNESS,
			    "fake_radio_reset() from a callback");
		return;
	}
	memset(&g, 0, sizeof(g));
	g.lc = LC_DOWN;
	g.cbs = fake_cbs_none;
	g.now = FAKE_RADIO_T_ORIGIN;
	g.next_op = 0u;
	g.isr_budget = 16u;
	g.default_rssi = FAKE_RADIO_RSSI_BENCH_DBM;
	clear_cur();
	fake_radio_caps_preset_nrf();
}

void fake_radio_bring_up(void)
{
	fake_radio_reset();
	(void)radiant_radio_init(&fake_cbs_none, NULL);
	(void)radiant_radio_enable();
}
