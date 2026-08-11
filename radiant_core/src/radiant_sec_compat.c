/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_compat - the two attestation tags of docs/radiant-security.md
 * section 11.4, as bytes in and bytes out.
 *
 * Provenance: clean-room, and in this file's case the phrase is nearly
 * redundant - it is derived entirely from this project's own specifications,
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md and
 * docs/radiant-security.md section 11.4, on top of the seam in
 * radiant_core/include/radiant_core/radiant_sec.h. NOTHING HERE DERIVES FROM
 * ANY ANT+ DEVICE PROFILE DOCUMENT, gated or otherwise, and it could not: this
 * file names no page and no field, so there is nothing in it an external
 * profile specification could have said. It derives nothing from sdk-ant, from
 * libant.a, or from disassembly of any binary. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ── Two halves, and the first one is still pure ────────────────────────────
 *
 * The three tag functions retain nothing between calls: arguments in, tag out.
 * Everything below them holds per-channel state - when a page is due, what the
 * window covers, which counters have been seen, which epoch a receiver is on -
 * and the split is worth keeping visible, because the pure half is what the
 * pinned vectors check and the stateful half is what a running node exercises.
 *
 * ── What is still deliberately absent ──────────────────────────────────────
 *
 *   - Any page number. The emit hook returns a SUBTYPE and fills bytes [1..7];
 *     the layer above composes byte [0]. So the page byte is derived from what
 *     went into the nonce rather than stated twice, and this file can be read
 *     without knowing which profile is on the air.
 *   - Any field name. The messages this file covers are eight opaque bytes.
 *   - Any clock. Every instant is an argument. The .h says why.
 *   - Any scheduler. Nothing here decides when a slot happens; it answers
 *     whether the slot it was told about owes an attestation page.
 *
 * radiant_core/include/radiant_core/radiant_sec_compat.h carries the reasoning;
 * this file carries the bytes.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>

#include <radiant_core/radiant_sec_compat.h>

#if defined(CONFIG_RADIANT_SEC_COMPAT)

/*
 * N in {4, 8, 16, 32} - inside the bounds with a single bit set, which is that
 * set spelled as an arithmetic property rather than as a table. Enumerating it
 * matters because N is simultaneously the airtime cost, the verification
 * latency and the amplification factor, and a caller reaching this with 5 has
 * one of three bugs rather than a rounding error.
 */
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

	/* Built by the shared helper and then extended, rather than laid out
	 * again here. Two byte layouts that must agree are two byte layouts
	 * that eventually do not, and the domain byte and the six trailing
	 * zeros are exactly the part a second copy would drift on. */
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
	/*
	 * base_epoch is what was set or adopted; `wraps` is how many times the
	 * attestation counter has rolled over since, and the epoch actually in
	 * the nonce is the sum. Keeping them apart is what makes a wrap
	 * reproducible from time alone on both sides rather than being a piece
	 * of history a receiver has to have witnessed.
	 */
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

/*
 * No channel -> context map array. radiant_sec.c keeps one because it allocates
 * eight contexts and is on the RX hot path; two contexts are searched in two
 * comparisons, and skipping the map is what keeps this file free of
 * radiant_channel.h and therefore of the HAL header behind it.
 */
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

/*
 * Tier I's counter: intervals of T seconds since the epoch anchor.
 *
 * DERIVED FROM TIME AND CARRIED EXPLICITLY, which is what "decoupled from the
 * data rate" means in arithmetic. Both sides compute it from their own clocks
 * and the page carries all sixteen low bits, so what a receiver has to get
 * right from time is only which rollover it is in - and that is a seven-day
 * question at the default T, not a per-packet one.
 */
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
 * The full counter, from the low bits on the air and the index time expects:
 * pick the rollover nearest to what time says. `span` is 0x10000 for Tier I,
 * whose page carries two bytes, and 0x100 for Tier II, whose page carries one.
 *
 * This is radiant_sec.c's resolve_counter() generalised in width rather than
 * copied, and the reason it has to exist at all is the same: a receiver joining
 * mid-stream is the NORMAL case and has no arrival history to count from.
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
 * D14, on the compat surface: a counter wrap advances the epoch by one on both
 * sides and re-derives the keys.
 *
 * ONE COUNTER DRIVES IT, AND IT IS TIER I'S. The nonce has one 16-bit counter
 * field and the two tiers put different quantities in it - an interval index
 * and a window index - which roll over at different times, so exactly one of
 * them can be the epoch's clock or the two sides will disagree about which
 * epoch they are in. Tier I's is the one, because it is a property of elapsed
 * time and of nothing else: it is reproducible on a receiver that has heard no
 * Tier II page, on a node with Tier II switched off, and after any amount of
 * loss. The window index simply rolls over inside an epoch, which costs
 * nothing - a repeated MAC nonce is not a repeated keystream, and a replayed
 * old window fails the receiver's time-derived index check long before its tag
 * is reached.
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

	/*
	 * Pinned. From here the answer is UNVERIFIED unless something recent
	 * says otherwise, which is the whole of downgrade protection: strip the
	 * attestation off the air and a pinned receiver notices, where a naive
	 * one would quietly fall back to clear and call it normal.
	 */
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

	/*
	 * TIER II FIRST WHEN BOTH FALL DUE. Its window closes at the Nth
	 * transmitted message and has no slack anywhere; Tier I's counter is
	 * derived from elapsed time, so a slot of slip is invisible to it.
	 */
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
		/* More messages than this window covers: the tag page was lost
		 * or something was injected. Either way the window cannot be
		 * judged, and saying so is cheaper and truer than tagging N
		 * messages and reporting a mismatch. */
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

	/*
	 * REPLAY IS CLOSED BY THE COUNTER AND BY NOTHING ELSE. The tag covers
	 * no payload, so a recorded page is a perfectly valid tag forever; what
	 * makes it worthless is that its counter has already been spent. The
	 * check is before the MAC because a replay is not a forgery and must
	 * not be counted as one - and because refreshing the freshness anchor
	 * from a recording would let an attacker hold a stream "verified" after
	 * the sensor had gone.
	 */
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

	/*
	 * The window is judged on what arrived, and a hole in it is not a
	 * forgery: a window CMAC is not self-synchronising under loss the way
	 * the spread tag is, and that regression is the honest reason this tier
	 * is off by default.
	 */
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

	/*
	 * EVERYTHING BUT THE TAG PAGE IS COVERED, including the Tier I page,
	 * the beacon and the common pages: the window is N consecutive
	 * TRANSMITTED messages rather than N data messages, which is what keeps
	 * a receiver counting messages instead of deciding which ones were
	 * "data" - a decision it would have to make with profile knowledge this
	 * layer does not have.
	 */
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

/*
 * What a candidate epoch is tested against. An internal struct, which the
 * no-structs-in-signatures rule permits and in fact wants: the alternative is
 * three near-identical searches, and three copies of a bounded loop is three
 * places for one of the bounds to go missing.
 */
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
 * The three bounded phases, in the order the cost table makes sense in.
 *
 * Forward first, because being behind is the ordinary condition and
 * `last_seen` itself is the ordinary answer - a sensor that has not rebooted
 * since. The backward probe is second and small; it is for a receiver that
 * recorded a candidate which never confirmed, not for a sensor that moved
 * backwards. The absolute scan is last and is the re-provisioned case: a strap
 * whose boot counter was reset is at a small epoch, so it is found in a handful
 * of operations once the search stops assuming forward motion.
 *
 * EVERY PHASE HAS A CEILING AND `ops` COUNTS ACROSS ALL THREE, so a caller can
 * hold the number against the published table and a failure costs a bounded
 * amount of time rather than a receiver that never comes back.
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
