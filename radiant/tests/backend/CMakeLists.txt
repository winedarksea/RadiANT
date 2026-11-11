# SPDX-License-Identifier: Apache-2.0
#
# The nRF backend's completion path. radiant_radio_nrf.c decides when a grant
# is given back, and that decision is in its interrupt handler, which no other
# application compiles. This one includes the file against register models, so
# the only production code under test is the backend itself.

cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_backend_tests)

file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
target_sources(app PRIVATE ${suite_sources})
target_sources(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/backend_under_test.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/radiant_addr_split.c")

target_include_directories(app BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/fakes")
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
