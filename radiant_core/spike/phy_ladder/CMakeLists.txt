# SPDX-License-Identifier: Apache-2.0
#
# Provenance: clean-room. Standard Zephyr application boilerplate, modelled on
# radiant_core/spike/x1m_len/CMakeLists.txt; nothing here derives from sdk-ant.
#
# A standalone Zephyr application, deliberately outside the repo root
# CMakeLists.txt so a spike can never be linked into a shipped image by
# accident, and so it can be built for two different boards with two different
# console transports without perturbing anything else.
#
#   . .\scripts\env.ps1 -NcsVersion v3.2.4
#   Push-Location C:\ncs\v3.2.4
#   west -z C:\ncs\v3.2.4\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant_core\spike\phy_ladder `
#        -d C:\Users\Colin\ant_dongle\build\spike_phy_ladder_54l15 `
#        -b nrf54l15dk/nrf54l15/cpuapp -p always --no-sysbuild
#   west -z C:\ncs\v3.2.4\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant_core\spike\phy_ladder `
#        -d C:\Users\Colin\ant_dongle\build\spike_phy_ladder_feather `
#        -b adafruit_feather_nrf52840/nrf52840/uf2 -p always --no-sysbuild
#   Pop-Location
#
# TWO BOARDS, AND NEITHER IS THE nRF5340 DK. CONFIG_RADIANT_CORE_BACKEND_NRF is
# `depends on SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX`, which no nRF53
# core sets, and the nRF5340's RADIO is on its network core anyway. Asking for
# that board does not fail: Kconfig falls back to the null backend and the image
# runs the console and moves no bits. src/main.c BUILD_ASSERTs against exactly
# that, because on a LADDER the null backend manufactures 100 % loss at every
# rung - which reads as a rig with too much attenuation.

cmake_minimum_required(VERSION 3.20.0)

# radiant_core consumed as a module, before find_package(Zephyr), with
# CONFIG_RADIANT_CORE=y in prj.conf doing the selecting - the same door
# radiant_core/tests, spike/x1m_len and a product build go through. The module
# owns its own source list; this file does not duplicate it.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_spike_phy_ladder)

target_sources(app PRIVATE src/main.c)
