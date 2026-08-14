/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_bridge.h - the sample bus and the sink registry.
 *
 * struct radiant_sample mirrors docs/radiant-bridge.md sections 3-4; the
 * field vocabulary below mirrors docs/radiant-telemetry.md section 7's
 * table and tools/ant_pages.py - three copies that must never diverge.
 *
 * This is P5 of the multiprotocol plan: sample bus + sink registry only.
 * `source` is an opaque caller-supplied index (not a device number, not
 * resolved to a label/UUID/policy here) - the binding table (S5) and rule
 * evaluator (S6) are P6, not this file. Sits strictly above the profile
 * decoder: nothing here or in radiant_bridge_hr.c touches an ANT frame or
 * a radiant_core header.
 */

#ifndef RADIANT_BRIDGE_H_
#define RADIANT_BRIDGE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Section 7 vocabulary. Instantaneous (0x01-0x2F) and accumulating
 * (0x30-0x3F) classes are decimal-scaled: value_SI = raw * 10^exp. 0x26
 * heart rate is bpm, the one stated non-SI exception. 0x27 time interval is
 * proposed, not allocated (section 3.2) - absent here until allocated in
 * all three vocabulary copies at once.
 * ---------------------------------------------------------------------------
 */
#define RADIANT_FIELD_BOOLEAN_STATE       0x01u /* canonical unit: none (0/1) */
#define RADIANT_FIELD_OCCUPANCY           0x02u /* none (0/1) */
#define RADIANT_FIELD_CONTACT             0x03u /* none (0/1) */
#define RADIANT_FIELD_ONOFF               0x04u /* none (0/1) */
#define RADIANT_FIELD_LOCK_STATE          0x05u /* enum 0..3 */

#define RADIANT_FIELD_TEMPERATURE         0x10u /* K */
#define RADIANT_FIELD_HUMIDITY            0x11u /* % */
#define RADIANT_FIELD_PRESSURE            0x12u /* Pa */
#define RADIANT_FIELD_ILLUMINANCE         0x13u /* lx */
#define RADIANT_FIELD_LENGTH              0x14u /* m */
#define RADIANT_FIELD_MASS                0x15u /* kg */
#define RADIANT_FIELD_SPEED               0x16u /* m/s */
#define RADIANT_FIELD_ACCELERATION        0x17u /* m/s^2 */
#define RADIANT_FIELD_ANGLE               0x18u /* rad */
#define RADIANT_FIELD_ANGULAR_RATE        0x19u /* rad/s */
#define RADIANT_FIELD_FORCE               0x1Au /* N */
#define RADIANT_FIELD_TORQUE              0x1Bu /* N.m */
#define RADIANT_FIELD_ACTIVE_POWER        0x1Cu /* W */
#define RADIANT_FIELD_VOLTAGE             0x1Du /* V */
#define RADIANT_FIELD_CURRENT             0x1Eu /* A */
#define RADIANT_FIELD_FREQUENCY           0x1Fu /* Hz */
#define RADIANT_FIELD_VOLUMETRIC_FLOW     0x20u /* m^3/s */
#define RADIANT_FIELD_GAS_CONCENTRATION   0x21u /* ppm */
#define RADIANT_FIELD_MASS_CONCENTRATION  0x22u /* ug/m^3 */
#define RADIANT_FIELD_SOUND_PRESSURE      0x23u /* dB SPL */
#define RADIANT_FIELD_PERCENTAGE          0x24u /* % */
#define RADIANT_FIELD_BATTERY_SOC         0x25u /* % */
#define RADIANT_FIELD_HEART_RATE          0x26u /* bpm - non-SI, the stated exception */
#define RADIANT_FIELD_SUBSTANCE_CONC      0x28u /* mol/m^3 */

#define RADIANT_FIELD_ENERGY              0x30u /* J - accumulating */
#define RADIANT_FIELD_CHARGE              0x31u /* C - accumulating */
#define RADIANT_FIELD_VOLUME              0x32u /* m^3 - accumulating */
#define RADIANT_FIELD_MASS_CUMULATIVE     0x33u /* kg - accumulating */
#define RADIANT_FIELD_DISTANCE            0x34u /* m - accumulating */
#define RADIANT_FIELD_REVOLUTIONS         0x35u /* count - accumulating */
#define RADIANT_FIELD_EVENT_COUNT         0x36u /* count - accumulating */
#define RADIANT_FIELD_DURATION            0x37u /* s - accumulating */

#define RADIANT_FIELD_ENUM_GENERIC        0x40u
#define RADIANT_FIELD_HVAC_MODE           0x41u
#define RADIANT_FIELD_FAN_MODE            0x42u

/* struct radiant_sample::flags */
#define RADIANT_SAMPLE_ACCUMULATING (1u << 0) /* raw is a monotone counter, not an instant */
#define RADIANT_SAMPLE_DERIVED      (1u << 1) /* produced by a rule (section 6), not decoded */
#define RADIANT_SAMPLE_STALE        (1u << 2) /* liveness heartbeat missed 3x its interval */

/*
 * The record (docs/radiant-bridge.md section 3). t_us is the decode-time
 * t_sync, not dequeue time, so a sink delayed behind a full bus still
 * reports when the sample was actually heard.
 */
struct radiant_sample {
	uint32_t source;     /* binding index - NOT a device number; see S5 (P6) */
	uint8_t  field_id;   /* stable within source */
	uint8_t  field_type; /* one of the RADIANT_FIELD_* constants above */
	uint8_t  flags;      /* RADIANT_SAMPLE_* bits */
	int8_t   exp;        /* value_SI = raw * 10^exp, in the type's unit */
	int64_t  raw;
	uint64_t t_us;
};

/* Reserved source for the bus's own diagnostics (drop counter, section
 * 3.3) - never a real binding's index. */
#define RADIANT_BRIDGE_DIAG_SOURCE  UINT32_MAX
#define RADIANT_BRIDGE_DIAG_FIELD_DROPPED 0u

/* ---------------------------------------------------------------------------
 * The bus. Ring buffer, drops oldest and counts drops (section 3.3).
 * post() is ISR-safe (bounded: lock, copy, index update, no blocking).
 * drain() dispatches to every sink and must run in thread context, since
 * publish() may do real work (socket write, flash write).
 * ---------------------------------------------------------------------------
 */

/* Copies *s into the ring. Never blocks, never fails silently: if the ring is
 * full the oldest record is discarded and radiant_bridge_stats().dropped
 * counts it. Safe to call from a radio ISR. */
void radiant_bridge_post(const struct radiant_sample *s);

/* Drains every queued sample to every sink whose want() returns true, and - if
 * the drop counter moved since the last drain - posts one more sample for it
 * on RADIANT_BRIDGE_DIAG_SOURCE before returning. Thread context only.
 * Returns the number of samples drained (the diagnostics sample, if emitted,
 * is not counted in it, since it did not come from the ring). */
uint32_t radiant_bridge_drain(void);

struct radiant_bridge_stats {
	uint32_t posted;  /* radiant_bridge_post() calls */
	uint32_t dropped; /* of those, how many evicted the oldest record */
	uint32_t drained; /* radiant_bridge_drain() deliveries, summed over every sink */
};

const struct radiant_bridge_stats *radiant_bridge_stats_get(void);

/* Test/reset hook. Not for production use. */
void radiant_bridge_reset(void);

/* ---------------------------------------------------------------------------
 * Sinks. A sink is a registration, not a case in a switch (section 4) -
 * same callback-first-refusal shape as profile_sched.c's client seam, but
 * N entries instead of one. STRUCT_SECTION_ITERABLE links every TU's
 * definition into one array with no central list to edit (as DEVICE_DEFINE
 * and SHELL_CMD_ARG_REGISTER do elsewhere in the tree).
 * ---------------------------------------------------------------------------
 */

struct radiant_binding; /* forward-declared; P6 (S5) owns the real definition */

struct radiant_sink {
	const char *name;

	/* First refusal. NULL means "wants everything". */
	bool (*want)(const struct radiant_sample *s);

	/* Thread context, from radiant_bridge_drain(). May block. */
	void (*publish)(const struct radiant_sample *s);

	/* A binding was added, changed or removed. NULL if the sink polls
	 * instead - most will not need to. */
	void (*binding_changed)(uint32_t source, const struct radiant_binding *b);
};

#include <zephyr/sys/iterable_sections.h>

#define RADIANT_SINK_DEFINE(_name, _want, _publish, _binding_changed)        \
	STRUCT_SECTION_ITERABLE(radiant_sink, _name) = {                     \
		.name = #_name,                                               \
		.want = (_want),                                              \
		.publish = (_publish),                                        \
		.binding_changed = (_binding_changed),                        \
	}

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BRIDGE_H_ */
