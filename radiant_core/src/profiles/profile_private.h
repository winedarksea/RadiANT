/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_private.h - Layer C: the private-mode switch.
 *
 * Provenance: docs/radiant-security.md section 11.5 (announcement bytes,
 * countdown, derived locator, the two `announce` modes), section 11.6 (the
 * four policy states, the precedence rule, the key dependency and its one
 * exemption), and docs/decisions/0008-antplus-additive-pages-and-compat-security.md.
 * All this project's own documents. No ANT+ device profile
 * document was read for this file, no sdk-ant source was consulted, and
 * nothing here derives from libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is
 * ---------------------------------------------------------------------------
 * The node stops being an ANT+ sensor and becomes a RadiANT one, announcing so
 * first, in the clear, to everybody, on a countdown. The compat channel CLOSES
 * and a device type 0x60 channel opens - keeping 0x0B alive while emitting
 * unknown pages would leave a head unit showing a connected sensor with no
 * data, read as a bug rather than the walked-out-of-range close users
 * understand. While private, a Garmin head unit and Zwift see nothing at all;
 * that is the price of confidentiality.
 *
 * ---------------------------------------------------------------------------
 * The announcement is broadcast, or 1:N is broken
 * ---------------------------------------------------------------------------
 * The command comes from one keyholder; the stream has N listeners. A node
 * that acts and vanishes gives every other keyed receiver a dropout. So the
 * switch is a broadcast state change with a countdown, and receivers act on
 * COUNTDOWN EXPIRY rather than receipt, so every keyed receiver retunes on the
 * same message instead of at N independent moments.
 *
 * Five rules from section 11.5:
 *   1. Clear but self-authenticating. Tier I covers no payload and Tier II is
 *      off by default, so frame B tags frame A's full eight transmitted bytes
 *      under K_auth with Tier I's counter in the nonce: verifies on receipt,
 *      survives loss of every other packet, a replay into another interval
 *      fails. A receiver acts only after the tag verifies.
 *   2. Therefore private mode requires a key - without a tag the announcement
 *      is an unauthenticated herding attack. Dependency and exemption below.
 *   3. The beacon rate rises to meet the countdown, inheriting
 *      profile_compat.c's existing promotion to one frame in eight.
 *   4. Receivers act on expiry, reading the remaining count from the frame.
 *   5. The return is announced the same way; only power-cycle/crash reverts
 *      skip it, and those are exactly the cases where a receiver searches
 *      anyway and finds the compat channel.
 *
 * ---------------------------------------------------------------------------
 * The countdown's arithmetic
 * ---------------------------------------------------------------------------
 * Frame A byte [7] carries six bits of countdown in units of eight transmitted
 * messages (K reaches 128, and six bits of raw messages would only reach 63).
 *
 * The scheduler never offers messages 119/120 of its 121-message cycle, so
 * promoted slots are not evenly eight apart across a cycle boundary - a
 * quantised countdown can't name an exact instant by rounding alone. Instead
 * the node RE-ANCHORS: every time frame A goes out it recomputes the count it
 * can honestly name and moves its own target to match:
 *
 *     c         = ceil( (switch_at - messages_sent_after_this_one) / 8 )
 *     switch_at = messages_sent_after_this_one + c * 8
 *
 * so every receiver that heard the latest announcement computes the same
 * message, and one that heard only an earlier copy is at most seven messages
 * early (never late). Rounded UP rather than down for the same reason: down
 * would shorten the countdown up to seven messages per off-quantum
 * announcement and retune a receiver LATE, after the node had already gone.
 *
 * K = 64 is ~16 s at 4 Hz, eight promoted beacon slots; the four-frame set
 * rotates through them one at a time, so a countdown carries two complete
 * announcement pairs at K = 64, four at K = 128. At the characterised ~0.4%
 * loss floor, losing both is not worth designing around - the fallback is the
 * derived locator, which does not depend on hearing anything.
 *
 * ---------------------------------------------------------------------------
 * `silent` is a mode, not a failure
 * ---------------------------------------------------------------------------
 * With announce = silent the node emits no announcement frame, publishes no
 * locator, never sets the pending-switch bit, never promotes the beacon: the
 * channel just closes. Keyholders rederive the locator and re-acquire.
 * `silent` buys unlinkability and pays in availability; `broadcast` is the
 * reverse - neither is "more secure". radiant_core/tests/src/test_profile_private.c
 * measures the gap between them.
 *
 * ---------------------------------------------------------------------------
 * Policy is configuration, never over the air
 * ---------------------------------------------------------------------------
 * A `never` node a stranger or paired keyholder can talk into becoming
 * `command` has no policy, only a suggestion. There is no over-air policy
 * operation here: not deferred, refused. Inbound commands carry only "go
 * private" and "come back"; an unrecognised op is counted and dropped.
 *
 * Three surfaces, one precedence rule: NVM IF PROVISIONED, ELSE KCONFIG; THE
 * HOST MESSAGE WRITES NVM rather than shadowing it.
 * profile_private_host_set_policy() fails if the write fails and the resolved
 * policy does not move, so it can never depend on when the node last rebooted.
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
 * Deliberately the same names the beacon carries: the policy in frame 0
 * byte [3] bits 1..0 IS this policy, not a translation of it.
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
 * (NEVER - a byte-exact ANT+ sensor for its whole life) when the symbols are
 * absent, which is what the unit-test application sees.
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
 * The key dependency and its one exemption, as a predicate rather than a
 * paragraph: it holds at three call sites (a BUILD_ASSERT in
 * profile_private.c, the runtime refusal in profile_private_init(), and a
 * host message that would write an unrunnable policy into NVM).
 *
 * `private_policy != never` requires K_auth and Tier I: `command` needs an
 * authenticated command, `broadcast` needs its frame-B tag. `physical` +
 * `silent` is the exemption - with no announcement there is nothing to forge
 * and no over-air trigger to authenticate.
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
 * ranged: a value the beacon's six bits or the receiver's arithmetic can't
 * express is a handover that silently misses. */
#define PROFILE_PRIVATE_K_MIN     16u
#define PROFILE_PRIVATE_K_MAX     128u
#define PROFILE_PRIVATE_K_DEFAULT 64u

#if defined(CONFIG_RADIANT_PRIVATE_COUNTDOWN)
#define PROFILE_PRIVATE_K_CONFIGURED ((uint16_t)CONFIG_RADIANT_PRIVATE_COUNTDOWN)
#else
#define PROFILE_PRIVATE_K_CONFIGURED ((uint16_t)PROFILE_PRIVATE_K_DEFAULT)
#endif

/* One unit of the countdown field, in transmitted messages - the promoted
 * beacon interval by definition, hence defined from it. */
#define PROFILE_PRIVATE_COUNTDOWN_UNIT PROFILE_COMPAT_BEACON_PROMOTED_EVERY
#define PROFILE_PRIVATE_COUNTDOWN_MAX  0x3Fu

/* Frame A byte [7] bits 7:6. `timeout-revert` is why the reason is on the air
 * at all: it lets a receiver distinguish an expired bounded duration from a
 * user-requested return. */
#define PROFILE_PRIVATE_REASON_COMMAND  0u
#define PROFILE_PRIVATE_REASON_PHYSICAL 1u
#define PROFILE_PRIVATE_REASON_TIMEOUT  2u

/* The announcement's two frames, and their payload width: bytes [2..7], because
 * profile_compat.c owns byte [0] and byte [1]. */
#define PROFILE_PRIVATE_FRAMES        2u
#define PROFILE_PRIVATE_FRAME_PAYLOAD 6u
#define PROFILE_PRIVATE_FRAME_A       0u
#define PROFILE_PRIVATE_FRAME_B       1u

/* The frame indices the announcement occupies in the beacon page's set, which
 * grows to four for a countdown's duration. Pinned by section 11.5, which is
 * what lets a receiver tell an announcement from an enrolment window's frames
 * without either module knowing about the other. */
#define PROFILE_PRIVATE_SET_FRAMES  4u
#define PROFILE_PRIVATE_SET_INDEX_A 2u
#define PROFILE_PRIVATE_SET_INDEX_B 3u

/* ---------------------------------------------------------------------------
 * The bounded duration, and the command's rate limit
 * ---------------------------------------------------------------------------
 */

/* A node-configured maximum private duration: a sensor that silently stays
 * private is indistinguishable from a dead one. One hour by default - long
 * enough for the ride, short enough that an abandoned node is an ANT+ sensor
 * again before its owner wonders if it's broken. */
#define PROFILE_PRIVATE_MAX_S_DEFAULT 3600u

#if defined(CONFIG_RADIANT_PRIVATE_MAX_S)
#define PROFILE_PRIVATE_MAX_S_CONFIGURED ((uint32_t)CONFIG_RADIANT_PRIVATE_MAX_S)
#else
#define PROFILE_PRIVATE_MAX_S_CONFIGURED ((uint32_t)PROFILE_PRIVATE_MAX_S_DEFAULT)
#endif

/*
 * The rate limit on accepted commands - not a defence against an attacker (the
 * tag is that); a bound on the damage a buggy or captured receiver can do to
 * the other N-1 listeners, each accepted command costing them a countdown and
 * a retune. Measured from the last accepted command, so refusals can't extend
 * the floor.
 */
#define PROFILE_PRIVATE_CMD_MIN_S 60u

/* ---------------------------------------------------------------------------
 * The inbound command - section 11.6's "authenticated command from a paired
 * keyholder", the first user of RADIANT_SEC_LABEL_CMD anywhere
 * ---------------------------------------------------------------------------
 *
 *   [0]     page number. Not examined - the caller already filtered on it
 *           with profile_compat_is_compat_page() - but covered by the tag.
 *   [1]     0x00 = (0 << 4) | (1 - 1): a one-frame set, unmistakable for an
 *           enrolment frame (count six, refused elsewhere at any other count).
 *   [2]     the operation. Two exist.
 *   [3..7]  trunc40( CMAC(K_cmd, nonce_block || [0..2]) ), subtype 0x04.
 *
 * Under K_cmd, not K_auth: every receiver holds K_auth to verify the node, and
 * if commands were signed with it any receiver could mute the sensor for every
 * other one. The nonce counter is Tier I's, derived from time on both sides
 * and carried nowhere, so a replay into a later interval fails.
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
	/* The radiant_sec_compat channel holding the root key. Both directions'
	 * announcements, including the RETURN, are tagged under it, so a
	 * receiver verifies both with the same machinery. */
	uint8_t  ch;

	/* Only used to derive the locator; never broadcast (section 11.2). */
	uint32_t epoch;

	/* enum-shaped: PROFILE_PRIVATE_* above. `policy` is the KCONFIG value;
	 * an NVM value, if one is provisioned, wins - see policy_load(). */
	uint8_t  policy;
	uint8_t  announce;

	/* Whether this node is actually attesting - its own answer, since it
	 * called radiant_sec_compat_configure(). Decides whether this
	 * configuration is legal at all; see PROFILE_PRIVATE_COMBINATION_OK(). */
	bool     attest;

	/* K, in transmitted messages. 0 means the default. */
	uint16_t k;

	/* The bounded duration, in seconds. 0 means the default; no value
	 * means "forever" (same refusal radiant_sec_pair_enter() makes about
	 * a pairing window). */
	uint32_t max_private_s;

	/* 0 means PROFILE_PRIVATE_CMD_MIN_S. */
	uint16_t cmd_min_s;

	/* Where the node goes. The device number is DERIVED rather than
	 * configured - that is the whole of the locator - so only the type and
	 * the period are settings. */
	uint8_t  private_device_type;
	uint16_t private_period;

	/* Where the node comes back to: its own compat channel id. A RETURN
	 * frame carries a locator like a SWITCH frame (same frame format), and
	 * it names the target, not the origin - repeating the private locator
	 * would tell receivers to follow the node to where they already are. */
	uint8_t  compat_device_type;
	uint16_t compat_devnum;
	uint16_t compat_period;

	/*
	 * ── The policy's three surfaces ──────────────────────────────────
	 *
	 * policy_load() answers 0 and writes a policy when NVM has one,
	 * negative when not provisioned (not an error). NULL means "no NVM",
	 * so Kconfig is the whole answer.
	 *
	 * policy_store() makes the precedence rule structural: a host message
	 * writes NVM and then re-resolves, so no third value in RAM outranks
	 * the record. NULL refuses host writes outright.
	 */
	int (*policy_load)(void *user, uint8_t *policy);
	int (*policy_store)(void *user, uint8_t policy);

	/*
	 * ── The switch itself ────────────────────────────────────────────
	 *
	 * One callback per direction, with the close inside it: a node that
	 * closed the compat channel and failed to open the private one would
	 * be muted, so the two halves are never separately failable from here.
	 *
	 * enter_private() is given the derived locator. A negative return
	 * abandons the transition and the node stays where it is.
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

	/* The last frame A that actually went on the air, as transmitted, and
	 * whether frame B may therefore be built. */
	uint8_t  frame_a[PROFILE_COMPAT_FRAME_LEN];
	bool     have_frame_a;
	bool     capture_next;

	/* The derived locator in use, and which candidate it is. */
	uint16_t locator;
	uint8_t  locator_attempt;

	struct profile_compat *compat;

	/* ── Counters, and every refusal has one ─────────────────────────
	 * A `never` node that refused a command silently is indistinguishable
	 * from one that never heard it, so every refusal is counted. */
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
	/* A transition the node itself could not perform (channel wouldn't
	 * open/close). Counted apart from the refusals above, which are
	 * decisions rather than failures. */
	uint32_t transitions_failed;
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/*
 * Resolve the policy, derive the locator and latch the configuration.
 *
 * A node whose resolved policy is `always` boots straight into PRIVATE and
 * never enters COMPAT (enter_private() is called from here) - today's
 * radiant_sec node, named as a policy state.
 *
 * Returns 0, or:
 *   -EINVAL   a null argument, an unknown policy or announce value, a K outside
 *             {16, 32, 64, 128}, or a private device type with bit 7 set
 *   -ENOTSUP  the resolved combination is refused (see BUILD_ASSERT in the
 *             .c); this is the arm that catches a policy that arrived from
 *             NVM on a node built for a different one
 *   whatever the locator derivation refused with (-ENOTSUP on a channel with
 *             no key) - better to find out at init than with the compat
 *             channel already closed
 */
int profile_private_init(struct profile_private *pp,
			 const struct profile_private_cfg *cfg);

/*
 * Publish the countdown's frames through the beacon page's client seam, the
 * locator through frame 1 and the pending-switch bit through frame 0.
 *
 * Required for announce = broadcast and refused with -EINVAL if that beacon
 * does not exist (the announcement is frames 2 and 3 of its set). Such a node
 * can still switch silently. Optional for announce = silent.
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
 * side. A node learns of a collision only by opening the channel and finding
 * it occupied, so this is a function it calls rather than a condition this
 * file can detect.
 *
 * Returns 0, or -ENOSPC after PROFILE_PRIVATE_LOCATOR_TRIES candidates: a
 * searching keyholder gives up on the same count and falls back to a wildcard
 * search, so walking further would reach numbers nobody looks at.
 */
#define PROFILE_PRIVATE_LOCATOR_TRIES 4u
int profile_private_next_locator(struct profile_private *pp);

/* ---------------------------------------------------------------------------
 * The triggers
 * ---------------------------------------------------------------------------
 */

/*
 * The physical action - a button, a magnet, a strap re-seat. A function call
 * so the thing that can't be simulated on a bench isn't also the thing that
 * can't be tested. Toggles: starts a switch from COMPAT, a return from
 * PRIVATE.
 *
 * Returns 0, or:
 *   -EPERM   policy `never` (counted) or `always` (counted: nowhere to go / no
 *            compat channel to return to)
 *   -EBUSY   a countdown is already running, or the interlock (an enrolment
 *            window is open; whoever is second is refused)
 *   -EINVAL  not initialised
 */
int profile_private_physical_action(struct profile_private *pp, uint64_t now_us);

/*
 * One inbound command, eight bytes, arriving as acknowledged data exactly as
 * an enrolment frame does. The tag is verified here - this module does not
 * take a caller's word for it.
 *
 * Returns 1 when the command was accepted and a transition has begun, or:
 *   -EACCES  the tag did not verify (the security property: a stranger cannot
 *            mute this sensor for every legacy receiver), counted
 *   -EPERM   the policy is not `command` (counted) - a `never` node refuses an
 *            otherwise-valid authenticated command
 *   -EPROTO  malformed, or an operation this file does not implement - there
 *            is no policy operation, so a policy-change-shaped message is
 *            counted and changes nothing
 *   -EAGAIN  rate-limited
 *   -EBUSY   a countdown is already running, or the interlock refused it
 *   -EINVAL  null, or a length that is not eight
 */
int profile_private_on_command(struct profile_private *pp, uint64_t now_us,
			       const uint8_t *pay, uint8_t len);

/*
 * The host surface for the policy, and the only way it ever moves.
 *
 * Writes NVM and re-resolves; if the write fails, nothing changes - no path
 * here leaves a running value the record disagrees with.
 *
 * Returns 0, or -ENOTSUP with no policy_store() or for a policy this node
 * cannot honour, -EINVAL for an unknown policy, or -EBUSY while a countdown is
 * running or the node is private.
 */
int profile_private_host_set_policy(struct profile_private *pp, uint8_t policy);

/* ---------------------------------------------------------------------------
 * The clock, and the frames
 * ---------------------------------------------------------------------------
 */

/*
 * The bounded duration expires here - the one revert path that needs a clock
 * rather than an event. profile_private_frame_count() calls it, so a node
 * whose frames go through profile_compat.c owes nothing extra.
 *
 * Returns true if this call started a timeout revert.
 */
bool profile_private_tick(struct profile_private *pp, uint64_t now_us);

/*
 * PROFILE_PRIVATE_FRAMES while a countdown is running, 0 otherwise. Signatures
 * match struct profile_compat_client; a private node drives these from its own
 * device type 0x60 rotation instead.
 *
 * Deferred: on device type 0x60 the RETURN frames need a page number in the
 * telemetry envelope's own namespace, which is an envelope change rather than
 * this phase's to make. Bytes [2..7] are identical either way.
 */
uint8_t profile_private_frame_count(void *user, uint64_t now_us);
bool profile_private_frame(void *user, uint8_t i, uint8_t *payload);

/*
 * Every transmitted message, including ones this module did not build. It is
 * the countdown's clock (measured in transmitted messages, so a module told
 * only about its own frames would miscount), and it is where frame A's
 * transmitted form is captured - byte [0]'s toggle and byte [1]'s count are
 * not knowable when the payload is built, and both are inside frame B's tag.
 */
void profile_private_sent(void *user, const uint8_t *body);

/* ---------------------------------------------------------------------------
 * The receiver's half
 * ---------------------------------------------------------------------------
 *
 * A keyed receiver following a switch. Here rather than in a test because a
 * node and a receiver computing different retune messages fail silently -
 * they simply stop meeting.
 */

/* How long a receiver waits before deciding the stream has stopped, in missed
 * messages. Eight is two seconds at 4 Hz; at the ~0.4% loss floor, eight
 * consecutive losses is ~10^-20, so this reads as "the sensor has gone", not
 * "the air was busy". Bounds the silent re-acquisition gap. */
#define PROFILE_PRIVATE_SILENCE_MSGS 8u

struct profile_private_rx {
	uint8_t  ch;          /* this receiver's keyed compat context */
	uint64_t period_us;   /* the channel period, for the silence rule */

	uint32_t msgs;        /* messages this receiver has taken in */
	uint64_t last_us;     /* ...and when the last one arrived */
	bool     any;

	bool     have_a;
	uint8_t  frame_a[PROFILE_COMPAT_FRAME_LEN];
	/* The message count when frame A arrived - the instant the node
	 * measured its countdown from. Measuring from frame B instead would
	 * put every receiver behind by the (variable) gap between the frames. */
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
 * caller has already decided this message is announcement frame `which`.
 * Still counts the message, so a caller uses one of the two functions per
 * message and never both.
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
 * The path that needs no announcement. A receiver that missed every copy, or
 * whose node is silent, or was enrolled after the node had already gone,
 * derives the locator from the root it holds and the epoch it knows.
 *
 * `attempt` walks the same candidate sequence the node walks; a searcher tries
 * RADIANT_SEC_COMPAT_LOCATOR_TRIES before falling back to a wildcard search.
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
