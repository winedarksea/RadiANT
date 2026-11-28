/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_ftms_data.c - see treadmill_ftms_data.h. Kernel-free.
 */

#include <errno.h>

#include "treadmill_ftms_data.h"

/* Table 4.5's flags. */
#define TD_FLAG_TOTAL_DISTANCE  (1u << 2)
#define TD_FLAG_INCLINATION     (1u << 3) /* AND ramp angle */
#define TD_FLAG_ELEVATION_GAIN  (1u << 4) /* the pos/neg PAIR */
#define TD_FLAG_HEART_RATE      (1u << 8)
#define TD_FLAG_ELAPSED_TIME    (1u << 10)

/* §4.4.1.5/§4.4.1.6: "cannot compute" for Inclination and Ramp Angle. This
 * machine has no ramp-angle sensor. */
#define FTMS_SINT16_UNKNOWN 0x7FFFu

static void put_le16(uint8_t *out, uint16_t v)
{
	out[0] = (uint8_t)(v & 0xFFu);
	out[1] = (uint8_t)(v >> 8);
}

static void put_u24(uint8_t *out, uint32_t v)
{
	out[0] = (uint8_t)(v & 0xFFu);
	out[1] = (uint8_t)((v >> 8) & 0xFFu);
	out[2] = (uint8_t)((v >> 16) & 0xFFu);
}

void treadmill_ftms_baseline_capture(struct treadmill_ftms_baseline *b,
				     const struct treadmill_state *s)
{
	b->distance_mm = s->distance_mm;
	b->elapsed_ms = s->elapsed_ms;
	b->pos_vertical_mm = s->pos_vertical_mm;
	b->neg_vertical_mm = s->neg_vertical_mm;
}

/* value - base, re-basing to 0 when the accumulator went below its base. */
static uint64_t since64(uint64_t value, uint64_t *base)
{
	if (value < *base) {
		*base = 0u;
	}
	return value - *base;
}

static uint32_t since32(uint32_t value, uint32_t *base)
{
	if (value < *base) {
		*base = 0u;
	}
	return value - *base;
}

int treadmill_ftms_data_encode(const struct treadmill_state *s,
			       struct treadmill_ftms_baseline *b, bool heart_rate,
			       uint8_t *buf, size_t cap)
{
	size_t need = heart_rate ? 18u : 17u;
	uint16_t flags = TD_FLAG_TOTAL_DISTANCE | TD_FLAG_INCLINATION |
			 TD_FLAG_ELEVATION_GAIN | TD_FLAG_ELAPSED_TIME;
	size_t i = 0;

	if (cap < need) {
		return -ENOMEM;
	}
	if (heart_rate) {
		flags |= TD_FLAG_HEART_RATE;
	}

	/* Bit 0 CLEAR: Instantaneous Speed present. */
	put_le16(&buf[i], flags);
	i += 2u;
	put_le16(&buf[i], treadmill_ftms_speed_from_mm_s(s->belt_speed_mm_s));
	i += 2u;
	put_u24(&buf[i], (uint32_t)(since64(s->distance_mm, &b->distance_mm) / 1000u));
	i += 3u;
	put_le16(&buf[i], (uint16_t)treadmill_ftms_incline_from_fec(s->incline_centi_pct));
	i += 2u;
	put_le16(&buf[i], FTMS_SINT16_UNKNOWN);
	i += 2u;
	put_le16(&buf[i], (uint16_t)((since32(s->pos_vertical_mm, &b->pos_vertical_mm) / 100u) &
				     0xFFFFu));
	i += 2u;
	put_le16(&buf[i], (uint16_t)((since32(s->neg_vertical_mm, &b->neg_vertical_mm) / 100u) &
				     0xFFFFu));
	i += 2u;
	if (heart_rate) {
		buf[i++] = s->heart_rate_bpm;
	}
	put_le16(&buf[i], (uint16_t)((since32(s->elapsed_ms, &b->elapsed_ms) / 1000u) & 0xFFFFu));
	i += 2u;

	return (int)i;
}
