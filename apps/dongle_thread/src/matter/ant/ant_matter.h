/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_matter.h - the C-visible face of the Matter glue (packages E4/E6).
 *
 * Three functions, and every one of them exists because a C translation unit
 * has to reach C++ code:
 *
 *   ant_matter_start()            src/main.c's ant_dongle_post_radio_init()
 *                                 hook starts the CHIP thread.
 *   ant_matter_binding_changed()  ant_matter_sink.c's radiant_sink callback.
 *   ant_matter_note_sample()      the same sink's publish().
 *
 * WHY A C SHIM SINK AT ALL, rather than defining the radiant_sink in C++.
 * RADIANT_SINK_DEFINE() is STRUCT_SECTION_ITERABLE plus a designated
 * initialiser. Designated initialisers are C99 and are only a GNU extension in
 * C++17, and the iterable-section machinery relies on the object landing in a
 * section named after the struct with no name mangling. Both work in practice
 * and neither is guaranteed; a nine-line C file is cheaper than finding out
 * which compiler upgrade stops agreeing.
 *
 * NOTE WHAT IS NOT HERE: the attribute writes. Those arrive through
 * radiant_matter.c's own __weak seam, radiant_matter_attr_write(), which
 * ant_bridge.cpp overrides with C linkage. That seam is radiant's contract and
 * routing it through this header as well would be a second way in.
 */

#ifndef ANT_MATTER_H_
#define ANT_MATTER_H_

#include <stdint.h>

struct radiant_binding;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the CHIP event loop and the Matter application thread.
 *
 * MUST NOT BLOCK. It is called from ant_dongle_post_radio_init(), which runs
 * before usb_ant_class_init(), and this repository has already read a
 * pre-main() stall as a hung board once (thread.conf's NET_CONFIG_NEED_IPV6
 * note, 31.04 s). So this spawns a thread and returns immediately; every
 * blocking call - PrepareServer(), StartServer() and the dispatch loop - is on
 * the far side of that.
 *
 * Returns 0, or a negative errno if the thread could not be created.
 */
int ant_matter_start(void);

/*
 * A binding was added, changed or removed. `b` is NULL for an unbind.
 *
 * An unbind is the ONLY thing that removes a Matter endpoint. A sensor going
 * quiet sets Reachable = false and keeps its endpoint, because Matter forbids
 * reusing an endpoint id for a different device and a controller that saw the
 * endpoint disappear would remove the entity, the automations bound to it and
 * the history behind it.
 */
void ant_matter_binding_changed(uint32_t source, const struct radiant_binding *b);

/* Every drained sample, for its RADIANT_SAMPLE_* flags only - the value has
 * already gone to radiant_matter.c. This is what drives Reachable. */
void ant_matter_note_sample(uint32_t source, uint8_t flags);

#ifdef __cplusplus
}
#endif

#endif /* ANT_MATTER_H_ */
