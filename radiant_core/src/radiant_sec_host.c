/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The host surface for the two payload transforms: messages 0xF1-0xF4, as
 * antr_* entry points.
 *
 * This is the one security file that reaches an application header. Everything
 * else in radiant_sec depends on nothing above radiant_core; this file exists
 * precisely to be the seam between the module's own return codes and the
 * ANT serial protocol's, and putting that translation anywhere else would put
 * ANTW_* constants into a module that has no business knowing them.
 *
 * ---------------------------------------------------------------------------
 * WHY THE SWITCH BITS ARE NOT TRANSLATED
 * ---------------------------------------------------------------------------
 * RADIANT_SEC_SW_CONF/AUTH/DROP_UNVER/DESC_CONF are 0x01/0x02/0x04/0x08, and
 * 0xF1's bitmask is bit 0 X_CONF, bit 1 X_AUTH, bit 2 drop-unverified, bit 3
 * descriptor confidentiality. They are the same byte, on purpose, and the
 * BUILD_ASSERTs below are what keep them that way - a mapping table here would
 * be a second definition of the wire format, free to drift from the first, and
 * the drift would be silent because both halves would still be self-consistent.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE KEY PATH PROMISES, AND WHAT IT CANNOT
 * ---------------------------------------------------------------------------
 * antr_sec_key_set() is write-only: nothing here reads a key back, there is no
 * read arm for 0xF2 anywhere, and MESG_REQUEST for it answers
 * ANTW_INVALID_MESSAGE rather than a key.
 *
 * The caller is expected to wipe its own copy of the message body after
 * dispatching one - src/ant_serial_bridge.c does. That wipe is worth having and
 * is NOT sufficient, and saying so here is more useful than implying otherwise:
 * the bytes also passed through the USB stack's ring buffer and whatever
 * transport buffer sits under it, and neither is reachable from here. A root key
 * that has crossed a USB cable should be treated as having been in host memory,
 * because it has.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <radiant_core/radiant_sec.h>
#include <radiant_core/radiant_channel.h>

#include "ant_radio.h"
#include "ant_wire.h"

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

/* Bits 7..4 are reserved and must be zero. Refusing them is what keeps a future
 * switch from being silently ignored by a node too old to know it. */
#define SEC_SW_KNOWN_MASK 0x0Fu

/* 0xF4's fixed payload length. */
#define SEC_STATUS_LEN 23u

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

static uint32_t get_u32(const uint8_t *in)
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
	       ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static uint64_t get_u64(const uint8_t *in)
{
	return (uint64_t)get_u32(in) | ((uint64_t)get_u32(&in[4]) << 32);
}

antr_err_t antr_sec_config(uint8_t channel, uint8_t switches, uint8_t w,
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

antr_err_t antr_sec_key_set(uint8_t channel, uint8_t bits, const uint8_t *key)
{
	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT || key == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}

	/*
	 * The base device number is the channel's own, read here rather than
	 * carried on the wire. 0xF2 moves exactly sixteen bytes and nothing
	 * else, and a device number the host had to repeat correctly would be a
	 * second place for it to be wrong.
	 *
	 * This is also why the key must be installed AFTER the channel ID:
	 * base_devnum is bound into the KDF, so keying first and setting the ID
	 * afterwards would derive keys under 0. Stated in the header; enforced
	 * here only to the extent that a wildcard ID is refused, because a
	 * wildcard cannot be the provisioning identity of anything.
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

antr_err_t antr_sec_epoch_set(uint8_t channel, uint8_t flags, uint32_t epoch,
			      uint64_t us_into_epoch)
{
	uint16_t period;

	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	/*
	 * Bit 0 says the epoch is coarse real time rather than a bare ordinal.
	 * Nothing here behaves differently either way - monotonicity is the only
	 * property the module needs and both encodings have it - so the bit is
	 * accepted and carried by the host's own bookkeeping. Every other bit is
	 * reserved.
	 */
	if ((flags & (uint8_t)~0x01u) != 0u) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	/*
	 * The channel period, read rather than sent. It is what turns a phase
	 * into a packet index, radiant_channel already holds the authoritative
	 * value, and a host-supplied one that disagreed would desynchronise the
	 * counter in a way that looks exactly like clock drift.
	 */
	if (radiant_channel_period_get(channel, &period) != RADIANT_CH_OK) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}

	return sec_err(radiant_sec_set_epoch(channel, epoch, us_into_epoch,
					     period));
}

antr_err_t antr_sec_status_get(uint8_t channel, uint8_t *out, uint8_t out_len)
{
	struct radiant_sec_state state;
	struct radiant_sec_stats stats;

	if (out == NULL || out_len < SEC_STATUS_LEN) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}

	/*
	 * An unkeyed channel answers rather than erroring, with zeros and a
	 * CLEAR verdict. "No security on this channel" is a legitimate state a
	 * host needs to be able to read, and making it an error would force a
	 * host to treat an ordinary channel as a fault.
	 */
	radiant_sec_get_state(channel, &state);
	radiant_sec_get_stats(channel, &stats);

	memset(out, 0, SEC_STATUS_LEN);
	out[0] = channel;
	out[1] = state.switches;
	out[2] = state.w;
	out[3] = state.page_lo;
	out[4] = state.page_hi;
	put_u32(&out[5], state.epoch);
	out[9] = (uint8_t)state.expected_index;
	out[10] = (uint8_t)(state.expected_index >> 8);
	put_u16_sat(&out[11], stats.windows_verified);
	put_u16_sat(&out[13], stats.windows_unverified);
	put_u16_sat(&out[15], stats.dropped_non_broadcast);
	put_u16_sat(&out[17], stats.dropped_replay);
	put_u16_sat(&out[19], stats.dropped_policy);
	out[21] = (uint8_t)((stats.epoch_advances > 0xFFu)
				    ? 0xFFu
				    : stats.epoch_advances);
	out[22] = (uint8_t)state.last_verdict;

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

/*
 * 0xF5, the pairing exchange.
 *
 * Sub-commands rather than four message IDs, because they are one conversation
 * with an order: enter, supply a scalar, exchange, leave. Separate IDs would
 * have let a host skip a step and get a state error it could not localise.
 *
 * The reply echoes the sub-command at [1] so a host reading a stream of them
 * can tell which one answered - the local public key and the fingerprint are
 * both "32-ish bytes after a channel byte" otherwise.
 */
antr_err_t antr_sec_pair(uint8_t channel, uint8_t subcmd, const uint8_t *arg,
			 uint8_t arg_len, uint8_t *reply, uint8_t reply_cap,
			 uint8_t *reply_len)
{
	uint32_t fp = 0u;
	int      rc;

	if (channel >= (uint8_t)RADIANT_CHANNEL_COUNT || reply == NULL ||
	    reply_len == NULL || reply_cap < 2u) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}

	reply[0] = channel;
	reply[1] = subcmd;
	*reply_len = 2u;

	switch (subcmd) {
	case ANTW_RADIANT_PAIR_LEAVE:
		radiant_sec_pair_leave(channel);
		return (antr_err_t)ANTW_RESPONSE_NO_ERROR;

	case ANTW_RADIANT_PAIR_ENTER:
		/* arg[0] is the timeout in seconds; 0 means the default, never
		 * "forever" - a node in pairing mode accepts a key from whoever
		 * asks, so one forgotten command must not leave it open. */
		rc = radiant_sec_pair_enter(channel,
					    (arg != NULL && arg_len >= 1u)
						    ? arg[0]
						    : 0u);
		return sec_err(rc);

	case ANTW_RADIANT_PAIR_SCALAR:
		if (arg == NULL || arg_len < RADIANT_SEC_X25519_BYTES) {
			return (antr_err_t)ANTW_INVALID_MESSAGE;
		}
		rc = radiant_sec_pair_set_scalar(channel, arg);
		if (rc != RADIANT_SEC_OK) {
			return sec_err(rc);
		}
		/* Answer with the public key it produced, which is what has to
		 * go on the air next. */
		if (reply_cap < 2u + RADIANT_SEC_X25519_BYTES) {
			return (antr_err_t)ANTW_INVALID_MESSAGE;
		}
		rc = radiant_sec_pair_local_pubkey(channel, &reply[2]);
		if (rc != RADIANT_SEC_OK) {
			return sec_err(rc);
		}
		*reply_len = (uint8_t)(2u + RADIANT_SEC_X25519_BYTES);
		return (antr_err_t)ANTW_RESPONSE_NO_ERROR;

	case ANTW_RADIANT_PAIR_EXCHANGE:
		if (arg == NULL || arg_len < RADIANT_SEC_X25519_BYTES) {
			return (antr_err_t)ANTW_INVALID_MESSAGE;
		}
		rc = radiant_sec_pair_peer(channel, arg, &fp);
		if (rc != RADIANT_SEC_OK) {
			return sec_err(rc);
		}
		if (reply_cap < 5u) {
			return (antr_err_t)ANTW_INVALID_MESSAGE;
		}
		/*
		 * The comparison fingerprint, six digits in a u24 LE. THIS IS
		 * THE ONLY DEFENCE AGAINST A MAN IN THE MIDDLE and it only
		 * works if a human actually compares it against the other end's
		 * - the exchange itself succeeds either way, which is what an
		 * anonymous key agreement is.
		 */
		reply[2] = (uint8_t)(fp & 0xFFu);
		reply[3] = (uint8_t)((fp >> 8) & 0xFFu);
		reply[4] = (uint8_t)((fp >> 16) & 0xFFu);
		*reply_len = 5u;
		return (antr_err_t)ANTW_RESPONSE_NO_ERROR;

	default:
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
}

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
