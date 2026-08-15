/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_matter_sink.c - the second radiant_sink in a Matter image, and the only
 * C file in src/matter/.
 *
 * WHY IT EXISTS AT ALL, when radiant_matter.c already registers a sink. That
 * sink answers `want` with "has this field type a row in the section 8.1
 * table", which is exactly right for the VALUES and blind to the two things
 * the Matter plane also needs:
 *
 *   - BINDINGS. radiant_matter.c's sink passes NULL for binding_changed and
 *     always has: the type map has no use for a device number or a uuid. The
 *     glue does - it needs a stable key to persist an endpoint id against
 *     (trap 8) and a label to put in NodeLabel - and a sink is the only way
 *     radiant_bridge_binding_changed() reaches anything.
 *
 *   - STALENESS. RADIANT_SAMPLE_STALE is a per-sample flag on ANY field,
 *     including fields the Matter plane declines (heart rate is the obvious
 *     one, and on a heart-rate strap it is the field that expires first). A
 *     `want` filtered to mapped types would miss it. So this sink's want()
 *     accepts everything and its publish() reads nothing but the flags.
 *
 * WHY IT IS C. See ant_matter.h's header comment: RADIANT_SINK_DEFINE() is
 * STRUCT_SECTION_ITERABLE plus a designated initialiser, and neither is
 * something to rely on from C++17.
 *
 * "bridge: N sinks" AT BOOT (src/bridge_pump.c) IS THEREFORE 4 IN A MATTER
 * ARM, not 2 and not 3. The rules evaluator, the liveness table,
 * radiant_matter.c and this file are all sinks; the count in that log line is
 * whatever linked. docs/matter-e3-vendoring.md predicted 2 from the symbol
 * table before this file existed, and the Matter CI row asserted the
 * non-Matter arm's 3 - both are superseded by this file, and the row's
 * assert_sink_names has been corrected to the exact set of four. That check is
 * an exact-set diff rather than a subset test, so adding a sink here without
 * adding it there is a CI failure by construction, which is the intent.
 */

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "radiant_binding.h"
#include "radiant_bridge.h"

#include "ant_matter.h"

/* Everything. See the header comment: the flags on a declined field are still
 * this sink's business. Returning true unconditionally is cheaper than any
 * filter that would have to be kept in step with radiant_matter.c's table. */
static bool matter_glue_want(const struct radiant_sample *s)
{
	ARG_UNUSED(s);
	return true;
}

static void matter_glue_publish(const struct radiant_sample *s)
{
	if (s->source >= RADIANT_BINDING_MAX) {
		/* RADIANT_BRIDGE_DIAG_SOURCE, the bus's own drop counter. It
		 * is a real, reserved, out-of-band source and it has no
		 * binding, no endpoint and no reachability. */
		return;
	}
	ant_matter_note_sample(s->source, s->flags);
}

static void matter_glue_binding_changed(uint32_t source, const struct radiant_binding *b)
{
	if (source >= RADIANT_BINDING_MAX) {
		return;
	}
	ant_matter_binding_changed(source, b);
}

RADIANT_SINK_DEFINE(radiant_matter_glue_sink, matter_glue_want, matter_glue_publish,
		    matter_glue_binding_changed);
