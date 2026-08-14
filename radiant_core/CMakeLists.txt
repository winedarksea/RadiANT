# SPDX-License-Identifier: Apache-2.0
#
# radiant_core - the clean-room ANT-compatible link layer, as a Zephyr module.
#
# ── Why this file is a module and not a subdirectory ────────────────────────
#
# It used to be pulled in with add_subdirectory(radiant_core) from the
# application's own CMakeLists.txt, and listed its sources with
# target_sources(app ...). That worked, but it ruled out the library form: the
# supported way to say "this is a library" is zephyr_library(), and
# add_subdirectory() from an application puts this file in Zephyr's
# *application* mode, where it prints
#
#   zephyr_library() or zephyr_library_named() called in Zephyr CMake
#   application mode. `radiant_core` will not be treated as a Zephyr library.
#
# and then compiles nothing at all. The build still reached the link and still
# failed on the same undefined antr_* references, so the symptom was identical
# to a correct build - the only way to notice was that no object file existed.
# A library form that silently drops every source it was given is exactly the
# kind of green light that means nothing, and it is recorded here because this
# file is the resolution of it, not because the trap is still live.
#
# The resolution is module shape: zephyr/module.yml plus an entry in
# ZEPHYR_EXTRA_MODULES. That is how every protocol stack in this ecosystem
# ships - OpenThread, LoRaMac-node, the Wi-Fi supplicant, and sdk-ant itself,
# the thing being replaced. It also means the source list lives here and only
# here: the unit-test application used to keep a duplicate copy of it, and a
# duplicate that silently stops growing is a suite that stops testing the
# module it names.
#
# The application always appends this module and CONFIG_RADIANT_CORE gates it,
# rather than appending it only for -DANT_RADIO=core. An sdk_ant build can then
# assert CONFIG_RADIANT_CORE=n positively; the module's mere absence proves
# nothing. The guard below is what makes that gate real - with it, an sdk_ant
# build cannot reach a <radiant_core/...> header even by accident.

if(NOT CONFIG_RADIANT_CORE)
  return()
endif()

# include/ carries the namespaced <radiant_core/...> headers.
#
# ../src is here for src/radiant_event.c and src/radiant_api.c alone. Their
# *output* format is struct antr_msg and antr_on_message(), which live in
# src/ant_radio.h and pull in src/ant_wire.h - the generated header every
# ANTW_* value in radiant_core comes from. Both are plain headers with no
# application dependency, so this costs the build nothing. Note the asymmetry
# deliberately: radiant_event.c is the only *module* allowed to know the wire
# format, because it is the only one that produces it. Everything below it
# speaks frames and channels.
zephyr_include_directories(include ${CMAKE_CURRENT_SOURCE_DIR}/../src)

zephyr_library()
zephyr_library_sources(
  src/radiant_frame.c
  src/radiant_sched.c
  src/radiant_channel.c
  src/radiant_search.c
  src/radiant_event.c
  src/radiant_ack.c
  src/radiant_burst.c
  src/radiant_noise.c
)

# Single-bit CRC repair. Compiled unconditionally rather than behind its own
# Kconfig symbol, and the ~1 KB it costs is worth what it buys: the unit-test
# application can exercise the arithmetic whether or not a given build turns
# the feature on, so the syndrome table's duplicate check runs in CI on every
# build instead of only on the ones that opted in. CONFIG_RADIANT_CORE_CRC_REPAIR
# gates the CALL SITE in src/radiant_api.c, which is where the false-accept
# trade-off is actually taken.
zephyr_library_sources(src/radiant_crc_repair.c)

# The third refutation for a repaired frame: is the payload a physically
# possible reading, for the handful of device types where that is decidable
# from one frame. Compiled unconditionally for the same reason as
# radiant_crc_repair.c above; CONFIG_RADIANT_CORE_PROFILE_SANITY gates the
# call site in src/radiant_api.c.
zephyr_library_sources(src/radiant_profile_sanity.c)

# The antr_* adapter. Kconfig-gated because it is the one file here that
# includes an application header; see RADIANT_CORE_ANTR_API.
zephyr_library_sources_ifdef(CONFIG_RADIANT_CORE_ANTR_API src/radiant_api.c)

# The channel-quality map. Gated rather than compiled unconditionally like
# radiant_crc_repair.c above, and the difference is the same one the security
# block below draws: this file is 1000 bytes of RAM, not just text, and a map
# allocated in every build so that a suite could reach it would be 1000 bytes
# spent by every user who never turns energy detect on. The unit-test
# application reaches it by selecting CONFIG_RADIANT_CORE_ED_SCAN=y in its own
# prj.conf, which is the same door a bench build uses.
zephyr_library_sources_ifdef(CONFIG_RADIANT_CORE_ED_SCAN src/radiant_chanmap.c)

# ── Security ────────────────────────────────────────────────────────────────
#
# Gated, unlike radiant_crc_repair.c above, and the difference is deliberate.
# CRC repair is compiled unconditionally so CI exercises its arithmetic on
# every build; security is not, because the governing constraint here is that
# almost nobody turns it on and it must cost those users LITERALLY ZERO - which
# the `zero-cost` CI job asserts section by section. A file compiled "so the
# tests can reach it" would fail that job on the day it landed.
#
# The unit-test application reaches the primitives by selecting
# CONFIG_RADIANT_SEC=y in its own prj.conf, which is the same door a real
# application uses.
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC src/radiant_sec_aes.c)
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC src/radiant_sec.c)

# The optional entropy backend (ADR 0009). Its own symbol, default n, and the
# only file here that includes a PSA header - which is the whole reason it is a
# file rather than three lines inside radiant_sec_aes.c, whose include list is a
# deliberately protected property.
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC_RNG_PSA src/radiant_sec_rng_psa.c)

# The compat attestation primitives, under their own symbol and inside
# CONFIG_RADIANT_SEC's `if` block in Kconfig. A file, not a directory: this
# lives beside the other radiant_core sources rather than under src/profiles/,
# because it names no page and no field and therefore has nothing to do with
# the profile boundary docs/decisions/0002 draws. What a compat page MEANS is
# src/profiles/'s business; this authenticates bytes.
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC_COMPAT src/radiant_sec_compat.c)

# The host surface, under its own symbol because it is the one security file
# that reaches an application header - a node with a key baked in and no serial
# host has no use for 0xF1-0xF4 and should not link them.
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC_HOST_MESSAGES
			     src/radiant_sec_host.c)

# X25519, under its own symbol and in src/ext/ because it is a standard
# algorithm rather than RadiANT protocol logic - the directory is the signal
# that a PKA backend may replace the whole file. Default n: most deployments
# should pair out of band (a QR code, a typed key), which needs no protocol and
# has no attack surface during pairing at all.
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC_PAIRING_X25519
			     src/ext/radiant_x25519.c)

# The pairing logic is ours and stays out of src/ext/, which holds only the
# standard algorithm. The split is the seam: replacing X25519 with a PKA
# backend replaces one file and leaves this one untouched.
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC_PAIRING_X25519
			     src/radiant_sec_pair.c)

# ── The radio backend ───────────────────────────────────────────────────────
#
# One of these, or none, and never two - see the choice in Kconfig. The two
# unwritten ones are a configure-time error naming their phase, because loud
# beats a link error whose cause is three layers away.
if(CONFIG_RADIANT_CORE_BACKEND_NULL)
  zephyr_library_sources(src/radiant_radio_null.c)
elseif(CONFIG_RADIANT_CORE_BACKEND_CC26XX)
  # The second vendor. Written against include/radiant_core/radiant_radio_hal.h
  # and the measurements in radiant_core/spike/ti_phy, and against nothing else
  # - radiant_radio_nrf.c is expressly NOT a template for it, because most of
  # that file is register arithmetic for a peripheral this part does not have.
  #
  # Guarded on existing rather than listed unconditionally, and the guard names
  # the phase, which is the precedent the two unwritten backends set above. A
  # missing source file makes CMake stop with "cannot find" and no clue as to
  # why, three layers from anything a reader would recognise.
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/radiant_radio_cc26xx.c")
    message(FATAL_ERROR
      "radiant_core: the cc26xx backend was selected but "
      "src/radiant_radio_cc26xx.c does not exist. It is written in P2 of the "
      "TI port, after the PHY spike in spike/ti_phy has received a real ANT+ "
      "frame. Build -DRADIANT_BACKEND=null until then: that image enumerates, "
      "answers every host command and runs every state machine, which is the "
      "whole port minus the radio.")
  endif()
  zephyr_library_sources(src/radiant_radio_cc26xx.c)

  # ── Three things hal_ti and the SoC layer will not do for a non-in-tree
  #    radio driver. All three were found by spike/ti_phy; the first two are
  #    link errors and the third is a branch to address zero at run time.
  if(DEFINED ZEPHYR_HAL_TI_MODULE_DIR)
    set(RADIANT_HAL_TI_DIR "${ZEPHYR_HAL_TI_MODULE_DIR}")
  elseif(DEFINED ZEPHYR_TI_MODULE_DIR)
    set(RADIANT_HAL_TI_DIR "${ZEPHYR_TI_MODULE_DIR}")
  else()
    message(FATAL_ERROR
      "radiant_core: the cc26xx backend needs the hal_ti module and this "
      "build has none. NCS's west manifest filters hal_ti out of its import, "
      "so it is never present unless it has been fetched deliberately:\n"
      "  .\\scripts\\fetch_hal_ti.ps1\n"
      "and then passed on the command line:\n"
      "  -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti")
  endif()

  # (1) and (2): hal_ti compiles RFCC26X2_multiMode.c - the RF driver -
  # unconditionally, and then compiles the NOROM_RFC* functions that driver
  # cannot link without only under CONFIG_IEEE802154_CC13XX_CC26XX, ..._SUB_GHZ
  # or CONFIG_BLE_CC13XX_CC26XX. rf_patch_cpe_prop.c is compiled by nothing at
  # all. Adding the two files here rather than patching hal_ti keeps that
  # checkout a pristine copy of the revision scripts/fetch_hal_ti.ps1 pins,
  # which is the whole point of pinning it.
  zephyr_library_sources(
    ${RADIANT_HAL_TI_DIR}/simplelink/source/ti/devices/cc13x2_cc26x2/driverlib/rfc.c
    ${RADIANT_HAL_TI_DIR}/simplelink/source/ti/devices/cc13x2_cc26x2/rf_patches/rf_patch_cpe_prop.c
  )

  # The multi-protocol patch, only under the symbol that selects it
  # (RADIANT_CORE_BACKEND_CC26XX_MULTI_PATCH, selected in turn by
  # ..._COEX - see Kconfig). rf_patch_cpe_prop.c above stays unconditional:
  # it is still what a plain, non-coex cc26xx build boots under.
  zephyr_library_sources_ifdef(
    CONFIG_RADIANT_CORE_BACKEND_CC26XX_MULTI_PATCH
    ${RADIANT_HAL_TI_DIR}/simplelink/source/ti/devices/cc13x2_cc26x2/rf_patches/rf_patch_cpe_multi_protocol.c
  )

  # RFCC26XX_schedulerPolicy (the submit/execute hooks) - see that file's own
  # header for why it is not folded into radiant_radio_cc26xx.c.
  zephyr_library_sources_ifdef(CONFIG_RADIANT_CORE_BACKEND_CC26XX_COEX
			       src/radiant_radio_cc26xx_arb.c)

  # (3): PowerCC26X2_config and the two scheduler hooks. Zephyr's version of
  # that file installs a NULL calibrateFxn which TI's driver dereferences
  # without checking - see src/radiant_ti_power.c for the fault this produced
  # on hardware. Ours replaces it, which is only correct if Zephyr's is not
  # compiled, or compiles it but takes a branch with a non-NULL calibrateFxn.
  #
  # RE-KEYED FOR COEX (was `if(CONFIG_PM OR CONFIG_PM_DEVICE OR
  # CONFIG_POWEROFF)` alone): that conflated "power.c is compiled" with
  # "power.c is unsafe". CONFIG_IEEE802154_CC13XX_CC26XX selects CONFIG_PM
  # (see Kconfig.defconfig in the Zephyr SoC layer), which DOES compile
  # Zephyr's soc/ti/simplelink/cc13x2_cc26x2/power.c - but that file's radio
  # branch is field-for-field identical to src/radiant_ti_power.c
  # (calibrateFxn = &PowerCC26XX_calibrate, enablePolicy = false, policyFxn =
  # NULL), so under 802.15.4 ours must NOT exist rather than duplicate it.
  # The three symbols below decide which *branch* power.c takes, which is
  # what actually matters - not whether CONFIG_PM is set at all.
  if(CONFIG_IEEE802154_CC13XX_CC26XX OR CONFIG_IEEE802154_CC13XX_CC26XX_SUB_GHZ
     OR CONFIG_BLE_CC13XX_CC26XX)
    # Zephyr's power.c takes the branch with a non-NULL calibrateFxn -
    # identical to ours field for field (see above). Ours must not exist.
  elseif(CONFIG_PM OR CONFIG_PM_DEVICE OR CONFIG_POWEROFF)
    message(FATAL_ERROR
      "radiant_core: the cc26xx backend cannot be built with CONFIG_PM, "
      "CONFIG_PM_DEVICE or CONFIG_POWEROFF outside 802.15.4/BLE coexistence. "
      "Any of the three makes Zephyr compile "
      "soc/ti/simplelink/cc13x2_cc26x2/power.c, which defines "
      "PowerCC26X2_config - and so does src/radiant_ti_power.c, because "
      "Zephyr's version installs a NULL calibrateFxn that TI's Power driver "
      "dereferences unchecked on the first RF_open(). Turn the symbol off, or "
      "read src/radiant_ti_power.c and decide deliberately which definition "
      "this build should have.")
  else()
    zephyr_library_sources(src/radiant_ti_power.c)
  endif()
elseif(CONFIG_RADIANT_CORE_BACKEND_NRF)
  zephyr_library_sources(src/radiant_radio_nrf.c)
  # Who owns the RADIO. A seam inside the backend rather than a second backend
  # beside it - see src/radiant_radio_nrf_gate.h for why the file is not split.
  # Exactly one of these is ever compiled.
  if(CONFIG_RADIANT_CORE_BACKEND_NRF_GATE_MPSL)
    # The FATAL_ERROR that used to stand here named P3 as unwritten. P3 is
    # written and receives - see the file's own header for the measurement and
    # for the fourteen bugs between those two states - so the refusal is gone
    # and the file is simply compiled.
    zephyr_library_sources(src/radiant_radio_nrf_gate_mpsl.c)
  else()
    zephyr_library_sources(src/radiant_radio_nrf_gate_direct.c)
  endif()
  message(WARNING
    "radiant_core: the nrf backend transmits and receives. Its ramp-up and "
    "turnaround figures are datasheet values, and the t_sync calibration "
    "constants - now three, one per PHY on receive plus one shared on "
    "transmit - are SEEDED from Zephyr's per-SoC BLE chain delays rather than "
    "measured on this bench - a defensible prior at a different deviation, not "
    "the wired two-board trigger the HAL contract asks for. The coded PHY's "
    "receive constant (-30 us) is three times the 1 M one and is no better "
    "established; ADR 0007 records both as bounded rather than measured. "
    "Every register "
    "fact is measured on nRF54L15 only. Expect timing to be approximately "
    "calibrated rather than exact - see the 'what is measured and what is not' "
    "header in src/radiant_radio_nrf.c before reading any A/B timing result "
    "off this build.")
endif()
