/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Reserves the linker section STRUCT_SECTION_ITERABLE(radiant_sink, ...) needs.
 *
 * RADIANT_SINK_DEFINE() (radiant_bridge.h) emits each sink into a section
 * named after the struct type, but emitting into a section and having that
 * section actually placed in the image - with the _radiant_sink_list_start/
 * _end symbols STRUCT_SECTION_FOREACH() walks between - are two different
 * things. Every other STRUCT_SECTION_ITERABLE user in the tree (shell
 * commands, bt_conn_cb, settings_handler_static, ...) has its own line like
 * this one in a linker fragment under zephyr/include/zephyr/linker/common-rom/;
 * radiant_sink has no such in-tree home, so this file is that line for it,
 * pulled in via zephyr_linker_sources(SECTIONS ...) below the CMakeLists.txt
 * that consumes src/bridge/.
 */

#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(radiant_sink, Z_LINK_ITERABLE_SUBALIGN)
