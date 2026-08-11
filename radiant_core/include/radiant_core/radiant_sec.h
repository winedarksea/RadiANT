/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec - the two per-channel payload transforms, X_AUTH and X_CONF,
 * and the crypto seam underneath them.
 *
 * docs/radiant-security.md is normative for everything here; this header is
 * the C shape of it. docs/decisions/0006-security-v1-scope-and-x-priv-
 * withdrawal.md records why there are two switches and not three.
 *
 * ── The governing constraint ────────────────────────────────────────────────
 *
 * Almost nobody will turn this on. So it must cost zero flash, zero RAM, zero
 * latency and zero power for every user who does not - and that is a CI job
 * (`zero-cost`) rather than an assertion in a comment. Three rules follow, and
 * each is a deliberate departure from a convention the rest of this module
 * keeps:
 *
 *   1. NO OPS STRUCT, NO CALLBACK TABLE. radiant_transfer, radiant_search and
 *      radiant_sched all take a `struct *_cbs`. This one does not: a function
 *      pointer table is a .data relocation the linker cannot garbage-collect,
 *      and one surviving relocation defeats the size gate.
 *   2. NO STRUCTS IN HOT-PATH SIGNATURES. Scalars and uint8_t * only, so the
 *      disabled inline collapses without the compiler having to prove a struct
 *      copy dead.
 *   3. NO INIT CALL AND NO ALLOCATION ON THE NON-SECURED PATH. antr_init()
 *      gains nothing. The context table is a static BSS array and the first
 *      0xF2 SET_KEY is what allocates an entry.
 *
 * ── Two levels, and why the seam is shaped for hardware that does not exist ─
 *
 * Every accelerator on the roadmap operates on WHOLE MODES with DMA, not on
 * single blocks:
 *
 *   nRF52840   ECB peripheral (AES-128 ECB, block at a time) *and* CryptoCell
 *              CC310 - ECB/CBC/CTR/CCM/CMAC, SHA, RSA, ECC incl. Curve25519
 *   nRF54L15   CRACEN - ECB/CBC/CTR/CCM/GCM/CMAC, SHA-2, TRNG, PKE. There is
 *              NO standalone ECB peripheral on this family at all, and CRACEN
 *              is reachable only through PSA / nrf_security
 *   others     a third-party part, if one is adopted, looks like CC310
 *
 * So a block-only seam is a design error: per-call setup dominates on all
 * three, and the nRF54L path is reachable only as a mode-level call. Hence:
 *
 *   Level 2 - modes.  radiant_sec.c calls ONLY these.
 *   Level 1 - block.  A backend implementing only this gets the modes for
 *                     free from the portable layer built on top of it.
 *
 * A backend implements whichever level it can do well. Software and the nRF52
 * ECB peripheral implement Level 1; CRACEN and CC310 implement Level 2.
 *
 * The seam is the one piece of this that is expensive to retrofit and free to
 * get right now, which is why it lands with the primitives rather than being
 * fitted around them afterwards. Two checks keep that honest: radiant_sec.c
 * contains no call to radiant_sec_aes_ecb(), and a test build selects a
 * mode-level backend and still passes every published vector.
 *
 * ── Serialisation ──────────────────────────────────────────────────────────
 *
 * NO CRYPTO RUNS IN ISR CONTEXT, unconditionally, and that rule does not vary
 * with caps.block_us - a rule that changes with the backend is a rule that
 * gets debugged twice. TX assembly is already thread-only; every RX packet
 * already transits the event thread before a host sees it, so the CTR XOR, the
 * CMAC absorption and the verdicts all happen there.
 *
 * The software backend keeps ONE shared key schedule to stay inside its RAM
 * budget, so the Level 1 and Level 2 entry points are NOT re-entrant. Callers
 * serialise them; radiant_core does that with api_lock.
 */

#ifndef RADIANT_CORE_RADIANT_SEC_H_
#define RADIANT_CORE_RADIANT_SEC_H_

/* Deliberately nothing but the freestanding headers. This header is reachable
 * from radiant_sec_aes.c, which is hammered by a ztest suite with no HAL, no
 * fake radio and no Zephyr, and that property is worth protecting. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ═══════════════════════════════════════════════════════════════════════════
 * The crypto seam
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define RADIANT_SEC_BLOCK_BYTES   16

/*
 * Storage inside a key handle. Sixteen bytes, because the PROTOCOL is AES-128:
 * 256 buys nothing against casual snooping and costs distribution bytes.
 *
 * The API is still 256-capable - radiant_sec_key_import() takes `bits` and
 * caps->key_bits_mask advertises what a backend can do - and an opaque
 * hardware handle is a slot index, which is four bytes. Only a *software*
 * 256-bit backend would need more room here, and that backend would raise this
 * constant. Sizing it at 32 today would double the RAM of every secured
 * channel to express something nothing implements.
 */
#define RADIANT_SEC_KEY_MAX_BYTES 16

/* Return codes. Negative, in the style radiant_radio_hal.h uses. */
#define RADIANT_SEC_OK        0
#define RADIANT_SEC_EINVAL   -1
#define RADIANT_SEC_ENOTSUP  -2
#define RADIANT_SEC_ENOKEY   -3
#define RADIANT_SEC_EBADMAC  -4
/* A counter this receiver has already accepted. Distinct from EBADMAC because
 * the two mean opposite things about the sender: a replay is a tag that
 * verifies perfectly and is worthless, and a caller that could not tell them
 * apart would count an attacker's recording as a forgery attempt or, worse,
 * a forgery attempt as a stale packet. Defined here rather than beside its one
 * user in radiant_sec_compat.h, because this is the list a reader checks a new
 * code against and a fifth value defined elsewhere is a collision waiting for
 * the sixth. */
#define RADIANT_SEC_EREPLAY  -5

/*
 * An opaque key handle. NEVER a `const uint8_t key[16]` at a call site.
 *
 * The software backend stores raw bytes here and expands them into a shared
 * schedule on demand; a hardware backend stores A SLOT INDEX FOR A KEY THE CPU
 * MAY NEVER BE ABLE TO READ BACK, in the same space, and sets
 * caps->key_is_opaque. That is precisely why PSA's API looks the way it does,
 * and taking that shape for one struct is what makes PSA/nrf_security a
 * *backend* later rather than a rewrite of every call site - which matters,
 * because on nRF54L PSA is the only route to CRACEN at all.
 *
 * Raw bytes rather than a stored key schedule is a RAM decision, not an
 * oversight: eight secured channels hold three keys each, and 44 words of
 * schedule per key would be 2.8 KB against a ~960 B budget for the whole
 * feature's state.
 */
struct radiant_sec_key {
	uint8_t  b[RADIANT_SEC_KEY_MAX_BYTES];
	uint16_t bits;      /* 128 or 256; 0 when the handle is empty */
	uint32_t serial;    /* import serial - the shared-schedule cache tag.
			     * Pointer identity is not enough: destroy one key
			     * and import another at the same address and a
			     * pointer-keyed cache silently uses the old
			     * schedule. Thirty-two bits rather than sixteen so
			     * the counter cannot wrap into a live serial in a
			     * long session that re-derives keys every epoch. */
};

/*
 * Capability query, mirroring struct radiant_radio_caps. It is what lets
 * radiant_sec.c fail loudly on a backend that cannot do what a channel was
 * configured for, instead of testing for a backend by name.
 */
struct radiant_sec_caps {
	const char *name;

	/* True when the backend implements the Level 2 entry point itself.
	 * False means the portable layer is providing it over Level 1. */
	bool has_ctr;
	bool has_cmac;

	/* Public-key. Software X25519 is its own Kconfig symbol; a PKA
	 * backend (CC310, CRACEN's PKE) sets this and displaces it. */
	bool has_x25519;

	/* True when key material never leaves the backend. Nothing in
	 * radiant_core reads a key back, so this is informational today and
	 * load-bearing the day a host asks to. */
	bool key_is_opaque;

	/* True when radiant_sec_rng() below is real. See ADR 0009: a hostless
	 * node derives its pairing scalar from K_dev and a counter BECAUSE
	 * there is usually no entropy source, and where one does exist it
	 * should be used instead. This bit is the whole of how a caller asks,
	 * and it is a bit rather than a backend name for the reason every other
	 * field here is: policy never names a backend. */
	bool has_rng;

	/* Bit 0 = 128-bit keys, bit 1 = 192, bit 2 = 256. */
	uint8_t key_bits_mask;

	/* Microseconds per 16-byte block, for budgeting only. It does NOT
	 * gate the no-crypto-in-ISR rule. */
	uint16_t block_us;
};

#define RADIANT_SEC_KEY_BITS_128 0x01
#define RADIANT_SEC_KEY_BITS_192 0x02
#define RADIANT_SEC_KEY_BITS_256 0x04

const struct radiant_sec_caps *radiant_sec_caps(void);

/*
 * ── Entropy, optional and never assumed ────────────────────────────────────
 *
 * `len` cryptographically strong random bytes, or RADIANT_SEC_ENOTSUP when the
 * build has no entropy source - which is the DEFAULT and not an error path.
 * docs/radiant-security.md section 7.4 pins why: reaching psa_rng/CRACEN drags
 * nrf_security into the build, so it is opt-in per part rather than assumed
 * per project.
 *
 * A CALLER MUST CHECK caps.has_rng RATHER THAN CALLING AND HOPING. The weak
 * definition below returns ENOTSUP without touching `out`, so a caller that
 * ignores the cap and also ignores the return code gets whatever was already in
 * its buffer - which is the failure mode a security-critical default must not
 * have quietly, and is why ADR 0009 makes the deterministic KDF path the
 * primary one rather than the fallback.
 *
 * Nothing in radiant_core calls this. It exists for the node application's
 * pairing-scalar precedence rule (ADR 0009): host-supplied scalar if present,
 * else the RNG when caps.has_rng, else KDF(K_dev, "pair" || pair_counter).
 */
int radiant_sec_rng(uint8_t *out, size_t len);

/*
 * ── Public key: X25519 ─────────────────────────────────────────────────────
 *
 * One entry point, so a PKA backend (CC310, CRACEN's PKE, the CC2652's PKA)
 * displaces the software implementation without the pairing logic knowing.
 * Public-key work is where hardware actually earns its keep - the ladder below
 * is milliseconds where an AES block is microseconds - so this is the seam most
 * likely to be taken up.
 *
 * Declared whenever CONFIG_RADIANT_SEC is on, defined only when something
 * provides it: CONFIG_RADIANT_SEC_PAIRING_X25519 selects the software one, and
 * caps.has_x25519 says whether any exists. A caller must check the cap rather
 * than assume, because "not built" and "not supported by this backend" are the
 * same condition from above.
 */
#define RADIANT_SEC_X25519_BYTES 32

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

/* The u-coordinate 9, RFC 7748's base point. Pass as `point` to turn a private
 * scalar into the public key that goes on the air. */
extern const uint8_t radiant_sec_x25519_basepoint[RADIANT_SEC_X25519_BYTES];

/*
 * out = X25519(scalar, point). All three are 32 bytes.
 *
 * `scalar` is clamped on a copy per RFC 7748 section 5, so the caller's key is
 * not rewritten. Constant time in the scalar; no claim is made about power or
 * electromagnetic analysis.
 *
 * Returns RADIANT_SEC_OK or RADIANT_SEC_EINVAL.
 */
int radiant_sec_x25519(uint8_t *out, const uint8_t *scalar,
		       const uint8_t *point);

/*
 * True when a shared secret is all zeros, which means the peer sent a
 * small-order point. Rejecting it is mandatory here rather than optional: the
 * result is fed to a KDF and installed as a root key, so accepting it would let
 * anyone who can inject one packet fix the group key to a value they know.
 */
bool radiant_sec_x25519_is_degenerate(const uint8_t *shared);

/*
 * ── Pairing ────────────────────────────────────────────────────────────────
 *
 * One exchange, turned into the 16 bytes the rest of this feature already
 * knows how to use. Driven by message 0xF5; see docs/radiant-security.md
 * sections 7.4 and 8.
 *
 * PAIRING HAPPENS IN THE CLEAR. There is no prior secret and no out-of-band
 * channel on the air, so an active attacker present during the exchange can be
 * the man in the middle. That is what an anonymous key agreement is, not an
 * oversight. The mitigation is the fingerprint below - six digits a human
 * compares across two screens, which an attacker holding two different shared
 * secrets cannot make match.
 *
 * Out-of-band pairing (a QR code, a typed key) remains the recommended path
 * for anything that matters: no protocol, no attack surface during pairing at
 * all, and no dependence on the user actually looking.
 */

/* A node in pairing mode accepts a key from whoever asks, so the mode is always
 * bounded. Zero from a host means this, not "forever". */
#define RADIANT_SEC_PAIR_TIMEOUT_DEFAULT_S 60

int  radiant_sec_pair_enter(uint8_t ch, uint8_t timeout_s);
void radiant_sec_pair_leave(uint8_t ch);

/*
 * Loss of tracking, from RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH.
 *
 * A channel that starts pairing and then loses its peer - the sensor walked out
 * of range - has no completion event and no failure event, so this is the ONLY
 * transition that can release the state. docs/radiant-security.md section 8.1
 * records why that matters more than it looks: sdk-ant serialises key exchange
 * device-wide, and the same shape here without this hook would deadlock
 * negotiation for every channel, permanently, with nothing to say so.
 */
void radiant_sec_pair_on_search(uint8_t ch);

/*
 * The host's 32-byte private scalar, and the local public key it produces.
 *
 * THE SCALAR COMES FROM THE HOST because the only entropy source on nRF54L is
 * psa_rng/CRACEN and reaching it drags nrf_security into every build. Nothing
 * here can check that what arrived is random: a host that sends a constant has
 * a node whose private key is public, and neither end can tell.
 */
int  radiant_sec_pair_set_scalar(uint8_t ch, const uint8_t *scalar);
int  radiant_sec_pair_local_pubkey(uint8_t ch, uint8_t *out);

/*
 * Complete the exchange against the peer's public key: derive K_root, install
 * it, and produce the comparison fingerprint.
 *
 * Refuses a small-order peer key. RFC 7748 leaves that optional for
 * Diffie-Hellman and it is mandatory here, because the result becomes a root
 * key - accepting it would let anyone able to inject one packet fix the group
 * key to a value they already know.
 */
int  radiant_sec_pair_peer(uint8_t ch, const uint8_t *peer_pub,
			   uint32_t *fingerprint);

/* Six digits, 0..999999. Both ends compute the same value without needing to
 * agree on who initiated. */
int  radiant_sec_pair_fingerprint(uint8_t ch, uint32_t *out);

bool radiant_sec_pair_is_open(uint8_t ch);
void radiant_sec_pair_reset(void);

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */

/* Key handling. Every backend implements these. */
int  radiant_sec_key_import(struct radiant_sec_key *k, const uint8_t *raw,
			    size_t bits);
void radiant_sec_key_destroy(struct radiant_sec_key *k);

/*
 * ── Level 2: modes. radiant_sec.c calls ONLY these. ────────────────────────
 */

/*
 * AES-CTR. `iv` is the initial counter block and is incremented AS A 128-BIT
 * BIG-ENDIAN INTEGER between blocks, which is what SP 800-38A F.5's vectors
 * do. RadiANT's nonce block ends in seven zero bytes, so for any payload this
 * protocol carries that is indistinguishable from incrementing the last byte -
 * but two implementations that disagree about it would diverge on the first
 * long message, so it is pinned rather than left to be discovered.
 *
 * XORs in place over `len` bytes. `iv` is not modified.
 */
int radiant_sec_ctr_xor(const struct radiant_sec_key *k,
			const uint8_t iv[RADIANT_SEC_BLOCK_BYTES],
			uint8_t *buf, size_t len);

/* AES-CMAC (RFC 4493), one shot. Writes the full 16-byte tag; the caller
 * truncates, because truncation length is a protocol decision. */
int radiant_sec_cmac(const struct radiant_sec_key *k, const uint8_t *msg,
		     size_t len, uint8_t tag[RADIANT_SEC_BLOCK_BYTES]);

/*
 * Incremental CMAC - this is what the spread-MAC window uses, and it is the
 * reason the window needs no payload buffer at all: CMAC absorbs 16 bytes at a
 * time, packets are delivered as they arrive, and the window is marked
 * retroactively when its tag lands.
 *
 * The context is opaque to callers even though its size is public, for the
 * same reason struct radiant_sec_key is: a hardware backend puts a session
 * handle where the software one puts a chaining value.
 */
struct radiant_sec_cmac_ctx {
	uint8_t  chain[RADIANT_SEC_BLOCK_BYTES];
	uint8_t  partial[RADIANT_SEC_BLOCK_BYTES];
	uint8_t  partial_len;
	uint8_t  started;
	const struct radiant_sec_key *key;
};

int radiant_sec_cmac_init(struct radiant_sec_cmac_ctx *c,
			  const struct radiant_sec_key *k);
int radiant_sec_cmac_update(struct radiant_sec_cmac_ctx *c, const uint8_t *msg,
			    size_t len);
int radiant_sec_cmac_final(struct radiant_sec_cmac_ctx *c,
			   uint8_t tag[RADIANT_SEC_BLOCK_BYTES]);

/*
 * ── Level 1: block. FOR BACKENDS, NOT FOR POLICY. ──────────────────────────
 *
 * radiant_sec.c must contain no call to this, and a grep in the zero-cost job
 * is what keeps that true. Policy calls modes; modes call blocks.
 */
int radiant_sec_aes_ecb(const struct radiant_sec_key *k,
			const uint8_t in[RADIANT_SEC_BLOCK_BYTES],
			uint8_t out[RADIANT_SEC_BLOCK_BYTES]);

/*
 * ── Portable helpers built on Level 2 ──────────────────────────────────────
 *
 * These are not part of the seam - they are byte layout plus one Level 2 call,
 * shared by every backend so that two backends cannot disagree about a block
 * this protocol pins to the byte. docs/radiant-security.md 3.3 is normative.
 */

/* Domain separation byte at nonce_block[8]. Without it a keystream block and a
 * MAC block can coincide, which is the omission that produces a scheme passing
 * every test and leaking the tag key. */
#define RADIANT_SEC_DOM_CTR        0x01
#define RADIANT_SEC_DOM_SPREAD_MAC 0x02
#define RADIANT_SEC_DOM_DESC_MAC   0x03

/*
 * The compat layer's, from docs/decisions/0008 and docs/radiant-security.md
 * section 11.4. It lives here rather than in radiant_core/radiant_sec_compat.h
 * for one reason: this is the list a reader checks a new domain byte against,
 * and a fourth value defined somewhere else is a collision waiting for the
 * fifth. Everything else about the compat layer - the subtype at position 9,
 * the two tag lengths, the window sizes - is in that header, behind
 * CONFIG_RADIANT_SEC_COMPAT.
 *
 * Nothing under CONFIG_RADIANT_SEC uses this byte. It is here to be reserved.
 */
#define RADIANT_SEC_DOM_COMPAT_MAC 0x04

/*
 * nonce_block = epoch[4 LE] || devnum[2 LE] || counter[2 LE] || dom || 0x00 x7
 */
void radiant_sec_nonce_block(uint8_t out[RADIANT_SEC_BLOCK_BYTES],
			     uint32_t epoch, uint16_t devnum, uint16_t counter,
			     uint8_t dom);

/*
 * SP 800-108 counter-mode KDF with CMAC as the PRF, one 128-bit output block:
 *
 *   kdf_block = 0x01 || label || 0x00 || epoch[4 LE] || base_devnum[2 LE]
 *               || 0x0080
 *
 * `label` is one of RADIANT_SEC_LABEL_*, NUL-terminated, and its terminator is
 * the block's own 0x00 separator. The epoch-less "id" label uses epoch = 0;
 * pass it explicitly rather than letting a caller guess.
 */
#define RADIANT_SEC_LABEL_ENC  "enc"
#define RADIANT_SEC_LABEL_AUTH "auth"
#define RADIANT_SEC_LABEL_ID   "id"
#define RADIANT_SEC_LABEL_CMD  "cmd"

/*
 * RESERVED, and used by nothing in radiant_core - the node application's
 * pairing-scalar derivation (ADR 0009) is the only holder. It is declared here
 * for the reason RADIANT_SEC_DOM_COMPAT_MAC is: this is the list a reader
 * checks a new label against, and a fifth label defined somewhere else is a
 * collision waiting for the sixth.
 *
 * NOTE THE TRAP, because it is a real one: an X25519 scalar is 32 bytes and
 * radiant_sec_kdf() emits 16, so passing this label to radiant_sec_kdf() does
 * NOT produce a pairing scalar and does not produce half of one either -
 * [L]_2 is part of the PRF input, so the 256-bit derivation's first block
 * differs from this label's 128-bit output in every byte. The node's
 * derivation is src/node/node_ident.c's, and it says so there.
 */
#define RADIANT_SEC_LABEL_PAIR "pair"

int radiant_sec_kdf(const struct radiant_sec_key *root, const char *label,
		    uint32_t epoch, uint16_t base_devnum,
		    struct radiant_sec_key *out);

/*
 * ═══════════════════════════════════════════════════════════════════════════
 * Policy: the per-channel transforms
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* Switch bits, as they appear in the 0xF1 host message. */
#define RADIANT_SEC_SW_CONF        0x01  /* X_CONF - confidentiality */
#define RADIANT_SEC_SW_AUTH        0x02  /* X_AUTH - authenticity */
#define RADIANT_SEC_SW_DROP_UNVER  0x04  /* deliver policy: 0 = deliver an
					  * unverified window (default),
					  * 1 = drop it */
#define RADIANT_SEC_SW_DESC_CONF   0x08  /* descriptor encryption - REFUSED in
					  * v1; the descriptor has no counter
					  * and therefore no nonce */

/*
 * The verdict every secured RX delivery carries. D6's deliver-as-unverified
 * only means something if a host cannot silently treat unverified as verified,
 * so this is not optional and not inferable from the absence of a flag.
 */
enum radiant_sec_verdict {
	RADIANT_SEC_VERDICT_CLEAR = 0,   /* not a secured channel, or a page
					  * outside the secured range */
	RADIANT_SEC_VERDICT_VERIFIED,    /* the window's tag verified */
	RADIANT_SEC_VERDICT_UNVERIFIED,  /* the window could not be verified -
					  * a loss, or a forgery. Delivered
					  * unless the channel says otherwise */
};

/* The legal MAC windows. W divides both 256 and 65536, which is the whole
 * reason the counter-derived window boundary resynchronises after a lost
 * packet - see docs/radiant-telemetry.md section 6. */
#define RADIANT_SEC_W_MIN     2
#define RADIANT_SEC_W_MAX     8
#define RADIANT_SEC_W_DEFAULT 2

/* The secured page range is host-set and BOUNDED, so the descriptor (0x00) and
 * the ANT+ common pages (0x50/0x51/0x52) stay in the clear mechanically rather
 * than by memory - and so a host cannot set the low bound to 0x00 and make its
 * own node undiscoverable by accident. */
#define RADIANT_SEC_PAGE_LO_MIN      0x01
#define RADIANT_SEC_PAGE_HI_MAX      0x1F
#define RADIANT_SEC_PAGE_LO_DEFAULT  0x01
#define RADIANT_SEC_PAGE_HI_DEFAULT  0x0F

#if defined(CONFIG_RADIANT_SEC)

/*
 * TX transform. Thread context, under api_lock, immediately before the frame
 * is built - the TX body is DMA'd and armed arbitrarily early, so ciphertext
 * must be final before radiant_sched_request_tx().
 *
 * Rewrites `pay` in place and takes ownership of byte [1] on a secured master
 * channel: a host counter would repeat across the retransmissions a master
 * makes every slot whether or not the application wrote a new body, and a
 * repeated counter under one (epoch, devnum) is keystream reuse.
 *
 * Returns RADIANT_SEC_OK when the payload was transformed or deliberately left
 * alone, and a negative code when the channel is configured for something the
 * backend cannot do.
 */
int radiant_sec_tx_transform(uint8_t ch, uint8_t *pay, uint8_t len);

/*
 * RX ingest. ISR CONTEXT, ZERO CRYPTO - it classifies and, when the packet is
 * ours, copies eight bytes into a queue for the pump.
 *
 *   RADIANT_SEC_RX_PLAIN  not a secured packet; the caller delivers it as
 *                         usual, and nothing here has touched it
 *   RADIANT_SEC_RX_DEFER  taken. THE CALLER MUST NOT DELIVER IT - the pump
 *                         will, plaintext and in order, from thread context
 *   RADIANT_SEC_RX_DROP   refused and counted. A non-broadcast frame carrying
 *                         a secured-range page lands here, which is what stops
 *                         an injector skipping the MAC by sending acknowledged
 *                         data instead of a broadcast
 */
#define RADIANT_SEC_RX_PLAIN 0
#define RADIANT_SEC_RX_DEFER 1
#define RADIANT_SEC_RX_DROP  2

int radiant_sec_rx_ingest(uint8_t ch, const uint8_t *pay, uint8_t len,
			  bool broadcast, uint64_t t_sync);

/*
 * All RX crypto. Event-thread context, called BEFORE the event drain so that
 * plaintext and verdict stay ordered behind the packets they describe.
 *
 * This is the whole of the no-crypto-in-ISR rule. Every RX packet already
 * transits the event thread before a host sees it and the MAC verdicts are
 * computed there anyway, so moving the CTR XOR here too costs no wakeup and no
 * host-visible latency - and it deletes the keystream-precompute state, the
 * miss-defer path, and the "no AES in the ISR, but XOR in the ISR" carve-out.
 * One unconditional rule instead of a nuanced one.
 */
void radiant_sec_pump(void);

/*
 * Where the pump's output goes. A DIRECT CALL, NOT A CALLBACK TABLE: an ops
 * struct is a .data relocation the linker cannot garbage-collect, and one
 * surviving relocation defeats the size gate.
 *
 * Both are weak and do nothing by default, so a build with no host adapter -
 * the unit-test application - links, and a suite can define its own to observe
 * what the pump produced.
 */
void radiant_sec_deliver(uint8_t ch, const uint8_t *pay, uint8_t len,
			 enum radiant_sec_verdict verdict, uint64_t t_sync);

/*
 * One window's verdict, emitted exactly once when the window that carries its
 * tag completes - so W+1 to 2W packets after the window itself, because the
 * tag lags one window (docs/radiant-security.md 3.2).
 *
 * `window_start` is the 16-bit counter of the window's first packet, so a host
 * can tell which delivered packets the verdict refers to.
 */
void radiant_sec_window_verdict(uint8_t ch, uint16_t window_start,
				enum radiant_sec_verdict verdict);

/* Lifecycle. No init call: the first SET_KEY allocates, unassign and reset
 * free. */
int  radiant_sec_set_key(uint8_t ch, const uint8_t *root, size_t bits,
			 uint16_t base_devnum);
int  radiant_sec_configure(uint8_t ch, uint8_t switches, uint8_t w,
			   uint8_t page_lo, uint8_t page_hi);

/*
 * Set the epoch and anchor its phase.
 *
 * `us_into_epoch` is how far into the epoch the caller believes it is now, and
 * it is what lets the receiver derive the packet counter from TIME rather than
 * from arrival history - the only thing that works for a mid-epoch join, for a
 * gap longer than 255 packets, and for sparse mode.
 *
 * `period_counts` is the channel period in counts of 1/32768 s, passed in
 * rather than read from radiant_channel so that this module depends on nothing
 * but the seam and the clock.
 *
 * REFUSES an epoch less than or equal to the current one, and refuses epochs
 * within RADIANT_SEC_EPOCH_HEADROOM of 0xFFFFFFFF so a counter wrap always has
 * room to advance into. No transform enables until this has been called after a
 * reset.
 */
#define RADIANT_SEC_EPOCH_HEADROOM 0x1000u

int  radiant_sec_set_epoch(uint8_t ch, uint32_t epoch, uint64_t us_into_epoch,
			   uint16_t period_counts);

/* The on-air device number, which is in the nonce. Separate from the
 * provisioning-time base device number in the KDF: on a Tier 2 node the first
 * re-rolls at power-up and the second does not. */
int  radiant_sec_set_devnum(uint8_t ch, uint16_t devnum);

/*
 * IDENTITY TIER 2, THE RECEIVER HALF: try a candidate on-air device number.
 *
 * A Tier 2 node re-rolls its device number at every power-up, which is what
 * answers the stalking case - a worn strap power-cycles between rides, so a
 * receiver planted near your house sees a different node each time. The cost is
 * that a STANDARD receiver must re-pair every session, and that cost is why
 * Tier 2 is opt-in and off by default.
 *
 * A keyed RadiANT receiver does not re-pair, and this is the whole of how. It
 * wildcard-searches its device type, hears some candidate node, and asks the
 * only question that survives a re-roll: does X_AUTH verify under my key? The
 * answer is cryptographic, so "which sensor is this" is decided by the MAC
 * rather than by a number an attacker can also read.
 *
 * Strictly the MAC answers with the KEY GROUP, not the node - two same-type
 * sensors sharing one household root are indistinguishable this way, which is
 * why the provisioning rule is one root per sensor.
 *
 * Mechanically this is set_devnum plus a clean slate: the RX window state
 * belonged to the previous candidate, and judging a new one on a window half
 * filled by the old one produces an unverified verdict that means nothing. The
 * base device number is untouched - it is fixed at provisioning, it does not
 * re-roll, and it is what the KDF binds - so no key is re-derived here and the
 * trial costs one nonce block per packet and nothing else.
 *
 * The caller drives the ordinary search path, calls this for each candidate,
 * and watches radiant_sec_last_verdict(): VERIFIED is the match. There is no
 * timeout here because there is no state to time out - a candidate that never
 * verifies is simply abandoned by the caller moving on to the next one.
 */
int  radiant_sec_try_devnum(uint8_t ch, uint16_t devnum);

void radiant_sec_channel_release(uint8_t ch);
void radiant_sec_reset(void);

/*
 * Is this channel transforming right now? The acknowledgement path asks,
 * because api_xfer_broadcast() memcpys the plaintext host buffer onto the air
 * and under X_CONF that hands a passive listener the plaintext of the same
 * page. A secured channel answers with the ciphertext already built for the
 * current counter instead - same nonce, same plaintext, identical ciphertext,
 * no reuse and no ISR crypto.
 */
bool radiant_sec_channel_is_secured(uint8_t ch);

/* The verdict for the packet the pump most recently produced, and the counters
 * 0xF4 reports. */
enum radiant_sec_verdict radiant_sec_last_verdict(uint8_t ch);

struct radiant_sec_stats {
	uint32_t windows_verified;
	uint32_t windows_unverified;
	uint32_t dropped_non_broadcast;
	uint32_t dropped_replay;
	uint32_t dropped_policy;
	uint32_t epoch_advances;
};

void radiant_sec_get_stats(uint8_t ch, struct radiant_sec_stats *out);

/*
 * The non-counter half of what 0xF4 reports: configuration and clock, as they
 * stand now.
 *
 * Kept apart from radiant_sec_stats deliberately. Those are monotone counters a
 * host subtracts to get a rate; these are levels a host reads to know what the
 * channel is currently doing. Merging them would make "did this change" and
 * "how many since last time" the same question, and they are not.
 *
 * `expected_index` is the packet index time says we should be at, which is what
 * makes a stalled or drifting link visible to a host: it advances whether or not
 * anything is arriving, so an expected index climbing against a flat verified
 * count is a link that has gone quiet rather than a receiver that has gone
 * wrong. Zero when no epoch is set.
 */
struct radiant_sec_state {
	uint8_t                  switches;
	uint8_t                  w;
	uint8_t                  page_lo;
	uint8_t                  page_hi;
	uint32_t                 epoch;
	uint16_t                 expected_index;
	enum radiant_sec_verdict last_verdict;
	bool                     secured;
};

void radiant_sec_get_state(uint8_t ch, struct radiant_sec_state *out);

#else  /* !CONFIG_RADIANT_SEC */

/*
 * The disabled shape. Scalars only, so each of these collapses to nothing
 * without the compiler having to prove a struct copy dead - and there is no
 * ops table anywhere for the linker to fail to garbage-collect.
 */
static inline int radiant_sec_tx_transform(uint8_t ch, uint8_t *p, uint8_t l)
{
	(void)ch; (void)p; (void)l;
	return RADIANT_SEC_OK;
}

static inline int radiant_sec_rx_ingest(uint8_t ch, const uint8_t *p, uint8_t l,
					bool broadcast, uint64_t t_sync)
{
	(void)ch; (void)p; (void)l; (void)broadcast; (void)t_sync;
	return 0;   /* RADIANT_SEC_RX_PLAIN */
}

static inline void radiant_sec_pump(void)
{
}

static inline void radiant_sec_channel_release(uint8_t ch)
{
	(void)ch;
}

static inline void radiant_sec_reset(void)
{
}

static inline enum radiant_sec_verdict radiant_sec_last_verdict(uint8_t ch)
{
	(void)ch;
	return RADIANT_SEC_VERDICT_CLEAR;
}

static inline bool radiant_sec_channel_is_secured(uint8_t ch)
{
	(void)ch;
	return false;
}

#endif /* CONFIG_RADIANT_SEC */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CORE_RADIANT_SEC_H_ */
