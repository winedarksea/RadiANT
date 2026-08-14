# SPDX-License-Identifier: Apache-2.0
#
# Provenance: clean-room. Standard Zephyr application boilerplate, modelled on
# radiant/spike/phy_ladder/CMakeLists.txt; nothing here derives from
# sdk-ant.
#
# P0 of the multiprotocol plan: a THROWAWAY measurement of MPSL's timeslot
# arbiter. It does not include radiant at all and touches no HAL - that is
# the point. Everything the arbitrated backend is designed against is measured
# here first, because designing the margins before measuring them is how you get
# a constant that P3 cannot honour and a signal it needs but does not have.
#
#   . .\scripts\env.ps1 -NcsVersion v3.4.0
#   Push-Location C:\ncs\v3.4.0
#   west -z C:\ncs\v3.4.0\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant\spike\mpsl_arb `
#        -d C:\Users\Colin\ant_dongle\build\spike_mpsl_arb `
#        -b nrf54l15dk/nrf54l15/cpuapp -p always --no-sysbuild
#   # ... and the same with -DEXTRA_CONF_FILE=ble.conf for the contended runs
#   Pop-Location
#
# v3.4.0 AND NOT v3.2.4, deliberately. MPSL_TIMESLOT_CONTEXT_SIZE is 96 bytes in
# v3.4.0 against 48 in v3.2.4, and every number this spike produces is a
# property of the MPSL build it ran against. scripts\build_all.ps1 defaults to
# v3.2.4 for the sdk-ant release images; this is not one of those and must be
# pinned explicitly rather than inherit that default.
#
# nRF54L15 DK ONLY. It is the first silicon for the combined build - single
# core, UART ANT transport, already a supported target - and it is the part
# whose MPSL_TIMESLOT_START_JITTER_US is 0 rather than 1 because it schedules
# off GRTC. Building this for an nRF52 would measure a different arbiter.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_spike_mpsl_arb)

target_sources(app PRIVATE src/main.c)
