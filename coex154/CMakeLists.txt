# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# BEFORE find_package(Zephyr), because ZEPHYR_EXTRA_MODULES is read during it -
# the same ordering the root CMakeLists.txt documents at length. Appended
# unconditionally: radiant_core/CMakeLists.txt returns immediately when
# CONFIG_RADIANT_CORE is n, so the uncontrolled half of the A/B pays nothing for
# it, while leaving it out would make CONFIG_RADIANT_CORE an UNDEFINED symbol -
# and an undefined symbol in rx_ant.conf is a warning, not an error, so the
# contended build would quietly become the uncontended one.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../radiant_core")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(coex154)

target_sources(app PRIVATE src/main.c)
