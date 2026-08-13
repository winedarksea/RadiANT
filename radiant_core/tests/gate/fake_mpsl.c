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

static struct {
	mpsl_timeslot_callback_t cb;
	mpsl_timeslot_session_id_t id;
	bool     open;
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
	if (cb == NULL || p_session_id == NULL) {
		return -NRF_EINVAL;
	}
	m.cb = cb;
	m.open = true;
	m.opens++;
	/* Not zero, so a gate that forgot to keep the id and passed a
	 * zero-initialised variable back is distinguishable from one that
	 * kept it. */
	m.id = 3u;
	*p_session_id = m.id;
	return 0;
}

int32_t mpsl_timeslot_session_close(mpsl_timeslot_session_id_t session_id)
{
	if (!m.open || session_id != m.id) {
		return -NRF_EINVAL;
	}
	m.open = false;
	m.closes++;
	return 0;
}

int32_t mpsl_timeslot_request(mpsl_timeslot_session_id_t      session_id,
			      mpsl_timeslot_request_t const *p_request)
{
	if (!m.open || session_id != m.id || p_request == NULL) {
		return -NRF_EINVAL;
	}
	m.requests++;
	m.last_req = *p_request;

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
		(void)fake_mpsl_signal(m.inline_signal);
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * Delivering a signal
 * ---------------------------------------------------------------------------
 */

uint8_t fake_mpsl_signal(uint32_t signal)
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

	p = m.cb(m.id, signal);
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
