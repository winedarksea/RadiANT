# SPDX-License-Identifier: Apache-2.0
#
# Provenance: clean-room. Standard Zephyr application boilerplate.
#
# The nRF5340 application-core half of the Spike B capture rig. Built and
# flashed separately from the network-core image on purpose: they are two
# independent programs on two independent cores, and sysbuild's "one build, two
# images" arrangement would hide which of them was last programmed - which is
# the first thing you want to know when the capture is silent.
#
#   . .\scripts\env.ps1 -NcsVersion v3.2.4
#   Push-Location C:\ncs\v3.2.4
#   west -z C:\ncs\v3.2.4\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant_core\spike\promisc\appcore `
#        -d C:\Users\Colin\ant_dongle\build\promisc_appcore2 `
#        -b nrf5340dk/nrf5340/cpuapp -p always
#   Pop-Location

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_spike_promisc_appcore)

target_sources(app PRIVATE src/main.c)
