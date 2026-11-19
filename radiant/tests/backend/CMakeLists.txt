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

# -DBACKEND_LP=1 (scenario radiant.backend.lp): the same backend with
# CONFIG_RADIANT_NRF_LOW_POWER and its simulated GRTC/HFXO instead of the MPSL
# gate. The two configurations define the same symbols, so one image each.
# -DBACKEND_LP_RTC=1 as well (scenario radiant.backend.lp_rtc): the same
# low-power cases against a simulated RTC1 and nRF52 TIMER (LPT_RTC), plus
# RTC1's own cases in src_lp_rtc/.
if(BACKEND_LP)
  file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src_lp/*.c")
  if(BACKEND_LP_RTC)
    file(GLOB rtc_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src_lp_rtc/*.c")
    list(APPEND suite_sources ${rtc_sources})
    target_compile_definitions(app PRIVATE LPT_RTC=1)
  endif()
  set(under_test "${CMAKE_CURRENT_SOURCE_DIR}/backend_lp_under_test.c")
else()
  file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
  set(under_test "${CMAKE_CURRENT_SOURCE_DIR}/backend_under_test.c")
endif()
target_sources(app PRIVATE ${suite_sources})
target_sources(app PRIVATE
  "${under_test}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/radiant_addr_split.c")

target_include_directories(app BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/fakes")
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
