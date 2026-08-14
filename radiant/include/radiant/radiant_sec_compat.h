/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_compat - the two attestation tags a RadiANT node puts on an ANT+
 * device type, and nothing else.
 *
 * docs/radiant-security.md section 11.4 is normative;
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md pins the
 * bytes and records why there are two tiers rather than one.
 *
 * ── This file names no page and no field, deliberately ─────────────────────
 * A compat channel's data pages are ANT+ heart rate or power, byte for byte -
 * NOT this module's business. What's here takes an epoch, device number,
 * counter, and opaque 8-byte messages, and returns a truncated CMAC, with no
 * page number or field name inside it. That makes it testable against
 * synthetic payloads, usable unchanged by a profile nobody's written yet, and
 * unable to grow an opinion about heart rate a power meter has to work
 * around. Same discipline as "radiant_sec.c contains no call to
 * radiant_sec_aes_ecb()"; the `zero-cost` CI job greps for it too. Page
 * numbers, the subtype nibble's position in the page, the beacon, and
 * SWITCH/RETURN frames all live above this layer.
 *
 * ── Why two tiers ───────────────────────────────────────────────────────────
 * Tier I answers "is this stream from the holder of K_auth, now?", covers NO
 * payload, is verifiable on receipt with verification rate = delivery rate -
 * on by default at ~1.2% of slots. Tier II answers "were these exact bytes
 * sent?", costs one page in N, and one lost packet unverifies the whole
 * window - off by default. Neither is a degraded version of the other.
 *
 * ── Zero cost when off ─────────────────────────────────────────────────────
 * CONFIG_RADIANT_SEC_COMPAT is default n, depends on CONFIG_RADIANT_SEC, and
 * inherits radiant_sec.h's discipline unchanged (no ops struct, no structs in
 * hot-path signatures, no init call, no-op inlines when off). Tag entry
 * points refuse with RADIANT_SEC_ENOTSUP rather than OK, since a disabled
 * build answering OK would hand back a tag of stack garbage.
 *
 * ── Every clock reading is an argument ─────────────────────────────────────
 * Nothing below calls radiant_radio_now(); the caller passes the instant it
 * means. radiant_sec.c reads the clock itself (right for a module wired into
 * the schedule); this one is reached from a profile that already knows when
 * a slot is, which lets a whole day of attestation be tested in a
 * millisecond with no mock clock. Microseconds, 64-bit, monotone; origin is
 * the caller's business.
 */

#ifndef RADIANT_RADIANT_SEC_COMPAT_H_
#define RADIANT_RADIANT_SEC_COMPAT_H_

#include <stdint.h>
#include <stddef.h>

#include <radiant/radiant_sec.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The subtype, at nonce_block[9]. RADIANT_SEC_DOM_COMPAT_MAC (0x04) lives in
 * radiant_sec.h beside the other domain bytes for the same reason.
 *
 * Inside the MAC'd block, not only in the page: both tiers share one page
 * number, so the subtype is the only thing separating a Tier I tag from a
 * Tier II tag over the same counter value - a separator an attacker could
 * rewrite in a plain page byte would separate nothing.
 *
 * 0x03 is SWITCH/RETURN's, pinned here so the value can't be double-spent
 * before that layer exists. Nothing here computes that tag today.
 */
#define RADIANT_SEC_COMPAT_SUBTYPE_TIER_I    0x01
#define RADIANT_SEC_COMPAT_SUBTYPE_TIER_II   0x02
#define RADIANT_SEC_COMPAT_SUBTYPE_ANNOUNCE  0x03

/*
 * 0x04 is the INBOUND command's, the one subtype not under K_auth - a command
 * verifies under K_cmd (RADIANT_SEC_LABEL_CMD).
 *
 * Two separations, both wanted: the KEY separates what a receiver sends the
 * node from what the node broadcasts, so a recorded attestation can never be
 * replayed into the command path; the SUBTYPE separates it inside the block
 * too, since "different key" is a property of the deployment, and the block
 * is where this protocol puts its separations.
 */
#define RADIANT_SEC_COMPAT_SUBTYPE_COMMAND   0x04

/*
 * 40 bits for Tier I, 48 for Tier II - arithmetic, not a security judgement:
 * Tier I carries a 2-byte counter (no window index implied), leaving
 * 8-1-2 = 5 bytes. 2^-40 per attempt, against a mechanism rate-limited to one
 * attempt per T, is not the weak link.
 */
#define RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES   5
#define RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES  6

/*
 * The announcement's tag is 48 bits (bytes [2..7] of a frame whose [0]/[1]
 * are framing) - same width as Tier II because six bytes are what's left, but
 * NOT Tier II's function; its subtype makes a different claim about
 * different bytes.
 *
 * A command's is 40 bits: 3 bytes precede the tag, leaving 8-3=5. The forgery
 * bound that matters isn't 2^-40 anyway - an attacker without K_cmd is
 * guessing against a rate-limited node, and one WITH K_cmd is a keyholder,
 * the trust boundary rather than a break of it.
 */
#define RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES 6
#define RADIANT_SEC_COMPAT_CMD_TAG_BYTES      5

/* The covered bytes of an inbound command: everything in front of its tag. */
#define RADIANT_SEC_COMPAT_CMD_BYTES          3

/*
 * The Tier II window: N consecutive TRANSMITTED messages, N in {4, 8, 16, 32}.
 * Enumerated rather than ranged because N is simultaneously the airtime cost,
 * the verification latency and the DoS amplification factor, so an unenumerated
 * N is three surprises at once.
 */
#define RADIANT_SEC_COMPAT_N_MIN      4
#define RADIANT_SEC_COMPAT_N_MAX      32
#define RADIANT_SEC_COMPAT_N_DEFAULT  8

/* One transmitted ANT message is eight payload bytes, all of which Tier II
 * covers - including byte [0]. Unlike the spread tag, nothing rides in byte
 * [7] here: the compat tag has a page of its own. */
#define RADIANT_SEC_COMPAT_MSG_BYTES  8

/*
 * Bytes [1..7] of an attestation message - everything except byte [0], which
 * carries the subtype and is therefore the one byte this module will not write.
 * The layer above composes it from the subtype returned by the emit hook, so
 * the page byte is DERIVED from what went into the nonce rather than being a
 * second, independent statement of the same thing.
 */
#define RADIANT_SEC_COMPAT_BODY_BYTES 7

/* The beacon's key-group hint, trunc24( CMAC(K_id, epoch[4 LE]) ). Three bytes:
 * enough for a receiver holding a handful of roots to skip the ones that cannot
 * match, and not enough to be an identifier. It is not a security claim - a
 * collision is expected every ~16 million epochs and is settled by the
 * attestation tag, which is 40 or 48 bits. */
#define RADIANT_SEC_COMPAT_HINT_BYTES 3

/*
 * The switches, in the shape of radiant_sec.h's. Tier I is the default-on half
 * and Tier II the default-off one, and the defaults live in the layer that
 * reads configuration - this header states which bits exist, not which are set.
 */
#define RADIANT_SEC_COMPAT_SW_TIER_I   0x01  /* attest_id  - identity  */
#define RADIANT_SEC_COMPAT_SW_TIER_II  0x02  /* attest_data - the window */
#define RADIANT_SEC_COMPAT_SW_PIN      0x04  /* receiver-side pinning, below */

/*
 * T, the Tier I interval, in SECONDS and decoupled from the data rate - the
 * whole compatibility argument: at 4 Hz, 20 s is one page in ~81 (1.2% of
 * slots, below ANT+'s own 1.65% on common pages 80/81), and a slower profile
 * spends proportionally LESS.
 */
#define RADIANT_SEC_COMPAT_T_DEFAULT_S 20u
#define RADIANT_SEC_COMPAT_T_MIN_S     1u
#define RADIANT_SEC_COMPAT_T_MAX_S     3600u

/*
 * How stale a verified Tier I page may be before a pinned receiver stops
 * calling the stream verified: two intervals. One interval of slack rather
 * than none, since at the ~0.4% loss floor a single missing page is
 * ordinary; two consecutive misses (~1.6e-5) are a real signal.
 */
#define RADIANT_SEC_COMPAT_STALE_INTERVALS 2u

/*
 * ── Epoch recovery, and its bounds ─────────────────────────────────────────
 * The beacon carries no epoch, deliberately: for a hostless node the epoch is
 * the boot counter, and broadcasting a slowly-incrementing 32-bit number
 * every 30 s would be a device fingerprint surviving every other privacy
 * measure here. Occasional contact (receiver A this week, B next month, both
 * many boots out of date) survives instead as a forward search.
 *
 * THE SEARCH IS BOUNDED IN EVERY PHASE, HERE not buried in the .c, because
 * "search until it matches" against a re-provisioned sensor that never will
 * is an unbounded loop in a receiver's event thread.
 *
 *   FORWARD   65536, the "absurd" row of the cost table (11.3) - ~200 ms once
 *             on a Cortex-M4 with AES hardware. A sensor that's booted more
 *             often than that won't be found by trying 65537 either.
 *   BACKWARD  16: covers a receiver holding an unconfirmed candidate epoch,
 *             not a sensor gone backwards - that's the absolute scan.
 *   SCAN      65536 from zero: a re-provisioned strap is at a SMALL epoch, so
 *             this finds it in a handful of operations; the ceiling just
 *             bounds the failure case.
 *
 * Worst case: FORWARD + 1 + BACKWARD + SCAN + 1 operations.
 */
#define RADIANT_SEC_COMPAT_EPOCH_FORWARD  65536u
#define RADIANT_SEC_COMPAT_EPOCH_BACKWARD 16u
#define RADIANT_SEC_COMPAT_EPOCH_SCAN     65536u

/* Which phase answered. Reported rather than inferred: "recovered" and
 * "recovered by the absolute scan, so this sensor has been re-provisioned" are
 * different facts and only one of them is worth telling a user about. */
#define RADIANT_SEC_COMPAT_PATH_NONE     0
#define RADIANT_SEC_COMPAT_PATH_FORWARD  1
#define RADIANT_SEC_COMPAT_PATH_BACKWARD 2
#define RADIANT_SEC_COMPAT_PATH_SCAN     3

/*
 * The counters a host reads. Separate from struct radiant_sec_stats for the
 * same reason the compat layer has its own context: these count events on an
 * ANT+ device type, and folding them into the RadiANT-envelope numbers would
 * make "did my sensor verify" a question about two different channels at once.
 */
struct radiant_sec_compat_stats {
	uint32_t tier1_verified;
	uint32_t tier1_unverified;
	uint32_t tier1_replays;     /* a counter already seen. THE tier's whole
				     * replay defence, so it is counted rather
				     * than merely rejected */
	uint32_t tier2_verified;
	uint32_t tier2_unverified;
	uint32_t epoch_advances;    /* counter wraps, per D14 */
	uint32_t epoch_recoveries;
};

#if defined(CONFIG_RADIANT_SEC_COMPAT)

/*
 * nonce_block = epoch[4 LE] || devnum[2 LE] || counter[2 LE]
 *               || RADIANT_SEC_DOM_COMPAT_MAC || sub || 0x00 x6
 *
 * Section 3.3's block EXTENDED at position 9, not duplicated: the domain byte
 * stays where every other block in this protocol has it and positions 10..15
 * stay zero, so a reader comparing this against radiant_sec_nonce_block() sees
 * one added byte rather than a second layout to keep in step.
 *
 * `counter` is the attestation counter for Tier I and the window index for
 * Tier II. Both are 16 bits here and both are carried truncated in the page -
 * two bytes for Tier I, one for Tier II - so a receiver reconstructs the high
 * bits from time exactly as it already does for the data counter.
 *
 * `sub` is a nibble in 1..15; out is left untouched for anything else, which is
 * the only refusal a void function can make and is why the tag entry points
 * below do not take a subtype at all.
 */
void radiant_sec_compat_nonce_block(uint8_t out[RADIANT_SEC_BLOCK_BYTES],
				    uint32_t epoch, uint16_t devnum,
				    uint16_t counter, uint8_t sub);

/*
 * Tier I - the identity attestation. trunc40( CMAC(K_auth, nonce_block) ).
 *
 * IT COVERS NO PAYLOAD, AND THAT IS THE POINT rather than a limitation. The
 * tag proves "this stream comes from the holder of K_auth, now, and is not a
 * replay" and nothing else, so it is verifiable the moment it arrives and a
 * packet lost anywhere else costs nothing. Replay is closed by `att_counter`,
 * which is monotone and derivable from elapsed time.
 *
 * Returns RADIANT_SEC_OK, RADIANT_SEC_EINVAL on a null argument, or whatever
 * the CMAC refused with (RADIANT_SEC_ENOKEY for an empty key handle).
 */
int radiant_sec_compat_tier1_tag(const struct radiant_sec_key *k_auth,
				 uint32_t epoch, uint16_t devnum,
				 uint16_t att_counter,
				 uint8_t out[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES]);

/*
 * Tier II - the data attestation.
 *
 *   trunc48( CMAC(K_auth, nonce_block || p_1 || p_2 || ... || p_{N-1}) )
 *
 * `msgs` is (n - 1) * 8 bytes: the N-1 preceding TRANSMITTED messages, in
 * transmission order, each the full eight payload bytes with the page number
 * included. Transmitted, not data - so the common pages, the beacon and any
 * Tier I page inside the window are covered too, and a receiver counts messages
 * rather than trying to decide which ones were "data".
 *
 * A flat byte array and two scalars rather than an array of structs, per
 * radiant_sec.h's rule 2. The caller owns the buffer; nothing is retained.
 *
 * Returns RADIANT_SEC_EINVAL for a null argument or an `n` outside
 * {4, 8, 16, 32}.
 */
int radiant_sec_compat_tier2_tag(const struct radiant_sec_key *k_auth,
				 uint32_t epoch, uint16_t devnum,
				 uint16_t window_index, const uint8_t *msgs,
				 uint8_t n,
				 uint8_t out[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES]);

/*
 * ═══════════════════════════════════════════════════════════════════════════
 * The stream: emitting those tags and judging them on the way in
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Everything above is pure. Everything below holds per-channel state, and it is
 * still the same module because it still names no page: the layer above hands
 * it eight-byte messages and a subtype, and gets back a body and a verdict.
 *
 * ── Why this is not radiant_sec.c's context table ──────────────────────────
 *
 * A compat channel could not be a secured RadiANT channel:
 * radiant_sec_configure() bounds the secured range to 0x01..0x1F (a compat
 * attestation page is nowhere near it), and radiant_sec owns byte [1] of
 * every secured page while a compat data page's byte [1] belongs to the
 * profile. So state and switches are separate; only the key hierarchy is
 * shared (both derive K_auth from a root via radiant_sec_kdf()).
 */

/*
 * Install the root. K_auth and K_id are derived from it here, the same way a
 * secured channel derives them, so a receiver holding the group root gets both
 * halves of the compat surface - the hint search and the tag check - from the
 * one key it was given.
 *
 * `base_devnum` is the PROVISIONING-time device number that the KDF binds, not
 * the on-air one: on an identity Tier 2 node the second re-rolls at every boot
 * and the first does not.
 *
 * A new root closes the epoch gate, exactly as radiant_sec_set_key() does.
 */
int radiant_sec_compat_set_key(uint8_t ch, const uint8_t *root, size_t bits,
			       uint16_t base_devnum);

/*
 * `switches` is a mask of RADIANT_SEC_COMPAT_SW_*; `n` is the Tier II window,
 * in {4, 8, 16, 32}, and is ignored when Tier II is off; `t_s` is Tier I's
 * interval in seconds.
 *
 * RADIANT_SEC_COMPAT_SW_PIN is the downgrade defence, receiver-side: strip
 * the beacon from the air and a naive receiver falls back to clear, so an
 * enrolled receiver pins the sensor instead - with a key installed, a stream
 * carrying no attestation reads UNVERIFIED, not CLEAR (which stays "no key
 * here"). Sensor-side advertising is a discovery aid, never a security
 * decision.
 */
int radiant_sec_compat_configure(uint8_t ch, uint8_t switches, uint8_t n,
				 uint16_t t_s);

/*
 * The epoch and its phase. `now_us` and `us_into_epoch` are the caller's clock
 * and how far into the epoch it believes it is, so the anchor is
 * `now_us - us_into_epoch`; `period_counts` is the channel period in counts of
 * 1/32768 s, which is what turns elapsed time into a transmitted-message index.
 *
 * Refuses a non-forward epoch and refuses one inside RADIANT_SEC_EPOCH_HEADROOM
 * of 0xFFFFFFFF, both for D3's reasons. The ONE route that may move an epoch
 * backwards is radiant_sec_compat_adopt_epoch() below, and it exists because a
 * re-provisioned sensor genuinely does.
 */
int radiant_sec_compat_set_epoch(uint8_t ch, uint32_t epoch, uint64_t now_us,
				 uint64_t us_into_epoch, uint16_t period_counts);

/* The on-air device number, which is in the nonce. */
int radiant_sec_compat_set_devnum(uint8_t ch, uint16_t devnum);

void radiant_sec_compat_channel_release(uint8_t ch);
void radiant_sec_compat_reset(void);

/*
 * ── Transmit ───────────────────────────────────────────────────────────────
 *
 * Two calls per slot and they are not interchangeable:
 *
 *     sub = radiant_sec_compat_tx_attest(ch, now, body, sizeof body);
 *     if (sub > 0) { pay[0] = <the page byte for sub>; memcpy(&pay[1], body, 7); }
 *     else         { ...the profile builds its own message... }
 *     radiant_sec_compat_tx_sent(ch, pay, 8);
 *
 * tx_attest() answers "does this slot owe an attestation page?" and fills bytes
 * [1..7] of it; tx_sent() is told what actually went on the air, EVERY message
 * without exception, because Tier II's window is N consecutive TRANSMITTED
 * messages rather than N data messages - the common pages, the beacon and the
 * Tier I page are inside the tag too, and a caller that reported only its data
 * pages would produce a window neither side agrees about.
 *
 * When both tiers fall due in one slot TIER II WINS and Tier I slips a slot.
 * Tier II's window closes at the Nth message and has no slack at all; Tier I's
 * counter is derived from elapsed time, so a slot of slip is invisible to it.
 *
 * Returns a subtype in {RADIANT_SEC_COMPAT_SUBTYPE_TIER_I,
 * ..._TIER_II} when this slot is claimed, 0 when nothing is due, or a negative
 * code.
 */
int radiant_sec_compat_tx_attest(uint8_t ch, uint64_t now_us, uint8_t *body,
				 uint8_t len);

int radiant_sec_compat_tx_sent(uint8_t ch, const uint8_t *pay, uint8_t len);

/*
 * ── Receive ────────────────────────────────────────────────────────────────
 *
 * One call per received message. `sub` is 0 for an ordinary message and the
 * subtype nibble for an attestation page - the layer above reads it out of byte
 * [0], which is the only place page numbers are known.
 *
 * Tier I is verified ON RECEIPT and that is the property the tier exists for:
 * nothing outside the page is covered, so a delivered page verifies whatever
 * else was lost, and the verification rate equals the page delivery rate no
 * matter how rarely it is sent. Tier II can only be judged when its window
 * closes, which is what one lost packet costs.
 *
 * Returns RADIANT_SEC_OK, RADIANT_SEC_EBADMAC when a tag did not verify,
 * RADIANT_SEC_EREPLAY for a counter already seen, or another negative code. The
 * verdict also arrives through the hook below, and the last one is readable.
 */
int radiant_sec_compat_rx(uint8_t ch, uint8_t sub, const uint8_t *pay,
			  uint8_t len, uint64_t t_sync);

/*
 * Where a verdict goes. A DIRECT CALL, NOT A CALLBACK TABLE, for the reason
 * radiant_sec.h gives: an ops struct is a .data relocation the linker cannot
 * garbage-collect, and one surviving relocation defeats the size gate. Weak and
 * empty by default, so a build with no consumer links.
 *
 * `counter` is the attestation counter for Tier I and the window index for
 * Tier II - the same field the nonce carries, so a host can say which packets a
 * verdict refers to.
 */
void radiant_sec_compat_verdict(uint8_t ch, uint8_t sub, uint16_t counter,
				enum radiant_sec_verdict verdict);

/* The verdict of the last attestation judged on this channel. */
enum radiant_sec_verdict radiant_sec_compat_last_verdict(uint8_t ch);

/*
 * The verdict for the DATA STREAM as it stands at `now_us`, which is the
 * question a host actually asks.
 *
 *   CLEAR       no key here, or attestation is not expected. Unchanged meaning.
 *   VERIFIED    a Tier I page verified inside the staleness horizon: this
 *               stream is coming from the holder of K_auth, now. IT IS AN
 *               IDENTITY CLAIM AND NOT AN INTEGRITY CLAIM - Tier I covers no
 *               payload, and a receiver that wants "were these exact bytes
 *               sent by that holder" enables Tier II and reads its per-window
 *               verdict instead.
 *   UNVERIFIED  pinned, and no attestation has arrived inside the horizon.
 */
enum radiant_sec_verdict radiant_sec_compat_stream_verdict(uint8_t ch,
							   uint64_t now_us);

void radiant_sec_compat_get_stats(uint8_t ch,
				  struct radiant_sec_compat_stats *out);

/*
 * ── Epoch recovery ─────────────────────────────────────────────────────────
 *
 * trunc24( CMAC(K_id, epoch[4 LE]) ) - the beacon's "is this one of mine",
 * computed for an arbitrary epoch under this channel's K_id. Epoch-derived and
 * never static: a fixed group string broadcast every 30 s would be a better
 * tracking identifier than the device number.
 */
int radiant_sec_compat_hint(uint8_t ch, uint32_t epoch,
			    uint8_t out[RADIANT_SEC_COMPAT_HINT_BYTES]);

/*
 * ── The derived locator ────────────────────────────────────────────────────
 *
 *   trunc16( CMAC(K_id, "priv" || epoch[4 LE]) ), 0x0000 excluded
 *   collision: rederive with a 1-byte suffix, 0x00, 0x01, ...
 *
 * Same shape as the hint above and for the same reason (K_id is epoch-less);
 * a NUMBER, not a channel or page - what the caller does with sixteen bits is
 * the caller's business.
 *
 * WHAT MAKES AN ANNOUNCEMENT OPTIONAL: any root holder computes where the
 * node went from the epoch it already has, so the announcement only saves a
 * search; an observer without the key cannot predict the number at all,
 * closing switch-time linkage down to timing correlation when nothing is
 * announced.
 *
 * `attempt` walks the sequence (0 = first candidate, 1/2/3 = fallbacks before
 * a wildcard search); RADIANT_SEC_COMPAT_LOCATOR_TRIES is the count, kept
 * here so both ends agree. Two skip rules fold into one walk: "derivation 0
 * with no suffix, then suffix 0x00, 0x01, ..., dropping any 0x0000 result" -
 * a node and searcher that disagreed about whether zero counts would
 * disagree about every candidate after it.
 *
 * Returns RADIANT_SEC_OK, RADIANT_SEC_ENOKEY with no root, or
 * RADIANT_SEC_EINVAL for null `out` or an unreachable `attempt` (needs 256
 * consecutive zeros - a bug, not a collision).
 */
#define RADIANT_SEC_COMPAT_LOCATOR_TRIES    4u
#define RADIANT_SEC_COMPAT_LOCATOR_WILDCARD 0x0000u

/*
 * "priv", and it is a CMAC MESSAGE PREFIX rather than a fifth
 * RADIANT_SEC_LABEL_*. The distinction is not pedantry: a KDF block is sixteen
 * bytes beginning 0x01, and this message is eight or nine beginning 'p', so the
 * two inputs cannot collide even though both are CMACs under a derived key.
 * Named here so that the list radiant_sec.h asks a reader to check a new label
 * against stays the list of KEYS, and this stays a message.
 */
#define RADIANT_SEC_COMPAT_LOCATOR_LABEL    "priv"

int radiant_sec_compat_locator(uint8_t ch, uint32_t epoch, uint8_t attempt,
			       uint16_t *out);

/*
 * ── The announcement's tag, and the command's ──────────────────────────────
 * Both take the covered bytes and the instant (not a clock read), since the
 * nonce's counter is Tier I's interval index, a function of time on both
 * sides. Neither message carries a counter field - it's DERIVED from the
 * shared epoch anchor, so a message replayed into another interval just
 * fails the tag.
 *
 * The cost of that exactness: a message straddling an interval boundary
 * verifies on one side only. At default T that's one boundary per 20 s
 * against an announcement repeated 8x over a countdown - costs at most one
 * copy, versus accepting a neighbouring interval accepting a whole interval
 * of replay.
 *
 * `len` is the covered length: 8 for an announcement (whole frame, bytes
 * [0]/[1] included) and RADIANT_SEC_COMPAT_CMD_BYTES for a command.
 *
 * Returns RADIANT_SEC_OK; RADIANT_SEC_EBADMAC when the tag doesn't match;
 * RADIANT_SEC_ENOKEY with no key/epoch; RADIANT_SEC_EINVAL for a null
 * argument or wrong length.
 */
int radiant_sec_compat_announce_tag(uint8_t ch, uint64_t now_us,
				    const uint8_t *msg, uint8_t len,
				    uint8_t out[RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES]);

int radiant_sec_compat_announce_verify(uint8_t ch, uint64_t t_sync,
				       const uint8_t *msg, uint8_t len,
				       const uint8_t *tag);

/*
 * The command arm, under K_cmd. The NODE is the verifier here - the opposite
 * direction from everything else in this file, and why the key differs: a
 * receiver that can verify the node's attestation must not thereby forge a
 * command to it.
 *
 * Decides nothing: answers "did the holder of K_cmd send these bytes, in
 * this interval". Whether the node acts is policy above this file.
 */
int radiant_sec_compat_cmd_tag(uint8_t ch, uint64_t now_us, const uint8_t *msg,
			       uint8_t len,
			       uint8_t out[RADIANT_SEC_COMPAT_CMD_TAG_BYTES]);

int radiant_sec_compat_cmd_verify(uint8_t ch, uint64_t t_sync,
				  const uint8_t *msg, uint8_t len,
				  const uint8_t *tag);

/*
 * PATH ONE, advertise = on: search for the epoch whose hint is the one the
 * beacon carried. One CMAC per candidate.
 *
 * `last_seen` is what this receiver recorded the last time it heard this
 * sensor. `*ops` is the number of candidate epochs examined, which is the
 * cost-table number plus one: the search tries `last_seen` ITSELF first,
 * because a sensor that has not rebooted since is the common case and not an
 * edge case. `*path` says which phase answered.
 *
 * A HINT MATCH IS NOT A CONFIRMATION. Twenty-four bits collide about every 16
 * million epochs, so the caller adopts the candidate and then lets the first
 * attestation judge it: a 40- or 48-bit tag cannot be survived by a hint
 * collision, and a collision therefore shows up as one unverified window and a
 * resumed search rather than as a receiver quietly following the wrong epoch.
 *
 * Returns RADIANT_SEC_OK, or RADIANT_SEC_EBADMAC when every bounded phase has
 * been exhausted - which is a real answer and not a timeout.
 */
int radiant_sec_compat_recover_hint(uint8_t ch,
				    const uint8_t hint[RADIANT_SEC_COMPAT_HINT_BYTES],
				    uint32_t last_seen, uint32_t *found,
				    uint32_t *ops, uint8_t *path);

/*
 * PATH TWO, advertise = off: there is no hint to search against, so the
 * candidate epochs are trial-verified against an attestation tag directly. An
 * advertise-off node is slower to re-acquire, not undiscoverable to a
 * keyholder.
 *
 * This is the path that otherwise gets written and never exercised, which is
 * why it is a first-class entry point rather than a note in the ADR. Tier I
 * costs one derivation and one block per candidate; the Tier II form below
 * costs N.
 */
int radiant_sec_compat_recover_tier1(uint8_t ch, uint16_t devnum,
				     uint16_t att_counter,
				     const uint8_t tag[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES],
				     uint32_t last_seen, uint32_t *found,
				     uint32_t *ops, uint8_t *path);

int radiant_sec_compat_recover_tier2(uint8_t ch, uint16_t devnum,
				     uint16_t window_index, const uint8_t *msgs,
				     uint8_t n,
				     const uint8_t tag[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES],
				     uint32_t last_seen, uint32_t *found,
				     uint32_t *ops, uint8_t *path);

/*
 * Take a recovered epoch, MOVING BACKWARDS IF THAT IS WHERE IT WENT.
 *
 * The one entry point that may lower an epoch: D3's monotonicity rule binds
 * the epoch AUTHORITY (the sensor), not a receiver reading it off the air,
 * and a re-provisioned sensor genuinely is at a lower epoch. Refusing to
 * follow it would leave the receiver permanently deaf to a strap the user
 * just reset - a support call, not a security property.
 *
 * Clears window state and the Tier I replay high-water mark, since a fresh
 * epoch has its own counter space and judging it against the old mark would
 * reject every page.
 */
int radiant_sec_compat_adopt_epoch(uint8_t ch, uint32_t epoch);

#else  /* !CONFIG_RADIANT_SEC_COMPAT */

/*
 * The disabled shape. Scalars and uint8_t * only, so each collapses to nothing
 * without the compiler having to prove a struct copy dead, and there is no ops
 * table anywhere for the linker to fail to garbage-collect.
 */
static inline void radiant_sec_compat_nonce_block(uint8_t *out, uint32_t epoch,
						  uint16_t devnum,
						  uint16_t counter, uint8_t sub)
{
	(void)out; (void)epoch; (void)devnum; (void)counter; (void)sub;
}

static inline int radiant_sec_compat_tier1_tag(const struct radiant_sec_key *k,
					       uint32_t epoch, uint16_t devnum,
					       uint16_t att_counter,
					       uint8_t *out)
{
	(void)k; (void)epoch; (void)devnum; (void)att_counter; (void)out;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_tier2_tag(const struct radiant_sec_key *k,
					       uint32_t epoch, uint16_t devnum,
					       uint16_t window_index,
					       const uint8_t *msgs, uint8_t n,
					       uint8_t *out)
{
	(void)k; (void)epoch; (void)devnum; (void)window_index; (void)msgs;
	(void)n; (void)out;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_set_key(uint8_t ch, const uint8_t *root,
					     size_t bits, uint16_t base_devnum)
{
	(void)ch; (void)root; (void)bits; (void)base_devnum;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_configure(uint8_t ch, uint8_t switches,
					       uint8_t n, uint16_t t_s)
{
	(void)ch; (void)switches; (void)n; (void)t_s;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_set_epoch(uint8_t ch, uint32_t epoch,
					       uint64_t now_us,
					       uint64_t us_into_epoch,
					       uint16_t period_counts)
{
	(void)ch; (void)epoch; (void)now_us; (void)us_into_epoch;
	(void)period_counts;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_set_devnum(uint8_t ch, uint16_t devnum)
{
	(void)ch; (void)devnum;
	return RADIANT_SEC_ENOTSUP;
}

static inline void radiant_sec_compat_channel_release(uint8_t ch)
{
	(void)ch;
}

static inline void radiant_sec_compat_reset(void)
{
}

/*
 * 0, not ENOTSUP: "no attestation page is due this slot" is the true answer on
 * a build with no attestation, and it is the one that lets a profile call this
 * unconditionally in its slot loop without an #ifdef of its own.
 */
static inline int radiant_sec_compat_tx_attest(uint8_t ch, uint64_t now_us,
					       uint8_t *body, uint8_t len)
{
	(void)ch; (void)now_us; (void)body; (void)len;
	return 0;
}

static inline int radiant_sec_compat_tx_sent(uint8_t ch, const uint8_t *pay,
					     uint8_t len)
{
	(void)ch; (void)pay; (void)len;
	return RADIANT_SEC_OK;
}

static inline int radiant_sec_compat_rx(uint8_t ch, uint8_t sub,
					const uint8_t *pay, uint8_t len,
					uint64_t t_sync)
{
	(void)ch; (void)sub; (void)pay; (void)len; (void)t_sync;
	return RADIANT_SEC_OK;
}

static inline enum radiant_sec_verdict radiant_sec_compat_last_verdict(uint8_t ch)
{
	(void)ch;
	return RADIANT_SEC_VERDICT_CLEAR;
}

static inline enum radiant_sec_verdict
radiant_sec_compat_stream_verdict(uint8_t ch, uint64_t now_us)
{
	(void)ch; (void)now_us;
	return RADIANT_SEC_VERDICT_CLEAR;
}

static inline void
radiant_sec_compat_get_stats(uint8_t ch, struct radiant_sec_compat_stats *out)
{
	(void)ch; (void)out;
}

static inline int radiant_sec_compat_hint(uint8_t ch, uint32_t epoch,
					  uint8_t *out)
{
	(void)ch; (void)epoch; (void)out;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_locator(uint8_t ch, uint32_t epoch,
					     uint8_t attempt, uint16_t *out)
{
	(void)ch; (void)epoch; (void)attempt; (void)out;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_announce_tag(uint8_t ch, uint64_t now_us,
						  const uint8_t *msg, uint8_t len,
						  uint8_t *out)
{
	(void)ch; (void)now_us; (void)msg; (void)len; (void)out;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_announce_verify(uint8_t ch, uint64_t t_sync,
						     const uint8_t *msg,
						     uint8_t len,
						     const uint8_t *tag)
{
	(void)ch; (void)t_sync; (void)msg; (void)len; (void)tag;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_cmd_tag(uint8_t ch, uint64_t now_us,
					     const uint8_t *msg, uint8_t len,
					     uint8_t *out)
{
	(void)ch; (void)now_us; (void)msg; (void)len; (void)out;
	return RADIANT_SEC_ENOTSUP;
}

/*
 * ENOTSUP and never OK, unlike tx_attest()'s 0 above. "No attestation page is
 * due" is a true answer on a build with no attestation; "this command is
 * authentic" is not, and a disabled build that answered it would be a node that
 * acted on an unverified command precisely when it had no way to verify one.
 */
static inline int radiant_sec_compat_cmd_verify(uint8_t ch, uint64_t t_sync,
						const uint8_t *msg, uint8_t len,
						const uint8_t *tag)
{
	(void)ch; (void)t_sync; (void)msg; (void)len; (void)tag;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_recover_hint(uint8_t ch,
						  const uint8_t *hint,
						  uint32_t last_seen,
						  uint32_t *found, uint32_t *ops,
						  uint8_t *path)
{
	(void)ch; (void)hint; (void)last_seen; (void)found; (void)ops;
	(void)path;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_recover_tier1(uint8_t ch, uint16_t devnum,
						   uint16_t att_counter,
						   const uint8_t *tag,
						   uint32_t last_seen,
						   uint32_t *found,
						   uint32_t *ops, uint8_t *path)
{
	(void)ch; (void)devnum; (void)att_counter; (void)tag; (void)last_seen;
	(void)found; (void)ops; (void)path;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_recover_tier2(uint8_t ch, uint16_t devnum,
						   uint16_t window_index,
						   const uint8_t *msgs, uint8_t n,
						   const uint8_t *tag,
						   uint32_t last_seen,
						   uint32_t *found,
						   uint32_t *ops, uint8_t *path)
{
	(void)ch; (void)devnum; (void)window_index; (void)msgs; (void)n;
	(void)tag; (void)last_seen; (void)found; (void)ops; (void)path;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_adopt_epoch(uint8_t ch, uint32_t epoch)
{
	(void)ch; (void)epoch;
	return RADIANT_SEC_ENOTSUP;
}

#endif /* CONFIG_RADIANT_SEC_COMPAT */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADIANT_SEC_COMPAT_H_ */
