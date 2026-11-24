/* SPDX-License-Identifier: Apache-2.0
 *
 * The shim's version, for its VERSIONINFO resource (shim_version.rc) and its
 * log banner. scripts/zwift_ant_shim.ps1 knows an installed DLL is one of
 * these by SHIM_PRODUCT_NAME in that resource, read without loading the DLL,
 * and refuses to replace or delete a file that does not carry it. Bump
 * SHIM_VERSION_* on any change that should be told apart in a log.
 * Plain #defines only: the resource compiler reads this file too.
 */
#ifndef SHIM_VERSION_H
#define SHIM_VERSION_H

#define SHIM_VERSION_BIN  2026,11,24,2
#define SHIM_VERSION_STR  "2026.11.24.2"
#define SHIM_PRODUCT_NAME "RadiANT Zwift ANT shim"

#endif /* SHIM_VERSION_H */
