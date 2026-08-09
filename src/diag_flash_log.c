/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Persistent diagnostic log for a board with no debugger attached.
 *
 * The plan for this project assumed RTT as the log channel, but RTT needs a
 * J-Link and there is none on this machine — which left bring-up blind, with
 * an LED heartbeat as the only signal. This module gives the log somewhere to
 * go that survives a reset and can be read back over the one channel the board
 * always has: the Adafruit UF2 bootloader.
 *
 * How it works:
 *
 *   1. It registers as a Zephyr log backend, so every LOG_INF/LOG_ERR — from
 *      the application *and* from the USB stack itself — is formatted into a
 *      RAM buffer.
 *   2. A low-priority thread periodically copies that buffer into a reserved
 *      flash region, and diag_flash_log_flush() does the same on demand.
 *   3. Double-tapping RESET mounts FTHR840BOOT, whose CURRENT.UF2 is a readback
 *      of the application flash. scripts/read_flash_log.ps1 pulls the region
 *      out of it and prints the text.
 *
 * Two copies are written, because the bootloader's CURRENT.UF2 only spans as
 * far as the last programmed page: the primary slot sits at the top of the
 * code partition, the alternate just above a typical image, so whichever way
 * the bootloader decides the extent, at least one copy is inside it.
 *
 * Log text is captured in immediate mode (see diag.conf), so a message written
 * immediately before a hang still reaches the buffer, and the next periodic
 * flush commits it.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fatal.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include "diag_flash_log.h"

LOG_MODULE_REGISTER(diag_flash_log, LOG_LEVEL_INF);

/* Forward declaration - the fatal path needs the lock-free flush. */
static int diag_flush(bool take_mutex);

#define DIAG_PAGE_SIZE   4096U
#define DIAG_REGION_SIZE (CONFIG_ANT_DONGLE_FLASH_LOG_PAGES * DIAG_PAGE_SIZE)

/* 'A','D','L','G' little-endian — what read_flash_log.ps1 searches for. */
#define DIAG_MAGIC       0x474C4441U
#define DIAG_HDR_BYTES   16U
#define DIAG_TEXT_MAX    (DIAG_REGION_SIZE - DIAG_HDR_BYTES)

static const struct device *const flash_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

static const uint32_t diag_slots[] = {
	CONFIG_ANT_DONGLE_FLASH_LOG_OFFSET,
#if CONFIG_ANT_DONGLE_FLASH_LOG_OFFSET_ALT != 0
	CONFIG_ANT_DONGLE_FLASH_LOG_OFFSET_ALT,
#endif
};

/* Word-aligned: the nRF flash driver writes 32-bit words straight out of the
 * caller's buffer, so an unaligned source is rejected.
 */
static char diag_text[DIAG_TEXT_MAX] __aligned(4);
static size_t diag_len;
static uint32_t diag_dropped;
static uint32_t diag_seq;
static struct k_spinlock diag_lock;
static K_MUTEX_DEFINE(flush_mutex);

/* ── Log backend ───────────────────────────────────────────────────────────── */

static int diag_char_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);

	k_spinlock_key_t key = k_spin_lock(&diag_lock);
	size_t room = DIAG_TEXT_MAX - diag_len;
	size_t take = MIN(room, length);

	memcpy(&diag_text[diag_len], data, take);
	diag_len += take;
	diag_dropped += length - take;

	k_spin_unlock(&diag_lock, key);

	/* Claim the whole write either way; log_output has nowhere to put a
	 * short count and reporting one just stalls the formatter.
	 */
	return length;
}

static uint8_t diag_out_buf[64];
LOG_OUTPUT_DEFINE(diag_log_output, diag_char_out, diag_out_buf,
		  sizeof(diag_out_buf));

static void diag_backend_init(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	log_output_timestamp_freq_set(sys_clock_hw_cycles_per_sec());
}

static void diag_backend_process(const struct log_backend *const backend,
				 union log_msg_generic *msg)
{
	ARG_UNUSED(backend);
	log_output_msg_process(&diag_log_output, &msg->log,
			       LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_TIMESTAMP);
}

static void diag_backend_dropped(const struct log_backend *const backend,
				 uint32_t cnt)
{
	ARG_UNUSED(backend);

	k_spinlock_key_t key = k_spin_lock(&diag_lock);

	diag_dropped += cnt;
	k_spin_unlock(&diag_lock, key);
}

static void diag_backend_panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	/* A fatal error is exactly the case this module exists for, so commit
	 * what we have. Lock-free: panic can be raised from an interrupt.
	 */
	(void)diag_flush(false);
}

static int diag_backend_format_set(const struct log_backend *const backend,
				   uint32_t log_type)
{
	ARG_UNUSED(backend);
	ARG_UNUSED(log_type);
	return 0;
}

static const struct log_backend_api diag_backend_api = {
	.process = diag_backend_process,
	.init = diag_backend_init,
	.panic = diag_backend_panic,
	.dropped = diag_backend_dropped,
	.format_set = diag_backend_format_set,
};

LOG_BACKEND_DEFINE(diag_flash_log_backend, diag_backend_api, true);

/* ── Flash writer ──────────────────────────────────────────────────────────── */

/*
 * The primary slot is chosen to sit above the image, but a build that grows
 * past it would otherwise silently erase its own code. _flash_used is the
 * linker's view of the image size, so the check costs nothing at runtime.
 */
static bool slot_collides_with_image(uint32_t off)
{
	uint32_t image_end = CONFIG_FLASH_LOAD_OFFSET +
			     (uint32_t)(uintptr_t)_flash_used;

	return off < ROUND_UP(image_end, DIAG_PAGE_SIZE);
}

/*
 * take_mutex is false only on the fatal-error path, which can run in an
 * interrupt context where k_mutex_lock() is not allowed. Nothing else is
 * running by then, so skipping it is safe there and nowhere else.
 */
static int diag_flush(bool take_mutex)
{
	uint8_t hdr[DIAG_HDR_BYTES] __aligned(4);
	size_t len;
	size_t write_len;
	int err = 0;

	if (!device_is_ready(flash_dev)) {
		return -ENODEV;
	}

	if (take_mutex) {
		k_mutex_lock(&flush_mutex, K_FOREVER);
	}

	k_spinlock_key_t key = k_spin_lock(&diag_lock);

	len = diag_len;
	sys_put_le32(DIAG_MAGIC, &hdr[0]);
	sys_put_le32((uint32_t)len, &hdr[4]);
	sys_put_le32(++diag_seq, &hdr[8]);
	sys_put_le32(diag_dropped, &hdr[12]);
	k_spin_unlock(&diag_lock, key);

	/* Both the header and the text are written straight out of their own
	 * storage — no staging copy — so the region is committed in two writes
	 * and costs no extra RAM. Flash wants 4-byte granularity; diag_text is
	 * a multiple of 4 long, so rounding up can never run past its end.
	 */
	write_len = ROUND_UP(len, 4);

	for (size_t i = 0; i < ARRAY_SIZE(diag_slots); i++) {
		uint32_t off = diag_slots[i];
		int ret;

		if (slot_collides_with_image(off)) {
			continue;
		}

		ret = flash_erase(flash_dev, off, DIAG_REGION_SIZE);
		if (ret == 0) {
			ret = flash_write(flash_dev, off, hdr, sizeof(hdr));
		}
		if (ret == 0 && write_len != 0U) {
			ret = flash_write(flash_dev, off + sizeof(hdr),
					  diag_text, write_len);
		}

		if (ret != 0 && err == 0) {
			err = ret;
		}
	}

	if (take_mutex) {
		k_mutex_unlock(&flush_mutex);
	}

	return err;
}

int diag_flash_log_flush(void)
{
	return diag_flush(true);
}

/*
 * Overrides the weak default, which halts the CPU without a trace. A halt is
 * indistinguishable from a hang from the outside - no USB, no LED, and no
 * periodic flush either - so committing the log here is what makes an early
 * fault visible at all.
 *
 * Overriding it also takes CONFIG_RESET_ON_FATAL_ERROR out of the picture,
 * since that option only changes what the *default* handler does. Honour it
 * explicitly instead, after the flush, so a diag build behaves the same way a
 * release build does rather than quietly still going dark. The log survives the
 * reboot - it is already in flash by this point, and read back through the
 * bootloader's CURRENT.UF2, which does not care how the CPU got there.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	LOG_ERR("FATAL: reason %u", reason);
	if (esf != NULL) {
		LOG_ERR("FATAL: pc 0x%08x lr 0x%08x xpsr 0x%08x",
			esf->basic.pc, esf->basic.lr, esf->basic.xpsr);
	}

	(void)diag_flush(false);

#if defined(CONFIG_RESET_ON_FATAL_ERROR)
	sys_reboot(SYS_REBOOT_COLD);
#else
	k_fatal_halt(reason);
#endif
}

/* ── Periodic flush ────────────────────────────────────────────────────────── */

/*
 * The leading zero delay commits a page as soon as the kernel starts
 * scheduling. That is the mechanism's own self-test: if no header ever shows
 * up in the readback, the flash write is what is broken, not whatever the log
 * was meant to catch. After that it front-loads and backs off, because
 * boot-time failures are the interesting ones and there is no point burning
 * erase cycles once the board has been idle for a minute.
 */
static void diag_flusher(void *p1, void *p2, void *p3)
{
	static const int delays_ms[] = { 0, 500, 1500, 3000, 5000, 10000, 30000 };

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (size_t i = 0; i < ARRAY_SIZE(delays_ms); i++) {
		k_sleep(K_MSEC(delays_ms[i]));
		(void)diag_flash_log_flush();
	}
}

/* 2 KB rather than 1: in immediate mode any log call made by the flash driver
 * is formatted on whichever stack triggered it, which is this one.
 */
K_THREAD_DEFINE(diag_flusher_thread, 2048, diag_flusher, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
