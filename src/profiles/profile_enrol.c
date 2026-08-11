/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_enrol.c - Layer D: adding a receiver to an existing network.
 *
 * Provenance: docs/radiant-security.md sections 11.5, 11.6 and 11.7,
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md and
 * docs/decisions/0009-hostless-node-identity.md. All of those are this
 * project's own documents; no adopter-gated ANT+ device profile document was
 * read for this file, no sdk-ant source was consulted, and nothing here
 * derives from libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * The header carries the argument for every decision in here, including the
 * one this file must not be read as having solved: a strap has no screen, so
 * the fingerprint is one-sided, and `closed` remains the recommendation for
 * anything that matters.
 *
 * This is the SECOND file in src/profiles/ that includes a radiant_core
 * header, and it is for the same reason profile_compat.c is the first: adding
 * a keyholder is a key operation and something has to hold the key. What it
 * does NOT do is name a page number or a frame index - profile_compat.c owns
 * byte [0] and byte [1] of every frame this module contributes - so the two
 * boundaries cross without either one bending.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_sec.h>

#include "profile_compat.h"
#include "profile_enrol.h"

#if defined(CONFIG_RADIANT_SEC) && defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

/*
 * The two numbers this file restates rather than includes, checked rather than
 * trusted. Restated because profile_enrol.h must not name a radiant_core type
 * and a header that pulled one in would put a key type on profile_hr.c's
 * include path the first time somebody was tidy.
 */
_Static_assert(PROFILE_ENROL_PUBKEY_BYTES == RADIANT_SEC_X25519_BYTES,
	       "the public key on the air and the one radiant_sec produces "
	       "must be the same length");
_Static_assert(PROFILE_ENROL_TIMEOUT_DEFAULT_S ==
		       RADIANT_SEC_PAIR_TIMEOUT_DEFAULT_S,
	       "the window this module bounds and the window radiant_sec "
	       "bounds must be the same window");
_Static_assert(PROFILE_ENROL_SET_BYTES ==
		       PROFILE_ENROL_PUBKEY_BYTES + PROFILE_ENROL_CHECK_BYTES,
	       "six frames of six bytes is exactly a key plus its set check; "
	       "a spare byte here is a byte nobody has decided the meaning of");

/* ── The set check ──────────────────────────────────────────────────────────
 *
 * trunc32( CMAC(all-zero key, pubkey) ). Integrity, not authentication - the
 * header says at length what it catches (a set spliced across two windows) and
 * what it cannot (a man in the middle, which is the fingerprint's job).
 */
static int set_check(const uint8_t *pubkey, uint8_t *out)
{
	struct radiant_sec_key zero_key;
	uint8_t                zero[RADIANT_SEC_KEY_MAX_BYTES];
	uint8_t                tag[RADIANT_SEC_BLOCK_BYTES];
	int                    rc;

	memset(zero, 0, sizeof(zero));
	memset(&zero_key, 0, sizeof(zero_key));

	rc = radiant_sec_key_import(&zero_key, zero, 128u);
	if (rc != RADIANT_SEC_OK) {
		return -ENOTSUP;
	}
	rc = radiant_sec_cmac(&zero_key, pubkey, PROFILE_ENROL_PUBKEY_BYTES,
			      tag);
	radiant_sec_key_destroy(&zero_key);
	if (rc != RADIANT_SEC_OK) {
		return -ENOTSUP;
	}

	memcpy(out, tag, PROFILE_ENROL_CHECK_BYTES);
	memset(tag, 0, sizeof(tag));
	return 0;
}

/* ── The window ─────────────────────────────────────────────────────────── */

/* Leave pairing mode, drop the half-assembled peer key, clear the beacon bit.
 * One function so that completion, timeout and an explicit close cannot drift
 * into clearing different subsets - the same argument radiant_sec_pair.c's
 * ctx_free() makes one layer down. */
static void window_close(struct profile_enrol *pe)
{
	if (!pe->open) {
		return;
	}
	pe->open = false;
	pe->rx_have = 0u;
	memset(pe->rx, 0, sizeof(pe->rx));
	memset(pe->tx, 0, sizeof(pe->tx));

	if (pe->cfg.close_pairing != NULL) {
		pe->cfg.close_pairing(pe->cfg.user, pe->cfg.ch);
	}
	if (pe->compat != NULL) {
		(void)profile_compat_set_pairing_open(pe->compat, false);
	}
}

static int window_open(struct profile_enrol *pe, uint64_t now_us)
{
	uint8_t timeout_s = pe->cfg.timeout_s;
	int     rc;

	if (timeout_s == 0u) {
		timeout_s = PROFILE_ENROL_TIMEOUT_DEFAULT_S;
	}

	/*
	 * The scalar and the public key come from the node, through the seam,
	 * and the counter-advance-before-transmission rule is enforced on that
	 * side. If it refuses, NOTHING here changes state: no window, no
	 * frames, no beacon bit. A burnt pair counter is not rolled back, which
	 * is the cheap half of ADR 0009's trade.
	 */
	rc = pe->cfg.open_pairing(pe->cfg.user, pe->cfg.ch, timeout_s, pe->tx);
	if (rc != 0) {
		memset(pe->tx, 0, sizeof(pe->tx));
		pe->windows_refused++;
		return rc;
	}

	rc = set_check(pe->tx, &pe->tx[PROFILE_ENROL_PUBKEY_BYTES]);
	if (rc != 0) {
		memset(pe->tx, 0, sizeof(pe->tx));
		if (pe->cfg.close_pairing != NULL) {
			pe->cfg.close_pairing(pe->cfg.user, pe->cfg.ch);
		}
		pe->windows_refused++;
		return rc;
	}

	pe->open = true;
	pe->opened_us = now_us;
	pe->window_us = (uint64_t)timeout_s * 1000000ull;
	pe->ever_opened = true;
	pe->last_open_us = now_us;
	pe->rx_have = 0u;
	pe->have_fp = false;
	pe->windows_opened++;

	/*
	 * The bit that makes an enrolment visible to the receivers that already
	 * exist. An enrolment the owner did not perform is the whole attack, so
	 * a window that opened silently would be the mistake - section 11.7,
	 * rule three.
	 */
	if (pe->compat != NULL) {
		(void)profile_compat_set_pairing_open(pe->compat, true);
	}
	return 0;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

int profile_enrol_init(struct profile_enrol *pe,
		       const struct profile_enrol_cfg *cfg)
{
	if (pe == NULL || cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->mode != (uint8_t)PROFILE_ENROL_CLOSED &&
	    cfg->mode != (uint8_t)PROFILE_ENROL_PHYSICAL &&
	    cfg->mode != (uint8_t)PROFILE_ENROL_OPEN_WINDOW) {
		return -EINVAL;
	}
	/*
	 * Both callbacks are required in EVERY mode, `closed` included. A
	 * `closed` node that was refusing because it had been misconfigured
	 * would be indistinguishable from one refusing on policy, and the
	 * second is a decision while the first is a bug.
	 */
	if (cfg->open_pairing == NULL || cfg->close_pairing == NULL) {
		return -EINVAL;
	}

	memset(pe, 0, sizeof(*pe));
	pe->cfg = *cfg;
	pe->armed = true;
	return 0;
}

int profile_enrol_attach(struct profile_enrol *pe, struct profile_compat *pc)
{
	struct profile_compat_client client;
	int                          rc;

	if (pe == NULL || pc == NULL || !pe->armed) {
		return -EINVAL;
	}

	client.frames = profile_enrol_frame_count;
	client.frame = profile_enrol_frame;
	client.user = pe;

	rc = profile_compat_set_client(pc, &client);
	if (rc != 0) {
		return rc;
	}
	pe->compat = pc;
	/* Whatever the beacon was built with, the truth is what this module
	 * knows: a node that initialised with pairing_open set and no window
	 * open would advertise a window nobody could use. */
	return profile_compat_set_pairing_open(pc, pe->open);
}

/* ── Opening ────────────────────────────────────────────────────────────── */

int profile_enrol_physical_action(struct profile_enrol *pe, uint64_t now_us)
{
	if (pe == NULL || !pe->armed) {
		return -EINVAL;
	}
	(void)profile_enrol_tick(pe, now_us);

	if (pe->cfg.mode == (uint8_t)PROFILE_ENROL_CLOSED) {
		/*
		 * `closed` means no over-air enrolment EVER. Not "unless
		 * somebody holds the button down", not "unless a keyholder
		 * asks". Counted, because a node whose button is being pressed
		 * repeatedly is a node somebody is trying to enrol against.
		 */
		pe->windows_refused++;
		return -EPERM;
	}
	if (pe->open) {
		/* ONE PAIRING PER WINDOW, and a second press does not extend
		 * the first - an extension is an unbounded window reached one
		 * press at a time. */
		pe->windows_refused++;
		return -EBUSY;
	}
	return window_open(pe, now_us);
}

int profile_enrol_keyholder_request(struct profile_enrol *pe, uint64_t now_us)
{
	uint64_t min_us;

	if (pe == NULL || !pe->armed) {
		return -EINVAL;
	}
	(void)profile_enrol_tick(pe, now_us);

	if (pe->cfg.mode != (uint8_t)PROFILE_ENROL_OPEN_WINDOW) {
		pe->windows_refused++;
		return -EPERM;
	}
	if (pe->open) {
		pe->windows_refused++;
		return -EBUSY;
	}

	min_us = (uint64_t)(pe->cfg.open_window_min_s != 0u
				    ? pe->cfg.open_window_min_s
				    : PROFILE_ENROL_OPEN_WINDOW_MIN_S) *
		 1000000ull;

	/*
	 * THE RATE LIMIT, and it is measured from the last window's OPENING
	 * rather than its closing. Measuring from the close would let a caller
	 * that completes an enrolment in one second reopen 299 seconds later,
	 * so the interval between two windows would depend on how long the
	 * previous one took - which is exactly the input an attacker controls.
	 */
	if (pe->ever_opened && (now_us - pe->last_open_us) < min_us) {
		pe->windows_refused++;
		return -EAGAIN;
	}
	return window_open(pe, now_us);
}

void profile_enrol_close(struct profile_enrol *pe)
{
	if (pe != NULL && pe->armed) {
		window_close(pe);
	}
}

bool profile_enrol_is_open(const struct profile_enrol *pe)
{
	return pe != NULL && pe->armed && pe->open;
}

bool profile_enrol_tick(struct profile_enrol *pe, uint64_t now_us)
{
	if (pe == NULL || !pe->armed || !pe->open) {
		return false;
	}
	/* Unsigned, and the clock is 64-bit microseconds handed in by the
	 * caller, so there is no wrap to survive here - unlike the 32-bit
	 * millisecond deadline radiant_sec_pair.c has to. */
	if ((now_us - pe->opened_us) < pe->window_us) {
		return false;
	}
	pe->windows_expired++;
	window_close(pe);
	return true;
}

/* ── The frames ─────────────────────────────────────────────────────────── */

uint8_t profile_enrol_frame_count(void *user, uint64_t now_us)
{
	struct profile_enrol *pe = (struct profile_enrol *)user;

	if (pe == NULL || !pe->armed) {
		return 0u;
	}
	/* Expiry lives here so that a node whose frames go through
	 * profile_compat.c owes no separate tick, and so that the frame count
	 * and the open state can never disagree by one slot. */
	(void)profile_enrol_tick(pe, now_us);
	return pe->open ? (uint8_t)PROFILE_ENROL_FRAMES : 0u;
}

bool profile_enrol_frame(void *user, uint8_t i, uint8_t *payload)
{
	struct profile_enrol *pe = (struct profile_enrol *)user;

	if (pe == NULL || !pe->armed || !pe->open || payload == NULL) {
		return false;
	}
	if (i >= (uint8_t)PROFILE_ENROL_FRAMES) {
		return false;
	}
	memcpy(payload, &pe->tx[(size_t)i * PROFILE_ENROL_FRAME_PAYLOAD],
	       PROFILE_ENROL_FRAME_PAYLOAD);
	pe->frames_sent++;
	return true;
}

/* ── The peer's key ─────────────────────────────────────────────────────── */

int profile_enrol_on_ack_data(struct profile_enrol *pe, const uint8_t *payload,
			      uint8_t len)
{
	uint8_t  check[PROFILE_ENROL_CHECK_BYTES];
	uint8_t  index;
	uint8_t  count;
	uint32_t fp = 0u;
	int      rc;

	if (pe == NULL || !pe->armed || payload == NULL ||
	    len != (uint8_t)(PROFILE_ENROL_FRAME_PAYLOAD + 2u)) {
		return -EINVAL;
	}
	if (!pe->open) {
		/*
		 * A key arriving with no window open is not an error the node
		 * can act on and is not silently absorbed either: it is exactly
		 * what "one pairing per window" looks like from the outside
		 * when a second peer answers a window the first one already
		 * closed.
		 */
		pe->rx_rejected++;
		return -EINVAL;
	}

	/* Byte [0] is a page number and this file names none - the caller has
	 * already filtered on it. Byte [1] is the convention. */
	index = (uint8_t)(payload[1] >> 4);
	count = (uint8_t)((payload[1] & 0x0Fu) + 1u);
	if (count != (uint8_t)PROFILE_ENROL_FRAMES ||
	    index >= (uint8_t)PROFILE_ENROL_FRAMES) {
		pe->rx_rejected++;
		return -EPROTO;
	}

	memcpy(&pe->rx[(size_t)index * PROFILE_ENROL_FRAME_PAYLOAD],
	       &payload[2], PROFILE_ENROL_FRAME_PAYLOAD);
	pe->rx_have |= (uint8_t)(1u << index);
	pe->rx_frames++;

	if (pe->rx_have != (uint8_t)((1u << PROFILE_ENROL_FRAMES) - 1u)) {
		return 0;
	}

	rc = set_check(pe->rx, check);
	if (rc != 0) {
		pe->rx_have = 0u;
		pe->rx_rejected++;
		return rc;
	}
	if (memcmp(check, &pe->rx[PROFILE_ENROL_PUBKEY_BYTES],
		   PROFILE_ENROL_CHECK_BYTES) != 0) {
		/*
		 * The six frames did not come from one key. The commonest way
		 * for that to happen is honest - a receiver that joined as one
		 * window closed and another opened - so the accumulator starts
		 * again and the window stays open.
		 */
		pe->rx_have = 0u;
		memset(pe->rx, 0, sizeof(pe->rx));
		pe->rx_rejected++;
		return -EBADMSG;
	}

	rc = radiant_sec_pair_peer(pe->cfg.ch, pe->rx, &fp);
	if (rc != RADIANT_SEC_OK) {
		/*
		 * Refused - a small-order point is the case that matters, and
		 * refusing it is mandatory rather than optional because the
		 * result becomes a root key. ONE INJECTED PACKET MUST NOT BURN
		 * A WINDOW the user opened, so the window stays open and only
		 * the accumulator is reset.
		 */
		pe->rx_have = 0u;
		memset(pe->rx, 0, sizeof(pe->rx));
		pe->rx_rejected++;
		return -EACCES;
	}

	pe->fingerprint = fp;
	pe->have_fp = true;
	pe->enrolments++;

	/*
	 * ONE PAIRING PER WINDOW. Closing here rather than at the timeout is
	 * what makes that structural: the frames stop, the beacon bit drops,
	 * pairing mode is left, and a second peer answering the same window
	 * finds no window. The fingerprint survives the close because it is
	 * this module's, not the pairing context's.
	 */
	window_close(pe);
	return 1;
}

int profile_enrol_fingerprint(const struct profile_enrol *pe, uint32_t *out)
{
	if (pe == NULL || out == NULL || !pe->have_fp) {
		return -EINVAL;
	}
	*out = pe->fingerprint;
	return 0;
}

#else /* no security, or no X25519 */

/*
 * The refusing shape.
 *
 * A build with no pairing has no way to complete an exchange, so the honest
 * behaviour is the behaviour of `closed` - and it is reached by refusing at
 * init rather than by broadcasting a public key that can never be answered.
 */

int profile_enrol_init(struct profile_enrol *pe,
		       const struct profile_enrol_cfg *cfg)
{
	if (pe != NULL) {
		memset(pe, 0, sizeof(*pe));
	}
	(void)cfg;
	return -ENOTSUP;
}

int profile_enrol_attach(struct profile_enrol *pe, struct profile_compat *pc)
{
	(void)pe;
	(void)pc;
	return -ENOTSUP;
}

int profile_enrol_physical_action(struct profile_enrol *pe, uint64_t now_us)
{
	(void)pe;
	(void)now_us;
	return -ENOTSUP;
}

int profile_enrol_keyholder_request(struct profile_enrol *pe, uint64_t now_us)
{
	(void)pe;
	(void)now_us;
	return -ENOTSUP;
}

void profile_enrol_close(struct profile_enrol *pe)
{
	(void)pe;
}

bool profile_enrol_is_open(const struct profile_enrol *pe)
{
	(void)pe;
	return false;
}

bool profile_enrol_tick(struct profile_enrol *pe, uint64_t now_us)
{
	(void)pe;
	(void)now_us;
	return false;
}

uint8_t profile_enrol_frame_count(void *user, uint64_t now_us)
{
	(void)user;
	(void)now_us;
	return 0u;
}

bool profile_enrol_frame(void *user, uint8_t i, uint8_t *payload)
{
	(void)user;
	(void)i;
	(void)payload;
	return false;
}

int profile_enrol_on_ack_data(struct profile_enrol *pe, const uint8_t *payload,
			      uint8_t len)
{
	(void)pe;
	(void)payload;
	(void)len;
	return -ENOTSUP;
}

int profile_enrol_fingerprint(const struct profile_enrol *pe, uint32_t *out)
{
	(void)pe;
	(void)out;
	return -EINVAL;
}

#endif /* CONFIG_RADIANT_SEC && CONFIG_RADIANT_SEC_PAIRING_X25519 */
