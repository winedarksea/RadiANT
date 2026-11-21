# SPDX-License-Identifier: Apache-2.0
#
# The TI CC26xx backend (radiant_radio_cc26xx.c), included against a fake TI
# RF driver (fakes/ti/drivers/rf/RF.h, fake_rf.c) and hal_ti's REAL driverlib
# command headers (rf_*_cmd.h, rf_data_entry.h, rf_patches/*.h), so the only
# production code under test is the backend - and, in the coex scenario, its
# scheduler-policy hooks (radiant_radio_cc26xx_arb.c).

cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_backend_cc26xx_tests)

# hal_ti, as radiant/CMakeLists.txt finds it for a real cc26xx build. A
# native_sim build gets nothing from the TI SoC layer, so the device's
# driverlib directory is put on the include path here, behind fakes/.
if(DEFINED ZEPHYR_HAL_TI_MODULE_DIR)
  set(cc_hal_ti "${ZEPHYR_HAL_TI_MODULE_DIR}")
elseif(DEFINED ZEPHYR_TI_MODULE_DIR)
  set(cc_hal_ti "${ZEPHYR_TI_MODULE_DIR}")
else()
  message(FATAL_ERROR
    "radiant/tests/backend_cc26xx needs the hal_ti module (west.yml's "
    "hal_ti project, group `ti`) for TI's driverlib command headers.")
endif()
set(cc_ti_dev "${cc_hal_ti}/simplelink/source/ti/devices/cc13x2_cc26x2")
if(NOT EXISTS "${cc_ti_dev}/driverlib/rf_prop_cmd.h")
  message(FATAL_ERROR
    "radiant/tests/backend_cc26xx: ${cc_ti_dev}/driverlib/rf_prop_cmd.h "
    "is missing; is hal_ti checked out?")
endif()

# -DBACKEND_COEX=1 (scenario radiant.backend_cc26xx.coex): the backend under
# CONFIG_RADIANT_BACKEND_CC26XX_COEX (RF_scheduleCmd, patched caps) plus
# radiant_radio_cc26xx_arb.c and its own cases in src_coex/.
#
# src_caps/ (the caps parity test) runs in every scenario. -DBACKEND_SHORT_MATCH=1
# (scenario radiant.backend_cc26xx.short_match) builds the backend with
# CONFIG_RADIANT_SEARCH_SHORT_MATCH, the shipping default, and runs only that:
# the src/ suites are written against the 3-byte hardware address.
file(GLOB caps_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src_caps/*.c")
if(BACKEND_SHORT_MATCH)
  set(suite_sources ${caps_sources})
  target_compile_definitions(app PRIVATE BACKEND_SHORT_MATCH=1)
else()
  file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
  list(APPEND suite_sources ${caps_sources})
endif()
if(BACKEND_COEX)
  file(GLOB coex_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src_coex/*.c")
  list(APPEND suite_sources ${coex_sources})
  target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/radiant_radio_cc26xx_arb.c")
  target_compile_definitions(app PRIVATE BACKEND_COEX=1)
endif()
target_sources(app PRIVATE ${suite_sources})
target_sources(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/backend_cc26xx_under_test.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/fake_rf.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/radiant_addr_split.c")

target_include_directories(app BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/fakes")
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
  "${cc_ti_dev}")
