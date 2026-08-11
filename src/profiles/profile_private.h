/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_private.h - Layer C: the private-mode switch.
 *
 * Provenance: docs/radiant-security.md section 11.5 (normative for the
 * announcement's bytes, the countdown, the derived locator and the two
 * `announce` modes), section 11.6 (the four policy states, the precedence rule,
 * the key dependency and its one exemption) and
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md, which pins
 * all of it. All of those are this project's own documents. No adopter-gated
 * ANT+ device profile document was read for this file, no sdk-ant source was
 * consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is, in one sentence
 * ---------------------------------------------------------------------------
 * The node STOPS BEING AN ANT+ SENSOR and becomes a RadiANT one - and it says
 * so first, in the clear, to everybody, on a countdown.
 *
 * It is not an extra channel and it is not a mode flip on the same one. The
 * compat channel CLOSES and a device type 0x60 channel opens; keeping 0x0B
 * alive while emitting only unknown page numbers leaves a head unit showing a
 * connected sensor with no data, which is a failure mode users report as a bug,
 * where a close looks like walking out of range, which users understand.
 *
 * While private, a Garmin head unit and Zwift see NOTHING AT ALL. That is the
 * price of confidentiality and there is no version of it that is cheaper.
 *
 * ---------------------------------------------------------------------------
 * The announcement is broadcast, or 1:N is broken
 * ---------------------------------------------------------------------------
 * The command comes from ONE keyholder; the stream has N listeners. A node that
 * acts on it and vanishes gives the receiver that asked a clean handover and
 * every other keyed receiver a dropout - a feature that works on the bench with
 * one head unit and fails in the room. So the switch is a broadcast state
 * change with a countdown, and receivers act on COUNTDOWN EXPIRY rather than on
 * receipt, which is what makes every keyed receiver retune on the same message
 * instead of at N independent moments.
 *
 * Five rules from section 11.5, and this file is each of them:
 *
 *   1. CLEAR BUT SELF-AUTHENTICATING. Tier I covers no payload and Tier II is
 *      off by default, so the announcement cannot lean on either. Frame B tags
 *      frame A's full eight TRANSMITTED bytes under K_auth with Tier I's
 *      counter in the nonce, so it verifies on receipt, needs no window to
 *      close, survives the loss of every other packet, and a replay into
 *      another interval fails the tag. A receiver acts only after the tag
 *      verifies.
 *   2. THEREFORE PRIVATE MODE REQUIRES A KEY. Without a tag the announcement is
 *      an unauthenticated "everybody follow me to channel X" - a one-packet
 *      herding attack strictly worse than the mute attack this design already
 *      refuses. The dependency and its one exemption are below.
 *   3. THE BEACON RATE RISES TO MEET THE COUNTDOWN. profile_compat.c already
 *      promotes the beacon to one frame in eight while a client contributes
 *      frames, which is this client, so the promotion is inherited rather than
 *      reimplemented.
 *   4. RECEIVERS ACT ON EXPIRY. A receiver joining mid-countdown reads the
 *      remaining count out of the frame.
 *   5. THE RETURN IS ANNOUNCED THE SAME WAY. The revert paths that cannot
 *      announce - a power cycle, a crash - are exactly the ones where the
 *      receiver searches anyway, and the compat channel is what it finds.
 *
 * ---------------------------------------------------------------------------
 * THE COUNTDOWN'S ARITHMETIC, which is where this is easiest to get wrong
 * ---------------------------------------------------------------------------
 * Frame A's byte [7] carries six bits of countdown in units of eight
 * transmitted messages, because six bits of MESSAGES would reach 63 and the
 * longest legal countdown is K = 128. Multiply by eight to get messages.
 *
 * A quantised countdown can only name an exact instant if the emissions land on
 * the quantum, and they cannot: the scheduler never offers messages 119 and 120
 * of its 121-message cycle, so the promoted slots are not eight messages apart
 * across a cycle boundary. THE ANSWER IS TO RE-ANCHOR RATHER THAN TO ROUND.
 * Every time frame A goes out the node recomputes the count it can honestly
 * name and MOVES ITS OWN TARGET TO WHAT IT JUST SAID:
 *
 *     c        = ceil( (switch_at - messages_sent_after_this_one) / 8 )
 *     switch_at = messages_sent_after_this_one + c * 8
 *
 * so the node's target is always exactly what its most recent announcement
 * claimed. Every receiver that heard that announcement computes the identical
 * message, and one that heard only an earlier copy is at most seven messages
 * out - retuning EARLY, so it arrives before the node and loses the tail of the
 * old channel, rather than late and losing the head of the new one.
 *
 * Rounded UP for that reason and for one more: rounding down would shorten the
 * countdown by up to seven messages every time an announcement landed off the
 * quantum, so three announcements would turn a K of 64 into 43. Leaving the
 * target fixed and rounding either way is the option that does not work at all
 * - it puts different receivers on different messages, which is the exact
 * failure the countdown exists to prevent.
 *
 * K = 64 is ~16 s at 4 Hz and eight promoted beacon slots. The four-frame set
 * rotates through those slots one frame at a time, so a countdown carries TWO
 * complete announcement pairs at K = 64 and four at K = 128 - not eight, which
 * is the number of promoted slots rather than the number of announcements. Two
 * pairs at the characterised ~0.4% loss floor is not a case worth designing
 * around, and the receiver that loses both falls back on the derived locator,
 * which is built, tested and does not depend on hearing anything.
 *
 * ---------------------------------------------------------------------------
 * `silent` is a mode, not a failure
 * ---------------------------------------------------------------------------
 * With announce = silent the node emits no announcement frame, publishes no
 * locator, never sets the pending-switch bit and never promotes the beacon: the
 * channel simply closes. Keyholders rederive the locator and re-acquire.
 *
 * `silent` buys unlinkability and pays in availability; `broadcast` is the
 * reverse. NEITHER IS "MORE SECURE", and the measured gap between them belongs
 * in the docs rather than the phrase "a short search" -
 * radiant_core/tests/src/test_profile_private.c measures both and prints them.
 *
 * ---------------------------------------------------------------------------
 * Policy is configuration, and it is never over the air
 * ---------------------------------------------------------------------------
 * A `never` node that a stranger - or a paired keyholder - can talk into
 * becoming a `command` node has no policy, it has a suggestion. There is no
 * over-air policy operation in this file: not deferred, REFUSED. The only ops
 * an inbound command carries are "go private" and "come back", and an
 * unrecognised one is counted and dropped rather than being reserved for a
 * later meaning.
 *
 * Three surfaces, ONE precedence rule, stated once because three sources with
 * an unstated precedence is a bug report waiting six months:
 *
 *     NVM IF PROVISIONED, ELSE KCONFIG; THE HOST MESSAGE WRITES NVM
 *
 * rather than shadowing it. profile_private_host_set_policy() therefore fails
 * if the write fails, and the resolved policy does not move - a host that
 * changed the running value and lost the record would leave a node whose policy
 * depended on when it was last rebooted.
 */

#ifndef RADIANT_PROFILE_PRIVATE_H_
#define RADIANT_PROFILE_PRIVATE_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * The four policy states - docs/radiant-security.md section 11.6
 * ---------------------------------------------------------------------------
 * The same values the beacon carries, and deliberately the same names: the
 * policy in frame 0 byte [3] bits 1..0 IS this policy, and a translation table
 * between two spellings of one field is a translation table that will one day
 * be wrong in one direction.
 */
#define PROFILE_PRIVATE_NEVER    PROFILE_COMPAT_POLICY_NEVER
#define PROFILE_PRIVATE_PHYSICAL PROFILE_COMPAT_POLICY_PHYSICAL
#define PROFILE_PRIVATE_COMMAND  PROFILE_COMPAT_POLICY_COMMAND
#define PROFILE_PRIVATE_ALWAYS   PROFILE_COMPAT_POLICY_ALWAYS

/* `announce`, section 11.6's table. */
#define PROFILE_PRIVATE_BROADCAST 0u
#define PROFILE_PRIVATE_SILENT    1u

/*
 * The compile-time default, from Kconfig, falling back to the table's default
 * when the symbols are absent - which is what the unit-test application sees.
 *
 * NEVER, and it is the setting a shipped heart-rate strap should ship in: a
 * byte-exact ANT+ sensor for its whole life, which refuses an authenticated
 * switch command from a keyholder it trusts and counts the refusal.
 */
#if defined(CONFIG_RADIANT_PRIVATE_POLICY_PHYSICAL)
#define PROFILE_PRIVATE_POLICY_DEFAULT PROFILE_PRIVATE_PHYSICAL
#elif defined(CONFIG_RADIANT_PRIVATE_POLICY_COMMAND)
#define PROFILE_PRIVATE_POLICY_DEFAULT PROFILE_PRIVATE_COMMAND
#elif defined(CONFIG_RADIANT_PRIVATE_POLICY_ALWAYS)
#define PROFILE_PRIVATE_POLICY_DEFAULT PROFILE_PRIVATE_ALWAYS
#else
#define PROFILE_PRIVATE_POLICY_DEFAULT PROFILE_PRIVATE_NEVER
#endif

#if defined(CONFIG_RADIANT_PRIVATE_ANNOUNCE_SILENT)
#define PROFILE_PRIVATE_ANNOUNCE_DEFAULT PROFILE_PRIVATE_SILENT
#else
#define PROFILE_PRIVATE_ANNOUNCE_DEFAULT PROFILE_PRIVATE_BROADCAST
#endif

/*
 * THE KEY DEPENDENCY AND ITS ONE EXEMPTION, as a predicate rather than as a
 * paragraph, because it has to hold in three places at once: a BUILD_ASSERT in
 * profile_private.c, the runtime refusal in profile_private_init(), and the
 * refusal of a host message that would write an unrunnable policy into NVM.
 * One expression, three call sites, and a test that walks the whole matrix -
 * which is how a compile-time rule gets tested at all.
 *
 * `private_policy != never` requires K_auth and Tier I, because a `command`
 * trigger needs an authenticated command and a `broadcast` announcement needs
 * its frame-B tag. `physical` + `silent` is the exemption: with no announcement
 * there is nothing to forge and no over-air trigger to authenticate, so a
 * button-only, manually-provisioned node whose only on-air security surface is
 * the ciphertext itself is permitted with both tiers off.
 */
#define PROFILE_PRIVATE_COMBINATION_OK(policy, announce, attest)               \
	((policy) == PROFILE_PRIVATE_NEVER || (attest) ||                      \
	 ((policy) == PROFILE_PRIVATE_PHYSICAL &&                              \
	  (announce) == PROFILE_PRIVATE_SILENT))

/* ---------------------------------------------------------------------------
 * The countdown
 * ---------------------------------------------------------------------------
 */

/* K in {16, 32, 64, 128}, default 64 (~16 s at 4 Hz). Enumerated rather than
 * ranged for the reason N is: both ends have to agree on how long a countdown
 * can be, and a value neither the beacon's six bits nor the receiver's
 * arithmetic can express is a handover that silently misses. */
#define PROFILE_PRIVATE_K_MIN     16u
#define PROFILE_PRIVATE_K_MAX     128u
#define PROFILE_PRIVATE_K_DEFAULT 64u

#if defined(CONFIG_RADIANT_PRIVATE_COUNTDOWN)
#define PROFILE_PRIVATE_K_CONFIGURED ((uint16_t)CONFIG_RADIANT_PRIVATE_COUNTDOWN)
#else
#define PROFILE_PRIVATE_K_CONFIGURED ((uint16_t)PROFILE_PRIVATE_K_DEFAULT)
#endif

/* One unit of the countdown field, in transmitted messages. It is the promoted
 * beacon interval and it is that number BY DEFINITION rather than by
 * coincidence, which is why it is defined from it. */
#define PROFILE_PRIVATE_COUNTDOWN_UNIT PROFILE_COMPAT_BEACON_PROMOTED_EVERY
#define PROFILE_PRIVATE_COUNTDOWN_MAX  0x3Fu

/* Frame A byte [7] bits 7:6. `timeout-revert` is why the reason is on the air
 * at all: a receiver that sees the node coming back because its bounded
 * duration expired can say so, where one that only saw "returning" would have
 * to guess whether the user had asked. */
#define PROFILE_PRIVATE_REASON_COMMAND  0u
#define PROFILE_PRIVATE_REASON_PHYSICAL 1u
#define PROFILE_PRIVATE_REASON_TIMEOUT  2u

/* The announcement's two frames, and their payload width: bytes [2..7], because
 * profile_compat.c owns byte [0] and byte [1]. */
#define PROFILE_PRIVATE_FRAMES        2u
#define PROFILE_PRIVATE_FRAME_PAYLOAD 6u
#define PROFILE_PRIVATE_FRAME_A       0u
#define PROFILE_PRIVATE_FRAME_B       1u

/*
 * The frame indices the announcement occupies IN THE BEACON PAGE'S SET, which
 * a receiver matches on: the set grows to four for the duration of a countdown
 * and the announcement is frames 2 and 3 of it. Section 11.5 pins them, and
 * pinning them is what lets a receiver tell an announcement from an enrolment
 * window's frames without either module knowing about the other.
 */
#define PROFILE_PRIVATE_SET_FRAMES  4u
#define PROFILE_PRIVATE_SET_INDEX_A 2u
#define PROFILE_PRIVATE_SET_INDEX_B 3u

/* ---------------------------------------------------------------------------
 * The bounded duration, and the command's rate limit
 * ---------------------------------------------------------------------------
 */

/*
 * A node-configured maximum private duration, because A SENSOR THAT SILENTLY
 * STAYS PRIVATE IS INDISTINGUISHABLE FROM A DEAD ONE. One hour by default: long
 * enough for the ride the feature exists for, short enough that a node whose
 * owner walked away is an ANT+ sensor again before they wonder whether it is
 * broken.
 */
#define PROFILE_PRIVATE_MAX_S_DEFAULT 3600u

#if defined(CONFIG_RADIANT_PRIVATE_MAX_S)
#define PROFILE_PRIVATE_MAX_S_CONFIGURED ((uint32_t)CONFIG_RADIANT_PRIVATE_MAX_S)
#else
#define PROFILE_PRIVATE_MAX_S_CONFIGURED ((uint32_t)PROFILE_PRIVATE_MAX_S_DEFAULT)
#endif

/*
 * THE RATE LIMIT ON ACCEPTED COMMANDS, and it is not a defence against an
 * attacker - the tag is that, and a party holding K_cmd is a keyholder rather
 * than an intruder. It is a bound on the damage a buggy or captured receiver
 * can do to the OTHER N-1 listeners: each accepted command costs every one of
 * them a countdown and a retune, so a node that took them as fast as they
 * arrived would be a node one receiver could flap. Measured from the last
 * ACCEPTED command, so a stream of refusals cannot extend the floor.
 */
#define PROFILE_PRIVATE_CMD_MIN_S 60u

/* ---------------------------------------------------------------------------
 * The inbound command - section 11.6's "authenticated command from a paired
 * keyholder", which is the first user of RADIANT_SEC_LABEL_CMD anywhere
 * ---------------------------------------------------------------------------
 *
 *   [0]     page number. NOT EXAMINED - the caller has already filtered on it
 *           with profile_compat_is_compat_page(), which is the one function
 *           that should ever ask - but COVERED BY THE TAG, so nothing an
 *           attacker can rewrite sits outside it.
 *   [1]     0x00 = (0 << 4) | (1 - 1): a ONE-FRAME SET on the same framing
 *           convention everything else here uses. That is what keeps the
 *           command off a page number of its own - ADR 0008 allocates exactly
 *           two and the argument is the 7-bit namespace - and it is what makes
 *           it unmistakable for an enrolment frame, whose count is six and
 *           which profile_enrol.c refuses at any other count.
 *   [2]     the operation. TWO EXIST.
 *   [3..7]  trunc40( CMAC(K_cmd, nonce_block || [0..2]) ), subtype 0x04.
 *
 * Under K_cmd and not K_auth, which is the whole reason the key hierarchy has a
 * fourth label: every receiver holds K_auth in order to VERIFY the node, and if
 * commands were signed with it then every receiver could mute the sensor for
 * every other one. The counter in the nonce is Tier I's, derived from time on
 * both sides and carried nowhere, so a recorded command replayed into a later
 * interval fails.
 */
#define PROFILE_PRIVATE_CMD_LEN        8u
#define PROFILE_PRIVATE_CMD_FRAME_BYTE 0x00u
#define PROFILE_PRIVATE_CMD_COVERED    3u

#define PROFILE_PRIVATE_OP_GO_PRIVATE 0x01u
#define PROFILE_PRIVATE_OP_RETURN     0x02u

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 */
struct profile_private_cfg {
	/* The radiant_sec_compat channel that holds the root key. Both
	 * directions' announcements are tagged under it, INCLUDING the RETURN
	 * that goes out from the private channel: a receiver then verifies a
	 * RETURN with exactly the machinery it used for the SWITCH, and neither
	 * end has to decide which context an announcement belongs to. */
	uint8_t  ch;

	/* Only ever used to derive the locator. It is not broadcast, there is
	 * no field for it, and section 11.2 explains at length why not. */
	uint32_t epoch;

	/* enum-shaped: PROFILE_PRIVATE_* above. `policy` is the KCONFIG value;
	 * an NVM value, if one is provisioned, wins - see policy_load(). */
	uint8_t  policy;
	uint8_t  announce;

	/*
	 * Whether this node is actually attesting. It is the node's own answer,
	 * because the node is what called radiant_sec_compat_configure(), and
	 * it decides whether this configuration is legal at all - see
	 * PROFILE_PRIVATE_COMBINATION_OK().
	 */
	bool     attest;

	/* K, in transmitted messages. 0 means the default. */
	uint16_t k;

	/* The bounded duration, in seconds. 0 means the default; there is no
	 * value meaning "forever", which is the same refusal
	 * radiant_sec_pair_enter() makes about a pairing window. */
	uint32_t max_private_s;

	/* 0 means PROFILE_PRIVATE_CMD_MIN_S. */
	uint16_t cmd_min_s;

	/* Where the node goes. The device number is DERIVED rather than
	 * configured - that is the whole of the locator - so only the type and
	 * the period are settings. */
	uint8_t  private_device_type;
	uint16_t private_period;

	/*
	 * Where the node comes BACK to, which is its own compat channel id.
	 *
	 * A RETURN frame carries a locator like a SWITCH frame does, because it
	 * is the same frame - and the locator it carries is the target, not the
	 * origin. A RETURN that repeated the private locator would be telling
	 * receivers to follow the node to where they already are, which is the
	 * kind of bug that looks like it works until somebody power-cycles the
	 * head unit.
	 */
	uint8_t  compat_device_type;
	uint16_t compat_devnum;
	uint16_t compat_period;

	/*
	 * ── The policy's three surfaces ──────────────────────────────────
	 *
	 * policy_load() answers 0 and writes a policy when NVM HAS ONE, and
	 * anything negative when it does not - which is the ordinary state of a
	 * node nobody has provisioned and is not an error. NULL means "this
	 * node has no NVM", and then Kconfig is the whole of the answer.
	 *
	 * policy_store() is what makes the precedence rule structural rather
	 * than remembered: a host message WRITES NVM and then re-resolves, so
	 * there is no third value living in RAM that outranks the record.
	 * NULL refuses host writes outright, which is the honest behaviour for
	 * a node that cannot persist one.
	 */
	int (*policy_load)(void *user, uint8_t *policy);
	int (*policy_store)(void *user, uint8_t policy);

	/*
	 * ── The switch itself ────────────────────────────────────────────
	 *
	 * ONE CALLBACK PER DIRECTION, and the close is inside it rather than
	 * beside it. A node that closed the compat channel and then failed to
	 * open the private one would be muted, which is worse than either state
	 * it was moving between, so the two halves are never separately
	 * callable and never separately failable from here.
	 *
	 * enter_private() is given the derived locator. A negative return
	 * abandons the transition and the node stays where it is, which is the
	 * only safe direction to fail in.
	 */
	int (*enter_private)(void *user, uint8_t device_type, uint16_t devnum,
			     uint16_t period);
	int (*leave_private)(void *user);

	void *user;
};

struct profile_private {
	struct profile_private_cfg cfg;
	bool                       armed;

	/* The resolved policy, and where it came from. Both are readable,
	 * because "never, from NVM" and "never, because nobody configured
	 * anything" are different facts about a node in the field. */
	uint8_t policy;
	bool    policy_from_nvm;

	uint8_t state;
	uint8_t reason;
	uint16_t k;

	/* Transmitted messages seen, and the message index at which the
	 * countdown expires. Both are counts of TRANSMITTED MESSAGES, which is
	 * the unit the countdown is expressed in. */
	uint32_t msgs;
	uint32_t switch_at;

	uint64_t now_us;
	uint64_t private_since_us;
	uint64_t last_cmd_us;
	bool     ever_cmd;

	/* The last frame A that actually went on the air, all eight bytes as
	 * transmitted, and whether frame B may therefore be built. */
	uint8_t  frame_a[PROFILE_COMPAT_FRAME_LEN];
	bool     have_frame_a;
	bool     capture_next;

	/* The derived locator in use, and which candidate it is. */
	uint16_t locator;
	uint8_t  locator_attempt;

	struct profile_compat *compat;

	/* ── Counters, and every refusal has one ─────────────────────────
	 *
	 * A `never` node that refuses a keyholder's command SILENTLY is a node
	 * whose owner cannot tell it from one that never heard the command.
	 * Section 11.6 says "and counts the refusal" for that reason.
	 */
	uint32_t switches;
	uint32_t returns;
	uint32_t announcements;
	uint32_t commands_ok;
	uint32_t refused_policy;      /* the wrong policy for this trigger */
	uint32_t refused_unauth;      /* the tag did not verify */
	uint32_t refused_malformed;   /* including every unknown operation */
	uint32_t refused_rate;
	uint32_t refused_busy;        /* the interlock */
	uint32_t reverts_timeout;
	/* A transition the node itself could not perform: the channel would not
	 * open or would not close. Counted apart from every refusal above,
	 * because those are decisions and this is a failure. */
	uint32_t transitions_failed;
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/*
 * Resolve the policy, derive the locator and latch the configuration.
 *
 * A node whose resolved policy is `always` BOOTS STRAIGHT INTO PRIVATE and
 * never enters COMPAT - enter_private() is called from here - because that is
 * what the state means: it is today's radiant_sec node, named as a policy state
 * so the axis is complete.
 *
 * Returns 0, or:
 *   -EINVAL   a null argument, an unknown policy or announce value, a K outside
 *             {16, 32, 64, 128}, or a private device type with bit 7 set
 *   -ENOTSUP  the resolved combination is refused: `private_policy != never`
 *             without attestation, except `physical` + `silent`. THE BUILD
 *             REFUSES THE SAME COMBINATION - see the BUILD_ASSERT in the .c -
 *             and this is the arm that catches a policy which arrived from NVM
 *             on a node built for a different one
 *   whatever the locator derivation refused with, which on a channel holding no
 *             key is -ENOTSUP: a node that cannot derive a locator cannot go
 *             anywhere, and finding that out at init is better than finding it
 *             out with the compat channel already closed
 */
int profile_private_init(struct profile_private *pp,
			 const struct profile_private_cfg *cfg);

/*
 * Publish the countdown's frames through the beacon page's client seam, the
 * locator through frame 1 and the pending-switch bit through frame 0.
 *
 * REQUIRED for announce = broadcast and refused with -EINVAL if that beacon
 * does not exist: the announcement is frames 2 and 3 of the beacon page's set,
 * so a node with advertise = off has no set for them to be frames of. Such a
 * node can still switch - silently, which is the mode that needs no beacon and
 * no announcement at all.
 *
 * Optional for announce = silent, which puts nothing in the set and nothing in
 * the beacon.
 */
int profile_private_attach(struct profile_private *pp, struct profile_compat *pc);

/* The resolved policy, and whether NVM is where it came from. */
uint8_t profile_private_policy(const struct profile_private *pp);
bool profile_private_policy_from_nvm(const struct profile_private *pp);

/* PROFILE_PRIVATE_STATE_*, below. */
#define PROFILE_PRIVATE_STATE_COMPAT     0u
#define PROFILE_PRIVATE_STATE_ANNOUNCING 1u
#define PROFILE_PRIVATE_STATE_PRIVATE    2u
#define PROFILE_PRIVATE_STATE_RETURNING  3u

uint8_t profile_private_state(const struct profile_private *pp);
bool profile_private_is_private(const struct profile_private *pp);

/* The locator this node would use, and the candidate index it is on. */
uint16_t profile_private_locator(const struct profile_private *pp);

/*
 * Move to the next locator candidate - the collision rule, from the node's
 * side. A node learns about a collision the only way it can, by opening the
 * channel and finding it occupied, so this is a function it calls rather than a
 * condition this file can detect.
 *
 * Returns 0, or -ENOSPC after PROFILE_PRIVATE_LOCATOR_TRIES candidates, which
 * is where a node stops rederiving and stays where it is: a searching keyholder
 * gives up on the same count and falls back to a wildcard search, and a node
 * that walked further would be walking to numbers nobody looks at.
 */
#define PROFILE_PRIVATE_LOCATOR_TRIES 4u
int profile_private_next_locator(struct profile_private *pp);

/* ---------------------------------------------------------------------------
 * The triggers
 * ---------------------------------------------------------------------------
 */

/*
 * THE PHYSICAL ACTION - a button, a magnet, a strap re-seat. A function call so
 * that the thing which cannot be simulated on a bench is not also the thing
 * that cannot be tested.
 *
 * Toggles: it starts a switch from COMPAT and a return from PRIVATE, because a
 * device with one button has one button.
 *
 * Returns 0, or:
 *   -EPERM   policy `never` (counted) or `always` (counted: an `always` node
 *            has nowhere to go and no compat channel to come back to)
 *   -EBUSY   a countdown is already running, or the interlock: an enrolment
 *            window is open, and whoever is second is refused
 *   -EINVAL  not initialised
 */
int profile_private_physical_action(struct profile_private *pp, uint64_t now_us);

/*
 * ONE INBOUND COMMAND, eight bytes, arriving as acknowledged data exactly as an
 * enrolment frame does. THE TAG IS VERIFIED HERE - this module does not take a
 * caller's word for it, unlike profile_enrol.c's keyholder request, and the
 * difference is that this command's tag is defined (section 11.6) while that
 * one's page was left to the layer that would define it.
 *
 * Returns 1 when the command was accepted and a transition has begun, or:
 *   -EACCES  the tag did not verify. THE SECURITY PROPERTY: a stranger cannot
 *            mute this sensor for every legacy receiver in the room, and the
 *            attempt is counted
 *   -EPERM   the policy is not `command` (counted). A `never` node refuses an
 *            OTHERWISE-VALID authenticated command from a keyholder it trusts,
 *            which is what `never` means
 *   -EPROTO  malformed, or an operation this file does not implement. THERE IS
 *            NO POLICY OPERATION and its absence is deliberate, so a
 *            policy-change-shaped message lands here, is counted, and changes
 *            nothing
 *   -EAGAIN  rate-limited
 *   -EBUSY   a countdown is already running, or the interlock refused it
 *   -EINVAL  null, or a length that is not eight
 */
int profile_private_on_command(struct profile_private *pp, uint64_t now_us,
			       const uint8_t *pay, uint8_t len);

/*
 * The host surface for the policy, and the ONLY way the policy ever moves.
 *
 * Writes NVM and re-resolves; if the write fails, NOTHING CHANGES. That is the
 * precedence rule made structural rather than documented: there is no path here
 * that leaves a running value the record does not agree with.
 *
 * Returns 0, or -ENOTSUP with no policy_store() or for a policy this node
 * cannot honour, -EINVAL for an unknown policy, or -EBUSY while a countdown is
 * running or the node is private - a policy that changed under a countdown
 * would change what the countdown was for, half way through it.
 */
int profile_private_host_set_policy(struct profile_private *pp, uint8_t policy);

/* ---------------------------------------------------------------------------
 * The clock, and the frames
 * ---------------------------------------------------------------------------
 */

/*
 * The bounded duration expires here, and this is the ONE revert path that needs
 * a clock rather than an event. profile_private_frame_count() calls it, so a
 * node whose frames go through profile_compat.c owes nothing extra; a node that
 * is private and driving its own rotation calls it from there.
 *
 * Returns true if this call started a timeout revert.
 */
bool profile_private_tick(struct profile_private *pp, uint64_t now_us);

/*
 * PROFILE_PRIVATE_FRAMES while a countdown is running, 0 otherwise. Signatures
 * match struct profile_compat_client, and a node that is private drives these
 * three from its own device type 0x60 rotation instead - the same arrangement
 * profile_enrol.c has, for the same reason: the window, or the countdown, is on
 * whichever channel the node is currently on.
 *
 * WHAT IS DEFERRED, named so the absence is a decision: on device type 0x60 the
 * RETURN frames need a page number in the telemetry envelope's own namespace,
 * and allocating one is an envelope change rather than this phase's to make -
 * exactly the deferral profile_enrol.h records for the same six frames. Bytes
 * [2..7] are identical either way, which is why nothing in this API changes
 * when it lands.
 */
uint8_t profile_private_frame_count(void *user, uint64_t now_us);
bool profile_private_frame(void *user, uint8_t i, uint8_t *payload);

/*
 * EVERY transmitted message, including the ones this module did not build.
 *
 * It is the countdown's clock - the countdown is measured in transmitted
 * messages, so a module told only about its own frames would count eight and
 * call it sixty-four - and it is also where frame A's transmitted form is
 * captured, because byte [0] carries a toggle and byte [1] carries a count that
 * are not knowable when the payload is built and are both inside frame B's tag.
 */
void profile_private_sent(void *user, const uint8_t *body);

/* ---------------------------------------------------------------------------
 * The receiver's half
 * ---------------------------------------------------------------------------
 *
 * A keyed receiver following a switch. It is here rather than in a test because
 * a rule written twice is a rule two implementations will one day disagree
 * about, and the disagreement here is silent: a node and a receiver that
 * compute different retune messages do not fail, they simply stop meeting.
 */

/*
 * How long a receiver waits before deciding the stream has stopped, in missed
 * messages. Eight is two seconds at 4 Hz, and at the characterised ~0.4% loss
 * floor eight consecutive losses is ~10^-20 - so this is "the sensor has gone",
 * not "the air was busy". It is what bounds the SILENT re-acquisition gap, and
 * therefore what the measured broadcast/silent difference is mostly made of.
 */
#define PROFILE_PRIVATE_SILENCE_MSGS 8u

struct profile_private_rx {
	uint8_t  ch;          /* this receiver's keyed compat context */
	uint64_t period_us;   /* the channel period, for the silence rule */

	uint32_t msgs;        /* messages this receiver has taken in */
	uint64_t last_us;     /* ...and when the last one arrived */
	bool     any;

	bool     have_a;
	uint8_t  frame_a[PROFILE_COMPAT_FRAME_LEN];
	/* The message count when frame A arrived, which is the instant the node
	 * measured its countdown from. Frame B verifies the pair several
	 * messages later, and measuring from THERE would put every receiver a
	 * few messages behind the node by exactly the distance between the two
	 * frames - which varies with what else was in the rotation. */
	uint32_t a_at;

	bool     armed;
	uint32_t act_at;
	uint8_t  target_type;
	uint16_t target_devnum;
	uint16_t target_period;
	uint8_t  target_reason;

	uint32_t verified;
	uint32_t rejected;    /* a tag that did not verify, and was ignored */
};

int profile_private_rx_init(struct profile_private_rx *rx, uint8_t ch,
			    uint64_t period_us);

/*
 * One received message on whichever channel this receiver is on. Counts it, and
 * consumes it as an announcement frame if byte [0] and byte [1] say it is one -
 * frames 2 and 3 of a four-frame set on the beacon page.
 *
 * Returns 1 when an announcement has just been VERIFIED and the receiver is now
 * armed, 0 for anything else, -EACCES for a frame whose tag did not verify (it
 * is counted and IGNORED, which is the rule: the cost of ignoring it is the
 * re-acquisition path, and that is already built), or -EINVAL.
 */
int profile_private_rx_message(struct profile_private_rx *rx,
			       const uint8_t *body, uint8_t len,
			       uint64_t t_sync);

/*
 * The same, for a channel whose page numbering this file does not own: the
 * caller has already decided this eight-byte message is announcement frame
 * `which` (PROFILE_PRIVATE_FRAME_A or _B). Still counts the message, so a
 * caller uses one of the two functions per message and never both.
 */
int profile_private_rx_announce(struct profile_private_rx *rx, uint8_t which,
				const uint8_t *body, uint8_t len,
				uint64_t t_sync);

/* True once the countdown this receiver heard has expired: RETUNE NOW. Every
 * keyed receiver that heard the same announcement reaches this on the same
 * message, which is the property the countdown exists for. */
bool profile_private_rx_due(const struct profile_private_rx *rx);

int profile_private_rx_target(const struct profile_private_rx *rx,
			      uint8_t *device_type, uint16_t *devnum,
			      uint16_t *period, uint8_t *reason);

/* Done retuning; forget the announcement. */
void profile_private_rx_clear(struct profile_private_rx *rx);

/*
 * THE PATH THAT NEEDS NO ANNOUNCEMENT. A receiver that missed every copy, or
 * one whose node is silent, or one enrolled after the node had already gone,
 * derives the locator from the root it holds and the epoch it knows.
 *
 * `attempt` walks the same candidate sequence the node walks, which is what
 * makes the collision rule usable from both ends;
 * RADIANT_SEC_COMPAT_LOCATOR_TRIES is how many a searcher tries before falling
 * back to a wildcard search on the private device type.
 */
int profile_private_rx_locator(const struct profile_private_rx *rx,
			       uint32_t epoch, uint8_t attempt,
			       uint16_t *devnum);

/* True when nothing has arrived for PROFILE_PRIVATE_SILENCE_MSGS message
 * periods: the stream has stopped and it is time to go looking. */
bool profile_private_rx_lost(const struct profile_private_rx *rx,
			     uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_PRIVATE_H_ */
