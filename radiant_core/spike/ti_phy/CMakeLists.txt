# SPDX-License-Identifier: Apache-2.0
#
# Provenance: clean-room. Standard Zephyr application boilerplate, plus the
# hal_ti module. Nothing here derives from sdk-ant.
#
# A standalone Zephyr application, not part of the dongle build, on the same
# terms as the five spikes beside it: outside the repo root CMakeLists.txt so
# that a spike can never be linked into a shipped image by accident, and so it
# can be built with a different board and a different Kconfig without
# perturbing anything else.
#
#   . .\scripts\env.ps1 -NcsVersion v3.4.0
#   .\scripts\fetch_hal_ti.ps1
#   Push-Location C:\ncs\v3.4.0
#   west -z C:\ncs\v3.4.0\zephyr build `
#        -s C:\Users\Colin\ant_dongle\radiant_core\spike\ti_phy `
#        -d C:\Users\Colin\ant_dongle\build\spike_ti_phy `
#        -b cc26x2r1_launchxl -p always `
#        -- -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti
#   Pop-Location
#   .\scripts\flash_ti.ps1 -HexPath build\spike_ti_phy\zephyr\zephyr.hex
#
# THE MODULE FLAG IS NOT OPTIONAL AND IS NOT A CONVENIENCE. NCS's west manifest
# filters hal_ti out of its import (see scripts/fetch_hal_ti.ps1), so a build
# without it fails at the first <ti/drivers/rf/RF.h>. That is the loud failure;
# it is mentioned here because the quiet one - a workspace where some OTHER
# hal_ti is on the path - would compile against a revision the rest of the SDK
# was never tested with.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_spike_ti_phy)

target_sources(app PRIVATE
  src/main.c
  # Replaces zephyr/soc/ti/simplelink/cc13x2_cc26x2/power.c, which is NOT
  # compiled here because prj.conf sets CONFIG_POWEROFF=n. Read the header of
  # ti_power_shim.c before changing either: Zephyr's version installs a NULL
  # calibrateFxn that TI's Power driver dereferences unchecked, and this spike
  # measured the resulting branch to address 0 on the first RF_open().
  src/ti_power_shim.c
)

# ── Two hal_ti files this application has to compile for itself ─────────────
#
# hal_ti's own CMakeLists.txt compiles RFCC26X2_multiMode.c - the RF driver -
# unconditionally, and then gates the two things that driver cannot link
# without on Kconfig symbols belonging to OTHER drivers:
#
#   driverlib/rfc.c   "Required for RFCDoorbellSendTo which is not in ROM",
#                     compiled only under CONFIG_IEEE802154_CC13XX_CC26XX,
#                     ..._SUB_GHZ or CONFIG_BLE_CC13XX_CC26XX.
#   rf_patches/*.c    likewise, and only the multi-protocol and 802.15.4
#                     patches; rf_patch_cpe_prop.c is compiled by nothing at
#                     all.
#
# So an application that uses the RF driver WITHOUT being one of Zephyr's
# in-tree radio drivers links against a dozen undefined NOROM_RFC* symbols. It
# is a loud failure and it is at the link, which is the right place, but it
# reads as "hal_ti is broken" rather than "hal_ti assumes you are the 802.15.4
# driver" - hence this note.
#
# Adding the files here rather than patching hal_ti keeps the module a
# pristine checkout of the revision scripts/fetch_hal_ti.ps1 pins, which is the
# whole point of pinning it. radiant_core/CMakeLists.txt does the same thing
# for the cc26xx backend.
if(DEFINED ZEPHYR_HAL_TI_MODULE_DIR)
  set(HAL_TI_DIR "${ZEPHYR_HAL_TI_MODULE_DIR}")
elseif(DEFINED ZEPHYR_TI_MODULE_DIR)
  set(HAL_TI_DIR "${ZEPHYR_TI_MODULE_DIR}")
else()
  message(FATAL_ERROR
    "spike/ti_phy: no hal_ti module in this build.\n"
    "NCS's west manifest filters hal_ti out of its import, so it is never "
    "present unless it has been fetched deliberately:\n"
    "  .\\scripts\\fetch_hal_ti.ps1\n"
    "and then passed on the command line:\n"
    "  -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti")
endif()

set(HAL_TI_DEV "${HAL_TI_DIR}/simplelink/source/ti/devices/cc13x2_cc26x2")

target_sources(app PRIVATE
  ${HAL_TI_DEV}/driverlib/rfc.c
  ${HAL_TI_DEV}/rf_patches/rf_patch_cpe_prop.c
)
