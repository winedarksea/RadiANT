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
 * are this project's own documents. No adopter-gated ANT+ device profile
 * document was read for this file, no sdk-ant source was consulted, and nothing
 * here derives from libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is for
 * ---------------------------------------------------------------------------
 * The characterised ~0.4 % loss floor on this bench is memoryless per-slot
 * collision at 2457 MHz, inside Wi-Fi channel 11. Nothing in the link layer can
 * fix that. Leaving 2457 MHz can, and this is the file that decides where to go
 * and announces the departure.
 *
 * THE VALUE IS LEAVING CROWDED AIRSPACE, NOT DODGING WITHIN IT. A node moves
 * once, to a quiet index, and stays. Blind frequency-diverse repetition and
 * fast per-event hopping are both declined and neither is deferred - the RF plan
 * records the arithmetic for both, and the second one would break the search
 * sweep's "certain within one sweep" guarantee, which is the property every
 * other extension axis has been built around.
 *
 * ---------------------------------------------------------------------------
 * The candidate set is BLE's, and that is an argument rather than a preference
 * ---------------------------------------------------------------------------
 * RF 2, 26 and 80 - 2402, 2426 and 2480 MHz - are the BLE advertising channels.
 * They sit in the gaps between the three non-overlapping Wi-Fi channels (1, 6
 * and 11), which is where a decade of shipped Bluetooth put them after the same
 * survey this project would otherwise have to repeat with worse instruments.
 *
 * A NODE IS NOT RESTRICTED TO THOSE THREE. Descriptor frame 0 byte [6] carries a
 * full 0..124 index and this file will announce any of them. The three are
 * defaults in two places: they are what the selector considers when a caller
 * does not supply its own list, and they are what a receiver that has lost an
 * off-57 node tries first. Nothing refuses an index outside them.
 *
 * ---------------------------------------------------------------------------
 * DISCOVERY NEVER MOVES, and this file has no way to move it
 * ---------------------------------------------------------------------------
 * radiant_search.c arms RADIANT_FRAME_CFG_SEARCH on cfg.rf_index, whose default
 * is RADIANT_RF_INDEX_ANT_PLUS, and ADR 0007 already made that a rule rather
 * than a default. Nothing here touches it: there is no call from this file into
 * radiant_search.c, no candidate list a sweep consults, and no configuration
 * that would widen one. The sweep covers all 256 values of devnum_lo and is
 * certain to find any transmitting device within one sweep; a frequency axis
 * would MULTIPLY that sweep rather than lengthen it, which is exactly the trade
 * ADR 0007 refused for a second PHY.
 *
 * What follows from that, stated plainly because it is the cost rather than the
 * feature: A NODE THAT HAS MOVED OFF RF 57 IS OUTSIDE THE SWEEP'S CERTAINTY.
 * It is found by the descriptor a receiver heard before the move, by the sync
 * handoff of page 0x12, or by the bounded retry list this file publishes -
 * profile_freq_reacquire_order(), which is at most five windows and is not a
 * sweep. That is the same shape of answer ADR 0007 gives for a long-range node
 * ("found by its 1 M descriptor or by the sync handoff, never by sweeping the
 * coded PHY"), one axis over, and it is why the retry list exists at all.
 *
 * ---------------------------------------------------------------------------
 * Why the announcement is a page and not a descriptor frame
 * ---------------------------------------------------------------------------
 * The descriptor is where the RF index lives, so a frame in the descriptor set
 * looks like the obvious home for a pending change to it. It is not, and the
 * reason is already written down: profile_schedule.h records that profile_sched.c
 * encodes the descriptor set ONCE at init and retransmits the bytes, which is
 * why RF-5a could put a downlink window on the wire but could not announce a
 * live one. A countdown has the same shape as that phase - it has to be
 * recomputed every time the frame goes out - so it hits the same wall for the
 * same reason.
 *
 * So the announcement is page 0x13, a one-frame set, claimed through
 * profile_sched.h's client seam like any other inserted page, and present in the
 * rotation only while a countdown is running. The descriptor keeps saying where
 * the node IS; this page says where it is GOING. When the countdown expires the
 * node re-encodes its descriptor with the new index and the page goes away.
 *
 * ---------------------------------------------------------------------------
 * The countdown's arithmetic, which is Layer C's and deliberately only that
 * ---------------------------------------------------------------------------
 * The private-mode switch (profile_private.h) solved the identical problem -
 * announce a move to N listeners so that all of them act on the same message -
 * and its arithmetic is reused here rather than re-derived:
 *
 *     c       = ceil( (move_at - msgs_after_this_one) / PROFILE_FREQ_UNIT )
 *     move_at = msgs_after_this_one + c * PROFILE_FREQ_UNIT
 *
 * The node MOVES ITS OWN TARGET TO WHAT IT JUST SAID, every time it says
 * anything. Rounded up, so a receiver holding an older copy retunes EARLY and
 * loses the tail of the old channel rather than the head of the new one, and so
 * that repeated announcements cannot walk the target closer one rounding at a
 * time.
 *
 * WHAT IS NOT BORROWED, and each absence is a decision:
 *
 *   - NO TAG, NO KEY, NO POLICY STATES. Layer C's announcement must be
 *     authenticated because it takes the node off the air entirely for every
 *     receiver that misses it: an unauthenticated "everybody follow me" is a
 *     one-packet herding attack strictly worse than muting. Here the recovery
 *     path is the retry list and then the sweep on RF 57, which never moves and
 *     needs no key - so a forged announcement costs a receiver one
 *     re-acquisition, which is exactly what a MISSED announcement already costs.
 *     A defence is worth its complexity when it buys more than the failure it
 *     prevents; this one would not. Recorded in ADR 0012 rather than left
 *     implicit, because "we did not authenticate it" should be a finding in a
 *     document rather than a discovery in a review.
 *   - NO BEACON PROMOTION. The announcement claims one slot in
 *     PROFILE_FREQ_ANNOUNCE_EVERY from the ordinary data rotation; nothing else
 *     changes cadence.
 *   - NO BOUNDED DURATION AND NO RETURN. A node that moved to a quiet index has
 *     no reason to go back on a timer. It goes back the same way it left, by
 *     announcing it, if the map ever says 57 is the better place.
 *
 * ---------------------------------------------------------------------------
 * A RECEIVER COUNTS SLOTS, NOT MESSAGES, and that is not a detail
 * ---------------------------------------------------------------------------
 * The countdown is in TRANSMITTED MESSAGES. A receiver that counted only the
 * messages it actually heard would fall behind the node by exactly its loss
 * rate, which on this bench is the ~0.4 % the whole feature exists to remove -
 * so at K = 64 it would retune a fraction of a message late, and at a longer
 * countdown on a worse link, later still. A tracked channel opens a window every
 * period whether or not a frame lands in it, so the receiver has a slot count
 * that matches the node's message count by construction. profile_freq_rx_slot()
 * takes one call per SLOT, carrying the message if one arrived and NULL if it
 * did not, so there is no ordering rule between two functions for a caller to
 * get wrong.
 *
 * THE SAME FACT BOUNDS WHERE THIS WORKS. A sparse node does not transmit in
 * every slot, so its message count and its receiver's slot count are unrelated,
 * and profile_freq_begin() REFUSES a sparse node rather than announcing a
 * countdown neither end can agree on. A sparse node's answer to a noisy
 * frequency is the same as its answer to everything else - it is on the air for
 * microseconds a minute - and it is not this file's.
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

/* Where every ANT+ device is, and where a node is until it says otherwise. Named
 * from the HAL's constant rather than spelled 57 again - there is one definition
 * of "the ANT+ frequency" in this project and it is not in a profile. */
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
 * `busy_dbm` is the map's maximum over per-dwell MEANS, which is deliberately
 * the statistic a frequency choice turns on: a single loud sample is a burst a
 * 250 ms period usually steps around, while a whole dwell that averaged loud is
 * a transmitter that lives there. `floor_dbm` is the minimum over per-dwell
 * minima and is the tie-break, not the ranking - ranking on the floor would
 * choose the index with the quietest single microsecond.
 *
 * `have` false means NOT MEASURED, which is a different fact from measured and
 * loud, and the selector treats it as such rather than as a bad score.
 */
struct profile_freq_evidence {
	uint8_t rf_index;
	bool    have;
	int8_t  busy_dbm;
	int8_t  floor_dbm;
};

/*
 * How much quieter a candidate must be before it is worth moving to, in dB.
 *
 * A margin rather than "better", because the alternative is churn: two indices
 * within a decibel of each other trade places as the room changes, and a node
 * that re-announced every time would be hopping slowly rather than adapting.
 * Six is chosen against the effect being chased - a Wi-Fi carrier sitting on
 * 2457 MHz is tens of dB above the floor, not six - so this admits the move that
 * motivated the feature and refuses the ones that are noise in the measurement.
 */
#define PROFILE_FREQ_MARGIN_DB 6

/*
 * Choose where to go, from evidence.
 *
 * PURE, AND THAT IS WHAT MAKES IT SERVE BOTH SIDES. The RF plan asks for
 * selection "node-side if it can measure, receiver-side if not"; those are two
 * callers with two maps and they must not be two rules. So the map is the
 * argument and this is the only ranking function in the project. Two receivers
 * given the same evidence reach the same answer, including the tie-breaks, which
 * matters the moment more than one of them can ask a node to move.
 *
 * `ev` must contain an entry for `incumbent` with `have` set. A node may not
 * move on the strength of a quiet candidate alone: without a measurement of
 * where it already is there is no margin to clear, and "somewhere else is quiet"
 * is not evidence that here is not.
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
 * The same, reading radiant_chanmap.c directly for the incumbent and for each of
 * `cand` (or profile_freq_defaults when `cand` is NULL).
 *
 * WITH CONFIG_RADIANT_CORE_ED_SCAN OFF THIS ALWAYS RETURNS -ENODATA, because the
 * map's accessor compiles to a no-op inline that reports nothing, and a node with
 * no way to measure the band has no business choosing a frequency for one. That
 * is the "receiver-side if not" case arriving at the right answer by
 * construction: the receiver measures, and calls profile_freq_select() with its
 * own evidence.
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

/*
 * One unit of the countdown field, in transmitted messages, and the interval
 * between announcements. They are the same number BY DEFINITION rather than by
 * coincidence - a countdown quantised more finely than the announcements that
 * carry it would name instants no announcement can land on - which is why there
 * is one constant and not two.
 */
#define PROFILE_FREQ_UNIT           8u
#define PROFILE_FREQ_ANNOUNCE_EVERY PROFILE_FREQ_UNIT
#define PROFILE_FREQ_COUNTDOWN_MAX  0xFFu

/*
 * K, the countdown, in transmitted messages. 64 is ~16 s at 4 Hz and eight
 * announcements, so a receiver has to lose eight consecutive copies to miss the
 * move - at the characterised ~0.4 % floor that is about 10^-19, and the
 * receiver that manages it falls back on the retry list, which depends on
 * hearing nothing.
 *
 * A multiple of PROFILE_FREQ_UNIT, refused otherwise: a K the countdown field
 * cannot express is a move that lands somewhere neither end named.
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
 * How long between completed moves, in seconds.
 *
 * MINUTES-SCALE IS A STRUCTURAL REFUSAL AND NOT ADVICE. Rechoosing faster than a
 * receiver can follow is fast hopping arrived at by accident, and it is declined
 * with arithmetic elsewhere rather than left to a caller's judgement here. Five
 * minutes is far longer than any interferer this is chasing takes to appear and
 * far shorter than a session.
 *
 * Measured from the last COMPLETED move, so a stream of refused requests cannot
 * extend the floor and a long countdown does not shorten it.
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
 * Latch the configuration. The node starts on PROFILE_FREQ_RF_HOME, always, and
 * there is no way to configure it onto another index at init: ADR 0007's rule
 * that a private-mode switch lands on 1 M / RF 57 and moves afterwards through
 * the ordinary mechanism is the same rule one axis over, and a node that BOOTED
 * off 57 would be a node no receiver had ever been told about.
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
 * EVERY transmitted message, including the ones this module did not build - it
 * is the countdown's clock, and a module told only about its own frames would
 * count eight and call it sixty-four.
 *
 * Returns true when this call performed the move, which is the moment the caller
 * must re-encode its descriptor (profile_freq_apply()) and retune its transmit
 * slot. The move happens on the message the announcement named, and never on
 * receipt of anything.
 *
 * `now_us` is what the rate limit is measured from, and it is taken here rather
 * than remembered from profile_freq_begin() so that the floor sits between
 * COMPLETED moves. Measuring from the request would let a long countdown buy the
 * next move sooner, which is backwards: the countdown is time a receiver already
 * spent following this one.
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
 * Write an RF index and the OFF_RF57 flag into a descriptor so that the node's
 * two statements about its own frequency agree.
 *
 * A ONE-LINE FUNCTION THAT EXISTS BECAUSE THE RULE IS ENFORCED SOMEWHERE ELSE.
 * profile_desc_encode() refuses a descriptor whose flag and index disagree, and
 * a caller that set one and forgot the other would get -EINVAL from a function
 * that is not about frequency at all. There is one way to change a descriptor's
 * frequency and this is it.
 *
 * Returns 0, or -EINVAL for a null descriptor or an index out of range.
 */
int profile_freq_apply(struct profile_descriptor *d, uint8_t rf_index);

/* ---------------------------------------------------------------------------
 * The receiver's half
 * ---------------------------------------------------------------------------
 *
 * Here rather than in a test, for the reason the private-mode receiver is: a
 * rule written twice is a rule two implementations will one day disagree about,
 * and this disagreement is silent - a node and a receiver that compute different
 * move messages do not fail, they stop meeting.
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
 * ONE CALL PER SLOT, WHICH IS THE WHOLE OF THE CONTRACT. A caller that skips
 * empty slots is a caller whose countdown runs at its own loss rate.
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
 *   1. the last target it was told about, if any - which is where the node is if
 *      the receiver simply stopped listening rather than missing a move;
 *   2. the three defaults, ascending, because a node that chose without telling
 *      this receiver chose from that list;
 *   3. PROFILE_FREQ_RF_HOME last, because a node that reverted is back where
 *      every other device already is and the ordinary sweep finds it there.
 *
 * At most PROFILE_FREQ_REACQUIRE_MAX entries. THIS IS NOT A SWEEP AND MUST NOT
 * BECOME ONE: it is a bounded retry over named indices, it does not multiply the
 * search sweep's set count, and "certain within one sweep" is a statement about
 * RF 57 that this list does not touch.
 */
#define PROFILE_FREQ_REACQUIRE_MAX (PROFILE_FREQ_N_DEFAULTS + 2u)

uint8_t profile_freq_reacquire_order(const struct profile_freq_rx *rx,
				     uint8_t *out, uint8_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_FREQ_H_ */
