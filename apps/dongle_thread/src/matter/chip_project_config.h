/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CHIP's per-project configuration header, named by
 * CONFIG_CHIP_PROJECT_CONFIG in matter.conf and #included by
 * connectedhomeip's CHIPProjectConfig.h very early in every CHIP translation
 * unit.
 *
 * OURS, NOT VENDORED, and the difference matters. NCS's
 * applications/matter_bridge/src/chip_project_config.h has the same shape and
 * the same load-bearing line, but it also silences seven CHIP log modules to
 * fit its own image. Copying that wholesale would have turned off Zcl,
 * InteractionModel and DataManagement PROGRESS logging in a build whose first
 * bring-up problem will almost certainly be a cluster that does not answer -
 * see src/matter/README.md for what else was left behind and why. So this file
 * restates the one line that is a correctness requirement and nothing else.
 *
 * ---------------------------------------------------------------------------
 * TRAP 15: THIS NUMBER SIZES REAL CLUSTER ARRAYS, NOT JUST A TV APP'S TABLE
 * ---------------------------------------------------------------------------
 *
 * CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT's own comment in
 * modules/lib/matter/src/include/platform/CHIPDeviceConfig.h describes it as
 * the dynamic endpoint count "for the TV app", which reads like something no
 * bridge needs to think about. It is not.
 *
 * Descriptor is a CODE-DRIVEN cluster with one server instance per endpoint
 * instance, and
 * modules/lib/matter/src/app/clusters/descriptor/CodegenIntegration.cpp sizes
 * its gServers array as (fixed endpoints + dynamic endpoints). Set this
 * smaller than the number of dynamic endpoints the bridge can create and the
 * Nth one registers WITH NO DESCRIPTOR CLUSTER: it enumerates, it answers, it
 * has no cluster list, and there is no warning and no error anywhere. A
 * controller shows an entity that does nothing.
 *
 * So three numbers must be equal, in three files, in two languages:
 *
 *   CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT   here
 *   CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER  src/matter/Kconfig
 *   RADIANT_MATTER_MAX_ENDPOINTS                radiant/src/bridge/radiant_matter.h
 *
 * The first is DEFINED as the second, so those two cannot drift. The third is
 * a C macro in a module this file must not include from here (this header is
 * pulled into CHIP's own headers before anything else exists), so it is
 * checked in src/matter/bridge_core_asserts.cpp instead - which is the whole
 * reason that file exists. radiant_matter.h's own comment at line 163 asks for
 * exactly this and names the trap.
 */

#pragma once

#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER
