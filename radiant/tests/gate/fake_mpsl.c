/* SPDX-License-Identifier: Apache-2.0 */
/* fake_mpsl.c - the arbiter and the backend, as two dozen lines each. See
 * fake_mpsl.h. */

#include <string.h>

#include <zephyr/sys/util.h>

#include <mpsl_timeslot.h>
#include <nrf_errno.h>
#include <hal/nrf_timer.h>

#include "fake_mpsl.h"
#include "radiant_radio_nrf_gate.h"

/* MPSL_TIMER0. Defined here rather than in the header so there is exactly one
 * of it, the same way MPSL owns exactly one. */
fake_timer_t fake_mpsl_timer0;

#define FAKE_SESS_MAX 2u

static struct {
	mpsl_timeslot_callback_t cb;
	mpsl_timeslot_session_id_t id;
	bool     open;
	/* ADR 0018 (d2): up to FAKE_SESS_MAX sessions, the first being the
	 * one every single-session test talks to. max_sessions 0 means 1, so a
	 * test that never asks for two sees exactly the old fake. */
	uint32_t max_sessions;
	bool     s_open[FAKE_SESS_MAX];
	uint32_t s_requests[FAKE_SESS_MAX];
	mpsl_timeslot_request_t s_last_req[FAKE_SESS_MAX];
	uint32_t target;      /* which session fake_mpsl_signal() talks to */
	uint32_t last_sess;   /* which session the last request was for */
	uint32_t opens;
	uint32_t closes;
	uint32_t requests;
	uint32_t accepted;
	mpsl_timeslot_request_t last_req;

	int32_t  force_rc;
	uint32_t force_left;

	uint32_t inline_signal;
	uint32_t inline_left;

	bool     in_timeslot;
	uint32_t last_extend_us;

	uint32_t viol_low_prio_action;
	uint32_t viol_end_when_ended;
	uint32_t viol_action_no_slot;
} m;

static struct fake_backend_counts b;
static bool release_in_grant;
static bool release_in_radio_irq;

/* ---------------------------------------------------------------------------
 * The MPSL entry points
 * ---------------------------------------------------------------------------
 */

int32_t mpsl_timeslot_session_open(mpsl_timeslot_callback_t cb,
				   mpsl_timeslot_session_id_t *p_session_id)
{
	uint32_t max = (m.max_sessions == 0u) ? 1u : m.max_sessions;
	uint32_t i;

	if (cb == NULL || p_session_id == NULL) {
		return -NRF_EINVAL;
	}
	for (i = 0u; i < max && i < FAKE_SESS_MAX; i++) {
		if (!m.s_open[i]) {
			break;
		}
	}
	if (i >= max || i >= FAKE_SESS_MAX) {
		/* mpsl_timeslot.h: -NRF_ENOMEM, all sessions are open. */
		return -NRF_ENOMEM;
	}
	m.cb = cb;
	m.s_open[i] = true;
	m.open = m.s_open[0];
	m.opens++;
	/* Not zero, so a gate that forgot to keep the id and passed a
	 * zero-initialised variable back is distinguishable from one that
	 * kept it. */
	m.id = 3u;
	*p_session_id = (mpsl_timeslot_session_id_t)(3u + i);
	return 0;
}

static int32_t sess_index(mpsl_timeslot_session_id_t session_id)
{
	uint32_t i = (uint32_t)session_id - 3u;

	if ((uint32_t)session_id < 3u || i >= FAKE_SESS_MAX || !m.s_open[i]) {
		return -1;
	}
	return (int32_t)i;
}

int32_t mpsl_timeslot_session_close(mpsl_timeslot_session_id_t session_id)
{
	int32_t i = sess_index(session_id);

	if (i < 0) {
		return -NRF_EINVAL;
	}
	m.s_open[i] = false;
	m.open = m.s_open[0];
	m.closes++;
	return 0;
}

int32_t mpsl_timeslot_request(mpsl_timeslot_session_id_t      session_id,
			      mpsl_timeslot_request_t const *p_request)
{
	int32_t i = sess_index(session_id);

	if (i < 0 || p_request == NULL) {
		return -NRF_EINVAL;
	}
	m.requests++;
	m.last_req = *p_request;
	m.s_requests[i]++;
	m.s_last_req[i] = *p_request;
	m.last_sess = (uint32_t)i;

	if (m.force_left > 0u) {
		m.force_left--;
		return m.force_rc;
	}
	m.accepted++;
	/* Answer on the caller's stack before acceptance is reported; see
	 * fake_mpsl_answer_inline(). Reuses fake_mpsl_signal() whole so the
	 * rule-violation counters still watch this delivery like any other. */
	if (m.inline_left > 0u) {
		m.inline_left--;
		(void)fake_mpsl_signal_s((uint32_t)i, m.inline_signal);
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * Delivering a signal
 * ---------------------------------------------------------------------------
 */

uint8_t fake_mpsl_signal(uint32_t signal)
{
	return fake_mpsl_signal_s(m.target, signal);
}

uint8_t fake_mpsl_signal_s(uint32_t sess, uint32_t signal)
{
	mpsl_timeslot_signal_return_param_t *p;
	uint8_t action;

	if (m.cb == NULL) {
		return MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	}

	if (signal == MPSL_TIMESLOT_SIGNAL_START) {
		/* MPSL_TIMER0 is lent for the duration of a grant and its count
		 * is the grant's own, from zero. */
		fake_mpsl_timer0.counter = 0u;
		m.in_timeslot = true;
	}

	p = m.cb((mpsl_timeslot_session_id_t)(3u + sess), signal);
	if (p == NULL) {
		/* mpsl_timeslot.h requires a pointer that outlives the
		 * callback; a null one is a fault in its own right. */
		m.viol_action_no_slot++;
		return MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	}
	action = p->callback_action;
	if (action == MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND) {
		m.last_extend_us = p->params.extend.length_us;
	}

	if (action != MPSL_TIMESLOT_SIGNAL_ACTION_NONE) {
		/* Rule 3 of the gate's header: the low-priority signals may not
		 * carry an action. */
		if (signal >= MPSL_TIMESLOT_SIGNAL_BLOCKED) {
			m.viol_low_prio_action++;
		}
		if (!m.in_timeslot) {
			m.viol_action_no_slot++;
			if (action == MPSL_TIMESLOT_SIGNAL_ACTION_END) {
				m.viol_end_when_ended++;
			}
		}
	}

	if (action == MPSL_TIMESLOT_SIGNAL_ACTION_END) {
		m.in_timeslot = false;
	}
	return action;
}

void fake_mpsl_reset(void)
{
	memset(&m, 0, sizeof(m));
	memset(&fake_mpsl_timer0, 0, sizeof(fake_mpsl_timer0));
}

void fake_mpsl_force_request_rc(int32_t rc, uint32_t count)
{
	m.force_rc = rc;
	m.force_left = count;
}

void fake_mpsl_answer_inline(uint32_t signal, uint32_t count)
{
	m.inline_signal = signal;
	m.inline_left = count;
}

bool     fake_mpsl_is_open(void)   { return m.open; }
uint32_t fake_mpsl_opens(void)     { return m.opens; }
uint32_t fake_mpsl_closes(void)    { return m.closes; }
uint32_t fake_mpsl_requests(void)  { return m.requests; }
uint32_t fake_mpsl_accepted(void)  { return m.accepted; }
bool     fake_mpsl_in_timeslot(void) { return m.in_timeslot; }
uint32_t fake_mpsl_last_extend_us(void) { return m.last_extend_us; }

const mpsl_timeslot_request_t *fake_mpsl_last_request(void)
{
	return &m.last_req;
}

void fake_mpsl_set_max_sessions(uint32_t n) { m.max_sessions = n; }
void fake_mpsl_target(uint32_t sess) { m.target = sess; }
uint32_t fake_mpsl_last_session(void) { return m.last_sess; }
bool fake_mpsl_is_open_s(uint32_t sess)
{
	return sess < FAKE_SESS_MAX && m.s_open[sess];
}
uint32_t fake_mpsl_requests_s(uint32_t sess)
{
	return (sess < FAKE_SESS_MAX) ? m.s_requests[sess] : 0u;
}
const mpsl_timeslot_request_t *fake_mpsl_last_request_s(uint32_t sess)
{
	return &m.s_last_req[(sess < FAKE_SESS_MAX) ? sess : 0u];
}

uint32_t fake_mpsl_viol_low_prio_action(void) { return m.viol_low_prio_action; }
uint32_t fake_mpsl_viol_end_when_ended(void)  { return m.viol_end_when_ended; }
uint32_t fake_mpsl_viol_action_no_slot(void)  { return m.viol_action_no_slot; }

/* ---------------------------------------------------------------------------
 * The backend side
 * ---------------------------------------------------------------------------
 */

void fake_backend_reset(void)
{
	memset(&b, 0, sizeof(b));
	release_in_grant = false;
	release_in_radio_irq = false;
}

const struct fake_backend_counts *fake_backend(void) { return &b; }

void fake_backend_release_in_grant(bool on)     { release_in_grant = on; }
void fake_backend_release_in_radio_irq(bool on) { release_in_radio_irq = on; }

void radiant_nrf_gate_on_grant(void)
{
	b.on_grant++;
	if (release_in_grant) {
		gate_release();
	}
}

void radiant_nrf_gate_on_denied(void)
{
	b.on_denied++;
}

void radiant_nrf_gate_finish_failed(void)
{
	b.finish_failed++;
}

void radiant_nrf_gate_on_radio_irq(void)
{
	b.on_radio_irq++;
	if (release_in_radio_irq) {
		gate_release();
	}
}

void radiant_nrf_gate_on_grant_end(void)
{
	b.on_grant_end++;
}

#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
void radiant_nrf_gate_on_booking_due(void)
{
	b.booking_due++;
}

void radiant_nrf_gate_set_elastic_lead(uint32_t us)
{
	b.elastic_lead_us = us;
	b.elastic_lead_sets++;
}
#endif
