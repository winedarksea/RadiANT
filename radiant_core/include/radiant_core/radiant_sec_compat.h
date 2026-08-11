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
 *
 * A compat channel exists to be read by a Garmin head unit, so its data pages
 * are ANT+ heart rate or ANT+ power, byte for byte. THAT IS NOT THIS MODULE'S
 * BUSINESS. What is here takes an epoch, a device number, a counter and some
 * opaque eight-byte messages, and returns a truncated CMAC. It contains no page
 * number and no field name, which is what makes it
 *
 *   - testable against synthetic payloads with no profile in the loop,
 *   - usable unchanged by a profile nobody has written yet, and
 *   - unable to grow an opinion about heart rate that a power meter then has to
 *     work around.
 *
 * Same discipline as "radiant_sec.c contains no call to radiant_sec_aes_ecb()",
 * and the `zero-cost` CI job greps for it in the same spirit. The page numbers,
 * the subtype nibble's position IN THE PAGE, the beacon and the SWITCH/RETURN
 * frames all live above this layer.
 *
 * ── Why two tiers, in one paragraph ────────────────────────────────────────
 *
 * Authenticating an identity and authenticating a data stream are two different
 * things with two different costs. Tier I answers "is this stream from the
 * holder of K_auth, now?" and covers NO PAYLOAD, so it is verifiable on receipt
 * and its verification rate equals its page delivery rate no matter how rarely
 * it is sent - which is what lets it be on by default at ~1.2% of slots. Tier II
 * answers "were these exact bytes sent by that holder?", costs one page in N,
 * and one lost packet unverifies the whole window - which is why it is off by
 * default. Neither is a degraded version of the other.
 *
 * ── Zero cost when off ─────────────────────────────────────────────────────
 *
 * CONFIG_RADIANT_SEC_COMPAT is default n and depends on CONFIG_RADIANT_SEC, and
 * the discipline is radiant_sec.h's, inherited unchanged: no ops struct, no
 * structs in hot-path signatures, no init call, and no-op static inlines below
 * when the symbol is off. The tag entry points refuse with
 * RADIANT_SEC_ENOTSUP rather than returning OK, because a disabled build that
 * answered OK would hand its caller a tag of stack garbage.
 *
 * ── Every clock reading is an argument ─────────────────────────────────────
 *
 * Nothing below calls radiant_radio_now(), and the .c includes no HAL header:
 * the caller passes the instant it means. radiant_sec.c reads the clock itself
 * and that is right for a module wired into the schedule; this one is reached
 * from a profile that already knows when a slot is, and keeping the clock out
 * of it is what lets a whole day of attestation be tested in a millisecond
 * without a mock clock in the loop. Microseconds, 64-bit, monotone, and the
 * origin is the caller's business.
 */

#ifndef RADIANT_CORE_RADIANT_SEC_COMPAT_H_
#define RADIANT_CORE_RADIANT_SEC_COMPAT_H_

#include <stdint.h>
#include <stddef.h>

#include <radiant_core/radiant_sec.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The subtype, at nonce_block[9]. RADIANT_SEC_DOM_COMPAT_MAC (0x04) is in
 * radiant_sec.h beside the other three domain bytes, because that is the byte a
 * reader compares against them.
 *
 * THE SUBTYPE IS INSIDE THE MAC'D BLOCK AND NOT ONLY IN THE PAGE. Both tiers
 * ride one page number - the 7-bit page namespace is why - so the subtype is
 * the only thing separating a Tier I tag from a Tier II tag over the same
 * counter value, and a separator an attacker rewrites in a page byte separates
 * nothing.
 *
 * 0x03 is the SWITCH/RETURN announcement's, pinned here rather than left free
 * so the value cannot be spent twice before the layer that emits it exists.
 * Nothing in this module computes that tag today.
 */
#define RADIANT_SEC_COMPAT_SUBTYPE_TIER_I    0x01
#define RADIANT_SEC_COMPAT_SUBTYPE_TIER_II   0x02
#define RADIANT_SEC_COMPAT_SUBTYPE_ANNOUNCE  0x03

/*
 * 0x04 is the INBOUND command's, and it is the one subtype that is not under
 * K_auth: a command is verified under K_cmd (RADIANT_SEC_LABEL_CMD), which had
 * no user anywhere in the tree until Layer C's trigger needed one.
 *
 * Two separations rather than one, and both are wanted. The KEY separates a
 * thing a receiver sends to the node from the three things the node broadcasts,
 * so a recorded attestation can never be replayed into the command path however
 * the bytes are rearranged. The SUBTYPE separates it inside the block anyway,
 * because "it is under a different key" is a property of the deployment - one
 * root, four derived keys - rather than of the block, and the block is where
 * this protocol puts its separations.
 */
#define RADIANT_SEC_COMPAT_SUBTYPE_COMMAND   0x04

/*
 * 40 bits for Tier I and 48 for Tier II, and the difference is arithmetic
 * rather than a security judgement: Tier I carries a two-byte counter in its
 * page because no window index is implied, and 8 - 1 - 2 = 5 bytes are what is
 * left. 2^-40 per attempt, against a mechanism rate-limited to one attempt per
 * T by construction, is not the weak link in this system.
 */
#define RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES   5
#define RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES  6

/*
 * The announcement's tag is 48 bits, in a frame of its own that carries nothing
 * else: bytes [2..7] of a frame whose [0] and [1] belong to the framing
 * convention. Tier II's width for the same reason Tier II has it - six bytes
 * are what is left - and NOT Tier II's function, because that one's subtype
 * says "this covers the last N-1 transmitted messages", which is a different
 * claim about different bytes.
 *
 * A command's is 40 bits, because a command carries three bytes of its own
 * before the tag and 8 - 3 = 5 is what is left. The forgery bound that matters
 * for it is not 2^-40 anyway: an attacker without K_cmd is guessing against a
 * node that rate-limits accepted commands, and one that HAS K_cmd is a
 * keyholder, which is the trust boundary rather than a break of it.
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
 * T, the Tier I interval, in SECONDS and decoupled from the data rate. That
 * decoupling is the whole compatibility argument: at 4 Hz, 20 s is one page in
 * ~81 - 1.2% of slots, below the 1.65% ANT+ itself spends on common pages 80
 * and 81 - and a slower profile spends proportionally LESS, not more.
 */
#define RADIANT_SEC_COMPAT_T_DEFAULT_S 20u
#define RADIANT_SEC_COMPAT_T_MIN_S     1u
#define RADIANT_SEC_COMPAT_T_MAX_S     3600u

/*
 * How stale a verified Tier I page may be before a pinned receiver stops
 * calling the stream verified: two intervals, i.e. the page due at T is a whole
 * interval late. One interval of slack rather than none, because at the
 * characterised ~0.4% loss floor a single missing page is ordinary and
 * unpinning a stream over it would make the mechanism cry wolf; two consecutive
 * misses are ~1.6e-5 and are a signal.
 */
#define RADIANT_SEC_COMPAT_STALE_INTERVALS 2u

/*
 * ── Epoch recovery, and its bounds ─────────────────────────────────────────
 *
 * The beacon carries no epoch, deliberately: for a hostless node the epoch is
 * the boot counter, and a slowly incrementing 32-bit number broadcast every 30 s
 * is a device fingerprint that survives every other privacy measure here. What
 * the removed field has to keep satisfying is occasional contact - receiver A
 * this week, receiver B next month, each many boots out of date and neither able
 * to ask the sensor anything - and it survives as a forward search.
 *
 * THE SEARCH IS BOUNDED IN EVERY PHASE AND THE BOUNDS ARE HERE, not buried in
 * the .c, because "search until it matches" against a re-provisioned sensor that
 * will never match is an unbounded loop in a receiver's event thread.
 *
 *   FORWARD   65536 is the "absurd" row of the cost table in
 *             docs/radiant-security.md section 11.3 - ~200 ms once, on a
 *             Cortex-M4 with AES hardware. A sensor that has booted more often
 *             than that since this receiver last heard it is not going to be
 *             found by trying 65537 either.
 *   BACKWARD  16, the small probe. It covers a receiver that recorded a
 *             candidate epoch which never went on to confirm, not a sensor that
 *             went backwards - that is what the absolute scan is for.
 *   SCAN      65536 from zero, the re-provisioned case: a strap whose boot
 *             counter was reset is at a SMALL epoch, so a scan from 0 finds it
 *             in a handful of operations and the ceiling is only there to make
 *             the failure bounded.
 *
 * Total worst case is FORWARD + 1 + BACKWARD + SCAN + 1 operations, and a
 * caller may hold that number against the table rather than hoping.
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
 * A compat channel is not a secured RadiANT channel and could not be one:
 * radiant_sec_configure() bounds the secured page range to 0x01..0x1F so that
 * the descriptor and the common pages stay in the clear mechanically, and a
 * compat attestation page is nowhere near that range. The two also disagree
 * about the payload: radiant_sec OWNS byte [1] of every secured page, and a
 * compat data page is a byte-exact ANT+ page whose byte [1] belongs to the
 * profile. So the state is its own, the switches are their own, and the one
 * thing they share is the key hierarchy - both derive K_auth from a root
 * through radiant_sec_kdf().
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
 * RADIANT_SEC_COMPAT_SW_PIN IS THE DOWNGRADE DEFENCE AND IT IS RECEIVER-SIDE.
 * Strip the beacon from the air and a naive receiver falls back to clear, so a
 * receiver that has enrolled a sensor pins it: with a key installed and
 * attestation expected, a stream carrying no attestation reads UNVERIFIED
 * rather than CLEAR. CLEAR keeps meaning "no key here". Sensor-side advertising
 * is a discovery aid and never a security decision.
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
 * The same shape as the hint above and for the same reason - K_id is
 * epoch-less, so one CMAC answers it - and this module computes it because K_id
 * is here and only here. It is a NUMBER, not a channel and not a page: what the
 * caller does with sixteen bits is the caller's business, and this file stays
 * unable to name the thing they identify.
 *
 * IT IS WHAT MAKES AN ANNOUNCEMENT OPTIONAL. Any holder of the root computes
 * where the node went from the epoch it already has, so the announcement saves
 * a search rather than being the only way to find the node; a receiver enrolled
 * after the switch finds a node that is already gone, with nothing to have
 * missed; and an observer without the key cannot predict the number at all,
 * which is what closes the switch-time linkage down to timing correlation when
 * nothing is announced.
 *
 * `attempt` walks the sequence: 0 is the first candidate, and 1, 2, 3 are what
 * a searcher tries next before falling back to a wildcard search.
 * RADIANT_SEC_COMPAT_LOCATOR_TRIES is how many of them a searcher tries, and it
 * is here rather than in the searcher so that both ends count the same way.
 *
 * TWO RULES ARE FOLDED INTO ONE WALK, and the folding is this function's only
 * judgement call. The excluded 0x0000 and the collision suffix are two
 * different reasons to move on to the next derivation, so the sequence is
 * "derivation 0 with no suffix, then suffix 0x00, 0x01, ... , with any
 * derivation that comes out 0x0000 dropped". A caller therefore never sees the
 * wildcard, never has to know why a candidate was skipped, and both ends skip
 * the same one - which is the property that matters, because a node and a
 * searcher that disagreed about whether a zero counts would disagree about
 * every candidate after it.
 *
 * Returns RADIANT_SEC_OK, RADIANT_SEC_ENOKEY when the channel holds no root, or
 * RADIANT_SEC_EINVAL for a null `out` or an `attempt` the bounded walk cannot
 * reach (which needs 256 consecutive zeros and is therefore a bug, not a
 * collision).
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
 *
 * Both take the covered bytes and answer with a tag, and both take the instant
 * rather than reading a clock, because the counter in the nonce is Tier I's
 * interval index and that is a function of time on both sides. There is no
 * counter field in either message: an announcement carries a locator and a
 * countdown and has no room for one, and adding a counter a sender chooses
 * would hand the replay defence to the sender. So the counter is DERIVED, both
 * ends compute it from the epoch anchor they already share, and a message
 * replayed into another interval fails the tag rather than being noticed
 * afterwards.
 *
 * The exactness that buys is worth stating, because it is also the cost: a
 * message whose transmission and reception straddle an interval boundary
 * verifies on one side of it and not the other. At the default T that is one
 * boundary every 20 s against an announcement repeated eight times over a
 * countdown, so it costs at most one copy - and the alternative, accepting a
 * neighbouring interval, would accept a replay for a whole interval after the
 * fact. Exact, and repeat.
 *
 * `len` is the covered length: 8 for an announcement (the whole transmitted
 * frame, byte [0] and byte [1] included, so nothing an attacker can rewrite is
 * outside the tag) and RADIANT_SEC_COMPAT_CMD_BYTES for a command.
 *
 * Returns RADIANT_SEC_OK; RADIANT_SEC_EBADMAC from the verify arms when the tag
 * does not match, which is the answer a caller acts on; RADIANT_SEC_ENOKEY with
 * no key or no epoch on the channel; RADIANT_SEC_EINVAL for a null argument or
 * a length that is not the covered length.
 */
int radiant_sec_compat_announce_tag(uint8_t ch, uint64_t now_us,
				    const uint8_t *msg, uint8_t len,
				    uint8_t out[RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES]);

int radiant_sec_compat_announce_verify(uint8_t ch, uint64_t t_sync,
				       const uint8_t *msg, uint8_t len,
				       const uint8_t *tag);

/*
 * The command arm, under K_cmd. THE NODE IS THE VERIFIER HERE, which is the
 * opposite direction from everything else in this file, and it is why the key
 * is different: a receiver that can verify the node's attestation must not
 * thereby be able to forge a command to it, and a passive observer who records
 * one must not be able to replay it into a later interval.
 *
 * What this does NOT do is decide anything. It answers "did the holder of
 * K_cmd send these bytes, in this interval". Whether the node then acts is
 * policy, it lives above this file, and on a `never` node the answer is no
 * however good the tag is.
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
 * This is the one entry point that may lower an epoch, and the asymmetry is the
 * point: D3's monotonicity rule binds the epoch AUTHORITY, which is the sensor.
 * A receiver is not the authority - it is reading the sensor's epoch off the
 * air - and a sensor that has been re-provisioned genuinely is at a lower one.
 * Refusing to follow it would leave the receiver permanently deaf to a strap
 * the user has just reset, which is not a security property, it is a support
 * call.
 *
 * Everything in flight belonged to the previous epoch, so the window state is
 * cleared and the Tier I replay high-water mark with it - a fresh epoch has its
 * own counter space and judging it against the old one's high-water mark would
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

#endif /* RADIANT_CORE_RADIANT_SEC_COMPAT_H_ */
