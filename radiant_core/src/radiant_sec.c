/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec.c - per-channel policy for the two payload transforms.
 *
 * docs/radiant-security.md is normative. This file is the policy layer of the
 * primitive / policy / host cut: it decides what happens to which bytes and
 * when, and it computes nothing itself.
 *
 * ── The one rule that keeps the seam real ──────────────────────────────────
 *
 * THIS FILE CONTAINS NO CALL TO radiant_sec_aes_ecb(). Level 1 is for
 * backends; Level 2 - radiant_sec_ctr_xor(), radiant_sec_cmac*() and the
 * portable helpers over them - is for policy. The `zero-cost` CI job greps for
 * that call, because a seam whose policy layer reaches past it is a seam only
 * in the prototypes, and it stays that way until somebody tries to attach
 * CRACEN and finds the mode-level path was never live.
 *
 * ── Context ────────────────────────────────────────────────────────────────
 *
 * TX transform: thread context, under api_lock, immediately before the frame
 * is built. The TX body is DMA'd and armed arbitrarily early, so ciphertext
 * must be final before radiant_sched_request_tx().
 *
 * RX ingest: ISR context, and it does no crypto at all - it classifies and
 * copies eight bytes. RX crypto: the pump, in the event thread. That rule does
 * not vary with caps.block_us, because a rule that changes with the backend is
 * a rule that gets debugged twice.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <radiant_core/radiant_channel.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_sec.h>

LOG_MODULE_DECLARE(radiant_core, CONFIG_RADIANT_CORE_LOG_LEVEL);

#define SEC_PAGE_BYTES  8u
#define SEC_MAX_CH      CONFIG_RADIANT_SEC_MAX_CHANNELS

/*
 * The ISR-to-pump queue.
 *
 * Sized so that a full window of every secured channel can be in flight at
 * once, which is the deepest backlog a pump that runs on every event-thread
 * wakeup can face. Overflow drops the OLDEST entry rather than the newest: a
 * dropped packet breaks its window either way, and dropping the newest would
 * also break every window after it.
 */
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

	/*
	 * D3 and D17. No transform enables until the host has advanced the
	 * epoch after a reset: a reboot that restarts the counter under an
	 * unchanged epoch is a two-time pad for X_CONF, and it makes K_auth and
	 * every recorded (packet, tag) pair from the previous session valid
	 * again, which is a full replay against an X_AUTH-only channel.
	 */
	bool     epoch_set;
	uint32_t epoch;
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
	/*
	 * The previous window: whether there WAS one, what it started at, and
	 * the tag this receiver computed for it.
	 *
	 * `seen` and `valid` are separate on purpose. A window with a hole in
	 * it produces no tag, so it cannot be compared against - but a host
	 * must still be told it was unverifiable, or a lost packet is silently
	 * indistinguishable from a verified one. Folding the two into a single
	 * flag skips the verdict for exactly the case that most needs it.
	 */
	bool     rx_prev_seen;
	bool     rx_prev_valid;
	uint16_t rx_prev_start;
	uint8_t  rx_prev_tag[RADIANT_SEC_BLOCK_BYTES];

	/*
	 * The drop policy's holding buffer, and the only place this feature
	 * buys memory with a switch.
	 *
	 * The default - deliver an unverifiable window, marked - needs no
	 * buffer at all: packets go up as they arrive and the verdict follows.
	 * Dropping instead means holding a window until its verdict is known,
	 * which is a window's worth of payload. 64 bytes per secured channel,
	 * paid by every secured channel whether or not it sets the bit, because
	 * a static table cannot be conditional.
	 */
	uint8_t  hold[RADIANT_SEC_W_MAX][SEC_PAGE_BYTES];
	uint8_t  hold_len[RADIANT_SEC_W_MAX];
	uint64_t hold_t[RADIANT_SEC_W_MAX];
	uint8_t  n_hold;

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

/*
 * D4: the expected packet index, from TIME and not from arrival history.
 *
 * A receiver joining mid-epoch is the NORMAL case, not an edge case, and it has
 * no arrival history to count from. Neither does a receiver after a gap longer
 * than 255 packets, and neither does anything in sparse mode. So the index
 * comes from the epoch phase and the channel period.
 */
static uint32_t expected_index(const struct sec_ctx *c, uint64_t now)
{
	uint64_t period_us;
	uint64_t elapsed;

	if (c->period_counts == 0u) {
		return 0u;
	}
	period_us = ((uint64_t)c->period_counts * 1000000u) / 32768u;
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
 * The 16-bit counter, from the byte on the air and the expected index: pick the
 * rollover nearest what time says. `epoch_delta` reports how many counter wraps
 * that implies, because a wrap advances the epoch by 1 on both sides (D14) -
 * the receiver gets that for free from the high bits it already computed.
 *
 * Drift budget: nearest-rollover resolution needs combined clock error below
 * +/-128 packet periods - +/-32 s at 4 Hz, against two 50 ppm crystals
 * diverging about 8.6 s/day - and the anchor is re-established on every
 * accepted packet, so drift only accumulates across a gap in which nothing was
 * accepted.
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

/*
 * D14: a counter wrap advances the epoch by 1, on both sides, and re-derives
 * the keys. Without it the 16-bit counter wraps after about 4.5 hours at 4 Hz
 * and reuses keystream inside one epoch - the same two-time pad the reboot rule
 * exists to prevent, reintroduced by the epoch no longer rotating on its own.
 *
 * 65536 is divisible by every legal W, so window alignment survives the wrap
 * untouched.
 */
static int advance_epoch(struct sec_ctx *c, uint16_t by)
{
	uint64_t period_us;

	if (by == 0u) {
		return RADIANT_SEC_OK;
	}
	if (c->epoch > 0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM) {
		return RADIANT_SEC_EINVAL;
	}
	c->epoch += by;
	stat_bump(&c->stats.epoch_advances);

	/* Move the anchor with it, so the phase stays continuous: 65536
	 * packets have gone by per wrap. */
	period_us = ((uint64_t)c->period_counts * 1000000u) / 32768u;
	c->epoch_anchor += (uint64_t)by * 65536u * period_us;

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

	sec_map_init();
	if (ch >= RADIANT_CHANNEL_COUNT || root == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	c = ctx_of(ch);
	if (c == NULL) {
		/* Allocation is the first SET_KEY and nothing else. There is no
		 * init call, so a build that never keys a channel never touches
		 * this table. */
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
		return rc;
	}
	c->base_devnum = base_devnum;
	if (c->devnum == 0u) {
		c->devnum = base_devnum;
	}

	/*
	 * A new root invalidates the epoch gate. Installing a key is a fresh
	 * start, and a fresh start under an epoch the previous key already used
	 * would be exactly the state D3 forbids.
	 */
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
		/* Configuring a channel with no key is a host sequencing error,
		 * not a state to invent a context for. */
		return RADIANT_SEC_ENOKEY;
	}

	/* Descriptor encryption is refused in v1: the descriptor has no
	 * counter - byte [1] is a frame index - so it has no nonce, and the
	 * time-derived reconstruction has nothing to reconstruct. */
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
	/* Bounded, so the descriptor (0x00) and the ANT+ common pages stay in
	 * the clear mechanically rather than by memory - and so a host cannot
	 * make its own node undiscoverable by accident. */
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
	c->tx_win_open = false;
	c->tx_prev_valid = false;
	c->n_hold = 0u;
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
	/*
	 * Monotone, and this is the whole of D3's enforceable half. The rest is
	 * a normative obligation on the epoch authority - persist the last
	 * epoch issued and never reissue it - because keys and state are wiped
	 * on reset and nothing here survives to check.
	 */
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
	c->tx_win_open = false;
	c->tx_prev_valid = false;
	c->rx_win_open = false;
	c->rx_prev_valid = false;
	c->rx_prev_seen = false;
	c->n_hold = 0u;

	return derive_keys(c);
}

int radiant_sec_set_devnum(uint8_t ch, uint16_t devnum)
{
	struct sec_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}
	c->devnum = devnum;
	return RADIANT_SEC_OK;
}

void radiant_sec_channel_release(uint8_t ch)
{
	struct sec_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return;
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

	sec_q_head = 0u;
	sec_q_count = 0u;
}

enum radiant_sec_verdict radiant_sec_last_verdict(uint8_t ch)
{
	const struct sec_ctx *c = ctx_of(ch);

	return (c == NULL) ? RADIANT_SEC_VERDICT_CLEAR : c->last_verdict;
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

/* ── Transmit ───────────────────────────────────────────────────────────── */

/*
 * Absorb one transmitted packet into the running window CMAC, and close the
 * window when it is full.
 *
 * The tag that comes out is transmitted across the NEXT window, one byte per
 * packet. That lag is forced rather than chosen: encrypt-then-MAC means the tag
 * of a window depends on all W of its packets, so the byte belonging in the
 * first packet cannot be known until the last has been built. The alternatives
 * are to delay every packet by up to W periods, which makes telemetry stale to
 * save a byte that was never at stake, or to build the whole window before
 * transmitting, which a host writing one page at a time cannot do.
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

	/* Bytes [0..6] of the packet as it goes on the air, which includes the
	 * page number: leave byte [0] out and an attacker who flips it
	 * reinterprets the same authenticated bits against a different schema.
	 * Byte [7] is excluded because that is where the tag itself rides. */
	(void)radiant_sec_cmac_update(&c->tx_cmac, pay, 7u);

	if ((uint8_t)(counter % c->w) == (uint8_t)(c->w - 1u)) {
		if (radiant_sec_cmac_final(&c->tx_cmac, tag) == RADIANT_SEC_OK) {
			memcpy(c->tx_prev_tag, tag, c->w);
			c->tx_prev_valid = true;
		}
		c->tx_win_open = false;
		memset(tag, 0, sizeof(tag));
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

	counter = c->tx_ctr;

	/*
	 * radiant_sec OWNS BYTE [1] ON A SECURED MASTER CHANNEL.
	 *
	 * The host cannot satisfy "+1 per transmitted data page": a master
	 * retransmits its current body every slot whether or not the
	 * application wrote a new one, so a host-maintained counter would
	 * repeat across retransmissions - and a repeated counter under one
	 * (epoch, devnum) is keystream reuse, which is the one failure that
	 * turns CTR from weak into catastrophic.
	 */
	pay[1] = (uint8_t)counter;

	if ((c->switches & RADIANT_SEC_SW_AUTH) != 0u) {
		/* The tag of the previous window. Zeros until one has closed,
		 * which is why the first window of an epoch is never
		 * verifiable and why a receiver reports that as unverified
		 * rather than as an attack. */
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

	/* Encrypt THEN MAC, always, in that order, and over what is on the air.
	 * The MAC covers ciphertext when X_CONF is on and plaintext when it is
	 * not, which is the same statement either way. */
	if ((c->switches & RADIANT_SEC_SW_AUTH) != 0u) {
		tx_absorb(c, pay, counter);
	}

	c->tx_ctr = (uint16_t)(counter + 1u);
	if (c->tx_ctr == 0u) {
		/* The wrap. Advancing the epoch here is what stops the counter
		 * repeating under one (epoch, devnum) after ~4.5 hours at
		 * 4 Hz; the receiver derives the same advance from time. */
		(void)advance_epoch(c, 1u);
		c->tx_prev_valid = false;
		c->tx_win_open = false;
	}
	return RADIANT_SEC_OK;
}

/*
 * The ciphertext the acknowledgement path must reuse.
 *
 * api_xfer_broadcast() memcpys the plaintext host buffer onto the air as an
 * acknowledgement payload, which under X_CONF hands a passive listener the
 * plaintext of the same page. Returning the body already built for the current
 * counter fixes it with no second nonce, no reuse and no ISR crypto: same
 * nonce, same plaintext, identical ciphertext.
 */
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

	/*
	 * D15. X_AUTH governs what a secured channel BROADCASTS, and the decode
	 * switch happily delivers acknowledged data and burst arms as well - so
	 * without this an injector sends a secured-range page as acknowledged
	 * data, skips the MAC machinery entirely, and is delivered as
	 * implicitly trusted data.
	 *
	 * Dropped and counted, in ISR context, with no crypto: the page number
	 * and the message type are all this decision needs.
	 */
	if (!broadcast) {
		stat_bump(&c->stats.dropped_non_broadcast);
		return RADIANT_SEC_RX_DROP;
	}

	key = k_spin_lock(&sec_q_lock);
	if (sec_q_count == SEC_QUEUE_DEPTH) {
		/* Drop the OLDEST. A dropped packet breaks its window either
		 * way; dropping the newest would break every window after it
		 * too. */
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

/* Release whatever the drop policy is holding, with the verdict now known. */
static void hold_flush(struct sec_ctx *c, enum radiant_sec_verdict verdict)
{
	uint8_t i;

	for (i = 0u; i < c->n_hold; i++) {
		if (verdict == RADIANT_SEC_VERDICT_VERIFIED) {
			radiant_sec_deliver(c->ch, c->hold[i], c->hold_len[i],
					    verdict, c->hold_t[i]);
		} else {
			stat_bump(&c->stats.dropped_policy);
		}
	}
	c->n_hold = 0u;
}

/*
 * Close the window that has just filled: compare the tag bytes it carried
 * against the tag this receiver computed for the PREVIOUS window, then make
 * this window's own tag the one to compare against next time.
 *
 * Order matters. The bytes collected during window k authenticate window k-1,
 * so the comparison has to happen before rx_prev_tag is overwritten.
 */
static void rx_close_window(struct sec_ctx *c)
{
	uint8_t                  own[RADIANT_SEC_BLOCK_BYTES];
	enum radiant_sec_verdict verdict = RADIANT_SEC_VERDICT_UNVERIFIED;
	bool                     tag_full;
	bool                     own_ok;

	/* Did this window carry a complete tag? That is what judges the
	 * PREVIOUS window. */
	tag_full = c->rx_tag_have == (uint8_t)((1u << c->w) - 1u);

	/* Could this window produce a tag of its own? That is what will judge
	 * the NEXT one. */
	own_ok = !c->rx_broken && c->rx_absorbed == c->w;

	if (c->rx_prev_seen) {
		if (c->rx_prev_valid && tag_full &&
		    memcmp(c->rx_prev_tag, c->rx_tag, c->w) == 0) {
			verdict = RADIANT_SEC_VERDICT_VERIFIED;
			stat_bump(&c->stats.windows_verified);
		} else {
			stat_bump(&c->stats.windows_unverified);
		}
		c->last_verdict = verdict;
		radiant_sec_window_verdict(c->ch, c->rx_prev_start, verdict);
		if ((c->switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
			hold_flush(c, verdict);
		}
	} else if ((c->switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		/* Nothing authenticates the first window of an epoch, because
		 * the tag lags one window. Under the drop policy that means
		 * dropping it, which is what the policy asked for. */
		hold_flush(c, RADIANT_SEC_VERDICT_UNVERIFIED);
	}

	if (own_ok &&
	    radiant_sec_cmac_final(&c->rx_cmac, own) == RADIANT_SEC_OK) {
		memcpy(c->rx_prev_tag, own, RADIANT_SEC_BLOCK_BYTES);
		c->rx_prev_valid = true;
	} else {
		/* An incomplete window cannot authenticate the next one
		 * either. Say so rather than comparing against a tag computed
		 * over fewer packets than the sender used. */
		c->rx_prev_valid = false;
	}
	c->rx_prev_seen = true;
	c->rx_prev_start = c->rx_win_start;

	memset(own, 0, sizeof(own));
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
	}

	/*
	 * D5's replay rejection, and it survives a receiver reboot in a way a
	 * volatile high-water mark cannot - the normal case is a receiver
	 * joining mid-stream, which a high-water mark handles by accepting a
	 * full epoch of replay.
	 *
	 * The slack is stated in packet counts because the anchor is
	 * re-established on every accepted packet, so this is a bound on the
	 * jitter of one hop rather than on accumulated drift.
	 */
	if (expected > counter && (uint32_t)(expected - counter) > 128u) {
		stat_bump(&c->stats.dropped_replay);
		return;
	}

	/*
	 * MAC FIRST, THEN DECRYPT, because the sender did encrypt-then-MAC and
	 * therefore MAC'd the ciphertext. Absorbing here - while pkt->pay is
	 * still exactly what was on the air - is what makes that free: doing
	 * the XOR first would mean either a second copy of every queued packet
	 * or re-deriving the keystream to undo it, and both cost more than
	 * getting the order right.
	 */
	if ((c->switches & RADIANT_SEC_SW_AUTH) != 0u) {
		start = (uint16_t)(counter - (counter % c->w));
		if (!c->rx_win_open || c->rx_win_start != start) {
			if (c->rx_win_open) {
				rx_close_window(c);
			}
			rx_open_window(c, start, (counter % c->w) == 0u);
		}

		if (!c->rx_broken) {
			/* Bytes [0..6] as they arrived, including the page
			 * number: leave byte [0] out and flipping it
			 * reinterprets the same authenticated bits against a
			 * different schema. Byte [7] is the tag itself. */
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
		/* X_CONF alone: no window, no verdict, and CLEAR is the honest
		 * word - the payload is confidential and nothing about it is
		 * authenticated. This is the weakest useful configuration and
		 * it is the one ANT+ shipped. */
		c->last_verdict = RADIANT_SEC_VERDICT_CLEAR;
		radiant_sec_deliver(c->ch, pkt->pay, pkt->len,
				    RADIANT_SEC_VERDICT_CLEAR, pkt->t_sync);
		return;
	}

	/* Re-anchor the phase on every accepted packet. With that slew, drift
	 * matters only across a gap in which nothing was accepted. */
	{
		uint64_t period_us = ((uint64_t)c->period_counts * 1000000u) /
				     32768u;

		if (period_us != 0u) {
			uint64_t ideal = (uint64_t)counter * period_us;

			c->epoch_anchor = (pkt->t_sync > ideal)
						  ? (pkt->t_sync - ideal)
						  : 0u;
		}
	}

	if ((c->switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		if (c->n_hold < RADIANT_SEC_W_MAX) {
			memcpy(c->hold[c->n_hold], pkt->pay, SEC_PAGE_BYTES);
			c->hold_len[c->n_hold] = pkt->len;
			c->hold_t[c->n_hold] = pkt->t_sync;
			c->n_hold++;
		}
	} else {
		/*
		 * D6: DELIVERED, MARKED, NOT DISCARDED - and marked unverified
		 * because at this instant it genuinely is. The window's verdict
		 * follows through radiant_sec_window_verdict() once the window
		 * carrying its tag completes.
		 *
		 * Discarding instead would cost W packets for every one lost -
		 * 3.2% delivered-data loss at W=8 against a measured ~0.4%
		 * floor - and hand an attacker a W-for-1 denial of service,
		 * where one injected frame destroys W legitimate ones.
		 */
		c->last_verdict = RADIANT_SEC_VERDICT_UNVERIFIED;
		radiant_sec_deliver(c->ch, pkt->pay, pkt->len,
				    RADIANT_SEC_VERDICT_UNVERIFIED,
				    pkt->t_sync);
	}

	if ((uint8_t)(counter % c->w) == (uint8_t)(c->w - 1u)) {
		rx_close_window(c);
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

		/* Outside the spinlock: this is where the AES happens, and it
		 * must not run with interrupts masked. */
		rx_process(&pkt);
	}
}
