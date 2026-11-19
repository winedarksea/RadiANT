#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Turn power-diag counters into a labelled average-current ESTIMATE.

There is no current meter on this bench. What there is: the firmware's
CONFIG_RADIANT_POWER_DIAG counters (HFXO request time, radio TX/RX on-time,
CPU active time, idle entries, CONSTLAT state) and, from
scripts/jlink_peek.ps1, a sampled fraction of time the HFXO was actually
running. This multiplies those duties by currents from the vendors' own
product specifications and prints a per-component breakdown. Every line is an
ESTIMATE; every current carries its citation; a current that could not be
verified in the official document is UNKNOWN and is left out of the total,
and the components left out are listed.

    # one jlink_peek session carries both snapshots and the sampled HFXO duty
    python tools/ant_power_est.py --board nrf54l15dk pd.json

    # two snapshots taken apart (jlink_peek -Json, or the simple shape below)
    python tools/ant_power_est.py --board promicro before.json after.json

    # the firmware's own log lines (CONFIG_RADIANT_POWER_DIAG_LOG_S)
    python tools/ant_power_est.py --board nrf52840 --log console.txt --uart-on

    # sampled HFXO duty given by hand, as a fraction or k/n (gets a Wilson CI)
    python tools/ant_power_est.py --board nrf5340net a.json b.json --hfxo-duty 388/400

INPUT SHAPES (TO RECONCILE WITH A1 - field names follow radiant_power_diag.h
on power/a1 2189023, struct version 1):

  * jlink_peek -Json: {"tool": "jlink_peek", "samples": {...}, "diag":
    {"snapshots": [{"ok": true, "fields": {...}}, ...]}, "host_start_unix":
    ..., "host_end_unix": ...}. With one file, its first and last usable
    snapshots are differenced; "samples" supplies the HFXO duty and
    CONSTLAT fraction - but only when "liveness": {"verdict": "running"}
    (samples of a halted core are one frozen state; the counters are used
    instead, with a warning). "parse_ok": false adds a warning.
  * simple snapshot: {"fields": {"uptime_ms": ..., "hfxo_req_us": ...,
    "radio_tx_us": ..., "radio_rx_us": ..., "radio_tx_ops": ...,
    "radio_rx_ops": ..., "cpu_active_us": ..., "idle_entries": ...,
    "seq": ..., "flags": ..., "constlat_stat": ...}, "host_time_unix": ...}
    (or the bare fields object). Missing or 0xFFFFFFFF fields are "not
    measured". host_time_unix is optional; with it, a reboot between the
    snapshots is caught by comparing uptime against the host clock.
  * --log: lines containing "power dt_ms=... hfxo_pm=... tx_us=... tx_ops=...
    rx_us=... rx_ops=... cpu_pm=... idle_x10=... constlat=R/H" (deltas per
    line; -1 = not measured). All lines are summed.

THE MODEL (deliberately simple, stated so it can be argued with):

    I = I_floor
      + d_tx  * (I_tx  - I_floor)          radio rows include the HFXO
      + d_rx  * (I_rx  - I_floor)
      + max(0, d_hfxo - d_tx - d_rx) * I_hfxo
      + d_cpu * (I_cpu - I_floor)          CPU rows include their clock
      + f_constlat * (I_constlat - I_idle0)        nRF54L15 only
      + max(0, d_rfon - d_tx - d_rx) * I_rfcore    cc2652 only, LOWER BOUND
      + I_uart                                     only with --uart-on

d_rfon is the TI RF core's powered time (rfon_pm on the log line,
ti_rf_core_on_us in a snapshot); the part outside TX and RX (power-up setup,
synth, the tail before power-down) is charged at the datasheet's "RF core
idle" peripheral delta. The radio rows already include the core. That term is
a LOWER BOUND: while the synthesiser is locked after CMD_FS, waiting for a TX
or RX, the chip draws milliamps, not the idle delta, and ti_rf_core_on_us
starts at the setup command, so the core's boot before it is not in d_rfon
at all. The datasheet has no row for either, so a separate "synth/boot"
component is listed as UNKNOWN and excluded; the wake time the driver
budgets (power-ups/s x pudur_us, in us/s) is printed beside it as a size
hint, not charged.

I_hfxo is, where the datasheet has one, the whole-chip "one TIMER running
@ 1 MHz, clock = HFXO" row counted over the floor - today's state is the
HFXO held with the stack's 1 MHz timebase TIMER free-running (nRF52840
ITIMER2, nRF5340 net INET,TIMER1; the nRF54L15 has only TIMER10 at 32 MHz,
ITIMER3, which leans high). --hfxo-row picks the crystal-core standby rows
instead, a lower bound. CPU_BAND_PM / --cpu-band-pm adds a resolution band
to the CPU term (TO RECONCILE WITH A1).

d_* are fractions of the interval, clamped to 1. CPU and radio time are added as if they
never overlap (an ISR during a radio window is counted twice), and the CPU
rows' own clock source is not subtracted from the HFXO term, so the result
leans high. Datasheet rows are typical values at 25 C, 3 V.

Exit status: 0 with an estimate; 1 on unusable input (reboot between
snapshots, counters that cannot be differenced, no interval).
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import re
import sys
from typing import Any

U32 = 1 << 32
NOT_MEASURED = 0xFFFFFFFF
MAGIC = 0x52504431          # RADIANT_POWER_DIAG_MAGIC (TO RECONCILE WITH A1)
VERSION = 1                 # RADIANT_POWER_DIAG_VERSION

# flags bits, radiant_power_diag.h (TO RECONCILE WITH A1)
F_HFXO_HELD = 1 << 0
F_HFXO_MEASURED = 1 << 1
F_CONSTLAT_REQ = 1 << 2
F_CONSTLAT_HW_VALID = 1 << 3
F_CPU_LOAD = 1 << 4
F_IDLE = 1 << 5
F_TI = 1 << 6               # RADIANT_POWER_DIAG_F_TI: ti_* fields measured (cc26xx)

# Which flag says a field is measured. A field with a flag is judged by the
# flag when the snapshot carries flags, else by the 0xFFFFFFFF sentinel.
# Fields with no flag (radio_*, uptime, seq) are always measured, so a
# free-running counter that happens to sit at 0xFFFFFFFF is a value, not
# "not measured" (TO RECONCILE WITH A1).
FIELD_FLAG = {
    "hfxo_req_us": F_HFXO_MEASURED,
    "cpu_active_us": F_CPU_LOAD,
    "idle_entries": F_IDLE,
    "ti_rf_core_on_us": F_TI,
}

# Fields that count microseconds: they wrap after 2^32 us (71.6 min).
US_FIELDS = ("hfxo_req_us", "radio_tx_us", "radio_rx_us", "cpu_active_us", "ti_rf_core_on_us")
COUNT_FIELDS = ("radio_tx_ops", "radio_rx_ops", "idle_entries")

HR_PERIOD_S = 8070 / 32768  # ANT+ heart-rate channel period

BANNER = "ESTIMATE — no current meter; datasheet currents"

# ---------------------------------------------------------------------------
# Documents. Nordic values were read from the docs.nordicsemi.com HTML of the
# version named (the site serves the latest version only); TI values from
# the ti.com PDFs. All typical, 25 C, 3 V.
# ---------------------------------------------------------------------------
DOCS = {
    "nrf52840": ("Nordic nRF52840 Product Specification v1.11 (Oct 2024)",
                 "https://docs.nordicsemi.com/bundle/ps_nrf52840/"),
    "nrf54l15": ("Nordic nRF54L15/L10/L05 Datasheet v1.0 (Sep 2025)",
                 "https://docs.nordicsemi.com/bundle/ps_nrf54l15/"),
    "nrf5340": ("Nordic nRF5340 Product Specification v1.6 (Feb 2025)",
                "https://docs.nordicsemi.com/bundle/ps_nrf5340/"),
    "cc2652r": ("TI CC2652R datasheet SWRS207J (rev. Nov 2023)",
                "https://www.ti.com/lit/ds/symlink/cc2652r.pdf"),
}


@dataclasses.dataclass(frozen=True)
class Current:
    """One datasheet current. ma None = UNKNOWN (excluded from totals)."""
    ma: float | None
    doc: str
    where: str      # section / table / symbol, or why it is UNKNOWN
    total: bool = False   # a whole-chip System ON figure: counted over the floor

    @property
    def known(self) -> bool:
        return self.ma is not None

    def cite(self) -> str:
        if self.doc in DOCS:
            return f"{DOCS[self.doc][0]}, {self.where}"
        return self.where


def _unk(why: str) -> Current:
    return Current(None, "", "UNKNOWN: " + why)


# --- nRF52840 PS v1.11, chapter "Current consumption" (common conditions:
# 3 V, 25 C, regulator LDO unless the row says DC/DC, 256 kB RAM on) ---
N52_TX0_DCDC = Current(6.40, "nrf52840", "Current consumption > Radio transmitting/receiving, IRADIO_TX1 (0 dBm, 1 Mbps BLE, HFXO, DC/DC)")
N52_TX8_DCDC = Current(16.40, "nrf52840", "Current consumption > Radio transmitting/receiving, IRADIO_TX0 (+8 dBm, 1 Mbps BLE, HFXO, DC/DC)")
N52_TX0_LDO = Current(10.80, "nrf52840", "Current consumption > Radio transmitting/receiving, IRADIO_TX3 (0 dBm, 1 Mbps BLE, HFXO, LDO)")
N52_RX_DCDC = Current(6.26, "nrf52840", "Current consumption > Radio transmitting/receiving, IRADIO_RX0 (1 Mbps BLE, HFXO, DC/DC)")
N52_RX_LDO = Current(10.10, "nrf52840", "Current consumption > Radio transmitting/receiving, IRADIO_RX1 (1 Mbps BLE, HFXO, LDO)")
N52_CPU_DCDC = Current(3.3, "nrf52840", "Current consumption > CPU running, ICPU0 (CoreMark 64 MHz from flash, HFXO, DC/DC)")
N52_CPU_LDO = Current(6.3, "nrf52840", "Current consumption > CPU running, ICPU1 (CoreMark 64 MHz from flash, HFXO, LDO)")
N52_FLOOR = Current(0.00316, "nrf52840", "Current consumption > Sleep, ION_RAMON_RTC (System ON, 256 kB retained, RTC on LFRC, LDO; nearest row - no LFXO or DC/DC sleep row)")
# "HFXO held, CPU idle" with the stack's 1 MHz timebase TIMER running is
# today's state (HFXO requested at init, TIMER free-running). The TIMER rows
# are that state as whole-chip totals. The common conditions default to LDO
# and the row names no regulator, so on the DC/DC DK it leans high.
N52_HFXO = {
    "ITIMER2": Current(0.646, "nrf52840", "Current consumption > TIMER running, ITIMER2 (one TIMER instance running @ 1 MHz, Clock = HFXO; LDO by the chapter's common conditions)", total=True),
}
N52_UART = _unk("nRF52840 PS has no UARTE current-consumption scenario")

# --- nRF54L15 datasheet v1.0, chapter "Current consumption" (common
# conditions: 3 V, 25 C, DC/DC, HFINT 128 MHz, 32 kB RAM unless stated) ---
L15_TX0 = Current(4.8, "nrf54l15", "Current consumption > RADIO transmitting/receiving, IRADIO_TX0 (0 dBm, HFXO)")
L15_TX4 = Current(6.6, "nrf54l15", "Current consumption > RADIO transmitting/receiving, IRADIO_TX1 (+4 dBm, HFXO)")
L15_TX8 = Current(9.8, "nrf54l15", "Current consumption > RADIO transmitting/receiving, IRADIO_TX2 (+8 dBm, HFXO)")
L15_RX = Current(3.4, "nrf54l15", "Current consumption > RADIO transmitting/receiving, IRADIO_RX0 (1 Mbps, HFXO)")
L15_CPU = Current(2.6, "nrf54l15", "Current consumption > CPU running, IAPPCPU0 (CoreMark 128 MHz from NVM, cache on; the cpuapp default clock is 128 MHz)")
L15_FLOOR = Current(0.0029, "nrf54l15", "Current consumption > Sleep, ION_IDLE8 (System ON, wake on pin + GRTC, LFXO, 256 KB RAM retained)")
L15_IDLE0 = Current(0.0008, "nrf54l15", "Current consumption > Sleep, ION_IDLE0 (System ON, wake on pin, 0 KB RAM)")
L15_CONSTLAT = Current(0.55, "nrf54l15", "Current consumption > Sleep, ION_IDLE11 (System ON, wake on pin, Constant Latency mode, 0 KB RAM); used as ION_IDLE11 - ION_IDLE0")
# The datasheet has no "HFXO running" current row. The nearest whole-chip
# figure is the TIMER table's only HFXO row: TIMER10 at 32 MHz. The stack's
# timebase is TIMER20 at 1 MHz (apps/*/boards/nrf54l15dk overlays,
# radiant,radio-timer = &timer20), and ITIMER2 "TIMER20 running at 1 MHz" is
# 121 uA on HFINT, so ITIMER3 leans high. The ISTBY rows are crystal-core
# standby only, a lower bound.
L15_HFXO = {
    "ITIMER3": Current(0.240, "nrf54l15", "Current consumption > TIMER active, ITIMER3 (TIMER10 running at 32 MHz, HFXO; the stack's TIMER20 at 1 MHz draws less)", total=True),
    "X2": Current(0.034, "nrf54l15", "32 MHz crystal oscillator (HFXO), ISTBY_X32M_X2 (core standby, 2.0x1.6 mm crystal; crystal core only - a lower bound)"),
    "X3": Current(0.063, "nrf54l15", "32 MHz crystal oscillator (HFXO), ISTBY_X32M_X3 (core standby, 1.2x1.0 mm crystal; crystal core only - a lower bound)"),
}
L15_UART = _unk("nRF54L15 datasheet lists no UARTE current")

# --- nRF5340 PS v1.6, network core (common conditions: 3 V, 25 C, DC/DC on
# VREGMAIN/VREGRADIO; radio rows: 64 kB network RAM retained) ---
N53_TX0_DCDC = Current(4.1, "nrf5340", "Current consumption > Radio, IRADIO_TX1 (0 dBm, 1 Mbps BLE, HFXO64M)")
N53_TX3_DCDC = Current(5.3, "nrf5340", "Current consumption > Radio, IRADIO_TX0 (+3 dBm, 1 Mbps BLE, HFXO64M)")
N53_TX0_LDO = Current(9.7, "nrf5340", "Current consumption > Radio, IRADIO_TX3 (0 dBm, 1 Mbps BLE, HFXO64M, LDO)")
N53_RX_DCDC = Current(3.7, "nrf5340", "Current consumption > Radio, IRADIO_RX0 (1 Mbps BLE, HFXO64M)")
N53_RX_LDO = Current(8.0, "nrf5340", "Current consumption > Radio, IRADIO_RX1 (1 Mbps BLE, HFXO64M, LDO)")
N53_CPU_DCDC = Current(2.6, "nrf5340", "Current consumption > Network CPU running, INETCPU2 (CoreMark from flash, HFXO64M)")
N53_CPU_LDO = Current(5.1, "nrf5340", "Current consumption > Network CPU running, INETCPU0 (CoreMark from flash, LDO; HFINT)")
N53_FLOOR = Current(0.0017, "nrf5340", "Current consumption > Sleep, 'System ON, 64 kB network RAM, wake on network RTC (running from LFXO clock)' (printed as ION_IDLE8; the symbol is duplicated in the table)")
N53_HFXO = {
    # The net core's own "TIMER @ 1 MHz on HFXO64M" row: today's state exactly.
    "INET_TIMER1": Current(0.400, "nrf5340", "Current consumption > TIMER running, INET,TIMER1 (one network TIMER running @ 1 MHz, clock = HFXO64M)", total=True),
    "X0": Current(0.065, "nrf5340", "Oscillators > 32 MHz crystal oscillator (HFXO), ISTBY_X32M, crystal X0 2.5x2.0 mm (crystal core only)"),
    "X1": Current(0.187, "nrf5340", "Oscillators > 32 MHz crystal oscillator (HFXO), ISTBY_X32M, crystal X1 1.6x1.2 mm (crystal core only)"),
    "X2": Current(0.135, "nrf5340", "Oscillators > 32 MHz crystal oscillator (HFXO), ISTBY_X32M, crystal X2 2.0x1.6 mm (crystal core only)"),
    "X3": Current(0.181, "nrf5340", "Oscillators > 32 MHz crystal oscillator (HFXO), ISTBY_X32M, crystal X3 1.2x1.0 mm (crystal core only)"),
}
N53_UART = _unk("nRF5340 PS 'UARTE active' rows do not say which core; the common conditions point at the application core")

# --- TI CC2652R SWRS207J sec. 8.5/8.6 (CC2652P SWRS195C sec. 7.5/7.6 prints
# the same values for these rows); CC26x2REM-7ID, 25 C, VDDS 3.0 V, DC/DC ---
TI_TX0 = Current(7.0, "cc2652r", "8.6 Power Consumption - Radio Modes, TX 2.4 GHz PA (BLE), 0 dBm setting, 2440 MHz")
TI_TX5 = Current(9.2, "cc2652r", "8.6 Power Consumption - Radio Modes, TX 2.4 GHz PA (BLE), +5 dBm setting, 2440 MHz")
TI_RX = Current(6.9, "cc2652r", "8.6 Power Consumption - Radio Modes, radio receive current, 2440 MHz")
TI_CPU = Current(3.39, "cc2652r", "8.5 Power Consumption - Power Modes, Active: MCU running CoreMark at 48 MHz, RCOSC_HF")
TI_IDLE = Current(0.675, "cc2652r", "8.5 Power Consumption - Power Modes, Idle: supply systems and RAM powered, RCOSC_HF")
TI_STANDBY = Current(0.00094, "cc2652r", "8.5 Power Consumption - Power Modes, Standby without cache retention, RTC running, 80 kB RAM, RCOSC_LF")
TI_UART = Current(0.1675, "cc2652r", "8.5 Iperi, UART: delta current, clock enabled, module idle")
# The RF core powered and clocked but not transmitting or receiving: what the
# core costs between power-up and power-down outside its TX/RX operations.
TI_RFCORE_IDLE = Current(0.2109, "cc2652r", "8.5 Iperi, RF core: delta current, power domain enabled, clock enabled, RF core idle")
TI_SYNTH_BOOT = _unk("CC2652R datasheet has no row for the RF core booting, or for the "
                     "synthesiser locked between CMD_FS and a TX/RX (mA, far above the "
                     "RF core idle delta)")
TI_HFXO_WHY = "CC2652R/P datasheets list no XOSC_HF current"


@dataclasses.dataclass(frozen=True)
class Board:
    name: str
    regulator: str
    floor: Current
    tx: dict[int, Current]   # dBm -> current
    rx: Current
    cpu: Current
    hfxo: dict[str, Current]  # rows for "HFXO held outside radio"; pick with --hfxo-row
    uart: Current
    constlat: Current | None = None
    constlat_base: Current | None = None
    notes: tuple[str, ...] = ()
    hfxo_default: str | None = None   # row used without --hfxo-row; None = UNKNOWN
    hfxo_unknown_why: str = "no row chosen"
    # RF core powered outside TX/RX (a delta, not counted over the floor);
    # None = the board has no such term (its radio has no separate core).
    rfcore_idle: Current | None = None


BOARDS: dict[str, Board] = {
    # nrf54l15dk: Zephyr board dts sets regulator-initial-mode DCDC.
    "nrf54l15dk": Board("nRF54L15 DK", "DC/DC", L15_FLOOR, {0: L15_TX0, 4: L15_TX4, 8: L15_TX8},
                        L15_RX, L15_CPU, L15_HFXO, L15_UART, L15_CONSTLAT, L15_IDLE0,
                        ("HFXO row ITIMER3 is TIMER10 at 32 MHz on HFXO (leans high); "
                         "--hfxo-row X2|X3 gives the crystal-core lower bound.",),
                        hfxo_default="ITIMER3"),
    # nrf52840: the DK (board dts regulator-initial-mode DCDC).
    "nrf52840": Board("nRF52840 DK", "DC/DC", N52_FLOOR, {0: N52_TX0_DCDC, 8: N52_TX8_DCDC},
                      N52_RX_DCDC, N52_CPU_DCDC, N52_HFXO, N52_UART,
                      notes=("HFXO row ITIMER2 is an LDO figure; this board runs DC/DC, so it leans high.",),
                      hfxo_default="ITIMER2"),
    # promicro: no DCDC node upstream, so LDO.
    "promicro": Board("Pro Micro nRF52840 (LDO)", "LDO", N52_FLOOR,
                      {0: N52_TX0_LDO, 8: _unk("nRF52840 PS has no +8 dBm LDO row")},
                      N52_RX_LDO, N52_CPU_LDO, N52_HFXO, N52_UART, hfxo_default="ITIMER2"),
    "nrf5340net": Board("nRF5340 network core", "DC/DC", N53_FLOOR, {0: N53_TX0_DCDC, 3: N53_TX3_DCDC},
                        N53_RX_DCDC, N53_CPU_DCDC, N53_HFXO, N53_UART,
                        notes=("--hfxo-row X0..X3 gives the crystal-core lower bound instead.",),
                        hfxo_default="INET_TIMER1"),
    "cc2652": Board("CC2652R/P LaunchXL", "DC/DC", TI_IDLE, {0: TI_TX0, 5: TI_TX5},
                    TI_RX, TI_CPU, {}, TI_UART,
                    notes=("Floor is Idle (675 uA): standby is off in this firmware; --ti-standby "
                           "uses the standby row instead.",),
                    hfxo_unknown_why=TI_HFXO_WHY, rfcore_idle=TI_RFCORE_IDLE),
}

LDO_OVERRIDES = {
    "nrf52840": dict(tx={0: N52_TX0_LDO, 8: _unk("nRF52840 PS has no +8 dBm LDO row")},
                     rx=N52_RX_LDO, cpu=N52_CPU_LDO, regulator="LDO", notes=()),
    # No LDO row for the network TIMER on HFXO64M: crystal rows only.
    "nrf5340net": dict(tx={0: N53_TX0_LDO, 3: _unk("nRF5340 PS has no +3 dBm LDO row")},
                       rx=N53_RX_LDO, cpu=N53_CPU_LDO, regulator="LDO",
                       hfxo={k: v for k, v in N53_HFXO.items() if k != "INET_TIMER1"},
                       hfxo_default=None,
                       hfxo_unknown_why="no LDO row for the network TIMER on HFXO64M; pass --hfxo-row X0..X3"),
}

# CPU-load resolution band, per-mille of the interval, (low, high) added to
# the measured CPU duty. TO RECONCILE WITH A1: A1 is changing how
# cpu_active_us is computed (cpu_load_get floors to whole per-mille, ~0.5
# per-mille low). Set this to A1's documented resolution, e.g. (0.0, 1.0) for
# a floor, or override per run with --cpu-band-pm LO,HI.
CPU_BAND_PM: tuple[float, float] = (0.0, 0.0)

# Longest single radio operation the scheduler charges in one piece: A1
# charges an operation's whole on-time when it ends, and a scan chunk is up
# to RADIANT_SCHED_SCAN_CHUNK_US (250 ms, radiant_sched.h), so a us counter
# can legitimately run ahead of the interval by that much.
MAX_OP_US = 260_000


# ---------------------------------------------------------------------------
# Arithmetic helpers.
# ---------------------------------------------------------------------------
def u32_delta(new: int, old: int) -> int:
    """Difference of two free-running u32 counters, modulo 2^32."""
    return (int(new) - int(old)) % U32


def wilson_interval(k: int, n: int, z: float = 1.959963984540054) -> tuple[float, float]:
    """95 % Wilson score interval for k successes in n trials."""
    if n <= 0:
        raise ValueError("n must be positive")
    if not 0 <= k <= n:
        raise ValueError("need 0 <= k <= n")
    p = k / n
    z2 = z * z
    den = 1 + z2 / n
    center = (p + z2 / (2 * n)) / den
    half = z * math.sqrt(p * (1 - p) / n + z2 / (4 * n * n)) / den
    # The bounds at k = 0 and k = n are exactly 0 and 1; floating point
    # lands a hair inside.
    lo = 0.0 if k == 0 else max(0.0, center - half)
    hi = 1.0 if k == n else min(1.0, center + half)
    return lo, hi


def charge_nc(current_ma: float, on_us: float) -> float:
    """Charge of one operation: mA x us = nC."""
    return current_ma * on_us


def ua_at_period(charge_per_period_nc: float, period_s: float = HR_PERIOD_S) -> float:
    """Average current of a charge spent once per period: nC / s = nA -> uA."""
    if period_s <= 0:
        raise ValueError("period must be positive")
    return charge_per_period_nc / period_s / 1000.0


# ---------------------------------------------------------------------------
# Inputs -> one Interval.
# ---------------------------------------------------------------------------
class InputError(Exception):
    pass


@dataclasses.dataclass
class Interval:
    dt_s: float
    tx_us: int | None = None
    rx_us: int | None = None
    tx_ops: int | None = None
    rx_ops: int | None = None
    hfxo_us: int | None = None
    cpu_us: int | None = None
    idle_entries: int | None = None
    constlat: bool | None = None
    rfon_us: int | None = None      # TI RF core powered (cc26xx power diag)
    rf_wake_us: float | None = None  # TI: sum of power-ups x the driver's pudur_us
    source: str = ""
    problems: list[str] = dataclasses.field(default_factory=list)


def _measured(v: Any) -> bool:
    return v is not None and int(v) != NOT_MEASURED


def normalise_snapshot(obj: Any) -> dict:
    """Accept a simple snapshot ({"fields": {...}}) or a bare fields dict."""
    if not isinstance(obj, dict):
        raise InputError("snapshot is not a JSON object")
    if "fields" in obj and isinstance(obj["fields"], dict):
        snap = {"fields": dict(obj["fields"])}
        if "host_time_unix" in obj:
            snap["host_time_unix"] = obj["host_time_unix"]
        return snap
    return {"fields": dict(obj)}


def _as_list(v: Any) -> list:
    if v is None:
        return []
    return v if isinstance(v, list) else [v]


def peek_snapshots(peek: dict) -> list[dict]:
    """Usable snapshots of a jlink_peek -Json result, in order, with host times."""
    diag = peek.get("diag") or {}
    snaps = [s for s in _as_list(diag.get("snapshots")) if s.get("ok") and s.get("fields")]
    out = []
    for s in snaps:
        snap = {"fields": dict(s["fields"])}
        when = s.get("when")
        if when == "before" and peek.get("host_start_unix"):
            snap["host_time_unix"] = peek["host_start_unix"]
        elif when == "after" and peek.get("host_end_unix"):
            snap["host_time_unix"] = peek["host_end_unix"]
        out.append(snap)
    return out


def diff_snapshots(a: dict, b: dict, host_tol_s: float = 5.0, host_tol_frac: float = 0.05) -> Interval:
    """Interval between two snapshots, with wrap handling and reboot checks.

    The host-clock check is loose on purpose: jlink_peek's host times bracket
    the whole session including the connect, and the struct is republished
    only once a second. A reboot or a wrong pair misses by far more.
    """
    fa, fb = a["fields"], b["fields"]
    for key, want in (("magic", MAGIC), ("version", VERSION)):
        for f in (fa, fb):
            if key in f and int(f[key]) != want:
                raise InputError(f"{key} {int(f[key]):#x} != {want:#x}: wrong address or "
                                 "layout (TO RECONCILE WITH A1)")
    if "uptime_ms" not in fa or "uptime_ms" not in fb:
        raise InputError("both snapshots need uptime_ms")
    if "seq" in fa and "seq" in fb and int(fb["seq"]) < int(fa["seq"]):
        raise InputError(f"seq went backwards ({fa['seq']} -> {fb['seq']}): the target "
                         "rebooted between snapshots, or they are in the wrong order")
    dt_ms = u32_delta(fb["uptime_ms"], fa["uptime_ms"])
    if dt_ms == 0:
        raise InputError("zero interval between snapshots (same snapshot twice?)")
    iv = Interval(dt_s=dt_ms / 1000.0, source="snapshots")

    ha, hb = a.get("host_time_unix"), b.get("host_time_unix")
    if ha is not None and hb is not None:
        host_dt = float(hb) - float(ha)
        if abs(host_dt - iv.dt_s) > max(host_tol_s, host_tol_frac * abs(host_dt)):
            raise InputError(f"target uptime advanced {iv.dt_s:.1f} s but the host clock "
                             f"{host_dt:.1f} s: reboot or wrap between snapshots")

    flags = int(fb["flags"]) if _measured(fb.get("flags")) else None
    dt_us = dt_ms * 1000
    us_ok = dt_us < U32
    if not us_ok:
        iv.problems.append(f"interval {iv.dt_s:.0f} s >= 2^32 us (71.6 min): the us counters "
                           "cannot be differenced unambiguously; take snapshots closer together")

    def measured(f: dict, name: str) -> bool:
        if f.get(name) is None:
            return False
        if name not in FIELD_FLAG:
            return True
        if _measured(f.get("flags")):
            return bool(int(f["flags"]) & FIELD_FLAG[name])
        return _measured(f[name])

    def get(name: str, is_us: bool, slack_us: int = 2000) -> int | None:
        if not (measured(fa, name) and measured(fb, name)):
            return None
        if is_us and not us_ok:
            return None
        d = u32_delta(fb[name], fa[name])
        if is_us and d > dt_us * 1.01 + slack_us:
            iv.problems.append(f"{name} advanced {d} us in a {dt_us} us interval: "
                               "impossible, counter not trusted")
            return None
        if is_us and d > dt_us:
            iv.problems.append(f"{name} advanced {d} us in a {dt_us} us interval (an operation "
                               "is charged whole when it ends); duty clamped to 100 %")
            d = dt_us
        return d

    # Radio on-time is charged per operation at its end, up to MAX_OP_US late.
    iv.tx_us = get("radio_tx_us", True, MAX_OP_US)
    iv.rx_us = get("radio_rx_us", True, MAX_OP_US)
    iv.tx_ops = get("radio_tx_ops", False)
    iv.rx_ops = get("radio_rx_ops", False)
    iv.hfxo_us = get("hfxo_req_us", True)
    iv.cpu_us = get("cpu_active_us", True)
    iv.idle_entries = get("idle_entries", False)
    iv.rfon_us = get("ti_rf_core_on_us", True)
    pu = get("ti_rf_powerups", False) if measured(fb, "ti_rf_core_on_us") else None
    if pu is not None and _measured(fb.get("ti_rf_pu_dur_us")):
        iv.rf_wake_us = float(pu) * int(fb["ti_rf_pu_dur_us"])
    if flags is not None:
        if flags & F_CONSTLAT_HW_VALID and fb.get("constlat_stat") is not None:
            iv.constlat = bool(int(fb["constlat_stat"]) & 1)
    elif _measured(fb.get("constlat_stat")):
        iv.constlat = bool(int(fb["constlat_stat"]) & 1)
    return iv


LOG_RE = re.compile(r"\bpower dt_ms=(\d+)(?P<rest>.*)$")
KV_RE = re.compile(r"(\w+)=(-?\d+)(?:/(-?\d+))?")


def parse_log(lines: list[str]) -> Interval:
    """Sum the firmware's per-interval power lines (TO RECONCILE WITH A1)."""
    dt_ms = 0
    sums = {"tx_us": 0, "rx_us": 0, "tx_ops": 0, "rx_ops": 0}
    weighted = {"hfxo_pm": [0, 0], "cpu_pm": [0, 0], "idle_x10": [0, 0],
                "rfon_pm": [0, 0]}  # [sum*dt, dt]
    constlat = None
    wake = [0.0, 0]   # [sum of power-ups x pudur_us, dt covered]
    n = 0
    for line in lines:
        m = LOG_RE.search(line)
        if not m:
            continue
        d = int(m.group(1))
        kv = {k: (int(v), int(w) if w else None) for k, v, w in KV_RE.findall(m.group("rest"))}
        if d <= 0:
            continue
        n += 1
        dt_ms += d
        if "rfpu_x10" in kv and "pudur_us" in kv and kv["pudur_us"][0] >= 0:
            # power-ups in this line = rate/s x dt_s, at the driver's pudur now
            wake[0] += kv["rfpu_x10"][0] / 10 * d / 1000 * kv["pudur_us"][0]
            wake[1] += d
        for k in sums:
            if k in kv:
                sums[k] += kv[k][0]
        for k, acc in weighted.items():
            if k in kv and kv[k][0] >= 0:
                acc[0] += kv[k][0] * d
                acc[1] += d
        if "constlat" in kv and kv["constlat"][1] is not None and kv["constlat"][1] >= 0:
            constlat = bool(kv["constlat"][1])
    if n == 0:
        raise InputError("no 'power dt_ms=' lines in the log")
    iv = Interval(dt_s=dt_ms / 1000.0, source=f"log ({n} lines)",
                  tx_us=sums["tx_us"], rx_us=sums["rx_us"],
                  tx_ops=sums["tx_ops"], rx_ops=sums["rx_ops"], constlat=constlat)
    dt_us = dt_ms * 1000

    def full(k: str) -> bool:
        return weighted[k][1] == dt_ms   # measured on every line

    if full("hfxo_pm"):
        iv.hfxo_us = round(weighted["hfxo_pm"][0] / dt_ms / 1000 * dt_us)   # mean per-mille x dt
    if full("cpu_pm"):
        iv.cpu_us = round(weighted["cpu_pm"][0] / dt_ms / 1000 * dt_us)
    if full("idle_x10"):
        iv.idle_entries = round(weighted["idle_x10"][0] / 10 / 1000)   # sum(rate/s x dt_s)
    if full("rfon_pm"):
        iv.rfon_us = round(weighted["rfon_pm"][0] / dt_ms / 1000 * dt_us)
        # The firmware floors rfon_pm per line: each line loses under 1 per-mille
        # of ITS dt, so the sum loses under 1 per-mille of the elapsed interval
        # (dt_ms us), whatever the duty. Against the on-time that is 1000/rfon_pm
        # per-mille, so at low duty the relative error is large. Not corrected:
        # the term it feeds is already a lower bound (RF core idle (LB)).
        low_us = dt_ms   # 1 per-mille of dt_us
        rel = (f"{100.0 * low_us / iv.rfon_us:.0f} % of the {iv.rfon_us} us measured"
               if iv.rfon_us else "unbounded relative to the 0 us measured")
        iv.problems.append(f"rfon_pm is floored on each log line: the RF core on-time "
                           f"may read up to 1 per-mille of the elapsed interval low "
                           f"(< {low_us} us in {iv.dt_s:g} s; {rel})")
    if wake[1] == dt_ms:
        iv.rf_wake_us = wake[0]
    return iv


# ---------------------------------------------------------------------------
# Estimate.
# ---------------------------------------------------------------------------
@dataclasses.dataclass
class Component:
    name: str
    basis: str            # duty / state text
    current: Current | None
    ua: float | None      # contribution, uA; None = excluded
    why_excluded: str = ""


@dataclasses.dataclass
class Estimate:
    board: Board
    interval: Interval
    components: list[Component]
    total_ua: float
    excluded: list[str]
    hfxo_duty: float | None
    hfxo_duty_source: str
    hfxo_ci: tuple[float, float] | None
    notes: list[str]
    total_band_ua: tuple[float, float] | None = None   # from the CPU resolution band


def resolve_board(name: str, regulator: str | None, ti_standby: bool) -> Board:
    b = BOARDS[name]
    if regulator == "ldo" and b.regulator != "LDO":
        if name not in LDO_OVERRIDES:
            raise InputError(f"no LDO rows for {name}")
        b = dataclasses.replace(b, **LDO_OVERRIDES[name])
    elif regulator == "dcdc" and b.regulator != "DC/DC":
        raise InputError(f"{name} is modelled as {b.regulator} only (use --board nrf52840 for DC/DC)")
    if ti_standby:
        if name != "cc2652":
            raise InputError("--ti-standby applies to --board cc2652 only")
        b = dataclasses.replace(b, floor=TI_STANDBY)
    return b


def estimate(board: Board, iv: Interval, *, tx_dbm: int = 0, uart_on: bool = False,
             hfxo_row: str | None = None, hfxo_duty: float | None = None,
             hfxo_ci: tuple[float, float] | None = None,
             constlat_fraction: float | None = None,
             cpu_band_pm: tuple[float, float] = CPU_BAND_PM,
             extra_notes: list[str] | None = None) -> Estimate:
    dt_us = iv.dt_s * 1e6
    comps: list[Component] = []
    notes = list(board.notes) + list(iv.problems) + list(extra_notes or [])

    floor_ma = board.floor.ma if board.floor.known else 0.0

    def add(name: str, basis: str, cur: Current | None, factor: float | None, why: str = "",
            minus_ma: float = 0.0) -> None:
        """Contribution = factor x (datasheet current - minus_ma); cur stays as cited."""
        if factor is None:
            comps.append(Component(name, basis, cur, None, why or "not measured on this build"))
        elif cur is None or not cur.known:
            comps.append(Component(name, basis, cur, None,
                                   why or (cur.where if cur else "no datasheet current")))
        else:
            comps.append(Component(name, basis, cur, factor * max(0.0, cur.ma - minus_ma) * 1000.0))

    def duty(us: int | None) -> float | None:
        if us is None:
            return None
        if us > dt_us:
            # A summed log can carry an operation charged at its end past the
            # interval; a duty is never above 1.
            notes.append(f"a counter ran {us - dt_us:.0f} us past the interval; duty clamped to 100 %")
            return 1.0
        return us / dt_us

    d_tx, d_rx, d_cpu = duty(iv.tx_us), duty(iv.rx_us), duty(iv.cpu_us)

    add("idle floor", "always", board.floor, 1.0)

    tx_cur = board.tx.get(tx_dbm, _unk(f"no {board.name} TX row at {tx_dbm} dBm "
                                      f"(rows: {sorted(board.tx)})"))
    add(f"radio TX {tx_dbm:+d} dBm", _pct(d_tx), tx_cur, d_tx, minus_ma=floor_ma)
    add("radio RX", _pct(d_rx), board.rx, d_rx, minus_ma=floor_ma)

    # TI: the RF core's powered time outside TX/RX, at the core's idle delta
    # (the radio rows already include the core while it transmits/receives).
    if board.rfcore_idle is not None:
        d_on = duty(iv.rfon_us)
        rf_idle = None
        if d_on is not None:
            rf_idle = max(0.0, d_on - (d_tx or 0.0) - (d_rx or 0.0))
        add("RF core idle (LB)",
            _pct(rf_idle) + (f" of {_pct(d_on)} on" if d_on is not None else ""),
            board.rfcore_idle, rf_idle,
            "" if d_on is not None else "RF core on-time not measured (rfon_pm / "
            "ti_rf_core_on_us need CONFIG_RADIANT_POWER_DIAG on cc26xx)")
        # What that term cannot see: the synthesiser locked between CMD_FS and
        # the TX/RX (mA), and the core's boot before setup (outside rfon).
        wake = None if iv.rf_wake_us is None else iv.rf_wake_us / iv.dt_s
        add("RF synth/boot", "n/a" if wake is None else f"wake {wake:.0f} us/s (info)",
            TI_SYNTH_BOOT, 1.0)
        notes.append("RF core idle (LB) is a LOWER BOUND: it charges the core's on-time "
                     "outside TX/RX at the idle delta, but the synth locked after CMD_FS "
                     "draws mA and the boot before setup is not in rfon at all "
                     "(RF synth/boot, UNKNOWN, excluded)"
                     + ("" if wake is None else
                        f"; the driver's wake budget, power-ups/s x pudur_us, is "
                        f"{wake:.0f} us/s ({wake / 1000:.2f} per-mille), informational"))

    # HFXO: sampled duty wins over the stack's own request counter (MPSL or
    # USB may hold the crystal too); radio rows already include the crystal.
    src = "not measured"
    d_h = None
    if hfxo_duty is not None:
        d_h, src = hfxo_duty, "jlink_peek register samples"
        if iv.hfxo_us is not None:
            notes.append(f"stack HFXO request counter says {_pct(duty(iv.hfxo_us))} "
                         f"(sampled {_pct(hfxo_duty)})")
    elif iv.hfxo_us is not None:
        d_h, src = duty(iv.hfxo_us), "hfxo_req_us counter (stack's own request only)"
    row = hfxo_row if hfxo_row is not None else board.hfxo_default
    if row is None:
        hint = f"; pass --hfxo-row ({'/'.join(board.hfxo)})" if board.hfxo else ""
        hcur = _unk(board.hfxo_unknown_why + hint)
    elif row not in board.hfxo:
        raise InputError(f"--hfxo-row {row}: {board.name} rows are {sorted(board.hfxo) or 'none'}")
    else:
        hcur = board.hfxo[row]
    hf_only = None
    if d_h is not None:
        hf_only = max(0.0, d_h - (d_tx or 0.0) - (d_rx or 0.0))
    add("HFXO (outside radio)", _pct(hf_only) + (f" of {_pct(d_h)} on" if d_h is not None else ""),
        hcur, hf_only, minus_ma=floor_ma if hcur.total else 0.0)

    add("CPU active", _pct(d_cpu), board.cpu, d_cpu, minus_ma=floor_ma)
    cpu_band = None
    cpu_c = comps[-1]
    if cpu_c.ua is not None and d_cpu is not None and cpu_band_pm != (0.0, 0.0):
        per_duty = max(0.0, board.cpu.ma - floor_ma) * 1000.0
        lo = min(1.0, max(0.0, d_cpu + cpu_band_pm[0] / 1000.0)) * per_duty
        hi = min(1.0, max(0.0, d_cpu + cpu_band_pm[1] / 1000.0)) * per_duty
        cpu_band = (lo - cpu_c.ua, hi - cpu_c.ua)
        cpu_c.basis += f" [{cpu_band_pm[0]:+g}/{cpu_band_pm[1]:+g} pm]"
        notes.append(f"CPU load resolution band {cpu_band_pm[0]:+g}..{cpu_band_pm[1]:+g} per-mille "
                     f"-> CPU term {cpu_band[0]:+.2f}..{cpu_band[1]:+.2f} uA (TO RECONCILE WITH A1)")

    if board.constlat is not None:
        f = constlat_fraction
        if f is None and iv.constlat is not None:
            f = 1.0 if iv.constlat else 0.0
        base = board.constlat_base
        add("CONSTLAT", "not read" if f is None else _pct(f), board.constlat, f,
            "CONSTLAT state not read" if f is None else "",
            minus_ma=base.ma if base is not None and base.known else 0.0)

    if uart_on:
        add("UART (declared on)", "always", board.uart, 1.0)

    total = sum(c.ua for c in comps if c.ua is not None)
    excluded = [c.name for c in comps if c.ua is None]
    band = None
    if cpu_band is not None:
        band = (total + cpu_band[0], total + cpu_band[1])
    return Estimate(board, iv, comps, total, excluded, d_h, src, hfxo_ci, notes, band)


def _pct(x: float | None) -> str:
    return "n/a" if x is None else f"{100 * x:.3f} %"


def per_op_lines(board: Board, iv: Interval, tx_dbm: int) -> list[str]:
    out = []
    cur = board.tx.get(tx_dbm)
    if iv.tx_ops and iv.tx_us is not None and cur is not None and cur.known:
        per = iv.tx_us / iv.tx_ops
        q = charge_nc(cur.ma, per)
        out.append(f"per broadcast: {per:.0f} us TX on-time -> {q:.0f} nC at {cur.ma} mA; "
                   f"once per HR period (8070/32768 s) = {ua_at_period(q):.2f} uA")
        rate = iv.tx_ops / iv.dt_s
        out.append(f"broadcast rate {rate:.3f} /s (HR period = {1 / HR_PERIOD_S:.3f} /s)")
    if iv.rx_ops and iv.rx_us is not None and board.rx.known:
        per = iv.rx_us / iv.rx_ops
        q = charge_nc(board.rx.ma, per)
        out.append(f"per RX window: {per:.0f} us -> {q:.0f} nC at {board.rx.ma} mA; "
                   f"once per HR period = {ua_at_period(q):.2f} uA")
    if iv.idle_entries is not None:
        out.append(f"wakes: {iv.idle_entries / iv.dt_s:.1f} idle entries /s")
    return out


def render(est: Estimate, tx_dbm: int) -> str:
    banner = BANNER
    enc = getattr(sys.stdout, "encoding", None)   # None: an in-memory text stream
    try:
        if enc:
            banner.encode(enc)
    except (UnicodeEncodeError, LookupError):
        banner = BANNER.replace("—", "--")
    L = [banner, "=" * len(banner)]
    iv = est.interval
    L.append(f"board {est.board.name} ({est.board.regulator}); interval {iv.dt_s:.1f} s from {iv.source}")
    if est.hfxo_duty is not None:
        ci = ""
        if est.hfxo_ci:
            ci = f", 95 % CI {100 * est.hfxo_ci[0]:.1f}..{100 * est.hfxo_ci[1]:.1f} %"
        L.append(f"HFXO duty {100 * est.hfxo_duty:.2f} % ({est.hfxo_duty_source}{ci})")
    L.append("")
    L.append(f"{'component':<22} {'duty/state':<26} {'mA':>8} {'avg uA':>10}")
    for c in est.components:
        ma = f"{c.current.ma:.4g}" if c.current is not None and c.current.known else "UNKNOWN"
        ua = f"{c.ua:10.2f}" if c.ua is not None else f"{'excluded':>10}"
        L.append(f"{c.name:<22} {c.basis:<26} {ma:>8} {ua}")
    L.append(f"{'TOTAL (known parts)':<22} {'':<26} {'':>8} {est.total_ua:10.2f}")
    if est.total_band_ua:
        L.append(f"{'  with CPU band':<22} {'':<26} {'':>8} "
                 f"{est.total_band_ua[0]:.2f}..{est.total_band_ua[1]:.2f}")
    if est.excluded:
        L.append(f"EXCLUDED from the total: {', '.join(est.excluded)}")
    L.append("")
    L.extend(per_op_lines(est.board, iv, tx_dbm))
    L.append("")
    L.append("citations (typical, 25 C, 3 V):")
    for c in est.components:
        if c.current is not None:
            L.append(f"  {c.name}: {c.current.cite()}")
        if c.ua is None and c.why_excluded and (c.current is None or c.why_excluded != c.current.where):
            L.append(f"  {c.name}: excluded - {c.why_excluded}")
    for n in est.notes:
        L.append(f"note: {n}")
    L.append("mA = the cited datasheet row. Radio/CPU rows are counted over the floor "
             "(CONSTLAT over ION_IDLE0); overlaps are added, so this leans high.")
    return "\n".join(L)


def to_json(est: Estimate) -> dict:
    return {
        "banner": BANNER,
        "board": est.board.name,
        "interval_s": est.interval.dt_s,
        "hfxo_duty": est.hfxo_duty,
        "hfxo_duty_source": est.hfxo_duty_source,
        "hfxo_ci95": est.hfxo_ci,
        "components": [
            {"name": c.name, "basis": c.basis,
             "ma": c.current.ma if c.current is not None else None,
             "avg_ua": c.ua, "citation": c.current.cite() if c.current is not None else None,
             "excluded": c.ua is None, "why_excluded": c.why_excluded or None}
            for c in est.components],
        "total_ua_known": est.total_ua,
        "total_ua_band": est.total_band_ua,
        "excluded": est.excluded,
        "notes": est.notes,
    }


def parse_duty(text: str) -> tuple[float, tuple[float, float] | None]:
    """'0.97' or '388/400' (the latter gets a Wilson interval)."""
    if "/" in text:
        try:
            k, n = (int(x) for x in text.split("/", 1))
        except ValueError:
            raise argparse.ArgumentTypeError(f"--hfxo-duty {text!r}: k/n needs two integers") from None
        if n <= 0 or not 0 <= k <= n:
            raise argparse.ArgumentTypeError(f"--hfxo-duty {text!r}: need n > 0 and 0 <= k <= n")
        return k / n, wilson_interval(k, n)
    try:
        v = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"--hfxo-duty {text!r}: not a number") from None
    if not 0.0 <= v <= 1.0:
        raise argparse.ArgumentTypeError("--hfxo-duty is a fraction 0..1 or k/n")
    return v, None


def parse_band(text: str) -> tuple[float, float]:
    """'LO,HI' per-mille, e.g. '0,1' or '-1,0'."""
    try:
        lo, hi = (float(x) for x in text.split(",", 1))
    except ValueError:
        raise argparse.ArgumentTypeError(f"--cpu-band-pm {text!r}: want LO,HI") from None
    if lo > hi:
        raise argparse.ArgumentTypeError(f"--cpu-band-pm {text!r}: LO > HI")
    return lo, hi


def sampled_duty(peek: dict | None) -> tuple[float | None, tuple[float, float] | None,
                                             float | None, list[str]]:
    """HFXO duty, its CI and the CONSTLAT fraction from a jlink_peek result.

    Refused unless the session's liveness verdict is "running": samples of a
    halted (or unknown) core are one frozen state, not a duty cycle.
    """
    notes: list[str] = []
    if not peek:
        return None, None, None, notes
    samples = peek.get("samples") or {}
    if not samples.get("valid"):
        return None, None, None, notes
    verdict = (peek.get("liveness") or {}).get("verdict")
    if peek.get("parse_ok") is False:
        notes.append("WARNING: jlink_peek could not align all of its output "
                     f"({peek.get('parse_error')}); samples after the gap are missing")
    if verdict != "running":
        notes.append(f"WARNING: jlink_peek liveness verdict is {verdict!r}, not 'running': "
                     "sampled HFXO/CONSTLAT state refused, falling back to the counters")
        return None, None, None, notes
    duty = ci = cl = None
    if samples.get("hfxo_duty") is not None:
        duty = float(samples["hfxo_duty"])
        ci = wilson_interval(int(samples["hfxo_running"]), int(samples["valid"]))
    if samples.get("constlat_valid"):
        cl = int(samples["constlat_on"]) / int(samples["constlat_valid"])
    return duty, ci, cl, notes


def load_inputs(paths: list[str], log: str | None) -> tuple[Interval, dict | None]:
    """Returns the interval and the jlink_peek result that carried samples, if any."""
    samples = None
    if log:
        if paths:
            raise InputError("give snapshots or --log, not both")
        with open(log, encoding="utf-8", errors="replace") as fh:
            return parse_log(fh.read().splitlines()), None
    objs = []
    for p in paths:
        with open(p, encoding="utf-8-sig") as fh:
            objs.append(json.load(fh))
    snaps: list[dict] = []
    for i, o in enumerate(objs):
        if isinstance(o, dict) and o.get("tool") == "jlink_peek":
            ps = peek_snapshots(o)
            if o.get("samples") and o["samples"].get("valid"):
                samples = o
            if len(objs) == 1:
                snaps.extend(ps)
            elif ps:
                snaps.append(ps[0] if i == 0 else ps[-1])
        else:
            snaps.append(normalise_snapshot(o))
    if len(snaps) < 2:
        raise InputError("need two usable snapshots (two files, or one jlink_peek run "
                         "with -Samples > 1 so it dumps before and after)")
    return diff_snapshots(snaps[0], snaps[-1]), samples


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("snapshots", nargs="*", help="jlink_peek -Json file(s) or simple snapshot JSON")
    ap.add_argument("--log", help="console log with 'power dt_ms=' lines instead of snapshots")
    ap.add_argument("--board", required=True, choices=sorted(BOARDS))
    ap.add_argument("--regulator", choices=("dcdc", "ldo"), help="override the board's regulator mode")
    ap.add_argument("--tx-dbm", type=int, default=0, help="transmit power the image uses (default 0)")
    ap.add_argument("--hfxo-duty", type=parse_duty, help="sampled HFXO duty: fraction or k/n")
    ap.add_argument("--hfxo-row", "--hfxo-crystal", dest="hfxo_row",
                    help="row for the HFXO-held current (nRF54L15: ITIMER3 default, X2/X3; "
                         "nRF5340 net: INET_TIMER1 default, X0..X3; nRF52840: ITIMER2)")
    ap.add_argument("--cpu-band-pm", type=parse_band, default=CPU_BAND_PM,
                    help="CPU-load resolution band LO,HI per-mille (TO RECONCILE WITH A1)")
    ap.add_argument("--uart-on", action="store_true", help="the console UART is enabled in this image")
    ap.add_argument("--ti-standby", action="store_true", help="cc2652: floor = standby, not idle")
    ap.add_argument("--json", help="also write the estimate as JSON")
    a = ap.parse_args(argv)
    try:
        board = resolve_board(a.board, a.regulator, a.ti_standby)
        iv, peek = load_inputs(a.snapshots, a.log)
        duty, ci, cl_frac, notes = sampled_duty(peek)
        if a.hfxo_duty is not None:
            duty, ci = a.hfxo_duty
        est = estimate(board, iv, tx_dbm=a.tx_dbm, uart_on=a.uart_on, hfxo_row=a.hfxo_row,
                       hfxo_duty=duty, hfxo_ci=ci, constlat_fraction=cl_frac,
                       cpu_band_pm=a.cpu_band_pm, extra_notes=notes)
    except (InputError, OSError, ValueError, KeyError) as e:
        print(f"ant_power_est: {e}", file=sys.stderr)
        return 1
    print(render(est, a.tx_dbm))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(to_json(est), fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
