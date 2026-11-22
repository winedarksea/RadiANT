/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Pairing: turning one X25519 exchange into the 16 bytes every other file in
 * this feature already knows how to use.
 *
 * Clean-room: docs/radiant-security.md sections 7.4 and 8, RFC 7748 for the
 * exchange, NIST SP 800-56C's extract-then-expand shape for the derivation,
 * and the commit/reveal shape of Bluetooth LE Secure Connections numeric
 * comparison (its f4 and g2) for the fingerprint. Only the shape is borrowed:
 * both functions here are AES-CMAC, the one MAC this build has.
 *
 * Pairing happens in the clear, structurally: no prior secret and no
 * out-of-band channel, so an active attacker present during the exchange can
 * MITM it - that's what an anonymous key agreement is (section 7.4). The
 * mitigation is the FINGERPRINT: six digits shown at both ends for a human to
 * compare.
 *
 * A fingerprint over the keys alone is not enough. A MITM sees both real
 * public keys before it has to send its own, so it can grind key pairs offline
 * (about 10^6 X25519 operations, seconds on a laptop) until the two six-digit
 * values agree (security review 2026-11-13, item 4). Hence a commitment:
 *
 *   1. Public keys cross (SCALAR, EXCHANGE). EXCHANGE computes and keeps the
 *      DH result and installs nothing.
 *   2. The responder sends Cb = f4(PKb, PKa, Nb) = CMAC_Nb(PKb || PKa || 0x00).
 *   3. The initiator reveals Na.
 *   4. The responder reveals Nb; the initiator checks it against Cb.
 *   5. Both show fp = g2(PKa, PKb, Na, Nb)
 *                   = (CMAC_Na(PKa || PKb || Nb) mod 2^32) mod 10^6.
 *   6. K_root = CMAC_PRK(label || PKa || PKb || Na || Nb), where PRK is the
 *      DH result condensed. Installed only now.
 *
 * Whoever sits in the middle has to fix one nonce before it learns the other
 * side's: towards the initiator it is bound by its commitment before Na
 * arrives, and towards the responder it must reveal its Na before Nb arrives.
 * So a completed MITM shows matching digits with probability 10^-6 per
 * user-initiated pairing attempt. Not "per window": an attacker who would
 * lose simply withholds Nb, A times out, and the user tries again - so an
 * abort or timeout on one end while the other shows digits is a FAILED
 * pairing, never a cue to retry until it matches.
 *
 * The key is live once PEER_NONCE answers, before the human compares. On a
 * mismatch or an abort the host must remove it: radiant_sec_pair_leave() does
 * NOT (it wipes the exchange only); radiant_sec_channel_release() does, which
 * over the serial protocol is closing the channel and, after
 * EVENT_CHANNEL_CLOSED, sending MESG_UNASSIGN_CHANNEL (0x41) and checking it
 * answered NO_ERROR: an unassign sent before the close completes is refused.
 * And "B reveals Nb only after its PEER_NONCE" is the responder host's
 * obligation: the node never sees Nb leave, so it cannot enforce it.
 *
 * Over-air enrolment (profile_enrol.c) has no room for the commit/reveal round
 * trip and calls radiant_sec_pair_peer_uncommitted() (sensor) and
 * radiant_sec_pair_join() / _accept_root() (receiver) instead, which the 0xF5
 * host surface does not reach: unauthenticated DH whose fingerprint is
 * one-sided and grindable. Its DH key only wraps the sensor's existing root
 * for delivery; it replaces nobody's. See radiant_sec.h.
 *
 * No exclusivity flag, deliberately: unlike sdk-ant's single global
 * "one channel negotiates at a time" lock, each channel's state here is its
 * own. If a flag is ever added, it must be released on LOSS OF TRACKING
 * (RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH), not only on completion/failure -
 * a channel whose peer walks out of range mid-exchange gets neither event,
 * and missing this would deadlock negotiation device-wide. See section 8.1.
 * radiant_sec_pair_on_search() is the hook for this, already wired in.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <radiant/radiant_sec.h>
#include <radiant/radiant_sec_compat.h>
#include <radiant/radiant_channel.h>

#define PAIR_MAX_CH CONFIG_RADIANT_SEC_MAX_CHANNELS

#define NONCE_BYTES RADIANT_SEC_PAIR_NONCE_BYTES
#define PUB_BYTES   RADIANT_SEC_X25519_BYTES

/* The root key's domain string. The fingerprint needs none: g2 is keyed by a
 * nonce, not by anything derived from the shared secret, so the six digits on
 * a screen are a function of public values only and cannot narrow the key
 * they attest to. */
#define LABEL_ROOT "RADIANT-pair-root"
/* Over-air enrolment's root, its own label so the uncommitted mode can never
 * produce a committed-mode key even for equal inputs. Same length as
 * LABEL_ROOT, so pair_finish() takes either without a length. */
#define LABEL_ENROL "RADIANT-enrl-root"
/* Enrolment's pairwise wrap keys, encryption and tag (radiant_sec.h, "the
 * group root, delivered"). */
#define LABEL_WRAP  "RADIANT-enrl-wrap"
#define LABEL_WMAC  "RADIANT-enrl-wmac"
BUILD_ASSERT(sizeof(LABEL_ROOT) == sizeof(LABEL_ENROL));
BUILD_ASSERT(sizeof(LABEL_WRAP) == sizeof(LABEL_ROOT) &&
	     sizeof(LABEL_WMAC) == sizeof(LABEL_ROOT));

/* Where one channel's pairing has got to. Every call names the one step it
 * accepts, so an out-of-order call is refused rather than interpreted. */
enum pair_step {
	STEP_OPEN = 0,  /* entered; SCALAR may arrive */
	STEP_EXCHANGED, /* DH done, PRK held, scalar spent, nothing installed */
	STEP_NONCE,     /* our nonce held (responder: commitment handed out) */
	STEP_COMMITTED, /* initiator only: the responder's commitment held */
	STEP_JOINED,    /* enrolment receiver: DH done, fp readable, PRK held
			 * until the wrapped root opens */
	STEP_DONE,      /* key installed or delivered, fingerprint readable */
};

struct pair_ctx {
	uint8_t  ch;
	bool     in_use;
	bool     open;          /* pairing mode entered */
	bool     have_scalar;
	bool     have_local;
	uint8_t  step;          /* enum pair_step */
	uint8_t  role;          /* RADIANT_SEC_PAIR_INITIATOR / _RESPONDER */

	uint32_t deadline_ms;   /* 0 = no timeout */

	/* The scalar until EXCHANGE spends it, the peer's public key after.
	 * One slot because the two never coexist: the call that consumes the
	 * one delivers the other, and overwriting the scalar with a public
	 * value is its wipe. */
	union {
		uint8_t scalar[PUB_BYTES];
		uint8_t peer_pub[PUB_BYTES];
	} k;
	uint8_t  local_pub[PUB_BYTES];
	/* The secrets of the commit/reveal, together so one wipe takes them. */
	struct {
		uint8_t prk[RADIANT_SEC_BLOCK_BYTES];    /* condensed DH result */
		uint8_t nonce[NONCE_BYTES];              /* ours */
		uint8_t commit[RADIANT_SEC_BLOCK_BYTES]; /* initiator: peer's Cb */
	} s;
	uint32_t fingerprint;   /* valid at STEP_DONE */
};

static struct pair_ctx pair[PAIR_MAX_CH];
static int8_t          pair_of[RADIANT_CHANNEL_COUNT];
static bool            pair_map_ready;

static void pair_map_init(void)
{
	uint8_t i;

	if (pair_map_ready) {
		return;
	}
	for (i = 0u; i < (uint8_t)RADIANT_CHANNEL_COUNT; i++) {
		pair_of[i] = -1;
	}
	pair_map_ready = true;
}

static struct pair_ctx *ctx_of(uint8_t ch)
{
	pair_map_init();
	if (ch >= (uint8_t)RADIANT_CHANNEL_COUNT || pair_of[ch] < 0) {
		return NULL;
	}
	return &pair[pair_of[ch]];
}

static struct pair_ctx *ctx_alloc(uint8_t ch)
{
	struct pair_ctx *c = ctx_of(ch);
	uint8_t          i;

	if (c != NULL) {
		return c;
	}
	if (ch >= (uint8_t)RADIANT_CHANNEL_COUNT) {
		return NULL;
	}
	for (i = 0u; i < (uint8_t)PAIR_MAX_CH; i++) {
		if (!pair[i].in_use) {
			memset(&pair[i], 0, sizeof(pair[i]));
			pair[i].in_use = true;
			pair[i].ch = ch;
			pair_of[ch] = (int8_t)i;
			return &pair[i];
		}
	}
	return NULL;
}

/* Wipe everything secret and give the slot back: the scalar, the PRK, our
 * nonce and the peer's commitment. Called on leave, on timeout, on loss of
 * tracking, on reset and on a commitment mismatch - one function so those
 * five cannot drift into clearing different subsets. */
static void ctx_free(struct pair_ctx *c)
{
	if (c == NULL) {
		return;
	}
	if (c->ch < (uint8_t)RADIANT_CHANNEL_COUNT) {
		pair_of[c->ch] = -1;
	}
	radiant_sec_wipe(c, sizeof(*c));
}

static bool pair_expired(const struct pair_ctx *c, uint32_t now_ms)
{
	if (c->deadline_ms == 0u) {
		return false;
	}
	/* Unsigned difference, so this survives the 32-bit millisecond wrap
	 * every 49 days rather than expiring everything at it. */
	return (uint32_t)(now_ms - c->deadline_ms) < 0x80000000u;
}

/* The channel's context if pairing mode is open and inside its window. An
 * expired one is freed on the way. */
static struct pair_ctx *ctx_live(uint8_t ch)
{
	struct pair_ctx *c = ctx_of(ch);

	if (c == NULL || !c->open) {
		return NULL;
	}
	if (pair_expired(c, k_uptime_get_32())) {
		ctx_free(c);
		return NULL;
	}
	return c;
}

struct cm_part {
	const uint8_t *p;
	size_t         n;
};

/* CMAC under a raw 16-byte key over the concatenation of `parts`. Every
 * function in this file is one of these: extract, f4, g2 and the root. */
static int cmac_parts(const uint8_t *key, const struct cm_part *parts,
		      size_t count, uint8_t *tag)
{
	struct radiant_sec_key      k;
	struct radiant_sec_cmac_ctx cm;
	size_t                      i;
	int                         rc;

	memset(&k, 0, sizeof(k));
	rc = radiant_sec_key_import(&k, key, 128u);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	rc = radiant_sec_cmac_init(&cm, &k);
	for (i = 0u; i < count && rc == RADIANT_SEC_OK; i++) {
		rc = radiant_sec_cmac_update(&cm, parts[i].p, parts[i].n);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_final(&cm, tag);
	}
	radiant_sec_key_destroy(&k);
	radiant_sec_wipe(&cm, sizeof(cm));
	return rc;
}

/* f4(PKb, PKa, Nb) = CMAC_Nb(PKb || PKa || 0x00): the responder's
 * commitment to its nonce, bound to both public keys. */
static int pair_f4(const uint8_t *pkb, const uint8_t *pka, const uint8_t *nb,
		   uint8_t *out)
{
	static const uint8_t z;
	const struct cm_part parts[] = {
		{ pkb, PUB_BYTES }, { pka, PUB_BYTES }, { &z, 1u },
	};

	return cmac_parts(nb, parts, ARRAY_SIZE(parts), out);
}

/* g2(PKa, PKb, Na, Nb) = (CMAC_Na(PKa || PKb || Nb) mod 2^32) mod 10^6, the
 * tag read as a 128-bit big-endian integer, so "mod 2^32" is its last four
 * bytes. Six digits: a 1-in-10^6 chance per attempt that a user will actually
 * read and compare; eight would raise the work factor and lower completion,
 * and that effect dominates. */
static int pair_g2(const uint8_t *pka, const uint8_t *pkb, const uint8_t *na,
		   const uint8_t *nb, uint32_t *fp)
{
	const struct cm_part parts[] = {
		{ pka, PUB_BYTES }, { pkb, PUB_BYTES }, { nb, NONCE_BYTES },
	};
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	int     rc;

	rc = cmac_parts(na, parts, ARRAY_SIZE(parts), tag);
	if (rc == RADIANT_SEC_OK) {
		*fp = (((uint32_t)tag[12] << 24) | ((uint32_t)tag[13] << 16) |
		       ((uint32_t)tag[14] << 8) | (uint32_t)tag[15]) %
		      1000000u;
	}
	return rc;
}

/* Channels whose peer was lost, to be released under the port's lock. */
static atomic_t pair_lost;

int radiant_sec_pair_enter(uint8_t ch, uint8_t timeout_s)
{
	RADIANT_SEC_ASSERT_LOCKED();

	/* A loss recorded before this pairing belongs to whatever came before
	 * it, and must not release it at the next service. */
	if (ch < (uint8_t)RADIANT_CHANNEL_COUNT) {
		(void)atomic_and(&pair_lost, ~(atomic_val_t)(1u << ch));
	}

	/* Always a fresh context: re-entering must not revive an old step
	 * with its PRK, nonce or scalar still in it. */
	ctx_free(ctx_of(ch));

	struct pair_ctx *c = ctx_alloc(ch);

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}

	/* A timeout is not optional: a node left in pairing mode accepts a
	 * key from anyone. Zero means the default, not "forever". */
	if (timeout_s == 0u) {
		timeout_s = RADIANT_SEC_PAIR_TIMEOUT_DEFAULT_S;
	}
	c->open = true;
	c->deadline_ms = k_uptime_get_32() + ((uint32_t)timeout_s * 1000u);
	if (c->deadline_ms == 0u) {
		c->deadline_ms = 1u;   /* 0 is the "no timeout" sentinel */
	}
	return RADIANT_SEC_OK;
}

void radiant_sec_pair_leave(uint8_t ch)
{
	RADIANT_SEC_ASSERT_LOCKED();
	ctx_free(ctx_of(ch));
}

void radiant_sec_pair_on_search(uint8_t ch)
{
	/* Loss of tracking: peer is gone mid-exchange, no completion/failure
	 * event will ever arrive, so this is the only release point. Called
	 * from the radio callback, so it only records the fact: freeing here
	 * would zero local_pub and the scalar under a pair_peer() that is in
	 * the middle of its 375 ms X25519 (it would then derive a PRK from
	 * zeros and leave the channel silently dead). */
	if (ch < (uint8_t)RADIANT_CHANNEL_COUNT) {
		(void)atomic_or(&pair_lost, (atomic_val_t)(1u << ch));
	}
}

void radiant_sec_pair_service(void)
{
	uint32_t now = k_uptime_get_32();
	uint8_t  ch;

	RADIANT_SEC_ASSERT_LOCKED();
	/* Expiry, eagerly: a window nobody touches again must not keep its
	 * scalar, PRK and nonce in RAM until the next pairing call. */
	for (ch = 0u; ch < (uint8_t)PAIR_MAX_CH; ch++) {
		if (pair[ch].in_use && pair_expired(&pair[ch], now)) {
			ctx_free(&pair[ch]);
		}
	}
	if (atomic_get(&pair_lost) == 0) {
		return;
	}
	for (ch = 0u; ch < (uint8_t)RADIANT_CHANNEL_COUNT; ch++) {
		if ((atomic_and(&pair_lost, ~(atomic_val_t)(1u << ch)) &
		     (atomic_val_t)(1u << ch)) != 0) {
			ctx_free(ctx_of(ch));
		}
	}
}

int radiant_sec_pair_set_scalar(uint8_t ch, const uint8_t *scalar)
{
	RADIANT_SEC_ASSERT_LOCKED();
	struct pair_ctx *c;
	int              rc;

	if (scalar == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	c = ctx_live(ch);
	if (c == NULL || c->step != (uint8_t)STEP_OPEN) {
		return RADIANT_SEC_EINVAL;
	}
	if (!radiant_sec_caps()->has_x25519) {
		return RADIANT_SEC_ENOTSUP;
	}

	/*
	 * The scalar comes from the host - a limitation, not a preference:
	 * the only entropy source on nRF54L is psa_rng/CRACEN, and reaching
	 * it drags nrf_security into every build. A host-less node must be
	 * provisioned out of band instead. Nothing here can check the scalar
	 * is actually random.
	 */
	memcpy(c->k.scalar, scalar, PUB_BYTES);
	c->have_scalar = true;

	rc = radiant_sec_x25519(c->local_pub, c->k.scalar,
				radiant_sec_x25519_basepoint);
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_wipe(c->k.scalar, sizeof(c->k.scalar));
		c->have_scalar = false;
		return rc;
	}
	c->have_local = true;
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_local_pubkey(uint8_t ch, uint8_t *out)
{
	RADIANT_SEC_ASSERT_LOCKED();
	const struct pair_ctx *c = ctx_of(ch);

	if (c == NULL || out == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	if (!c->have_local) {
		return RADIANT_SEC_EINVAL;
	}
	memcpy(out, c->local_pub, PUB_BYTES);
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_peer(uint8_t ch, const uint8_t *peer_pub)
{
	RADIANT_SEC_ASSERT_LOCKED();
	static const uint8_t zero[RADIANT_SEC_BLOCK_BYTES];
	struct pair_ctx     *c;
	uint8_t              shared[PUB_BYTES];
	const struct cm_part part = { shared, PUB_BYTES };
	int                  rc;

	if (peer_pub == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	/* One scalar, one exchange: a successful exchange spends the scalar,
	 * so a second one (a host retrying after a lost reply) has nothing to
	 * run on (review 2026-10-31, D1-2). */
	c = ctx_live(ch);
	if (c == NULL || c->step != (uint8_t)STEP_OPEN || !c->have_local ||
	    !c->have_scalar) {
		return RADIANT_SEC_EINVAL;
	}

	rc = radiant_sec_x25519(shared, c->k.scalar, peer_pub);
	if (rc != RADIANT_SEC_OK) {
		goto out;
	}

	/* A small-order peer key makes the shared secret all zeros regardless
	 * of our scalar. RFC 7748 leaves rejecting it optional; mandatory
	 * here since this becomes a root key. Refused with the scalar intact,
	 * so one injected key does not burn the window. */
	if (radiant_sec_x25519_is_degenerate(shared)) {
		rc = RADIANT_SEC_EINVAL;
		goto out;
	}

	/* Extract: the raw X25519 output is a curve point's u-coordinate, not
	 * a uniform key, so CMAC under an all-zero key condenses it into 16
	 * uniform bytes. Only this is kept; the expand waits for the nonces. */
	rc = cmac_parts(zero, &part, 1u, c->s.prk);
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_wipe(c->s.prk, sizeof(c->s.prk));
		goto out;
	}

	/* The scalar is spent, and the peer's key takes its slot. */
	memcpy(c->k.peer_pub, peer_pub, PUB_BYTES);
	c->have_scalar = false;
	c->step = (uint8_t)STEP_EXCHANGED;

out:
	radiant_sec_wipe(shared, sizeof(shared));
	return rc;
}

/* The device number the key will be bound to, or 0 if the channel has none.
 * Checked at the nonce step as well as at the install, so nothing after a
 * commitment can fail in a way that leaves the step open for a retry. */
static uint16_t pair_devnum(uint8_t ch)
{
	struct radiant_channel_id id;

	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return 0u;
	}
	return id.device_number;
}

/* An all-zero nonce is the uncommitted mode's constant, and a host that sends
 * one has no random source; either way it is refused. */
static bool nonce_is_zero(const uint8_t *n)
{
	static const uint8_t zero[NONCE_BYTES];

	return radiant_sec_ct_equal(n, zero, NONCE_BYTES);
}

/*
 * Expand and install: the last step of both paths. `pka`, `pkb`, `na` and
 * `nb` are in protocol order (initiator first), whichever end this is.
 */
static int pair_finish(struct pair_ctx *c, const char *label,
		       const uint8_t *pka, const uint8_t *pkb,
		       const uint8_t *na, const uint8_t *nb,
		       uint32_t *fingerprint)
{
	uint16_t                  devnum = pair_devnum(c->ch);
	uint8_t                   root[RADIANT_SEC_BLOCK_BYTES];
	uint32_t                  fp = 0u;
	const struct cm_part      parts[] = {
		{ (const uint8_t *)label, sizeof(LABEL_ROOT) },
		{ pka, PUB_BYTES }, { pkb, PUB_BYTES },
		{ na, NONCE_BYTES }, { nb, NONCE_BYTES },
	};
	int                       rc;

	/* Device number read from the channel, not the wire, since it's bound
	 * into the KDF (same reasoning as radiant_ant_sec_key_set()). Also checked at
	 * set_nonce; if it fails here anyway (the ID changed since), the
	 * callers free the context rather than leave a step to retry. */
	if (devnum == 0u) {
		return RADIANT_SEC_ENOKEY;
	}

	rc = pair_g2(pka, pkb, na, nb, &fp);
	if (rc == RADIANT_SEC_OK) {
		rc = cmac_parts(c->s.prk, parts, ARRAY_SIZE(parts), root);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_set_key(c->ch, root, 128u, devnum);
	}
	radiant_sec_wipe(root, sizeof(root));
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	(void)radiant_sec_set_devnum(c->ch, devnum);

	/* Counted HERE rather than in the caller: this is the one place a
	 * pairing has actually taken, so "exactly once per enrolment" is
	 * structural rather than a rule each caller must remember. */
	radiant_sec_stat_enrolment(c->ch);

	c->fingerprint = fp;
	if (fingerprint != NULL) {
		*fingerprint = fp;
	}

	/* Everything secret has done its one job. Pairing mode stays open until
	 * the host leaves it or the timeout fires, so the fingerprint can still
	 * be read. */
	radiant_sec_wipe(&c->s, sizeof(c->s));
	c->step = (uint8_t)STEP_DONE;
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_set_nonce(uint8_t ch, uint8_t role, const uint8_t *nonce,
			       uint8_t *commit_out)
{
	RADIANT_SEC_ASSERT_LOCKED();
	struct pair_ctx *c;
	int              rc;

	BUILD_ASSERT(RADIANT_SEC_PAIR_INITIATOR == 0u &&
		     RADIANT_SEC_PAIR_RESPONDER == 1u);
	if (nonce == NULL || role > RADIANT_SEC_PAIR_RESPONDER ||
	    (role == RADIANT_SEC_PAIR_RESPONDER && commit_out == NULL) ||
	    nonce_is_zero(nonce)) {
		return RADIANT_SEC_EINVAL;
	}
	/* After EXCHANGE, because the commitment covers the peer's key, and
	 * exactly once: a second nonce would be a second commitment. */
	c = ctx_live(ch);
	if (c == NULL || c->step != (uint8_t)STEP_EXCHANGED) {
		return RADIANT_SEC_EINVAL;
	}
	/* The install needs a device number; refuse now, with nothing
	 * committed, rather than fail after the commitment is out. */
	if (pair_devnum(ch) == 0u) {
		return RADIANT_SEC_ENOKEY;
	}

	if (role == RADIANT_SEC_PAIR_RESPONDER) {
		/* Cb = f4(PKb, PKa, Nb), this end being B. */
		rc = pair_f4(c->local_pub, c->k.peer_pub, nonce, commit_out);
		if (rc != RADIANT_SEC_OK) {
			return rc;
		}
	}
	memcpy(c->s.nonce, nonce, NONCE_BYTES);
	c->role = role;
	c->step = (uint8_t)STEP_NONCE;
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_peer_commit(uint8_t ch, const uint8_t *commit)
{
	RADIANT_SEC_ASSERT_LOCKED();
	struct pair_ctx *c;

	if (commit == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	/* Initiator only, after its own nonce and before the peer's: a
	 * commitment accepted after the reveal commits to nothing. */
	c = ctx_live(ch);
	if (c == NULL || c->step != (uint8_t)STEP_NONCE ||
	    c->role != RADIANT_SEC_PAIR_INITIATOR) {
		return RADIANT_SEC_EINVAL;
	}
	memcpy(c->s.commit, commit, sizeof(c->s.commit));
	c->step = (uint8_t)STEP_COMMITTED;
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_peer_nonce(uint8_t ch, const uint8_t *peer_nonce,
				uint32_t *fingerprint)
{
	RADIANT_SEC_ASSERT_LOCKED();
	struct pair_ctx *c;
	uint8_t          expect[RADIANT_SEC_BLOCK_BYTES];
	bool             init;
	int              rc;

	if (peer_nonce == NULL || nonce_is_zero(peer_nonce)) {
		return RADIANT_SEC_EINVAL;
	}
	c = ctx_live(ch);
	if (c == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	init = (c->role == RADIANT_SEC_PAIR_INITIATOR);
	/* A takes the reveal only after the commitment; B right after its own
	 * commitment went out. B has nothing to check: its protection is that
	 * Nb was fixed before Na was seen. */
	if (c->step != (uint8_t)(init ? STEP_COMMITTED : STEP_NONCE)) {
		return RADIANT_SEC_EINVAL;
	}

	if (init) {
		/* A, receiving Nb: it must open the commitment B sent before it
		 * saw Na. A mismatch is a peer that changed its nonce after the
		 * fact, which is the grinding attack, so the whole exchange is
		 * wiped rather than left open for another guess. */
		rc = pair_f4(c->k.peer_pub, c->local_pub, peer_nonce, expect);
		if (rc != RADIANT_SEC_OK) {
			return rc;
		}
		if (!radiant_sec_ct_equal(expect, c->s.commit, sizeof(expect))) {
			ctx_free(c);
			return RADIANT_SEC_EBADMAC;
		}
		rc = pair_finish(c, LABEL_ROOT, c->local_pub, c->k.peer_pub,
				 c->s.nonce, peer_nonce, fingerprint);
	} else {
		rc = pair_finish(c, LABEL_ROOT, c->k.peer_pub, c->local_pub,
				 peer_nonce, c->s.nonce, fingerprint);
	}
	/* Past the commitment nothing may fail retryably: a responder left at
	 * its nonce step would take a second Na and show a chosen fp. */
	if (rc != RADIANT_SEC_OK) {
		ctx_free(c);
	}
	return rc;
}

/* ── Over-air enrolment ─────────────────────────────────────────────────── */

static const uint8_t zero_nonce[NONCE_BYTES];

/* No roles and no nonces: the keys ordered by value stand in for
 * initiator/responder, so both ends compute the same thing without agreeing
 * who started. */
static void enrol_order(const struct pair_ctx *c, const uint8_t **lo,
			const uint8_t **hi)
{
	bool local_low = memcmp(c->local_pub, c->k.peer_pub, PUB_BYTES) <= 0;

	*lo = local_low ? c->local_pub : c->k.peer_pub;
	*hi = local_low ? c->k.peer_pub : c->local_pub;
}

/* K_wrap and K_wrap' from the held PRK: pairwise, and unrelated to any root. */
static int wrap_keys(const struct pair_ctx *c, struct radiant_sec_key *kw,
		     struct radiant_sec_key *km)
{
	uint8_t        raw[RADIANT_SEC_BLOCK_BYTES];
	const uint8_t *lo;
	const uint8_t *hi;
	struct cm_part parts[3];
	int            rc;

	enrol_order(c, &lo, &hi);
	parts[0].p = (const uint8_t *)LABEL_WRAP;
	parts[0].n = sizeof(LABEL_WRAP);
	parts[1].p = lo;
	parts[1].n = PUB_BYTES;
	parts[2].p = hi;
	parts[2].n = PUB_BYTES;
	memset(kw, 0, sizeof(*kw));
	memset(km, 0, sizeof(*km));
	rc = cmac_parts(c->s.prk, parts, ARRAY_SIZE(parts), raw);
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_key_import(kw, raw, 128u);
	}
	parts[0].p = (const uint8_t *)LABEL_WMAC;
	if (rc == RADIANT_SEC_OK) {
		rc = cmac_parts(c->s.prk, parts, ARRAY_SIZE(parts), raw);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_key_import(km, raw, 128u);
	}
	radiant_sec_wipe(raw, sizeof(raw));
	return rc;
}

/* trunc64( CMAC(K_wrap', n || ct) ), into `tag` (a full block). */
static int wrap_tag(const struct radiant_sec_key *km, const uint8_t *nb,
		    const uint8_t *ct, uint8_t *tag)
{
	uint8_t msg[2u * RADIANT_SEC_BLOCK_BYTES];

	memcpy(msg, nb, RADIANT_SEC_BLOCK_BYTES);
	memcpy(&msg[RADIANT_SEC_BLOCK_BYTES], ct, RADIANT_SEC_BLOCK_BYTES);
	return radiant_sec_cmac(km, msg, sizeof(msg), tag);
}

int radiant_sec_pair_seal(const struct radiant_sec_key *k_wrap,
			  const struct radiant_sec_key *k_mac, uint8_t *buf)
{
	uint8_t nb[RADIANT_SEC_BLOCK_BYTES];
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	int     rc = RADIANT_SEC_ENOTSUP;

	radiant_sec_nonce_block(nb, 0u, 0u, 0u, RADIANT_SEC_DOM_ENROL_WRAP);
	/* An opaque backend's handle holds a slot index, not a root. */
	if (!radiant_sec_caps()->key_is_opaque) {
		rc = radiant_sec_ctr_xor(k_wrap, nb, buf,
					 RADIANT_SEC_BLOCK_BYTES);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = wrap_tag(k_mac, nb, buf, tag);
	}
	if (rc == RADIANT_SEC_OK) {
		memcpy(&buf[RADIANT_SEC_BLOCK_BYTES], tag,
		       RADIANT_SEC_PAIR_WRAP_TAG_BYTES);
	} else {
		radiant_sec_wipe(buf, RADIANT_SEC_PAIR_WRAPPED_BYTES);
	}
	return rc;
}

int radiant_sec_pair_peer_uncommitted(uint8_t ch, const uint8_t *peer_pub,
				      uint8_t *wrapped, uint32_t *fingerprint)
{
	RADIANT_SEC_ASSERT_LOCKED();
	struct radiant_sec_key kw;
	struct radiant_sec_key km;
	struct pair_ctx       *c;
	const uint8_t         *lo;
	const uint8_t         *hi;
	bool                   compat = false;
	int                    rc;

	if (wrapped == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	rc = radiant_sec_pair_peer(ch, peer_pub);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	c = ctx_of(ch);
	enrol_order(c, &lo, &hi);

	/* Deliver, never replace: the root this channel already holds goes out
	 * sealed, and its keyholders, epoch and stream are untouched. */
	rc = wrap_keys(c, &kw, &km);
#if defined(CONFIG_RADIANT_SEC_COMPAT)
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_compat_wrap_root(ch, &kw, &km, wrapped);
		compat = (rc != RADIANT_SEC_ENOKEY);
		if (!compat) {
			rc = RADIANT_SEC_OK;
		}
	}
#endif
	if (rc == RADIANT_SEC_OK && !compat) {
		rc = radiant_sec_wrap_root(ch, &kw, &km, wrapped);
	}

	if (rc == RADIANT_SEC_ENOKEY) {
		/* No root at all: nothing to deliver and nobody to lock out,
		 * so the exchange establishes the first one (pair_finish()
		 * counts it and computes the fingerprint), then delivers it. */
		rc = pair_finish(c, LABEL_ENROL, lo, hi, zero_nonce,
				 zero_nonce, fingerprint);
		if (rc == RADIANT_SEC_OK) {
			rc = radiant_sec_wrap_root(ch, &kw, &km, wrapped);
		}
	} else if (rc == RADIANT_SEC_OK) {
		rc = pair_g2(lo, hi, zero_nonce, zero_nonce, &c->fingerprint);
		if (rc == RADIANT_SEC_OK) {
#if defined(CONFIG_RADIANT_SEC_COMPAT)
			if (compat) {
				radiant_sec_compat_stat_enrolment(ch);
			} else
#endif
			{
				radiant_sec_stat_enrolment(ch);
			}
			if (fingerprint != NULL) {
				*fingerprint = c->fingerprint;
			}
			radiant_sec_wipe(&c->s, sizeof(c->s));
			c->step = (uint8_t)STEP_DONE;
		}
	}
	radiant_sec_key_destroy(&kw);
	radiant_sec_key_destroy(&km);
	if (rc != RADIANT_SEC_OK) {
		/* The scalar is spent, so nothing usable is left, and a
		 * half-finished context must not linger. */
		radiant_sec_wipe(wrapped, RADIANT_SEC_PAIR_WRAPPED_BYTES);
		ctx_free(c);
	}
	return rc;
}

int radiant_sec_pair_join(uint8_t ch, const uint8_t *sensor_pub,
			  uint32_t *fingerprint)
{
	RADIANT_SEC_ASSERT_LOCKED();
	struct pair_ctx *c;
	const uint8_t   *lo;
	const uint8_t   *hi;
	int              rc;

	/* The install binds the channel's device number; refuse before the
	 * scalar is spent rather than after. */
	if (pair_devnum(ch) == 0u) {
		return RADIANT_SEC_ENOKEY;
	}
	rc = radiant_sec_pair_peer(ch, sensor_pub);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	c = ctx_of(ch);
	enrol_order(c, &lo, &hi);
	rc = pair_g2(lo, hi, zero_nonce, zero_nonce, &c->fingerprint);
	if (rc != RADIANT_SEC_OK) {
		ctx_free(c);
		return rc;
	}
	if (fingerprint != NULL) {
		*fingerprint = c->fingerprint;
	}
	c->step = (uint8_t)STEP_JOINED;
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_accept_root(uint8_t ch, const uint8_t *wrapped,
				 uint8_t into)
{
	RADIANT_SEC_ASSERT_LOCKED();
	struct radiant_sec_key kw;
	struct radiant_sec_key km;
	struct pair_ctx       *c;
	uint8_t                nb[RADIANT_SEC_BLOCK_BYTES];
	uint8_t                tag[RADIANT_SEC_BLOCK_BYTES];
	uint8_t                root[RADIANT_SEC_BLOCK_BYTES];
	uint16_t               devnum = pair_devnum(ch);
	int                    rc;

	if (wrapped == NULL || into > RADIANT_SEC_PAIR_INTO_COMPAT) {
		return RADIANT_SEC_EINVAL;
	}
#if !defined(CONFIG_RADIANT_SEC_COMPAT)
	if (into == RADIANT_SEC_PAIR_INTO_COMPAT) {
		return RADIANT_SEC_ENOTSUP;
	}
#endif
	c = ctx_live(ch);
	if (c == NULL || c->step != (uint8_t)STEP_JOINED) {
		return RADIANT_SEC_EINVAL;
	}
	if (devnum == 0u) {
		return RADIANT_SEC_ENOKEY;
	}

	/* Encrypt-then-MAC, so the tag is checked before anything is
	 * decrypted, and in constant time. A failure leaves the join intact:
	 * one forged or torn copy must not cost the receiver its enrolment. */
	rc = wrap_keys(c, &kw, &km);
	radiant_sec_nonce_block(nb, 0u, 0u, 0u, RADIANT_SEC_DOM_ENROL_WRAP);
	if (rc == RADIANT_SEC_OK) {
		rc = wrap_tag(&km, nb, wrapped, tag);
	}
	if (rc == RADIANT_SEC_OK &&
	    !radiant_sec_ct_equal(tag, &wrapped[RADIANT_SEC_BLOCK_BYTES],
				  RADIANT_SEC_PAIR_WRAP_TAG_BYTES)) {
		rc = RADIANT_SEC_EBADMAC;
	}
	if (rc == RADIANT_SEC_OK) {
		memcpy(root, wrapped, sizeof(root));
		rc = radiant_sec_ctr_xor(&kw, nb, root, sizeof(root));
	}
#if defined(CONFIG_RADIANT_SEC_COMPAT)
	if (rc == RADIANT_SEC_OK && into == RADIANT_SEC_PAIR_INTO_COMPAT) {
		rc = radiant_sec_compat_set_key(ch, root, 128u, devnum);
		if (rc == RADIANT_SEC_OK) {
			(void)radiant_sec_compat_set_devnum(ch, devnum);
			radiant_sec_compat_stat_enrolment(ch);
		}
	} else
#endif
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_set_key(ch, root, 128u, devnum);
		if (rc == RADIANT_SEC_OK) {
			(void)radiant_sec_set_devnum(ch, devnum);
			radiant_sec_stat_enrolment(ch);
		}
	}
	radiant_sec_wipe(root, sizeof(root));
	radiant_sec_key_destroy(&kw);
	radiant_sec_key_destroy(&km);
	if (rc == RADIANT_SEC_OK) {
		radiant_sec_wipe(&c->s, sizeof(c->s));
		c->step = (uint8_t)STEP_DONE;
	}
	return rc;
}

int radiant_sec_pair_fingerprint(uint8_t ch, uint32_t *out)
{
	RADIANT_SEC_ASSERT_LOCKED();
	const struct pair_ctx *c = ctx_of(ch);

	if (c == NULL || out == NULL || c->step < (uint8_t)STEP_JOINED) {
		return RADIANT_SEC_EINVAL;
	}
	*out = c->fingerprint;
	return RADIANT_SEC_OK;
}

bool radiant_sec_pair_is_open(uint8_t ch)
{
	struct pair_ctx *c = ctx_of(ch);

	if (c == NULL) {
		return false;
	}
	if (pair_expired(c, k_uptime_get_32())) {
		ctx_free(c);
		return false;
	}
	return c->open;
}

void radiant_sec_pair_reset(void)
{
	RADIANT_SEC_ASSERT_LOCKED();
	uint8_t i;

	pair_map_init();
	atomic_set(&pair_lost, 0);
	for (i = 0u; i < (uint8_t)PAIR_MAX_CH; i++) {
		ctx_free(&pair[i]);
	}
	for (i = 0u; i < (uint8_t)RADIANT_CHANNEL_COUNT; i++) {
		pair_of[i] = -1;
	}
}
