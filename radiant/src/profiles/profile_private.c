/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_private.c - Layer C: the private-mode switch.
 *
 * Provenance: docs/radiant-security.md sections 11.5, 11.6 and
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md, both this
 * project's own documents. No ANT+ device profile document was
 * read for this file, no sdk-ant source was consulted, and nothing here derives
 * from libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * The header carries the argument for every decision here; this file carries
 * the bytes and the state machine. Third file in src/profiles/ to include a
 * radiant header (after profile_compat.c and profile_enrol.c): deriving a
 * locator and verifying a command are key operations, and K_id/K_cmd live where
 * the root key does.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <radiant/radiant_sec_compat.h>

#include "profile_compat.h"
#include "profile_private.h"

#if defined(CONFIG_RADIANT_SEC) && defined(CONFIG_RADIANT_SEC_COMPAT)

/*
 * The build-time half of the key dependency: a node with attest off must not
 * build with private_policy != never, except physical + silent. Kconfig's
 * `depends on` blocks it at menuconfig; this BUILD_ASSERT blocks every other
 * route (defconfig fragment, out-of-tree board file, a relaxed dependency).
 * One predicate, shared with the runtime refusal, three enforcement points.
 *
 * PROFILE_PRIVATE_ATTEST_DEFAULT mirrors Tier I's Kconfig default (ON), so an
 * application that does not source src/profiles/Kconfig (e.g. the unit-test
 * image) still gets the table's default rather than "attestation is off".
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
	/* {16, 32, 64, 128}: in bounds and a single bit set. */
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
		/* No root key on this channel means no locator, and -ENOTSUP
		 * (not the module's own numbering) matches profile_enrol.c's
		 * translation for the same case. */
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

/* A silent node publishes neither locator nor pending-switch bit, ever: no
 * announcement frame and no beacon-rate promotion either. */
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
		/* One call; the compat channel's close is inside it. A node
		 * that closed one channel and failed to open the other would
		 * be muted, so on failure this file just refuses to have moved. */
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

	/* The interlock: Layer D's six pubkey frames occupy the indices this
	 * announcement's two would, and a switch closes the channel that
	 * window runs on. Whoever is second is refused; profile_enrol.c's
	 * window_open() is the other side of the same rule. */
	if (pp->compat != NULL &&
	    profile_compat_client_busy(pp->compat, now_us, pp)) {
		pp->refused_busy++;
		return -EBUSY;
	}

	if (pp->cfg.announce == PROFILE_PRIVATE_SILENT) {
		/* `silent`: the channel just closes. No countdown, since a
		 * countdown with nothing to carry it only widens the window
		 * an observer sees between one channel stopping and another
		 * starting. */
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

/* NVM if provisioned, else Kconfig - the one precedence rule, applied here so
 * the host arm below can re-run it rather than re-implement it. */
static void resolve_policy(struct profile_private *pp)
{
	uint8_t stored = 0u;

	pp->policy = pp->cfg.policy;
	pp->policy_from_nvm = false;

	if (pp->cfg.policy_load == NULL) {
		return;
	}
	if (pp->cfg.policy_load(pp->cfg.user, &stored) != 0) {
		/* Not provisioned - ordinary, not an error. */
		return;
	}
	if (!policy_is_known(stored)) {
		/* Unrecognised record: fall back to the compile-time value. */
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

	/* The runtime arm of the key dependency: catches a policy that
	 * arrived from NVM on a node built for a different one, which the
	 * BUILD_ASSERT cannot see. */
	if (!PROFILE_PRIVATE_COMBINATION_OK(pp->policy, pp->cfg.announce,
					    pp->cfg.attest)) {
		return -ENOTSUP;
	}

	pp->state = PROFILE_PRIVATE_STATE_COMPAT;

	if (pp->policy == PROFILE_PRIVATE_NEVER) {
		/* No locator: nowhere to go, and the beacon must carry zero.
		 * profile_compat.c refuses a `never` node with a nonzero one. */
		pp->armed = true;
		return 0;
	}

	rc = locator_at(pp, 0u);
	if (rc != 0) {
		return rc;
	}
	pp->armed = true;

	if (pp->policy == PROFILE_PRIVATE_ALWAYS) {
		/* Straight into PRIVATE, never through COMPAT - today's
		 * radiant_sec node. Nothing is announced because there was
		 * never a compat channel with listeners on it. */
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

	/* A broadcast announcement is frames 2 and 3 of the beacon page's set,
	 * so advertise = off has no set for them to be frames of. Refused
	 * here rather than discovered at the switch, when the compat channel
	 * is already closing. Such a node can still switch silently. */
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
		 * return to. Both refusals are counted. */
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
		 * (count six) lands here uncounted as an attack. */
		pp->refused_malformed++;
		return -EPROTO;
	}

	/* The tag first, before the operation or the policy is even looked
	 * at: an unauthenticated request must never reach the state machine,
	 * or a stranger on RF 57 could mute the sensor for every listener. */
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
		/* Only two operations exist. A policy-change operation is
		 * refused, not deferred: a policy a remote party can rewrite
		 * is not a policy. An unknown op is counted and dropped. */
		pp->refused_malformed++;
		return -EPROTO;
	}

	if (pp->policy != PROFILE_PRIVATE_COMMAND) {
		/* A `never` node refuses an otherwise-valid authenticated
		 * command and counts the refusal - that is what `never` means. */
		pp->refused_policy++;
		return -EPERM;
	}
	if (announcing(pp)) {
		return -EBUSY;
	}

	/* Idempotent rather than an error: this is the retransmission of an
	 * already-acknowledged message. */
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

	/* Measured from the last accepted command, so a stream of refusals
	 * cannot extend the floor. */
	pp->last_cmd_us = now_us;
	pp->ever_cmd = true;
	pp->commands_ok++;
	return 1;
}

int profile_private_host_set_policy(struct profile_private *pp, uint8_t policy)
{
	uint8_t was;
	bool    was_nvm;

	if (pp == NULL || !pp->armed) {
		return -EINVAL;
	}
	if (!policy_is_known(policy)) {
		return -EINVAL;
	}
	if (pp->state != PROFILE_PRIVATE_STATE_COMPAT) {
		/* A policy changed mid-countdown would change what the
		 * countdown was for, half way through it. */
		return -EBUSY;
	}
	if (pp->cfg.policy_store == NULL) {
		/* No record to write; refused rather than applied only in RAM. */
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

	/* Re-resolved rather than assigned, through the same precedence rule -
	 * so a backend that read back something other than what it wrote
	 * shows up here rather than three reboots later. */
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

	/* The bounded duration: a sensor that silently stays private is
	 * indistinguishable from a dead one, so this revert needs no user,
	 * command or keyholder, and announces reason `timeout-revert`. */
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
	/* The bounded duration also expires here, so a node whose frames go
	 * through profile_compat.c owes nothing extra. */
	(void)profile_private_tick(pp, now_us);
	pp->now_us = now_us;
	return announcing(pp) ? (uint8_t)PROFILE_PRIVATE_FRAMES : 0u;
}

/*
 * Frame A, bytes [2..7]:
 *   [2]     target device type (bits 6:0)
 *   [3..4]  target device number, LE
 *   [5..6]  target channel period, LE
 *   [7]     bits 7:6 reason, bits 5:0 countdown in promoted beacon intervals
 *
 * The target is where the node is going: the private locator outbound, the
 * compat channel's own id on return (not the private locator again).
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

	/* The re-anchor: `after` is the message count once this frame has
	 * gone out - the same instant a receiver's count reaches on taking it
	 * in, so both sides measure from the same message. The node then
	 * moves its target to exactly what it just said, so a receiver that
	 * heard only an earlier copy is at most seven messages late. */
	after = pp->msgs + 1u;
	remaining = (pp->switch_at > after) ? (pp->switch_at - after) : 0u;
	/* Rounded up, not down: rounding down would shorten the countdown by
	 * up to seven messages per off-quantum announcement and leave a
	 * receiver retuning LATE, after the node had already gone. Rounding
	 * up costs at most seven messages and puts it EARLY instead, losing
	 * only the tail of the old channel - the cheaper end to lose. */
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

	/* The next message on the air IS this frame; its transmitted form -
	 * byte [0]'s toggle, byte [1]'s count, neither knowable here - is what
	 * frame B has to tag. */
	pp->capture_next = true;
	return true;
}

static bool build_frame_b(struct profile_private *pp, uint8_t *payload)
{
	uint8_t tag[RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES];

	if (!pp->have_frame_a) {
		/* Frame A has not been on the air this countdown, so there is
		 * nothing to tag. */
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
		/* The count after this frame, which is what the node measured
		 * its countdown from. */
		rx->a_at = rx->msgs;
		return 0;
	}

	if (!rx->have_a) {
		/* Frame B without a frame A: nothing to verify it against. */
		return 0;
	}

	/* Act on a switch frame only after its tag verifies. An unverified
	 * one is counted and ignored; the re-acquisition path covers it. */
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

	/* Act on expiry, not receipt: a receiver joining mid-countdown reads
	 * the remaining count out of the frame, so every keyed receiver
	 * retunes on the same message. */
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
		/* A two-frame beacon or an eight-frame enrolment set - neither
		 * an announcement nor an error. */
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
 * The refusing shape. Without the compat layer there is no K_id, so there is
 * no locator, and a node that cannot say where it would go cannot go. Refusing
 * at init is the safe alternative to a node that closes its compat channel and
 * opens one nobody can predict.
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
