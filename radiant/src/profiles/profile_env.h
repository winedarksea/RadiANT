/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_env.h - ANT+ Environment, device type 0x19.
 *
 * ---------------------------------------------------------------------------
 * Provenance
 * ---------------------------------------------------------------------------
 * Transcribed from the PRIMARY document: "ANT+ Managed Network Document -
 * Environment Device Profile", D00001502 Rev 1.0 (marked DEPRECATED by the ANT
 * team, which is a support statement, not a correctness one - the sensors are
 * on the market and this is the layout they transmit). Sections cited below
 * are that document's: §5.1 channel configuration, §6.3 data page 0, §6.3.1
 * transmission info, §6.3.2 supported pages, §6.4 data page 1, §6.5 the
 * reserved range.
 *
 * This supersedes docs/device-profiles.md §3.7, which recorded the same layout
 * from four converging open-source implementations and carried a "not derived
 * from a primary spec" caveat. The caveat can be lifted: the four agreed with
 * Table 6-4 byte for byte.
 *
 * ---------------------------------------------------------------------------
 * What this profile is NOT
 * ---------------------------------------------------------------------------
 *   IT IS A THERMOMETER. Pages 0 and 1 are the whole profile and pages 2-63
 *   are reserved (§6.5). There is NO humidity, NO barometric pressure, NO wind
 *   and NO air quality anywhere in device type 0x19, however much the name
 *   "Environment" suggests otherwise. Everything else environmental reaches
 *   this project through ANT+ Common Data Page 84, which Common Data Pages Rev
 *   3.1 Table 4-1 keys on the channel's TRANSMISSION TYPE rather than its
 *   device type and which therefore arrives on any profile's channel.
 *
 *   THERE IS NO SENSOR PLACEMENT, LOCATION OR KIND FIELD. Not in page 0, not
 *   in page 1. A body-temperature sensor cannot describe itself here, which is
 *   why docs/device-profiles.md §6.3 makes physiological temperature a 0x60
 *   recipe instead of an additive page on this device type - and why ANT+
 *   itself minted Core Temperature 0x7F rather than extending this.
 *
 *   THIS FILE IS DECODE ONLY, and has no encoder. See profile_env.c.
 *
 *   NO CRYPTOGRAPHY and no call to radiant_sec, matching profile_rd.h's and
 *   profile_hr.h's claims about themselves. No radiant header, no bridge
 *   header: the sample-bus mapping lives in src/bridge/radiant_env_adapter.h.
 *
 * ---------------------------------------------------------------------------
 * Traps in the page layout
 * ---------------------------------------------------------------------------
 *   NO PAGE-CHANGE TOGGLE. Byte [0] is the whole page number, all eight bits -
 *   the same warning profile_rd.h carries. Masking with 0x7F out of heart-rate
 *   habit would work by accident forever, since every page this profile
 *   defines or reserves is below 0x40.
 *
 *   THE TWO 24-HOUR FIELDS ARE PACKED IN OPPOSITE DIRECTIONS AND SHARE BYTE
 *   [4]. Low: byte [3] is the LSB, byte [4] bits 7:4 the MSN. High: byte [4]
 *   bits 3:0 are the LSN, byte [5] the MSB. Reusing one shape for both yields
 *   a wrong number that still looks like a temperature.
 *
 *   THEY ARE TWELVE-BIT SIGNED, and must be sign-extended from bit 11
 *   explicitly. A decoder that forgets is correct in every warm room and wrong
 *   below 0 degC.
 *
 *   THREE DIFFERENT SCALES AND TWO DIFFERENT SENTINELS IN ONE PAGE. The
 *   24-hour statistics are 0.1 degC with 0x800 invalid; the current reading is
 *   0.01 degC with 0x8000 invalid. Applying the current reading's scale to the
 *   statistics understates them tenfold.
 *
 *   AN INVALID FIELD IS NOT ZEROED. 0.00 degC is a real reading, so the
 *   decoder reports validity in a separate flag and profile_rd.h's
 *   "invalid arrives as 0" convention deliberately does NOT apply here.
 *
 * ---------------------------------------------------------------------------
 * THE 0.5 Hz CHANNEL PERIOD, and why it is a liveness problem
 * ---------------------------------------------------------------------------
 * §5.1 permits TWO channel periods, not one: 65535 counts (0.5 Hz - what the
 * Garmin tempe uses and the profile's own low-power default) as well as the
 * 8192 counts (4 Hz) that nearly every other ANT+ profile fixes. Page 0's
 * transmission-info field declares which one a given sensor defaults to;
 * profile_env_period_counts() turns that declaration into counts.
 *
 * Two consequences a reader will otherwise meet the hard way:
 *
 *   LIVENESS. radiant_liveness.c's stale_after_us() is period x 3, so the
 *   staleness threshold for this profile is 6.0 s at 0.5 Hz against 750 ms at
 *   4 Hz - an eightfold difference. struct radiant_binding::period defaults to
 *   0 meaning "never set", which radiant_liveness.c treats as "no honest
 *   expiry" and counts in stats.no_period rather than guessing 8192. So a
 *   binding whose period was never set does not go falsely STALE; it simply
 *   never goes STALE at all, silently, and only a counter says so.
 *   apps/dongle_thread/src/self_channels.c does set it, reading the value back
 *   off the channel with antr_channel_period_get() at bind time - verified,
 *   not assumed. Any NEW bind path must do the same.
 *
 *   ACQUISITION. A slave channel's period must match its master's, so a
 *   receiver hard-wired to 8192 will never track a 0.5 Hz sensor at all. §5.1
 *   also recommends a 45 second search timeout for exactly this reason, which
 *   is where PROFILE_ENV_SEARCH_TIMEOUT_S comes from.
 */

#ifndef RADIANT_PROFILE_ENV_H_
#define RADIANT_PROFILE_ENV_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_ENV_DEVICE_TYPE 0x19u

/* RF 57 (2457 MHz), the ANT+ public frequency, spelled as an offset from
 * 2400 MHz exactly as ANT_PLUS_RF_FREQ is in tools/ant_pages.py (§5.1). */
#define PROFILE_ENV_RF_FREQ 57u

/*
 * The two permitted channel periods, in counts of 1/32768 s (§5.1). Unlike a
 * page number, a period has to match or the channel never opens - see the
 * header comment above.
 */
#define PROFILE_ENV_PERIOD_4_HZ   8192u
#define PROFILE_ENV_PERIOD_0P5_HZ 65535u

/* §5.1's recommended receiver search timeout, in seconds. Longer than the
 * usual ANT+ figure specifically so a 0.5 Hz master has time to be found. */
#define PROFILE_ENV_SEARCH_TIMEOUT_S 45u

/* The page numbers. 0x00 and 0x01 are the entire profile; 0x02..0x3F are
 * reserved for future use (§6.5) and must be declined, not guessed at. */
#define PROFILE_ENV_PAGE_CAPABILITIES 0x00u
#define PROFILE_ENV_PAGE_TEMPERATURE  0x01u
#define PROFILE_ENV_PAGE_RESERVED_MIN 0x02u
#define PROFILE_ENV_PAGE_RESERVED_MAX 0x3Fu

/*
 * The invalid-value sentinels, as the RAW field patterns Table 6-4 names them
 * by, before any sign extension. Each is the most negative value its width can
 * hold, and §6.4's stated ranges stop one count short of it (-204.7 not -204.8,
 * -327.67 not -327.68) so that the sentinel is not also a legal reading.
 */
#define PROFILE_ENV_INVALID_12 0x0800u /* the two 24-hour fields */
#define PROFILE_ENV_INVALID_16 0x8000u /* the current temperature */

/*
 * Table 6-3's "Default Transmission Rate", bits 0:1 of the transmission-info
 * byte. 0b10 and 0b11 are Reserved and are passed through raw rather than
 * folded into either of these.
 */
#define PROFILE_ENV_RATE_0P5_HZ 0u
#define PROFILE_ENV_RATE_4_HZ   1u

/*
 * Table 6-3's UTC Time (bits 2:3) and Local Time (bits 4:5) enumerations. Both
 * fields use the same three values, which is why there is one set of names.
 * "Supported, not set" is not the same as "not supported": the first means a
 * display may set the clock over ANT-FS (§6.3.1.1), the second means it may
 * not.
 */
#define PROFILE_ENV_TIME_UNSUPPORTED     0u
#define PROFILE_ENV_TIME_SUPPORTED_UNSET 1u
#define PROFILE_ENV_TIME_SUPPORTED_SET   2u
#define PROFILE_ENV_TIME_RESERVED        3u

/* Bit positions in the supported-pages bitmap (§6.3.2): bit position IS the
 * page number. Bit 0 shall always be set; a temperature sensor also sets bit
 * 1; bits 2..31 shall be 0 in this revision. */
#define PROFILE_ENV_SUPPORTED_PAGE_BIT(page) ((uint32_t)1u << (page))

/*
 * Page 0x00. Every two-bit field is the raw wire value, so a reserved encoding
 * stays visible to the caller instead of being flattened into a neighbouring
 * meaning.
 */
struct profile_env_capabilities {
	uint8_t  default_rate; /* PROFILE_ENV_RATE_*, or a reserved 2/3 */
	uint8_t  utc_time;     /* PROFILE_ENV_TIME_* */
	uint8_t  local_time;   /* PROFILE_ENV_TIME_* */
	uint32_t supported_pages; /* bit position == page number */
};

/*
 * Page 0x01. Every value is in its own wire scale rather than SI or float -
 * two scales in one page is two chances to apply the wrong multiplier, so the
 * conversion is the caller's to do once and visibly (the same rule
 * profile_rd.h states for its four fractional widths).
 *
 * The *_valid flags are the ONLY way to test for a missing reading. The
 * corresponding value member is left as the sign-extended sentinel rather than
 * zeroed, because 0.00 degC is a legitimate temperature.
 */
struct profile_env_temperature {
	uint8_t event_count;    /* u8 accumulator; increments per measurement */

	int16_t low_24h_deci;   /* 0.1 degC, lowest over the last 24 hours */
	int16_t high_24h_deci;  /* 0.1 degC, highest over the last 24 hours */
	int16_t current_centi;  /* 0.01 degC, the live reading */

	bool low_24h_valid;
	bool high_24h_valid;
	bool current_valid;
};

/* Fills `c` from a page 0x00 body. -EINVAL on a NULL argument or if byte [0]
 * is not PROFILE_ENV_PAGE_CAPABILITIES. */
int profile_env_decode_capabilities(const uint8_t *body,
				    struct profile_env_capabilities *c);

/* Fills `t` from a page 0x01 body. -EINVAL on a NULL argument or if byte [0]
 * is not PROFILE_ENV_PAGE_TEMPERATURE. Reserved pages 0x02..0x3F therefore
 * decline here rather than decoding into nonsense. */
int profile_env_decode_temperature(const uint8_t *body,
				   struct profile_env_temperature *t);

/*
 * The channel period in 1/32768 s counts that a page 0 `default_rate` names,
 * or 0 for a reserved encoding. 0 is the same value struct
 * radiant_binding::period uses for "never set", and radiant_liveness.c already
 * abstains on it - so a reserved rate propagates as "no honest expiry" rather
 * than as a fabricated one.
 */
uint16_t profile_env_period_counts(uint8_t default_rate);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_ENV_H_ */
