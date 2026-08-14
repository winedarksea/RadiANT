# SPDX-License-Identifier: Apache-2.0
#
# Provenance: clean-room. Standard Zephyr application boilerplate; nothing here
# derives from sdk-ant.
#
# A standalone Zephyr application, deliberately outside the repo root
# CMakeLists.txt so a spike can never be linked into a shipped image by
# accident, and so it can be built with a different board and a different
# Kconfig without perturbing anything else.
#
# Unlike spike/rx_raw and spike/promisc, this one does NOT poke RADIO registers.
# It consumes radiant as a Zephyr module and drives the radio through
# radiant/include/radiant/radiant_radio_hal.h, exactly as the
# unit-test application and a product build do. That is the point: ADR 0007's
# open question is "can today's code already do this", and an experiment that
# answered it with its own private register writes would have proved nothing
# about today's code.
#
#   . .\scripts\env.ps1 -NcsVersion v3.2.4
#   Push-Location C:\ncs\v3.2.4
#   west -z C:\ncs\v3.2.4\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant\spike\x1m_len `
#        -d C:\Users\Colin\ant_dongle\build\spike_x1m_len `
#        -b nrf54l15dk/nrf54l15/cpuapp -p always --no-sysbuild
#   Pop-Location
#
# THE BOARD IS THE nRF54L15 DK AND NOT THE nRF5340 DK, and that is not a
# preference - see README.md, "Why not the nRF5340 DK". The nRF5340's RADIO is
# on its network core, and RADIANT_BACKEND_NRF depends on
# SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX, which no nRF53 core sets.
# Asking for that board does not fail; Kconfig falls back to the null backend
# and the image transmits nothing. src/main.c BUILD_ASSERTs against exactly
# that.

cmake_minimum_required(VERSION 3.20.0)

# radiant consumed as a module, before find_package(Zephyr), with
# CONFIG_RADIANT=y in prj.conf doing the selecting - the same door
# radiant/tests and a product build go through. The module owns its own
# source list; this file does not duplicate it.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_spike_x1m_len)

target_sources(app PRIVATE src/main.c)
