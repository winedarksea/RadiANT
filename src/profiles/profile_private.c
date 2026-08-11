/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_private.c - Layer C: the private-mode switch.
 *
 * Provenance: docs/radiant-security.md sections 11.5 and 11.6 and
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md. Both are
 * this project's own documents. No adopter-gated ANT+ device profile document
 * was read for this file, no sdk-ant source was consulted, and nothing here
 * derives from libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * The header carries the argument for every decision here - the re-anchoring
 * countdown, the key dependency and its exemption, the precedence rule, and why
 * `silent` is a mode rather than a failure. This file carries the bytes and the
 * state machine.
 *
 * It is the THIRD file in src/profiles/ that includes a radiant_core header,
 * after profile_compat.c and profile_enrol.c, and it is for the third distinct
 * reason: deriving a locator and verifying a command are key operations, and
 * K_id and K_cmd live where the root key does. It names no page number that is
 * not profile_compat.h's already, and profile_hr.c and profile_power.c reach
 * neither this file nor a key.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <radiant_core/radiant_sec_compat.h>

#include "profile_compat.h"
#include "profile_private.h"

#if defined(CONFIG_RADIANT_SEC) && defined(CONFIG_RADIANT_SEC_COMPAT)

/*
 * ── THE BUILD-TIME HALF OF THE KEY DEPENDENCY ──────────────────────────────
 *
 * "A node with attest off refuses to build with private_policy != never, except
 * the physical + silent combination, which must build and work."
 *
 * Expressed twice, and neither expression is redundant. src/profiles/Kconfig
 * carries `depends on` so the combination cannot be SELECTED - which is where a
 * person finds out, in menuconfig, while they are choosing. This BUILD_ASSERT
 * carries the same predicate so it cannot be REACHED by any other route: a
 * defconfig fragment, an out-of-tree board file, or a Kconfig edit that relaxes
 * the dependency and forgets why it was there. The predicate itself is in the
 * header, shared with the runtime refusal, so there is one expression of the
 * rule and three places that hold it.
 *
 * PROFILE_PRIVATE_ATTEST_DEFAULT is Tier I's Kconfig, defaulting to ON exactly
 * as section 11.6's table does - so an application that does not source
 * src/profiles/Kconfig at all (the unit-test image is one) gets the table's
 * default rather than an accidental "attestation is off".
 */
#if defined(CONFIG_RADIANT_PROFILE_POLICY)
#define PROFILE_PRIVATE_ATTEST_DEFAULT IS_ENABLED(CONFIG_RADIANT_ATTEST_ID)
#else
#define PROFILE_PRIVATE_ATTEST_DEFAULT 1
#endif

BUILD_ASSERT(PROFILE_PRIVATE_COMBINATION_OK(PROFILE_PRIVATE_POLICY_DEFAULT,
					    PROFILE_PRIVATE_ANNOUNCE_DEFAULT,
					    PROFILE_PRIVATE_ATTEST_DEFAULT),
	     "private_policy != never requires K_auth and Tier I: a command "
	     "trigger needs an authenticated command and a broadcast "
	     "announcement needs its frame-B tag. physical + silent is the one "
	     "exemption, because with no announcement there is nothing to forge "
	     "and no over-air trigger to authenticate");

BUILD_ASSERT(PROFILE_PRIVATE_K_CONFIGURED == 16u ||
		     PROFILE_PRIVATE_K_CONFIGURED == 32u ||
		     PROFILE_PRIVATE_K_CONFIGURED == 64u ||
		     PROFILE_PRIVATE_K_CONFIGURED == 128u,
	     "K is one of {16, 32, 64, 128}: both ends have to agree on how "
	     "long a countdown can be, and the six-bit field counts promoted "
	     "beacon intervals rather than messages so that 128 fits");

BUILD_ASSERT(PROFILE_PRIVATE_SET_FRAMES ==
		     PROFILE_COMPAT_BEACON_FRAMES + PROFILE_PRIVATE_FRAMES,
	     "the announcement is frames 2 and 3 of the beacon page's set, so "
	     "the set is four frames while a countdown runs and every frame of "
	     "it says four");

BUILD_ASSERT(PROFILE_PRIVATE_FRAME_PAYLOAD == PROFILE_COMPAT_FRAME_LEN - 2u,
	     "a client owns bytes [2..7] and nothing else");

BUILD_ASSERT(PROFILE_PRIVATE_LOCATOR_TRIES == RADIANT_SEC_COMPAT_LOCATOR_TRIES,
	     "a node that rederived further than a searcher looks would walk "
	     "to a device number nobody tries");

/* ── Small helpers ──────────────────────────────────────────────────────── */

#define US_PER_S 1000000ull

static bool policy_is_known(uint8_t policy)
{
	return policy <= PROFILE_PRIVATE_ALWAYS;
}

static bool k_is_legal(uint16_t k)
{
	/* Inside the bounds with a single bit set, which is {16, 32, 64, 128}
	 * spelled as an arithmetic property rather than as a table. */
	return k >= PROFILE_PRIVATE_K_MIN && k <= PROFILE_PRIVATE_K_MAX &&
	       (k & (uint16_t)(k - 1u)) == 0u;
}

static bool announcing(const struct profile_private *pp)
{
	return pp->state == PROFILE_PRIVATE_STATE_ANNOUNCING ||
	       pp->state == PROFILE_PRIVATE_STATE_RETURNING;
}

static uint64_t max_private_us(const struct profile_private *pp)
{
	uint32_t s = pp->cfg.max_private_s != 0u ? pp->cfg.max_private_s
						 : PROFILE_PRIVATE_MAX_S_DEFAULT;

	return (uint64_t)s * US_PER_S;
}

/* ── The locator ────────────────────────────────────────────────────────── */

static int locator_at(struct profile_private *pp, uint8_t attempt)
{
	uint16_t devnum = 0u;
	int      rc;

	rc = radiant_sec_compat_locator(pp->cfg.ch, pp->cfg.epoch, attempt,
					&devnum);
	if (rc != RADIANT_SEC_OK) {
		/* A channel with no root cannot say where this node would go,
		 * and a node that cannot say that cannot go. ENOTSUP rather
		 * than the module's own numbering, which is a different scale -
		 * profile_enrol.c makes the same translation for the same
		 * reason. */
		return -ENOTSUP;
	}
	pp->locator = devnum;
	pp->locator_attempt = attempt;
	return 0;
}

uint16_t profile_private_locator(const struct profile_private *pp)
{
	return (pp == NULL) ? 0u : pp->locator;
}

int profile_private_next_locator(struct profile_private *pp)
{
	if (pp == NULL || !pp->armed) {
		return -EINVAL;
	}
	if ((uint8_t)(pp->locator_attempt + 1u) >=
	    (uint8_t)PROFILE_PRIVATE_LOCATOR_TRIES) {
		return -ENOSPC;
	}
	return locator_at(pp, (uint8_t)(pp->locator_attempt + 1u));
}

/* ── The beacon's two Layer C fields ────────────────────────────────────── */

/*
 * A silent node publishes NEITHER, ever, and that is the whole of `silent` on
 * this page: no locator field, no pending-switch bit, no announcement frame,
 * and no promotion of the beacon rate. Three absences from one decision, kept
 * in one place so that adding a fourth thing to publish cannot forget it.
 */
static bool publishes(const struct profile_private *pp)
{
	return pp->compat != NULL &&
	       pp->cfg.announce == PROFILE_PRIVATE_BROADCAST;
}

static void publish_locator(struct profile_private *pp)
{
	if (!publishes(pp)) {
		return;
	}
	(void)profile_compat_set_locator(pp->compat,
					 pp->cfg.private_device_type,
					 pp->locator, pp->cfg.private_period);
}

static void publish_pending(struct profile_private *pp, bool pending)
{
	if (!publishes(pp)) {
		return;
	}
	(void)profile_compat_set_pending_switch(pp->compat, pending);
}

/* ── The transition ─────────────────────────────────────────────────────── */

static int commit(struct profile_private *pp, bool to_private)
{
	int rc;

	if (to_private) {
		/*
		 * ONE CALL, and the compat channel's close is inside it. A node
		 * that closed one channel and failed to open the other would be
		 * muted, which is worse than either state it was moving
		 * between - so the failure is the callee's to avoid and the
		 * only thing this file does about it is refuse to have moved.
		 */
		rc = pp->cfg.enter_private(pp->cfg.user,
					   pp->cfg.private_device_type,
					   pp->locator, pp->cfg.private_period);
		if (rc != 0) {
			pp->transitions_failed++;
			pp->state = PROFILE_PRIVATE_STATE_COMPAT;
			publish_pending(pp, false);
			return rc;
		}
		pp->state = PROFILE_PRIVATE_STATE_PRIVATE;
		pp->private_since_us = pp->now_us;
		pp->switches++;
	} else {
		rc = pp->cfg.leave_private(pp->cfg.user);
		if (rc != 0) {
			pp->transitions_failed++;
			pp->state = PROFILE_PRIVATE_STATE_PRIVATE;
			return rc;
		}
		pp->state = PROFILE_PRIVATE_STATE_COMPAT;
		pp->returns++;
	}

	pp->have_frame_a = false;
	pp->capture_next = false;
	publish_pending(pp, false);
	return 0;
}

static int begin(struct profile_private *pp, uint8_t reason, bool to_private,
		 uint64_t now_us)
{
	pp->now_us = now_us;

	/*
	 * THE INTERLOCK. Layer D's six pubkey frames occupy the indices this
	 * announcement's two would, and a switch closes the channel that window
	 * is running on - so a countdown that started while a window was open
	 * would tear a public key in half AND take the channel out from under
	 * it. Whoever is second is refused, and this is the second one's side of
	 * that rule; profile_enrol.c's window_open() is the other.
	 */
	if (pp->compat != NULL &&
	    profile_compat_client_busy(pp->compat, now_us, pp)) {
		pp->refused_busy++;
		return -EBUSY;
	}

	if (pp->cfg.announce == PROFILE_PRIVATE_SILENT) {
		/*
		 * `silent`: the channel simply closes. No countdown, because a
		 * countdown with nothing to carry it is a delay rather than a
		 * warning, and delaying the switch would only widen the window
		 * in which an observer sees one channel stop and another start.
		 */
		return commit(pp, to_private);
	}

	pp->state = to_private ? PROFILE_PRIVATE_STATE_ANNOUNCING
			       : PROFILE_PRIVATE_STATE_RETURNING;
	pp->reason = reason;
	pp->switch_at = pp->msgs + (uint32_t)pp->k;
	pp->have_frame_a = false;
	pp->capture_next = false;
	publish_pending(pp, true);
	return 0;
}

/* ── Init ───────────────────────────────────────────────────────────────── */

/*
 * NVM IF PROVISIONED, ELSE KCONFIG. The one precedence rule, in the one place
 * it is applied, so that the host arm below can re-run it rather than
 * re-implement it.
 */
static void resolve_policy(struct profile_private *pp)
{
	uint8_t stored = 0u;

	pp->policy = pp->cfg.policy;
	pp->policy_from_nvm = false;

	if (pp->cfg.policy_load == NULL) {
		return;
	}
	if (pp->cfg.policy_load(pp->cfg.user, &stored) != 0) {
		/* Not provisioned. The ordinary state of a node nobody has
		 * configured, and not an error. */
		return;
	}
	if (!policy_is_known(stored)) {
		/* A record this firmware cannot interpret is not a licence to
		 * pick one: fall back to the compile-time value, which on a
		 * shipped strap is `never`. */
		return;
	}
	pp->policy = stored;
	pp->policy_from_nvm = true;
}

int profile_private_init(struct profile_private *pp,
			 const struct profile_private_cfg *cfg)
{
	int rc;

	if (pp == NULL || cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->enter_private == NULL || cfg->leave_private == NULL) {
		return -EINVAL;
	}
	if (!policy_is_known(cfg->policy) ||
	    cfg->announce > PROFILE_PRIVATE_SILENT) {
		return -EINVAL;
	}
	if ((cfg->private_device_type & PROFILE_COMPAT_PAGE_TOGGLE) != 0u ||
	    (cfg->compat_device_type & PROFILE_COMPAT_PAGE_TOGGLE) != 0u) {
		/* A device type is 7 bits in these fields; bit 7 is an ANT
		 * channel id's pairing bit and is not ours to set. */
		return -EINVAL;
	}

	memset(pp, 0, sizeof(*pp));
	pp->cfg = *cfg;

	pp->k = (cfg->k != 0u) ? cfg->k : (uint16_t)PROFILE_PRIVATE_K_DEFAULT;
	if (!k_is_legal(pp->k)) {
		return -EINVAL;
	}

	resolve_policy(pp);

	/* The runtime arm of the key dependency, and the one that catches a
	 * policy which arrived from NVM on a node built for a different one -
	 * which the BUILD_ASSERT cannot see and is the whole reason the
	 * predicate is shared rather than duplicated. */
	if (!PROFILE_PRIVATE_COMBINATION_OK(pp->policy, pp->cfg.announce,
					    pp->cfg.attest)) {
		return -ENOTSUP;
	}

	pp->state = PROFILE_PRIVATE_STATE_COMPAT;

	if (pp->policy == PROFILE_PRIVATE_NEVER) {
		/*
		 * No locator, because there is nowhere for this node to go and
		 * a locator it did not use would still have to be zero in the
		 * beacon. profile_compat.c refuses a `never` node that carries
		 * one, which is the same rule from the other side.
		 */
		pp->armed = true;
		return 0;
	}

	rc = locator_at(pp, 0u);
	if (rc != 0) {
		return rc;
	}
	pp->armed = true;

	if (pp->policy == PROFILE_PRIVATE_ALWAYS) {
		/*
		 * Straight into PRIVATE, never through COMPAT. This is today's
		 * radiant_sec node, named as a policy state so the axis is
		 * complete and so the Tier 2 posture has somewhere to live -
		 * and it is the one path where nothing is announced because
		 * there was never a compat channel with listeners on it.
		 */
		rc = commit(pp, true);
		if (rc != 0) {
			pp->armed = false;
			return rc;
		}
	}
	return 0;
}

int profile_private_attach(struct profile_private *pp, struct profile_compat *pc)
{
	struct profile_compat_client client;
	int                          rc;

	if (pp == NULL || pc == NULL || !pp->armed) {
		return -EINVAL;
	}

	memset(&client, 0, sizeof(client));
	client.frames = profile_private_frame_count;
	client.frame = profile_private_frame;
	client.sent = profile_private_sent;
	client.user = pp;

	rc = profile_compat_set_client(pc, &client);
	if (rc != 0) {
		return rc;
	}
	pp->compat = pc;

	/*
	 * A broadcast announcement is FRAMES 2 AND 3 OF THE BEACON PAGE'S SET,
	 * so a node with advertise = off has no set for them to be frames of.
	 * Refused here rather than discovered at the switch, because the switch
	 * is the moment the compat channel closes and "the announcement did not
	 * go out" is not a thing to find out then. Such a node can still switch
	 * silently, which is the mode that needs no beacon at all.
	 */
	if (pp->cfg.announce == PROFILE_PRIVATE_BROADCAST &&
	    pp->policy != PROFILE_PRIVATE_NEVER && pc->n_frames == 0u) {
		pp->compat = NULL;
		(void)profile_compat_set_client(pc, NULL);
		return -EINVAL;
	}

	if (pp->policy != PROFILE_PRIVATE_NEVER) {
		publish_locator(pp);
	}
	publish_pending(pp, announcing(pp));
	return 0;
}

uint8_t profile_private_policy(const struct profile_private *pp)
{
	return (pp == NULL) ? (uint8_t)PROFILE_PRIVATE_NEVER : pp->policy;
}

bool profile_private_policy_from_nvm(const struct profile_private *pp)
{
	return pp != NULL && pp->policy_from_nvm;
}

uint8_t profile_private_state(const struct profile_private *pp)
{
	return (pp == NULL) ? (uint8_t)PROFILE_PRIVATE_STATE_COMPAT : pp->state;
}

bool profile_private_is_private(const struct profile_private *pp)
{
	return pp != NULL && (pp->state == PROFILE_PRIVATE_STATE_PRIVATE ||
			      pp->state == PROFILE_PRIVATE_STATE_RETURNING);
}

/* ── The triggers ───────────────────────────────────────────────────────── */

int profile_private_physical_action(struct profile_private *pp, uint64_t now_us)
{
	if (pp == NULL || !pp->armed) {
		return -EINVAL;
	}
	(void)profile_private_tick(pp, now_us);

	if (pp->policy == PROFILE_PRIVATE_NEVER ||
	    pp->policy == PROFILE_PRIVATE_ALWAYS) {
		/* `never` has nowhere to go; `always` has no compat channel to
		 * come back to. Both are counted, because a node whose button
		 * is being pressed is a node somebody is trying something on. */
		pp->refused_policy++;
		return -EPERM;
	}
	if (announcing(pp)) {
		return -EBUSY;
	}

	return begin(pp, PROFILE_PRIVATE_REASON_PHYSICAL,
		     pp->state == PROFILE_PRIVATE_STATE_COMPAT, now_us);
}

int profile_private_on_command(struct profile_private *pp, uint64_t now_us,
			       const uint8_t *pay, uint8_t len)
{
	uint64_t min_us;
	uint8_t  op;
	int      rc;

	if (pp == NULL || !pp->armed || pay == NULL ||
	    len != (uint8_t)PROFILE_PRIVATE_CMD_LEN) {
		return -EINVAL;
	}
	(void)profile_private_tick(pp, now_us);

	if (pay[1] != PROFILE_PRIVATE_CMD_FRAME_BYTE) {
		/* Not a one-frame set, so not a command. An enrolment frame
		 * (count six) lands here and is neither consumed nor counted as
		 * an attack. */
		pp->refused_malformed++;
		return -EPROTO;
	}

	/*
	 * THE TAG FIRST, before the operation is even looked at and before the
	 * policy is consulted.
	 *
	 * An unauthenticated over-air request must never reach the state
	 * machine: a stranger on RF 57 who could mute a power meter for every
	 * legacy receiver in the room would be a one-packet denial of service
	 * against everybody, and the sensor cannot switch "just for one
	 * receiver" because it has one stream and N listeners.
	 */
	rc = radiant_sec_compat_cmd_verify(pp->cfg.ch, now_us, pay,
					   (uint8_t)PROFILE_PRIVATE_CMD_COVERED,
					   &pay[PROFILE_PRIVATE_CMD_COVERED]);
	if (rc != RADIANT_SEC_OK) {
		pp->refused_unauth++;
		return -EACCES;
	}

	op = pay[2];
	if (op != PROFILE_PRIVATE_OP_GO_PRIVATE &&
	    op != PROFILE_PRIVATE_OP_RETURN) {
		/*
		 * TWO OPERATIONS EXIST. A policy-change operation is not
		 * deferred here, it is refused: a policy a remote party can
		 * rewrite is not a policy, and downgrading `command` to `never`
		 * over the air would be a mute attack wearing a safety hat. An
		 * unknown operation is counted and dropped rather than reserved
		 * for a later meaning, so a well-tagged message shaped like a
		 * policy change changes nothing.
		 */
		pp->refused_malformed++;
		return -EPROTO;
	}

	if (pp->policy != PROFILE_PRIVATE_COMMAND) {
		/*
		 * A `never` node refuses an OTHERWISE-VALID authenticated
		 * command from a keyholder it trusts, and counts the refusal.
		 * That is what `never` means, and the count is what lets an
		 * owner tell a refusal from a command that never arrived.
		 */
		pp->refused_policy++;
		return -EPERM;
	}
	if (announcing(pp)) {
		return -EBUSY;
	}

	/* Idempotent rather than an error: a keyholder asking a private node to
	 * go private has been answered already, and the retransmission of an
	 * acknowledged message is exactly how that happens. */
	if ((op == PROFILE_PRIVATE_OP_GO_PRIVATE &&
	     pp->state == PROFILE_PRIVATE_STATE_PRIVATE) ||
	    (op == PROFILE_PRIVATE_OP_RETURN &&
	     pp->state == PROFILE_PRIVATE_STATE_COMPAT)) {
		pp->commands_ok++;
		return 0;
	}

	min_us = (uint64_t)(pp->cfg.cmd_min_s != 0u ? pp->cfg.cmd_min_s
						    : PROFILE_PRIVATE_CMD_MIN_S) *
		 US_PER_S;
	if (pp->ever_cmd && (now_us - pp->last_cmd_us) < min_us) {
		pp->refused_rate++;
		return -EAGAIN;
	}

	rc = begin(pp, PROFILE_PRIVATE_REASON_COMMAND,
		   op == PROFILE_PRIVATE_OP_GO_PRIVATE, now_us);
	if (rc != 0) {
		return rc;
	}

	/* Measured from the last ACCEPTED command, so a stream of refusals
	 * cannot extend the floor and a refused one does not reset it. */
	pp->last_cmd_us = now_us;
	pp->ever_cmd = true;
	pp->commands_ok++;
	return 1;
}

int profile_private_host_set_policy(struct profile_private *pp, uint8_t policy)
{
	uint8_t was;
	bool    was_nvm;
	int     rc;

	if (pp == NULL || !pp->armed) {
		return -EINVAL;
	}
	if (!policy_is_known(policy)) {
		return -EINVAL;
	}
	if (pp->state != PROFILE_PRIVATE_STATE_COMPAT) {
		/* A policy that changed under a countdown would change what the
		 * countdown was for, half way through it. */
		return -EBUSY;
	}
	if (pp->cfg.policy_store == NULL) {
		/* No record to write. Refused outright rather than applied in
		 * RAM, because a policy that survives until the next reboot and
		 * then does not is the bug report the precedence rule exists to
		 * prevent. */
		return -ENOTSUP;
	}
	if (!PROFILE_PRIVATE_COMBINATION_OK(policy, pp->cfg.announce,
					    pp->cfg.attest)) {
		return -ENOTSUP;
	}

	if (pp->cfg.policy_store(pp->cfg.user, policy) != 0) {
		return -EIO;
	}

	was = pp->policy;
	was_nvm = pp->policy_from_nvm;

	/*
	 * RE-RESOLVED rather than assigned. The write went to the record, so
	 * the running value comes back out of the record through the same
	 * precedence rule everything else uses - and if a backend accepted a
	 * write and then read back something else, this is where that shows up
	 * rather than three reboots later.
	 */
	resolve_policy(pp);

	if (pp->policy != policy) {
		pp->policy = was;
		pp->policy_from_nvm = was_nvm;
		return -EIO;
	}

	if (pp->policy != PROFILE_PRIVATE_NEVER) {
		int lrc = locator_at(pp, 0u);

		if (lrc != 0) {
			return lrc;
		}
		publish_locator(pp);
	} else {
		pp->locator = 0u;
		pp->locator_attempt = 0u;
		if (publishes(pp)) {
			(void)profile_compat_set_locator(pp->compat, 0u, 0u, 0u);
		}
	}
	return 0;
}

/* ── The clock ──────────────────────────────────────────────────────────── */

bool profile_private_tick(struct profile_private *pp, uint64_t now_us)
{
	if (pp == NULL || !pp->armed) {
		return false;
	}
	pp->now_us = now_us;

	if (pp->state != PROFILE_PRIVATE_STATE_PRIVATE) {
		return false;
	}
	if ((now_us - pp->private_since_us) < max_private_us(pp)) {
		return false;
	}

	/*
	 * THE BOUNDED DURATION. A sensor that silently stays private is
	 * indistinguishable from a dead one, so this revert needs no user, no
	 * command and no keyholder - and it announces itself with reason
	 * `timeout-revert` so a receiver can say why the node came back.
	 */
	pp->reverts_timeout++;
	(void)begin(pp, PROFILE_PRIVATE_REASON_TIMEOUT, false, now_us);
	return true;
}

/* ── The frames ─────────────────────────────────────────────────────────── */

uint8_t profile_private_frame_count(void *user, uint64_t now_us)
{
	struct profile_private *pp = (struct profile_private *)user;

	if (pp == NULL || !pp->armed) {
		return 0u;
	}
	/* The bounded duration expires here as well as in an explicit tick, so
	 * a node whose frames go through profile_compat.c owes nothing extra -
	 * profile_enrol.c's window does the same in the same place. */
	(void)profile_private_tick(pp, now_us);
	pp->now_us = now_us;
	return announcing(pp) ? (uint8_t)PROFILE_PRIVATE_FRAMES : 0u;
}

/*
 * Frame A, bytes [2..7]:
 *
 *   [2]     target device type (bits 6:0)
 *   [3..4]  target device number, LE
 *   [5..6]  target channel period, LE
 *   [7]     bits 7:6 reason, bits 5:0 countdown in promoted beacon intervals
 *
 * The target is where the node is GOING, which is the private locator on the
 * way out and the compat channel's own id on the way back. A RETURN frame that
 * carried the private locator would be telling receivers to follow the node to
 * where they already are.
 */
static bool build_frame_a(struct profile_private *pp, uint8_t *payload)
{
	uint32_t after;
	uint32_t remaining;
	uint8_t  count;
	uint8_t  type;
	uint16_t devnum;
	uint16_t period;

	if (pp->state == PROFILE_PRIVATE_STATE_ANNOUNCING) {
		type = pp->cfg.private_device_type;
		devnum = pp->locator;
		period = pp->cfg.private_period;
	} else {
		type = pp->cfg.compat_device_type;
		devnum = pp->cfg.compat_devnum;
		period = pp->cfg.compat_period;
	}

	/*
	 * THE RE-ANCHOR. `after` is the message count once this frame has gone
	 * out, which is the instant a receiver's own count reaches when it
	 * takes this frame in - so both sides measure the countdown from the
	 * same message. The node then MOVES ITS TARGET to exactly what it just
	 * said, so every receiver that heard this copy retunes on the message
	 * the node leaves on, and one that heard only an earlier copy is at
	 * most seven messages late rather than any amount early.
	 */
	after = pp->msgs + 1u;
	remaining = (pp->switch_at > after) ? (pp->switch_at - after) : 0u;
	/*
	 * ROUNDED UP, not down, and the direction is the whole of the choice.
	 * Rounding down would shorten the countdown by up to seven messages
	 * every time an announcement landed off the quantum - three
	 * announcements and a K of 64 would be a countdown of 43 - and would
	 * leave a receiver that heard only an earlier copy retuning LATE, after
	 * the node had already gone, so it would miss the head of the new
	 * channel. Rounding up costs at most seven messages of countdown and
	 * puts that receiver EARLY instead: it arrives before the node and
	 * loses the tail of the old channel, which is the cheaper end to lose.
	 */
	count = (uint8_t)((remaining + PROFILE_PRIVATE_COUNTDOWN_UNIT - 1u) /
			  PROFILE_PRIVATE_COUNTDOWN_UNIT);
	if (count > PROFILE_PRIVATE_COUNTDOWN_MAX) {
		count = (uint8_t)PROFILE_PRIVATE_COUNTDOWN_MAX;
	}
	pp->switch_at = after + (uint32_t)count * PROFILE_PRIVATE_COUNTDOWN_UNIT;

	payload[0] = (uint8_t)(type & PROFILE_COMPAT_PAGE_MASK);
	payload[1] = (uint8_t)(devnum & 0xFFu);
	payload[2] = (uint8_t)((devnum >> 8) & 0xFFu);
	payload[3] = (uint8_t)(period & 0xFFu);
	payload[4] = (uint8_t)((period >> 8) & 0xFFu);
	payload[5] = (uint8_t)((uint8_t)(pp->reason << 6) | count);

	/* The next message on the air IS this frame, and its transmitted form -
	 * byte [0]'s toggle and byte [1]'s count, neither of which is knowable
	 * here - is what frame B has to tag. */
	pp->capture_next = true;
	return true;
}

static bool build_frame_b(struct profile_private *pp, uint8_t *payload)
{
	uint8_t tag[RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES];

	if (!pp->have_frame_a) {
		/* Frame A has not been on the air since this countdown began, so
		 * there is nothing to tag. Declining the slot costs one frame;
		 * tagging a frame that was never sent would put a verifiable
		 * claim about bytes nobody heard on the air. */
		return false;
	}
	if (radiant_sec_compat_announce_tag(pp->cfg.ch, pp->now_us, pp->frame_a,
					    (uint8_t)PROFILE_COMPAT_FRAME_LEN,
					    tag) != RADIANT_SEC_OK) {
		return false;
	}
	memcpy(payload, tag, sizeof(tag));
	return true;
}

bool profile_private_frame(void *user, uint8_t i, uint8_t *payload)
{
	struct profile_private *pp = (struct profile_private *)user;

	if (pp == NULL || !pp->armed || payload == NULL || !announcing(pp)) {
		return false;
	}
	if (i == (uint8_t)PROFILE_PRIVATE_FRAME_A) {
		return build_frame_a(pp, payload);
	}
	if (i == (uint8_t)PROFILE_PRIVATE_FRAME_B) {
		return build_frame_b(pp, payload);
	}
	return false;
}

void profile_private_sent(void *user, const uint8_t *body)
{
	struct profile_private *pp = (struct profile_private *)user;

	if (pp == NULL || !pp->armed || body == NULL) {
		return;
	}

	pp->msgs++;

	if (pp->capture_next) {
		memcpy(pp->frame_a, body, PROFILE_COMPAT_FRAME_LEN);
		pp->have_frame_a = true;
		pp->capture_next = false;
	}

	if (announcing(pp) && pp->msgs >= pp->switch_at) {
		(void)commit(pp, pp->state == PROFILE_PRIVATE_STATE_ANNOUNCING);
	}
}

/* ── The receiver's half ────────────────────────────────────────────────── */

int profile_private_rx_init(struct profile_private_rx *rx, uint8_t ch,
			    uint64_t period_us)
{
	if (rx == NULL || period_us == 0u) {
		return -EINVAL;
	}
	memset(rx, 0, sizeof(*rx));
	rx->ch = ch;
	rx->period_us = period_us;
	return 0;
}

static int rx_take(struct profile_private_rx *rx, uint8_t which,
		   const uint8_t *body, uint64_t t_sync)
{
	uint8_t count;

	if (which == (uint8_t)PROFILE_PRIVATE_FRAME_A) {
		memcpy(rx->frame_a, body, PROFILE_COMPAT_FRAME_LEN);
		rx->have_a = true;
		/* The count AFTER this frame, which is what the node measured
		 * its countdown from. */
		rx->a_at = rx->msgs;
		return 0;
	}

	if (!rx->have_a) {
		/* Frame B without a frame A. Nothing to verify it against, and
		 * a tag without the bytes it covers is not evidence of
		 * anything. */
		return 0;
	}

	/*
	 * A RECEIVER ACTS ON A SWITCH FRAME ONLY AFTER ITS TAG VERIFIES. An
	 * unverified one is counted and ignored, and the cost of ignoring it is
	 * the re-acquisition path, which is already built and does not depend
	 * on hearing anything.
	 */
	if (radiant_sec_compat_announce_verify(rx->ch, t_sync, rx->frame_a,
					       (uint8_t)PROFILE_COMPAT_FRAME_LEN,
					       &body[2]) != RADIANT_SEC_OK) {
		rx->rejected++;
		rx->have_a = false;
		return -EACCES;
	}

	rx->target_type = (uint8_t)(rx->frame_a[2] & PROFILE_COMPAT_PAGE_MASK);
	rx->target_devnum = (uint16_t)((uint16_t)rx->frame_a[3] |
				       ((uint16_t)rx->frame_a[4] << 8));
	rx->target_period = (uint16_t)((uint16_t)rx->frame_a[5] |
				       ((uint16_t)rx->frame_a[6] << 8));
	rx->target_reason = (uint8_t)(rx->frame_a[7] >> 6);
	count = (uint8_t)(rx->frame_a[7] & PROFILE_PRIVATE_COUNTDOWN_MAX);

	/* ACT ON EXPIRY, NOT ON RECEIPT. A receiver that joined mid-countdown
	 * reads the remaining count out of the frame, so every keyed receiver
	 * retunes on the same message rather than at N independent moments. */
	rx->act_at = rx->a_at + (uint32_t)count * PROFILE_PRIVATE_COUNTDOWN_UNIT;
	rx->armed = true;
	rx->have_a = false;
	rx->verified++;
	return 1;
}

static void rx_count(struct profile_private_rx *rx, uint64_t t_sync)
{
	rx->msgs++;
	rx->last_us = t_sync;
	rx->any = true;
}

int profile_private_rx_announce(struct profile_private_rx *rx, uint8_t which,
				const uint8_t *body, uint8_t len,
				uint64_t t_sync)
{
	if (rx == NULL || body == NULL ||
	    len != (uint8_t)PROFILE_COMPAT_FRAME_LEN ||
	    which > (uint8_t)PROFILE_PRIVATE_FRAME_B) {
		return -EINVAL;
	}
	rx_count(rx, t_sync);
	return rx_take(rx, which, body, t_sync);
}

int profile_private_rx_message(struct profile_private_rx *rx,
			       const uint8_t *body, uint8_t len,
			       uint64_t t_sync)
{
	uint8_t index;
	uint8_t count;

	if (rx == NULL || body == NULL ||
	    len != (uint8_t)PROFILE_COMPAT_FRAME_LEN) {
		return -EINVAL;
	}
	rx_count(rx, t_sync);

	if ((body[0] & PROFILE_COMPAT_PAGE_MASK) !=
	    PROFILE_COMPAT_PAGE_BEACON) {
		return 0;
	}
	index = (uint8_t)(body[1] >> 4);
	count = (uint8_t)((body[1] & 0x0Fu) + 1u);
	if (count != (uint8_t)PROFILE_PRIVATE_SET_FRAMES) {
		/* A two-frame beacon, or an eight-frame enrolment set. Neither
		 * is an announcement and neither is an error. */
		return 0;
	}
	if (index == (uint8_t)PROFILE_PRIVATE_SET_INDEX_A) {
		return rx_take(rx, (uint8_t)PROFILE_PRIVATE_FRAME_A, body,
			       t_sync);
	}
	if (index == (uint8_t)PROFILE_PRIVATE_SET_INDEX_B) {
		return rx_take(rx, (uint8_t)PROFILE_PRIVATE_FRAME_B, body,
			       t_sync);
	}
	return 0;
}

bool profile_private_rx_due(const struct profile_private_rx *rx)
{
	return rx != NULL && rx->armed && rx->msgs >= rx->act_at;
}

int profile_private_rx_target(const struct profile_private_rx *rx,
			      uint8_t *device_type, uint16_t *devnum,
			      uint16_t *period, uint8_t *reason)
{
	if (rx == NULL || !rx->armed) {
		return -EINVAL;
	}
	if (device_type != NULL) {
		*device_type = rx->target_type;
	}
	if (devnum != NULL) {
		*devnum = rx->target_devnum;
	}
	if (period != NULL) {
		*period = rx->target_period;
	}
	if (reason != NULL) {
		*reason = rx->target_reason;
	}
	return 0;
}

void profile_private_rx_clear(struct profile_private_rx *rx)
{
	if (rx != NULL) {
		rx->armed = false;
		rx->have_a = false;
	}
}

int profile_private_rx_locator(const struct profile_private_rx *rx,
			       uint32_t epoch, uint8_t attempt,
			       uint16_t *devnum)
{
	if (rx == NULL || devnum == NULL) {
		return -EINVAL;
	}
	if (radiant_sec_compat_locator(rx->ch, epoch, attempt, devnum) !=
	    RADIANT_SEC_OK) {
		return -ENOTSUP;
	}
	return 0;
}

bool profile_private_rx_lost(const struct profile_private_rx *rx,
			     uint64_t now_us)
{
	uint64_t horizon;

	if (rx == NULL || !rx->any) {
		return false;
	}
	horizon = (uint64_t)PROFILE_PRIVATE_SILENCE_MSGS * rx->period_us;
	return (now_us - rx->last_us) >= horizon;
}

#else /* no security, or no compat attestation */

/*
 * The refusing shape.
 *
 * Without the compat layer there is no K_id, so there is no locator - and a
 * node that cannot say where it would go cannot go. Refusing at init is the
 * honest behaviour and it is also the safe one: the alternative is a node that
 * closes its compat channel and opens a channel nobody can predict.
 */

int profile_private_init(struct profile_private *pp,
			 const struct profile_private_cfg *cfg)
{
	if (pp != NULL) {
		memset(pp, 0, sizeof(*pp));
	}
	(void)cfg;
	return -ENOTSUP;
}

int profile_private_attach(struct profile_private *pp, struct profile_compat *pc)
{
	(void)pp;
	(void)pc;
	return -ENOTSUP;
}

uint8_t profile_private_policy(const struct profile_private *pp)
{
	(void)pp;
	return (uint8_t)PROFILE_PRIVATE_NEVER;
}

bool profile_private_policy_from_nvm(const struct profile_private *pp)
{
	(void)pp;
	return false;
}

uint8_t profile_private_state(const struct profile_private *pp)
{
	(void)pp;
	return (uint8_t)PROFILE_PRIVATE_STATE_COMPAT;
}

bool profile_private_is_private(const struct profile_private *pp)
{
	(void)pp;
	return false;
}

uint16_t profile_private_locator(const struct profile_private *pp)
{
	(void)pp;
	return 0u;
}

int profile_private_next_locator(struct profile_private *pp)
{
	(void)pp;
	return -ENOTSUP;
}

int profile_private_physical_action(struct profile_private *pp, uint64_t now_us)
{
	(void)pp;
	(void)now_us;
	return -ENOTSUP;
}

int profile_private_on_command(struct profile_private *pp, uint64_t now_us,
			       const uint8_t *pay, uint8_t len)
{
	(void)pp;
	(void)now_us;
	(void)pay;
	(void)len;
	return -ENOTSUP;
}

int profile_private_host_set_policy(struct profile_private *pp, uint8_t policy)
{
	(void)pp;
	(void)policy;
	return -ENOTSUP;
}

bool profile_private_tick(struct profile_private *pp, uint64_t now_us)
{
	(void)pp;
	(void)now_us;
	return false;
}

uint8_t profile_private_frame_count(void *user, uint64_t now_us)
{
	(void)user;
	(void)now_us;
	return 0u;
}

bool profile_private_frame(void *user, uint8_t i, uint8_t *payload)
{
	(void)user;
	(void)i;
	(void)payload;
	return false;
}

void profile_private_sent(void *user, const uint8_t *body)
{
	(void)user;
	(void)body;
}

int profile_private_rx_init(struct profile_private_rx *rx, uint8_t ch,
			    uint64_t period_us)
{
	(void)rx;
	(void)ch;
	(void)period_us;
	return -ENOTSUP;
}

int profile_private_rx_message(struct profile_private_rx *rx,
			       const uint8_t *body, uint8_t len,
			       uint64_t t_sync)
{
	(void)rx;
	(void)body;
	(void)len;
	(void)t_sync;
	return -ENOTSUP;
}

int profile_private_rx_announce(struct profile_private_rx *rx, uint8_t which,
				const uint8_t *body, uint8_t len,
				uint64_t t_sync)
{
	(void)rx;
	(void)which;
	(void)body;
	(void)len;
	(void)t_sync;
	return -ENOTSUP;
}

bool profile_private_rx_due(const struct profile_private_rx *rx)
{
	(void)rx;
	return false;
}

int profile_private_rx_target(const struct profile_private_rx *rx,
			      uint8_t *device_type, uint16_t *devnum,
			      uint16_t *period, uint8_t *reason)
{
	(void)rx;
	(void)device_type;
	(void)devnum;
	(void)period;
	(void)reason;
	return -ENOTSUP;
}

void profile_private_rx_clear(struct profile_private_rx *rx)
{
	(void)rx;
}

int profile_private_rx_locator(const struct profile_private_rx *rx,
			       uint32_t epoch, uint8_t attempt,
			       uint16_t *devnum)
{
	(void)rx;
	(void)epoch;
	(void)attempt;
	(void)devnum;
	return -ENOTSUP;
}

bool profile_private_rx_lost(const struct profile_private_rx *rx,
			     uint64_t now_us)
{
	(void)rx;
	(void)now_us;
	return false;
}

#endif /* CONFIG_RADIANT_SEC && CONFIG_RADIANT_SEC_COMPAT */
