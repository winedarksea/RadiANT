# SPDX-License-Identifier: Apache-2.0
#
# ant_core - the clean-room ANT link layer.
#
# This file is what makes -DANT_RADIO=core compile something. The root
# CMakeLists.txt does add_subdirectory(ant_core) when this file exists and
# warns when it does not, so its arrival is the whole difference between a
# core build that fails at "no source file" and one that fails at the link
# with a list of unimplemented entry points - and the second is a useful
# answer while the first hides every other problem behind the first missing
# file.
#
# ── What a core build does and does not link today ──────────────────────────
#
# The stack is being written by a fan-out of agents, one module each, and the
# list below grows one line per module as they land. Until ant_api.c exists
# there is nothing implementing src/ant_radio.h's antr_* contract, so a core
# build still fails at the link with undefined antr_* references. That is
# expected and correct: it is the honest report of how much of the stack is
# present. Any *compile* error, in contrast, is a defect.
#
# ── Why target_sources(app) and not zephyr_library_named() ──────────────────
#
# Because the latter does not work here, and fails in the worst possible way.
# add_subdirectory() from the application's own CMakeLists.txt puts this file
# in Zephyr's *application* mode, where zephyr_library_named() prints
#
#   zephyr_library() or zephyr_library_named() called in Zephyr CMake
#   application mode. `ant_core` will not be treated as a Zephyr library.
#
# and then compiles nothing at all. The build still reaches the link and still
# fails on the same undefined antr_* references, so the symptom is identical to
# a correct build - the only way to notice is that no ant_*.c.obj exists. A
# library form that silently drops every source it was given is exactly the
# kind of green light that means nothing, so it is recorded here rather than
# left to be rediscovered.
#
# The library shape is still the right long-term answer, and the supported way
# to get it is to make ant_core a Zephyr module (a zephyr/module.yml and an
# entry in ZEPHYR_EXTRA_MODULES) rather than a subdirectory. That is a change
# to the root CMakeLists.txt, which this file does not own; it is worth doing
# when ant_core grows Kconfig-selected backends and needs its own Kconfig
# sourced, and not before.
#
# Nothing is lost in the meantime. ant_core has to stay buildable outside this
# application - ant_core/tests builds these same sources with no dongle, no USB
# and no serial bridge in sight, which is what lets the modules be verified in
# CI on Linux with no board - and that test application never reads this file.
# It lists the module sources itself, for the same reason the list here is
# explicit: a suite that silently stops linking the module it tests still
# passes.

target_sources(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_frame.c
)

# ant_core/include goes on the search path for everything in the image, not
# just for the sources above: the modules will include each other's headers,
# and later src/ code will include ant_api.h.
zephyr_include_directories(include)
