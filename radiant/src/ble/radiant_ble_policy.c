/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_policy.c - the kernel-free decisions of the shared BLE layer:
 * advertising restart and backoff, the connection-parameter rule, the
 * per-unit name and the pairing window. No Zephyr header beyond libc, so
 * radiant/tests/ble compiles this file unmodified on native_sim; the glue in
 * radiant_ble_{adv,conn,pairing}.c only feeds it events.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <radiant/radiant_ble_adv.h>
#include <radiant/radiant_ble_conn.h>
#include <radiant/radiant_ble_pairing.h>

/* ── Advertising ──────────────────────────────────────────────────────────── */

void radiant_ble_adv_state_init(struct radiant_ble_adv_state *s,
				uint8_t max_conns, uint32_t min_ms,
				uint32_t max_ms, uint32_t watchdog_ms)
{
	memset(s, 0, sizeof(*s));
	s->max_conns = max_conns == 0u ? 1u : max_conns;
	s->min_ms = min_ms == 0u ? 1u : min_ms;
	s->max_ms = max_ms < s->min_ms ? s->min_ms : max_ms;
	s->watchdog_ms = watchdog_ms;
}

bool radiant_ble_adv_may_start(const struct radiant_ble_adv_state *s)
{
	return s->want && s->conns < s->max_conns;
}

bool radiant_ble_adv_begin_attempt(struct radiant_ble_adv_state *s)
{
	if (radiant_ble_adv_may_start(s)) {
		return true;
	}
	s->backoff_ms = 0u;
	return false;
}

uint32_t radiant_ble_adv_next_backoff(uint32_t prev_ms, uint32_t min_ms,
				      uint32_t max_ms)
{
	if (prev_ms == 0u) {
		return min_ms;
	}
	if (prev_ms >= max_ms / 2u) {
		return max_ms;
	}
	return prev_ms * 2u;
}

uint32_t radiant_ble_adv_note_result(struct radiant_ble_adv_state *s, int err)
{
	if (err == 0 || err == -EALREADY) {
		s->active = true;
		s->failures = 0u;
		s->backoff_ms = 0u;
		return 0u;
	}
	s->active = false;
	if (!s->want) {
		s->backoff_ms = 0u;
		return 0u;
	}
	s->failures++;
	s->backoff_ms = radiant_ble_adv_next_backoff(s->backoff_ms, s->min_ms,
						     s->max_ms);
	return s->backoff_ms;
}

void radiant_ble_adv_note_connected(struct radiant_ble_adv_state *s, bool ok)
{
	/* A connectable advertiser ends when a connection is established, and
	 * also when one fails to establish after its CONNECT_IND. */
	s->active = false;
	if (ok && s->conns < 0xFFu) {
		s->conns++;
	}
}

void radiant_ble_adv_note_disconnected(struct radiant_ble_adv_state *s)
{
	if (s->conns > 0u) {
		s->conns--;
	}
}

bool radiant_ble_adv_note_recycled(struct radiant_ble_adv_state *s)
{
	/* Whatever we believed, a freed object is the moment a start can
	 * succeed; a start of a running advertiser costs one -EALREADY. A
	 * retry already pending will make the attempt itself. */
	s->active = false;
	return radiant_ble_adv_may_start(s) && s->backoff_ms == 0u;
}

bool radiant_ble_adv_watchdog_due(struct radiant_ble_adv_state *s,
				  uint32_t now_ms)
{
	if (s->watchdog_ms == 0u || !radiant_ble_adv_may_start(s) ||
	    s->backoff_ms != 0u) {
		return false;
	}
	if ((uint32_t)(now_ms - s->last_probe_ms) < s->watchdog_ms) {
		return false;
	}
	s->last_probe_ms = now_ms;
	return true;
}

int radiant_ble_name_with_devnum(char *out, size_t cap, const char *base,
				 uint16_t devnum)
{
	size_t base_len;
	int    n;

	if (out == NULL || cap == 0u || base == NULL) {
		return -1;
	}
	base_len = strlen(base);
	n = snprintf(out, cap, "%s %u", base, (unsigned int)devnum);
	if (n < 0 || (size_t)n >= cap) {
		if (base_len < cap) {
			memcpy(out, base, base_len + 1u);
		} else {
			out[0] = '\0';
		}
		return -1;
	}
	return n;
}

/* ── Connection parameters ────────────────────────────────────────────────── */

bool radiant_ble_conn_param_valid(const struct radiant_ble_conn_param *p)
{
	return (uint32_t)p->timeout * 4u >
	       ((uint32_t)p->latency + 1u) * (uint32_t)p->interval_max;
}

bool radiant_ble_conn_param_apply(struct radiant_ble_conn_param *p,
				  uint16_t min_interval, uint16_t min_timeout)
{
	bool compliant = p->interval_min >= min_interval &&
			 p->interval_max >= min_interval &&
			 p->latency == 0u &&
			 p->timeout >= min_timeout;

	if (compliant && radiant_ble_conn_param_valid(p)) {
		return false;
	}

	p->interval_min = min_interval;
	p->interval_max = min_interval;
	p->latency = 0u;
	if (p->timeout < min_timeout) {
		p->timeout = min_timeout;
	}
	return true;
}

/* ── Pairing window ───────────────────────────────────────────────────────── */

bool radiant_ble_pairing_window_allows(uint32_t opened_ms, uint32_t len_ms,
				       uint32_t now_ms)
{
	if (len_ms == 0u) {
		return false;
	}
	return (uint32_t)(now_ms - opened_ms) < len_ms;
}
