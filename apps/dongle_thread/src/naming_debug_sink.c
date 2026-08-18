/* SPDX-License-Identifier: Apache-2.0 */
/*
 * naming_debug_sink.c - a bench instrument, off in every shipping image.
 *
 * WHY THIS EXISTS. radiant_naming.c composes the user-visible name for every
 * endpoint, and both real consumers of that name are hard to read on a bench:
 * the Matter NodeLabel needs a commissioned controller to display, and the MQTT
 * discovery payload needs a broker on a Thread network. So "what does this
 * bridge actually call things" was, until this file, only ever inferred from
 * the tables rather than observed from a running image with real sensors on it.
 *
 * It answers exactly one question, once per distinct (source, field_type,
 * field_id) it sees: what name does the formatter produce, and does any other
 * field on the SAME source produce the same one. The second half is the point -
 * a duplicate name is invisible in a unit test that checks names one at a time,
 * and it is the defect this instrument was written to measure.
 *
 * CONFIG_ANT_DONGLE_NAMING_DEBUG defaults n, so this file is not compiled into
 * bridge.conf, matter.conf or any of the five section 7.4 coexistence arms, and
 * it adds nothing to .github/workflows/release.yml's exact-set assert_sink_names
 * diff on those builds. Turn it on with -DCONFIG_ANT_DONGLE_NAMING_DEBUG=y for
 * a bench sitting and read the console.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_naming.h"

LOG_MODULE_REGISTER(naming_debug, LOG_LEVEL_INF);

/*
 * Per source, the names already announced. Sized for the fields a single
 * binding can realistically carry: three from a profile adapter, five derived
 * booleans, and the common-page block. 16 is comfortably above every profile
 * in radiant/src/bridge today and the overflow is reported rather than silent.
 */
#define SEEN_MAX 16u

struct seen_row {
	uint8_t field_type;
	uint8_t field_id;
	bool    derived; /* the other half of mqtt_sink.c's field_key() */
	char    name[RADIANT_NAMING_MAX];
};

static struct seen_row seen[RADIANT_BINDING_MAX][SEEN_MAX];
static uint8_t         seen_n[RADIANT_BINDING_MAX];
static uint8_t         subtype[RADIANT_BINDING_MAX];

/* The FE-C equipment type, exactly as mqtt_sink.c caches it - so this
 * instrument names things the way a real sink does rather than the way the
 * tables would if the subtype never arrived. */
#define FEC_TYPE_FIELD_ID 0x0Du
#define FEC_DEVICE_TYPE   0x11u

static K_MUTEX_DEFINE(lock);

/*
 * A STALE census, added after a bench sitting showed bridge_pump.c reporting
 * "N binding(s) went STALE" once per SECOND, forever, with real sensors sitting
 * happily on air. That line counts transitions and names nothing, so a healthy
 * expiry and a field flapping in and out of expiry every sweep look identical
 * from outside. This says WHICH field, which is the whole question.
 *
 * Bounded rather than continuous: at a few per second an unbounded log would
 * push the very payloads this build exists to read off the console.
 */
#define STALE_LOG_MAX 60u

static uint32_t stale_logged;

static void naming_debug_publish(const struct radiant_sample *s)
{
	const struct radiant_binding *b;
	char name[RADIANT_NAMING_MAX];
	uint32_t src = s->source;
	uint8_t i;
	bool recompose = false;
	bool derived = ((s->flags & RADIANT_SAMPLE_DERIVED) != 0u);

	if (src >= RADIANT_BINDING_MAX) {
		return;
	}

	b = radiant_binding_get(src);
	if (b == NULL) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);

	if ((s->flags & RADIANT_SAMPLE_STALE) != 0u) {
		if (stale_logged < STALE_LOG_MAX) {
			stale_logged++;
			LOG_WRN("STALE #%u: source %u devtype 0x%02x devnum %u "
				"(type 0x%02x id 0x%02x) period %u last %llu ms",
				stale_logged, src, b->devtype, b->devnum,
				s->field_type, s->field_id, b->period,
				(unsigned long long)(s->t_us / 1000u));
		}
		k_mutex_unlock(&lock);
		return;
	}

	if (s->field_type == RADIANT_FIELD_ENUM_GENERIC &&
	    s->field_id == FEC_TYPE_FIELD_ID && b->devtype == FEC_DEVICE_TYPE &&
	    s->raw >= 0 && s->raw <= (int64_t)UINT8_MAX &&
	    subtype[src] != (uint8_t)s->raw) {
		subtype[src] = (uint8_t)s->raw;
		/* The base name just changed for every field on this source.
		 * Re-announce them all, which is also what the real sinks do. */
		seen_n[src] = 0u;
		recompose = true;
		LOG_INF("source %u learned equipment type %u - renaming all fields",
			src, subtype[src]);
	}

	for (i = 0u; i < seen_n[src]; i++) {
		if (seen[src][i].field_type == s->field_type &&
		    seen[src][i].field_id == s->field_id) {
			k_mutex_unlock(&lock);
			return; /* already reported */
		}
	}

	(void)radiant_naming_format(name, sizeof(name), b->devtype, b->devnum,
				    subtype[src], b->label, s->field_type,
				    s->field_id);

	/* THE MEASUREMENT: does any field already on this source carry this
	 * exact name? Two endpoints a user cannot tell apart is the defect. */
	for (i = 0u; i < seen_n[src]; i++) {
		if (strcmp(seen[src][i].name, name) == 0) {
			LOG_ERR("DUPLICATE NAME on source %u: \"%s\" is both "
				"(type 0x%02x id 0x%02x) and (type 0x%02x id 0x%02x)",
				src, name, seen[src][i].field_type,
				seen[src][i].field_id, s->field_type,
				s->field_id);
			break;
		}
	}

	/*
	 * THE SECOND MEASUREMENT, and it is about identity rather than display.
	 *
	 * radiant_rules.c posts its derived booleans onto the SOURCE'S OWN id
	 * space - RADIANT_RULE_FIELD_WORN is 0, and so is radiant_hr_adapter.c's
	 * computed bpm - so a field id is NOT unique per source. That is a
	 * property of the bus and it is not changing; what matters is whether a
	 * sink's identity key survives it.
	 *
	 * So this checks the key mqtt_sink.c actually uses: field_id plus the
	 * DERIVED flag, the same discriminator its field_key() applies. Keyed on
	 * the bare id (as it was) a strap published bpm and "worn" to one topic
	 * under one unique_id, and announced only whichever arrived first.
	 */
	for (i = 0u; i < seen_n[src]; i++) {
		bool same_plane = (seen[src][i].derived == derived);

		if (seen[src][i].field_id == s->field_id && same_plane &&
		    seen[src][i].field_type != s->field_type) {
			LOG_ERR("SINK KEY COLLISION on source %u: key %s%u is both "
				"type 0x%02x (\"%s\") and type 0x%02x (\"%s\")",
				src, derived ? "d" : "", s->field_id,
				seen[src][i].field_type, seen[src][i].name,
				s->field_type, name);
			break;
		}

		/* Worth seeing even when it is harmless, because it is what
		 * makes the discriminator load-bearing. */
		if (seen[src][i].field_id == s->field_id && !same_plane) {
			LOG_INF("id 0x%02x is reused across planes on source %u "
				"(\"%s\" vs \"%s\") - separated by the derived flag",
				s->field_id, src, seen[src][i].name, name);
		}
	}

	if (seen_n[src] < SEEN_MAX) {
		seen[src][seen_n[src]].field_type = s->field_type;
		seen[src][seen_n[src]].field_id = s->field_id;
		seen[src][seen_n[src]].derived = derived;
		(void)strncpy(seen[src][seen_n[src]].name, name,
			      sizeof(seen[src][seen_n[src]].name) - 1u);
		seen[src][seen_n[src]].name[sizeof(seen[src][seen_n[src]].name) - 1u] = '\0';
		seen_n[src]++;
	} else {
		LOG_WRN("source %u has more than %u fields - not tracking the rest",
			src, SEEN_MAX);
	}

	LOG_INF("name: source %u devtype 0x%02x devnum %u subtype %u "
		"(type 0x%02x id 0x%02x) -> \"%s\"%s",
		src, b->devtype, b->devnum, subtype[src], s->field_type,
		s->field_id, name, recompose ? " [recomposed]" : "");

	k_mutex_unlock(&lock);
}

static void naming_debug_binding_changed(uint32_t source,
					 const struct radiant_binding *b)
{
	if (source >= RADIANT_BINDING_MAX) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	seen_n[source] = 0u;
	subtype[source] = 0u;
	k_mutex_unlock(&lock);

	if (b != NULL) {
		LOG_INF("bound source %u: devtype 0x%02x devnum %u label \"%s\"",
			source, b->devtype, b->devnum, b->label);
	} else {
		LOG_INF("unbound source %u", source);
	}
}

RADIANT_SINK_DEFINE(naming_debug_sink, NULL, naming_debug_publish,
		    naming_debug_binding_changed);
