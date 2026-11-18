# SPDX-License-Identifier: Apache-2.0
#
# Pass B1 spike for the nRF HFXO/timebase release (power plan, Track B).
# Throwaway: it does not link the radiant stack. It drives the same peripherals
# the nRF backend uses (RADIO, the radiant,radio-timer TIMER, (D)PPI through
# nrfx_gppi, the HFXO through the clock-control onoff manager) plus the LF
# counter the design anchors to (GRTC on nRF54L15, RTC1 user channels on
# nRF52840), and prints the measurements. Results: docs/spike-lp-clock-results.md.
#
#   . .\scripts\env.ps1
#   Push-Location C:\ncs\v3.4.0
#   # nRF54L15 DK, plain (UART console COM3 @115200, needs DTR)
#   cmd /c "west build -b nrf54l15dk/nrf54l15/cpuapp -p always --no-sysbuild
#       -s <wt>\radiant\spike\lp_clock -d <wt>\build\lp_l15 > log 2>&1"
#   # nRF54L15 DK with BT/MPSL (S2 + short S3 only)
#   ... -d <wt>\build\lp_l15_ble -- -DEXTRA_CONF_FILE=ble.conf
#   # Pro Micro nRF52840 (USB CDC console, enabled only AFTER the HFXO
#   # experiments, because USB holds the HFXO while it is up)
#   ... -b promicro_nrf52840/nrf52840/uf2 -d <wt>\build\lp_pm
#   Pop-Location

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_spike_lp_clock)

target_sources(app PRIVATE src/main.c)

# -DSPIKE_S3ONLY=1: S2 + the v2 S3 only (compare while asleep, capture awake).
if(SPIKE_S3ONLY)
  target_compile_definitions(app PRIVATE SPIKE_S3ONLY=1)
endif()
