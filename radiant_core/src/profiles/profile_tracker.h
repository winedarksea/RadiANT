/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_tracker.h - ANT+ Tracker (Asset Tracker), device type 0x29.
 *
 * No cryptography here and no call to radiant_sec, matching profile_hr.h's
 * and profile_rd.h's claims about themselves. No compat client is attached:
 * this device type's page space (1, 2, 3, 16, 17, 32) does not intersect the
 * heart-rate compat allocation (0x70-0x72 on 0x78) at all.
 *
 * ---------------------------------------------------------------------------
 * Location is split across TWO pages and TWO 32-bit fields cross that split
 * ---------------------------------------------------------------------------
 * Page 1 carries the low 16 bits of latitude; page 2 carries the high 16
 * bits of latitude AND the whole 32-bit longitude. A receiver that heard
 * only page 1 has a quarter of the position report, not half a coordinate.
 * The document's transmission pattern (page 1 always immediately followed by
 * page 2) makes this safe in practice, and this module enforces the pairing
 * at decode time rather than exposing two half-decoders a caller could call
 * out of order.
 *
 * ---------------------------------------------------------------------------
 * Units: semicircles and bradians, not degrees
 * ---------------------------------------------------------------------------
 * Latitude/longitude: semicircles = degrees * (2^31 / 180). Bearing:
 * bradians = degrees * (256 / 360). Both exact-in-reverse integer mappings
 * chosen so neither device needs pi in firmware; this module stores and
 * returns the raw wire integers and leaves degree conversion to a caller
 * that wants it - the same division of labour profile_rd.h draws between
 * wire scale and physical units.
 *
 * ---------------------------------------------------------------------------
 * Asset index is not a device number
 * ---------------------------------------------------------------------------
 * One ANT+ tracker (one device number, one channel) reports MANY assets
 * (dogs, in the profile's worked example), each identified by a 5-bit index
 * carried on pages 1, 2, 16 and 17. Indices assign sequentially from 0 and
 * are NOT reused or shifted when an asset is removed - a removed index's
 * pages simply stop being sent, leaving a hole rather than a renumbering.
 * struct profile_tracker tracks N independent asset slots, not one.
 */

#ifndef RADIANT_PROFILE_TRACKER_H_
#define RADIANT_PROFILE_TRACKER_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_common.h"
#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_TRACKER_DEVICE_TYPE 0x29u

/* 2048 counts of 1/32768 s = 16 Hz, the profile's one permitted rate. */
#define PROFILE_TRACKER_PERIOD 2048u

#define PROFILE_TRACKER_RF_FREQ 57u

#define PROFILE_TRACKER_PAGE_LOCATION_1 0x01u
#define PROFILE_TRACKER_PAGE_LOCATION_2 0x02u
#define PROFILE_TRACKER_PAGE_NO_ASSETS  0x03u
#define PROFILE_TRACKER_PAGE_IDENT_1    0x10u
#define PROFILE_TRACKER_PAGE_IDENT_2    0x11u
#define PROFILE_TRACKER_PAGE_DISCONNECT 0x20u

/* Common page 70's command types for this profile (section 7.10.1). */
#define PROFILE_TRACKER_REQUEST_PAGE     0x01u
#define PROFILE_TRACKER_REQUEST_PAGE_SET 0x04u

/* The asset index is 5 bits; the reserved top 3 bits are set to 0x7 (0b111)
 * on every page that carries it - this profile's own reserved-bit
 * convention. */
#define PROFILE_TRACKER_ASSET_INDEX_MASK 0x1Fu
#define PROFILE_TRACKER_MAX_ASSETS       32u

/* Status byte bits, page 1 byte [4]. */
#define PROFILE_TRACKER_STATUS_LOW_BATTERY     (1u << 3)
#define PROFILE_TRACKER_STATUS_GPS_LOST        (1u << 4)
#define PROFILE_TRACKER_STATUS_COMM_LOST       (1u << 5)
#define PROFILE_TRACKER_STATUS_REMOVE          (1u << 6)
#define PROFILE_TRACKER_SITUATION_MASK         0x07u

/* Asset situation is type-dependent (table 7-5); the dog enum is transcribed
 * as the profile's own worked example. 0xFF is "undefined" for every other
 * asset type. */
#define PROFILE_TRACKER_SITUATION_DOG_SITTING 0u
#define PROFILE_TRACKER_SITUATION_DOG_MOVING  1u
#define PROFILE_TRACKER_SITUATION_DOG_POINTED 2u
#define PROFILE_TRACKER_SITUATION_DOG_TREED   3u
#define PROFILE_TRACKER_SITUATION_DOG_UNKNOWN 4u
#define PROFILE_TRACKER_SITUATION_UNDEFINED   0xFFu

#define PROFILE_TRACKER_ASSET_TYPE_TRACKER 0u
#define PROFILE_TRACKER_ASSET_TYPE_DOG     1u

/* Semicircles: value = degrees * 2^31 / 180. Bradians: value = degrees *
 * 256 / 360. Both stored as the raw wire integer; conversion is the caller's,
 * same division of labour as profile_rd.h's wire-scale metrics. */
#define PROFILE_TRACKER_SEMICIRCLES_PER_180DEG 2147483648.0 /* 2^31 */
#define PROFILE_TRACKER_BRADIANS_PER_360DEG    256.0

struct profile_tracker_location {
	bool     connected; /* false = this asset slot is empty */
	uint16_t distance_m;   /* 0-65535 m */
	uint8_t  bearing_brad; /* bradians, 0-255 = 0-360 deg */
	uint8_t  status;       /* PROFILE_TRACKER_STATUS_* bits, plus situation
				* in the low 3 bits */
	int32_t  latitude_semi;  /* signed semicircles, two's complement */
	int32_t  longitude_semi; /* signed semicircles, two's complement */
};

struct profile_tracker_ident {
	uint8_t colour; /* 3-3-2 RGB */
	uint8_t asset_type; /* PROFILE_TRACKER_ASSET_TYPE_* */
	/* 10 Latin-1/UTF-8 characters plus a null when shorter; callers own
	 * truncation of longer UTF-8 names, which this module does not
	 * attempt at a byte boundary. */
	uint8_t name[10];
};

/* Everything about the node that does not change per asset. */
struct profile_tracker_cfg {
	struct profile_common_id id;
	uint16_t common_82_every;
	struct profile_common_battery battery;
};

struct profile_tracker {
	struct profile_tracker_cfg cfg;
	struct profile_sched       sched;

	struct profile_tracker_location location[PROFILE_TRACKER_MAX_ASSETS];
	struct profile_tracker_ident    ident[PROFILE_TRACKER_MAX_ASSETS];

	/* Which asset slot the rotation is currently on. Location pages
	 * (1 then 2) cycle every connected asset each channel period; identity
	 * pages (16 then 17) go out only on request or on change, via
	 * ident_pending. */
	uint8_t next_asset;

	/* Bitmask of assets whose identity pages are queued to go out. Set by
	 * profile_tracker_connect() and profile_tracker_request_ident();
	 * cleared as each asset's pair is sent. */
	uint32_t ident_pending;

	/* Disconnect-command repeat counter: the document requires 5
	 * transmissions before the node goes off. 0 means "not disconnecting". */
	uint8_t disconnect_left;

	/* 0 = about to send this asset's first page of a pair (location 1, or
	 * identity 1 if ident_pending is set); 1 = the second. */
	uint8_t page_phase;

	uint8_t rotation[1]; /* profile_sched needs a non-empty rotation; this
			      * one entry is a placeholder this profile ignores -
			      * see profile_tracker.c's data_page callback. */
};

/* ---------------------------------------------------------------------------
 * The page encoders. Each writes eight bytes and returns 0, or -EINVAL.
 * ---------------------------------------------------------------------------
 */

/* Page 0x01: asset index, distance, bearing, status, latitude low 16 bits. */
int profile_tracker_encode_location_1(uint8_t asset_index,
				      const struct profile_tracker_location *loc,
				      uint8_t *out);

/* Page 0x02: asset index, latitude high 16 bits, longitude (all 32 bits). */
int profile_tracker_encode_location_2(uint8_t asset_index,
				      const struct profile_tracker_location *loc,
				      uint8_t *out);

/* Page 0x03: no assets connected. Takes no arguments beyond the buffer -
 * every other byte is the profile's own 0xFF fill. */
int profile_tracker_encode_no_assets(uint8_t *out);

int profile_tracker_encode_ident_1(uint8_t asset_index,
				   const struct profile_tracker_ident *id,
				   uint8_t *out);
int profile_tracker_encode_ident_2(uint8_t asset_index,
				   const struct profile_tracker_ident *id,
				   uint8_t *out);

int profile_tracker_encode_disconnect(uint8_t *out);

/* ---------------------------------------------------------------------------
 * The page decoders
 * ---------------------------------------------------------------------------
 */

/* Fills the fields page 0x01 carries: asset_index, distance, bearing, status,
 * and latitude's low 16 bits. A caller with only page 1 has a half-decoded
 * location and must not treat latitude_semi as final until page 0x02
 * arrives. */
int profile_tracker_decode_location_1(const uint8_t *body, uint8_t *asset_index,
				      struct profile_tracker_location *loc);

/* Completes latitude (ORs in the high 16 bits) and fills longitude. Call
 * after profile_tracker_decode_location_1() on the paired page 1; calling it
 * first leaves latitude_semi holding only the high half. */
int profile_tracker_decode_location_2(const uint8_t *body, uint8_t *asset_index,
				      struct profile_tracker_location *loc);

int profile_tracker_decode_ident_1(const uint8_t *body, uint8_t *asset_index,
				   struct profile_tracker_ident *id);
int profile_tracker_decode_ident_2(const uint8_t *body, uint8_t *asset_index,
				   struct profile_tracker_ident *id);

/* ---------------------------------------------------------------------------
 * Unit conversions. Exact in neither direction alone - a semicircle is
 * pi/2^31 degrees, an irrational fraction - but round-tripping degrees ->
 * semicircles -> degrees is exact to the wire's own resolution.
 * ---------------------------------------------------------------------------
 */
int32_t profile_tracker_degrees_to_semicircles(double degrees);
double  profile_tracker_semicircles_to_degrees(int32_t semicircles);
uint8_t profile_tracker_degrees_to_bradians(double degrees);
double  profile_tracker_bradians_to_degrees(uint8_t bradians);

/* ---------------------------------------------------------------------------
 * The master
 * ---------------------------------------------------------------------------
 */

int profile_tracker_init(struct profile_tracker *t,
			 const struct profile_tracker_cfg *cfg);

/* Add or update an asset at `asset_index` (0..31) and queue its identity
 * pages to go out at least once. -EINVAL for an out-of-range index. */
int profile_tracker_connect(struct profile_tracker *t, uint8_t asset_index,
			    const struct profile_tracker_location *loc,
			    const struct profile_tracker_ident *id);

/* Update an already-connected asset's location without re-queuing identity
 * pages. -ENOENT if the slot is not connected. */
int profile_tracker_update_location(struct profile_tracker *t,
				    uint8_t asset_index,
				    const struct profile_tracker_location *loc);

/* Mark an asset removed or lost. Does not itself queue the required "send
 * page 1 four times with the new status" burst - a node driving real
 * hardware does that at the transport layer. This stops the asset from
 * participating in the ordinary rotation once the burst is done. */
void profile_tracker_disconnect_asset(struct profile_tracker *t,
				      uint8_t asset_index);

/* Re-queue identity pages for `asset_index`, per a display's common-page-70
 * request or the document's on-change rule. */
int profile_tracker_request_ident(struct profile_tracker *t,
				  uint8_t asset_index);

/* Begin the 5-repeat disconnect-command burst (section 7.3.1, figure 7-6).
 * profile_tracker_next() serves PROFILE_TRACKER_PAGE_DISCONNECT five times
 * and returns to the ordinary rotation - power-off itself is the real
 * node's to do, not this module's. */
void profile_tracker_begin_disconnect(struct profile_tracker *t);

/* Fill `body` with the next message and say what it was. Alternates location
 * pages 1/2 across every connected asset each channel period, interleaves
 * queued identity pairs, and serves common pages 80/81/82 on
 * profile_sched.c's own cadence, satisfying this profile's coarser "once
 * every 1920 messages". */
enum profile_slot_kind profile_tracker_next(struct profile_tracker *t,
					    uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_TRACKER_H_ */
