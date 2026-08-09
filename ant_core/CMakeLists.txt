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
# list below grows one line per module as they land. ant_api.c is the one that
# implements src/ant_radio.h's antr_* contract, so with it present a core build
# links; without it the build reached the link and failed on 46 undefined
# antr_* references, which was the honest report of how much of the stack was
# present. Any *compile* error, in contrast, is a defect.
#
# Note what the archive semantics hide, because it cost time to find: with
# target_sources(app ...) every module lands in one static library and the
# linker pulls only the members that resolve something. Before ant_api.c
# existed nothing referenced ant_sched.c, ant_channel.c, ant_search.c,
# ant_event.c, ant_ack.c or ant_burst.c, so none of their objects were pulled
# in and none of THEIR undefined references appeared either. The list of
# missing symbols therefore grew when the integration layer landed, which looks
# like a regression and is the opposite of one.
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
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_sched.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_channel.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_search.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_event.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_ack.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_burst.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_api.c
)

# ── The radio backend ───────────────────────────────────────────────────────
#
# ant_core is a link layer: it decides what goes on the air and when, and a
# BACKEND decides how. ant_core/include/ant_radio_hal.h is the whole of that
# boundary, and exactly one implementation of it exists in the tree today -
# ant_core/tests/fake_radio.c, a simulator with a virtual clock that belongs in
# a test image and nowhere near a dongle. So a firmware link needs a backend
# that none of the module agents was asked to write, and its eight symbols
# (ant_radio_caps_get, _init, _enable, _disable, _now, _tx, _rx, _abort) are
# undefined without one.
#
# `null` is the honest stand-in: the lifecycle and the clock are real and every
# arm call is refused with ANT_RADIO_ENOTSUP. It is the HAL-level counterpart of
# src/ant_radio_stub.c - the image enumerates, answers the host, runs the
# scheduler, the search policy, the channel state machines and the event queue,
# and finds no sensors.
#
# A REAL BACKEND REPLACES THIS FILE RATHER THAN JOINING IT. Two definitions of
# ant_radio_now() is a duplicate-symbol link error, which is the right way for
# that mistake to be discovered; adding an `nrf52` branch here and leaving
# `null` in the list would be the wrong way. The variable is a cache entry so a
# bring-up build can pick one without editing this file.
set(ANT_CORE_RADIO_BACKEND "null" CACHE STRING
    "ant_core HAL backend: null (lifecycle and clock only, refuses every arm)")
set_property(CACHE ANT_CORE_RADIO_BACKEND PROPERTY STRINGS null)

if(ANT_CORE_RADIO_BACKEND STREQUAL "null")
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ant_radio_null.c)
else()
  message(FATAL_ERROR
    "ant_core: ANT_CORE_RADIO_BACKEND='${ANT_CORE_RADIO_BACKEND}' is not a "
    "backend. The only value today is 'null'; an nRF backend is the largest "
    "single piece of work left in the stack and adding it means adding a "
    "branch here, not a second source file alongside the null one.")
endif()

# ant_core/include goes on the search path for everything in the image, not
# just for the sources above: the modules will include each other's headers,
# and later src/ code will include ant_api.h.
#
# ../src is here for ant_event.c alone. Its *output* format is struct antr_msg
# and antr_on_message(), which live in src/ant_radio.h and pull in
# src/ant_wire.h - the generated header every ANTW_* value in ant_core comes
# from. Both are plain headers with no application dependency, so this costs
# the build nothing. Note the asymmetry deliberately: ant_event.c is the only
# module allowed to know the wire format, because it is the only one that
# produces it. Everything below it speaks frames and channels.
zephyr_include_directories(include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
