/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_compat - the two attestation tags of docs/radiant-security.md
 * section 11.4, as bytes in and bytes out.
 *
 * Clean-room, derived from docs/decisions/0008-antplus-additive-pages-and-compat-security.md
 * and docs/radiant-security.md section 11.4 only; nothing here derives from
 * any ANT+ device profile document, sdk-ant, or libant.a. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * The three tag functions are pure (arguments in, tag out); everything below
 * them holds per-channel state (due pages, window contents, seen counters,
 * receiver epoch).
 *
 * Deliberately absent: any page number (the emit hook returns a SUBTYPE and
 * fills bytes [1..7]; the layer above composes byte [0]), any field name
 * (messages here are eight opaque bytes), any clock (every instant is an
 * argument), any scheduler (this only answers whether an already-told-about
 * slot owes an attestation page).
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>

#include <radiant_core/radiant_sec_compat.h>

#if defined(CONFIG_RADIANT_SEC_COMPAT)

/* N in {4, 8, 16, 32}: inside range with a single bit set. N is the airtime
 * cost, verification latency, and amplification factor at once. */
static bool window_is_legal(uint8_t n)
{
	return n >= RADIANT_SEC_COMPAT_N_MIN &&
	       n <= RADIANT_SEC_COMPAT_N_MAX &&
	       (n & (uint8_t)(n - 1u)) == 0u;
}

void radiant_sec_compat_nonce_block(uint8_t out[RADIANT_SEC_BLOCK_BYTES],
				    uint32_t epoch, uint16_t devnum,
				    uint16_t counter, uint8_t sub)
{
	if (out == NULL || sub == 0u || sub > 0x0Fu) {
		return;
	}

	/* Built by the shared helper and extended, not laid out again here -
	 * two copies of a byte layout eventually drift. */
	radiant_sec_nonce_block(out, epoch, devnum, counter,
				RADIANT_SEC_DOM_COMPAT_MAC);
	out[9] = sub;
}

int radiant_sec_compat_tier1_tag(const struct radiant_sec_key *k_auth,
				 uint32_t epoch, uint16_t devnum,
				 uint16_t att_counter,
				 uint8_t out[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES])
{
	uint8_t block[RADIANT_SEC_BLOCK_BYTES];
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	int rc;

	if (k_auth == NULL || out == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	radiant_sec_compat_nonce_block(block, epoch, devnum, att_counter,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_I);

	/* The message IS the nonce and there is nothing after it. That single
	 * fact is the tier: with no payload covered, a delivered tag verifies
	 * whatever else was lost. */
	rc = radiant_sec_cmac(k_auth, block, sizeof(block), tag);
	if (rc == RADIANT_SEC_OK) {
		memcpy(out, tag, RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES);
	}

	/* The eleven bytes not sent are as secret as the key that made them:
	 * an attacker who learns them learns the untruncated tag for a counter
	 * value that has not been used yet. */
	memset(tag, 0, sizeof(tag));
	memset(block, 0, sizeof(block));
	return rc;
}

int radiant_sec_compat_tier2_tag(const struct radiant_sec_key *k_auth,
				 uint32_t epoch, uint16_t devnum,
				 uint16_t window_index, const uint8_t *msgs,
				 uint8_t n,
				 uint8_t out[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES])
{
	struct radiant_sec_cmac_ctx ctx;
	uint8_t block[RADIANT_SEC_BLOCK_BYTES];
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	int rc;

	if (k_auth == NULL || msgs == NULL || out == NULL ||
	    !window_is_legal(n)) {
		return RADIANT_SEC_EINVAL;
	}

	radiant_sec_compat_nonce_block(block, epoch, devnum, window_index,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_II);

	/* Absorbed incrementally, which is why there is no buffer here for the
	 * covered messages at all: at N = 32 a one-shot call would want 248
	 * bytes of stack for something CMAC consumes sixteen at a time. The
	 * same reason the spread MAC's window needs no payload buffer. */
	rc = radiant_sec_cmac_init(&ctx, k_auth);
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&ctx, block, sizeof(block));
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&ctx, msgs,
					     (size_t)(n - 1u) *
					     RADIANT_SEC_COMPAT_MSG_BYTES);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_final(&ctx, tag);
	}
	if (rc == RADIANT_SEC_OK) {
		memcpy(out, tag, RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES);
	}

	memset(&ctx, 0, sizeof(ctx));
	memset(tag, 0, sizeof(tag));
	memset(block, 0, sizeof(block));
	return rc;
}

/*
 * ═══════════════════════════════════════════════════════════════════════════
 * The stream
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define COMPAT_MAX_CH   CONFIG_RADIANT_SEC_COMPAT_MAX_CHANNELS
#define COMPAT_MSG      RADIANT_SEC_COMPAT_MSG_BYTES
#define COMPAT_WIN_MAX  ((RADIANT_SEC_COMPAT_N_MAX - 1) * COMPAT_MSG)
#define COMPAT_US_PER_S 1000000u

struct compat_ctx {
	bool     used;
	uint8_t  ch;

	uint8_t  switches;
	uint8_t  n;        /* the Tier II window */
	uint16_t t_s;      /* Tier I's interval, in seconds */

	uint16_t period_counts;
	uint16_t devnum;       /* on air, in the nonce */
	uint16_t base_devnum;  /* provisioning time, in the KDF */

	bool     epoch_set;
	/* base_epoch is what was set/adopted; `wraps` is rollovers since;
	 * epoch = base_epoch + wraps. Kept apart so a wrap is reproducible
	 * from time alone rather than requiring witnessed history. */
	uint32_t base_epoch;
	uint32_t wraps;
	uint32_t epoch;
	uint64_t anchor_us;

	struct radiant_sec_key k_root;
	struct radiant_sec_key k_auth;
	struct radiant_sec_key k_id;

	/* ── TX ── */
	uint32_t tx_msgs;               /* transmitted messages since the anchor */
	uint8_t  tx_win[COMPAT_WIN_MAX];
	bool     tx_closing;            /* the body just handed out is a Tier II
					 * tag, so the message it becomes closes
					 * the window instead of joining it */
	uint32_t tx_t1_last;
	bool     tx_t1_any;

	/* ── RX ── */
	uint8_t  rx_win[COMPAT_WIN_MAX];
	uint8_t  rx_absorbed;
	bool     rx_overflow;           /* more messages than the window covers,
					 * which is a lost tag page or an
					 * injected message and is unverifiable
					 * either way */
	uint32_t rx_t1_last;
	bool     rx_t1_any;
	bool     rx_t1_ok;              /* at least one Tier I page VERIFIED */
	uint64_t rx_t1_us;              /* ...and when, for the staleness rule */

	enum radiant_sec_verdict        last_verdict;
	struct radiant_sec_compat_stats stats;
};

/* No channel -> context map: skipping it keeps this file free of
 * radiant_channel.h and the HAL header behind it. */
static struct compat_ctx compat[COMPAT_MAX_CH];

static struct compat_ctx *ctx_of(uint8_t ch)
{
	uint8_t i;

	for (i = 0u; i < COMPAT_MAX_CH; i++) {
		if (compat[i].used && compat[i].ch == ch) {
			return &compat[i];
		}
	}
	return NULL;
}

static bool ctx_armed(const struct compat_ctx *c)
{
	return c != NULL && c->used && c->epoch_set &&
	       (c->switches & (uint8_t)(RADIANT_SEC_COMPAT_SW_TIER_I |
				       RADIANT_SEC_COMPAT_SW_TIER_II)) != 0u;
}

static void stat_bump(uint32_t *counter)
{
	if (*counter != UINT32_MAX) {
		(*counter)++;
	}
}

static uint64_t elapsed_us(const struct compat_ctx *c, uint64_t now_us)
{
	return (now_us > c->anchor_us) ? (now_us - c->anchor_us) : 0u;
}

/* Tier I's counter: intervals of T seconds since the epoch anchor, derived
 * from time on both sides rather than carried per-packet. */
static uint32_t interval_index(const struct compat_ctx *c, uint64_t now_us)
{
	uint64_t t_us = (uint64_t)c->t_s * COMPAT_US_PER_S;

	if (t_us == 0u) {
		return 0u;
	}
	return (uint32_t)(elapsed_us(c, now_us) / t_us);
}

/* The transmitted-message index time says we should be at - D4's rule, applied
 * to a channel whose messages carry no counter of their own. */
static uint32_t msg_index(const struct compat_ctx *c, uint64_t now_us)
{
	uint64_t period_us;

	if (c->period_counts == 0u) {
		return 0u;
	}
	period_us = ((uint64_t)c->period_counts * COMPAT_US_PER_S) / 32768u;
	if (period_us == 0u) {
		return 0u;
	}
	return (uint32_t)(elapsed_us(c, now_us) / period_us);
}

/*
 * Full counter from the low bits on the air plus the index time expects:
 * pick the rollover nearest to what time says. `span` is 0x10000 for Tier I
 * (two-byte page) and 0x100 for Tier II (one-byte page). A receiver joining
 * mid-stream is normal and has no arrival history to count from.
 */
static uint32_t resolve_low(uint32_t expected, uint32_t low, uint32_t span)
{
	uint32_t base = expected & ~(span - 1u);
	uint32_t best = base + low;
	uint32_t best_err = (best > expected) ? (best - expected)
					     : (expected - best);
	uint32_t cand;
	uint32_t err;

	if (base >= span) {
		cand = base - span + low;
		err = (cand > expected) ? (cand - expected) : (expected - cand);
		if (err < best_err) {
			best = cand;
			best_err = err;
		}
	}
	cand = base + span + low;
	err = (cand > expected) ? (cand - expected) : (expected - cand);
	if (err < best_err) {
		best = cand;
	}
	return best;
}

static int derive_auth(struct compat_ctx *c)
{
	radiant_sec_key_destroy(&c->k_auth);
	return radiant_sec_kdf(&c->k_root, RADIANT_SEC_LABEL_AUTH, c->epoch,
			       c->base_devnum, &c->k_auth);
}

/*
 * D14, on the compat surface: a counter wrap advances the epoch by one on
 * both sides and re-derives the keys.
 *
 * Only Tier I's counter drives the epoch: the nonce has one 16-bit counter
 * field, and Tier I's is purely a function of elapsed time, so it stays
 * reproducible even with no Tier II page ever heard. The Tier II window
 * index just rolls over inside an epoch instead.
 */
static int apply_wraps(struct compat_ctx *c, uint32_t wraps)
{
	if (wraps == c->wraps) {
		return RADIANT_SEC_OK;
	}
	if ((uint64_t)c->base_epoch + wraps >
	    (uint64_t)0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM) {
		return RADIANT_SEC_EINVAL;
	}
	c->wraps = wraps;
	c->epoch = c->base_epoch + wraps;
	stat_bump(&c->stats.epoch_advances);
	return derive_auth(c);
}

/* ── Weak delivery hook ─────────────────────────────────────────────────── */

__weak void radiant_sec_compat_verdict(uint8_t ch, uint8_t sub, uint16_t counter,
				       enum radiant_sec_verdict verdict)
{
	(void)ch;
	(void)sub;
	(void)counter;
	(void)verdict;
}

/* ── Configuration ──────────────────────────────────────────────────────── */

int radiant_sec_compat_set_key(uint8_t ch, const uint8_t *root, size_t bits,
			       uint16_t base_devnum)
{
	struct compat_ctx *c;
	uint8_t            i;
	int                rc;

	if (root == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	c = ctx_of(ch);
	if (c == NULL) {
		for (i = 0u; i < COMPAT_MAX_CH; i++) {
			if (!compat[i].used) {
				memset(&compat[i], 0, sizeof(compat[i]));
				compat[i].used = true;
				compat[i].ch = ch;
				compat[i].n = RADIANT_SEC_COMPAT_N_DEFAULT;
				compat[i].t_s = RADIANT_SEC_COMPAT_T_DEFAULT_S;
				compat[i].switches =
					RADIANT_SEC_COMPAT_SW_TIER_I;
				c = &compat[i];
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

	/* K_id is epoch-less - the "id" label uses epoch 0 - so it is derived
	 * once here and survives every wrap, every recovery and every adopted
	 * epoch. That is what makes the hint search one CMAC per candidate
	 * rather than one derivation plus one CMAC. */
	radiant_sec_key_destroy(&c->k_id);
	rc = radiant_sec_kdf(&c->k_root, RADIANT_SEC_LABEL_ID, 0u, base_devnum,
			     &c->k_id);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	/* A new root is a fresh start, and a fresh start under an epoch the
	 * previous key already used is the state D3 forbids. */
	c->epoch_set = false;
	radiant_sec_key_destroy(&c->k_auth);
	return RADIANT_SEC_OK;
}

int radiant_sec_compat_configure(uint8_t ch, uint8_t switches, uint8_t n,
				 uint16_t t_s)
{
	struct compat_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}
	if ((switches & ~(uint8_t)(RADIANT_SEC_COMPAT_SW_TIER_I |
				   RADIANT_SEC_COMPAT_SW_TIER_II |
				   RADIANT_SEC_COMPAT_SW_PIN)) != 0u) {
		return RADIANT_SEC_EINVAL;
	}
	/* N in {4, 8, 16, 32}, enumerated rather than ranged: N is the airtime
	 * cost, the verification latency and the amplification factor at once,
	 * so an unenumerated N is three surprises. */
	if ((switches & RADIANT_SEC_COMPAT_SW_TIER_II) != 0u &&
	    !window_is_legal(n)) {
		return RADIANT_SEC_EINVAL;
	}
	if (t_s < RADIANT_SEC_COMPAT_T_MIN_S || t_s > RADIANT_SEC_COMPAT_T_MAX_S) {
		return RADIANT_SEC_EINVAL;
	}

	c->switches = switches;
	if ((switches & RADIANT_SEC_COMPAT_SW_TIER_II) != 0u) {
		c->n = n;
	}
	c->t_s = t_s;

	/* Any window in flight was counted against a different N. */
	c->tx_msgs = 0u;
	c->tx_closing = false;
	c->rx_absorbed = 0u;
	c->rx_overflow = false;
	return RADIANT_SEC_OK;
}

int radiant_sec_compat_set_epoch(uint8_t ch, uint32_t epoch, uint64_t now_us,
				 uint64_t us_into_epoch, uint16_t period_counts)
{
	struct compat_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}
	if (period_counts == 0u) {
		return RADIANT_SEC_EINVAL;
	}
	if (c->epoch_set && epoch <= c->epoch) {
		return RADIANT_SEC_EINVAL;
	}
	/* Leave room for a wrap to advance into. */
	if (epoch > 0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM) {
		return RADIANT_SEC_EINVAL;
	}

	c->base_epoch = epoch;
	c->wraps = 0u;
	c->epoch = epoch;
	c->anchor_us = (now_us > us_into_epoch) ? (now_us - us_into_epoch) : 0u;
	c->period_counts = period_counts;
	c->epoch_set = true;

	c->tx_msgs = 0u;
	c->tx_closing = false;
	c->tx_t1_any = false;
	c->rx_absorbed = 0u;
	c->rx_overflow = false;
	c->rx_t1_any = false;
	c->rx_t1_ok = false;
	c->last_verdict = RADIANT_SEC_VERDICT_CLEAR;

	return derive_auth(c);
}

int radiant_sec_compat_set_devnum(uint8_t ch, uint16_t devnum)
{
	struct compat_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}
	c->devnum = devnum;
	return RADIANT_SEC_OK;
}

void radiant_sec_compat_channel_release(uint8_t ch)
{
	struct compat_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return;
	}
	radiant_sec_key_destroy(&c->k_root);
	radiant_sec_key_destroy(&c->k_auth);
	radiant_sec_key_destroy(&c->k_id);
	memset(c, 0, sizeof(*c));
}

void radiant_sec_compat_reset(void)
{
	uint8_t i;

	for (i = 0u; i < COMPAT_MAX_CH; i++) {
		radiant_sec_key_destroy(&compat[i].k_root);
		radiant_sec_key_destroy(&compat[i].k_auth);
		radiant_sec_key_destroy(&compat[i].k_id);
	}
	memset(compat, 0, sizeof(compat));
}

void radiant_sec_compat_get_stats(uint8_t ch,
				  struct radiant_sec_compat_stats *out)
{
	const struct compat_ctx *c = ctx_of(ch);

	if (out == NULL) {
		return;
	}
	if (c == NULL) {
		memset(out, 0, sizeof(*out));
		return;
	}
	*out = c->stats;
}

enum radiant_sec_verdict radiant_sec_compat_last_verdict(uint8_t ch)
{
	const struct compat_ctx *c = ctx_of(ch);

	return (c == NULL) ? RADIANT_SEC_VERDICT_CLEAR : c->last_verdict;
}

enum radiant_sec_verdict radiant_sec_compat_stream_verdict(uint8_t ch,
							   uint64_t now_us)
{
	const struct compat_ctx *c = ctx_of(ch);
	uint64_t                 horizon;

	/* CLEAR keeps meaning "no key here", and it also means "this receiver
	 * never asked for attestation". Neither is a security claim. */
	if (c == NULL || !c->used || c->k_root.bits == 0u) {
		return RADIANT_SEC_VERDICT_CLEAR;
	}
	if ((c->switches & (uint8_t)(RADIANT_SEC_COMPAT_SW_TIER_I |
				     RADIANT_SEC_COMPAT_SW_TIER_II)) == 0u ||
	    (c->switches & RADIANT_SEC_COMPAT_SW_PIN) == 0u) {
		return RADIANT_SEC_VERDICT_CLEAR;
	}

	/* Pinned: UNVERIFIED unless something recent says otherwise. This is
	 * downgrade protection - stripping attestation off the air is noticed
	 * here instead of quietly falling back to clear. */
	if ((c->switches & RADIANT_SEC_COMPAT_SW_TIER_I) == 0u) {
		/* Tier II only: the last window is all there is to go on. */
		return (c->last_verdict == RADIANT_SEC_VERDICT_VERIFIED)
			       ? RADIANT_SEC_VERDICT_VERIFIED
			       : RADIANT_SEC_VERDICT_UNVERIFIED;
	}
	if (!c->rx_t1_ok) {
		return RADIANT_SEC_VERDICT_UNVERIFIED;
	}
	horizon = (uint64_t)RADIANT_SEC_COMPAT_STALE_INTERVALS *
		  (uint64_t)c->t_s * COMPAT_US_PER_S;
	if (now_us >= c->rx_t1_us + horizon) {
		return RADIANT_SEC_VERDICT_UNVERIFIED;
	}
	return RADIANT_SEC_VERDICT_VERIFIED;
}

/* ── Transmit ───────────────────────────────────────────────────────────── */

int radiant_sec_compat_tx_attest(uint8_t ch, uint64_t now_us, uint8_t *body,
				 uint8_t len)
{
	struct compat_ctx *c = ctx_of(ch);
	uint8_t            tag[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];
	uint32_t           idx;
	int                rc;

	if (!ctx_armed(c)) {
		return 0;
	}
	if (body == NULL || len < RADIANT_SEC_COMPAT_BODY_BYTES) {
		return RADIANT_SEC_EINVAL;
	}

	rc = apply_wraps(c, interval_index(c, now_us) >> 16);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	/* Tier II goes first when both fall due: its window closes at the Nth
	 * message with no slack, while Tier I's time-derived counter tolerates
	 * a slot of slip. */
	if ((c->switches & RADIANT_SEC_COMPAT_SW_TIER_II) != 0u &&
	    (c->tx_msgs % c->n) == (uint32_t)(c->n - 1u)) {
		uint32_t window = c->tx_msgs / c->n;

		rc = radiant_sec_compat_tier2_tag(&c->k_auth, c->epoch,
						  c->devnum,
						  (uint16_t)(window & 0xFFFFu),
						  c->tx_win, c->n, tag);
		if (rc != RADIANT_SEC_OK) {
			return rc;
		}
		body[0] = (uint8_t)(window & 0xFFu);
		memcpy(&body[1], tag, RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES);
		c->tx_closing = true;
		return RADIANT_SEC_COMPAT_SUBTYPE_TIER_II;
	}

	if ((c->switches & RADIANT_SEC_COMPAT_SW_TIER_I) != 0u) {
		idx = interval_index(c, now_us);
		if (!c->tx_t1_any || idx != c->tx_t1_last) {
			rc = radiant_sec_compat_tier1_tag(&c->k_auth, c->epoch,
							  c->devnum,
							  (uint16_t)(idx & 0xFFFFu),
							  tag);
			if (rc != RADIANT_SEC_OK) {
				return rc;
			}
			body[0] = (uint8_t)(idx & 0xFFu);
			body[1] = (uint8_t)((idx >> 8) & 0xFFu);
			memcpy(&body[2], tag,
			       RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES);
			c->tx_t1_last = idx;
			c->tx_t1_any = true;
			return RADIANT_SEC_COMPAT_SUBTYPE_TIER_I;
		}
	}

	return 0;
}

int radiant_sec_compat_tx_sent(uint8_t ch, const uint8_t *pay, uint8_t len)
{
	struct compat_ctx *c = ctx_of(ch);
	uint32_t           pos;

	if (!ctx_armed(c)) {
		return RADIANT_SEC_OK;
	}
	if (pay == NULL || len < COMPAT_MSG) {
		return RADIANT_SEC_EINVAL;
	}

	if ((c->switches & RADIANT_SEC_COMPAT_SW_TIER_II) != 0u) {
		pos = c->tx_msgs % c->n;
		if (c->tx_closing) {
			/* This message IS the tag. It closes the window rather
			 * than joining it, and the next window starts empty at
			 * the message after it. */
			c->tx_closing = false;
		} else if (pos < (uint32_t)(c->n - 1u)) {
			memcpy(&c->tx_win[pos * COMPAT_MSG], pay, COMPAT_MSG);
		}
		/* else: the slot owed a tag and the caller sent something else.
		 * The window closes untagged; a receiver will absorb N messages
		 * without a close and report the window unverified, which is
		 * the truth about what went on the air. */
	}
	c->tx_msgs++;
	return RADIANT_SEC_OK;
}

/* ── Receive ────────────────────────────────────────────────────────────── */

static void rx_absorb(struct compat_ctx *c, const uint8_t *pay)
{
	if ((c->switches & RADIANT_SEC_COMPAT_SW_TIER_II) == 0u) {
		return;
	}
	if (c->rx_absorbed < (uint8_t)(c->n - 1u)) {
		memcpy(&c->rx_win[c->rx_absorbed * COMPAT_MSG], pay, COMPAT_MSG);
		c->rx_absorbed++;
	} else {
		/* More messages than this window covers: tag page lost or
		 * something injected. Window is unjudgeable either way. */
		c->rx_overflow = true;
	}
}

static int rx_tier1(struct compat_ctx *c, const uint8_t *pay, uint64_t t_sync)
{
	uint8_t                  tag[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES];
	enum radiant_sec_verdict verdict;
	uint32_t                 low;
	uint32_t                 full;
	int                      rc;

	low = (uint32_t)pay[1] | ((uint32_t)pay[2] << 8);
	full = resolve_low(interval_index(c, t_sync), low, 0x10000u);

	rc = apply_wraps(c, full >> 16);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	/* Replay is closed by the counter alone: the tag covers no payload, so
	 * a recorded page stays validly-tagged forever, and only a spent
	 * counter makes it worthless. Checked before the MAC so a replay isn't
	 * counted as a forgery and can't refresh the freshness anchor. */
	if (c->rx_t1_any && full <= c->rx_t1_last) {
		stat_bump(&c->stats.tier1_replays);
		return RADIANT_SEC_EREPLAY;
	}

	rc = radiant_sec_compat_tier1_tag(&c->k_auth, c->epoch, c->devnum,
					  (uint16_t)(full & 0xFFFFu), tag);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	if (memcmp(tag, &pay[3], sizeof(tag)) == 0) {
		verdict = RADIANT_SEC_VERDICT_VERIFIED;
		stat_bump(&c->stats.tier1_verified);
		c->rx_t1_last = full;
		c->rx_t1_any = true;
		c->rx_t1_ok = true;
		c->rx_t1_us = t_sync;
	} else {
		verdict = RADIANT_SEC_VERDICT_UNVERIFIED;
		stat_bump(&c->stats.tier1_unverified);
	}

	c->last_verdict = verdict;
	radiant_sec_compat_verdict(c->ch, RADIANT_SEC_COMPAT_SUBTYPE_TIER_I,
				   (uint16_t)(full & 0xFFFFu), verdict);
	return (verdict == RADIANT_SEC_VERDICT_VERIFIED) ? RADIANT_SEC_OK
							 : RADIANT_SEC_EBADMAC;
}

static int rx_tier2(struct compat_ctx *c, const uint8_t *pay, uint64_t t_sync)
{
	uint8_t                  tag[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];
	enum radiant_sec_verdict verdict = RADIANT_SEC_VERDICT_UNVERIFIED;
	uint32_t                 window;
	int                      rc;

	if ((c->switches & RADIANT_SEC_COMPAT_SW_TIER_II) == 0u) {
		return RADIANT_SEC_ENOTSUP;
	}

	window = resolve_low(msg_index(c, t_sync) / c->n, pay[1], 0x100u);

	rc = apply_wraps(c, interval_index(c, t_sync) >> 16);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	/* Judged on what arrived; a hole is not a forgery. A window CMAC is
	 * not self-synchronising under loss the way the spread tag is - the
	 * reason this tier is off by default. */
	if (!c->rx_overflow && c->rx_absorbed == (uint8_t)(c->n - 1u)) {
		rc = radiant_sec_compat_tier2_tag(&c->k_auth, c->epoch,
						  c->devnum,
						  (uint16_t)(window & 0xFFFFu),
						  c->rx_win, c->n, tag);
		if (rc == RADIANT_SEC_OK &&
		    memcmp(tag, &pay[2], sizeof(tag)) == 0) {
			verdict = RADIANT_SEC_VERDICT_VERIFIED;
		}
	}

	if (verdict == RADIANT_SEC_VERDICT_VERIFIED) {
		stat_bump(&c->stats.tier2_verified);
	} else {
		stat_bump(&c->stats.tier2_unverified);
	}

	/* The next window starts clean whatever this one's verdict was, which
	 * is what bounds the damage of one lost packet to one window. */
	c->rx_absorbed = 0u;
	c->rx_overflow = false;

	c->last_verdict = verdict;
	radiant_sec_compat_verdict(c->ch, RADIANT_SEC_COMPAT_SUBTYPE_TIER_II,
				   (uint16_t)(window & 0xFFFFu), verdict);
	return (verdict == RADIANT_SEC_VERDICT_VERIFIED) ? RADIANT_SEC_OK
							 : RADIANT_SEC_EBADMAC;
}

int radiant_sec_compat_rx(uint8_t ch, uint8_t sub, const uint8_t *pay,
			  uint8_t len, uint64_t t_sync)
{
	struct compat_ctx *c = ctx_of(ch);

	if (c == NULL || !c->used) {
		return RADIANT_SEC_ENOKEY;
	}
	if (!c->epoch_set) {
		return RADIANT_SEC_EINVAL;
	}
	if (pay == NULL || len < COMPAT_MSG) {
		return RADIANT_SEC_EINVAL;
	}

	/* Everything but the tag page is covered (Tier I page, beacon, common
	 * pages): the window is N consecutive transmitted messages, not N
	 * data messages, so a receiver just counts rather than classifying. */
	if (sub != RADIANT_SEC_COMPAT_SUBTYPE_TIER_II) {
		rx_absorb(c, pay);
	}

	if (sub == 0u) {
		return RADIANT_SEC_OK;
	}
	if (sub == RADIANT_SEC_COMPAT_SUBTYPE_TIER_I) {
		return rx_tier1(c, pay, t_sync);
	}
	if (sub == RADIANT_SEC_COMPAT_SUBTYPE_TIER_II) {
		return rx_tier2(c, pay, t_sync);
	}
	return RADIANT_SEC_EINVAL;
}

/* ── Epoch recovery ─────────────────────────────────────────────────────── */

static int hint_of(const struct radiant_sec_key *k_id, uint32_t epoch,
		   uint8_t out[RADIANT_SEC_COMPAT_HINT_BYTES])
{
	uint8_t msg[4];
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	int     rc;

	msg[0] = (uint8_t)(epoch & 0xFFu);
	msg[1] = (uint8_t)((epoch >> 8) & 0xFFu);
	msg[2] = (uint8_t)((epoch >> 16) & 0xFFu);
	msg[3] = (uint8_t)((epoch >> 24) & 0xFFu);

	rc = radiant_sec_cmac(k_id, msg, sizeof(msg), tag);
	if (rc == RADIANT_SEC_OK) {
		memcpy(out, tag, RADIANT_SEC_COMPAT_HINT_BYTES);
	}
	memset(tag, 0, sizeof(tag));
	return rc;
}

int radiant_sec_compat_hint(uint8_t ch, uint32_t epoch,
			    uint8_t out[RADIANT_SEC_COMPAT_HINT_BYTES])
{
	const struct compat_ctx *c = ctx_of(ch);

	if (c == NULL || !c->used) {
		return RADIANT_SEC_ENOKEY;
	}
	if (out == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	return hint_of(&c->k_id, epoch, out);
}

/* ── The derived locator ─────────────────────────────────────────────────
 * trunc16( CMAC(K_id, "priv" || epoch[4 LE]) ), 0x0000 dropped, then the
 * same with a 1-byte suffix 0x00, 0x01, ... Read little-endian, matching
 * the beacon's locator field, so the on-air bytes equal the tag's first
 * two bytes without reversing anything.
 */
#define COMPAT_LOCATOR_MSG_MAX 9u   /* "priv" + epoch[4] + one suffix byte */
#define COMPAT_LOCATOR_WALK    257u /* the unsuffixed derivation plus all 256 */

static int locator_derive(const struct radiant_sec_key *k_id, uint32_t epoch,
			  uint16_t derivation, uint16_t *out)
{
	static const char label[] = RADIANT_SEC_COMPAT_LOCATOR_LABEL;
	uint8_t msg[COMPAT_LOCATOR_MSG_MAX];
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	uint8_t len;
	int     rc;

	/* sizeof - 1: the terminator is not on the air. Unlike a KDF block,
	 * where the terminator IS the separator. */
	memcpy(msg, label, sizeof(label) - 1u);
	len = (uint8_t)(sizeof(label) - 1u);

	msg[len++] = (uint8_t)(epoch & 0xFFu);
	msg[len++] = (uint8_t)((epoch >> 8) & 0xFFu);
	msg[len++] = (uint8_t)((epoch >> 16) & 0xFFu);
	msg[len++] = (uint8_t)((epoch >> 24) & 0xFFu);

	if (derivation > 0u) {
		msg[len++] = (uint8_t)((derivation - 1u) & 0xFFu);
	}

	rc = radiant_sec_cmac(k_id, msg, len, tag);
	if (rc == RADIANT_SEC_OK) {
		*out = (uint16_t)((uint16_t)tag[0] | ((uint16_t)tag[1] << 8));
	}
	memset(tag, 0, sizeof(tag));
	return rc;
}

int radiant_sec_compat_locator(uint8_t ch, uint32_t epoch, uint8_t attempt,
			       uint16_t *out)
{
	const struct compat_ctx *c = ctx_of(ch);
	uint16_t                 derivation;
	uint16_t                 devnum = 0u;
	uint8_t                  found = 0u;
	int                      rc;

	if (c == NULL || !c->used) {
		return RADIANT_SEC_ENOKEY;
	}
	if (out == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	for (derivation = 0u; derivation < COMPAT_LOCATOR_WALK; derivation++) {
		rc = locator_derive(&c->k_id, epoch, derivation, &devnum);
		if (rc != RADIANT_SEC_OK) {
			return rc;
		}
		/* The ANT wildcard, which a master cannot own. Dropped rather
		 * than mapped onto a neighbouring number, because "add one" is
		 * a second rule that a searcher has to implement identically
		 * and the suffix walk is already here. */
		if (devnum == RADIANT_SEC_COMPAT_LOCATOR_WILDCARD) {
			continue;
		}
		if (found == attempt) {
			*out = devnum;
			return RADIANT_SEC_OK;
		}
		found++;
	}

	/* 256 consecutive zeros under one key. Bounded rather than believed. */
	return RADIANT_SEC_EINVAL;
}

/* ── The announcement's tag, and the command's ──────────────────────────────
 *
 * Both are one CMAC over a nonce block and the covered bytes, and both derive
 * the counter from time on both sides rather than carrying one. The header says
 * why that is exact rather than tolerant, and what the exactness costs.
 */
static int tagged(const struct radiant_sec_key *k, uint32_t epoch,
		  uint16_t devnum, uint16_t counter, uint8_t sub,
		  const uint8_t *msg, uint8_t len, uint8_t *out, uint8_t taglen)
{
	struct radiant_sec_cmac_ctx ctx;
	uint8_t block[RADIANT_SEC_BLOCK_BYTES];
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	int     rc;

	radiant_sec_compat_nonce_block(block, epoch, devnum, counter, sub);

	rc = radiant_sec_cmac_init(&ctx, k);
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&ctx, block, sizeof(block));
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&ctx, msg, len);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_final(&ctx, tag);
	}
	if (rc == RADIANT_SEC_OK) {
		memcpy(out, tag, taglen);
	}

	memset(&ctx, 0, sizeof(ctx));
	memset(tag, 0, sizeof(tag));
	memset(block, 0, sizeof(block));
	return rc;
}

/* Armed enough to authenticate: a key and an epoch, weaker than ctx_armed()
 * because it doesn't require either attestation tier on. `physical` +
 * `silent` is the one policy combo permitted with both tiers off, and it
 * still has a key. */
static bool ctx_keyed(const struct compat_ctx *c)
{
	return c != NULL && c->used && c->epoch_set && c->k_root.bits != 0u;
}

static int announce_tag(struct compat_ctx *c, uint64_t when_us,
			const uint8_t *msg, uint8_t len, uint8_t *out)
{
	int rc;

	rc = apply_wraps(c, interval_index(c, when_us) >> 16);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	return tagged(&c->k_auth, c->epoch, c->devnum,
		      (uint16_t)(interval_index(c, when_us) & 0xFFFFu),
		      RADIANT_SEC_COMPAT_SUBTYPE_ANNOUNCE, msg, len, out,
		      RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES);
}

int radiant_sec_compat_announce_tag(uint8_t ch, uint64_t now_us,
				    const uint8_t *msg, uint8_t len,
				    uint8_t out[RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES])
{
	struct compat_ctx *c = ctx_of(ch);

	if (!ctx_keyed(c)) {
		return RADIANT_SEC_ENOKEY;
	}
	if (msg == NULL || out == NULL || len != RADIANT_SEC_COMPAT_MSG_BYTES) {
		return RADIANT_SEC_EINVAL;
	}
	return announce_tag(c, now_us, msg, len, out);
}

int radiant_sec_compat_announce_verify(uint8_t ch, uint64_t t_sync,
				       const uint8_t *msg, uint8_t len,
				       const uint8_t *tag)
{
	struct compat_ctx *c = ctx_of(ch);
	uint8_t            want[RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES];
	int                rc;

	if (!ctx_keyed(c)) {
		return RADIANT_SEC_ENOKEY;
	}
	if (msg == NULL || tag == NULL || len != RADIANT_SEC_COMPAT_MSG_BYTES) {
		return RADIANT_SEC_EINVAL;
	}

	rc = announce_tag(c, t_sync, msg, len, want);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	/* No verdict hook and no counter here. An announcement is not an
	 * attestation: it says where the stream is going, not that the stream
	 * is authentic, and folding it into the stream verdict would let a
	 * refused announcement read as a stream that stopped verifying. */
	return (memcmp(want, tag, sizeof(want)) == 0) ? RADIANT_SEC_OK
						      : RADIANT_SEC_EBADMAC;
}

static int cmd_tag(struct compat_ctx *c, uint64_t when_us, const uint8_t *msg,
		   uint8_t len, uint8_t *out)
{
	struct radiant_sec_key k_cmd;
	int                    rc;

	/* Derived per call and destroyed rather than cached: a command is
	 * rare, and a key slot is the scarce resource on a node with no key
	 * store. */
	memset(&k_cmd, 0, sizeof(k_cmd));
	rc = radiant_sec_kdf(&c->k_root, RADIANT_SEC_LABEL_CMD, c->epoch,
			     c->base_devnum, &k_cmd);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	rc = tagged(&k_cmd, c->epoch, c->devnum,
		    (uint16_t)(interval_index(c, when_us) & 0xFFFFu),
		    RADIANT_SEC_COMPAT_SUBTYPE_COMMAND, msg, len, out,
		    RADIANT_SEC_COMPAT_CMD_TAG_BYTES);
	radiant_sec_key_destroy(&k_cmd);
	return rc;
}

int radiant_sec_compat_cmd_tag(uint8_t ch, uint64_t now_us, const uint8_t *msg,
			       uint8_t len,
			       uint8_t out[RADIANT_SEC_COMPAT_CMD_TAG_BYTES])
{
	struct compat_ctx *c = ctx_of(ch);
	int                rc;

	if (!ctx_keyed(c)) {
		return RADIANT_SEC_ENOKEY;
	}
	if (msg == NULL || out == NULL || len != RADIANT_SEC_COMPAT_CMD_BYTES) {
		return RADIANT_SEC_EINVAL;
	}
	rc = apply_wraps(c, interval_index(c, now_us) >> 16);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	return cmd_tag(c, now_us, msg, len, out);
}

int radiant_sec_compat_cmd_verify(uint8_t ch, uint64_t t_sync,
				  const uint8_t *msg, uint8_t len,
				  const uint8_t *tag)
{
	struct compat_ctx *c = ctx_of(ch);
	uint8_t            want[RADIANT_SEC_COMPAT_CMD_TAG_BYTES];
	int                rc;

	if (!ctx_keyed(c)) {
		return RADIANT_SEC_ENOKEY;
	}
	if (msg == NULL || tag == NULL || len != RADIANT_SEC_COMPAT_CMD_BYTES) {
		return RADIANT_SEC_EINVAL;
	}
	rc = apply_wraps(c, interval_index(c, t_sync) >> 16);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	rc = cmd_tag(c, t_sync, msg, len, want);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	return (memcmp(want, tag, sizeof(want)) == 0) ? RADIANT_SEC_OK
						      : RADIANT_SEC_EBADMAC;
}

/* What a candidate epoch is tested against; kept as one internal struct so
 * the three searches share one bounded loop instead of three near-copies. */
struct epoch_probe {
	const uint8_t *hint;    /* the beacon's, or NULL for a tag probe */
	const uint8_t *tag;
	const uint8_t *msgs;    /* Tier II's covered messages, or NULL */
	uint16_t       devnum;
	uint16_t       counter;
	uint8_t        n;
	uint8_t        taglen;
};

static bool probe_at(struct compat_ctx *c, const struct epoch_probe *p,
		     uint32_t epoch)
{
	uint8_t                got[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];
	struct radiant_sec_key k;
	bool                   ok;
	int                    rc;

	if (p->hint != NULL) {
		if (hint_of(&c->k_id, epoch, got) != RADIANT_SEC_OK) {
			return false;
		}
		return memcmp(got, p->hint, RADIANT_SEC_COMPAT_HINT_BYTES) == 0;
	}

	/* K_auth is epoch-bound, so a tag probe costs a derivation as well as
	 * a tag - which is why the hint path exists and is the cheap one. */
	memset(&k, 0, sizeof(k));
	if (radiant_sec_kdf(&c->k_root, RADIANT_SEC_LABEL_AUTH, epoch,
			    c->base_devnum, &k) != RADIANT_SEC_OK) {
		return false;
	}
	if (p->msgs != NULL) {
		rc = radiant_sec_compat_tier2_tag(&k, epoch, p->devnum,
						  p->counter, p->msgs, p->n,
						  got);
	} else {
		rc = radiant_sec_compat_tier1_tag(&k, epoch, p->devnum,
						  p->counter, got);
	}
	ok = (rc == RADIANT_SEC_OK) && (memcmp(got, p->tag, p->taglen) == 0);
	radiant_sec_key_destroy(&k);
	return ok;
}

/*
 * Three bounded phases: forward (the ordinary case - sensor hasn't
 * rebooted), backward (small, for a candidate that never confirmed), then
 * an absolute scan from 0 (the re-provisioned/reset-strap case). `ops`
 * counts across all three and every phase has a ceiling, so a failure costs
 * bounded time rather than a receiver that never comes back.
 */
static int epoch_search(struct compat_ctx *c, const struct epoch_probe *p,
			uint32_t last_seen, uint32_t *found, uint32_t *ops,
			uint8_t *path)
{
	uint32_t n_ops = 0u;
	uint32_t hit = 0u;
	uint32_t i;
	uint32_t e;

	*path = RADIANT_SEC_COMPAT_PATH_NONE;

	for (i = 0u; i <= RADIANT_SEC_COMPAT_EPOCH_FORWARD; i++) {
		if (last_seen > 0xFFFFFFFFu - i) {
			break;
		}
		e = last_seen + i;
		n_ops++;
		if (probe_at(c, p, e)) {
			*path = RADIANT_SEC_COMPAT_PATH_FORWARD;
			hit = e;
			goto done;
		}
	}
	for (i = 1u; i <= RADIANT_SEC_COMPAT_EPOCH_BACKWARD && i <= last_seen;
	     i++) {
		e = last_seen - i;
		n_ops++;
		if (probe_at(c, p, e)) {
			*path = RADIANT_SEC_COMPAT_PATH_BACKWARD;
			hit = e;
			goto done;
		}
	}
	for (e = 0u; e <= RADIANT_SEC_COMPAT_EPOCH_SCAN; e++) {
		n_ops++;
		if (probe_at(c, p, e)) {
			*path = RADIANT_SEC_COMPAT_PATH_SCAN;
			hit = e;
			goto done;
		}
	}

	*ops = n_ops;
	return RADIANT_SEC_EBADMAC;

done:
	*found = hit;
	*ops = n_ops;
	stat_bump(&c->stats.epoch_recoveries);
	return RADIANT_SEC_OK;
}

static int recover(uint8_t ch, const struct epoch_probe *p, uint32_t last_seen,
		   uint32_t *found, uint32_t *ops, uint8_t *path)
{
	struct compat_ctx *c = ctx_of(ch);
	uint32_t           scratch_ops;
	uint8_t            scratch_path;

	if (c == NULL || !c->used) {
		return RADIANT_SEC_ENOKEY;
	}
	if (found == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	if (ops == NULL) {
		ops = &scratch_ops;
	}
	if (path == NULL) {
		path = &scratch_path;
	}
	return epoch_search(c, p, last_seen, found, ops, path);
}

int radiant_sec_compat_recover_hint(uint8_t ch,
				    const uint8_t hint[RADIANT_SEC_COMPAT_HINT_BYTES],
				    uint32_t last_seen, uint32_t *found,
				    uint32_t *ops, uint8_t *path)
{
	struct epoch_probe p;

	if (hint == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	memset(&p, 0, sizeof(p));
	p.hint = hint;
	return recover(ch, &p, last_seen, found, ops, path);
}

int radiant_sec_compat_recover_tier1(uint8_t ch, uint16_t devnum,
				     uint16_t att_counter,
				     const uint8_t tag[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES],
				     uint32_t last_seen, uint32_t *found,
				     uint32_t *ops, uint8_t *path)
{
	struct epoch_probe p;

	if (tag == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	memset(&p, 0, sizeof(p));
	p.tag = tag;
	p.devnum = devnum;
	p.counter = att_counter;
	p.taglen = RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES;
	return recover(ch, &p, last_seen, found, ops, path);
}

int radiant_sec_compat_recover_tier2(uint8_t ch, uint16_t devnum,
				     uint16_t window_index, const uint8_t *msgs,
				     uint8_t n,
				     const uint8_t tag[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES],
				     uint32_t last_seen, uint32_t *found,
				     uint32_t *ops, uint8_t *path)
{
	struct epoch_probe p;

	if (tag == NULL || msgs == NULL || !window_is_legal(n)) {
		return RADIANT_SEC_EINVAL;
	}
	memset(&p, 0, sizeof(p));
	p.tag = tag;
	p.msgs = msgs;
	p.n = n;
	p.devnum = devnum;
	p.counter = window_index;
	p.taglen = RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES;
	return recover(ch, &p, last_seen, found, ops, path);
}

int radiant_sec_compat_adopt_epoch(uint8_t ch, uint32_t epoch)
{
	struct compat_ctx *c = ctx_of(ch);

	if (c == NULL || !c->used) {
		return RADIANT_SEC_ENOKEY;
	}
	if (!c->epoch_set) {
		/* There is no anchor and no period yet, so there is nothing for
		 * a recovered epoch to be a correction to. */
		return RADIANT_SEC_EINVAL;
	}
	if (epoch > 0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM) {
		return RADIANT_SEC_EINVAL;
	}

	c->base_epoch = epoch;
	c->wraps = 0u;
	c->epoch = epoch;

	/* Everything in flight belonged to the previous epoch, and the replay
	 * high-water mark most of all: a fresh epoch has its own counter space,
	 * and judging it against the old one's mark would reject every page. */
	c->rx_absorbed = 0u;
	c->rx_overflow = false;
	c->rx_t1_any = false;
	c->rx_t1_ok = false;
	c->last_verdict = RADIANT_SEC_VERDICT_CLEAR;

	return derive_auth(c);
}

#endif /* CONFIG_RADIANT_SEC_COMPAT */
