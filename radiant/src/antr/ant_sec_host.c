/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The host surface for the two payload transforms: messages 0xF1-0xF4, as
 * antr_* entry points.
 *
 * The one security file that reaches an application header - the seam
 * between this module's return codes and the ANT serial protocol's.
 *
 * The switch bits are NOT translated on purpose: RADIANT_SEC_SW_CONF/AUTH/
 * DROP_UNVER/DESC_CONF/VERDICT_EVENT are 0x01/0x02/0x04/0x08/0x10, matching
 * 0xF1's bitmask byte-for-byte. The BUILD_ASSERTs below keep them that way; a mapping table
 * would be a second, driftable definition of the wire format.
 *
 * antr_sec_key_set() is write-only - no read arm for 0xF2 exists.
 *
 * The caller (src/ant_serial_bridge.c) wipes its copy of the message body
 * after dispatch, but that is not sufficient: the bytes also passed through
 * the USB stack's ring buffer and transport buffer, neither reachable from
 * here. A root key that crossed USB should be treated as having been in
 * host memory.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <radiant/radiant_sec.h>
#include <radiant/radiant_channel.h>

#include <radiant/ant_radio.h>
#include "ant_radio_radiant_priv.h"
#include <radiant/ant_wire.h>

BUILD_ASSERT(RADIANT_SEC_SW_CONF == 0x01,
	     "0xF1 bit 0 is X_CONF; the wire and the module must agree");
BUILD_ASSERT(RADIANT_SEC_SW_AUTH == 0x02,
	     "0xF1 bit 1 is X_AUTH; the wire and the module must agree");
BUILD_ASSERT(RADIANT_SEC_SW_DROP_UNVER == 0x04,
	     "0xF1 bit 2 is the deliver policy; the wire and the module must "
	     "agree");
BUILD_ASSERT(RADIANT_SEC_SW_DESC_CONF == 0x08,
	     "0xF1 bit 3 is descriptor confidentiality; the wire and the module "
	     "must agree");
BUILD_ASSERT(RADIANT_SEC_SW_VERDICT_EVENT == 0x10,
	     "0xF1 bit 4 asks for the 0xF8 window-verdict event; the wire and "
	     "the module must agree");

/* Bits 7..5 are reserved and must be zero. Refusing them is what keeps a future
 * switch from being silently ignored by a node too old to know it. Bit 4
 * without X_AUTH is refused by radiant_sec_configure(), which owns that rule
 * for every caller, not only this one. */
#define SEC_SW_KNOWN_MASK 0x1Fu

/*
 * The module's codes, in the serial protocol's vocabulary.
 *
 * ENOKEY becomes CHANNEL_IN_WRONG_STATE rather than INVALID_PARAMETER, because
 * it is not a bad parameter - it is a correct one sent too early, and a host
 * that cannot tell those apart retries the wrong thing.
 */
static antr_err_t sec_err(int rc)
{
	switch (rc) {
	case RADIANT_SEC_OK:
		return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
	case RADIANT_SEC_ENOKEY:
	case RADIANT_SEC_ESTATE:   /* a node's epoch authority follows it */
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	case RADIANT_SEC_ENOTSUP:
	case RADIANT_SEC_EINVAL:
	default:
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
}

/* Saturate rather than wrap. A counter that wrapped would let a long run look
 * quiet, and "quiet" is exactly the reading these counters exist to disprove. */
static void put_u16_sat(uint8_t *out, uint32_t v)
{
	uint16_t n = (v > 0xFFFFu) ? 0xFFFFu : (uint16_t)v;

	out[0] = (uint8_t)n;
	out[1] = (uint8_t)(n >> 8);
}

static void put_u32(uint8_t *out, uint32_t v)
{
	out[0] = (uint8_t)v;
	out[1] = (uint8_t)(v >> 8);
	out[2] = (uint8_t)(v >> 16);
	out[3] = (uint8_t)(v >> 24);
}

/* No get_u32()/get_u64() here on purpose: this file only ever encodes for the
 * wire. Decoding 0xF3's epoch and us_into_epoch happens in
 * ant_serial_bridge.c, where the frame still exists. */

/*
 * Every radiant_sec call runs under api_lock, like every other antr_* entry
 * point: the pump runs on the event thread under it, and the software AES
 * shares one key schedule that a preempting host command would re-expand
 * under another key. Each public function below validates, then calls its
 * _locked body with the lock held. radiant_sec asserts it in a debug build
 * (RADIANT_SEC_LOCK_CHECK).
 */

static antr_err_t sec_config_locked(uint8_t channel, uint8_t switches, uint8_t w,
				    uint8_t page_lo, uint8_t page_hi)
{
	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if ((switches & ~SEC_SW_KNOWN_MASK) != 0u) {
		/* A reserved bit set is a host speaking a later dialect. Refuse
		 * rather than mask: masking would enable the switches it DOES
		 * understand and silently drop the one it does not, which is
		 * the worst of both answers. */
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	return sec_err(radiant_sec_configure(channel, switches, w, page_lo,
					     page_hi));
}

static antr_err_t sec_key_set_locked(uint8_t channel, uint8_t bits,
				     const uint8_t *key)
{
	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT || key == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}

	/*
	 * base_devnum is read from the channel rather than carried on the
	 * wire, so 0xF2 moves exactly sixteen bytes. The key must be
	 * installed AFTER the channel ID: base_devnum is bound into the KDF,
	 * so keying first would derive under 0. A wildcard ID is refused
	 * since it can't be a provisioning identity.
	 */
	struct radiant_channel_id id;
	int                       rc;

	if (radiant_channel_id_get(channel, &id) != RADIANT_CH_OK) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}
	if (id.device_number == 0u) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}

	rc = radiant_sec_set_key(channel, key, (size_t)bits, id.device_number);
	if (rc != RADIANT_SEC_OK) {
		return sec_err(rc);
	}

	/* The on-air number starts equal to the provisioning one. A Tier 2
	 * receiver moves it with radiant_sec_try_devnum() and never touches the
	 * base. */
	(void)radiant_sec_set_devnum(channel, id.device_number);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

static antr_err_t sec_epoch_set_locked(uint8_t channel, uint8_t flags,
				       uint32_t epoch, uint64_t us_into_epoch)
{
	uint16_t period;

	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	/* Bit 0 says the epoch is coarse real time vs. a bare ordinal; nothing
	 * here behaves differently either way since both are monotonic. Every
	 * other bit is reserved. */
	if ((flags & (uint8_t)~0x01u) != 0u) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	/* The channel period, read rather than sent: a host-supplied value
	 * that disagreed would desynchronise the counter in a way that looks
	 * like clock drift. */
	if (radiant_channel_period_get(channel, &period) != RADIANT_CH_OK) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}

	return sec_err(radiant_sec_set_epoch(channel, epoch, us_into_epoch,
					     period));
}

antr_err_t antr_sec_config(uint8_t channel, uint8_t switches, uint8_t w,
			   uint8_t page_lo, uint8_t page_hi)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = sec_config_locked(channel, switches, w, page_lo, page_hi);
	k_mutex_unlock(&api_lock);
	return rc;
}

antr_err_t antr_sec_key_set(uint8_t channel, uint8_t bits, const uint8_t *key)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = sec_key_set_locked(channel, bits, key);
	k_mutex_unlock(&api_lock);
	return rc;
}

antr_err_t antr_sec_epoch_set(uint8_t channel, uint8_t flags, uint32_t epoch,
			      uint64_t us_into_epoch)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = sec_epoch_set_locked(channel, flags, epoch, us_into_epoch);
	k_mutex_unlock(&api_lock);
	return rc;
}

antr_err_t antr_sec_status_get(uint8_t channel, uint8_t *out, uint8_t out_len)
{
	struct radiant_sec_state state;
	struct radiant_sec_stats stats;

	if (out == NULL || out_len < ANTR_SEC_STATUS_LEN) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}

	/* An unkeyed channel answers with zeros and CLEAR rather than erroring
	 * - "no security here" is a legitimate state, not a fault. The version
	 * byte is set either way: it describes the layout, not the channel. */
	k_mutex_lock(&api_lock, K_FOREVER);
	radiant_sec_get_state(channel, &state);
	radiant_sec_get_stats(channel, &stats);
	k_mutex_unlock(&api_lock);

	memset(out, 0, ANTR_SEC_STATUS_LEN);
	out[0] = channel;
	out[1] = (uint8_t)ANTR_SEC_STATUS_VERSION;
	out[2] = state.switches;
	out[3] = state.w;
	out[4] = state.page_lo;
	out[5] = state.page_hi;
	put_u32(&out[6], state.epoch);
	out[10] = (uint8_t)state.expected_index;
	out[11] = (uint8_t)(state.expected_index >> 8);
	put_u16_sat(&out[12], stats.windows_verified);
	put_u16_sat(&out[14], stats.windows_unverified);
	put_u16_sat(&out[16], stats.dropped_non_broadcast);
	put_u16_sat(&out[18], stats.dropped_replay);
	put_u16_sat(&out[20], stats.dropped_policy);
	out[22] = (uint8_t)((stats.epoch_advances > 0xFFu)
				    ? 0xFFu
				    : stats.epoch_advances);
	out[23] = (uint8_t)state.last_verdict;
	/* Pairings that took - the 0xF5 host exchange and the over-air window
	 * alike. An enrolment the owner did not perform is the attack this
	 * exists to show. */
	put_u16_sat(&out[24], stats.enrolments);
	/* Secured-range broadcasts a master heard and refused (radiant-security.md
	 * 7.6): forgeries, or its own windows reflected back. */
	put_u16_sat(&out[26], stats.dropped_direction);
	/* 0xF8 events the event ring refused. A USB drop and a window discard
	 * show only as 0xF8 seq gaps, never here. */
	put_u16_sat(&out[28], stats.verdict_events_dropped);

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

/* The wire's role byte is the core's role constant, so it passes straight
 * through. */
BUILD_ASSERT(RADIANT_SEC_PAIR_INITIATOR == 0u &&
	     RADIANT_SEC_PAIR_RESPONDER == 1u,
	     "0xF5 NONCE carries role 0 = initiator, 1 = responder");

/*
 * 0xF5, the pairing exchange. Sub-commands rather than seven message IDs,
 * since they are one conversation with an order: enter, supply a scalar,
 * exchange keys, commit to and reveal nonces, leave. The order is enforced
 * by radiant_sec_pair.c, not here. The reply echoes the sub-command at [1]
 * so a host can tell which one answered.
 *
 * There is no sub-command that installs a key without the commit/reveal:
 * radiant_sec_pair_peer_uncommitted() is over-air enrolment's and is not
 * reachable from the host, so the grindable path is not a downgrade option.
 */
/* Each sub-command's exact argument length, indexed by sub-command. One
 * table rather than a check per case: the pairing build has its own size
 * budget (release.yml, "pairing text"). ENTER's timeout byte is optional. */
static const uint8_t pair_arg_len[] = {
	[ANTW_RADIANT_PAIR_LEAVE]       = 0u,
	[ANTW_RADIANT_PAIR_ENTER]       = 1u,  /* own path below: 0 or 1 byte */
	[ANTW_RADIANT_PAIR_SCALAR]      = RADIANT_SEC_X25519_BYTES,
	[ANTW_RADIANT_PAIR_EXCHANGE]    = RADIANT_SEC_X25519_BYTES,
	[ANTW_RADIANT_PAIR_NONCE]       = 1u + RADIANT_SEC_PAIR_NONCE_BYTES,
	[ANTW_RADIANT_PAIR_PEER_COMMIT] = RADIANT_SEC_BLOCK_BYTES,
	[ANTW_RADIANT_PAIR_PEER_NONCE]  = RADIANT_SEC_PAIR_NONCE_BYTES,
};

static antr_err_t sec_pair_locked(uint8_t channel, uint8_t subcmd,
				  const uint8_t *arg, uint8_t arg_len,
				  uint8_t *reply, uint8_t reply_cap,
				  uint8_t *reply_len)
{
	uint32_t fp = 0u;
	int      rc;

	/* The reply buffer must hold the largest answer (SCALAR's key), so no
	 * sub-command can fail for want of room after it has changed state. */
	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT || reply == NULL ||
	    reply_len == NULL || reply_cap < 2u + RADIANT_SEC_X25519_BYTES) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (subcmd >= ARRAY_SIZE(pair_arg_len)) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
	if (((subcmd == ANTW_RADIANT_PAIR_ENTER)
		     ? (arg_len > 1u)
		     : (arg_len != pair_arg_len[subcmd])) ||
	    (arg_len != 0u && arg == NULL)) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}

	reply[0] = channel;
	reply[1] = subcmd;
	*reply_len = 2u;

	switch (subcmd) {
	case ANTW_RADIANT_PAIR_LEAVE:
		radiant_sec_pair_leave(channel);
		rc = RADIANT_SEC_OK;
		break;

	case ANTW_RADIANT_PAIR_ENTER:
		/* arg[0] is the timeout in seconds; 0 means the default, never
		 * "forever" - a node in pairing mode accepts a key from whoever
		 * asks, so one forgotten command must not leave it open. */
		rc = radiant_sec_pair_enter(channel,
					    (arg != NULL && arg_len >= 1u)
						    ? arg[0]
						    : 0u);
		break;

	case ANTW_RADIANT_PAIR_SCALAR:
		/* Answer with the public key it produced, which is what has to
		 * go on the air next. */
		rc = radiant_sec_pair_set_scalar(channel, arg);
		if (rc == RADIANT_SEC_OK) {
			rc = radiant_sec_pair_local_pubkey(channel, &reply[2]);
			*reply_len = (uint8_t)(2u + RADIANT_SEC_X25519_BYTES);
		}
		break;

	case ANTW_RADIANT_PAIR_EXCHANGE:
		/* DH only: no key, no fingerprint until the nonces cross. */
		rc = radiant_sec_pair_peer(channel, arg);
		break;

	case ANTW_RADIANT_PAIR_NONCE:
		/* [role, nonce16]. A responder answers with its commitment,
		 * which is what goes on the air next. */
		rc = radiant_sec_pair_set_nonce(channel, arg[0], &arg[1],
						&reply[2]);
		if (arg[0] == RADIANT_SEC_PAIR_RESPONDER) {
			*reply_len = (uint8_t)(2u + RADIANT_SEC_BLOCK_BYTES);
		}
		break;

	case ANTW_RADIANT_PAIR_PEER_COMMIT:
		rc = radiant_sec_pair_peer_commit(channel, arg);
		break;

	default: /* ANTW_RADIANT_PAIR_PEER_NONCE */
		rc = radiant_sec_pair_peer_nonce(channel, arg, &fp);
		/* The comparison fingerprint, six digits in a u24 LE: the only
		 * defence against a MITM, and only if a human compares it. */
		reply[2] = (uint8_t)fp;
		reply[3] = (uint8_t)(fp >> 8);
		reply[4] = (uint8_t)(fp >> 16);
		*reply_len = 5u;
		break;
	}
	/* A reveal that does not open the commitment is the man-in-the-middle
	 * signature, so it gets a code of its own rather than the generic
	 * refusal an out-of-order call gets. The bridge sends the reply only
	 * on success, so a refused sub-command's reply_len is never read. */
	if (rc == RADIANT_SEC_EBADMAC) {
		return (antr_err_t)ANTW_RADIANT_PAIR_COMMIT_MISMATCH;
	}
	return sec_err(rc);
}

/* The X25519 exchange (0xF5 EXCHANGE, and SCALAR for the public key) holds
 * api_lock for its whole duration, like the rest; see the hold-time
 * measurement in radiant/tests/api/src/test_api_sec_host.c. */
antr_err_t antr_sec_pair(uint8_t channel, uint8_t subcmd, const uint8_t *arg,
			 uint8_t arg_len, uint8_t *reply, uint8_t reply_cap,
			 uint8_t *reply_len)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = sec_pair_locked(channel, subcmd, arg, arg_len, reply, reply_cap,
			     reply_len);
	k_mutex_unlock(&api_lock);
	return rc;
}

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
