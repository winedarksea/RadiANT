/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_private.c - compat-C8's gate: the node stops being an ANT+
 * sensor, and everybody who holds its key follows.
 *
 * Provenance: docs/radiant-security.md sections 11.5-11.6 and
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md, plus the
 * locator/command vectors tools/test_radiant_crypto.py pins independently. No
 * ANT+ profile document was read; nothing derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * Catches the 1:N failure a single-head-unit bench cannot see: one keyholder
 * sends the switch command, and all three receivers (only one of which asked)
 * must retune on the SAME message, because receivers act on countdown expiry
 * rather than on receipt.
 *
 * Real: every byte on air, every tag, every countdown, the beacon frame set,
 * the derived locator, policy resolution and every refusal - a genuine
 * profile_hr.c stream with profile_compat.c inserting pages, and receivers are
 * profile_private.c's own receiver half. Simulated: the radio (no channel is
 * opened) and the private channel's synthetic 8-byte bodies.
 *
 * The one modelled quantity is the retune cost: RETUNE_MSGS message periods,
 * the same constant in both modes, so the broadcast-vs-silent gap difference
 * measures the announcement's cost alone.
 */

#include <zephyr/ztest.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_sec.h>
#include <radiant_core/radiant_sec_compat.h>
#include <radiant_core/radiant_channel.h>

#include "fake_radio.h"
#include "profile_common.h"
#include "profile_compat.h"
#include "profile_hr.h"
#include "profile_private.h"
#include "profile_sched.h"

#if defined(CONFIG_RADIANT_SEC_COMPAT)

/* ── The cast ───────────────────────────────────────────────────────────── */

#define NODE_CH 0u
#define RX_A    1u   /* the receiver that sends the command */
#define RX_B    2u   /* two that do not */
#define RX_C    3u
#define FOLLOWERS 3u

#define NODE_DEVNUM 0x2C41u

/* The same root and epoch test_profile_compat.c and test_profile_enrol.c pin,
 * so a capture from any of the three suites is a stream under one key - and so
 * the locator vectors below are the ones tools/test_radiant_crypto.py pins
 * against the same K_id. */
static const uint8_t compat_root[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

#define TEST_EPOCH 7u
#define TEST_T_S   RADIANT_SEC_COMPAT_T_DEFAULT_S
#define TEST_N     RADIANT_SEC_COMPAT_N_DEFAULT

#define TX_SWITCHES RADIANT_SEC_COMPAT_SW_TIER_I
#define RX_SWITCHES ((uint8_t)(RADIANT_SEC_COMPAT_SW_TIER_I | \
			       RADIANT_SEC_COMPAT_SW_PIN))

/* Device type 0x60 is the RadiANT telemetry envelope's, which is where a
 * private node goes. Written out rather than included, because this suite must
 * not depend on the envelope's headers to assert a device type appearing in a
 * frame. */
#define PRIVATE_DEVICE_TYPE 0x60u
#define PRIVATE_PERIOD      8192u

#define T_ORIGIN ((uint64_t)0x100000000ull + 4321u)
#define US_PER_S 1000000u

#define MAX_STEPS 512u

static uint64_t period_us(uint16_t counts)
{
	return ((uint64_t)counts * US_PER_S) / 32768u;
}

static uint64_t at(uint32_t step)
{
	return T_ORIGIN + (uint64_t)step * period_us(PROFILE_HR_PERIOD);
}

/* ── The node ───────────────────────────────────────────────────────────── */

static struct profile_hr      g_hr;
static struct profile_compat  g_pc;
static struct profile_private g_pp;

static uint32_t g_enter_calls;
static uint32_t g_leave_calls;
static uint8_t  g_entered_type;
static uint16_t g_entered_devnum;
static uint16_t g_entered_period;
static int      g_enter_rc;

static int on_enter(void *user, uint8_t device_type, uint16_t devnum,
		    uint16_t period)
{
	ARG_UNUSED(user);
	if (g_enter_rc != 0) {
		return g_enter_rc;
	}
	g_enter_calls++;
	g_entered_type = device_type;
	g_entered_devnum = devnum;
	g_entered_period = period;
	return 0;
}

static int on_leave(void *user)
{
	ARG_UNUSED(user);
	g_leave_calls++;
	return 0;
}

/* The policy's persistent surface. `g_nvm_has` false is a node nobody has
 * provisioned, which is the ordinary state and not an error. */
static bool    g_nvm_has;
static uint8_t g_nvm_policy;
static int     g_nvm_store_rc;

static int policy_load(void *user, uint8_t *policy)
{
	ARG_UNUSED(user);
	if (!g_nvm_has) {
		return -ENOENT;
	}
	*policy = g_nvm_policy;
	return 0;
}

static int policy_store(void *user, uint8_t policy)
{
	ARG_UNUSED(user);
	if (g_nvm_store_rc != 0) {
		return g_nvm_store_rc;
	}
	g_nvm_has = true;
	g_nvm_policy = policy;
	return 0;
}

/* ── The receivers ──────────────────────────────────────────────────────── */

/*
 * A retune costs one message period. THE SAME CONSTANT IN BOTH MODES, so the
 * measured difference between broadcast and silent is entirely made of the
 * announcement and none of it is made of this number.
 */
#define RETUNE_MSGS 1u

struct follower {
	struct profile_private_rx rx;
	bool     hears_announcements;  /* false models a receiver that missed
					* every copy */
	bool     retuning;
	bool     retune_to_private;
	uint32_t retune_until;         /* stream index it is deaf until */
	bool     on_private;
	bool     saw_target;
	uint8_t  target_type;
	uint16_t target_devnum;
	uint32_t due_at;               /* stream index where the countdown
					* expired for this receiver */
	uint32_t last_read;            /* stream index of its last good read */
	uint32_t first_private;        /* ...and of its first private read */
	uint32_t announcements;
};

static struct follower g_f[FOLLOWERS];

static const uint8_t follower_ch[FOLLOWERS] = { RX_A, RX_B, RX_C };

/* ── The run engine ─────────────────────────────────────────────────────── */

static uint32_t g_stream;        /* transmitted messages so far, both channels */
static uint32_t g_switch_msg;    /* the message index the node switched on */
static bool     g_switched;
static uint32_t g_return_msg;
static bool     g_returned;
static uint32_t g_priv_msgs;
static uint8_t  g_priv_cursor;

/* A page number on device type 0x60 for the RETURN frames. THE ALLOCATION IS
 * DEFERRED - profile_enrol.h records the same deferral for the same six frames
 * of a pairing set, because a page number in the telemetry envelope's own
 * namespace is an envelope change. This suite picks one so it can drive the
 * return countdown; nothing in profile_private.c depends on the value. */
#define PRIVATE_PAGE 0x40u

static bool g_log_enable;
static uint8_t  g_log[MAX_STEPS][8];
static uint32_t g_log_n;

static void log_put(const uint8_t *body)
{
	if (g_log_enable && g_log_n < MAX_STEPS) {
		memcpy(g_log[g_log_n++], body, 8);
	}
}

/* One receiver, one message; a receiver not on `node_private`'s channel hears
 * nothing at all. */
static void deliver(struct follower *f, const uint8_t *body, uint64_t now,
		    int announce_frame, bool node_private)
{
	int rc;

	if (f->retuning) {
		if (g_stream < f->retune_until) {
			return;   /* deaf: the channel is being opened */
		}
		f->retuning = false;
		f->on_private = f->retune_to_private;
	}
	if (f->on_private != node_private) {
		return;
	}
	if (announce_frame >= 0 && !f->hears_announcements) {
		/* A LOST PACKET, modelled as a lost packet: this receiver
		 * simply never sees the announcement's frames. Everything else
		 * in the stream still reaches it, so its silence detection is
		 * honest. */
		return;
	}

	if (announce_frame >= 0) {
		rc = profile_private_rx_announce(&f->rx, (uint8_t)announce_frame,
						 body, 8u, now);
	} else {
		rc = profile_private_rx_message(&f->rx, body, 8u, now);
	}
	if (rc == 1) {
		f->announcements++;
	}
	f->last_read = g_stream;

	if (f->on_private && f->first_private == UINT32_MAX) {
		f->first_private = g_stream;
	}
}

/*
 * One message of the node's life, on whichever channel it is on. The compat
 * half is a real profile_hr.c stream; the private half is synthetic (no radio)
 * but still drives profile_private_sent()/profile_private_frame(), so the
 * countdown and tag logic are the shipped code either way.
 */
static void step(void)
{
	uint64_t now = at(g_stream);
	uint8_t  body[8];
	int      announce_frame = -1;
	uint32_t i;
	bool     was_private = profile_private_is_private(&g_pp);

	if (!was_private) {
		enum profile_slot_kind kind;

		if ((g_stream % 4u) == 0u) {
			profile_hr_beat(&g_hr, (uint16_t)((g_stream / 4u) * 1024u));
		}
		profile_hr_set_operating_time(&g_hr, g_stream / 4u);
		kind = profile_hr_next(&g_hr, now, body);
		zassert_not_equal(PROFILE_SLOT_IDLE, kind,
				  "the strap went silent at message %u",
				  g_stream);

		/* Is this one of the announcement's frames? Decided the way a
		 * receiver decides it - from byte [0] and byte [1] - so the
		 * test is reading the air rather than the node's state. */
		if ((body[0] & PROFILE_COMPAT_PAGE_MASK) ==
			    PROFILE_COMPAT_PAGE_BEACON &&
		    ((body[1] & 0x0Fu) + 1u) == PROFILE_PRIVATE_SET_FRAMES) {
			uint8_t idx = (uint8_t)(body[1] >> 4);

			if (idx == PROFILE_PRIVATE_SET_INDEX_A) {
				announce_frame = PROFILE_PRIVATE_FRAME_A;
			} else if (idx == PROFILE_PRIVATE_SET_INDEX_B) {
				announce_frame = PROFILE_PRIVATE_FRAME_B;
			}
		}
	} else {
		/* The private channel: a device type 0x60 master driving the
		 * same three client entry points from its own rotation. */
		memset(body, 0, sizeof(body));
		body[0] = 0x01u;   /* an ordinary telemetry page */
		body[1] = (uint8_t)(g_priv_msgs & 0xFFu);

		if ((g_priv_msgs % PROFILE_PRIVATE_COUNTDOWN_UNIT) == 0u &&
		    profile_private_frame_count(&g_pp, now) ==
			    PROFILE_PRIVATE_FRAMES) {
			uint8_t which = (uint8_t)(g_priv_cursor % 2u);
			uint8_t frame[8];

			memset(frame, 0, sizeof(frame));
			frame[0] = PRIVATE_PAGE;
			frame[1] = (uint8_t)(((PROFILE_PRIVATE_SET_INDEX_A +
					       which) << 4) |
					     (PROFILE_PRIVATE_SET_FRAMES - 1u));
			if (profile_private_frame(&g_pp, which, &frame[2])) {
				memcpy(body, frame, sizeof(body));
				announce_frame = (int)which;
				g_priv_cursor++;
			}
		}
		(void)profile_private_tick(&g_pp, now);
		profile_private_sent(&g_pp, body);
		g_priv_msgs++;
	}

	log_put(body);

	/* The transition happens INSIDE profile_private_sent(), so this index is
	 * the one every receiver's countdown should also expire on. */
	if (!g_switched && profile_private_is_private(&g_pp)) {
		g_switched = true;
		g_switch_msg = g_stream;
	}
	if (g_switched && !g_returned && !profile_private_is_private(&g_pp)) {
		g_returned = true;
		g_return_msg = g_stream;
	}

	for (i = 0u; i < FOLLOWERS; i++) {
		struct follower *f = &g_f[i];

		deliver(f, body, now, announce_frame, was_private);

		/* Did the countdown it heard just expire? */
		if (!f->saw_target && profile_private_rx_due(&f->rx)) {
			uint8_t  type = 0u;
			uint16_t devnum = 0u;
			uint16_t per = 0u;
			uint8_t  reason = 0u;

			zassert_equal(0, profile_private_rx_target(&f->rx, &type,
								   &devnum, &per,
								   &reason));
			f->saw_target = true;
			f->target_type = type;
			f->target_devnum = devnum;
			f->due_at = g_stream;
			f->retuning = true;
			f->retune_to_private = profile_private_is_private(&g_pp);
			f->retune_until = g_stream + 1u + RETUNE_MSGS;
			profile_private_rx_clear(&f->rx);
		} else if (!f->saw_target && !f->retuning &&
			   f->on_private != profile_private_is_private(&g_pp) &&
			   profile_private_rx_lost(&f->rx, now)) {
			/* Nothing arrived for 8 message periods: the receiver
			 * derives the locator from its own root/epoch and
			 * retunes, with no announcement involved. */
			uint16_t devnum = 0u;

			zassert_equal(0, profile_private_rx_locator(&f->rx,
								    TEST_EPOCH,
								    0u, &devnum));
			f->saw_target = true;
			f->target_type = PRIVATE_DEVICE_TYPE;
			f->target_devnum = devnum;
			f->due_at = g_stream;
			f->retuning = true;
			f->retune_to_private = profile_private_is_private(&g_pp);
			f->retune_until = g_stream + 1u + RETUNE_MSGS;
		}
	}

	g_stream++;
}

static void run(uint32_t n)
{
	uint32_t i;

	for (i = 0u; i < n; i++) {
		zassert_true(g_stream < MAX_STEPS, "ran off the end of the log");
		step();
	}
}

/* ── Fixtures ───────────────────────────────────────────────────────────── */

static void keys_up(void)
{
	uint8_t i;

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(NODE_CH, compat_root, 128,
						 NODE_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_devnum(NODE_CH, NODE_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(NODE_CH, TX_SWITCHES, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(NODE_CH, TEST_EPOCH, T_ORIGIN,
						   0u, PROFILE_HR_PERIOD));

	for (i = 0u; i < FOLLOWERS; i++) {
		uint8_t ch = follower_ch[i];

		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_set_key(ch, compat_root, 128,
							 NODE_DEVNUM));
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_set_devnum(ch, NODE_DEVNUM));
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_configure(ch, RX_SWITCHES,
							   TEST_N, TEST_T_S));
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_set_epoch(ch, TEST_EPOCH,
							   T_ORIGIN, 0u,
							   PROFILE_HR_PERIOD));
	}
}

static void hr_up(void)
{
	struct profile_hr_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id.hw_revision = 1u;
	cfg.id.manufacturer_id = 0x00FFu;
	cfg.id.model_number = 0x0001u;
	cfg.id.sw_revision_main = 1u;
	cfg.id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg.id.serial_number = PROFILE_COMMON_INVALID_U32;
	cfg.manufacturer_id_8 = 0xFFu;
	cfg.serial_upper16 = 0x4553u;
	cfg.model_number_8 = 1u;
	cfg.hw_version = 1u;
	cfg.sw_version = 1u;

	zassert_equal(0, profile_hr_init(&g_hr, &cfg));
	profile_hr_set_computed(&g_hr, 60u);
}

static void compat_up(uint8_t policy)
{
	struct profile_compat_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.ch = NODE_CH;
	cfg.epoch = TEST_EPOCH;
	cfg.advertise = true;
	cfg.attest_available = true;
	cfg.policy = policy;
	cfg.window = TEST_N;
	cfg.mode = PROFILE_COMPAT_MODE_FIXED;
	/* No locator here: profile_private.c publishes it, because the number
	 * is derived and this layer holds no key. */

	zassert_equal(0, profile_compat_init(&g_pc, &cfg));
	zassert_equal(0, profile_hr_set_compat(&g_hr, &g_pc));
}

static void private_cfg_fill(struct profile_private_cfg *cfg, uint8_t policy,
			     uint8_t announce, uint16_t k)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->ch = NODE_CH;
	cfg->epoch = TEST_EPOCH;
	cfg->policy = policy;
	cfg->announce = announce;
	cfg->attest = true;
	cfg->k = k;
	cfg->private_device_type = PRIVATE_DEVICE_TYPE;
	cfg->private_period = PRIVATE_PERIOD;
	cfg->compat_device_type = PROFILE_HR_DEVICE_TYPE;
	cfg->compat_devnum = NODE_DEVNUM;
	cfg->compat_period = PROFILE_HR_PERIOD;
	cfg->policy_load = policy_load;
	cfg->policy_store = policy_store;
	cfg->enter_private = on_enter;
	cfg->leave_private = on_leave;
}

static void private_up(uint8_t policy, uint8_t announce, uint16_t k)
{
	struct profile_private_cfg cfg;

	private_cfg_fill(&cfg, policy, announce, k);
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(0, profile_private_attach(&g_pp, &g_pc));
}

static void followers_up(void)
{
	uint8_t i;

	for (i = 0u; i < FOLLOWERS; i++) {
		memset(&g_f[i], 0, sizeof(g_f[i]));
		g_f[i].hears_announcements = true;
		g_f[i].first_private = UINT32_MAX;
		zassert_equal(0, profile_private_rx_init(&g_f[i].rx,
							 follower_ch[i],
							 period_us(PROFILE_HR_PERIOD)));
	}
}

/* Everything a switch test needs: keys, a strap, a beacon, a policy and three
 * keyed receivers. */
static void node_up(uint8_t policy, uint8_t announce, uint16_t k)
{
	keys_up();
	hr_up();
	compat_up(policy);
	private_up(policy, announce, k);
	followers_up();
}

static void private_before(void *f)
{
	ARG_UNUSED(f);
	radiant_sec_compat_reset();
	radiant_sec_reset();
	fake_radio_reset();
	radiant_channel_init();

	memset(&g_pc, 0, sizeof(g_pc));
	memset(&g_pp, 0, sizeof(g_pp));
	memset(&g_hr, 0, sizeof(g_hr));
	memset(g_f, 0, sizeof(g_f));

	g_enter_calls = 0u;
	g_leave_calls = 0u;
	g_enter_rc = 0;
	g_entered_type = 0u;
	g_entered_devnum = 0u;
	g_entered_period = 0u;
	g_nvm_has = false;
	g_nvm_policy = 0u;
	g_nvm_store_rc = 0;
	g_stream = 0u;
	g_switched = false;
	g_switch_msg = 0u;
	g_returned = false;
	g_return_msg = 0u;
	g_priv_msgs = 0u;
	g_priv_cursor = 0u;
	g_log_n = 0u;
	g_log_enable = false;
}

ZTEST_SUITE(profile_private, NULL, NULL, private_before, NULL, NULL);

/* ═══ The load-bearing test ══════════════════════════════════════════════ */

/*
 * Build the command one keyholder sends. Signed with RX_A's own context, which
 * is the point: a receiver holding the group root can build this, and the node
 * verifies it under a context that was never told anything about RX_A.
 */
static void build_command(uint8_t ch, uint8_t op, uint64_t now, uint8_t out[8])
{
	memset(out, 0, 8);
	out[0] = PROFILE_COMPAT_PAGE_BEACON;
	out[1] = PROFILE_PRIVATE_CMD_FRAME_BYTE;
	out[2] = op;
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_cmd_tag(ch, now, out,
						 PROFILE_PRIVATE_CMD_COVERED,
						 &out[PROFILE_PRIVATE_CMD_COVERED]));
}

ZTEST(profile_private, test_three_keyed_receivers_follow_one_switch)
{
	uint8_t  cmd[8];
	uint32_t i;

	/* THE WHOLE OF LAYER C IN ONE TEST: one of three keyed receivers sends
	 * an authenticated command, and ~16 s later all three are on the
	 * private channel, having left on the same message. */
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);

	/* Let the stream settle so the receivers are anchored and verifying. */
	run(24u);
	zassert_equal(PROFILE_PRIVATE_STATE_COMPAT,
		      profile_private_state(&g_pp));

	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, at(g_stream), cmd,
						    sizeof(cmd)),
		      "an authenticated command from a paired keyholder was not "
		      "accepted by a `command` node");
	zassert_equal(PROFILE_PRIVATE_STATE_ANNOUNCING,
		      profile_private_state(&g_pp));

	run(96u);

	/* ── The node went ──────────────────────────────────────────────── */
	zassert_true(g_switched, "the countdown never expired");
	zassert_equal(1u, g_enter_calls);
	zassert_equal(PRIVATE_DEVICE_TYPE, g_entered_type);
	zassert_equal(PRIVATE_PERIOD, g_entered_period);
	zassert_equal(profile_private_locator(&g_pp), g_entered_devnum,
		      "the node opened a channel on a number that is not its "
		      "derived locator");

	/* ── ...and all three followed, on the same message ─────────────── */
	for (i = 0u; i < FOLLOWERS; i++) {
		zassert_true(g_f[i].saw_target,
			     "receiver %u never learned where the node went", i);
		zassert_true(g_f[i].announcements > 0u,
			     "receiver %u verified no announcement", i);
		zassert_equal(g_entered_devnum, g_f[i].target_devnum,
			      "receiver %u followed to the wrong device number",
			      i);
		zassert_equal(PRIVATE_DEVICE_TYPE, g_f[i].target_type);
	}
	zassert_equal(g_f[0].due_at, g_f[1].due_at,
		      "the receiver that sent the command retuned on message %u "
		      "and one that did not retuned on message %u; that is the "
		      "1:N failure this feature is most likely to ship with",
		      g_f[0].due_at, g_f[1].due_at);
	zassert_equal(g_f[0].due_at, g_f[2].due_at,
		      "receivers %u and %u retuned on different messages",
		      0u, 2u);

	/* ...and on the message the node itself left on: both act on the same
	 * countdown expiry, so these are one number, not two that are close. */
	zassert_equal(g_switch_msg, g_f[0].due_at,
		      "the node left on message %u and its receivers retuned on "
		      "message %u", g_switch_msg, g_f[0].due_at);

	/* The countdown was as long as it said it was: K messages, less the at
	 * most seven the re-anchoring gives up to land on the quantum. */
	zassert_true(g_switch_msg >= 24u + PROFILE_PRIVATE_K_DEFAULT -
					 PROFILE_PRIVATE_COUNTDOWN_UNIT,
		     "the countdown was %u messages, K is %u",
		     g_switch_msg - 24u, PROFILE_PRIVATE_K_DEFAULT);
	zassert_true(g_switch_msg <= 24u + PROFILE_PRIVATE_K_DEFAULT + 8u,
		     "the countdown overran: %u messages against K = %u",
		     g_switch_msg - 24u, PROFILE_PRIVATE_K_DEFAULT);
}

ZTEST(profile_private, test_a_receiver_joining_mid_countdown_follows)
{
	uint8_t  cmd[8];
	uint32_t joined_at;

	/* A receiver that joins mid-countdown reads the REMAINING count out of
	 * the frame rather than needing the first one - "act on expiry, not
	 * receipt" is what buys this. */
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);

	/* Receiver C is not in the room yet. */
	g_f[2].on_private = true;   /* hears nothing on the compat channel */

	run(16u);
	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, at(g_stream), cmd,
						    sizeof(cmd)));

	/* Half way through the countdown, C switches on. */
	run(32u);
	joined_at = g_stream;
	g_f[2].on_private = false;
	zassert_equal(0, profile_private_rx_init(&g_f[2].rx, RX_C,
						 period_us(PROFILE_HR_PERIOD)));

	run(96u);

	zassert_true(g_switched);
	zassert_true(g_f[2].saw_target,
		     "the receiver that joined at message %u never followed",
		     joined_at);
	zassert_true(g_f[2].announcements > 0u,
		     "the late joiner verified no announcement at all, so it "
		     "cannot have read a remaining count");
	zassert_equal(g_f[0].due_at, g_f[2].due_at,
		      "the receiver that joined mid-countdown retuned on "
		      "message %u and the ones that heard the whole countdown "
		      "retuned on message %u", g_f[2].due_at, g_f[0].due_at);
	zassert_equal(g_switch_msg, g_f[2].due_at);
}

/* ═══ The derived locator ════════════════════════════════════════════════ */

ZTEST(profile_private, test_the_locator_agrees_with_the_python_mirror)
{
	uint16_t devnum = 0u;
	uint8_t  attempt;

	/* tools/test_radiant_crypto.py's TestCompatLocator pins these same four
	 * numbers independently under the same K_id: a node and a keyholder
	 * that derive the locator differently would never meet, silently. */
	static const uint16_t expected[4] = { 0xBB18u, 0xB4B5u, 0x8A57u,
					      0x472Cu };

	keys_up();

	for (attempt = 0u; attempt < 4u; attempt++) {
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_locator(NODE_CH, TEST_EPOCH,
							 attempt, &devnum));
		zassert_equal(expected[attempt], devnum,
			      "candidate %u is 0x%04X, the Python mirror says "
			      "0x%04X", attempt, devnum, expected[attempt]);
	}

	/* Every candidate a searching keyholder tries agrees, on the receiver's
	 * context as well as the node's - they are different contexts holding
	 * the same root, which is the only reason any of this works. */
	for (attempt = 0u; attempt < RADIANT_SEC_COMPAT_LOCATOR_TRIES;
	     attempt++) {
		uint16_t theirs = 0u;

		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_locator(RX_C, TEST_EPOCH,
							 attempt, &theirs));
		zassert_equal(expected[attempt], theirs);
	}
}

ZTEST(profile_private, test_the_wildcard_is_excluded_and_the_suffix_takes_over)
{
	uint16_t devnum = 0u;

	/* Epoch 1315 is not arbitrary: under this root's K_id the unsuffixed
	 * derivation there comes out 0x0000 (the ANT wildcard) - the one epoch
	 * in ~65000 where the exclusion rule does anything. Pinned the same way
	 * in tools/test_radiant_crypto.py. */
	keys_up();

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_locator(NODE_CH, 1315u, 0u, &devnum));
	zassert_equal(0x50E2u, devnum,
		      "the excluded wildcard was not skipped: 0x%04X", devnum);
	zassert_not_equal(RADIANT_SEC_COMPAT_LOCATOR_WILDCARD, devnum);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_locator(NODE_CH, 1315u, 1u, &devnum));
	zassert_equal(0xDEABu, devnum);
}

ZTEST(profile_private, test_a_node_walks_the_candidates_on_a_collision)
{
	uint16_t first;
	uint16_t second;
	int      rc;
	uint8_t  i;

	/* A node learns of a collision by opening the channel and finding it
	 * occupied, so it walks exactly as far as a searching keyholder looks,
	 * then stops. */
	node_up(PROFILE_PRIVATE_PHYSICAL, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);

	first = profile_private_locator(&g_pp);
	zassert_equal(0, profile_private_next_locator(&g_pp));
	second = profile_private_locator(&g_pp);
	zassert_not_equal(first, second);

	for (i = 2u; i < PROFILE_PRIVATE_LOCATOR_TRIES; i++) {
		zassert_equal(0, profile_private_next_locator(&g_pp));
	}
	rc = profile_private_next_locator(&g_pp);
	zassert_equal(-ENOSPC, rc,
		      "the walk is bounded at %u candidates",
		      PROFILE_PRIVATE_LOCATOR_TRIES);
}

ZTEST(profile_private, test_a_receiver_that_missed_the_announcement_reacquires)
{
	uint8_t cmd[8];

	/* Receiver C hears the stream but misses every copy of the
	 * announcement; it notices the silence, derives the locator from its
	 * own root/epoch, and arrives where the node is. */
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);
	g_f[2].hears_announcements = false;

	run(16u);
	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, at(g_stream), cmd,
						    sizeof(cmd)));
	run(160u);

	zassert_true(g_switched);
	zassert_equal(0u, g_f[2].announcements,
		      "the receiver that was supposed to miss the announcement "
		      "verified one");
	zassert_true(g_f[2].saw_target,
		     "a keyed receiver that missed the announcement never "
		     "re-acquired; the derived locator is the whole reason the "
		     "announcement is an optimisation rather than the "
		     "mechanism");
	zassert_equal(g_entered_devnum, g_f[2].target_devnum,
		      "it re-acquired on the wrong number");
	zassert_true(g_f[2].due_at > g_f[0].due_at,
		     "re-acquiring by search should cost more than following an "
		     "announcement, or the announcement is doing nothing");
}

/* ═══ broadcast against silent, measured ═════════════════════════════════ */

ZTEST(profile_private, test_silent_says_nothing_and_everyone_still_reacquires)
{
	uint32_t broadcast_gap[FOLLOWERS];
	uint32_t silent_gap[FOLLOWERS];
	uint32_t i;
	uint8_t  cmd[8];

	/*
	 * THE NEGATIVE CLAIM AND THE MEASURED COST, in one test. With
	 * announce = silent, nothing about the switch appears on the air (no
	 * announcement frame, no locator, no pending-switch bit) - the whole
	 * capture is checked for all of those. All three receivers still
	 * arrive, at a cost this test MEASURES rather than asserts is finite.
	 */

	/* ── First, the broadcast run, for the number to compare against ── */
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);
	run(16u);
	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, at(g_stream), cmd,
						    sizeof(cmd)));
	run(200u);
	zassert_true(g_switched);
	for (i = 0u; i < FOLLOWERS; i++) {
		zassert_not_equal(UINT32_MAX, g_f[i].first_private,
				  "broadcast: receiver %u never re-acquired", i);
		broadcast_gap[i] = g_f[i].first_private - g_switch_msg;
	}

	/* ── Then the silent run, same node, same three receivers ───────── */
	private_before(NULL);
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_SILENT,
		PROFILE_PRIVATE_K_DEFAULT);
	g_log_enable = true;
	run(16u);
	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, at(g_stream), cmd,
						    sizeof(cmd)),
		      "a silent node still takes the command; what changes is "
		      "what it says about it");
	/* SILENT MEANS NOW: there is no countdown to wait for, because a
	 * countdown with nothing to carry it is a delay rather than a warning. */
	zassert_equal(PROFILE_PRIVATE_STATE_PRIVATE,
		      profile_private_state(&g_pp));
	run(200u);

	for (i = 0u; i < FOLLOWERS; i++) {
		zassert_true(g_f[i].saw_target,
			     "silent: receiver %u never re-acquired", i);
		zassert_equal(g_entered_devnum, g_f[i].target_devnum,
			      "silent: receiver %u derived the wrong locator",
			      i);
		zassert_equal(0u, g_f[i].announcements,
			      "silent: receiver %u verified an announcement, so "
			      "one went on the air", i);
		zassert_not_equal(UINT32_MAX, g_f[i].first_private);
		silent_gap[i] = g_f[i].first_private - g_switch_msg;
	}

	/* ── The negative claim, against the capture ────────────────────── */
	for (i = 0u; i < g_log_n; i++) {
		const uint8_t *b = g_log[i];
		uint8_t        count;

		if ((b[0] & PROFILE_COMPAT_PAGE_MASK) !=
		    PROFILE_COMPAT_PAGE_BEACON) {
			continue;
		}
		count = (uint8_t)((b[1] & 0x0Fu) + 1u);
		zassert_not_equal(PROFILE_PRIVATE_SET_FRAMES, count,
				  "message %u is a four-frame beacon set: the "
				  "announcement went on the air on a silent "
				  "node", i);
		if ((b[1] >> 4) == 0u) {
			zassert_equal(0u,
				      (uint8_t)(b[3] & PROFILE_COMPAT_PENDING_SWITCH),
				      "message %u has the pending-switch bit "
				      "set on a silent node", i);
		}
		if ((b[1] >> 4) == 1u) {
			zassert_equal(0u, b[2],
				      "message %u carries a locator device type "
				      "on a silent node", i);
			zassert_equal(0u, (uint8_t)(b[3] | b[4]),
				      "message %u carries a locator device "
				      "number on a silent node", i);
			zassert_equal(0u, (uint8_t)(b[5] | b[6]),
				      "message %u carries a locator period on a "
				      "silent node", i);
		}
	}

	/* ── The two numbers, side by side ──────────────────────────────── */
	TC_PRINT("re-acquisition gap, in transmitted messages "
		 "(retune modelled at %u):\n", RETUNE_MSGS);
	for (i = 0u; i < FOLLOWERS; i++) {
		TC_PRINT("  receiver %u: broadcast %u, silent %u\n", i,
			 broadcast_gap[i], silent_gap[i]);
	}
	TC_PRINT("  at %u ms per message that is broadcast ~%u ms, "
		 "silent ~%u ms\n",
		 (unsigned int)(period_us(PROFILE_HR_PERIOD) / 1000u),
		 (unsigned int)(broadcast_gap[0] *
				period_us(PROFILE_HR_PERIOD) / 1000u),
		 (unsigned int)(silent_gap[0] *
				period_us(PROFILE_HR_PERIOD) / 1000u));

	for (i = 0u; i < FOLLOWERS; i++) {
		zassert_true(silent_gap[i] > broadcast_gap[i],
			     "silent cost receiver %u %u messages and broadcast "
			     "cost it %u; silent buys unlinkability and PAYS IN "
			     "AVAILABILITY, so if these are equal the "
			     "measurement is wrong", i, silent_gap[i],
			     broadcast_gap[i]);
		zassert_true(silent_gap[i] >= PROFILE_PRIVATE_SILENCE_MSGS,
			     "the silent gap cannot be shorter than the silence "
			     "the receiver has to observe first");
	}

	/* The receiver that ASKED pays the same as the two that did not, in
	 * both modes. That symmetry is the 1:N property, and it is the one a
	 * bench with a single head unit cannot see. */
	zassert_equal(broadcast_gap[0], broadcast_gap[1]);
	zassert_equal(broadcast_gap[0], broadcast_gap[2]);
	zassert_equal(silent_gap[0], silent_gap[1]);
	zassert_equal(silent_gap[0], silent_gap[2]);
}

/* ═══ The announcement's tag ═════════════════════════════════════════════ */

ZTEST(profile_private, test_an_announcement_that_does_not_verify_is_ignored)
{
	struct profile_private_rx rx;
	uint8_t frame_a[8];
	uint8_t frame_b[8];
	uint8_t tag[RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES];
	uint64_t now = at(40u);

	keys_up();
	zassert_equal(0, profile_private_rx_init(&rx, RX_A,
						 period_us(PROFILE_HR_PERIOD)));

	/* A well-formed announcement, built the way the node builds one. */
	memset(frame_a, 0, sizeof(frame_a));
	frame_a[0] = PROFILE_COMPAT_PAGE_BEACON;
	frame_a[1] = (uint8_t)((PROFILE_PRIVATE_SET_INDEX_A << 4) |
			       (PROFILE_PRIVATE_SET_FRAMES - 1u));
	frame_a[2] = PRIVATE_DEVICE_TYPE;
	frame_a[3] = 0x34u;
	frame_a[4] = 0x12u;
	frame_a[5] = 0x00u;
	frame_a[6] = 0x20u;
	frame_a[7] = (uint8_t)((PROFILE_PRIVATE_REASON_COMMAND << 6) | 2u);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_announce_tag(NODE_CH, now, frame_a, 8u,
						      tag));
	memset(frame_b, 0, sizeof(frame_b));
	frame_b[0] = PROFILE_COMPAT_PAGE_BEACON;
	frame_b[1] = (uint8_t)((PROFILE_PRIVATE_SET_INDEX_B << 4) |
			       (PROFILE_PRIVATE_SET_FRAMES - 1u));
	memcpy(&frame_b[2], tag, sizeof(tag));

	/* ── It verifies, and arms the receiver ─────────────────────────── */
	zassert_equal(0, profile_private_rx_message(&rx, frame_a, 8u, now));
	zassert_equal(1, profile_private_rx_message(&rx, frame_b, 8u, now));
	zassert_equal(1u, rx.verified);
	zassert_equal(0x1234u, rx.target_devnum);

	/* ── One bit of the LOCATOR moved, tag untouched ────────────────── */
	profile_private_rx_clear(&rx);
	rx.verified = 0u;
	frame_a[3] ^= 0x01u;
	zassert_equal(0, profile_private_rx_message(&rx, frame_a, 8u, now));
	zassert_equal(-EACCES,
		      profile_private_rx_message(&rx, frame_b, 8u, now),
		      "a herding attack: rewrite the device number in a frame "
		      "that is in the clear and every keyed receiver in the "
		      "room follows you instead");
	zassert_equal(1u, rx.rejected);
	zassert_false(rx.armed,
		      "an unverified announcement armed the receiver anyway");
	frame_a[3] ^= 0x01u;

	/* ── One bit of the COUNTDOWN moved ─────────────────────────────── */
	frame_a[7] ^= 0x01u;
	zassert_equal(0, profile_private_rx_message(&rx, frame_a, 8u, now));
	zassert_equal(-EACCES,
		      profile_private_rx_message(&rx, frame_b, 8u, now),
		      "moving the countdown desynchronises every receiver in "
		      "the room from every other one");
	frame_a[7] ^= 0x01u;

	/* ── One bit of the TAG ─────────────────────────────────────────── */
	frame_b[4] ^= 0x80u;
	zassert_equal(0, profile_private_rx_message(&rx, frame_a, 8u, now));
	zassert_equal(-EACCES,
		      profile_private_rx_message(&rx, frame_b, 8u, now));
	frame_b[4] ^= 0x80u;

	/* ── Byte [1] is inside the tag too ─────────────────────────────── */
	zassert_equal(0, profile_private_rx_message(&rx, frame_a, 8u, now));
	frame_b[1] = (uint8_t)((PROFILE_PRIVATE_SET_INDEX_B << 4) |
			       (PROFILE_PRIVATE_SET_FRAMES - 1u));
	frame_a[1] = (uint8_t)((PROFILE_PRIVATE_SET_INDEX_A << 4) | 7u);
	zassert_equal(0, profile_private_rx_message(&rx, frame_a, 8u, now),
		      "an eight-frame set is not an announcement and must not "
		      "be read as one");
	frame_a[1] = (uint8_t)((PROFILE_PRIVATE_SET_INDEX_A << 4) |
			       (PROFILE_PRIVATE_SET_FRAMES - 1u));

	/* ── And a frame B with no frame A is not evidence of anything ─── */
	profile_private_rx_clear(&rx);
	zassert_equal(0, profile_private_rx_message(&rx, frame_b, 8u, now));
	zassert_false(rx.armed);
}

ZTEST(profile_private, test_a_replayed_announcement_fails_in_another_window)
{
	struct profile_private_rx rx;
	uint8_t  frame_a[8];
	uint8_t  frame_b[8];
	uint8_t  tag[RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES];
	uint64_t then = at(40u);
	uint64_t later = then + (uint64_t)TEST_T_S * US_PER_S;

	/* A recorded announcement replayed later fails not because it's
	 * recognised, but because Tier I's counter (in the nonce, derived from
	 * time) has moved on: the same frame A produces a different tag in a
	 * later interval. */
	keys_up();
	zassert_equal(0, profile_private_rx_init(&rx, RX_A,
						 period_us(PROFILE_HR_PERIOD)));

	memset(frame_a, 0, sizeof(frame_a));
	frame_a[0] = PROFILE_COMPAT_PAGE_BEACON;
	frame_a[1] = (uint8_t)((PROFILE_PRIVATE_SET_INDEX_A << 4) |
			       (PROFILE_PRIVATE_SET_FRAMES - 1u));
	frame_a[2] = PRIVATE_DEVICE_TYPE;
	frame_a[3] = 0x99u;
	frame_a[7] = 4u;

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_announce_tag(NODE_CH, then, frame_a, 8u,
						      tag));
	memset(frame_b, 0, sizeof(frame_b));
	frame_b[0] = PROFILE_COMPAT_PAGE_BEACON;
	frame_b[1] = (uint8_t)((PROFILE_PRIVATE_SET_INDEX_B << 4) |
			       (PROFILE_PRIVATE_SET_FRAMES - 1u));
	memcpy(&frame_b[2], tag, sizeof(tag));

	zassert_equal(0, profile_private_rx_message(&rx, frame_a, 8u, then));
	zassert_equal(1, profile_private_rx_message(&rx, frame_b, 8u, then));

	profile_private_rx_clear(&rx);
	rx.rejected = 0u;

	zassert_equal(0, profile_private_rx_message(&rx, frame_a, 8u, later));
	zassert_equal(-EACCES,
		      profile_private_rx_message(&rx, frame_b, 8u, later),
		      "an announcement recorded in one interval verified in "
		      "another; a passive observer could then herd every keyed "
		      "receiver in the room with yesterday's capture");
	zassert_equal(1u, rx.rejected);
	zassert_false(rx.armed);
}

/* ═══ Policy ═════════════════════════════════════════════════════════════ */

ZTEST(profile_private, test_a_never_node_refuses_an_authenticated_command)
{
	uint8_t cmd[8];

	/* `never` means never: the node refuses an otherwise-valid authenticated
	 * command and COUNTS the refusal, so an owner can tell it apart from a
	 * command that never arrived. */
	node_up(PROFILE_PRIVATE_NEVER, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);
	run(8u);

	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(-EPERM,
		      profile_private_on_command(&g_pp, at(g_stream), cmd,
						 sizeof(cmd)));
	zassert_equal(1u, g_pp.refused_policy);
	zassert_equal(0u, g_pp.refused_unauth,
		      "the command was authentic; refusing it is a policy "
		      "decision and counting it as an attack would tell an "
		      "owner the wrong thing");
	zassert_equal(PROFILE_PRIVATE_STATE_COMPAT,
		      profile_private_state(&g_pp));

	/* The physical action is refused too, and counted. */
	zassert_equal(-EPERM,
		      profile_private_physical_action(&g_pp, at(g_stream)));
	zassert_equal(2u, g_pp.refused_policy);

	run(64u);
	zassert_equal(0u, g_enter_calls, "a `never` node left the compat "
					 "channel");
	zassert_equal(0u, profile_private_locator(&g_pp),
		      "a `never` node derived a locator; there is nowhere for "
		      "it to go and the beacon must carry zeros");

	/* ...and the beacon says so, in both fields, which must agree. */
	zassert_equal(PROFILE_PRIVATE_NEVER,
		      (uint8_t)(g_pc.frames[0][3] & 0x03u));
	zassert_equal(0u, (uint8_t)(g_pc.frames[0][2] &
				    PROFILE_COMPAT_CAP_PRIVATE_AVAILABLE));
	zassert_equal(0u, (uint8_t)(g_pc.frames[0][3] &
				    PROFILE_COMPAT_PENDING_SWITCH));
	zassert_equal(0u, g_pc.frames[1][2]);
	zassert_equal(0u, (uint8_t)(g_pc.frames[1][3] | g_pc.frames[1][4]));
}

ZTEST(profile_private, test_an_unauthenticated_request_can_never_switch_a_node)
{
	uint8_t cmd[8];
	uint8_t i;

	/* THE SECURITY PROPERTY: there is no unauthenticated path because a
	 * stranger who could mute a power meter for every legacy receiver in
	 * the room would have a one-packet DoS against everybody. */
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);
	run(8u);

	/* No tag at all. */
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = PROFILE_COMPAT_PAGE_BEACON;
	cmd[1] = PROFILE_PRIVATE_CMD_FRAME_BYTE;
	cmd[2] = PROFILE_PRIVATE_OP_GO_PRIVATE;
	zassert_equal(-EACCES,
		      profile_private_on_command(&g_pp, at(g_stream), cmd,
						 sizeof(cmd)));

	/* A guessed tag, every byte of it, one at a time. */
	for (i = 0u; i < 8u; i++) {
		memset(&cmd[3], i, 5u);
		zassert_equal(-EACCES,
			      profile_private_on_command(&g_pp, at(g_stream),
							 cmd, sizeof(cmd)));
	}

	/* A VALID tag from a stranger's key, which is the interesting one: the
	 * construction is right and only the key is wrong. */
	{
		uint8_t stranger_root[16];
		uint8_t stolen[8];

		memset(stranger_root, 0xA5, sizeof(stranger_root));
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_set_key(RX_C, stranger_root,
							 128, NODE_DEVNUM));
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_set_epoch(RX_C, TEST_EPOCH,
							   T_ORIGIN, 0u,
							   PROFILE_HR_PERIOD));
		build_command(RX_C, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream),
			      stolen);
		zassert_equal(-EACCES,
			      profile_private_on_command(&g_pp, at(g_stream),
							 stolen,
							 sizeof(stolen)));
	}

	zassert_equal(10u, g_pp.refused_unauth);
	zassert_equal(0u, g_pp.commands_ok);
	zassert_equal(PROFILE_PRIVATE_STATE_COMPAT,
		      profile_private_state(&g_pp));

	run(64u);
	zassert_equal(0u, g_enter_calls,
		      "an unauthenticated over-air request moved the node");
}

ZTEST(profile_private, test_no_over_air_path_can_change_policy)
{
	uint8_t cmd[8];
	uint8_t op;

	/* An over-air policy-change command is REFUSED, not deferred - a policy
	 * a remote party can rewrite is not a policy. Sends a validly-tagged
	 * command under every operation byte that isn't one of the two real
	 * ones and asserts none of them are consumed as anything. */
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);
	run(8u);

	for (op = 0u; op != 0xFFu; op++) {
		if (op == PROFILE_PRIVATE_OP_GO_PRIVATE ||
		    op == PROFILE_PRIVATE_OP_RETURN) {
			continue;
		}
		build_command(RX_A, op, at(g_stream), cmd);
		zassert_equal(-EPROTO,
			      profile_private_on_command(&g_pp, at(g_stream),
							 cmd, sizeof(cmd)),
			      "operation 0x%02X was consumed as something", op);
		zassert_equal(PROFILE_PRIVATE_COMMAND,
			      profile_private_policy(&g_pp),
			      "the policy moved on operation 0x%02X", op);
	}

	/* Including the shapes a policy command would most plausibly take: an
	 * operation byte carrying a policy value in its low bits. */
	zassert_equal(PROFILE_PRIVATE_COMMAND, profile_private_policy(&g_pp));
	zassert_false(profile_private_policy_from_nvm(&g_pp));
	zassert_false(g_nvm_has,
		      "something over the air wrote the policy record");
	zassert_true(g_pp.refused_malformed >= 253u);
}

ZTEST(profile_private, test_policy_precedence_is_nvm_then_kconfig)
{
	struct profile_private_cfg cfg;

	/* NVM if provisioned, else Kconfig; the host message writes NVM rather
	 * than shadowing it. */
	keys_up();
	hr_up();

	/* ── Nothing provisioned: Kconfig's value, which on a shipped strap
	 *    is `never` ───────────────────────────────────────────────────── */
	compat_up(PROFILE_PRIVATE_NEVER);
	private_cfg_fill(&cfg, PROFILE_PRIVATE_NEVER, PROFILE_PRIVATE_BROADCAST,
			 PROFILE_PRIVATE_K_DEFAULT);
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(PROFILE_PRIVATE_NEVER, profile_private_policy(&g_pp));
	zassert_false(profile_private_policy_from_nvm(&g_pp));

	/* ── A provisioned record OUTRANKS Kconfig ──────────────────────── */
	g_nvm_has = true;
	g_nvm_policy = PROFILE_PRIVATE_COMMAND;
	private_cfg_fill(&cfg, PROFILE_PRIVATE_NEVER, PROFILE_PRIVATE_BROADCAST,
			 PROFILE_PRIVATE_K_DEFAULT);
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(PROFILE_PRIVATE_COMMAND, profile_private_policy(&g_pp));
	zassert_true(profile_private_policy_from_nvm(&g_pp));

	/* ── A record this firmware cannot interpret is not a licence to
	 *    pick one ─────────────────────────────────────────────────────── */
	g_nvm_policy = 0x7Fu;
	private_cfg_fill(&cfg, PROFILE_PRIVATE_NEVER, PROFILE_PRIVATE_BROADCAST,
			 PROFILE_PRIVATE_K_DEFAULT);
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(PROFILE_PRIVATE_NEVER, profile_private_policy(&g_pp));

	/* ── The host writes the RECORD, and the running value comes back
	 *    out of it ───────────────────────────────────────────────────── */
	g_nvm_has = false;
	private_cfg_fill(&cfg, PROFILE_PRIVATE_NEVER, PROFILE_PRIVATE_BROADCAST,
			 PROFILE_PRIVATE_K_DEFAULT);
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(0, profile_private_attach(&g_pp, &g_pc));
	zassert_equal(0, profile_private_host_set_policy(&g_pp,
							 PROFILE_PRIVATE_PHYSICAL));
	zassert_equal(PROFILE_PRIVATE_PHYSICAL, profile_private_policy(&g_pp));
	zassert_true(profile_private_policy_from_nvm(&g_pp));
	zassert_true(g_nvm_has);
	zassert_equal(PROFILE_PRIVATE_PHYSICAL, g_nvm_policy);

	/* ── A write that does not stick changes NOTHING, which is the whole
	 *    of "writes NVM rather than shadowing it" ──────────────────────── */
	g_nvm_store_rc = -EIO;
	zassert_equal(-EIO, profile_private_host_set_policy(&g_pp,
							    PROFILE_PRIVATE_COMMAND));
	zassert_equal(PROFILE_PRIVATE_PHYSICAL, profile_private_policy(&g_pp),
		      "the running policy moved even though the record did "
		      "not; a node whose policy depends on when it was last "
		      "rebooted is the bug the precedence rule exists to stop");
	zassert_equal(PROFILE_PRIVATE_PHYSICAL, g_nvm_policy);
	g_nvm_store_rc = 0;

	/* ── And a node with no record refuses the host outright rather than
	 *    applying a policy that would not survive a reboot ────────────── */
	private_cfg_fill(&cfg, PROFILE_PRIVATE_NEVER, PROFILE_PRIVATE_BROADCAST,
			 PROFILE_PRIVATE_K_DEFAULT);
	cfg.policy_load = NULL;
	cfg.policy_store = NULL;
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(-ENOTSUP,
		      profile_private_host_set_policy(&g_pp,
						      PROFILE_PRIVATE_COMMAND));
	zassert_equal(PROFILE_PRIVATE_NEVER, profile_private_policy(&g_pp));
}

ZTEST(profile_private, test_attest_off_refuses_every_policy_but_the_exemption)
{
	struct profile_private_cfg cfg;
	uint8_t policy;
	uint8_t announce;

	/*
	 * THE KEY DEPENDENCY: a node with attest off refuses to BUILD with
	 * private_policy != never, except physical + silent, which must build
	 * and work. A compile-time refusal can't be asserted by a test that has
	 * to compile, so this walks all sixteen inputs of THE PREDICATE ITSELF
	 * - the same expression as the BUILD_ASSERT and the Kconfig `depends
	 * on`. The runtime arm underneath catches a policy arriving from NVM on
	 * a node built for a different one, which no BUILD_ASSERT can see.
	 */
	keys_up();
	hr_up();
	compat_up(PROFILE_PRIVATE_NEVER);

	for (policy = PROFILE_PRIVATE_NEVER; policy <= PROFILE_PRIVATE_ALWAYS;
	     policy++) {
		for (announce = PROFILE_PRIVATE_BROADCAST;
		     announce <= PROFILE_PRIVATE_SILENT; announce++) {
			bool with = PROFILE_PRIVATE_COMBINATION_OK(policy,
								   announce,
								   true);
			bool without = PROFILE_PRIVATE_COMBINATION_OK(policy,
								      announce,
								      false);

			zassert_true(with,
				     "policy %u announce %u is refused even "
				     "WITH attestation", policy, announce);

			if (policy == PROFILE_PRIVATE_NEVER) {
				zassert_true(without,
					     "`never` needs no attestation: a "
					     "plain ANT+ sensor is the setting "
					     "most straps ship in");
			} else if (policy == PROFILE_PRIVATE_PHYSICAL &&
				   announce == PROFILE_PRIVATE_SILENT) {
				zassert_true(without,
					     "physical + silent is THE "
					     "exemption: nothing is announced, "
					     "so there is nothing to forge, "
					     "and there is no over-air trigger "
					     "to authenticate");
			} else {
				zassert_false(without,
					      "policy %u announce %u was "
					      "permitted with attestation off; "
					      "a broadcast announcement needs "
					      "its frame-B tag and a command "
					      "trigger needs K_cmd",
					      policy, announce);
			}

			/* The runtime arm agrees with the predicate, value for
			 * value. */
			private_cfg_fill(&cfg, policy, announce,
					 PROFILE_PRIVATE_K_DEFAULT);
			cfg.attest = false;
			if (without) {
				zassert_equal(0,
					      profile_private_init(&g_pp, &cfg),
					      "policy %u announce %u must build "
					      "AND WORK with attestation off",
					      policy, announce);
			} else {
				zassert_equal(-ENOTSUP,
					      profile_private_init(&g_pp, &cfg),
					      "policy %u announce %u was "
					      "accepted at runtime with "
					      "attestation off", policy,
					      announce);
			}
		}
	}
}

ZTEST(profile_private, test_the_exempted_combination_actually_switches)
{
	struct profile_private_cfg cfg;

	/* The exemption is not a hole, it's a supported node: a button, no
	 * announcement, no attestation, and a private channel whose only on-air
	 * security surface is the ciphertext itself. */
	keys_up();
	hr_up();
	compat_up(PROFILE_PRIVATE_PHYSICAL);
	private_cfg_fill(&cfg, PROFILE_PRIVATE_PHYSICAL, PROFILE_PRIVATE_SILENT,
			 PROFILE_PRIVATE_K_DEFAULT);
	cfg.attest = false;
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(0, profile_private_attach(&g_pp, &g_pc));
	followers_up();

	run(8u);
	zassert_equal(0, profile_private_physical_action(&g_pp, at(g_stream)));
	zassert_equal(PROFILE_PRIVATE_STATE_PRIVATE,
		      profile_private_state(&g_pp),
		      "a silent switch happens at once: there is no countdown "
		      "to wait for");
	zassert_equal(1u, g_enter_calls);
	zassert_equal(profile_private_locator(&g_pp), g_entered_devnum);

	/* And the button brings it back. */
	run(8u);
	zassert_equal(0, profile_private_physical_action(&g_pp, at(g_stream)));
	zassert_equal(PROFILE_PRIVATE_STATE_COMPAT,
		      profile_private_state(&g_pp));
	zassert_equal(1u, g_leave_calls);
}

/* ═══ Coming back ════════════════════════════════════════════════════════ */

ZTEST(profile_private, test_the_node_announces_the_return_and_reverts_on_timeout)
{
	struct profile_private_cfg cfg;
	uint32_t i;

	/* A sensor that silently stays private is indistinguishable from a dead
	 * one, so the duration is bounded and the revert announces itself the
	 * same way the switch did, reason included. */
	keys_up();
	hr_up();
	compat_up(PROFILE_PRIVATE_PHYSICAL);
	private_cfg_fill(&cfg, PROFILE_PRIVATE_PHYSICAL,
			 PROFILE_PRIVATE_BROADCAST, PROFILE_PRIVATE_K_DEFAULT);
	/* Sixty seconds rather than an hour, so the whole revert fits inside a
	 * test that runs in a millisecond of simulated air. */
	cfg.max_private_s = 60u;
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(0, profile_private_attach(&g_pp, &g_pc));
	followers_up();

	run(8u);
	zassert_equal(0, profile_private_physical_action(&g_pp, at(g_stream)));
	run(96u);
	zassert_true(g_switched);
	for (i = 0u; i < FOLLOWERS; i++) {
		zassert_true(g_f[i].saw_target, "receiver %u did not follow", i);
		/* Ready to hear the return: they are on the private channel
		 * now, and the announcement comes from there. */
		g_f[i].saw_target = false;
		g_f[i].announcements = 0u;
		profile_private_rx_clear(&g_f[i].rx);
	}

	/* 60 s at ~4 Hz is ~244 messages; the countdown is another ~57. */
	run(340u);

	zassert_true(g_returned,
		     "the bounded duration expired and the node stayed private");
	zassert_equal(1u, g_pp.reverts_timeout);
	zassert_equal(1u, g_leave_calls);
	zassert_equal(PROFILE_PRIVATE_STATE_COMPAT,
		      profile_private_state(&g_pp));

	for (i = 0u; i < FOLLOWERS; i++) {
		zassert_true(g_f[i].announcements > 0u,
			     "receiver %u heard no RETURN announcement, so it "
			     "would have sat on a dead channel until a timeout",
			     i);
		zassert_true(g_f[i].saw_target, "receiver %u did not come back",
			     i);
		zassert_equal(NODE_DEVNUM, g_f[i].target_devnum,
			      "the RETURN sent receiver %u to 0x%04X, not back "
			      "to the compat channel", i,
			      g_f[i].target_devnum);
		zassert_equal(PROFILE_HR_DEVICE_TYPE, g_f[i].target_type);
		zassert_equal(PROFILE_PRIVATE_REASON_TIMEOUT,
			      g_f[i].rx.target_reason,
			      "the reason on the air is what lets a receiver "
			      "say the node came back because its timer ran "
			      "out rather than because somebody asked");
	}
	zassert_equal(g_f[0].due_at, g_f[1].due_at);
	zassert_equal(g_f[0].due_at, g_f[2].due_at);
	zassert_equal(g_return_msg, g_f[0].due_at);
}

ZTEST(profile_private, test_a_power_cycle_reverts)
{
	struct profile_private_cfg cfg;

	/* Private mode is RAM state, never persisted, so a rebooted node is an
	 * ANT+ sensor again. Policy survives (it's in NVM); state does not -
	 * the two being in different places is the design. */
	keys_up();
	hr_up();
	compat_up(PROFILE_PRIVATE_PHYSICAL);
	private_cfg_fill(&cfg, PROFILE_PRIVATE_PHYSICAL, PROFILE_PRIVATE_SILENT,
			 PROFILE_PRIVATE_K_DEFAULT);
	zassert_equal(0, profile_private_init(&g_pp, &cfg));
	zassert_equal(0, profile_private_attach(&g_pp, &g_pc));

	zassert_equal(0, profile_private_physical_action(&g_pp, T_ORIGIN));
	zassert_true(profile_private_is_private(&g_pp));

	/* The power cycle: everything in RAM is gone, the record is not. */
	memset(&g_pp, 0xCD, sizeof(g_pp));
	zassert_equal(0, profile_private_init(&g_pp, &cfg));

	zassert_equal(PROFILE_PRIVATE_STATE_COMPAT,
		      profile_private_state(&g_pp),
		      "the node came back up private; a sensor that silently "
		      "stays private is indistinguishable from a dead one, and "
		      "this is the revert that has to work when nothing else "
		      "did");
	zassert_false(profile_private_is_private(&g_pp));
	zassert_equal(PROFILE_PRIVATE_PHYSICAL, profile_private_policy(&g_pp),
		      "the POLICY must survive the reboot even though the "
		      "state must not");
}

ZTEST(profile_private, test_always_boots_straight_into_private)
{
	struct profile_private_cfg cfg;

	/* `always` is today's radiant_sec node, named as a policy state so the
	 * axis is complete. Nothing is announced, because there was never a
	 * compat channel with listeners on it. */
	keys_up();
	hr_up();
	compat_up(PROFILE_PRIVATE_ALWAYS);
	private_cfg_fill(&cfg, PROFILE_PRIVATE_ALWAYS, PROFILE_PRIVATE_BROADCAST,
			 PROFILE_PRIVATE_K_DEFAULT);
	zassert_equal(0, profile_private_init(&g_pp, &cfg));

	zassert_equal(PROFILE_PRIVATE_STATE_PRIVATE,
		      profile_private_state(&g_pp));
	zassert_equal(1u, g_enter_calls);
	zassert_equal(profile_private_locator(&g_pp), g_entered_devnum);
	zassert_equal(0u, g_pp.announcements);

	/* There is nowhere for a physical action to take it. */
	zassert_equal(-EPERM, profile_private_physical_action(&g_pp, T_ORIGIN));
	zassert_equal(1u, g_pp.refused_policy);
}

/* ═══ The rate limit and the interlock ═══════════════════════════════════ */

ZTEST(profile_private, test_accepted_commands_are_rate_limited)
{
	uint8_t  cmd[8];
	uint64_t t;

	/* Not a defence against an attacker (the tag is that) but a bound on
	 * what one buggy/captured receiver can cost the other N-1 listeners,
	 * each of whom pays a countdown and a retune per accepted command. */
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_SILENT,
		PROFILE_PRIVATE_K_DEFAULT);
	run(8u);

	t = at(g_stream);
	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, t, cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, t, cmd, sizeof(cmd)));
	zassert_true(profile_private_is_private(&g_pp));

	/* Straight back out again, one second later. */
	t += US_PER_S;
	build_command(RX_A, PROFILE_PRIVATE_OP_RETURN, t, cmd);
	zassert_equal(-EAGAIN,
		      profile_private_on_command(&g_pp, t, cmd, sizeof(cmd)));
	zassert_equal(1u, g_pp.refused_rate);
	zassert_true(profile_private_is_private(&g_pp));

	/* A refusal does not extend the floor: the clock still runs from the
	 * last ACCEPTED command. */
	t = at(g_stream) + (uint64_t)PROFILE_PRIVATE_CMD_MIN_S * US_PER_S;
	build_command(RX_A, PROFILE_PRIVATE_OP_RETURN, t, cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, t, cmd, sizeof(cmd)));
	zassert_false(profile_private_is_private(&g_pp));
}

/* A stand-in for Layer D's window: a client of the same seam that says it is
 * contributing frames. The interlock is between two ACTIVITIES, and this is the
 * smallest honest one. */
static bool     g_other_open;
static uint32_t g_other_frames;

static uint8_t other_frame_count(void *user, uint64_t now_us)
{
	ARG_UNUSED(user);
	ARG_UNUSED(now_us);
	return g_other_open ? 6u : 0u;
}

static bool other_frame(void *user, uint8_t i, uint8_t *payload)
{
	ARG_UNUSED(user);
	ARG_UNUSED(i);
	memset(payload, 0xEE, PROFILE_PRIVATE_FRAME_PAYLOAD);
	g_other_frames++;
	return true;
}

ZTEST(profile_private, test_a_countdown_and_an_open_window_refuse_each_other)
{
	struct profile_compat_client other;
	uint8_t                      cmd[8];

	/* THE INTERLOCK: Layer D's frames and this announcement share indices,
	 * and a switch closes the channel the window runs on. A countdown and a
	 * window must not run at once; whichever arrives second is refused,
	 * both directions, or the rule is a race. */
	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);

	memset(&other, 0, sizeof(other));
	other.frames = other_frame_count;
	other.frame = other_frame;
	other.user = &g_other_open;
	zassert_equal(0, profile_compat_set_client(&g_pc, &other),
		      "the beacon page must hold two clients: the exclusion is "
		      "between two activities, not two registrations");

	g_other_open = true;
	g_other_frames = 0u;
	run(8u);

	/* The window is open; the countdown is second and is refused. */
	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(-EBUSY,
		      profile_private_on_command(&g_pp, at(g_stream), cmd,
						 sizeof(cmd)));
	zassert_equal(1u, g_pp.refused_busy);
	zassert_equal(-EBUSY,
		      profile_private_physical_action(&g_pp, at(g_stream)));
	zassert_equal(2u, g_pp.refused_busy);

	run(16u);
	zassert_true(g_other_frames > 0u,
		     "the other client's frames stopped going out, which is the "
		     "damage the interlock exists to prevent");
	zassert_equal(PROFILE_PRIVATE_STATE_COMPAT,
		      profile_private_state(&g_pp));

	/* The window closes, and now the countdown is allowed. */
	g_other_open = false;
	run(1u);
	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, at(g_stream), cmd,
						    sizeof(cmd)));
	zassert_equal(PROFILE_PRIVATE_STATE_ANNOUNCING,
		      profile_private_state(&g_pp));

	/* ...and now the window is the one that would be second. The other
	 * client's side of this is asserted in test_profile_enrol.c against the
	 * real module; here it is the seam's answer that matters. */
	zassert_true(profile_compat_client_busy(&g_pc, at(g_stream),
						&g_other_open),
		     "a running countdown does not show as busy to the other "
		     "client, so a window would open straight into it");
}

/* ═══ The beacon, while a switch is pending ══════════════════════════════ */

ZTEST(profile_private, test_the_beacon_carries_the_locator_and_the_pending_bit)
{
	uint8_t  cmd[8];
	uint32_t i;
	bool     saw_pending = false;
	bool     saw_locator = false;
	bool     saw_four = false;

	node_up(PROFILE_PRIVATE_COMMAND, PROFILE_PRIVATE_BROADCAST,
		PROFILE_PRIVATE_K_DEFAULT);

	/* Before anything happens: the locator is published, because a receiver
	 * has to be able to find this node whether or not it hears an
	 * announcement, and private-available is set because the policy is not
	 * `never`. */
	zassert_equal(PRIVATE_DEVICE_TYPE, g_pc.frames[1][2]);
	zassert_equal((uint8_t)(profile_private_locator(&g_pp) & 0xFFu),
		      g_pc.frames[1][3]);
	zassert_equal((uint8_t)(profile_private_locator(&g_pp) >> 8),
		      g_pc.frames[1][4]);
	zassert_true((g_pc.frames[0][2] &
		      PROFILE_COMPAT_CAP_PRIVATE_AVAILABLE) != 0u);
	zassert_equal(0u, (uint8_t)(g_pc.frames[0][3] &
				    PROFILE_COMPAT_PENDING_SWITCH));

	g_log_enable = true;
	run(8u);
	build_command(RX_A, PROFILE_PRIVATE_OP_GO_PRIVATE, at(g_stream), cmd);
	zassert_equal(1, profile_private_on_command(&g_pp, at(g_stream), cmd,
						    sizeof(cmd)));
	run(96u);

	for (i = 0u; i < g_log_n; i++) {
		const uint8_t *b = g_log[i];
		uint8_t        idx;
		uint8_t        count;

		if ((b[0] & PROFILE_COMPAT_PAGE_MASK) !=
		    PROFILE_COMPAT_PAGE_BEACON) {
			continue;
		}
		idx = (uint8_t)(b[1] >> 4);
		count = (uint8_t)((b[1] & 0x0Fu) + 1u);
		if (count == PROFILE_PRIVATE_SET_FRAMES) {
			saw_four = true;
		}
		if (idx == 0u && count == PROFILE_PRIVATE_SET_FRAMES &&
		    (b[3] & PROFILE_COMPAT_PENDING_SWITCH) != 0u) {
			saw_pending = true;
		}
		if (idx == 1u && b[2] == PRIVATE_DEVICE_TYPE) {
			saw_locator = true;
		}
	}

	zassert_true(saw_four,
		     "the beacon page's set never grew to four frames, so no "
		     "announcement can have gone out");
	zassert_true(saw_pending,
		     "frame 0 never carried the pending-switch bit during a "
		     "countdown");
	zassert_true(saw_locator,
		     "frame 1 never carried the locator");
}

#endif /* CONFIG_RADIANT_SEC_COMPAT */
