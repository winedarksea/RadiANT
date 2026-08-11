/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Pairing: turning one X25519 exchange into the 16 bytes every other file in
 * this feature already knows how to use.
 *
 * Provenance: clean-room. Written from docs/radiant-security.md sections 7.4
 * and 8, RFC 7748 for the exchange itself, and NIST SP 800-56C's
 * extract-then-expand shape for the derivation. No third-party implementation
 * was consulted.
 *
 * ---------------------------------------------------------------------------
 * PAIRING HAPPENS IN THE CLEAR, AND THAT IS STRUCTURAL
 * ---------------------------------------------------------------------------
 * There is no prior secret and no out-of-band channel on the air, so an active
 * attacker present during the exchange can be the man in the middle. That is
 * not an oversight to be fixed later - it is what an anonymous key agreement
 * is. Section 7.4 records it as a limit rather than hiding it.
 *
 * The mitigation is the FINGERPRINT: six digits derived from the shared secret
 * and both public keys, shown at both ends for a human to compare. An attacker
 * in the middle holds two different shared secrets and cannot make both
 * fingerprints match. This converts an undetectable attack into one that
 * requires the user not to look, which is the same bargain BLE numeric
 * comparison makes.
 *
 * The window is small and it is not zero. A user who skips the comparison gets
 * an unauthenticated key and no warning, which is why out-of-band pairing - a
 * QR code, a typed key - remains the recommended path for anything that
 * matters.
 *
 * ---------------------------------------------------------------------------
 * NO EXCLUSIVITY FLAG, DELIBERATELY
 * ---------------------------------------------------------------------------
 * sdk-ant serialises its own key exchange across the whole device with a
 * single global "one channel negotiates at a time" flag. Nothing here does,
 * because nothing here needs a shared resource: each channel's state is its
 * own and the ladder runs on the calling thread.
 *
 * If that ever changes, section 8.1 records the trap in advance and it is worth
 * repeating at the point where the flag would be added: the flag must be
 * released on LOSS OF TRACKING, not only on completion or failure. A channel
 * that starts pairing and then loses its peer has no completion event and no
 * failure event - the sensor walked out of range - and
 * RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH is the only transition that fires.
 * Miss it and that channel deadlocks negotiation for every other channel on the
 * device, permanently, with nothing anywhere to say so.
 *
 * radiant_sec_pair_on_search() exists for exactly that reason and is called
 * from the same place, so the hook is live before there is anything to hang on
 * it. Today it cancels one channel's own pairing; that is cheap and it means
 * the release path is exercised rather than merely designed.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <radiant_core/radiant_sec.h>
#include <radiant_core/radiant_channel.h>

#define PAIR_MAX_CH CONFIG_RADIANT_SEC_MAX_CHANNELS

/* The derivation's domain strings. Distinct labels over the same key are what
 * stop the root key and the fingerprint being functions of each other - a
 * fingerprint shown on a screen must not narrow the key it attests to. */
#define LABEL_ROOT "RADIANT-pair-root"
#define LABEL_FP   "RADIANT-pair-fp"

struct pair_ctx {
	uint8_t  ch;
	bool     in_use;
	bool     open;          /* pairing mode entered */
	bool     have_scalar;
	bool     have_local;

	uint32_t deadline_ms;   /* 0 = no timeout */

	uint8_t  scalar[RADIANT_SEC_X25519_BYTES];
	uint8_t  local_pub[RADIANT_SEC_X25519_BYTES];
	uint32_t fingerprint;   /* valid once a peer has been accepted */
	bool     have_fp;
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

/* Wipe everything secret and give the slot back. Called on leave, on timeout,
 * on loss of tracking and on reset - one function so those four cannot drift
 * into clearing different subsets. */
static void ctx_free(struct pair_ctx *c)
{
	if (c == NULL) {
		return;
	}
	if (c->ch < (uint8_t)RADIANT_CHANNEL_COUNT) {
		pair_of[c->ch] = -1;
	}
	memset(c, 0, sizeof(*c));
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

/*
 * Extract-then-expand over CMAC, which is the only MAC this build has.
 *
 * EXTRACT. The raw X25519 output is not a uniform key: it is a curve point's
 * u-coordinate, with structure. CMAC under an all-zero key condenses all 32
 * bytes into 16 uniform ones. Importing the first 16 bytes directly would have
 * been simpler and would have thrown away half the entropy.
 *
 * EXPAND. Both public keys go into the expansion, ordered by value rather than
 * by role, so the two ends compute the same thing without having to agree on
 * who was the initiator. Binding them is what makes the fingerprint attest to
 * the identities and not merely to the secret.
 */
static int pair_derive(const uint8_t *shared, const uint8_t *pub_a,
		       const uint8_t *pub_b, uint8_t *root_out,
		       uint32_t *fp_out)
{
	struct radiant_sec_key      zero_key;
	struct radiant_sec_key      prk_key;
	struct radiant_sec_cmac_ctx cm;
	uint8_t                     zero[16];
	uint8_t                     prk[RADIANT_SEC_BLOCK_BYTES];
	uint8_t                     tag[RADIANT_SEC_BLOCK_BYTES];
	const uint8_t              *lo;
	const uint8_t              *hi;
	int                         rc;

	memset(zero, 0, sizeof(zero));
	memset(&zero_key, 0, sizeof(zero_key));
	memset(&prk_key, 0, sizeof(prk_key));

	rc = radiant_sec_key_import(&zero_key, zero, 128u);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	rc = radiant_sec_cmac(&zero_key, shared, RADIANT_SEC_X25519_BYTES, prk);
	radiant_sec_key_destroy(&zero_key);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	rc = radiant_sec_key_import(&prk_key, prk, 128u);
	memset(prk, 0, sizeof(prk));
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	if (memcmp(pub_a, pub_b, RADIANT_SEC_X25519_BYTES) <= 0) {
		lo = pub_a;
		hi = pub_b;
	} else {
		lo = pub_b;
		hi = pub_a;
	}

	/* K_root. */
	rc = radiant_sec_cmac_init(&cm, &prk_key);
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&cm, (const uint8_t *)LABEL_ROOT,
					     sizeof(LABEL_ROOT));
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&cm, lo, RADIANT_SEC_X25519_BYTES);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&cm, hi, RADIANT_SEC_X25519_BYTES);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_final(&cm, root_out);
	}
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_key_destroy(&prk_key);
		return rc;
	}

	/* The fingerprint, under a different label over the same key. */
	rc = radiant_sec_cmac_init(&cm, &prk_key);
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&cm, (const uint8_t *)LABEL_FP,
					     sizeof(LABEL_FP));
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&cm, lo, RADIANT_SEC_X25519_BYTES);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&cm, hi, RADIANT_SEC_X25519_BYTES);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_final(&cm, tag);
	}
	radiant_sec_key_destroy(&prk_key);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	/*
	 * Six digits, from four bytes.
	 *
	 * Six rather than four or eight because it is what a person will
	 * actually read off two screens and compare: a 1-in-10^6 chance of a
	 * man in the middle guessing it, against a comparison a user will
	 * finish. Eight digits raise the work factor and lower the number of
	 * users who complete the check, and the second effect dominates.
	 */
	*fp_out = (((uint32_t)tag[0] << 24) | ((uint32_t)tag[1] << 16) |
		   ((uint32_t)tag[2] << 8) | (uint32_t)tag[3]) % 1000000u;

	memset(tag, 0, sizeof(tag));
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_enter(uint8_t ch, uint8_t timeout_s)
{
	struct pair_ctx *c = ctx_alloc(ch);

	if (c == NULL) {
		return RADIANT_SEC_ENOKEY;
	}

	/*
	 * A TIMEOUT IS NOT OPTIONAL. A node left in pairing mode accepts a key
	 * from anyone who asks, so an interface that let a host enter it
	 * without a bound would turn a forgotten command into a permanently
	 * open node. Zero means the default rather than "forever".
	 */
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
	ctx_free(ctx_of(ch));
}

void radiant_sec_pair_on_search(uint8_t ch)
{
	struct pair_ctx *c = ctx_of(ch);

	/*
	 * Loss of tracking. The peer is gone mid-exchange and no completion or
	 * failure event will ever arrive, so this is the only place the state
	 * can be released. See the header comment: today that matters because
	 * it wipes a half-finished exchange's scalar; it matters far more the
	 * day anything here becomes device-wide.
	 */
	if (c != NULL) {
		ctx_free(c);
	}
}

int radiant_sec_pair_set_scalar(uint8_t ch, const uint8_t *scalar)
{
	struct pair_ctx *c = ctx_of(ch);
	int              rc;

	if (c == NULL || scalar == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	if (!c->open) {
		return RADIANT_SEC_EINVAL;
	}
	if (pair_expired(c, k_uptime_get_32())) {
		ctx_free(c);
		return RADIANT_SEC_EINVAL;
	}
	if (!radiant_sec_caps()->has_x25519) {
		return RADIANT_SEC_ENOTSUP;
	}

	/*
	 * THE SCALAR COMES FROM THE HOST, and that is a limitation rather than
	 * a design preference: the only entropy source on nRF54L is
	 * psa_rng/CRACEN, and reaching it drags nrf_security into every build
	 * of this module. The honest consequence is that a host-less node
	 * cannot pair this way and must be provisioned out of band.
	 *
	 * Nothing here can check that what arrived is random. A host that sends
	 * a constant has a node whose private key is public, and neither end
	 * can tell.
	 */
	memcpy(c->scalar, scalar, RADIANT_SEC_X25519_BYTES);
	c->have_scalar = true;

	rc = radiant_sec_x25519(c->local_pub, c->scalar,
				radiant_sec_x25519_basepoint);
	if (rc != RADIANT_SEC_OK) {
		memset(c->scalar, 0, sizeof(c->scalar));
		c->have_scalar = false;
		return rc;
	}
	c->have_local = true;
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_local_pubkey(uint8_t ch, uint8_t *out)
{
	const struct pair_ctx *c = ctx_of(ch);

	if (c == NULL || out == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	if (!c->have_local) {
		return RADIANT_SEC_EINVAL;
	}
	memcpy(out, c->local_pub, RADIANT_SEC_X25519_BYTES);
	return RADIANT_SEC_OK;
}

int radiant_sec_pair_peer(uint8_t ch, const uint8_t *peer_pub,
			  uint32_t *fingerprint)
{
	struct pair_ctx          *c = ctx_of(ch);
	struct radiant_channel_id id;
	uint8_t                   shared[RADIANT_SEC_X25519_BYTES];
	uint8_t                   root[RADIANT_SEC_BLOCK_BYTES];
	int                       rc;

	if (c == NULL || peer_pub == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	if (!c->open || !c->have_local) {
		return RADIANT_SEC_EINVAL;
	}
	if (pair_expired(c, k_uptime_get_32())) {
		ctx_free(c);
		return RADIANT_SEC_EINVAL;
	}

	rc = radiant_sec_x25519(shared, c->scalar, peer_pub);
	if (rc != RADIANT_SEC_OK) {
		goto out;
	}

	/*
	 * A small-order peer key makes the shared secret all zeros whatever our
	 * scalar was. RFC 7748 leaves rejecting it optional for Diffie-Hellman;
	 * it is mandatory here, because this value becomes a root key - so
	 * accepting it would let anyone able to inject one packet fix the group
	 * key to a value they already know.
	 */
	if (radiant_sec_x25519_is_degenerate(shared)) {
		rc = RADIANT_SEC_EINVAL;
		goto out;
	}

	rc = pair_derive(shared, c->local_pub, peer_pub, root, &c->fingerprint);
	if (rc != RADIANT_SEC_OK) {
		goto out;
	}
	c->have_fp = true;

	/*
	 * Install it. The provisioning device number is read from the channel
	 * for the same reason antr_sec_key_set() reads it rather than taking it
	 * on the wire: it is bound into the KDF, and a second copy of it would
	 * be a second place for it to be wrong.
	 */
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK ||
	    id.device_number == 0u) {
		rc = RADIANT_SEC_ENOKEY;
		goto out;
	}
	rc = radiant_sec_set_key(ch, root, 128u, id.device_number);
	if (rc != RADIANT_SEC_OK) {
		goto out;
	}
	(void)radiant_sec_set_devnum(ch, id.device_number);

	if (fingerprint != NULL) {
		*fingerprint = c->fingerprint;
	}

	/*
	 * The scalar is spent. Pairing mode stays open until the host leaves it
	 * or the timeout fires, so the fingerprint can still be read - but the
	 * private key has done its one job and there is no reason to keep it.
	 */
	memset(c->scalar, 0, sizeof(c->scalar));
	c->have_scalar = false;

out:
	memset(shared, 0, sizeof(shared));
	memset(root, 0, sizeof(root));
	return rc;
}

int radiant_sec_pair_fingerprint(uint8_t ch, uint32_t *out)
{
	const struct pair_ctx *c = ctx_of(ch);

	if (c == NULL || out == NULL || !c->have_fp) {
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
	uint8_t i;

	pair_map_init();
	for (i = 0u; i < (uint8_t)PAIR_MAX_CH; i++) {
		ctx_free(&pair[i]);
	}
	for (i = 0u; i < (uint8_t)RADIANT_CHANNEL_COUNT; i++) {
		pair_of[i] = -1;
	}
}
