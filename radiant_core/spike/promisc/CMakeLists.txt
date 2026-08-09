# SPDX-License-Identifier: Apache-2.0
#
# Provenance: clean-room. Standard Zephyr application boilerplate; nothing here
# derives from sdk-ant.
#
# A standalone Zephyr application, deliberately outside the repo root
# CMakeLists.txt so a spike can never be linked into a shipped image by
# accident. Two board targets, and the second one is the interesting build:
#
#   . .\scripts\env.ps1 -NcsVersion v3.2.4
#   Push-Location C:\ncs\v3.2.4
#
#   # nRF54L15 DK application core - the proven-silicon capture board
#   west -z C:\ncs\v3.2.4\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant_core\spike\promisc `
#        -d C:\Users\Colin\ant_dongle\build\promisc_l15 `
#        -b nrf54l15dk/nrf54l15/cpuapp -p always
#
#   # nRF5340 DK network core - where the RADIO lives on that part. This image
#   # alone will neither run nor print: the network core is held in reset by
#   # NETWORK.FORCEOFF, and its console pins belong to the application core,
#   # until an application-core image fixes both. Build and flash appcore/ as
#   # well - it exists for exactly those two jobs and nothing else.
#   west -z C:\ncs\v3.2.4\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant_core\spike\promisc `
#        -d C:\Users\Colin\ant_dongle\build\promisc_net2 `
#        -b nrf5340dk/nrf5340/cpunet -p always
#
#   Pop-Location

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_spike_promisc)

target_sources(app PRIVATE src/main.c)
