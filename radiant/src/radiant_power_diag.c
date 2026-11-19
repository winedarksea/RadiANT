/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_power_diag.c - the software power proxy. See
 * radiant/include/radiant/radiant_power_diag.h for the layout, the seq
 * protocol and what each number means. Built only with
 * CONFIG_RADIANT_POWER_DIAG.
 *
 * Provenance: clean-room. See the header.
 *
 * Contexts. Three kinds of writer feed the private accumulators below, each
 * the only writer of its own fields: the scheduler (radio counters, under its
 * own serialization), the backend's HFXO hook (irq_lock, rare) and the idle
 * hooks (idle thread and first ISR after a wake, irq_lock). The work item is
 * the only writer of the published struct, and reads the u32 radio counters
 * without a lock: each is naturally aligned and read whole, and a counter
 * read a moment early is a counter read a moment early. The HFXO and idle
 * state are 64-bit, so they are read under the irq_lock their hooks take.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_RADIANT_BACKEND_NRF)
#include <nrfx.h>
#endif

#include <radiant/radiant_power_diag.h>

/* The ti_* fields: the RF driver's counters (radiant_ti_rf_hwattrs.c, which
 * CONFIG_RADIANT_POWER_DIAG selects on cc26xx) and the backend's fold. */
#if defined(CONFIG_RADIANT_BACKEND_CC26XX)
#define PD_TI 1
#include "radiant_rat_fold.h"
#include "radiant_ti_rf_hwattrs.h"
#else
#define PD_TI 0
#endif

LOG_MODULE_REGISTER(radiant_power, CONFIG_RADIANT_LOG_LEVEL);

/* Which fields this build can fill. Fixed at build time. */
#if defined(CONFIG_RADIANT_BACKEND_NRF)
#define PD_HFXO_MEASURED 1
#else
#define PD_HFXO_MEASURED 0
#endif

#if defined(CONFIG_RADIANT_BACKEND_NRF) && defined(POWER_CONSTLATSTAT_STATUS_Msk)
#define PD_CONSTLAT_HW 1
#else
#define PD_CONSTLAT_HW 0
#endif

/* The log line needs both its period and a logging subsystem. */
#if (CONFIG_RADIANT_POWER_DIAG_LOG_S > 0) && defined(CONFIG_LOG)
#define PD_LOG 1
#else
#define PD_LOG 0
#endif

#define PD_INIT_HFXO  (PD_HFXO_MEASURED ? 0u : RADIANT_POWER_DIAG_NOT_MEASURED)
/* The idle hooks measure both idle entries and CPU time. */
#define PD_INIT_IDLE  (IS_ENABLED(CONFIG_RADIANT_POWER_DIAG_IDLE) ? 0u \
		       : RADIANT_POWER_DIAG_NOT_MEASURED)
#define PD_INIT_TI    (PD_TI ? 0u : RADIANT_POWER_DIAG_NOT_MEASURED)

#define PD_FLAGS_STATIC                                                        \
	((PD_HFXO_MEASURED ? RADIANT_POWER_DIAG_F_HFXO_MEASURED : 0u) |        \
	 (PD_CONSTLAT_HW ? RADIANT_POWER_DIAG_F_CONSTLAT_HW_VALID : 0u) |      \
	 (IS_ENABLED(CONFIG_RADIANT_POWER_DIAG_IDLE)                           \
		  ? (RADIANT_POWER_DIAG_F_CPU_LOAD | RADIANT_POWER_DIAG_F_IDLE)  \
		  : 0u) |                                                \
	 (PD_TI ? RADIANT_POWER_DIAG_F_TI : 0u))

/*
 * The published snapshot. Valid header and sentinels from reset, so an SWD
 * reader can check magic before the first snapshot; seq 0 says no snapshot
 * yet. __used: nothing in C may read it, and the symbol is the interface.
 */
__used struct radiant_power_diag radiant_power_diag = {
	.magic = RADIANT_POWER_DIAG_MAGIC,
	.version = RADIANT_POWER_DIAG_VERSION,
	.size = RADIANT_POWER_DIAG_SIZE,
	.seq = 0u,
	.hfxo_req_us = PD_INIT_HFXO,
	.hfxo_requests = PD_INIT_HFXO,
	.hfxo_releases = PD_INIT_HFXO,
	.cpu_active_us = PD_INIT_IDLE,
	.cpu_load_permille = PD_INIT_IDLE,
	.idle_entries = PD_INIT_IDLE,
	.flags = PD_FLAGS_STATIC,
	.constlat_stat = RADIANT_POWER_DIAG_NOT_MEASURED,
	.ti_rf_core_on_us = PD_INIT_TI,
	.ti_rf_powerups = PD_INIT_TI,
	/* Until the RF core has powered up once, on every backend. */
	.ti_rf_pu_dur_us = RADIANT_POWER_DIAG_NOT_MEASURED,
	.ti_fs_errors = PD_INIT_TI,
	.ti_fold_backsteps = PD_INIT_TI,
};

static struct {
	/* Scheduler-serialized. */
	uint32_t tx_us;
	uint32_t rx_us;
	uint32_t tx_ops;
	uint32_t rx_ops;
	/* irq_lock. */
	struct radiant_power_diag_hfxo hfxo;
	/* irq_lock: idle thread and the first ISR after a wake. */
	struct radiant_power_diag_idle idle;
	uint32_t idle_entries;
	/* Backend init thread, once. */
	bool constlat_req;
	/* cc26xx fold, under the backend's irq_lock. */
	uint32_t fold_backsteps;
	uint32_t fold_back_max;  /* ticks, the largest back-step */
	uint32_t fold_wraps;

	/* Work item only (and pd_init()). */
	/* fold_back_max and fold_wraps as of the snapshot that took
	 * ti_fold_backsteps, so one log line pairs bks with its own bks_max. */
	uint32_t snap_back_max;
	uint32_t snap_wraps;
	uint32_t last_cyc;
	uint64_t last_idle_cyc;
	uint64_t active_cyc;
	uint32_t snaps_since_log;
	bool     have_log;
	struct radiant_power_diag last_log;
} pd;

/* Microseconds on the system timer: independent of the HFXO on every nRF
 * (RTC / GRTC on the LF clock), so it times the crystal's own request. */
static uint64_t pd_now_us(void)
{
	return k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}

void radiant_power_diag_hfxo(bool on)
{
	unsigned int key = irq_lock();

	radiant_power_diag_hfxo_note(&pd.hfxo, on, pd_now_us());
	irq_unlock(key);
}

void radiant_power_diag_constlat_requested(void)
{
	pd.constlat_req = true;
}

void radiant_power_diag_radio(bool tx, uint32_t on_us)
{
	if (tx) {
		pd.tx_us += on_us;
		pd.tx_ops++;
	} else {
		pd.rx_us += on_us;
		pd.rx_ops++;
	}
}

#if PD_TI
void radiant_power_diag_rat_backward(uint32_t last, uint32_t now)
{
	switch (radiant_rat_step_classify(last, now)) {
	case RADIANT_RAT_STEP_WRAP:
		pd.fold_wraps++;
		break;
	case RADIANT_RAT_STEP_BACK: {
		uint32_t ticks = radiant_rat_backstep_ticks(last, now);

		pd.fold_backsteps++;
		if (ticks > pd.fold_back_max) {
			pd.fold_back_max = ticks;
		}
		break;
	}
	default:
		break;
	}
}
#endif

#if defined(CONFIG_RADIANT_POWER_DIAG_IDLE)
/*
 * Zephyr's TRACING_USER hooks (subsys/tracing/user/tracing_user.c defines
 * both __weak). The idle thread calls sys_trace_idle_user() with interrupts
 * locked, just before the CPU's sleep instruction: count the entry and open an
 * idle interval. TRACING_USER has no user hook for the idle exit, so the
 * interval is closed by sys_trace_isr_enter_user(), which every traced
 * interrupt runs on entry (CONFIG_TRACING_ISR): the first one after the wake
 * is the wake. Arm's arch_cpu_idle() unmasks right after its own (non-user)
 * idle-exit trace, so the gap between the wake and that close is a few
 * instructions. Nested and later interrupts find the interval closed and only
 * pay one load. An application that defines either hook itself turns
 * CONFIG_RADIANT_POWER_DIAG_IDLE off.
 */
void sys_trace_idle_user(void)
{
	uint32_t     now = k_cycle_get_32();
	unsigned int key = irq_lock();

	pd.idle_entries++;
	radiant_power_diag_idle_open(&pd.idle, now);
	irq_unlock(key);
}

void sys_trace_isr_enter_user(void)
{
	unsigned int key;

	if (!pd.idle.open) {
		return;
	}
	key = irq_lock();
	radiant_power_diag_idle_close(&pd.idle, k_cycle_get_32());
	irq_unlock(key);
}
#endif

static void pd_snapshot(void)
{
	struct radiant_power_diag next = radiant_power_diag;
	uint64_t     now_us = pd_now_us();
	uint32_t     flags = PD_FLAGS_STATIC;
	unsigned int key;

	next.uptime_ms = (uint32_t)(now_us / 1000u);

#if PD_HFXO_MEASURED
	{
		struct radiant_power_diag_hfxo h;

		key = irq_lock();
		h = pd.hfxo;
		irq_unlock(key);
		next.hfxo_req_us = (uint32_t)radiant_power_diag_hfxo_held(&h, now_us);
		next.hfxo_requests = h.requests;
		next.hfxo_releases = h.releases;
		if (h.on) {
			flags |= RADIANT_POWER_DIAG_F_HFXO_HELD;
		}
	}
#else
	ARG_UNUSED(key);
#endif

	next.radio_tx_us = pd.tx_us;
	next.radio_rx_us = pd.rx_us;
	next.radio_tx_ops = pd.tx_ops;
	next.radio_rx_ops = pd.rx_ops;

#if defined(CONFIG_RADIANT_POWER_DIAG_IDLE)
	{
		/* The span since the previous snapshot on the same cycle clock the
		 * idle hooks use, minus the idle in it. Summed in cycles and
		 * converted once, so no rounding accumulates. An interval still
		 * open here is stale (its wake was an untraced interrupt): this is
		 * not the idle thread. */
		uint32_t now_cyc = k_cycle_get_32();
		uint32_t span = radiant_power_diag_delta(now_cyc, pd.last_cyc);
		uint32_t active;
		uint64_t idle;

		key = irq_lock();
		radiant_power_diag_idle_close(&pd.idle, now_cyc);
		idle = pd.idle.idle_cyc;
		next.idle_entries = pd.idle_entries;
		irq_unlock(key);

		active = radiant_power_diag_active_cyc(span, idle - pd.last_idle_cyc);
		pd.active_cyc += active;
		pd.last_cyc = now_cyc;
		pd.last_idle_cyc = idle;
		next.cpu_active_us = (uint32_t)k_cyc_to_us_floor64(pd.active_cyc);
		next.cpu_load_permille = radiant_power_diag_duty_permille(active, span);
	}
#endif

	if (pd.constlat_req) {
		flags |= RADIANT_POWER_DIAG_F_CONSTLAT_REQ;
	}
#if PD_CONSTLAT_HW
	next.constlat_stat = NRF_POWER->CONSTLATSTAT;
#endif
#if PD_TI
	{
		struct radiant_ti_rf_diag t;

		radiant_ti_rf_diag_read(&t);
		next.ti_rf_core_on_us = t.on_us;
		next.ti_rf_powerups = t.powerups;
		next.ti_rf_pu_dur_us = t.pu_dur_us;
		next.ti_fs_errors = t.fs_errors;
		/* One irq_lock (the fold's writer runs under the backend's) for
		 * all three, so the log never shows bks=0 with bks_max>0. */
		key = irq_lock();
		next.ti_fold_backsteps = pd.fold_backsteps;
		pd.snap_back_max = pd.fold_back_max;
		pd.snap_wraps = pd.fold_wraps;
		irq_unlock(key);
	}
#endif
	next.flags = flags;

	radiant_power_diag_publish(&radiant_power_diag, &next);
}

#if PD_LOG
/* -1 for a field this build does not measure, else the value. */
static int pd_or_unmeasured(bool measured, uint32_t v)
{
	return measured ? (int)MIN(v, (uint32_t)INT32_MAX) : -1;
}

static void pd_log(const struct radiant_power_diag *now,
		   const struct radiant_power_diag *then)
{
	uint32_t dt_ms = radiant_power_diag_delta(now->uptime_ms, then->uptime_ms);
	uint32_t dt_us = (uint32_t)MIN((uint64_t)dt_ms * 1000u, (uint64_t)UINT32_MAX);
	bool     cpu = (now->flags & RADIANT_POWER_DIAG_F_CPU_LOAD) != 0u;
	bool     idle = (now->flags & RADIANT_POWER_DIAG_F_IDLE) != 0u;
	bool     hfxo = (now->flags & RADIANT_POWER_DIAG_F_HFXO_MEASURED) != 0u;
	bool     cl_hw = (now->flags & RADIANT_POWER_DIAG_F_CONSTLAT_HW_VALID) != 0u;

	int      hfxo_pm = pd_or_unmeasured(hfxo, radiant_power_diag_duty_permille(
		radiant_power_diag_delta(now->hfxo_req_us, then->hfxo_req_us), dt_us));
	uint32_t tx_us = radiant_power_diag_delta(now->radio_tx_us, then->radio_tx_us);
	uint32_t tx_ops = radiant_power_diag_delta(now->radio_tx_ops, then->radio_tx_ops);
	uint32_t rx_us = radiant_power_diag_delta(now->radio_rx_us, then->radio_rx_us);
	uint32_t rx_ops = radiant_power_diag_delta(now->radio_rx_ops, then->radio_rx_ops);
	int      cpu_pm = pd_or_unmeasured(cpu, radiant_power_diag_duty_permille(
		radiant_power_diag_delta(now->cpu_active_us, then->cpu_active_us), dt_us));
	int      idle_x10 = pd_or_unmeasured(idle, radiant_power_diag_rate_x10(
		radiant_power_diag_delta(now->idle_entries, then->idle_entries), dt_ms));
	uint32_t cl_req = (now->flags & RADIANT_POWER_DIAG_F_CONSTLAT_REQ) != 0u ? 1u : 0u;
	int      cl_stat = pd_or_unmeasured(cl_hw, now->constlat_stat & 1u);

	/* key=value, integers only: one line a parser and a person both read.
	 * _pm is per-mille of dt_ms, _x10 is tenths per second, -1 is not
	 * measured on this build. A cc26xx build appends its ti_* fields to the
	 * same line. */
#if PD_TI
	/*
	 * The cc26xx fields. rfon_pm and rfpu_x10 are over dt_ms like the rest;
	 * every other field is cumulative since boot, because each is a rare
	 * event whose total is the answer: pudur_us is the RF driver's
	 * nPowerUpDuration now (-1 before the first power-up), fsx the synth
	 * errors, bks the fold's mistaken wraps and bks_max the largest of them
	 * in radio-timer ticks (4 per us), wraps the fold's genuine wraps
	 * (one per 1074 s of radio timer), cold the first commands after a
	 * power-up handed to the RF core at or after their start time, txl the
	 * transmits handed over late (cold or warm) and txl_max_us the latest.
	 */
	struct radiant_ti_rf_diag t;

	radiant_ti_rf_diag_read(&t);
	LOG_INF("power dt_ms=%u hfxo_pm=%d tx_us=%u tx_ops=%u rx_us=%u rx_ops=%u "
		"cpu_pm=%d idle_x10=%d constlat=%u/%d "
		"rfon_pm=%u rfpu_x10=%u pudur_us=%d fsx=%u bks=%u bks_max=%u "
		"wraps=%u cold=%u txl=%u txl_max_us=%u",
		dt_ms, hfxo_pm, tx_us, tx_ops, rx_us, rx_ops, cpu_pm, idle_x10,
		cl_req, cl_stat,
		radiant_power_diag_duty_permille(
			radiant_power_diag_delta(now->ti_rf_core_on_us,
						 then->ti_rf_core_on_us), dt_us),
		radiant_power_diag_rate_x10(
			radiant_power_diag_delta(now->ti_rf_powerups,
						 then->ti_rf_powerups), dt_ms),
		pd_or_unmeasured(now->ti_rf_pu_dur_us != RADIANT_POWER_DIAG_NOT_MEASURED,
				 now->ti_rf_pu_dur_us),
		now->ti_fs_errors, now->ti_fold_backsteps, pd.snap_back_max,
		pd.snap_wraps, t.cold_late, t.tx_late, t.tx_late_max_us);
#else
	LOG_INF("power dt_ms=%u hfxo_pm=%d tx_us=%u tx_ops=%u rx_us=%u rx_ops=%u "
		"cpu_pm=%d idle_x10=%d constlat=%u/%d",
		dt_ms, hfxo_pm, tx_us, tx_ops, rx_us, rx_ops, cpu_pm, idle_x10,
		cl_req, cl_stat);
#endif
}
#endif

static void pd_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(pd_work, pd_work_fn);

static void pd_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	pd_snapshot();

#if PD_LOG
	/* The work queue is the published struct's only writer, so it may read
	 * it here without the seq protocol. */
	if (!pd.have_log) {
		pd.last_log = radiant_power_diag;
		pd.have_log = true;
		pd.snaps_since_log = 0u;
	} else if (++pd.snaps_since_log >=
		   (CONFIG_RADIANT_POWER_DIAG_LOG_S * 1000u) /
			   RADIANT_POWER_DIAG_SNAPSHOT_MS) {
		pd_log(&radiant_power_diag, &pd.last_log);
		pd.last_log = radiant_power_diag;
		pd.snaps_since_log = 0u;
	}
#endif

	(void)k_work_schedule(&pd_work, K_MSEC(RADIANT_POWER_DIAG_SNAPSHOT_MS));
}

static int pd_init(void)
{
	/* The first CPU window starts here, not at a cycle count of 0: a GRTC
	 * can carry its count across a soft reset. */
	pd.last_cyc = k_cycle_get_32();
	(void)k_work_schedule(&pd_work, K_MSEC(RADIANT_POWER_DIAG_SNAPSHOT_MS));
	return 0;
}

SYS_INIT(pd_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
