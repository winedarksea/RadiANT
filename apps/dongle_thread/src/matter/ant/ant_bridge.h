/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one C++ entry point ant_bridge.cpp exposes. Everything else it does is
 * reached either through the C shim (ant_matter.h) or through
 * radiant_matter.c's own __weak seam.
 */

#pragma once

#include <lib/core/CHIPError.h>

namespace RadiantMatter
{

/*
 * Creates the bridge, restores persisted endpoints and starts the 1 Hz flush
 * pacer.
 *
 * MUST be called from Nrf::Matter::InitData::mPostServerInitClbk and from
 * nowhere else. That callback runs on the CHIP thread after Server::Init(),
 * which is the only point at which emberAfSetDynamicEndpoint() has tables to
 * write into and a reporting engine to notify.
 */
CHIP_ERROR PostServerInit();

} /* namespace RadiantMatter */
