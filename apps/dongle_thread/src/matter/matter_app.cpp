/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Package E6: where the Matter stack starts, and where it does NOT.
 *
 * ===========================================================================
 * main() DOES NOT MOVE
 * ===========================================================================
 *
 * nrf/applications/matter_bridge's main() is `AppTask::StartApp()`, which is
 * `Init()` followed by `while (true) DispatchNextTask()`, and it is easy to
 * read that as "Matter owns main()". It does not. Two different loops are
 * involved and only one of them is that one:
 *
 *   - THE CHIP EVENT LOOP is its own Zephyr thread, started inside
 *     Nrf::Matter::StartServer() -> PlatformMgr().StartEventLoopTask(). It
 *     runs at K_PRIO_PREEMPT(1) with CONFIG_CHIP_TASK_STACK_SIZE bytes of
 *     stack, and every CHIP data model operation, every BLE commissioning
 *     exchange and every OpenThread callback happens on it.
 *   - THE APPLICATION TASK QUEUE is `Nrf::DispatchNextTask()`, a blocking
 *     read of a message queue that the samples' buttons and LEDs post to. It
 *     is ordinary application work and it has no privileged relationship with
 *     CHIP at all.
 *
 * So this application keeps its own main() - apps/common/ant_dongle_main.c,
 * byte for byte the one apps/dongle boots through - and gives the second loop
 * a thread of its own. Moving main() would have meant this application's boot
 * sequence, its USB class registration and its ANT radio bring-up all becoming
 * something that happens inside a Matter sample's task, for no benefit.
 *
 * ===========================================================================
 * WHY THE HOOK MUST NOT BLOCK
 * ===========================================================================
 *
 * ant_matter_start() is called from ant_dongle_post_radio_init(), which runs
 * after antr_init() and BEFORE usb_ant_class_init(). Nrf::Matter::StartServer()
 * is documented as "a blocking function which ... waits until all Matter server
 * components are initialized", and Server::Init() on a first boot does key
 * generation, settings enumeration and network bring-up. Blocking there would
 * hold up USB enumeration by however long that takes.
 *
 * This repository has paid for exactly that once already:
 * CONFIG_NET_CONFIG_NEED_IPV6 blocking in net_config_init() before main() for
 * 31.04 s on an unattached MTD, which was read as a hung board by a human and
 * by ant_verify.py in the same session (thread.conf:60-75). The lesson is not
 * "that particular symbol"; it is that a pre-transport stall is indistinguishable
 * from a dead board. So the hook creates a thread and returns.
 *
 * ===========================================================================
 * THREAD PRIORITY - TRAP 9, MEASURED RATHER THAN PRE-EMPTED
 * ===========================================================================
 *
 * The plan's trap 9 says CHIP's event loop at K_PRIO_PREEMPT(1) sits above the
 * ANT host thread (5) and radiant's event thread (6), so commissioning crypto
 * can preempt ANT event delivery, and it says MEASURE BEFORE OVERRIDING.
 * Nothing here overrides anything: CONFIG_CHIP_TASK_PRIORITY is left at
 * connectedhomeip's own default and the thread below is the low-priority
 * application task, not the event loop.
 *
 * What this file DOES do is make the question answerable, by putting the
 * application task at the same priority as the bridge pump - strictly below
 * the ANT host thread - so that at least the half of the Matter plane this
 * repository owns cannot be the thing that delays a receive window. The other
 * half is Nordic's and is measured on a bench, not asserted here.
 */

#include "ant/ant_bridge.h"
#include "ant/ant_matter.h"

#include "app/matter_init.h"
#include "app/task_executor.h"

#include <platform/CHIPDeviceLayer.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cerrno>

/* ANTR_HOST_THREAD_PRIORITY, and ONLY that. The header carries no
 * `extern "C"` guard, so its function declarations get C++ linkage here; that
 * is harmless because none of them is called from this file, and taking the
 * number from the header rather than restating it is what keeps the assertion
 * below meaningful. */
#include "ant_radio.h"

/*
 * THE `app` LOG MODULE, REGISTERED HERE AND NOWHERE ELSE.
 *
 * Every vendored file in src/matter/core/ and every NCS sample-common source
 * does LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL), and upstream
 * registers it in matter_bridge's own main.cpp - a file
 * docs/matter-e3-vendoring.md deliberately did not vendor, because main() is
 * this package's business. This is the replacement for that one line. If a
 * second registration ever appears the link fails loudly, which is the good
 * case.
 */
LOG_MODULE_REGISTER(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::DeviceLayer;

namespace
{

/*
 * 2 KB, matching src/bridge_pump.c's pump. This thread runs PrepareServer(),
 * StartServer() and then blocks in DispatchNextTask() forever; the deep stacks
 * belong to the CHIP event loop (CONFIG_CHIP_TASK_STACK_SIZE, 9 216 B on this
 * part because of CRACEN) and to OpenThread, neither of which is this one.
 */
constexpr size_t kMatterAppStackSize = 2048;

/* Same priority as the bridge pump, and BUILD_ASSERTed below for the same
 * reason bridge_pump.c asserts its own: this thread must never be able to
 * delay an ANT receive window. */
constexpr int kMatterAppPriority = ANTR_HOST_THREAD_PRIORITY + 2;

static_assert(kMatterAppPriority > ANTR_HOST_THREAD_PRIORITY,
	      "The Matter application task must be strictly lower priority than the ANT host thread. "
	      "It is not the CHIP event loop - that is Nordic's thread at K_PRIO_PREEMPT(1) and is "
	      "trap 9's subject - but it is ours, and ours has no business preempting a receive window.");

K_THREAD_STACK_DEFINE(sMatterAppStack, kMatterAppStackSize);
k_thread sMatterAppThread;

void MatterAppThreadFn(void *, void *, void *)
{
	/*
	 * PrepareServer() SCHEDULES the initialisation onto the CHIP thread;
	 * StartServer() starts that thread and blocks until it has finished.
	 * mPostServerInitClbk therefore runs on the CHIP thread, after
	 * Server::Init(), which is the only legal moment to create a dynamic
	 * endpoint - see ant/ant_bridge.h.
	 */
	CHIP_ERROR err = Nrf::Matter::PrepareServer(
		Nrf::Matter::InitData{ .mPostServerInitClbk = [] { return RadiantMatter::PostServerInit(); } });

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter PrepareServer failed: %" CHIP_ERROR_FORMAT, err.Format());
		return;
	}

	err = Nrf::Matter::StartServer();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter StartServer failed: %" CHIP_ERROR_FORMAT, err.Format());
		return;
	}

	LOG_INF("Matter server started; ANT+ bridge is live");

	/*
	 * The APPLICATION task queue, not the CHIP event loop - the loop above
	 * started that on its own thread. Nothing in this product posts to it
	 * today (there are no buttons and CONFIG_NCS_SAMPLE_MATTER_LEDS is n),
	 * and it is still run rather than dropped: NCS's sample-common code -
	 * matter_event_handler.cpp among it - posts tasks here, and a queue
	 * nobody drains is a queue that silently fills.
	 */
	while (true) {
		Nrf::DispatchNextTask();
	}
}

} /* namespace */

extern "C" int ant_matter_start(void)
{
	k_tid_t tid = k_thread_create(&sMatterAppThread, sMatterAppStack, K_THREAD_STACK_SIZEOF(sMatterAppStack),
				      MatterAppThreadFn, nullptr, nullptr, nullptr, kMatterAppPriority, 0, K_NO_WAIT);

	if (tid == nullptr) {
		return -EAGAIN;
	}
	k_thread_name_set(tid, "matter_app");
	return 0;
}
