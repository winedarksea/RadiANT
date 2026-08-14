# SPDX-License-Identifier: Apache-2.0
#
# The radiant_api composition test application.
#
# ── Why this is a second application and not a suite in ../src ───────────────
#
# src/ant_radio_radiant.c (radiant_api.c, before it moved out of radiant_core -
# see radiant_core/include/radiant_core/radiant_msg.h for why) is the only
# module in radiant_core's orbit with no ztest coverage, and the whole
# master-exchange path lives in it: every module underneath is tested and the
# composition is not. That gap is where the "a master answers the first slave
# exchange and none after it" bug lived, and closing it turns a two-board
# human-gated experiment into a 40-second run.
#
# The obvious move - add it to the parent application's own suite - does not
# link. radiant_event_crit_enter(), radiant_event_crit_exit() and
# radiant_event_wakeup() are port hooks, and ../src/test_event.c defines its own
# versions of all three; ../src/test_channel.c likewise defines
# radiant_channel_event_out(). ant_radio_radiant.c defines the production
# versions of the same four symbols (plus radiant_on_message()), so the two
# would collide at link - and if the collision were resolved in
# ant_radio_radiant.c's favour it would change what those 187 tests assert,
# which is worse than a link error because it is quiet.
#
# So: a sibling application, sharing ../fake_radio.c and the module itself, with
# ../src/ deliberately out of the glob. Nothing in the existing suite changes,
# and CI's `twister -T app/radiant_core/tests` walks subdirectories, so
# testcase.yaml here is picked up with no workflow edit.

cmake_minimum_required(VERSION 3.20.0)

# Two levels up: the module root is radiant_core/, this is radiant_core/tests/api.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_core_api_tests)

# Same glob rule as the parent application: a new suite is a new file in src/
# and this file does not change. The parent's glob does not reach in here and
# this one does not reach out there, which is what keeps the two link images
# separate.
file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
target_sources(app PRIVATE ${suite_sources})

# The mock radio, shared with the parent application rather than copied. It is
# the HAL implementation for this image too - prj.conf selects
# CONFIG_RADIANT_CORE_BACKEND_NONE for the same reason.
target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../fake_radio.c")

# The one thing this application exists to build: the antr_* adapter, from its
# new home outside the module. Unconditional rather than
# target_sources_ifdef(CONFIG_RADIANT_CORE_ANTR_API ...) - compiling it is the
# whole point of this application, unlike root/strap's CMakeLists.txt where the
# symbol genuinely toggles whether a real product links it.
target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/ant_radio_radiant.c")
target_sources_ifdef(CONFIG_RADIANT_SEC_HOST_MESSAGES app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/ant_sec_host.c")

# ../ so a suite writes #include "fake_radio.h", the same spelling the parent's
# suites use. The module's own zephyr_include_directories() puts
# radiant_core/include on the search path; ../../../src is added explicitly
# here (unlike the parent application, which never needs it) for
# ant_radio_radiant.c's own #include "ant_wire.h" / "ant_radio.h" and for any
# suite that wants the same.
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../src")
