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

bool radiant_ble_pairing_window_allows(uint64_t opened_ms, uint32_t len_ms,
				       uint64_t now_ms)
{
	if (len_ms == 0u || now_ms < opened_ms) {
		return false;
	}
	return now_ms - opened_ms < (uint64_t)len_ms;
}

/* ── Bond LRU ─────────────────────────────────────────────────────────────── */

static int lru_find(const struct radiant_ble_lru_entry *t, size_t n,
		    const uint8_t addr[RADIANT_BLE_ADDR_BYTES])
{
	for (size_t i = 0; i < n; i++) {
		if (t[i].seq != 0u &&
		    memcmp(t[i].addr, addr, RADIANT_BLE_ADDR_BYTES) == 0) {
			return (int)i;
		}
	}
	return -1;
}

void radiant_ble_lru_touch(struct radiant_ble_lru_entry *t, size_t n,
			   const uint8_t addr[RADIANT_BLE_ADDR_BYTES],
			   uint32_t *next_seq)
{
	int i;

	if (t == NULL || n == 0u || addr == NULL || next_seq == NULL) {
		return;
	}
	i = lru_find(t, n, addr);
	if (i < 0) {
		/* An empty slot (seq 0 sorts first), else the least recent. */
		size_t best = 0u;

		for (size_t k = 1; k < n; k++) {
			if (t[k].seq < t[best].seq) {
				best = k;
			}
		}
		i = (int)best;
		memcpy(t[i].addr, addr, RADIANT_BLE_ADDR_BYTES);
	}
	if (*next_seq == 0xFFFFFFFFu) {
		/* ~4 billion encrypted connections. Halve rather than wrap:
		 * the order survives (ties at worst) and no entry becomes 0. */
		uint32_t hi = 0u;

		for (size_t k = 0; k < n; k++) {
			if (t[k].seq != 0u) {
				t[k].seq = t[k].seq / 2u + 1u;
				if (t[k].seq > hi) {
					hi = t[k].seq;
				}
			}
		}
		*next_seq = hi;
	}
	t[i].seq = ++(*next_seq);
}

void radiant_ble_lru_forget(struct radiant_ble_lru_entry *t, size_t n,
			    const uint8_t addr[RADIANT_BLE_ADDR_BYTES])
{
	int i;

	if (t == NULL || addr == NULL) {
		return;
	}
	i = lru_find(t, n, addr);
	if (i >= 0) {
		memset(&t[i], 0, sizeof(t[i]));
	}
}

bool radiant_ble_lru_rekey(struct radiant_ble_lru_entry *t, size_t n,
			   const uint8_t from[RADIANT_BLE_ADDR_BYTES],
			   const uint8_t to[RADIANT_BLE_ADDR_BYTES])
{
	int f;
	int d;

	if (t == NULL || from == NULL || to == NULL ||
	    memcmp(from, to, RADIANT_BLE_ADDR_BYTES) == 0) {
		return false;
	}
	f = lru_find(t, n, from);
	if (f < 0) {
		return false;
	}
	d = lru_find(t, n, to);
	if (d >= 0) {
		/* Both exist: one peer, keep its most recent use. */
		if (t[f].seq > t[d].seq) {
			t[d].seq = t[f].seq;
		}
		memset(&t[f], 0, sizeof(t[f]));
	} else {
		memcpy(t[f].addr, to, RADIANT_BLE_ADDR_BYTES);
	}
	return true;
}

int radiant_ble_lru_pick_victim(const struct radiant_ble_lru_entry *t, size_t n,
				const uint8_t (*bonds)[RADIANT_BLE_ADDR_BYTES],
				const bool *connected, size_t nb)
{
	int      victim = -1;
	uint32_t victim_seq = 0u;

	if (bonds == NULL || connected == NULL) {
		return -1;
	}
	for (size_t b = 0; b < nb; b++) {
		int      e;
		uint32_t seq;

		if (connected[b]) {
			continue;
		}
		e = t == NULL ? -1 : lru_find(t, n, bonds[b]);
		seq = e < 0 ? 0u : t[e].seq;
		if (victim < 0 || seq < victim_seq) {
			victim = (int)b;
			victim_seq = seq;
		}
	}
	return victim;
}

int radiant_ble_lru_encode(const struct radiant_ble_lru_entry *t, size_t n,
			   uint8_t *out, size_t cap)
{
	size_t len = 1u;

	if (t == NULL || out == NULL || cap < 1u) {
		return -1;
	}
	out[0] = RADIANT_BLE_LRU_FORMAT_V1;
	for (size_t i = 0; i < n; i++) {
		if (t[i].seq == 0u) {
			continue;
		}
		if (len + RADIANT_BLE_LRU_ENTRY_BYTES > cap) {
			return -1;
		}
		memcpy(out + len, t[i].addr, RADIANT_BLE_ADDR_BYTES);
		out[len + 7u] = (uint8_t)t[i].seq;
		out[len + 8u] = (uint8_t)(t[i].seq >> 8);
		out[len + 9u] = (uint8_t)(t[i].seq >> 16);
		out[len + 10u] = (uint8_t)(t[i].seq >> 24);
		len += RADIANT_BLE_LRU_ENTRY_BYTES;
	}
	return (int)len;
}

int radiant_ble_lru_decode(const uint8_t *in, size_t len,
			   struct radiant_ble_lru_entry *t, size_t n,
			   uint32_t *max_seq)
{
	size_t   count;
	uint32_t hi = 0u;

	if (in == NULL || t == NULL || len < 1u ||
	    in[0] != RADIANT_BLE_LRU_FORMAT_V1 ||
	    (len - 1u) % RADIANT_BLE_LRU_ENTRY_BYTES != 0u) {
		return -1;
	}
	count = (len - 1u) / RADIANT_BLE_LRU_ENTRY_BYTES;
	if (count > n) {
		/* Written with a larger CONFIG_BT_MAX_PAIRED: keep the first n. */
		count = n;
	}
	memset(t, 0, n * sizeof(*t));
	for (size_t i = 0; i < count; i++) {
		const uint8_t *p = in + 1u + i * RADIANT_BLE_LRU_ENTRY_BYTES;

		memcpy(t[i].addr, p, RADIANT_BLE_ADDR_BYTES);
		t[i].seq = (uint32_t)p[7] | ((uint32_t)p[8] << 8) |
			   ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 24);
		if (t[i].seq > hi) {
			hi = t[i].seq;
		}
	}
	if (max_seq != NULL) {
		*max_seq = hi;
	}
	return (int)count;
}
