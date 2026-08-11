/* SPDX-License-Identifier: Apache-2.0 */
/*
 * A mode-level crypto backend, for the seam and not for a product.
 *
 * ── Why this file exists ───────────────────────────────────────────────────
 *
 * radiant_sec.h claims a two-level seam: policy calls modes, modes call
 * blocks, and a backend implements whichever level it can do well. That claim
 * is free to make and easy to get wrong - a "seam" whose only implementation
 * is the portable one is indistinguishable from a direct implementation with
 * extra prototypes, and it stays that way until the day somebody tries to
 * attach CRACEN and discovers the mode-level path was never live.
 *
 * So: CONFIG_RADIANT_SEC_BACKEND_MODES suppresses the portable AES-CTR and
 * AES-CMAC layer in src/radiant_sec_aes.c, and this file supplies the Level 2
 * entry points and radiant_sec_caps() instead - advertising has_ctr and
 * has_cmac, exactly as a CRACEN or CC310 backend would. The same published
 * vectors then run against it. If a future edit lets policy reach past Level 2
 * to radiant_sec_aes_ecb(), the vectors still pass and the seam is still
 * broken, which is why the zero-cost job greps radiant_sec.c for that call as
 * well.
 *
 * ── Why it is a second implementation and not a wrapper ────────────────────
 *
 * The CMAC below buffers differently from the portable one: it processes a
 * full block eagerly and keeps a one-block lookahead, where the portable
 * version holds a full block back until more data arrives. Both are correct;
 * they fail differently. Running the RFC 4493 vectors through both is a real
 * cross-check rather than a check that memcpy still works, and the
 * message-length-a-multiple-of-16 case - the classic incremental-CMAC bug -
 * is exactly where two buffering strategies diverge.
 *
 * The block primitive is shared, because there is only one AES here and
 * duplicating it would test the S-box twice and the seam not at all.
 */

#ifdef CONFIG_RADIANT_SEC_BACKEND_MODES

#include <string.h>

#include <radiant_core/radiant_sec.h>

static const struct radiant_sec_caps modes_caps = {
	.name          = "test-modes",
	.has_ctr       = true,
	.has_cmac      = true,
	.has_x25519    = false,
	/* Stated rather than left to the zero-initialiser: a mode-level
	 * accelerator is exactly the sort of part that also has a TRNG, so the
	 * next person to copy this struct should see the field. */
	.has_rng       = false,
	.key_is_opaque = false,
	.key_bits_mask = RADIANT_SEC_KEY_BITS_128,
	.block_us      = 0,
};

const struct radiant_sec_caps *radiant_sec_caps(void)
{
	return &modes_caps;
}

static void counter_inc(uint8_t c[16])
{
	unsigned i;

	for (i = 16; i-- > 0;) {
		c[i]++;
		if (c[i] != 0u) {
			return;
		}
	}
}

int radiant_sec_ctr_xor(const struct radiant_sec_key *k, const uint8_t iv[16],
			uint8_t *buf, size_t len)
{
	uint8_t counter[16];
	uint8_t stream[16];
	size_t done = 0;

	if (iv == NULL || (buf == NULL && len != 0u)) {
		return RADIANT_SEC_EINVAL;
	}
	memcpy(counter, iv, sizeof(counter));

	while (done < len) {
		size_t i;
		size_t chunk = len - done;
		int rc = radiant_sec_aes_ecb(k, counter, stream);

		if (rc != RADIANT_SEC_OK) {
			return rc;
		}
		if (chunk > 16u) {
			chunk = 16u;
		}
		for (i = 0; i < chunk; i++) {
			buf[done + i] ^= stream[i];
		}
		done += chunk;
		counter_inc(counter);
	}

	memset(stream, 0, sizeof(stream));
	return RADIANT_SEC_OK;
}

static void dbl_block(uint8_t v[16])
{
	unsigned i;
	uint8_t carry = (uint8_t)(v[0] >> 7);

	for (i = 0; i < 15; i++) {
		v[i] = (uint8_t)((v[i] << 1) | (v[i + 1] >> 7));
	}
	v[15] = (uint8_t)((v[15] << 1) ^ (0x87u * carry));
}

int radiant_sec_cmac_init(struct radiant_sec_cmac_ctx *c,
			  const struct radiant_sec_key *k)
{
	if (c == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	if (k == NULL || k->bits == 0u || k->serial == 0u) {
		return RADIANT_SEC_ENOKEY;
	}
	memset(c, 0, sizeof(*c));
	c->key = k;
	c->started = 1u;
	return RADIANT_SEC_OK;
}

/*
 * Eager buffering with a one-block lookahead: `partial` holds a block that has
 * been accepted but not yet folded in, because it might turn out to be the
 * last one. partial_len == 16 therefore means "a full block is pending", which
 * is the same state the portable version reaches by a different route.
 */
int radiant_sec_cmac_update(struct radiant_sec_cmac_ctx *c, const uint8_t *msg,
			    size_t len)
{
	if (c == NULL || c->started == 0u) {
		return RADIANT_SEC_EINVAL;
	}
	if (len == 0u) {
		return RADIANT_SEC_OK;
	}
	if (msg == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	while (len != 0u) {
		size_t room;
		size_t take;

		if (c->partial_len == 16u) {
			unsigned i;
			int rc;

			for (i = 0; i < 16; i++) {
				c->chain[i] ^= c->partial[i];
			}
			rc = radiant_sec_aes_ecb(c->key, c->chain, c->chain);
			if (rc != RADIANT_SEC_OK) {
				return rc;
			}
			c->partial_len = 0u;
		}

		room = 16u - c->partial_len;
		take = (len < room) ? len : room;
		memcpy(c->partial + c->partial_len, msg, take);
		c->partial_len = (uint8_t)(c->partial_len + take);
		msg += take;
		len -= take;
	}
	return RADIANT_SEC_OK;
}

int radiant_sec_cmac_final(struct radiant_sec_cmac_ctx *c, uint8_t tag[16])
{
	static const uint8_t zero[16] = { 0 };
	uint8_t sub[16];
	uint8_t last[16];
	unsigned i;
	int rc;

	if (c == NULL || c->started == 0u || tag == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	rc = radiant_sec_aes_ecb(c->key, zero, sub);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	dbl_block(sub);                    /* K1 */
	if (c->partial_len != 16u) {
		dbl_block(sub);            /* K2 */
	}

	memcpy(last, c->partial, c->partial_len);
	if (c->partial_len != 16u) {
		last[c->partial_len] = 0x80u;
		for (i = (unsigned)c->partial_len + 1u; i < 16u; i++) {
			last[i] = 0u;
		}
	}
	for (i = 0; i < 16; i++) {
		last[i] ^= sub[i] ^ c->chain[i];
	}

	rc = radiant_sec_aes_ecb(c->key, last, tag);

	memset(c, 0, sizeof(*c));
	memset(sub, 0, sizeof(sub));
	memset(last, 0, sizeof(last));
	return rc;
}

int radiant_sec_cmac(const struct radiant_sec_key *k, const uint8_t *msg,
		     size_t len, uint8_t tag[16])
{
	struct radiant_sec_cmac_ctx c;
	int rc = radiant_sec_cmac_init(&c, k);

	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	rc = radiant_sec_cmac_update(&c, msg, len);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	return radiant_sec_cmac_final(&c, tag);
}

#endif /* CONFIG_RADIANT_SEC_BACKEND_MODES */
