/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_freq.h - adaptive frequency: choosing a quieter RF index, and saying
 * so before moving to it.
 *
 * Provenance: docs/decisions/0005-extension-inside-ant-plus.md axis 5 (the
 * descriptor announces the node's RF channel, discovery stays on RF 57, and an
 * in-band "next epoch on RF N" lets a deployed pair move away from fresh
 * interference), docs/decisions/0012-adaptive-frequency.md, which builds it, and
 * docs/radiant-telemetry.md's page map, which allocates page 0x13. All of those
 * are this project's own documents. No ANT+ device profile
 * document was read for this file, no sdk-ant source was consulted, and nothing
 * here derives from libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * WHAT THIS IS FOR: the characterised ~0.4% loss floor on this bench is
 * memoryless per-slot collision at 2457 MHz inside Wi-Fi channel 11. Nothing
 * in the link layer can fix that; leaving 2457 MHz can, and this file decides
 * where to go and announces the departure. THE VALUE IS LEAVING CROWDED
 * AIRSPACE, NOT DODGING WITHIN IT - a node moves once, to a quiet index, and
 * stays. Frequency-diverse repetition and fast per-event hopping are both
 * declined; the latter would break the search sweep's "certain within one
 * sweep" guarantee every other extension axis is built around.
 *
 * THE CANDIDATE SET IS BLE'S: RF 2, 26, 80 (2402/2426/2480 MHz) are BLE's
 * advertising channels, sitting in the gaps between Wi-Fi 1/6/11. A node is
 * not restricted to those three - descriptor frame 0 byte [6] carries a full
 * 0..124 index - they are only the selector's default candidate list and a
 * lost receiver's first retry.
 *
 * DISCOVERY NEVER MOVES, and this file has no way to move it: radiant_search.c
 * always arms RADIANT_FRAME_CFG_SEARCH on RADIANT_RF_INDEX_ANT_PLUS (ADR
 * 0007), and nothing here touches it - a frequency axis would multiply the
 * sweep rather than lengthen it. Consequence: A NODE THAT HAS MOVED OFF RF 57
 * IS OUTSIDE THE SWEEP'S CERTAINTY, found instead by a descriptor heard
 * before the move, by the sync handoff (page 0x12), or by the bounded retry
 * list profile_freq_reacquire_order() publishes (at most five windows, not a
 * sweep) - the same shape ADR 0007 gives a long-range node one axis over.
 *
 * WHY A PAGE, NOT A DESCRIPTOR FRAME: profile_sched.c encodes the descriptor
 * set once at init and retransmits the bytes, so it can't carry a countdown
 * that must be recomputed every time. So the announcement is page 0x13, a
 * one-frame set claimed through profile_sched.h's client seam, present only
 * while a countdown runs. The descriptor says where the node IS; this page
 * says where it's GOING.
 *
 * THE COUNTDOWN ARITHMETIC is Layer C's (profile_private.h), reused rather
 * than re-derived:
 *
 *     c       = ceil( (move_at - msgs_after_this_one) / PROFILE_FREQ_UNIT )
 *     move_at = msgs_after_this_one + c * PROFILE_FREQ_UNIT
 *
 * The node moves its own target to what it just said, every time it says
 * anything, rounded up so a receiver holding an older copy retunes EARLY
 * rather than late.
 *
 * NOT BORROWED FROM LAYER C, each a decision: no tag/key/policy states (a
 * forged announcement costs one re-acquisition, same as a missed one, so
 * authentication buys nothing - recorded in ADR 0012); no beacon promotion
 * (one slot in PROFILE_FREQ_ANNOUNCE_EVERY, nothing else changes cadence);
 * no bounded duration or automatic return (a node goes back the same way it
 * left, by announcing it).
 *
 * A RECEIVER COUNTS SLOTS, NOT MESSAGES. The countdown is in transmitted
 * messages; a receiver counting only what it heard would fall behind by its
 * own loss rate. A tracked channel opens a window every period whether or
 * not a frame lands, so profile_freq_rx_slot() takes one call per SLOT
 * (message or NULL), matching the node's message count by construction.
 * SAME FACT BOUNDS WHERE THIS WORKS: a sparse node's message count and its
 * receiver's slot count are unrelated, so profile_freq_begin() REFUSES a
 * sparse node rather than announce a countdown neither end can agree on.
 */

#ifndef RADIANT_PROFILE_FREQ_H_
#define RADIANT_PROFILE_FREQ_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_radio_hal.h>

#include "profile_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * The candidate set
 * ---------------------------------------------------------------------------
 */

/* Where every ANT+ device is, and where a node is until it says otherwise.
 * Named from the HAL's constant, not spelled 57 again. */
#define PROFILE_FREQ_RF_HOME RADIANT_RF_INDEX_ANT_PLUS

#define PROFILE_FREQ_RF_LOW  2u  /* 2402 MHz - below Wi-Fi 1 */
#define PROFILE_FREQ_RF_MID  26u /* 2426 MHz - between Wi-Fi 1 and 6 */
#define PROFILE_FREQ_RF_HIGH 80u /* 2480 MHz - above Wi-Fi 11 */

#define PROFILE_FREQ_N_DEFAULTS 3u

/* The three defaults, ascending. A caller may pass its own list to every
 * function that takes one; this is what it gets by passing NULL. */
extern const uint8_t profile_freq_defaults[PROFILE_FREQ_N_DEFAULTS];

/* ---------------------------------------------------------------------------
 * Selection
 * ---------------------------------------------------------------------------
 */

/*
 * One index's evidence, in the channel-quality map's own vocabulary.
 *
 * `busy_dbm` is the map's max over per-dwell MEANS - a loud burst a 250 ms
 * period usually steps around vs. a dwell that averaged loud (a resident
 * transmitter). `floor_dbm` is the min over per-dwell minima and is the
 * tie-break, not the ranking.
 *
 * `have` false means NOT MEASURED, a different fact from measured-and-loud;
 * the selector treats it as such rather than as a bad score.
 */
struct profile_freq_evidence {
	uint8_t rf_index;
	bool    have;
	int8_t  busy_dbm;
	int8_t  floor_dbm;
};

/*
 * How much quieter a candidate must be before it's worth moving to, in dB.
 * A margin rather than "better", to avoid churn between two indices within a
 * decibel of each other. Six is chosen against the effect being chased - a
 * Wi-Fi carrier is tens of dB above the floor, not six - admitting the move
 * that motivated the feature while refusing measurement noise.
 */
#define PROFILE_FREQ_MARGIN_DB 6

/*
 * Choose where to go, from evidence.
 *
 * PURE, so it can serve both the "node-side if it can measure, receiver-side
 * if not" callers as one rule: the map is the argument, and this is the only
 * ranking function in the project, so two receivers given the same evidence
 * reach the same answer including tie-breaks.
 *
 * `ev` must contain an entry for `incumbent` with `have` set - a node may not
 * move on a quiet candidate alone; without a measurement of where it already
 * is there's no margin to clear.
 *
 * Returns 0 and writes *out_rf, or:
 *   -EINVAL    a null argument, or an rf_index above RADIANT_RF_INDEX_MAX
 *   -ENODATA   no evidence for the incumbent, or none for any candidate
 *   -EALREADY  nothing beats the incumbent by PROFILE_FREQ_MARGIN_DB. THE
 *              ORDINARY ANSWER, and not an error: a node on a quiet frequency
 *              stays there, and a caller that treated this as a failure would
 *              log one every time the feature worked.
 */
int profile_freq_select(uint8_t incumbent, const struct profile_freq_evidence *ev,
			uint8_t n, uint8_t *out_rf);

/*
 * The same, reading radiant_chanmap.c directly for the incumbent and each of
 * `cand` (or profile_freq_defaults when `cand` is NULL).
 *
 * WITH CONFIG_RADIANT_CORE_ED_SCAN OFF THIS ALWAYS RETURNS -ENODATA - the
 * map's accessor is a no-op inline, and a node that can't measure the band
 * has no business choosing a frequency for it. The "receiver-side if not"
 * case then measures itself and calls profile_freq_select() directly.
 */
int profile_freq_select_from_map(uint8_t incumbent, const uint8_t *cand,
				 uint8_t n_cand, uint8_t *out_rf);

/* ---------------------------------------------------------------------------
 * The announcement page - docs/radiant-telemetry.md's page map, 0x13
 *
 *   [0]    0x13
 *   [1]    0x00 = (0 << 4) | (1 - 1): a ONE-FRAME SET, on the framing convention
 *          every other RadiANT page uses. Byte [1] is a frame index here, which
 *          is the descriptor's documented exception to the counter invariant and
 *          is why this page may not carry a counter: it is a retained
 *          announcement, repeated identically-shaped every time, and a counter
 *          would make two copies of one announcement look like two
 *          announcements.
 *   [2]    target RF index, 0..124
 *   [3]    countdown, in units of PROFILE_FREQ_UNIT transmitted messages, 1..255
 *   [4..7] reserved, must be zero
 * ---------------------------------------------------------------------------
 */

#define PROFILE_FREQ_PAGE       0x13u
#define PROFILE_FREQ_FRAME_BYTE 0x00u
#define PROFILE_FREQ_LEN        8u

/* One unit of the countdown field, in transmitted messages, and the interval
 * between announcements - the same number by definition: a countdown
 * quantised finer than its announcements would name instants none can land
 * on, so there is one constant, not two. */
#define PROFILE_FREQ_UNIT           8u
#define PROFILE_FREQ_ANNOUNCE_EVERY PROFILE_FREQ_UNIT
#define PROFILE_FREQ_COUNTDOWN_MAX  0xFFu

/*
 * K, the countdown, in transmitted messages. 64 is ~16 s at 4 Hz and eight
 * announcements, so a receiver must lose eight consecutive copies (~10^-19 at
 * the ~0.4% loss floor) to miss the move and fall back to the retry list.
 *
 * Must be a multiple of PROFILE_FREQ_UNIT: a K the countdown field can't
 * express is a move landing nowhere either end named.
 */
#define PROFILE_FREQ_K_DEFAULT 64u
#define PROFILE_FREQ_K_MAX     ((uint16_t)(PROFILE_FREQ_COUNTDOWN_MAX * PROFILE_FREQ_UNIT))

/* Encode the announcement. Returns 0, or -EINVAL for an index above
 * RADIANT_RF_INDEX_MAX or a zero countdown. Bytes [4..7] are written as zero. */
int profile_freq_encode(uint8_t target, uint8_t countdown, uint8_t *body);

/*
 * Decode it. Returns 0, or -EINVAL for a null argument or a length that is not
 * eight, -ENOENT for a body that is not this page, or -EPROTO for a reserved
 * byte that is not zero, a countdown of zero, or an index out of range.
 *
 * -EPROTO ON A NONZERO RESERVED BYTE, which is the strict half of the
 * forward-compatibility rule and is right here: this page's bytes tell a
 * receiver where to point its radio, so a field it does not understand is not
 * something to ignore. The fail-open half belongs to descriptor flags that
 * describe a node; this is not one.
 */
int profile_freq_decode(const uint8_t *body, uint8_t len, uint8_t *target,
			uint8_t *countdown);

/* ---------------------------------------------------------------------------
 * The node's half
 * ---------------------------------------------------------------------------
 */

/*
 * How long between completed moves, in seconds. Minutes-scale is a structural
 * refusal, not advice - rechoosing faster than a receiver can follow is
 * accidental fast hopping. Five minutes is far longer than any interferer
 * takes to appear and far shorter than a session.
 *
 * Measured from the last COMPLETED move, so refused requests can't extend
 * the floor and a long countdown can't shorten it.
 */
#define PROFILE_FREQ_MIN_MOVE_S 300u

struct profile_freq_cfg {
	/* K in transmitted messages; 0 means PROFILE_FREQ_K_DEFAULT. Must be a
	 * nonzero multiple of PROFILE_FREQ_UNIT, not above PROFILE_FREQ_K_MAX. */
	uint16_t k;

	/* 0 means PROFILE_FREQ_MIN_MOVE_S. There is no value meaning "no limit",
	 * which is the same refusal the private-mode window makes. */
	uint32_t min_move_s;

	/* True for a node that does not transmit in every slot. It is taken from
	 * the caller rather than from a descriptor pointer so that this file
	 * needs no descriptor at all to refuse the case it cannot serve. */
	bool sparse;
};

struct profile_freq {
	struct profile_freq_cfg cfg;
	bool                    armed;

	uint8_t rf_index; /* where the node transmits now */
	uint8_t target;   /* where it is going; meaningless unless announcing */
	bool    announcing;

	/* Transmitted messages, and the message index at which the move happens.
	 * Both are counts of TRANSMITTED MESSAGES, which is the unit the
	 * countdown is expressed in. */
	uint32_t msgs;
	uint32_t move_at;

	uint64_t now_us;
	uint64_t last_move_us;
	bool     ever_moved;

	/* Counters, and every refusal has one: a node that declined to move and
	 * said nothing is indistinguishable from one that never had a reason. */
	uint32_t moves;
	uint32_t announcements;
	uint32_t refused_rate;
	uint32_t refused_busy;
	uint32_t refused_sparse;
	uint32_t refused_range;
};

/*
 * Latch the configuration. The node always starts on PROFILE_FREQ_RF_HOME -
 * no way to configure another index at init, since a node that BOOTED off 57
 * would be one no receiver was ever told about (same rule as ADR 0007's
 * private-mode switch landing on RF 57).
 *
 * Returns 0, -EINVAL for a null argument or a K that is not a nonzero multiple
 * of PROFILE_FREQ_UNIT within PROFILE_FREQ_K_MAX.
 */
int profile_freq_init(struct profile_freq *pf, const struct profile_freq_cfg *cfg);

uint8_t profile_freq_rf_index(const struct profile_freq *pf);
bool    profile_freq_off_home(const struct profile_freq *pf);
bool    profile_freq_announcing(const struct profile_freq *pf);
/* Where the node is going, or its current index when nothing is pending. */
uint8_t profile_freq_target(const struct profile_freq *pf);

/*
 * Begin a move, starting the countdown at K.
 *
 * Returns 0, or:
 *   -EINVAL    not initialised, or a target above RADIANT_RF_INDEX_MAX (counted)
 *   -EALREADY  the target is where the node already is (counted as neither a
 *              refusal nor a move; it is a caller asking for nothing)
 *   -ENOTSUP   a sparse node (counted). See the header: its message count and a
 *              receiver's slot count are unrelated, so no countdown either end
 *              could agree on exists
 *   -EBUSY     a countdown is already running (counted)
 *   -EAGAIN    rate-limited (counted)
 */
int profile_freq_begin(struct profile_freq *pf, uint8_t target, uint64_t now_us);

/*
 * EVERY transmitted message, including ones this module didn't build - it's
 * the countdown's clock; a module counting only its own frames would count
 * eight and call it sixty-four.
 *
 * Returns true when this call performed the move - the moment the caller must
 * re-encode its descriptor (profile_freq_apply()) and retune its transmit
 * slot. The move happens on the message the announcement named, never on
 * receipt of anything.
 *
 * `now_us` is what the rate limit is measured from, taken here rather than
 * remembered from profile_freq_begin() so the floor sits between COMPLETED
 * moves - measuring from the request would let a long countdown buy the next
 * move sooner.
 */
bool profile_freq_sent(struct profile_freq *pf, uint64_t now_us);

/*
 * The slot-insertion seam, with the signature of struct profile_sched_client's
 * claim(). Claims one slot in PROFILE_FREQ_ANNOUNCE_EVERY while a countdown is
 * running and none at any other time, so a node that never moves puts nothing
 * extra on the air - byte for byte the node it was before this file existed.
 *
 * `user` is the struct profile_freq. `m` is ignored: the cadence is counted in
 * transmitted messages, which is the unit the countdown is in, and the engine's
 * message index within a 121-cycle is not.
 */
bool profile_freq_claim(uint32_t m, uint8_t *body, void *user);

/*
 * Write an RF index and the OFF_RF57 flag into a descriptor so the node's two
 * statements about its own frequency agree. A one-line function that exists
 * because profile_desc_encode() refuses a descriptor whose flag and index
 * disagree - this is the one way to change a descriptor's frequency.
 *
 * Returns 0, or -EINVAL for a null descriptor or an index out of range.
 */
int profile_freq_apply(struct profile_descriptor *d, uint8_t rf_index);

/* ---------------------------------------------------------------------------
 * The receiver's half
 * ---------------------------------------------------------------------------
 * Here rather than in a test, since a rule written twice is a rule two
 * implementations will one day disagree about, silently - a node and a
 * receiver computing different move messages don't fail, they stop meeting.
 */

struct profile_freq_rx {
	/* Slots elapsed on this channel, whether or not a message arrived in
	 * them. See the header: this is what tracks the node's transmitted
	 * message count across loss. */
	uint32_t slots;

	bool     armed;
	uint32_t move_at;
	uint8_t  target;

	/* The last target this receiver was ever told about, kept after the move
	 * for profile_freq_reacquire_order(). */
	uint8_t  last_target;
	bool     have_last;

	uint32_t heard;    /* announcements taken */
	uint32_t rejected; /* page 0x13 bodies that would not decode */
};

void profile_freq_rx_init(struct profile_freq_rx *rx);

/*
 * One channel period elapsed. `body` is the message heard in it, or NULL when
 * nothing arrived; `len` is ignored when `body` is NULL.
 *
 * ONE CALL PER SLOT is the whole contract - skipping empty slots makes the
 * countdown run at the caller's own loss rate.
 *
 * Returns 1 when an announcement was just taken and this receiver is now armed,
 * 0 for anything else including a message on some other page, or -EPROTO for a
 * page 0x13 body that would not decode (counted, and otherwise ignored - the
 * cost of ignoring it is one re-acquisition, and that path is built).
 */
int profile_freq_rx_slot(struct profile_freq_rx *rx, const uint8_t *body,
			 uint8_t len);

/* True once the countdown this receiver heard has expired: RETUNE NOW. Every
 * receiver that heard the same announcement reaches this on the same slot, which
 * is the property the countdown exists for. */
bool profile_freq_rx_due(const struct profile_freq_rx *rx);

/* The announced target. -ENOENT when nothing is armed. */
int profile_freq_rx_target(const struct profile_freq_rx *rx, uint8_t *rf_index);

/* Done retuning; forget the announcement, keep the target for the retry list. */
void profile_freq_rx_clear(struct profile_freq_rx *rx);

/*
 * THE PATH THAT NEEDS NO ANNOUNCEMENT: where a receiver that lost this node
 * should point its radio, in order, deduplicated.
 *
 *   1. the last target it was told about, if any;
 *   2. the three defaults, ascending;
 *   3. PROFILE_FREQ_RF_HOME last.
 *
 * At most PROFILE_FREQ_REACQUIRE_MAX entries. NOT A SWEEP: a bounded retry
 * over named indices that does not touch the search sweep's set count or its
 * "certain within one sweep" guarantee on RF 57.
 */
#define PROFILE_FREQ_REACQUIRE_MAX (PROFILE_FREQ_N_DEFAULTS + 2u)

uint8_t profile_freq_reacquire_order(const struct profile_freq_rx *rx,
				     uint8_t *out, uint8_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_FREQ_H_ */
