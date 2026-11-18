/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_power_diag.h - a software power proxy for boards with no current
 * meter (CONFIG_RADIANT_POWER_DIAG).
 *
 * Provenance: clean-room. Written from this project's own source; the
 * register named below is from the nRF54L15 MDK header.
 *
 * WHAT THIS IS. One small struct, the global symbol `radiant_power_diag`,
 * holding free-running counters of the things that set a battery node's
 * current: how long the stack has held its HFXO request, how long the radio
 * was on (transmit and receive apart), how busy the CPU was and how often it
 * went idle. Nothing reaches a host: it is not a 0xF7 page and no message
 * carries it. It is read two ways:
 *
 *   - over SWD while the target runs: look the symbol up in the ELF
 *     (arm-zephyr-eabi-nm zephyr.elf | grep radiant_power_diag) and read
 *     RADIANT_POWER_DIAG_SIZE bytes there with mem32 (scripts/jlink_peek.ps1);
 *   - as a log line every CONFIG_RADIANT_POWER_DIAG_LOG_S seconds, with the
 *     deltas since the previous line already worked out.
 *
 * Every number is a PROXY. Multiply by a datasheet current to get an
 * estimate; nothing here measures current.
 *
 * LAYOUT (little-endian, every field a u32, no padding; version 1):
 *
 *   off  field              meaning
 *   0x00 magic              RADIANT_POWER_DIAG_MAGIC (0x52504431, "RPD1")
 *   0x04 version            RADIANT_POWER_DIAG_VERSION (1)
 *   0x08 size               sizeof(struct radiant_power_diag) (0x58)
 *   0x0C seq                snapshot sequence, see THE SEQ PROTOCOL
 *   0x10 uptime_ms          kernel uptime at the snapshot (system timer)
 *   0x14 hfxo_req_us        cumulative us the backend's HFXO request was held
 *   0x18 hfxo_requests      off->on transitions of that request
 *   0x1C hfxo_releases      on->off transitions of that request
 *   0x20 radio_tx_us        cumulative transmit on-time, us
 *   0x24 radio_rx_us        cumulative receive on-time (energy detect too), us
 *   0x28 radio_tx_ops       transmit operations that ran
 *   0x2C radio_rx_ops       receive / energy-detect operations that ran
 *   0x30 cpu_active_us      cumulative CPU non-idle time, us (..._IDLE)
 *   0x34 cpu_load_permille  CPU load over the last snapshot interval, 0..1000
 *   0x38 idle_entries       idle-thread entries (CONFIG_RADIANT_POWER_DIAG_IDLE)
 *   0x3C flags              RADIANT_POWER_DIAG_F_* below
 *   0x40 constlat_stat      nRF54L POWER.CONSTLATSTAT read at the snapshot
 *   0x44 ti_rf_core_on_us   cc26xx: cumulative us the RF core was powered
 *   0x48 ti_rf_powerups     cc26xx: RF core power-ups
 *   0x4C ti_rf_pu_dur_us    cc26xx: the RF driver's nPowerUpDuration, us (a
 *                           gauge, not cumulative)
 *   0x50 ti_fs_errors       cc26xx: synthesiser-programming errors (CMD_FS)
 *   0x54 ti_fold_backsteps  cc26xx: small backward steps of the 32-bit radio
 *                           timer that the 64-bit fold took for a wrap
 *
 * A field this build cannot measure reads RADIANT_POWER_DIAG_NOT_MEASURED
 * (0xFFFFFFFF) for the whole run: cpu_* and idle_entries without
 * CONFIG_RADIANT_POWER_DIAG_IDLE, constlat_stat on a
 * part with no CONSTLATSTAT register (every nRF52/nRF53, and cc26xx), the
 * hfxo_* trio on a backend that does not report its clock request (cc26xx,
 * null), and all five ti_* fields on any backend but cc26xx (flag F_TI says
 * they are measured). ti_rf_pu_dur_us also reads NOT_MEASURED on cc26xx until
 * the RF core has powered up once.
 *
 * WRAP. The cumulative fields are free-running u32s: a reader takes
 * differences modulo 2^32 (radiant_power_diag_delta()). The us counters wrap
 * after 71.6 minutes of accumulated time, so two reads must be less than
 * that apart; CONFIG_RADIANT_POWER_DIAG_LOG_S is capped at 3600 for that
 * reason. uptime_ms wraps after 49.7 days.
 *
 * THE SEQ PROTOCOL. The struct has exactly one writer: a work item on the
 * system work queue that publishes a fresh snapshot every
 * RADIANT_POWER_DIAG_SNAPSHOT_MS. It sets seq odd, fences, writes every other
 * field, fences, and sets seq even (seq += 2 per snapshot). A reader:
 *
 *   1. reads seq (s1); if s1 is odd, a snapshot is being written - retry;
 *   2. reads the fields;
 *   3. reads seq again (s2); the read is consistent only if s2 == s1.
 *
 * seq == 0 means no snapshot has been published yet (magic, version, size,
 * flags and the NOT_MEASURED sentinels are valid from reset; the counters
 * are still 0 and constlat_stat still reads NOT_MEASURED). An SWD block read takes microseconds and a snapshot
 * comes once a second, so a retry is rare; a reader that sees s2 != s1 twice
 * running should suspect it is reading the wrong address.
 *
 * WHAT EACH PROXY IS, EXACTLY.
 *
 *   hfxo_req_us is timed on the system timer (the LF clock), not on the
 *   radio TIMER, so it stays correct across an HFXO release. It is the
 *   stack's own request: on an nRF52840 with USB, or under MPSL, something
 *   else may hold the crystal too, which this cannot see. Today the nRF
 *   backend requests the HFXO once at init and never releases it, so it
 *   reads ~uptime: that is intended, and is the baseline the low-power work
 *   (RADIANT_NRF_LOW_POWER) measures itself against.
 *
 *   radio_*_us run from the operation's armed open to the instant the
 *   scheduler hears it ended, on radiant_radio_now(). A transmit's open is
 *   its sync-word time, so the ramp-up and preamble before it are not
 *   counted and the interrupt latency after its END is. A receive window
 *   counts from its open whether or not a frame came. An operation denied by
 *   an arbiter (MPSL) is not counted at all, and neither is one that ends
 *   (aborted, failed) before its open. Backend-neutral: the same code counts
 *   on nRF and cc26xx.
 *
 *   cpu_* are this file's own idle accumulator on Zephyr's cycle clock
 *   (k_cycle_get_32(): 32768 Hz RTC on nRF52/53 and cc26xx, 1 MHz GRTC on
 *   nRF54L), through two TRACING_USER hooks: sys_trace_idle_user() opens an
 *   idle interval as the idle thread goes to sleep, and
 *   sys_trace_isr_enter_user() closes it in the first interrupt after the
 *   wake (later, nested ones find it closed). Active time is the snapshot
 *   interval minus the idle in it, accumulated in cycles and converted to us
 *   once, so there is no per-window rounding (Zephyr's cpu_load_get() floors
 *   each window to whole per-mille, which is why it is not used). Resolution
 *   is one cycle per idle interval. An interrupt that does not run Zephyr's
 *   tracing (a zero-latency or bare direct ISR) does not close the interval;
 *   the idle thread's next entry, or the next snapshot, closes it instead,
 *   and that ISR's run time is counted as idle. cpu_load_permille is the
 *   last snapshot interval's load, floored; use cpu_active_us for sums.
 *
 *   idle_entries counts sys_trace_idle_user() calls: one per entry into the
 *   CPU's sleep instruction from the idle thread. Its rate is the wake rate.
 *
 *   flags bit F_CONSTLAT_REQ: the nRF backend requested CONSTLAT at init
 *   (nRF54L errata 20): a raw trigger on a direct build, an nrfx reference on
 *   an MPSL build. F_CONSTLAT_HW_VALID + constlat_stat bit 0: what the
 *   hardware says now. The two disagreeing means something triggered LOWPWR
 *   over the request (MPSL did, before the reference count).
 *
 *   ti_rf_* (cc26xx only) come from the TI RF driver's global callback in
 *   src/radiant_ti_rf_hwattrs.c, which defines RFCC26XX_hwAttrs for this
 *   build; that file's header says exactly what each includes. In short:
 *   ti_rf_core_on_us runs from the setup command of each power-up to the
 *   power-down, timed on the AON RTC (it excludes the wake before setup,
 *   which ti_rf_powerups x ti_rf_pu_dur_us bounds); ti_rf_powerups counts
 *   power-ups, not reconfigurations. If the driver powers the core down
 *   between distant radio slots, ti_rf_powerups grows at about the slot
 *   rate; if it keeps the core on, ti_rf_core_on_us reads ~uptime.
 *
 *   ti_fs_errors counts RF_ERROR_CMDFS_SYNTH_PROG through the backend's
 *   pErrCb (the driver's default callback ignores it).
 *
 *   ti_fold_backsteps counts reads of the 32-bit radio timer below the
 *   previous one by less than 2^31 ticks - not a wrap - which the
 *   backend's fold nevertheless treated as one, adding 2^32 ticks (1074 s)
 *   to the clock (src/radiant_rat_fold.h). Measurement only: the fold is
 *   unchanged. Nonzero means that clock jump happened. Two blind spots, both
 *   on the log line (rat_fold.h has the detail): a back-step that straddles
 *   the counter's wrap point is not counted here but shows as an extra
 *   genuine wrap, so two `wraps` far less than 1074 s apart are a mis-fold
 *   too; and a largest back-step (bks_max) well above ~130 ticks is not the
 *   RTC round-up but a switch of time base (radiant_radio_disable()/enable()
 *   moving between the RAT and the uptime-derived clock can step back
 *   thousands of ticks).
 *
 * ZERO COST WHEN OFF. Without CONFIG_RADIANT_POWER_DIAG the hook functions
 * below are empty static inlines, radiant_power_diag.c is not built and no
 * symbol is added. The arithmetic helpers are static inlines with no state,
 * defined in every build so that the unit tests reach them. The stack's own
 * callers (radiant_sched.c, radiant_radio_nrf.c) include this header only
 * when the option is on: even unused inline declarations renumber GCC's
 * internal switch tables (CSWTCH.n), and the zero-cost symbol comparison
 * reports the rename.
 */

#ifndef RADIANT_POWER_DIAG_H_
#define RADIANT_POWER_DIAG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIANT_POWER_DIAG_MAGIC        0x52504431u /* "RPD1" */
#define RADIANT_POWER_DIAG_VERSION      1u
#define RADIANT_POWER_DIAG_NOT_MEASURED 0xFFFFFFFFu

/* How often the work item publishes a snapshot. Fixed: a J-Link reader needs
 * fresh values whether or not the log line is on. One wake a second is noise
 * beside a 4 Hz radio and the API thread's own 20 Hz poll. */
#define RADIANT_POWER_DIAG_SNAPSHOT_MS  1000u

/* flags (offset 0x3C) */
#define RADIANT_POWER_DIAG_F_HFXO_HELD       (1u << 0) /* request held now */
#define RADIANT_POWER_DIAG_F_HFXO_MEASURED   (1u << 1) /* backend reports it */
#define RADIANT_POWER_DIAG_F_CONSTLAT_REQ    (1u << 2) /* backend triggered it */
#define RADIANT_POWER_DIAG_F_CONSTLAT_HW_VALID (1u << 3) /* constlat_stat read */
#define RADIANT_POWER_DIAG_F_CPU_LOAD        (1u << 4) /* cpu_* measured (..._IDLE) */
#define RADIANT_POWER_DIAG_F_IDLE            (1u << 5) /* idle_entries measured */
#define RADIANT_POWER_DIAG_F_TI              (1u << 6) /* ti_* measured (cc26xx) */

struct radiant_power_diag {
	uint32_t magic;             /* 0x00 */
	uint32_t version;           /* 0x04 */
	uint32_t size;              /* 0x08 */
	uint32_t seq;               /* 0x0C */
	uint32_t uptime_ms;         /* 0x10 */
	uint32_t hfxo_req_us;       /* 0x14 */
	uint32_t hfxo_requests;     /* 0x18 */
	uint32_t hfxo_releases;     /* 0x1C */
	uint32_t radio_tx_us;       /* 0x20 */
	uint32_t radio_rx_us;       /* 0x24 */
	uint32_t radio_tx_ops;      /* 0x28 */
	uint32_t radio_rx_ops;      /* 0x2C */
	uint32_t cpu_active_us;     /* 0x30 */
	uint32_t cpu_load_permille; /* 0x34 */
	uint32_t idle_entries;      /* 0x38 */
	uint32_t flags;             /* 0x3C */
	uint32_t constlat_stat;     /* 0x40 */
	uint32_t ti_rf_core_on_us;  /* 0x44 */
	uint32_t ti_rf_powerups;    /* 0x48 */
	uint32_t ti_rf_pu_dur_us;   /* 0x4C */
	uint32_t ti_fs_errors;      /* 0x50 */
	uint32_t ti_fold_backsteps; /* 0x54 */
};

#define RADIANT_POWER_DIAG_SIZE 0x58u

/* The offsets are the contract with scripts that read raw memory. */
#ifndef __cplusplus
_Static_assert(sizeof(struct radiant_power_diag) == RADIANT_POWER_DIAG_SIZE,
	       "radiant_power_diag layout changed: update the table above");
_Static_assert(offsetof(struct radiant_power_diag, seq) == 0x0Cu, "seq");
_Static_assert(offsetof(struct radiant_power_diag, hfxo_req_us) == 0x14u,
	       "hfxo_req_us");
_Static_assert(offsetof(struct radiant_power_diag, radio_tx_us) == 0x20u,
	       "radio_tx_us");
_Static_assert(offsetof(struct radiant_power_diag, cpu_active_us) == 0x30u,
	       "cpu_active_us");
_Static_assert(offsetof(struct radiant_power_diag, flags) == 0x3Cu, "flags");
_Static_assert(offsetof(struct radiant_power_diag, ti_rf_core_on_us) == 0x44u,
	       "ti_rf_core_on_us");
_Static_assert(offsetof(struct radiant_power_diag, ti_rf_powerups) == 0x48u,
	       "ti_rf_powerups");
_Static_assert(offsetof(struct radiant_power_diag, ti_rf_pu_dur_us) == 0x4Cu,
	       "ti_rf_pu_dur_us");
_Static_assert(offsetof(struct radiant_power_diag, ti_fs_errors) == 0x50u,
	       "ti_fs_errors");
_Static_assert(offsetof(struct radiant_power_diag, ti_fold_backsteps) == 0x54u,
	       "ti_fold_backsteps");
#endif

/* ---------------------------------------------------------------------------
 * Arithmetic. Pure, stateless, in every build (the unit tests use them).
 * ---------------------------------------------------------------------------
 */

/* now - then for a free-running u32 counter: right across one wrap. */
static inline uint32_t radiant_power_diag_delta(uint32_t now, uint32_t then)
{
	return now - then;
}

/* on / interval in per-mille, clamped to 1000. An on-time a little longer
 * than its interval is normal (the two are sampled at different instants), so
 * the clamp is not an error. An empty interval reads 0. */
static inline uint32_t radiant_power_diag_duty_permille(uint32_t on_us,
							 uint32_t interval_us)
{
	uint64_t pm;

	if (interval_us == 0u) {
		return 0u;
	}
	pm = ((uint64_t)on_us * 1000u) / interval_us;
	return (pm > 1000u) ? 1000u : (uint32_t)pm;
}

/* count per second, in tenths, over interval_ms. Saturates at UINT32_MAX. */
static inline uint32_t radiant_power_diag_rate_x10(uint32_t count,
						    uint32_t interval_ms)
{
	uint64_t r;

	if (interval_ms == 0u) {
		return 0u;
	}
	r = ((uint64_t)count * 10000u) / interval_ms;
	return (r > UINT32_MAX) ? UINT32_MAX : (uint32_t)r;
}

/* Radio on-time of an operation armed to open at `open` and seen ending at
 * `now` (radiant_radio_now() units, 64-bit, never wraps). 0 if it never
 * opened; saturates at UINT32_MAX. */
static inline uint32_t radiant_power_diag_on_us(uint64_t now, uint64_t open)
{
	uint64_t d;

	if (now <= open) {
		return 0u;
	}
	d = now - open;
	return (d > UINT32_MAX) ? UINT32_MAX : (uint32_t)d;
}

/*
 * The idle accumulator, on a free-running u32 cycle clock (k_cycle_get_32()).
 * One interval at a time: open as the CPU goes to sleep, closed by the first
 * interrupt after the wake. Each interval is a u32 difference, so the clock
 * may wrap between open and close (an interval must be shorter than one wrap:
 * 36.4 h at 32768 Hz, 71.6 min at 1 MHz; the 1 s snapshot guarantees it).
 */
struct radiant_power_diag_idle {
	uint64_t idle_cyc; /* closed intervals, cycles */
	uint32_t t0;       /* open time, valid while open */
	bool     open;
};

/* Close the open interval at now. No-op when none is open, so a nested or
 * later interrupt cannot count the same sleep twice. */
static inline void radiant_power_diag_idle_close(struct radiant_power_diag_idle *d,
						 uint32_t now)
{
	if (d->open) {
		d->idle_cyc += (uint32_t)(now - d->t0);
		d->open = false;
	}
}

/* Open an interval at now. One still open (its wake went through an
 * interrupt that is not traced) is closed at now first. */
static inline void radiant_power_diag_idle_open(struct radiant_power_diag_idle *d,
						uint32_t now)
{
	radiant_power_diag_idle_close(d, now);
	d->t0 = now;
	d->open = true;
}

/* Active cycles in a span that held idle_delta idle cycles. Idle can read a
 * little over the span (an interval closed just after the span's end); that
 * clamps to 0 rather than wrapping. */
static inline uint32_t radiant_power_diag_active_cyc(uint32_t span,
						      uint64_t idle_delta)
{
	return (idle_delta >= span) ? 0u : (span - (uint32_t)idle_delta);
}

/*
 * The HFXO request's accumulator. Times are 64-bit microseconds on a clock
 * that does not depend on the HFXO (the system timer). Only transitions
 * count: a request while already held, or a release while not held, changes
 * nothing, so a caller that cannot track its own state may call freely.
 */
struct radiant_power_diag_hfxo {
	uint64_t held_us;  /* completed held intervals */
	uint64_t since_us; /* start of the current one, valid while on */
	uint32_t requests;
	uint32_t releases;
	bool     on;
};

static inline void radiant_power_diag_hfxo_note(struct radiant_power_diag_hfxo *h,
						bool on, uint64_t now_us)
{
	if (on == h->on) {
		return;
	}
	if (on) {
		h->since_us = now_us;
		h->requests++;
	} else {
		if (now_us > h->since_us) {
			h->held_us += now_us - h->since_us;
		}
		h->releases++;
	}
	h->on = on;
}

/* Total held time up to now_us, the running interval included. */
static inline uint64_t radiant_power_diag_hfxo_held(const struct radiant_power_diag_hfxo *h,
						    uint64_t now_us)
{
	uint64_t t = h->held_us;

	if (h->on && now_us > h->since_us) {
		t += now_us - h->since_us;
	}
	return t;
}

/* Writer half of THE SEQ PROTOCOL: copy src into dst under dst->seq. The
 * caller is the only writer of dst. src->seq is ignored. */
static inline void radiant_power_diag_publish(struct radiant_power_diag *dst,
					      const struct radiant_power_diag *src)
{
	uint32_t seq = dst->seq;

	/* Odd: a write is in progress. Never rests odd, so | 1 is +1. */
	dst->seq = seq | 1u;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	memcpy(dst, src, offsetof(struct radiant_power_diag, seq));
	memcpy((uint8_t *)dst + offsetof(struct radiant_power_diag, uptime_ms),
	       (const uint8_t *)src + offsetof(struct radiant_power_diag, uptime_ms),
	       sizeof(*dst) - offsetof(struct radiant_power_diag, uptime_ms));
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	dst->seq = (seq | 1u) + 1u;
}

/* Reader half: s1 and s2 are seq read before and after the fields. */
static inline bool radiant_power_diag_seq_consistent(uint32_t s1, uint32_t s2)
{
	return ((s1 & 1u) == 0u) && (s1 == s2);
}

/* ---------------------------------------------------------------------------
 * Hooks. Empty without CONFIG_RADIANT_POWER_DIAG.
 * ---------------------------------------------------------------------------
 */

#if defined(CONFIG_RADIANT_POWER_DIAG)

/* The published snapshot. Read it over SWD; in C, read it under the seq
 * protocol or from the system work queue (its writer). */
extern struct radiant_power_diag radiant_power_diag;

/*
 * The backend took (on) or dropped (off) its HFXO request. Thread or ISR
 * context; takes irq_lock briefly, so never from a zero-latency interrupt
 * (the MPSL signal handler). The nRF backend calls it once with true after
 * clock_control_on() succeeds; a low-power backend that releases and
 * re-requests calls it on every transition.
 */
void radiant_power_diag_hfxo(bool on);

/* The backend requested CONSTLAT (nRF54L errata 20). Thread context. */
void radiant_power_diag_constlat_requested(void);

/*
 * The scheduler's armed operation ended after on_us of radio on-time (see
 * radiant_power_diag_on_us()). Called under the scheduler's own
 * serialization (radio callback or radiant_event_crit_enter()), which is the
 * only writer of the radio counters; lock-free.
 */
void radiant_power_diag_radio(bool tx, uint32_t on_us);

/*
 * The cc26xx backend's 64-bit fold of its 32-bit radio timer saw a read
 * (`now`) below the previous one (`last`) and is about to add 2^32. Called
 * only on that backward step, under the backend's irq_lock (the fold's own
 * serialization), which is the only writer of the fold counters; lock-free.
 * Classifies the step (src/radiant_rat_fold.h): a genuine wrap or a small
 * back-step the fold mistook for one.
 */
void radiant_power_diag_rat_backward(uint32_t last, uint32_t now);

#else

static inline void radiant_power_diag_hfxo(bool on)
{
	(void)on;
}

static inline void radiant_power_diag_constlat_requested(void)
{
}

static inline void radiant_power_diag_radio(bool tx, uint32_t on_us)
{
	(void)tx;
	(void)on_us;
}

static inline void radiant_power_diag_rat_backward(uint32_t last, uint32_t now)
{
	(void)last;
	(void)now;
}

#endif /* CONFIG_RADIANT_POWER_DIAG */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_POWER_DIAG_H_ */
