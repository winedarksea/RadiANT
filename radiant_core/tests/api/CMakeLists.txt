# SPDX-License-Identifier: Apache-2.0
#
# The radiant_api composition test application.
#
# ── Why this is a second application and not a suite in ../src ───────────────
#
# radiant_api.c is the only module in radiant_core with no ztest coverage, and
# the whole master-exchange path lives in it: every module underneath is tested
# and the composition is not. That gap is where the "a master answers the first
# slave exchange and none after it" bug lived, and closing it turns a two-board
# human-gated experiment into a 40-second run.
#
# The obvious move - flip CONFIG_RADIANT_CORE_ANTR_API=y in ../prj.conf - does
# not link. radiant_event_crit_enter(), radiant_event_crit_exit() and
# radiant_event_wakeup() are port hooks, and ../src/test_event.c defines its own
# versions of all three; ../src/test_channel.c likewise defines
# radiant_channel_event_out(). radiant_api.c defines the production versions of
# the same four symbols, so the two would collide at link - and if the collision
# were resolved in radiant_api.c's favour it would change what those 187 tests
# assert, which is worse than a link error because it is quiet.
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

# ../ so a suite writes #include "fake_radio.h", the same spelling the parent's
# suites use. The module's own zephyr_include_directories() already puts
# radiant_core/include and the application's src/ (for src/ant_radio.h and the
# generated src/ant_wire.h) on the search path.
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
