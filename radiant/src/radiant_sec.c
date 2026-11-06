/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec.c - per-channel policy for the two payload transforms.
 *
 * docs/radiant-security.md is normative. This is the policy layer of the
 * primitive/policy/host cut: decides what happens to which bytes and when,
 * computes nothing itself.
 *
 * THIS FILE CONTAINS NO CALL TO radiant_sec_aes_ecb(). Level 1 is for
 * backends; Level 2 (radiant_sec_ctr_xor(), radiant_sec_cmac*(), and the
 * portable helpers over them) is for policy. The `zero-cost` CI job greps for
 * that call, since a seam whose policy layer reaches past it is only a seam
 * in the prototypes.
 *
 * Context: TX transform runs in thread context under api_lock, immediately
 * before the frame is built - the TX body is DMA'd and armed arbitrarily
 * early, so ciphertext must be final before radiant_sched_request_tx(). RX
 * ingest is ISR context and does no crypto (classifies and copies 8 bytes);
 * RX crypto runs in the pump, on the event thread, regardless of
 * caps.block_us.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <radiant/radiant_channel.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_sec.h>

LOG_MODULE_DECLARE(radiant, CONFIG_RADIANT_LOG_LEVEL);

#define SEC_PAGE_BYTES  8u
#define SEC_MAX_CH      CONFIG_RADIANT_SEC_MAX_CHANNELS

/* The ISR-to-pump queue, sized so a full window of every secured channel can
 * be in flight at once (the deepest backlog possible). Overflow drops the
 * OLDEST entry: a dropped packet breaks its window either way, and dropping
 * the newest would also break every window after it. */
#define SEC_QUEUE_DEPTH (SEC_MAX_CH * RADIANT_SEC_W_MAX)

struct sec_pkt {
	uint64_t t_sync;
	uint8_t  pay[SEC_PAGE_BYTES];
	uint8_t  len;
	uint8_t  ch;
};

struct sec_ctx {
	bool     used;
	uint8_t  ch;

	uint8_t  switches;
	uint8_t  w;
	uint8_t  page_lo;
	uint8_t  page_hi;

	uint16_t period_counts;
	uint16_t devnum;        /* on air, in the nonce */
	uint16_t base_devnum;   /* provisioning time, in the KDF */

	/* D3/D17: no transform enables until the host advances the epoch after
	 * a reset - a reboot restarting the counter under an unchanged epoch
	 * is a two-time pad for X_CONF and a full replay against X_AUTH. */
	bool     epoch_set;
	/* One past the newest VERIFIED window this epoch: a counter below it is
	 * a replay (docs/radiant-security.md 7.6). 0 = nothing verified since
	 * the last reset, which is also "the anchor has not locked yet" (3.1).
	 * Raised only by a verdict - never by an unverified packet, or one
	 * forged expected+127 would blind the receiver for 32 s. */
	uint16_t rx_floor;
	uint32_t epoch;
	/* epoch_anchor error, in us, implied by rx_prev's closing packet and
	 * applied only if rx_prev verifies. rx_floor and this sit where they do
	 * because both fill alignment holes on a 32-bit target, so the fix that
	 * added them cost no RAM. */
	int32_t  rx_prev_err;
	uint64_t epoch_anchor;  /* radiant_radio_now() at us_into_epoch == 0 */

	struct radiant_sec_key k_root;
	struct radiant_sec_key k_enc;
	struct radiant_sec_key k_auth;

	/* ── TX ── */
	uint16_t tx_ctr;
	bool     tx_win_open;
	uint16_t tx_win_start;
	struct radiant_sec_cmac_ctx tx_cmac;
	/* The tag of the window that just closed, transmitted one byte per
	 * packet across the NEXT window. See the header comment on the lag. */
	uint8_t  tx_prev_tag[RADIANT_SEC_W_MAX];
	bool     tx_prev_valid;
	/* Epoch exhausted at a wrap: no further secured TX until a new epoch. */
	bool     tx_spent;

	/* ── RX ── */
	bool     rx_win_open;
	uint16_t rx_win_start;
	uint8_t  rx_absorbed;
	bool     rx_broken;
	struct radiant_sec_cmac_ctx rx_cmac;
	/* Tag bytes collected during the CURRENT window, which authenticate the
	 * PREVIOUS one. */
	uint8_t  rx_tag[RADIANT_SEC_W_MAX];
	uint8_t  rx_tag_have;
	/* The previous window: whether there WAS one, what it started at, and
	 * the tag computed for it. `seen`/`valid` are separate: a window with
	 * a hole produces no tag to compare, but the host must still be told
	 * it was unverifiable rather than silently look verified. */
	bool     rx_prev_seen;
	bool     rx_prev_valid;
	uint16_t rx_prev_start;
	uint8_t  rx_prev_tag[RADIANT_SEC_BLOCK_BYTES];

	/* The drop policy's holding buffers. The default (deliver marked
	 * unverifiable) needs none; dropping means holding a window until
	 * ITS OWN verdict is known, and that verdict exists only once the
	 * window after it has closed (its tag rides in that window). So two:
	 * hold[hold_cur] fills with the window now open, hold[hold_cur ^ 1]
	 * holds the window before it, the one whose verdict the next close
	 * computes. ONE buffer was review D1-1: it released the closing
	 * window on its predecessor's verdict, so a forged packet in any
	 * window went out VERIFIED behind a genuine one. Delivery latency
	 * under the policy is therefore W+1 to 2W packet periods, the same
	 * as detection latency (docs/radiant-security.md 7.5).
	 *
	 * hold_start is the window each buffer belongs to, checked against
	 * the verdict's window before anything is released - aligned by
	 * construction (both advance only in rx_close_window()), checked
	 * anyway because this is the last gate before VERIFIED. No hold_len:
	 * rx_process() refuses len < 8 and ingest clamps to 8, so every held
	 * packet is SEC_PAGE_BYTES. Paid by every secured channel whether or
	 * not it sets the bit - a static table can't be conditional. */
	uint8_t  hold[2][RADIANT_SEC_W_MAX][SEC_PAGE_BYTES];
	uint64_t hold_t[2][RADIANT_SEC_W_MAX];
	uint16_t hold_start[2];
	uint8_t  n_hold[2];
	uint8_t  hold_cur;

	enum radiant_sec_verdict last_verdict;
	struct radiant_sec_stats stats;
};

static struct sec_ctx sec[SEC_MAX_CH];

/* channel -> context index, 0xFF for none. Allocation is by first SET_KEY. */
static uint8_t sec_of[RADIANT_CHANNEL_COUNT];
static bool    sec_map_inited;

static struct sec_pkt sec_q[SEC_QUEUE_DEPTH];
static uint16_t       sec_q_head;   /* next to consume */
static uint16_t       sec_q_count;

/* The queue is written from the radio ISR and read from the event thread, so
 * it needs the same discipline radiant_event.c uses for its ring. A spinlock
 * rather than a mutex: one end of this is an interrupt. */
static struct k_spinlock sec_q_lock;

/* ── Small helpers ──────────────────────────────────────────────────────── */

static void sec_map_init(void)
{
	if (!sec_map_inited) {
		memset(sec_of, 0xFF, sizeof(sec_of));
		sec_map_inited = true;
	}
}

static struct sec_ctx *ctx_of(uint8_t ch)
{
	sec_map_init();
	if (ch >= RADIANT_CHANNEL_COUNT) {
		return NULL;
	}
	if (sec_of[ch] >= SEC_MAX_CH) {
		return NULL;
	}
	return &sec[sec_of[ch]];
}

/* Is this channel transforming anything at all right now? Every hot-path entry
 * point asks this first, and it is deliberately three loads and a branch. */
static bool ctx_armed(const struct sec_ctx *c)
{
	return c != NULL && c->used && c->epoch_set &&
	       (c->switches & (RADIANT_SEC_SW_CONF | RADIANT_SEC_SW_AUTH)) != 0u;
}

static bool page_secured(const struct sec_ctx *c, uint8_t page)
{
	return page >= c->page_lo && page <= c->page_hi;
}

static void stat_bump(uint32_t *counter)
{
	if (*counter != UINT32_MAX) {
		(*counter)++;
	}
}

/* A hold_start no window can have: W is 2, 4 or 8, so every window starts on
 * an even counter. A buffer marked with it can never match a verdict's window
 * and is dropped whole when flushed. */
#define HOLD_START_POISON 0xFFFFu

/*
 * Empty both drop-policy buffers, counting what they held into dropped_policy.
 * Every site that throws the RX window state away calls this, because a held
 * window is only releasable by a verdict computed under the same window state:
 * left behind across a configure, an epoch change or a new try_devnum
 * candidate, it would be judged by the next state's MAC. Counted rather than
 * silent so a host can tell "held and discarded" from "never heard" - which
 * makes every try_devnum trial under the policy a visible bump.
 */
static void hold_reset(struct sec_ctx *c)
{
	uint8_t b;
	uint8_t i;

	for (b = 0u; b < 2u; b++) {
		for (i = 0u; i < c->n_hold[b]; i++) {
			stat_bump(&c->stats.dropped_policy);
		}
		c->n_hold[b] = 0u;
	}
	c->hold_cur = 0u;
}

/* The channel period in us. 0 only for period_counts 0 (no epoch yet). */
static uint64_t period_us_of(const struct sec_ctx *c)
{
	return ((uint64_t)c->period_counts * 1000000u) / 32768u;
}

/* D4: expected packet index, from TIME rather than arrival history - a
 * receiver joining mid-epoch (the normal case) has none to count from. */
static uint32_t expected_index(const struct sec_ctx *c, uint64_t now)
{
	uint64_t period_us = period_us_of(c);
	uint64_t elapsed;

	if (period_us == 0u) {
		return 0u;
	}
	if (now <= c->epoch_anchor) {
		return 0u;
	}
	elapsed = now - c->epoch_anchor;
	return (uint32_t)(elapsed / period_us);
}

/*
 * The 16-bit counter, from the on-air byte and the expected index: pick the
 * rollover nearest what time says. `epoch_delta` reports how many counter
 * wraps that implies (a wrap advances the epoch by 1 on both sides, D14).
 * Drift budget: nearest-rollover resolution needs combined clock error below
 * +/-128 packet periods, and the anchor re-establishes on every VERIFIED
 * window (rx_close_window()), so drift only accumulates across a gap with
 * nothing verified.
 */
static uint16_t resolve_counter(uint32_t expected, uint8_t low,
				uint16_t *epoch_delta)
{
	uint32_t base = expected & ~0xFFu;
	uint32_t best = base + low;
	uint32_t candidate;
	uint32_t best_err;

	best_err = (best > expected) ? (best - expected) : (expected - best);

	if (base >= 256u) {
		candidate = base - 256u + low;
		if ((candidate > expected ? candidate - expected
					  : expected - candidate) < best_err) {
			best = candidate;
			best_err = (candidate > expected) ? candidate - expected
							  : expected - candidate;
		}
	}
	candidate = base + 256u + low;
	if ((candidate > expected ? candidate - expected
				  : expected - candidate) < best_err) {
		best = candidate;
	}

	*epoch_delta = (uint16_t)(best >> 16);
	return (uint16_t)(best & 0xFFFFu);
}

/* Derive K_enc and K_auth for the context's current epoch. */
static int derive_keys(struct sec_ctx *c)
{
	int rc;

	radiant_sec_key_destroy(&c->k_enc);
	radiant_sec_key_destroy(&c->k_auth);

	rc = radiant_sec_kdf(&c->k_root, RADIANT_SEC_LABEL_ENC, c->epoch,
			     c->base_devnum, &c->k_enc);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	return radiant_sec_kdf(&c->k_root, RADIANT_SEC_LABEL_AUTH, c->epoch,
			       c->base_devnum, &c->k_auth);
}

/* D14: a counter wrap advances the epoch by 1 on both sides and re-derives
 * keys - without it the 16-bit counter wraps at ~4.5 h at 4 Hz and reuses
 * keystream within one epoch. 65536 divides every legal W, so window
 * alignment survives the wrap untouched. */
static int advance_epoch(struct sec_ctx *c, uint16_t by)
{
	if (by == 0u) {
		return RADIANT_SEC_OK;
	}
	if (c->epoch > 0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM) {
		return RADIANT_SEC_EINVAL;
	}
	c->epoch += by;
	stat_bump(&c->stats.epoch_advances);

	/* Move the anchor with it so the phase stays continuous: 65536 packets
	 * per wrap. */
	c->epoch_anchor += (uint64_t)by * 65536u * period_us_of(c);

	return derive_keys(c);
}

/* ── Weak delivery hooks ────────────────────────────────────────────────── */

__weak void radiant_sec_deliver(uint8_t ch, const uint8_t *pay, uint8_t len,
				enum radiant_sec_verdict verdict,
				uint64_t t_sync)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(pay);
	ARG_UNUSED(len);
	ARG_UNUSED(verdict);
	ARG_UNUSED(t_sync);
}

__weak void radiant_sec_window_verdict(uint8_t ch, uint16_t window_start,
				       enum radiant_sec_verdict verdict)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(window_start);
	ARG_UNUSED(verdict);
}

/* ── Configuration ──────────────────────────────────────────────────────── */

int radiant_sec_set_key(uint8_t ch, const uint8_t *root, size_t bits,
			uint16_t base_devnum)
{
	struct sec_ctx *c;
	uint8_t         i;
	int             rc;
	bool            fresh = false;

	sec_map_init();
	if (ch >= RADIANT_CHANNEL_COUNT || root == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	c = ctx_of(ch);
	if (c == NULL) {
		fresh = true;
		/* Allocation is the first SET_KEY and nothing else - a build
		 * that never keys a channel never touches this table. */
		for (i = 0u; i < SEC_MAX_CH; i++) {
			if (!sec[i].used) {
				memset(&sec[i], 0, sizeof(sec[i]));
				sec[i].used = true;
				sec[i].ch = ch;
				sec[i].w = RADIANT_SEC_W_DEFAULT;
				sec[i].page_lo = RADIANT_SEC_PAGE_LO_DEFAULT;
				sec[i].page_hi = RADIANT_SEC_PAGE_HI_DEFAULT;
				sec_of[ch] = i;
				c = &sec[i];
				break;
			}
		}
		if (c == NULL) {
			return RADIANT_SEC_ENOTSUP;
		}
	}

	rc = radiant_sec_key_import(&c->k_root, root, bits);
	if (rc != RADIANT_SEC_OK) {
		if (fresh) {
			/* Undo the allocation: a keyless slot left behind
			 * accepted configure/set_epoch where ENOKEY is owed,
			 * and held a context forever (review 2026-10-31,
			 * D1-6(b)). */
			memset(c, 0, sizeof(*c));
			sec_of[ch] = 0xFFu;
		}
		return rc;
	}
	c->base_devnum = base_devnum;
	if (c->devnum == 0u) {
		c->devnum = base_devnum;
	}

	/* A new root invalidates the epoch gate: a fresh start under an epoch
	 * the previous key already used is exactly the state D3 forbids. */
	c->epoch_set = false;
	radiant_sec_key_destroy(&c->k_enc);
	radiant_sec_key_destroy(&c->k_auth);
	return RADIANT_SEC_OK;
}

int radiant_sec_configure(uint8_t ch, uint8_t switches, uint8_t w,
			  uint8_t page_lo, uint8_t page_hi)
{
	struct sec_ctx *c = ctx_of(ch);

	if (c == NULL) {
		/* Configuring a channel with no key is a host sequencing
		 * error, not a state to invent a context for. */
		return RADIANT_SEC_ENOKEY;
	}

	/* Descriptor encryption refused in v1: the descriptor has no counter
	 * (byte [1] is a frame index), so no nonce and nothing to reconstruct. */
	if ((switches & RADIANT_SEC_SW_DESC_CONF) != 0u) {
		return RADIANT_SEC_ENOTSUP;
	}
	if ((switches & ~(uint8_t)(RADIANT_SEC_SW_CONF | RADIANT_SEC_SW_AUTH |
				   RADIANT_SEC_SW_DROP_UNVER)) != 0u) {
		return RADIANT_SEC_EINVAL;
	}
	/* W in {2,4,8}. W must divide 256 and 65536 or the counter-derived
	 * window stops resynchronising after a lost packet, and W=1 needs a
	 * data-page layout no text defines. */
	if (w != 2u && w != 4u && w != 8u) {
		return RADIANT_SEC_EINVAL;
	}
	/* Bounded so the descriptor (0x00) and ANT+ common pages stay in the
	 * clear mechanically, and a host can't make its own node
	 * undiscoverable by accident. */
	if (page_lo < RADIANT_SEC_PAGE_LO_MIN ||
	    page_hi > RADIANT_SEC_PAGE_HI_MAX || page_hi < page_lo) {
		return RADIANT_SEC_EINVAL;
	}

	if ((switches & RADIANT_SEC_SW_CONF) != 0u &&
	    !IS_ENABLED(CONFIG_RADIANT_SEC_CONF)) {
		return RADIANT_SEC_ENOTSUP;
	}
	if ((switches & RADIANT_SEC_SW_AUTH) != 0u &&
	    !IS_ENABLED(CONFIG_RADIANT_SEC_AUTH)) {
		return RADIANT_SEC_ENOTSUP;
	}

	c->switches = switches;
	c->w = w;
	c->page_lo = page_lo;
	c->page_hi = page_hi;

	/* Any window in flight is meaningless under a new W or a new range. */
	c->rx_win_open = false;
	c->rx_prev_valid = false;
	c->rx_prev_seen = false;
	c->rx_floor = 0u;
	c->tx_win_open = false;
	c->tx_prev_valid = false;
	hold_reset(c);
	return RADIANT_SEC_OK;
}

int radiant_sec_set_epoch(uint8_t ch, uint32_t epoch, uint64_t us_into_epoch,
			  uint16_t period_counts)
{
	struct sec_ctx *c = ctx_of(ch);
	uint64_t        now;

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}
	if (period_counts == 0u) {
		return RADIANT_SEC_EINVAL;
	}
	/* Monotone - the whole of D3's enforceable half; the rest is a
	 * normative obligation on the epoch authority since nothing here
	 * survives a reset to check it. */
	if (c->epoch_set && epoch <= c->epoch) {
		return RADIANT_SEC_EINVAL;
	}
	/* Leave room for a counter wrap to advance into. */
	if (epoch > 0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM) {
		return RADIANT_SEC_EINVAL;
	}

	now = (uint64_t)radiant_radio_now();
	c->epoch = epoch;
	c->epoch_anchor = (now > us_into_epoch) ? (now - us_into_epoch) : 0u;
	c->period_counts = period_counts;
	c->epoch_set = true;

	/* Everything in flight belonged to the old epoch. */
	c->tx_ctr = 0u;
	c->tx_spent = false;
	c->tx_win_open = false;
	c->tx_prev_valid = false;
	c->rx_win_open = false;
	c->rx_prev_valid = false;
	c->rx_prev_seen = false;
	c->rx_floor = 0u;
	hold_reset(c);

	return derive_keys(c);
}

#if !defined(CONFIG_RADIANT_SEC_HAS_RNG)
/*
 * No entropy source in this build - the default, not a degraded mode: ADR
 * 0009 makes the deterministic KDF the primary pairing-scalar path so this
 * is supported rather than a hole. `out` is deliberately left untouched:
 * zeroing it would hand a caller that ignored the return code something that
 * looks like data.
 */
int radiant_sec_rng(uint8_t *out, size_t len)
{
	(void)out;
	(void)len;
	return RADIANT_SEC_ENOTSUP;
}
#endif /* !CONFIG_RADIANT_SEC_HAS_RNG */

int radiant_sec_set_devnum(uint8_t ch, uint16_t devnum)
{
	struct sec_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}
	c->devnum = devnum;
	return RADIANT_SEC_OK;
}

int radiant_sec_try_devnum(uint8_t ch, uint16_t devnum)
{
	struct sec_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}
	if (!c->epoch_set) {
		/* Nothing can verify before the epoch is known, so a trial
		 * started here would report "not my sensor" for every
		 * candidate, including the right one. */
		return RADIANT_SEC_EINVAL;
	}

	c->devnum = devnum;

	/* Clean slate: everything below belonged to the previous candidate.
	 * last_verdict goes back to CLEAR, not UNVERIFIED, since CLEAR is
	 * "no window judged yet" and the caller's loop is "is it VERIFIED
	 * yet". The floor too: the nonce binds the device number, so the
	 * previous candidate's verified windows say nothing about this one's
	 * counter (a Tier 2 power cycle restarts it at 0). */
	c->rx_win_open = false;
	c->rx_prev_valid = false;
	c->rx_prev_seen = false;
	c->rx_floor = 0u;
	c->rx_absorbed = 0u;
	c->rx_tag_have = 0u;
	c->rx_broken = false;
	hold_reset(c);
	c->last_verdict = RADIANT_SEC_VERDICT_CLEAR;

	return RADIANT_SEC_OK;
}

void radiant_sec_channel_release(uint8_t ch)
{
	struct sec_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return;
	}

	{
		k_spinlock_key_t key = k_spin_lock(&sec_q_lock);
		uint16_t         i;
		uint16_t         n = 0u;

		/* Queued ciphertext belongs to the context being released; left
		 * here it is judged under whatever key this channel number gets
		 * next (review 2026-10-31, A6-1). */
		for (i = 0u; i < sec_q_count; i++) {
			struct sec_pkt *p =
				&sec_q[(sec_q_head + i) % SEC_QUEUE_DEPTH];

			if (p->ch != ch) {
				sec_q[(sec_q_head + n) % SEC_QUEUE_DEPTH] = *p;
				n++;
			}
		}
		sec_q_count = n;
		k_spin_unlock(&sec_q_lock, key);
	}

	radiant_sec_key_destroy(&c->k_root);
	radiant_sec_key_destroy(&c->k_enc);
	radiant_sec_key_destroy(&c->k_auth);
	memset(c, 0, sizeof(*c));
	sec_of[ch] = 0xFFu;
}

void radiant_sec_reset(void)
{
	uint8_t i;

	sec_map_init();
	for (i = 0u; i < SEC_MAX_CH; i++) {
		radiant_sec_key_destroy(&sec[i].k_root);
		radiant_sec_key_destroy(&sec[i].k_enc);
		radiant_sec_key_destroy(&sec[i].k_auth);
	}
	memset(sec, 0, sizeof(sec));
	memset(sec_of, 0xFF, sizeof(sec_of));

	{
		/* The ISR end of the queue may be mid-ingest. */
		k_spinlock_key_t key = k_spin_lock(&sec_q_lock);

		sec_q_head = 0u;
		sec_q_count = 0u;
		k_spin_unlock(&sec_q_lock, key);
	}
}

enum radiant_sec_verdict radiant_sec_last_verdict(uint8_t ch)
{
	const struct sec_ctx *c = ctx_of(ch);

	return (c == NULL) ? RADIANT_SEC_VERDICT_CLEAR : c->last_verdict;
}

void radiant_sec_stat_enrolment(uint8_t ch)
{
	struct sec_ctx *c = ctx_of(ch);

	/* Silent for a channel with no context. Unreachable from the one
	 * caller (which installs the key first), but a counter that faulted
	 * on an unkeyed channel would be worse than one that missed a tick. */
	if (c != NULL) {
		stat_bump(&c->stats.enrolments);
	}
}

void radiant_sec_get_stats(uint8_t ch, struct radiant_sec_stats *out)
{
	const struct sec_ctx *c = ctx_of(ch);

	if (out == NULL) {
		return;
	}
	if (c == NULL) {
		memset(out, 0, sizeof(*out));
		return;
	}
	*out = c->stats;
}

void radiant_sec_get_state(uint8_t ch, struct radiant_sec_state *out)
{
	const struct sec_ctx *c = ctx_of(ch);

	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (c == NULL) {
		out->last_verdict = RADIANT_SEC_VERDICT_CLEAR;
		return;
	}

	out->switches = c->switches;
	out->w = c->w;
	out->page_lo = c->page_lo;
	out->page_hi = c->page_hi;
	out->epoch = c->epoch;
	out->last_verdict = c->last_verdict;
	out->secured = ctx_armed(c);

	if (c->epoch_set) {
		/* Truncated to 16 bits because that is the counter's own width -
		 * the high bits are the epoch's business, not this field's. */
		out->expected_index =
			(uint16_t)expected_index(c, (uint64_t)radiant_radio_now());
	}
}

/* ── Transmit ───────────────────────────────────────────────────────────── */

/*
 * Absorb one transmitted packet into the running window CMAC, closing the
 * window when full. The resulting tag is sent across the NEXT window, one
 * byte per packet - forced, not chosen: encrypt-then-MAC means the tag
 * depends on all W packets, so the first packet's byte can't be known until
 * the last is built.
 */
static void tx_absorb(struct sec_ctx *c, const uint8_t *pay, uint16_t counter)
{
	uint16_t start = (uint16_t)(counter - (counter % c->w));
	uint8_t  tag[RADIANT_SEC_BLOCK_BYTES];
	uint8_t  nonce[RADIANT_SEC_BLOCK_BYTES];

	if (!c->tx_win_open || c->tx_win_start != start) {
		radiant_sec_nonce_block(nonce, c->epoch, c->devnum, start,
					RADIANT_SEC_DOM_SPREAD_MAC);
		if (radiant_sec_cmac_init(&c->tx_cmac, &c->k_auth) !=
		    RADIANT_SEC_OK) {
			c->tx_win_open = false;
			return;
		}
		(void)radiant_sec_cmac_update(&c->tx_cmac, nonce, sizeof(nonce));
		c->tx_win_open = true;
		c->tx_win_start = start;
	}

	/* Bytes [0..6] as they go on the air, including the page number - omit
	 * byte [0] and an attacker who flips it reinterprets the same
	 * authenticated bits under a different schema. Byte [7] carries the
	 * tag itself. */
	(void)radiant_sec_cmac_update(&c->tx_cmac, pay, 7u);

	if ((uint8_t)(counter % c->w) == (uint8_t)(c->w - 1u)) {
		if (radiant_sec_cmac_final(&c->tx_cmac, tag) == RADIANT_SEC_OK) {
			memcpy(c->tx_prev_tag, tag, c->w);
			c->tx_prev_valid = true;
		}
		c->tx_win_open = false;
		radiant_sec_wipe(tag, sizeof(tag));
	}
}

int radiant_sec_tx_transform(uint8_t ch, uint8_t *pay, uint8_t len)
{
	struct sec_ctx *c = ctx_of(ch);
	uint8_t         nonce[RADIANT_SEC_BLOCK_BYTES];
	uint16_t        counter;
	int             rc;

	if (!ctx_armed(c) || pay == NULL || len < SEC_PAGE_BYTES) {
		return RADIANT_SEC_OK;
	}
	if (!page_secured(c, pay[0])) {
		/* The descriptor and the common pages travel in the clear, and
		 * the range is what makes that mechanical. */
		return RADIANT_SEC_OK;
	}
	if (c->tx_spent) {
		/* The last wrap found no epoch to advance into: every packet
		 * from here would reuse an (epoch, devnum, counter) nonce. The
		 * caller withholds TX on a non-OK return. A new epoch clears
		 * this (radiant_sec_set_epoch). */
		return RADIANT_SEC_EINVAL;
	}

	counter = c->tx_ctr;

	/* radiant_sec owns byte [1] on a secured master channel: a
	 * host-maintained counter would repeat across retransmissions (a
	 * master resends its body every slot), and a repeated counter under
	 * one (epoch, devnum) is keystream reuse. */
	pay[1] = (uint8_t)counter;

	if ((c->switches & RADIANT_SEC_SW_AUTH) != 0u) {
		/* Tag of the previous window; zero until one has closed. So
		 * the first window of an epoch carries nothing in byte [7] -
		 * there is no window -1 for it to authenticate - but the
		 * window ITSELF is verified like any other, by the tag the
		 * second window carries. */
		pay[7] = c->tx_prev_valid
				 ? c->tx_prev_tag[counter % c->w]
				 : 0x00u;
	}

	if ((c->switches & RADIANT_SEC_SW_CONF) != 0u) {
		radiant_sec_nonce_block(nonce, c->epoch, c->devnum, counter,
					RADIANT_SEC_DOM_CTR);
		rc = radiant_sec_ctr_xor(&c->k_enc, nonce, &pay[2], 5u);
		if (rc != RADIANT_SEC_OK) {
			return rc;
		}
	}

	/* Encrypt then MAC, always, over what's on the air - the MAC covers
	 * ciphertext when X_CONF is on and plaintext when it isn't. */
	if ((c->switches & RADIANT_SEC_SW_AUTH) != 0u) {
		tx_absorb(c, pay, counter);
	}

	c->tx_ctr = (uint16_t)(counter + 1u);
	if (c->tx_ctr == 0u) {
		/* The wrap. Advancing the epoch here stops the counter
		 * repeating under one (epoch, devnum); the receiver derives
		 * the same advance from time. If it cannot advance (no
		 * headroom left), nothing secured goes out again until a new
		 * epoch is installed - it used to carry on and reuse the
		 * keystream (review 2026-10-31, D1-6(a)). */
		if (advance_epoch(c, 1u) != RADIANT_SEC_OK) {
			c->tx_spent = true;
		}
		c->tx_prev_valid = false;
		c->tx_win_open = false;
	}
	return RADIANT_SEC_OK;
}

/* The ciphertext the acknowledgement path must reuse: api_xfer_broadcast()
 * memcpys the plaintext host buffer as an ack payload, which under X_CONF
 * would leak plaintext. Returning the body already built for the current
 * counter fixes it with no second nonce and no ISR crypto. */
bool radiant_sec_channel_is_secured(uint8_t ch)
{
	return ctx_armed(ctx_of(ch));
}

/* ── Receive ────────────────────────────────────────────────────────────── */

int radiant_sec_rx_ingest(uint8_t ch, const uint8_t *pay, uint8_t len,
			  bool broadcast, uint64_t t_sync)
{
	struct sec_ctx *c = ctx_of(ch);
	k_spinlock_key_t key;
	struct sec_pkt  *slot;

	if (!ctx_armed(c) || pay == NULL || len == 0u) {
		return RADIANT_SEC_RX_PLAIN;
	}
	if (!page_secured(c, pay[0])) {
		return RADIANT_SEC_RX_PLAIN;
	}

	/* D15: X_AUTH governs what a secured channel BROADCASTS; without this
	 * check an injector could send a secured-range page as acknowledged
	 * data, skip the MAC machinery, and be delivered as trusted. Dropped
	 * and counted here, in ISR context, with no crypto needed. */
	if (!broadcast) {
		stat_bump(&c->stats.dropped_non_broadcast);
		return RADIANT_SEC_RX_DROP;
	}

	key = k_spin_lock(&sec_q_lock);
	if (sec_q_count == SEC_QUEUE_DEPTH) {
		/* Drop the OLDEST - dropping the newest would break every
		 * window after it too. */
		sec_q_head = (uint16_t)((sec_q_head + 1u) % SEC_QUEUE_DEPTH);
		sec_q_count--;
		stat_bump(&c->stats.dropped_policy);
	}
	slot = &sec_q[(sec_q_head + sec_q_count) % SEC_QUEUE_DEPTH];
	slot->ch = ch;
	slot->len = (len > SEC_PAGE_BYTES) ? SEC_PAGE_BYTES : len;
	memcpy(slot->pay, pay, slot->len);
	slot->t_sync = t_sync;
	sec_q_count++;
	k_spin_unlock(&sec_q_lock, key);

	return RADIANT_SEC_RX_DEFER;
}

/*
 * Release drop-policy buffer `b` on `verdict`, which was computed for the
 * window starting at `window`. Delivered only if the verdict is VERIFIED AND
 * the buffer holds that window; anything else is dropped whole and counted. A
 * window mismatch is a bug, not an attack - the buffers advance with
 * rx_prev_start by construction - so it fails closed, into dropped_policy.
 *
 * No LOG_ERR for the mismatch, deliberately: this file has never logged, and
 * the Feather security.conf image registers no `radiant` log module for the
 * LOG_MODULE_DECLARE above to bind to - one LOG_* call here and that image
 * fails to link on `log_const_radiant`, while the ztest images link fine.
 */
static void hold_flush(struct sec_ctx *c, uint8_t b,
		       enum radiant_sec_verdict verdict, uint16_t window)
{
	bool    release = verdict == RADIANT_SEC_VERDICT_VERIFIED &&
			  c->hold_start[b] == window;
	uint8_t i;

	for (i = 0u; i < c->n_hold[b]; i++) {
		if (release) {
			radiant_sec_deliver(c->ch, c->hold[b][i],
					    SEC_PAGE_BYTES, verdict,
					    c->hold_t[b][i]);
		} else {
			stat_bump(&c->stats.dropped_policy);
		}
	}
	c->n_hold[b] = 0u;
}

/*
 * Close the window that just filled: compare its tag bytes against the tag
 * computed for the PREVIOUS window, then make this window's own tag the one
 * to compare next time. Order matters - window k's bytes authenticate window
 * k-1, so the comparison must happen before rx_prev_tag is overwritten.
 *
 * Under the drop policy the verdict computed here releases the PREVIOUS
 * window's buffer, never the closing one's: window k's packets are not
 * covered by anything until window k+1's tag bytes arrive, so they stay held
 * through this close and are judged at the next. Releasing the closing window
 * here - on window k-1's verdict - was review D1-1.
 *
 * Runs once per window HEARD, not once per window on the air: a window with
 * no packet received never opens and so never closes. That keeps the buffers
 * and rx_prev_* paired, since both advance here and only here - if window
 * k+1 is lost entirely, the close of k+2 judges k (against tag bytes meant
 * for k+1, so UNVERIFIED for a genuine sender) and flushes the buffer that
 * holds k. hold_start is the check on that pairing, not the mechanism.
 *
 * This is also the only place the clock (epoch_anchor) and the replay floor
 * move, and only on VERIFIED: unverified input must not move state that
 * judges later input. They used to move on every packet that passed the
 * replay check, a window or two before its verdict existed, so a keyless
 * injector could drag the clock 127 periods per forged packet
 * (docs/radiant-security.md 3.1). `last` is the packet that closed the window
 * in-window - its last counter - or NULL when a packet from another window
 * closed it. Only the former can have a tag of its own: a window that never
 * saw its last counter is incomplete unless a counter repeated, and a repeat
 * cannot verify either.
 */
static void rx_close_window(struct sec_ctx *c, const struct sec_pkt *last,
			    uint16_t counter)
{
	uint8_t                  own[RADIANT_SEC_BLOCK_BYTES];
	enum radiant_sec_verdict verdict = RADIANT_SEC_VERDICT_UNVERIFIED;
	uint64_t                 period_us = period_us_of(c);
	bool                     tag_full;
	bool                     own_ok;

	/* Did this window carry a complete tag? Judges the PREVIOUS window. */
	tag_full = c->rx_tag_have == (uint8_t)((1u << c->w) - 1u);

	/* Could this window produce a tag of its own? Will judge the NEXT. */
	own_ok = last != NULL && !c->rx_broken && c->rx_absorbed == c->w;

	if (c->rx_prev_seen) {
		if (c->rx_prev_valid && tag_full &&
		    radiant_sec_ct_equal(c->rx_prev_tag, c->rx_tag, c->w)) {
			/* The previous window's closing packet is now known
			 * to be the sender's - bytes, counter and all; only
			 * its arrival time could be a replay's. The first
			 * verified window after a reset is taken whole (the
			 * host's 0xF3 anchor error, once); after that the
			 * clock slews at most period/16 per window. Real
			 * drift is ~0.004 period per window, so the limit
			 * binds only on a jam-and-replay-late attacker, who
			 * gains 1/16 period per window instead of 128. */
			int32_t step = c->rx_prev_err;
			int32_t lim = (int32_t)(period_us >> 4);

			if (c->rx_floor != 0u) {
				step = (step > lim) ? lim
						    : ((step < -lim) ? -lim : step);
			}
			if (step < 0 &&
			    (uint64_t)(-(int64_t)step) > c->epoch_anchor) {
				c->epoch_anchor = 0u;
			} else {
				c->epoch_anchor += (uint64_t)(int64_t)step;
			}
			c->rx_floor = (uint16_t)(c->rx_prev_start + c->w);

			verdict = RADIANT_SEC_VERDICT_VERIFIED;
			stat_bump(&c->stats.windows_verified);
		} else {
			stat_bump(&c->stats.windows_unverified);
		}
		c->last_verdict = verdict;
		radiant_sec_window_verdict(c->ch, c->rx_prev_start, verdict);
		if ((c->switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
			hold_flush(c, c->hold_cur ^ 1u, verdict,
				   c->rx_prev_start);
		}
	} else if ((c->switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		/* The first window heard since a reset: there is no previous
		 * window, so nothing to judge and the other buffer should be
		 * empty - flushed UNVERIFIED anyway, so a bug that left it
		 * full drops rather than delivers. The window closing now
		 * stays held. Its own tag bytes are zeros (no window -1), but
		 * the next window's bytes authenticate it like any other,
		 * which is why it is no longer dropped here. */
		hold_flush(c, c->hold_cur ^ 1u, RADIANT_SEC_VERDICT_UNVERIFIED,
			   c->rx_win_start);
	}

	if (own_ok &&
	    radiant_sec_cmac_final(&c->rx_cmac, own) == RADIANT_SEC_OK) {
		/* Where this window's closing packet says the epoch began,
		 * against the anchor as it now stands. Measured per window, not
		 * per packet, and applied only if this window verifies. Bounded
		 * by the replay slack to ~129 periods (258 s at the longest
		 * period); saturated only when t_sync precedes the anchor. */
		int64_t err = (int64_t)last->t_sync -
			      (int64_t)((uint64_t)counter * period_us) -
			      (int64_t)c->epoch_anchor;

		c->rx_prev_err = (err > INT32_MAX)
					 ? INT32_MAX
					 : ((err < -INT32_MAX) ? -INT32_MAX
							       : (int32_t)err);
		memcpy(c->rx_prev_tag, own, RADIANT_SEC_BLOCK_BYTES);
		c->rx_prev_valid = true;
	} else {
		/* An incomplete window can't authenticate the next one
		 * either - say so rather than comparing against a tag
		 * computed over fewer packets than the sender used. */
		c->rx_prev_valid = false;
	}
	c->rx_prev_seen = true;
	c->rx_prev_start = c->rx_win_start;
	/* The closing window's buffer becomes the previous one, and the one
	 * just flushed takes the next window. Unconditional, policy bit or
	 * not, so the pairing never depends on which path ran. */
	c->hold_cur ^= 1u;

	radiant_sec_wipe(own, sizeof(own));
	c->rx_win_open = false;
}

static void rx_open_window(struct sec_ctx *c, uint16_t start, bool at_boundary)
{
	uint8_t nonce[RADIANT_SEC_BLOCK_BYTES];

	c->rx_win_start = start;
	c->rx_absorbed = 0u;
	c->rx_tag_have = 0u;
	c->rx_broken = !at_boundary;
	c->rx_win_open = true;
	memset(c->rx_tag, 0, sizeof(c->rx_tag));

	radiant_sec_nonce_block(nonce, c->epoch, c->devnum, start,
				RADIANT_SEC_DOM_SPREAD_MAC);
	if (radiant_sec_cmac_init(&c->rx_cmac, &c->k_auth) != RADIANT_SEC_OK) {
		c->rx_broken = true;
		return;
	}
	(void)radiant_sec_cmac_update(&c->rx_cmac, nonce, sizeof(nonce));
}

static void rx_process(struct sec_pkt *pkt)
{
	struct sec_ctx *c = ctx_of(pkt->ch);
	uint8_t         nonce[RADIANT_SEC_BLOCK_BYTES];
	uint32_t        expected;
	uint16_t        counter;
	uint16_t        epoch_delta = 0u;
	uint16_t        start;

	if (!ctx_armed(c) || pkt->len < SEC_PAGE_BYTES) {
		return;
	}

	expected = expected_index(c, pkt->t_sync);
	counter = resolve_counter(expected, pkt->pay[1], &epoch_delta);

	if (epoch_delta != 0u) {
		if (advance_epoch(c, epoch_delta) != RADIANT_SEC_OK) {
			return;
		}
		c->rx_win_open = false;
		c->rx_prev_valid = false;
		c->rx_prev_seen = false;
		c->rx_floor = 0u;
		/* `expected` was computed against the old epoch's anchor (~65536)
		 * and the counter now counts from 0 again: compared as they
		 * stand, new-epoch packet 0 was refused as a replay, every
		 * time, so window 0 of a new epoch could never verify.
		 * advance_epoch() has moved the anchor; re-read it. */
		expected = expected_index(c, pkt->t_sync);
		/* The fourth reset site, and the one that had no hold reset
		 * at all while there was one buffer. Two make it load-bearing:
		 * the old epoch's last window would sit in the previous buffer
		 * and be judged by the new epoch's first verdict, and the open
		 * window's buffer would take new-epoch packets on top of
		 * old-epoch ones. Nothing dropped here could still verify: the
		 * last old-epoch window's tag would ride in new-epoch window 0,
		 * which carries zeros (the sender clears its tag at the wrap),
		 * and the window before it is still held only if that last
		 * window's tag bytes never all arrived. */
		hold_reset(c);
	}

	/* D5's replay rejection; survives a receiver reboot in a way a
	 * volatile high-water mark can't. Slack is stated in packet counts
	 * because the anchor re-establishes on every verified window, so
	 * this bounds drift since the last one, not across a session. And the
	 * in-session floor beside it: below rx_floor is a window that has
	 * already verified, which must not verify twice (radiant-security.md
	 * 7.6). */
	if ((expected > counter && (uint32_t)(expected - counter) > 128u) ||
	    counter < c->rx_floor) {
		stat_bump(&c->stats.dropped_replay);
		return;
	}

	/* MAC first, then decrypt - the sender did encrypt-then-MAC, so the
	 * MAC covers ciphertext. Absorbing here while pkt->pay is still what
	 * was on the air makes that free; doing the XOR first would cost a
	 * second copy or re-deriving the keystream. */
	if ((c->switches & RADIANT_SEC_SW_AUTH) != 0u) {
		start = (uint16_t)(counter - (counter % c->w));
		if (!c->rx_win_open || c->rx_win_start != start) {
			if (c->rx_win_open) {
				rx_close_window(c, NULL, 0u);
			}
			rx_open_window(c, start, (counter % c->w) == 0u);
		}

		if (!c->rx_broken) {
			/* Bytes [0..6] as they arrived, including the page
			 * number. Byte [7] is the tag itself. */
			if (radiant_sec_cmac_update(&c->rx_cmac, pkt->pay, 7u) ==
			    RADIANT_SEC_OK) {
				c->rx_absorbed++;
			} else {
				c->rx_broken = true;
			}
		}

		c->rx_tag[counter % c->w] = pkt->pay[7];
		c->rx_tag_have |= (uint8_t)(1u << (counter % c->w));
	}

	if ((c->switches & RADIANT_SEC_SW_CONF) != 0u) {
		radiant_sec_nonce_block(nonce, c->epoch, c->devnum, counter,
					RADIANT_SEC_DOM_CTR);
		if (radiant_sec_ctr_xor(&c->k_enc, nonce, &pkt->pay[2], 5u) !=
		    RADIANT_SEC_OK) {
			return;
		}
	}

	if ((c->switches & RADIANT_SEC_SW_AUTH) == 0u) {
		/* X_CONF alone: no window, no verdict - CLEAR is honest here,
		 * confidential but unauthenticated. Weakest useful config,
		 * and the one ANT+ shipped. */
		c->last_verdict = RADIANT_SEC_VERDICT_CLEAR;
		radiant_sec_deliver(c->ch, pkt->pay, pkt->len,
				    RADIANT_SEC_VERDICT_CLEAR, pkt->t_sync);
		return;
	}

	/* No re-anchor here: this packet is not verified yet, and the clock
	 * moves only on a verified window (rx_close_window()). */

	if ((c->switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		uint8_t b = c->hold_cur;

		/* Into the open window's buffer, tagged with that window. A
		 * buffer only ever holds one window - a packet for another
		 * closes the open one first, above - so a mismatch here is a
		 * bug, and poisoning the tag makes its flush drop the lot. */
		if (c->n_hold[b] == 0u) {
			c->hold_start[b] = c->rx_win_start;
		} else if (c->hold_start[b] != c->rx_win_start) {
			c->hold_start[b] = HOLD_START_POISON;
		}
		if (c->n_hold[b] < RADIANT_SEC_W_MAX) {
			memcpy(c->hold[b][c->n_hold[b]], pkt->pay,
			       SEC_PAGE_BYTES);
			c->hold_t[b][c->n_hold[b]] = pkt->t_sync;
			c->n_hold[b]++;
		} else {
			/* Only a repeated counter fills a buffer past W, and
			 * that window cannot verify; counted, not silent. */
			stat_bump(&c->stats.dropped_policy);
		}
	} else {
		/* D6: delivered, marked, not discarded - genuinely unverified
		 * at this instant; the verdict follows via
		 * radiant_sec_window_verdict() once the tagging window
		 * completes. Discarding instead would cost W packets per one
		 * lost (3.2% loss at W=8 vs. the ~0.4% floor) and hand an
		 * attacker a W-for-1 denial of service. */
		c->last_verdict = RADIANT_SEC_VERDICT_UNVERIFIED;
		radiant_sec_deliver(c->ch, pkt->pay, pkt->len,
				    RADIANT_SEC_VERDICT_UNVERIFIED,
				    pkt->t_sync);
	}

	if ((uint8_t)(counter % c->w) == (uint8_t)(c->w - 1u)) {
		rx_close_window(c, pkt, counter);
	}
}

void radiant_sec_pump(void)
{
	for (;;) {
		k_spinlock_key_t key;
		struct sec_pkt   pkt;

		key = k_spin_lock(&sec_q_lock);
		if (sec_q_count == 0u) {
			k_spin_unlock(&sec_q_lock, key);
			return;
		}
		pkt = sec_q[sec_q_head];
		sec_q_head = (uint16_t)((sec_q_head + 1u) % SEC_QUEUE_DEPTH);
		sec_q_count--;
		k_spin_unlock(&sec_q_lock, key);

		/* Outside the spinlock: AES happens here and must not run
		 * with interrupts masked. */
		rx_process(&pkt);
	}
}
