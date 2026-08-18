/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_naming.h - the user-visible name of one bridged endpoint.
 *
 * WHAT THIS FIXES. Every endpoint a sink exposes used to be named with the
 * thing the sink had to hand: the Matter bridge fell back to the 18-character
 * hex UniqueID ("3f2a91c40e77b21205") because struct radiant_binding::label is
 * empty on every production bind, and the MQTT sink emitted "RadiANT 4" from
 * the label and the raw field_id. Both are unreadable, and both are WORSE than
 * they look, because one sensor is several endpoints: a heart-rate strap
 * announces four derived occupancy booleans (worn / active / at rest / zone 2)
 * and FE-C adds a fifth (in use). All five are Matter device type 0x0107
 * Occupancy Sensor, so the NAME is the only thing telling them apart.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS A PURE FUNCTION OVER SCALARS, and not a lookup on a `source`
 * ---------------------------------------------------------------------------
 * The obvious signature takes a source index and reads the binding table and
 * whatever cache holds the FE-C subtype. That would give this file cross-thread
 * state and a lock discipline of its own - it is called from the CHIP thread in
 * ant_bridge.cpp and from the bridge pump thread in mqtt_sink.c - and it would
 * make it untestable without standing up a binding table.
 *
 * Taking scalars instead makes every caller snapshot what it needs under
 * whatever lock it already holds, and leaves this file with no state at all:
 * three static const tables and a formatter over them.
 *
 * ---------------------------------------------------------------------------
 * WHY THE SUBTYPE IS A PARAMETER AND NOT A BINDING FIELD
 * ---------------------------------------------------------------------------
 * The FE-C equipment type (treadmill / rower / indoor bike) is LEARNED FROM
 * TRAFFIC - it is page 16 byte 1, not something read off the channel at bind
 * time. radiant_binding.h states that doctrine for `set_period` in as many
 * words, so the subtype does not become a struct radiant_binding member; it
 * travels on the sample bus like every other decoded fact
 * (RADIANT_POWER_FIELD_FEC_TYPE) and each sink caches the one byte itself.
 *
 * A consequence, and it is the reason ant_bridge.cpp carries a `labelDirty`
 * flag: an endpoint can be created BEFORE the first page 16 arrives, so the
 * name must be allowed to improve afterwards rather than being stuck at its
 * first guess.
 *
 * ---------------------------------------------------------------------------
 * `label` IS AN OVERRIDE, NOT STORAGE FOR THIS
 * ---------------------------------------------------------------------------
 * struct radiant_binding::label keeps its documented meaning - "what a human
 * calls it". When it is non-empty it REPLACES the composed base name and the
 * device number; the field suffix is still appended, because that is the half
 * that disambiguates the five occupancy booleans from each other. A derived
 * name is never written back into it.
 *
 * ---------------------------------------------------------------------------
 * TRADEMARK (docs/decisions/0003-naming-trademark-and-usb-identity.md)
 * ---------------------------------------------------------------------------
 * No user-visible text may claim Garmin/ANT+ origin. The base names below are
 * therefore generic equipment nouns - "Heart Rate", "Treadmill" - with no
 * "ANT+" prefix and no vendor word anywhere.
 */

#ifndef RADIANT_NAMING_H_
#define RADIANT_NAMING_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The buffer every caller must be able to afford, INCLUDING the NUL.
 *
 * 32 is not a round number picked here: it is Matter's kNodeLabelSize, and
 * MatterBridgedDevice::SetNodeLabel() memcpy()s with no length check of its own
 * (it is vendored code the tree promises to keep byte-identical to upstream, so
 * it cannot be fixed there). A 32-byte buffer at every call site is what keeps
 * that safe by construction. radiant_naming.c carries a build-time assertion
 * that its own worst-case composition fits.
 */
#define RADIANT_NAMING_MAX 32u

/*
 * Composes "<base> <devnum> <suffix>", or "<label> <suffix>" when `label` is a
 * non-empty binding override, into `out`. The trailing space and suffix are
 * omitted when the field has no suffix.
 *
 *   devtype     ANT device type (0x78 heart rate, 0x11 fitness equipment, ...).
 *   devnum      ANT device number, printed as-is; it is what a user sees on the
 *               sensor and the only thing distinguishing two straps.
 *   subtype     PROFILE_FEC_TYPE_* for devtype 0x11, 0 ("not yet learned") for
 *               everything else. Ignored for every other device type.
 *   label       struct radiant_binding::label, or NULL/"" for none.
 *   field_type  RADIANT_FIELD_* - the sample's vocabulary type.
 *   field_id    the producer's own id within the source.
 *
 * Returns the number of characters written, excluding the NUL, or a negative
 * errno for a NULL/zero-length buffer. The result is always NUL-terminated.
 */
int radiant_naming_format(char *out, size_t n,
			  uint8_t devtype, uint16_t devnum, uint8_t subtype,
			  const char *label,
			  uint8_t field_type, uint8_t field_id);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_NAMING_H_ */
