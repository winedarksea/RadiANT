# SPDX-License-Identifier: Apache-2.0
#
# The nRF low-power timebase arithmetic (src/radiant_radio_nrf_lp.h, power
# plan Track B). The header is pure - plain integers, no Zephyr, no registers
# - so this application compiles nothing of radiant but that header: no module,
# no backend, no fake radio. A fourth application rather than a suite in ../src
# for that reason: the header is only ever included by the nRF backend under
# CONFIG_RADIANT_NRF_LOW_POWER, and ../src builds the null backend with
# its own fakes, which this has no use for.
#
# CI's `twister -T app/radiant/tests` walks subdirectories, so testcase.yaml
# here is picked up with no workflow edit; tests/expected_counts.yaml holds its
# floor.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_lp_clock_tests)

file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
target_sources(app PRIVATE ${suite_sources})

# radiant/src, for radiant_radio_nrf_lp.h (internal to the backend, so not on
# the module's public include path).
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
