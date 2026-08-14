# SPDX-License-Identifier: Apache-2.0
#
# Provenance: clean-room. Standard Zephyr application boilerplate; nothing here
# derives from sdk-ant.
#
# This is a standalone Zephyr application, not part of the dongle build. It is
# deliberately outside the repo root CMakeLists.txt so that a spike can never
# be linked into a shipped image by accident, and so that it can be built with
# a different board and a different Kconfig without perturbing anything else:
#
#   . .\scripts\env.ps1 -NcsVersion v3.2.4
#   Push-Location C:\ncs\v3.2.4
#   west -z C:\ncs\v3.2.4\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant\spike\rx_raw `
#        -d C:\Users\Colin\ant_dongle\build\spike_rx_raw `
#        -b nrf54l15dk/nrf54l15/cpuapp -p always
#   Pop-Location

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_spike_rx_raw)

target_sources(app PRIVATE src/main.c)
