# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Read-only J-Link sampler: clock/power status registers and the
    radiant_power_diag struct of a RUNNING nRF target, never reset or halted.

.DESCRIPTION
    There is no current meter on this bench. This script supplies the two
    software proxies tools/ant_power_est.py turns into labelled estimates:

      - how often the HFXO is running (and, on the nRF54L15, whether constant
        latency mode is on), from N register samples taken at randomised
        intervals, reported as a fraction with a 95 % Wilson interval;
      - a raw dump of a RAM symbol (by default radiant_power_diag, the
        CONFIG_RADIANT_POWER_DIAG struct), decoded with -Decode, before and
        after the sampling run so one session yields two snapshots to diff.

    THE GUARD. J-Link V9.66 carries newer OB-probe firmware than the nRF54L15
    DK's probe and tries to "update" it on ANY connect - including a
    read-only one, including an empty script. The update fails at
    "Emulator did not re-enumerate" and leaves the probe in its bootloader
    (VID_1366&PID_0101). `exec DisableAutoUpdateFW` must therefore be the
    FIRST line of every CommanderScript, and SelectEmuBySN must come after it
    (selecting an emulator connects to it). The prologue below is copied from
    scripts/flash_sim_jlink.ps1, and Assert-SafeJLinkScript refuses to run any
    script whose first line is not that guard - checked on the lines built
    here AND on the file as written to disk, immediately before JLink.exe.

    READ-ONLY. The script is checked against a whitelist before it runs: the
    guard, SelectEmuBySN, si SWD, speed, device, connect, mem32, sleep and q.
    Nothing else - in particular no r (reset), h (halt), g (go), s (step),
    loadfile, erase or w1/w2/w4 (writes) - can reach JLink.exe from here.
    mem32 addresses are limited to the preset registers, the Cortex-M debug
    status register and SRAM (symbol dumps).

    LIVENESS. DHCSR (0xE000EDF0) is read at the start and end of the session:
    S_HALT (bit 17) set means the core is halted and every sample is a frozen
    state, so the run is reported as such. On the nRF52840 and nRF5340 network
    core RTC1.COUNTER (the Zephyr system timer) is read too, as corroboration;
    a counter that did not move is reported, not treated as fatal (an image
    may not run RTC1). Reading DHCSR clears its sticky S_RESET_ST/S_RETIRE_ST
    bits; that only matters to another debugger.

    SAMPLING. ANT+ slots repeat every 8070/32768 s (246 ms). A fixed sample
    interval would phase-lock to that grid and report one slot phase as the
    duty, so each delay is drawn uniformly from IntervalMs +/- JitterMs; keep
    2*JitterMs at or above 247 ms. The Wilson interval assumes independent
    samples: HFXO on/off periods longer than the sample spacing make it
    optimistic. An attached SWD session may change the target's sleep
    behaviour (debug interface mode); cross-check the fraction against the
    struct's hfxo_req_us.

    BENCH-CHECKED ON THE nRF54L15 DK ONLY (Gate GA, 2026-11-18, J-Link V9.66,
    OB firmware Feb 2025): `connect` left the core running (DHCSR S_HALT
    clear at both ends, power_diag uptime advancing with the host clock), the
    mem32 output parsed as assumed, and the only security-pattern lines were
    the two benign Cortex-M33 banners below. The nRF52840 and 5340-net
    presets are still unchecked on hardware. The DHCSR check, the power_diag
    uptime notes and the strict output alignment stay to catch a change.
    UNSECURE = MASS ERASE. If the target is read-protected, J-Link may offer
    to unsecure it. JLink.exe's stdin carries explicit "n" answers; the output
    is scanned for security/erase/unlock wording (exit 3, loud warning); and
    a live run refuses to start if J-Link's saved settings (registry
    Software\SEGGER, %APPDATA%\SEGGER\*.ini) look like a remembered answer.
    Whether the prompt is a console prompt (which the "n" answers reach) or
    a dialog is unverified. A 5340 network core held off by the
    application core cannot be connected to.

    NEVER ON TI. CC26xx parts use an XDS110, and a JTAG connect there wipes
    RAM. Any -Device naming a CC part is refused.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.PARAMETER Preset
    nrf54l15   - CLOCK.XO.RUN/STAT + POWER.CONSTLATSTAT (device nRF54L15_M33)
    nrf52840   - CLOCK.HFCLKRUN/HFCLKSTAT              (device nRF52840_xxAA)
    nrf5340net - network-core CLOCK.HFCLKRUN/HFCLKSTAT (device nRF5340_xxAA_NET)

.PARAMETER Device
    J-Link device name. Defaults from -Preset. Must start with nRF.

.PARAMETER Serial
    Probe serial number (required in practice with more than one probe).

.PARAMETER Samples
    Number of register samples (default 1).

.PARAMETER IntervalMs
    Mean delay between samples in ms (default 300).

.PARAMETER JitterMs
    Half-width of the uniform delay range (default 150: delays 150..450 ms).

.PARAMETER Seed
    RNG seed for the delays; 0 (default) picks one and reports it.

.PARAMETER ElfPath
    zephyr.elf to look -Symbol up in (arm-zephyr-eabi-nm -S).

.PARAMETER Symbol
    Symbol to dump (default radiant_power_diag). Must live in SRAM.

.PARAMETER SymbolAddress
    Hex address, instead of -ElfPath (with -SymbolSize in bytes).

.PARAMETER Decode
    Decode a radiant_power_diag dump with the layout table below.

.PARAMETER Json
    Write the result as JSON for tools/ant_power_est.py.

.PARAMETER NonSecure
    nRF54L15 only: read the non-secure peripheral alias (0x4010E000) instead
    of the secure one (0x5010E000). Default is secure, which is what a
    non-TF-M Zephyr image uses.

.PARAMETER DryRun
    Print the generated CommanderScript (after the safety checks) and exit.
    Needs neither JLink.exe nor a probe.

.PARAMETER ParseLog
    Do not run JLink.exe: parse a saved JLink.exe output against the plan the
    same arguments generate (re-analysis, and the self-test).

.PARAMETER SelfTest
    Run the built-in checks (whitelist, parser, Wilson, decode) and exit.

.EXAMPLE
    .\scripts\jlink_peek.ps1 -Preset nrf54l15 -Serial 001057737173 -DryRun
    .\scripts\jlink_peek.ps1 -Preset nrf54l15 -Serial 001057737173 -Samples 400
    .\scripts\jlink_peek.ps1 -Preset nrf54l15 -Serial 001057737173 -Samples 400 `
        -ElfPath build\hrtx_l15\hr_transmitter\zephyr\zephyr.elf -Decode -Json pd.json
    .\scripts\jlink_peek.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [ValidateSet('nrf54l15', 'nrf52840', 'nrf5340net')]
    [string]$Preset,
    [string]$JLinkExe = 'C:\Program Files\SEGGER\JLink_V966\JLink.exe',
    [string]$Device,
    [int]$Speed = 4000,
    [string]$Serial,
    [int]$Samples = 1,
    [int]$IntervalMs = 300,
    [int]$JitterMs = 150,
    [int]$Seed = 0,
    [string]$ElfPath,
    [string]$Symbol = 'radiant_power_diag',
    [string]$SymbolAddress,
    [int]$SymbolSize = 0,
    [string]$NmExe = 'C:\ncs\toolchains\dcbdc366a1\opt\zephyr-sdk\gnu\arm-zephyr-eabi\bin\arm-zephyr-eabi-nm.exe',
    [switch]$Decode,
    [string]$Json,
    [switch]$NonSecure,
    [switch]$DryRun,
    [string]$ParseLog,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

# ---------------------------------------------------------------------------
# radiant_power_diag layout.  TO RECONCILE WITH A1.
#
# Taken from radiant/include/radiant/radiant_power_diag.h as committed on the
# power/a1 branch (2189023, struct version 1): every field a little-endian
# u32, no padding, 0x58 bytes. If the header changes in review, edit this
# table (and FIELDS in tools/ant_power_est.py) - nothing else here depends on
# the field names except SeqOffset/magic/version/size below.
# ---------------------------------------------------------------------------
$PowerDiag = @{
    Magic       = 0x52504431L     # RADIANT_POWER_DIAG_MAGIC, "RPD1"
    Version     = 1               # RADIANT_POWER_DIAG_VERSION
    Size        = 0x58            # sizeof(struct radiant_power_diag)
    SeqOffset   = 0x0C            # seq: odd = being written, 0 = none yet
    NotMeasured = 4294967295L     # RADIANT_POWER_DIAG_NOT_MEASURED (0xFFFFFFFF)
    Fields      = @(
        @{ Off = 0x00; Name = 'magic' },
        @{ Off = 0x04; Name = 'version' },
        @{ Off = 0x08; Name = 'size' },
        @{ Off = 0x0C; Name = 'seq' },
        @{ Off = 0x10; Name = 'uptime_ms' },
        @{ Off = 0x14; Name = 'hfxo_req_us' },
        @{ Off = 0x18; Name = 'hfxo_requests' },
        @{ Off = 0x1C; Name = 'hfxo_releases' },
        @{ Off = 0x20; Name = 'radio_tx_us' },
        @{ Off = 0x24; Name = 'radio_rx_us' },
        @{ Off = 0x28; Name = 'radio_tx_ops' },
        @{ Off = 0x2C; Name = 'radio_rx_ops' },
        @{ Off = 0x30; Name = 'cpu_active_us' },
        @{ Off = 0x34; Name = 'cpu_load_permille' },
        @{ Off = 0x38; Name = 'idle_entries' },
        @{ Off = 0x3C; Name = 'flags' },
        @{ Off = 0x40; Name = 'constlat_stat' },
        @{ Off = 0x44; Name = 'ti_rf_core_on_us' },
        @{ Off = 0x48; Name = 'ti_rf_powerups' },
        @{ Off = 0x4C; Name = 'ti_rf_pu_dur_us' },
        @{ Off = 0x50; Name = 'ti_fs_errors' },
        @{ Off = 0x54; Name = 'ti_fold_backsteps' }
    )
}
$DiagAttempts = 3   # (block, seq) read pairs per snapshot; first consistent wins
$DiagMinSpanMs = 2500   # minimum planned sleep between the before and after dumps

# ---------------------------------------------------------------------------
# Register map. Addresses are [int64] (L suffix): PowerShell 5.1 reads a hex
# literal at or above 0x80000000 as a NEGATIVE int32.
# MDK headers: C:\ncs\v3.4.0\modules\hal\nordic\nrfx\bsp\stable\mdk\
# ---------------------------------------------------------------------------

# Cortex-M Debug Halting Control and Status Register (ARMv7-M / ARMv8-M).
# CMSIS core_cm33.h / core_cm4.h: DCB_BASE = CoreDebug_BASE = 0xE000EDF0,
# DHCSR at offset 0; DCB_DHCSR_S_HALT_Pos 17, S_SLEEP_Pos 18, S_LOCKUP_Pos 19.
$DHCSR = 0xE000EDF0L

$Presets = @{
    'nrf54l15' = @{
        Device     = 'nRF54L15_M33'
        DevicePattern = '^nRF54L15_M33$'
        # nrf54l15_global.h: NRF_CLOCK_S_BASE = NRF_POWER_S_BASE = 0x5010E000UL,
        #                    NRF_CLOCK_NS_BASE = NRF_POWER_NS_BASE = 0x4010E000UL
        # nrf54l15_types.h:  NRF_CLOCK_Type.XO @0x400 (NRF_CLOCK_XO_Type:
        #                    RUN @0x08, STAT @0x0C) -> XO.RUN 0x408, XO.STAT 0x40C
        #                    NRF_POWER_Type.CONSTLATSTAT @0x520
        Base       = 0x5010E000L
        BaseNs     = 0x4010E000L
        Reads      = @(
            # One 2-word read: XO.RUN then XO.STAT.
            @{ Name = 'xo'; Off = 0x408; Count = 2 },
            @{ Name = 'constlat'; Off = 0x520; Count = 1 }
        )
        Counter    = $null   # GRTC SYSCOUNTER is not readable unless a domain
                             # holds it ACTIVE (SYSCOUNTERH.BUSY); DHCSR only.
    }
    'nrf52840' = @{
        Device     = 'nRF52840_xxAA'
        DevicePattern = '^nRF52840_'
        # nrf52840.h: NRF_CLOCK_BASE 0x40000000UL; NRF_CLOCK_Type HFCLKRUN @0x408,
        #             HFCLKSTAT @0x40C
        Base       = 0x40000000L
        BaseNs     = $null
        Reads      = @( @{ Name = 'hfclk'; Off = 0x408; Count = 2 } )
        # nrf52840.h: NRF_RTC1_BASE 0x40011000UL; NRF_RTC_Type COUNTER @0x504
        Counter    = @{ Name = 'RTC1.COUNTER'; Addr = 0x40011504L }
    }
    'nrf5340net' = @{
        Device     = 'nRF5340_xxAA_NET'
        DevicePattern = '^nRF5340_.*NET$'
        # nrf5340_network.h: NRF_CLOCK_NS_BASE 0x41005000UL; NRF_CLOCK_Type
        #             HFCLKRUN @0x408, HFCLKSTAT @0x40C
        Base       = 0x41005000L
        BaseNs     = $null
        Reads      = @( @{ Name = 'hfclk'; Off = 0x408; Count = 2 } )
        # nrf5340_network.h: NRF_RTC1_NS_BASE 0x41016000UL; COUNTER @0x504
        Counter    = @{ Name = 'RTC1.COUNTER'; Addr = 0x41016504L }
    }
}

# Decode one sample (hashtable read-name -> word array) into flags.
function Get-SampleState([string]$presetName, [hashtable]$words) {
    $s = [ordered]@{ hfxo = $null; constlat = $null; raw = [ordered]@{} }
    switch ($presetName) {
        'nrf54l15' {
            $run = [int64]$words['xo'][0]; $stat = [int64]$words['xo'][1]
            $cl = [int64]$words['constlat'][0]
            $s.raw['XO.RUN'] = '0x{0:X8}' -f $run
            $s.raw['XO.STAT'] = '0x{0:X8}' -f $stat
            $s.raw['CONSTLATSTAT'] = '0x{0:X8}' -f $cl
            # nrf54l15_types.h: CLOCK_XO_STAT_STATE_Pos 16 (1 = Running,
            # "between START task and STOPPED event"); CLOCK_XO_RUN_STATUS_Pos 0
            # (1 = XOSTART triggered); POWER_CONSTLATSTAT_STATUS_Pos 0 (1 = Enable).
            $s.hfxo = ((($stat -shr 16) -band 1) -eq 1)
            $s.constlat = (($cl -band 1) -eq 1)
            $s['xo_run_triggered'] = (($run -band 1) -eq 1)
        }
        default {
            $run = [int64]$words['hfclk'][0]; $stat = [int64]$words['hfclk'][1]
            $s.raw['HFCLKRUN'] = '0x{0:X8}' -f $run
            $s.raw['HFCLKSTAT'] = '0x{0:X8}' -f $stat
            # nrf52840_bitfields.h / nrf5340_network_bitfields.h:
            # CLOCK_HFCLKSTAT_STATE_Pos 16 (1 = Running), CLOCK_HFCLKSTAT_SRC_Pos 0
            # (52840: 1 = Xtal "64 MHz crystal oscillator (HFXO)", 0 = RC/HFINT;
            #  5340 net: 1 = HFXO, 0 = HFINT). HFCLK running from HFINT is NOT
            # an HFXO-on sample.
            $state = (($stat -shr 16) -band 1) -eq 1
            $src = ($stat -band 1) -eq 1
            $s.hfxo = ($state -and $src)
            $s['hfclk_running'] = $state
        }
    }
    return $s
}

# ---------------------------------------------------------------------------
# Safety checks.
# ---------------------------------------------------------------------------
$GuardLine = 'exec DisableAutoUpdateFW'

function Test-DeviceAllowed([string]$dev) {
    if (-not $dev) { throw 'No J-Link device name (pass -Preset or -Device).' }
    if ($dev -match '^\s*CC' -or $dev -match 'CC(13|26)') {
        throw "Refusing $dev : TI parts are not J-Link targets here (XDS110), and a JTAG connect wipes their RAM."
    }
    if ($dev -notmatch '^nRF[0-9A-Za-z_]+$') {
        throw "Refusing device '$dev': only Nordic nRF J-Link device names are accepted."
    }
    if ($dev -match 'RV32|FLPR|PPR') {
        throw "Refusing $dev : not a Cortex-M core (no DHCSR, different address map)."
    }
}

# The whitelist. Every line must match one of these exactly (whole line).
# Note 'si SWD' is listed literally: 's' alone is Commander's single-step.
function Assert-SafeJLinkScript([string[]]$lines) {
    $lines = @($lines | Where-Object { $_ -ne $null -and $_.Trim() -ne '' })
    if ($lines.Count -lt 3) { throw 'J-Link script too short.' }
    if ($lines[0] -cne $GuardLine) {
        throw "UNSAFE J-Link script: first line is '$($lines[0])', not '$GuardLine'. Not running it."
    }
    if ($lines[-1] -cne 'q') { throw "J-Link script must end with 'q'." }
    $connects = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $l = $lines[$i]
        $ok = $false
        if ($i -eq 0) { $ok = ($l -ceq $GuardLine) }
        elseif ($l -cmatch '^SelectEmuBySN [0-9]{1,12}$') { $ok = ($i -eq 1) }
        elseif ($l -ceq 'si SWD') { $ok = $true }
        elseif ($l -cmatch '^speed [0-9]{1,6}$') { $ok = $true }
        elseif ($l -cmatch '^device nRF[0-9A-Za-z_]+$') { $ok = ($l -notmatch 'CC(13|26)') }
        elseif ($l -ceq 'connect') { $connects++; $ok = ($connects -eq 1) }
        elseif ($l -cmatch '^mem32 0x([0-9A-F]{8}), 0x([0-9A-F]{1,4})$') {
            $ok = ($connects -eq 1) -and (Test-ReadAddressAllowed ([Convert]::ToInt64($Matches[1], 16)) ([Convert]::ToInt32($Matches[2], 16)))
        }
        elseif ($l -cmatch '^sleep [0-9]{1,5}$') { $ok = ($connects -eq 1) }
        elseif ($l -ceq 'q') { $ok = ($i -eq $lines.Count - 1) }
        if (-not $ok) {
            throw "UNSAFE J-Link script: line $($i + 1) '$l' is not on the read-only whitelist. Not running it."
        }
    }
    if ($connects -ne 1) { throw 'J-Link script must connect exactly once.' }
}

# Reads allowed: DHCSR (1 word), the CLOCK/POWER block and RTC1.COUNTER of
# the three presets, and SRAM (symbol dumps).
function Test-ReadAddressAllowed([int64]$addr, [int]$count) {
    if ($count -lt 1 -or $count -gt 4096) { return $false }
    $end = $addr + 4L * $count - 1
    if ($addr -band 3) { return $false }
    if ($addr -eq $DHCSR -and $count -eq 1) { return $true }
    if ($addr -ge 0x20000000L -and $end -le 0x3FFFFFFFL) { return $true }   # SRAM region
    foreach ($p in $Presets.Values) {
        foreach ($b in @($p.Base, $p.BaseNs)) {
            if ($null -eq $b) { continue }
            foreach ($r in $p.Reads) {
                $ra = $b + $r.Off
                if ($addr -eq $ra -and $count -eq $r.Count) { return $true }
            }
        }
        if ($p.Counter -and $addr -eq $p.Counter.Addr -and $count -eq 1) { return $true }
    }
    return $false
}

# ---------------------------------------------------------------------------
# Unsecure / mass-erase defences.
#
# J-Link offers to "unsecure" a read-protected nRF (APPROTECT) on connect, and
# accepting is a mass erase of the board. Three layers, none of them a probe
# test (this was written without hardware):
#   1. JLink.exe's stdin is fed explicit "n" answers, so a console prompt
#      reads a refusal rather than whatever PowerShell's stdin holds.
#   2. The output is scanned for security/erase/unlock wording; any hit ends
#      the run with exit 3 and a loud warning (Find-SecurityPromptLines).
#   3. Before running, J-Link's saved settings (registry under
#      Software\SEGGER, and %APPDATA%\SEGGER\*.ini) are read - read only -
#      for a remembered answer or auto-accept; any hit refuses the run.
# ---------------------------------------------------------------------------
$StdinRefusals = @('n') * 64
# 'eras' catches erase, erasing, erased and ERASEALL.
$SecurityPattern = '(?i)secur|unsecur|eras|ERASEALL|APPROTECT|unlock'
# Lines that match the pattern but are routine Cortex-M33 connect banners.
# UNVERIFIED wording: if a bench run trips on another harmless line, add it
# here only after reading it.
$SecurityBenign = @('(?i)^\s*Security extension\s*:', '(?i)^\s*Secure debug\s*:')
$AutoAcceptPattern = '(?i)unsecur|unlock|erase|remember|dont.?ask|do.?not.?ask|noask|auto.?(accept|confirm|answer)|suppress.*(dialog|prompt|question)'

function Find-SecurityPromptLines([string[]]$text) {
    $hits = @()
    foreach ($line in $text) {
        if ($null -eq $line -or $line -notmatch $SecurityPattern) { continue }
        $benign = $false
        foreach ($b in $SecurityBenign) { if ($line -match $b) { $benign = $true } }
        if (-not $benign) { $hits += $line }
    }
    return ,$hits
}

function Find-AutoAcceptEntries([string[]]$entries) {
    return ,@($entries | Where-Object { $_ -match $AutoAcceptPattern })
}

# "where: name = value" strings from J-Link's saved settings, read only.
function Get-JLinkSavedSettings {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($root in 'HKCU:\Software\SEGGER', 'HKLM:\Software\SEGGER', 'HKLM:\Software\WOW6432Node\SEGGER') {
        if (-not (Test-Path $root)) { continue }
        $keys = @(Get-Item $root) + @(Get-ChildItem $root -Recurse -ErrorAction SilentlyContinue)
        foreach ($k in $keys) {
            foreach ($n in $k.GetValueNames()) { $out.Add(("{0}: {1} = {2}" -f $k.Name, $n, $k.GetValue($n))) }
        }
    }
    $iniDir = Join-Path $env:APPDATA 'SEGGER'
    if (Test-Path $iniDir) {
        foreach ($f in Get-ChildItem $iniDir -Filter *.ini -Recurse -File -ErrorAction SilentlyContinue) {
            foreach ($l in Get-Content $f.FullName -ErrorAction SilentlyContinue) { $out.Add(("{0}: {1}" -f $f.FullName, $l)) }
        }
    }
    return ,$out.ToArray()
}

# ---------------------------------------------------------------------------
# Plan: the script lines plus the reads they produce, in order.
# ---------------------------------------------------------------------------
function Format-Mem32([int64]$addr, [int]$count) {
    return ('mem32 0x{0:X8}, 0x{1:X}' -f $addr, $count)
}

function New-PeekPlan {
    param([hashtable]$Cfg)
    $lines = New-Object System.Collections.Generic.List[string]
    $reads = New-Object System.Collections.Generic.List[object]
    $sleeps = New-Object System.Collections.Generic.List[int]

    # --- guard prologue, as in scripts/flash_sim_jlink.ps1 ---
    # exec DisableAutoUpdateFW must be first. After the guard, never before
    # it: selecting an emulator connects to it, and a connect without the
    # guard is the thing that drops the probe into bootloader.
    $lines.Add($GuardLine)
    if ($Cfg.Serial) { $lines.Add("SelectEmuBySN $($Cfg.Serial)") }
    $lines.Add('si SWD')
    $lines.Add("speed $($Cfg.Speed)")
    $lines.Add("device $($Cfg.Device)")
    $lines.Add('connect')
    # --- no r / h here, unlike the flash scripts: the target keeps running ---

    $addRead = {
        param($tag, [int64]$addr, [int]$count)
        $lines.Add((Format-Mem32 $addr $count))
        $reads.Add([pscustomobject]@{ Tag = $tag; Addr = $addr; Count = $count })
    }
    $addDiag = {
        param($when)
        if (-not $Cfg.Diag) { return }
        $attempts = 1
        if ($Cfg.Diag.Layout) { $attempts = $DiagAttempts }
        for ($a = 0; $a -lt $attempts; $a++) {
            & $addRead "diag.$when.$a.block" $Cfg.Diag.Addr $Cfg.Diag.Words
            if ($Cfg.Diag.Layout) {
                & $addRead "diag.$when.$a.seq" ($Cfg.Diag.Addr + $PowerDiag.SeqOffset) 1
            }
        }
    }

    & $addRead 'live.0' $DHCSR 1
    $p = $null
    if ($Cfg.Preset) { $p = $Presets[$Cfg.Preset] }
    if ($p -and $p.Counter) { & $addRead 'ctr.0' $p.Counter.Addr 1 }
    & $addDiag 'before'

    if ($p) {
        $base = $p.Base
        if ($Cfg.NonSecure) { $base = $p.BaseNs }
        $rng = New-Object System.Random($Cfg.Seed)
        for ($i = 0; $i -lt $Cfg.Samples; $i++) {
            if ($i -gt 0) {
                $lo = [Math]::Max(1, $Cfg.IntervalMs - $Cfg.JitterMs)
                $hi = $Cfg.IntervalMs + $Cfg.JitterMs
                $d = $rng.Next($lo, $hi + 1)
                $sleeps.Add($d)
                $lines.Add("sleep $d")
            }
            foreach ($r in $p.Reads) { & $addRead "s.$i.$($r.Name)" ($base + $r.Off) $r.Count }
        }
    }

    if ($Cfg.Samples -gt 1 -and $Cfg.Diag) {
        # The struct is republished once a second (RADIANT_POWER_DIAG_SNAPSHOT_MS),
        # so dumps closer together than a couple of seconds usually catch the
        # same snapshot twice and ant_power_est.py sees a zero interval.
        $span = [int](($sleeps | Measure-Object -Sum).Sum)
        if ($span -lt $DiagMinSpanMs) {
            $pad = $DiagMinSpanMs - $span
            $sleeps.Add($pad)
            $lines.Add("sleep $pad")
        }
    }
    if ($Cfg.Samples -gt 1) { & $addDiag 'after' }
    elseif ($p -and $p.Counter) { $lines.Add('sleep 20') }   # let RTC1 tick between the counter reads
    if ($p -and $p.Counter) { & $addRead 'ctr.1' $p.Counter.Addr 1 }
    & $addRead 'live.1' $DHCSR 1
    $lines.Add('q')

    return [pscustomobject]@{ Lines = [string[]]$lines.ToArray(); Reads = $reads.ToArray(); Sleeps = $sleeps.ToArray() }
}

# ---------------------------------------------------------------------------
# Parsing JLink.exe output.
# Commander prints a mem32 result as "AAAAAAAA = WWWWWWWW WWWWWWWW ..." (several
# words per line). The plan reads the same register many times, so results
# are matched to reads strictly in order, and every line's address must be the
# next one the current read expects. A failed read ("Could not read memory")
# leaves a gap that breaks the alignment; the run is then reported invalid
# rather than shifted onto the wrong samples.
# ---------------------------------------------------------------------------
function ConvertFrom-Mem32Output([string[]]$text) {
    $out = New-Object System.Collections.Generic.List[object]
    foreach ($line in $text) {
        if ($null -eq $line) { continue }
        $m = [regex]::Match($line, '^\s*([0-9A-Fa-f]{8})\s*=\s*((?:[0-9A-Fa-f]{8}\s*)+)$')
        if (-not $m.Success) { continue }
        $words = @($m.Groups[2].Value.Trim() -split '\s+' | ForEach-Object { [Convert]::ToInt64($_, 16) })
        $out.Add([pscustomobject]@{ Addr = [Convert]::ToInt64($m.Groups[1].Value, 16); Words = $words })
    }
    return $out.ToArray()
}

function Join-ReadResults($reads, $parsed) {
    $res = @{}
    $k = 0
    foreach ($r in $reads) {
        $got = New-Object System.Collections.Generic.List[int64]
        while ($got.Count -lt $r.Count) {
            if ($k -ge $parsed.Count) {
                return @{ Ok = $false; Error = "output ended before read '$($r.Tag)' at 0x{0:X8}" -f $r.Addr; Results = $res }
            }
            $want = $r.Addr + 4L * $got.Count
            if ($parsed[$k].Addr -ne $want) {
                return @{ Ok = $false; Error = ("misaligned at read '{0}': expected 0x{1:X8}, output has 0x{2:X8} (a read failed?)" -f $r.Tag, $want, $parsed[$k].Addr); Results = $res }
            }
            foreach ($w in $parsed[$k].Words) { $got.Add($w) }
            $k++
        }
        if ($got.Count -ne $r.Count) {
            return @{ Ok = $false; Error = "read '$($r.Tag)' returned $($got.Count) words, expected $($r.Count)"; Results = $res }
        }
        $res[$r.Tag] = $got.ToArray()
    }
    if ($k -ne $parsed.Count) {
        return @{ Ok = $false; Error = "output has $($parsed.Count - $k) unexpected mem32 result line(s)"; Results = $res }
    }
    return @{ Ok = $true; Error = $null; Results = $res }
}

# ---------------------------------------------------------------------------
# Statistics.
# ---------------------------------------------------------------------------
function Get-WilsonInterval([int]$k, [int]$n, [double]$z = 1.959963984540054) {
    if ($n -le 0) { return @($null, $null) }
    $p = $k / [double]$n
    $z2 = $z * $z
    $den = 1.0 + $z2 / $n
    $center = ($p + $z2 / (2.0 * $n)) / $den
    $half = $z * [Math]::Sqrt($p * (1.0 - $p) / $n + $z2 / (4.0 * $n * $n)) / $den
    # Exactly 0 / 1 at k = 0 / k = n; floating point lands a hair inside.
    $lo = [Math]::Max(0.0, $center - $half); if ($k -eq 0) { $lo = 0.0 }
    $hi = [Math]::Min(1.0, $center + $half); if ($k -eq $n) { $hi = 1.0 }
    return @($lo, $hi)
}

# ---------------------------------------------------------------------------
# radiant_power_diag decode.
# ---------------------------------------------------------------------------
function ConvertFrom-PowerDiagWords([int64[]]$words) {
    $f = [ordered]@{}
    foreach ($fd in $PowerDiag.Fields) {
        $i = [int]($fd.Off / 4)
        if ($i -lt $words.Count) { $f[$fd.Name] = [int64]$words[$i] }
    }
    return $f
}

# Pick the first consistent attempt of one snapshot (see THE SEQ PROTOCOL in
# radiant_power_diag.h): seq in the block even, and equal to the seq re-read
# right after it.
function Select-DiagSnapshot([hashtable]$results, [string]$when, $diagCfg) {
    $snap = [ordered]@{ when = $when; ok = $false; attempt = $null; words = $null; fields = $null; problems = @() }
    $attempts = 1
    if ($diagCfg.Layout) { $attempts = $DiagAttempts }
    for ($a = 0; $a -lt $attempts; $a++) {
        $blk = $results["diag.$when.$a.block"]
        if ($null -eq $blk) { continue }
        if (-not $diagCfg.Layout) {
            $snap.ok = $true; $snap.attempt = $a; $snap.words = $blk; return $snap
        }
        $f = ConvertFrom-PowerDiagWords $blk
        $seqRead = $results["diag.$when.$a.seq"]
        if ($null -eq $seqRead) { $snap.problems += "attempt $a has no seq re-read"; continue }
        $s2 = $seqRead[0]
        if (($f['seq'] -band 1) -ne 0 -or $f['seq'] -ne $s2) {
            $snap.problems += "attempt $a torn (seq $($f['seq']) then $s2)"
            continue
        }
        $snap.attempt = $a; $snap.words = $blk; $snap.fields = $f
        if ($f['magic'] -ne $PowerDiag.Magic) { $snap.problems += ('magic 0x{0:X8} != 0x{1:X8}: wrong address or image' -f $f['magic'], $PowerDiag.Magic); return $snap }
        if ($f['version'] -ne $PowerDiag.Version) { $snap.problems += "version $($f['version']) != $($PowerDiag.Version): update the layout table"; return $snap }
        if ($f['size'] -ne $PowerDiag.Size) { $snap.problems += "size $($f['size']) != $($PowerDiag.Size): update the layout table"; return $snap }
        if ($f['seq'] -eq 0) { $snap.problems += 'seq 0: no snapshot published yet (counters are 0)' }
        $snap.ok = $true
        return $snap
    }
    if ($snap.problems.Count -eq 0) { $snap.problems += 'no data' }
    else { $snap.problems += 'every attempt torn; suspect the address' }
    return $snap
}

# ---------------------------------------------------------------------------
# Analysis of one run (live or -ParseLog).
# ---------------------------------------------------------------------------
function Get-PeekAnalysis($cfg, $plan, [string[]]$output) {
    $parsed = ConvertFrom-Mem32Output $output
    $j = Join-ReadResults $plan.Reads $parsed
    $r = $j.Results
    $an = [ordered]@{
        tool = 'jlink_peek'; schema = 1
        preset = $cfg.Preset; device = $cfg.Device; serial = $cfg.Serial
        parse_ok = $j.Ok; parse_error = $j.Error
        liveness = [ordered]@{ verdict = 'unknown'; dhcsr = @(); counter = $null; counter_moved = $null; notes = @() }
        samples = $null; diag = $null
    }

    # Liveness from DHCSR (start and end), counter as corroboration.
    $halted = $false; $lockup = $false; $have = 0
    foreach ($t in 'live.0', 'live.1') {
        if ($r.ContainsKey($t)) {
            $v = [int64]$r[$t][0]; $have++
            $an.liveness.dhcsr += ('0x{0:X8}' -f $v)
            if (($v -shr 17) -band 1) { $halted = $true }
            if (($v -shr 19) -band 1) { $lockup = $true }
        }
    }
    if ($have -eq 2) {
        if ($lockup) { $an.liveness.verdict = 'lockup' }
        elseif ($halted) { $an.liveness.verdict = 'halted' }
        else { $an.liveness.verdict = 'running' }
    }
    if ($r.ContainsKey('ctr.0') -and $r.ContainsKey('ctr.1')) {
        $c0 = [int64]$r['ctr.0'][0]; $c1 = [int64]$r['ctr.1'][0]
        $an.liveness.counter = @($c0, $c1)
        $an.liveness.counter_moved = ($c0 -ne $c1)
        if ($c0 -eq $c1) { $an.liveness.notes += 'RTC1.COUNTER did not move (stopped in this image, or the core is halted)' }
    }

    # Samples.
    if ($cfg.Preset) {
        $n = 0; $k = 0; $cn = 0; $ck = 0
        $states = @()
        for ($i = 0; $i -lt $cfg.Samples; $i++) {
            $w = @{}; $complete = $true
            foreach ($rd in $Presets[$cfg.Preset].Reads) {
                $t = "s.$i.$($rd.Name)"
                if ($r.ContainsKey($t)) { $w[$rd.Name] = $r[$t] } else { $complete = $false }
            }
            if (-not $complete) { continue }
            $st = Get-SampleState $cfg.Preset $w
            $states += $st
            $n++; if ($st.hfxo) { $k++ }
            if ($null -ne $st.constlat) { $cn++; if ($st.constlat) { $ck++ } }
        }
        $ci = Get-WilsonInterval $k $n
        $smp = [ordered]@{
            requested = $cfg.Samples; valid = $n
            hfxo_running = $k
            hfxo_duty = $(if ($n) { $k / [double]$n } else { $null })
            hfxo_ci95 = $ci
            constlat_on = $(if ($cn) { $ck } else { $null })
            constlat_valid = $cn
            constlat_ci95 = $(if ($cn) { Get-WilsonInterval $ck $cn } else { $null })
            interval_ms = $cfg.IntervalMs; jitter_ms = $cfg.JitterMs; seed = $cfg.Seed
            planned_sleep_ms = [int64](($plan.Sleeps | Measure-Object -Sum).Sum)
            first = $(if ($states.Count) { $states[0].raw } else { $null })
            last = $(if ($states.Count) { $states[-1].raw } else { $null })
        }
        $an.samples = $smp
    }

    # Diag snapshots.
    if ($cfg.Diag) {
        $d = [ordered]@{
            symbol = $cfg.Diag.Symbol
            address = ('0x{0:X8}' -f $cfg.Diag.Addr)
            size_bytes = $cfg.Diag.Bytes
            layout = $(if ($cfg.Diag.Layout) { 'radiant_power_diag v1 per power/a1 2189023 (TO RECONCILE WITH A1)' } else { 'raw' })
            snapshots = @()
        }
        $whens = @('before')
        if ($cfg.Samples -gt 1) { $whens += 'after' }
        foreach ($wn in $whens) { $d.snapshots += (Select-DiagSnapshot $r $wn $cfg.Diag) }
        $an.diag = $d
        # Second liveness signal: the firmware's own uptime. A connect that
        # reset the target shows as a small uptime in the first snapshot.
        $fs = @($d.snapshots | Where-Object { $_.ok -and $_.fields })
        if ($fs.Count -ge 1) {
            $an.liveness.notes += ("power_diag uptime at first snapshot: {0} ms" -f $fs[0].fields['uptime_ms'])
        }
        if ($fs.Count -ge 2) {
            $du = ([int64]$fs[-1].fields['uptime_ms'] - [int64]$fs[0].fields['uptime_ms'] + 4294967296L) % 4294967296L
            $an.liveness.notes += ("power_diag uptime advanced {0} ms across the session (planned sleeps {1} ms; the struct is republished once a second)" -f $du, [int64](($plan.Sleeps | Measure-Object -Sum).Sum))
            if ($du -eq 0) { $an.liveness.notes += 'power_diag uptime did NOT advance: firmware not running?' }
        }
    }
    return $an
}

function Write-PeekReport($an, $cfg) {
    Write-Host ''
    if (-not $an.parse_ok) {
        Write-Host "PARSE PROBLEM: $($an.parse_error)" -ForegroundColor Yellow
        Write-Host '  Results after the problem are missing; nothing below is shifted onto the wrong read.'
    }
    $lv = $an.liveness
    $col = 'Green'; if ($lv.verdict -ne 'running') { $col = 'Yellow' }
    Write-Host ("liveness: {0}  DHCSR {1}" -f $lv.verdict, ($lv.dhcsr -join ' -> ')) -ForegroundColor $col
    if ($lv.verdict -eq 'halted') { Write-Host '  The core is HALTED: every sample is one frozen state, not a duty cycle.' -ForegroundColor Yellow }
    if ($lv.verdict -eq 'unknown') { Write-Host '  DHCSR was not read at both ends; the target may not have been running.' -ForegroundColor Yellow }
    if ($null -ne $lv.counter) { Write-Host ("  RTC1.COUNTER {0} -> {1} (moved: {2})" -f $lv.counter[0], $lv.counter[1], $lv.counter_moved) }
    foreach ($nt in $lv.notes) { Write-Host "  note: $nt" }

    if ($an.samples) {
        $s = $an.samples
        Write-Host ''
        if ($s.valid -gt 0) {
            Write-Host ("HFXO running: {0}/{1} = {2:P1}  (95 % Wilson {3:P1} .. {4:P1})" -f $s.hfxo_running, $s.valid, $s.hfxo_duty, $s.hfxo_ci95[0], $s.hfxo_ci95[1])
            if ($null -ne $s.constlat_on) {
                Write-Host ("CONSTLAT on:  {0}/{1} = {2:P1}  (95 % Wilson {3:P1} .. {4:P1})" -f $s.constlat_on, $s.constlat_valid, ($s.constlat_on / [double]$s.constlat_valid), $s.constlat_ci95[0], $s.constlat_ci95[1])
            }
            Write-Host ("first sample: {0}" -f (($s.first.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))
            Write-Host ("last sample:  {0}" -f (($s.last.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))
            if ($s.requested -gt 1) {
                Write-Host ("delays uniform {0}..{1} ms (seed {2}), {3} ms planned sleep in total" -f [Math]::Max(1, $s.interval_ms - $s.jitter_ms), ($s.interval_ms + $s.jitter_ms), $s.seed, $s.planned_sleep_ms)
                Write-Host '  The interval assumes independent samples; long HFXO on/off runs make it optimistic.'
            }
        } else {
            Write-Host 'No valid samples.' -ForegroundColor Yellow
        }
    }

    if ($an.diag) {
        Write-Host ''
        Write-Host ("{0} @ {1}, {2} bytes ({3})" -f $an.diag.symbol, $an.diag.address, $an.diag.size_bytes, $an.diag.layout)
        foreach ($sn in $an.diag.snapshots) {
            $tag = 'ok'; if (-not $sn.ok) { $tag = 'NOT USABLE' }
            Write-Host ("  snapshot '{0}': {1}" -f $sn.when, $tag)
            foreach ($pb in $sn.problems) { Write-Host "    $pb" -ForegroundColor Yellow }
            if ($null -ne $sn.words) {
                for ($i = 0; $i -lt $sn.words.Count; $i += 4) {
                    $chunk = $sn.words[$i..([Math]::Min($i + 3, $sn.words.Count - 1))] | ForEach-Object { '{0:X8}' -f $_ }
                    Write-Host ("    +0x{0:X3}: {1}" -f (4 * $i), ($chunk -join ' '))
                }
            }
            if ($cfg.DoDecode -and $sn.fields) {
                foreach ($kv in $sn.fields.GetEnumerator()) {
                    $v = $kv.Value
                    $txt = "$v"
                    if ($v -eq $PowerDiag.NotMeasured) { $txt = 'NOT_MEASURED' }
                    elseif ($kv.Key -in @('magic', 'flags', 'constlat_stat')) { $txt = '0x{0:X8}' -f $v }
                    Write-Host ("    {0,-18} {1}" -f $kv.Key, $txt)
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Self-test: no probe, no JLink.exe.
# ---------------------------------------------------------------------------
function Invoke-SelfTest {
    $check = {
        param([string]$name, [bool]$cond)
        if ($cond) { Write-Host "  PASS  $name" } else { Write-Host "  FAIL  $name" -ForegroundColor Red; $script:stFail++ }
    }
    $script:stFail = 0

    # Wilson against hand-computed values.
    $w = Get-WilsonInterval 5 10
    & $check 'wilson 5/10 = 0.2366..0.7634' ([Math]::Abs($w[0] - 0.23659) -lt 1e-4 -and [Math]::Abs($w[1] - 0.76341) -lt 1e-4)
    $w = Get-WilsonInterval 0 10
    & $check 'wilson 0/10 = 0..0.2775' ($w[0] -eq 0 -and [Math]::Abs($w[1] - 0.27753) -lt 1e-4)
    $w = Get-WilsonInterval 100 100
    & $check 'wilson 100/100 upper 1, lower 0.963' ($w[1] -eq 1 -and [Math]::Abs($w[0] - 0.96301) -lt 1e-4)

    # Every preset's script passes, and starts with the guard.
    foreach ($pn in $Presets.Keys) {
        $cfg = @{ Preset = $pn; Device = $Presets[$pn].Device; Serial = '001057737173'; Speed = 4000; Samples = 5; IntervalMs = 300; JitterMs = 150; Seed = 7; NonSecure = $false
                  Diag = @{ Symbol = 'radiant_power_diag'; Addr = 0x20001000L; Words = 22; Bytes = 88; Layout = $true } }
        $pl = New-PeekPlan -Cfg $cfg
        $okRun = $true
        try { Assert-SafeJLinkScript $pl.Lines } catch { $okRun = $false }
        & $check "preset $pn script passes the whitelist" $okRun
        & $check "preset $pn first line is the guard" ($pl.Lines[0] -ceq $GuardLine)
        & $check "preset $pn second line is SelectEmuBySN" ($pl.Lines[1] -ceq 'SelectEmuBySN 001057737173')
        # Four randomised delays, then (diag dump present) one pad to the minimum span.
        $sl = @($pl.Lines | Where-Object { $_ -like 'sleep *' } | ForEach-Object { [int]($_ -split ' ')[1] })
        & $check "preset $pn has 4 randomised sleeps in 150..450" ($sl.Count -ge 4 -and @($sl[0..3] | Where-Object { $_ -lt 150 -or $_ -gt 450 }).Count -eq 0)
    }

    # The whitelist refuses what it must.
    $good = (New-PeekPlan -Cfg @{ Preset = 'nrf54l15'; Device = 'nRF54L15_M33'; Serial = $null; Speed = 4000; Samples = 2; IntervalMs = 300; JitterMs = 150; Seed = 1; NonSecure = $false; Diag = $null }).Lines
    $bad = @{
        'guard missing'          = @($good | Select-Object -Skip 1)
        'guard second'           = @(@('si SWD', $GuardLine) + @($good | Select-Object -Skip 2))
        'reset'                  = @($good[0..($good.Count - 2)] + @('r', 'q'))
        'halt'                   = @($good[0..($good.Count - 2)] + @('h', 'q'))
        'go'                     = @($good[0..($good.Count - 2)] + @('g', 'q'))
        'step'                   = @($good[0..($good.Count - 2)] + @('s', 'q'))
        'loadfile'               = @($good[0..($good.Count - 2)] + @('loadfile x.hex', 'q'))
        'erase'                  = @($good[0..($good.Count - 2)] + @('erase', 'q'))
        'w4'                     = @($good[0..($good.Count - 2)] + @('w4 0x20000000, 0', 'q'))
        'mem32 peripheral'       = @($good[0..($good.Count - 2)] + @('mem32 0x40000000, 0x1', 'q'))
        'mem32 flash'            = @($good[0..($good.Count - 2)] + @('mem32 0x00000000, 0x4', 'q'))
        'exec other'             = @($good[0..($good.Count - 2)] + @('exec SetResetType = 0', 'q'))
        'SelectEmu before guard' = @(@('SelectEmuBySN 1', $GuardLine) + @($good | Select-Object -Skip 1))
        'second connect'         = @($good[0..($good.Count - 2)] + @('connect', 'q'))
        'no q'                   = @($good[0..($good.Count - 2)])
        'TI device'              = @($good | ForEach-Object { if ($_ -like 'device *') { 'device CC2652R1F' } else { $_ } })
    }
    foreach ($name in $bad.Keys) {
        $refused = $false
        try { Assert-SafeJLinkScript $bad[$name] } catch { $refused = $true }
        & $check "whitelist refuses: $name" $refused
    }
    foreach ($dev in 'CC2652R1F', 'CC2652P', 'cc1352', 'nRF54L15_RV32', 'Cortex-M4') {
        $refused = $false
        try { Test-DeviceAllowed $dev } catch { $refused = $true }
        & $check "device refused: $dev" $refused
    }

    # Parser + analysis on canned Commander output (nrf54l15, 3 samples, no diag).
    $cfg = @{ Preset = 'nrf54l15'; Device = 'nRF54L15_M33'; Serial = $null; Speed = 4000; Samples = 3; IntervalMs = 300; JitterMs = 150; Seed = 3; NonSecure = $false; Diag = $null }
    $pl = New-PeekPlan -Cfg $cfg
    $canned = @(
        'J-Link>mem32 0xE000EDF0, 0x1', 'E000EDF0 = 01050003 ',
        '5010E408 = 00000001 00010001 ', '5010E520 = 00000001 ',
        'Sleep(212)', '5010E408 = 00000001 00000000 ', '5010E520 = 00000001 ',
        '5010E408 = 00000001 00010000 ', '5010E520 = 00000000 ',
        'E000EDF0 = 00040003 ', 'Script processing completed.'
    )
    $an = Get-PeekAnalysis $cfg $pl $canned
    & $check 'parse ok on canned output' ($an.parse_ok)
    & $check 'liveness running' ($an.liveness.verdict -eq 'running')
    & $check 'hfxo 2/3' ($an.samples.hfxo_running -eq 2 -and $an.samples.valid -eq 3)
    & $check 'constlat 2/3' ($an.samples.constlat_on -eq 2)

    # Halted core.
    $halted = $canned.Clone(); $halted[1] = 'E000EDF0 = 00030003 '
    $an = Get-PeekAnalysis $cfg $pl $halted
    & $check 'liveness halted from DHCSR S_HALT' ($an.liveness.verdict -eq 'halted')

    # One failed read: alignment refuses rather than shifting.
    $gap = @($canned[0..6] + @('Could not read memory.') + $canned[8..10])
    $an = Get-PeekAnalysis $cfg $pl $gap
    & $check 'a missing read is reported, not shifted' (-not $an.parse_ok -and $an.samples.valid -eq 2)

    # 52840 decode: HFCLK running from RC is not HFXO.
    $st = Get-SampleState 'nrf52840' @{ hfclk = @([int64]1, [int64]0x00010000) }
    & $check '52840 HFCLK on RC is not HFXO' (-not $st.hfxo -and $st.hfclk_running)
    $st = Get-SampleState 'nrf52840' @{ hfclk = @([int64]1, [int64]0x00010001) }
    & $check '52840 HFCLK on Xtal is HFXO' ($st.hfxo)

    # Diag decode: first attempt torn (odd seq), second consistent.
    $dcfg = @{ Symbol = 'radiant_power_diag'; Addr = 0x20001000L; Words = 22; Bytes = 88; Layout = $true }
    $wordsOdd = New-Object 'int64[]' 22; $wordsOk = New-Object 'int64[]' 22
    foreach ($arr in @($wordsOdd, $wordsOk)) { $arr[0] = 0x52504431L; $arr[1] = 1; $arr[2] = 0x58; $arr[4] = 12345; $arr[5] = 12000000 }
    $wordsOdd[3] = 41; $wordsOk[3] = 42
    $res = @{ 'diag.before.0.block' = $wordsOdd; 'diag.before.0.seq' = @([int64]42)
              'diag.before.1.block' = $wordsOk; 'diag.before.1.seq' = @([int64]42)
              'diag.before.2.block' = $wordsOk; 'diag.before.2.seq' = @([int64]42) }
    $sn = Select-DiagSnapshot $res 'before' $dcfg
    & $check 'diag torn attempt skipped, attempt 1 used' ($sn.ok -and $sn.attempt -eq 1 -and $sn.fields['uptime_ms'] -eq 12345)
    $wordsBad = $wordsOk.Clone(); $wordsBad[0] = 0x12345678L
    $res2 = @{ 'diag.before.0.block' = $wordsBad; 'diag.before.0.seq' = @([int64]42) }
    $sn = Select-DiagSnapshot $res2 'before' $dcfg
    & $check 'diag wrong magic refused' (-not $sn.ok)

    # Full diag plan round trip through the parser.
    $cfg = @{ Preset = 'nrf52840'; Device = 'nRF52840_xxAA'; Serial = $null; Speed = 4000; Samples = 2; IntervalMs = 300; JitterMs = 150; Seed = 9; NonSecure = $false; Diag = $dcfg }
    $pl = New-PeekPlan -Cfg $cfg
    $txt = New-Object System.Collections.Generic.List[string]
    foreach ($rd in $pl.Reads) {
        for ($i = 0; $i -lt $rd.Count; $i += 4) {
            $n = [Math]::Min(4, $rd.Count - $i)
            $vals = @()
            for ($j = 0; $j -lt $n; $j++) {
                $idx = $i + $j
                if ($rd.Tag -like 'diag.*.block') { $vals += ('{0:X8}' -f $wordsOk[$idx]) }
                elseif ($rd.Tag -like 'diag.*.seq') { $vals += '0000002A' }
                elseif ($rd.Tag -like 'ctr.0') { $vals += '00000100' }
                elseif ($rd.Tag -like 'ctr.1') { $vals += '00000200' }
                elseif ($rd.Tag -like 'live.*') { $vals += '01000003' }
                elseif ($idx -eq 1) { $vals += '00010001' } else { $vals += '00000001' }
            }
            $txt.Add(('{0:X8} = {1} ' -f ($rd.Addr + 4 * $i), ($vals -join ' ')))
        }
    }
    $an = Get-PeekAnalysis $cfg $pl $txt.ToArray()
    & $check 'diag plan parses (4 words per line)' ($an.parse_ok -and $an.diag.snapshots.Count -eq 2 -and $an.diag.snapshots[1].ok)
    & $check 'counter moved' ($an.liveness.counter_moved -eq $true)
    & $check '52840 hfxo 2/2' ($an.samples.hfxo_running -eq 2)

    # Security-prompt scan.
    $secOut = @('SEGGER J-Link Commander V9.66', 'Cortex-M33 identified.', 'Security extension: implemented',
                'Secure debug: enabled', 'E000EDF0 = 01040003 ')
    & $check 'security scan: routine connect banner passes' ((Find-SecurityPromptLines $secOut).Count -eq 0)
    foreach ($bad in 'Device is secured. Unsecure (mass erase)? (y/n)', 'APPROTECT is enabled',
                     'Erasing device...', 'ERASEALL', 'Unlocking device', 'Debug access is not secured') {
        & $check "security scan flags: $bad" ((Find-SecurityPromptLines @($secOut + $bad)).Count -eq 1)
    }
    & $check 'stdin refusals are all n' (@($StdinRefusals | Where-Object { $_ -ne 'n' }).Count -eq 0 -and $StdinRefusals.Count -ge 16)

    # Saved-answer check.
    $okEntries = @('HKEY_CURRENT_USER\Software\SEGGER\J-Link: NotifyOnSecurityUpdate = 1',
                   'HKEY_CURRENT_USER\Software\SEGGER\J-Link: StartMinimized = 1',
                   'C:\x\JLinkDLL.ini: NRF54L15_M33')
    & $check 'saved settings: this box''s kind of entries pass' ((Find-AutoAcceptEntries $okEntries).Count -eq 0)
    foreach ($bad in 'J-Link: AutoUnsecure = 1', 'J-Link: RememberAnswer = 1', 'J-Link: DontAskAgain = 1',
                     'J-Link: AutoConfirm = 1', 'JLinkSettings.ini: SuppressUnlockDialog = 1') {
        & $check "saved settings flags: $bad" ((Find-AutoAcceptEntries @($okEntries + $bad)).Count -eq 1)
    }

    # Diag dump with short sleeps gets padded to the minimum span.
    $short = New-PeekPlan -Cfg @{ Preset = 'nrf54l15'; Device = 'nRF54L15_M33'; Serial = $null; Speed = 4000; Samples = 3; IntervalMs = 100; JitterMs = 50; Seed = 5; NonSecure = $false; Diag = $dcfg }
    $padOk = $true
    try { Assert-SafeJLinkScript $short.Lines } catch { $padOk = $false }
    & $check "diag span padded to >= $DiagMinSpanMs ms" ($padOk -and ($short.Sleeps | Measure-Object -Sum).Sum -ge $DiagMinSpanMs)

    Write-Host ''
    if ($script:stFail) { Write-Host "SELF-TEST: $($script:stFail) FAILED" -ForegroundColor Red; exit 1 }
    Write-Host 'SELF-TEST: all passed' -ForegroundColor Green
    exit 0
}

# ===========================================================================
# Main.
# ===========================================================================
if ($SelfTest) { Invoke-SelfTest }

if (-not $Device) {
    if ($Preset) { $Device = $Presets[$Preset].Device }
}
Test-DeviceAllowed $Device
if ($Preset -and ($Device -notmatch $Presets[$Preset].DevicePattern)) {
    throw "Device $Device does not match preset $Preset (expects $($Presets[$Preset].DevicePattern)); its register map would be wrong."
}
if ($NonSecure -and $Preset -ne 'nrf54l15') { throw '-NonSecure applies to -Preset nrf54l15 only.' }
if ($Serial -and $Serial -notmatch '^[0-9]{1,12}$') { throw "-Serial must be the probe's decimal serial number." }
if ($Samples -lt 1 -or $Samples -gt 20000) { throw '-Samples must be 1..20000.' }
if ($Samples -gt 1 -and -not $Preset) { throw '-Samples needs -Preset (what would it sample?).' }
if ($IntervalMs -lt 1 -or $JitterMs -lt 0 -or $JitterMs -ge $IntervalMs) { throw 'Need IntervalMs >= 1 and 0 <= JitterMs < IntervalMs.' }
if ($Samples -gt 1 -and (2 * $JitterMs) -lt 247) {
    Write-Warning ("Delay range {0} ms is narrower than one 246 ms ANT+ slot: samples can phase-lock to the slot grid." -f (2 * $JitterMs))
}
if ($Seed -eq 0) { $Seed = (Get-Random -Minimum 1 -Maximum 2147483647) }

# Symbol: from the ELF, or an explicit address.
$diag = $null
if ($ElfPath -or $SymbolAddress) {
    if ($ElfPath) {
        if (-not (Test-Path $ElfPath)) { throw "No ELF at $ElfPath" }
        if (-not (Test-Path $NmExe)) { throw "No nm at $NmExe (pass -NmExe)." }
        $nmOut = & $NmExe -S --defined-only $ElfPath
        if ($LASTEXITCODE -ne 0) { throw "nm failed on $ElfPath" }
        $hit = $null
        foreach ($l in $nmOut) {
            $m = [regex]::Match($l, '^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([A-Za-z])\s+(\S+)$')
            if ($m.Success -and $m.Groups[4].Value -ceq $Symbol) { $hit = $m; break }
        }
        if (-not $hit) { throw "Symbol $Symbol not found (with a size) in $ElfPath. Is CONFIG_RADIANT_POWER_DIAG=y in that image?" }
        if ('bBdD' -notmatch [regex]::Escape($hit.Groups[3].Value)) { throw "Symbol $Symbol is type '$($hit.Groups[3].Value)', not RAM data (b/B/d/D)." }
        $symAddr = [Convert]::ToInt64($hit.Groups[1].Value, 16)
        $symBytes = [int][Convert]::ToInt64($hit.Groups[2].Value, 16)
    } else {
        $symAddr = [Convert]::ToInt64(($SymbolAddress -replace '^0x', ''), 16)
        $symBytes = $SymbolSize
        if ($symBytes -le 0) { throw '-SymbolAddress needs -SymbolSize (bytes).' }
    }
    $words = [int][Math]::Ceiling($symBytes / 4.0)
    if (-not (Test-ReadAddressAllowed $symAddr $words) -or $symAddr -lt 0x20000000L) {
        throw ('{0} at 0x{1:X8} ({2} bytes) is not in SRAM; refusing to read it.' -f $Symbol, $symAddr, $symBytes)
    }
    $layout = ($Symbol -ceq 'radiant_power_diag')
    if ($layout -and $symBytes -ne $PowerDiag.Size) {
        Write-Warning ("{0} is {1} bytes in the ELF but the layout table says {2}: reconcile the table (TO RECONCILE WITH A1). Dumping raw." -f $Symbol, $symBytes, $PowerDiag.Size)
        $layout = $false
    }
    $diag = @{ Symbol = $Symbol; Addr = $symAddr; Words = $words; Bytes = $symBytes; Layout = $layout }
} elseif ($Decode) {
    throw '-Decode needs -ElfPath or -SymbolAddress.'
}
if (-not $Preset -and -not $diag) { throw 'Nothing to read: pass -Preset and/or -ElfPath/-SymbolAddress.' }

$cfg = @{
    Preset = $Preset; Device = $Device; Serial = $Serial; Speed = $Speed
    Samples = $Samples; IntervalMs = $IntervalMs; JitterMs = $JitterMs; Seed = $Seed
    NonSecure = [bool]$NonSecure; Diag = $diag; DoDecode = [bool]$Decode
}
$plan = New-PeekPlan -Cfg $cfg

# Safety check #1: the lines as built.
Assert-SafeJLinkScript $plan.Lines

if ($DryRun) {
    Write-Host "# jlink_peek dry run: $($plan.Lines.Count) lines, $($plan.Reads.Count) reads, seed $Seed (whitelist passed)"
    $saved = Get-JLinkSavedSettings
    $auto = Find-AutoAcceptEntries $saved
    Write-Host ("# J-Link saved settings: {0} entries read, {1} look like a remembered unsecure/erase answer{2}" -f $saved.Count, $auto.Count, $(if ($auto.Count) { ' - a live run would REFUSE: ' + ($auto -join '; ') } else { '' }))
    Write-Host "# JLink.exe stdin would carry $($StdinRefusals.Count) 'n' answers."
    $plan.Lines | ForEach-Object { Write-Output $_ }
    exit 0
}

$hostStart = [DateTimeOffset]::UtcNow
if ($ParseLog) {
    if (-not (Test-Path $ParseLog)) { throw "No log at $ParseLog" }
    $output = Get-Content -Path $ParseLog
    $logPath = (Resolve-Path $ParseLog).Path
    $hostEnd = $hostStart
} else {
    if (-not (Test-Path $JLinkExe)) { throw "No JLink.exe at $JLinkExe. Install SEGGER J-Link, or pass -JLinkExe <path>." }
    $saved = Get-JLinkSavedSettings
    $auto = Find-AutoAcceptEntries $saved
    if ($auto.Count -gt 0) {
        throw ("Refusing to run: J-Link's saved settings carry what looks like a remembered answer " +
               "or auto-accept (an unsecure prompt could be answered YES = mass erase):`n  " + ($auto -join "`n  "))
    }
    Write-Host ("J-Link saved settings: {0} entries read, none look like a remembered unsecure/erase answer." -f $saved.Count)
    $tmp = [System.IO.Path]::GetTempPath()
    $scriptPath = Join-Path $tmp ("jlink_peek_{0}.jlink" -f $PID)
    $logPath = Join-Path $tmp ("jlink_peek_{0}.log" -f $PID)
    Set-Content -Path $scriptPath -Value $plan.Lines -Encoding ascii
    # Safety check #2: the file exactly as JLink.exe will read it.
    Assert-SafeJLinkScript (Get-Content -Path $scriptPath)

    $est = [Math]::Round((($plan.Sleeps | Measure-Object -Sum).Sum) / 1000.0, 1)
    Write-Host ("Reading {0} on {1} ({2} samples, ~{3} s of sleeps); target is not reset or halted." -f $(if ($Preset) { $Preset } else { $Symbol }), $Device, $Samples, $est)
    # Refusals on stdin: a console unsecure/erase prompt reads "n".
    $output = $StdinRefusals | & $JLinkExe -NoGui 1 -CommanderScript $scriptPath
    $code = $LASTEXITCODE
    $hostEnd = [DateTimeOffset]::UtcNow
    # Set-Content, not '>': PowerShell 5.1 redirection writes UTF-16.
    Set-Content -Path $logPath -Value $output -Encoding ascii
    Remove-Item $scriptPath -ErrorAction SilentlyContinue
    if ($code -ne 0) {
        Write-Warning @"
J-Link exited with $code (raw output: $logPath).
If it reported a firmware update, the probe is now in its bootloader
(VID_1366&PID_0101); give it ~2 minutes to re-enumerate before replugging.
"@
    }
}

$an = Get-PeekAnalysis $cfg $plan $output
$secHits = Find-SecurityPromptLines $output
$an['security_lines'] = $secHits
# Host times bracket the session (connect included). A re-parsed log has
# none: the times of this run would be a lie about that one.
$an['host_start_unix'] = $null
$an['host_end_unix'] = $null
if (-not $ParseLog) {
    $an['host_start_unix'] = $hostStart.ToUnixTimeMilliseconds() / 1000.0
    $an['host_end_unix'] = $hostEnd.ToUnixTimeMilliseconds() / 1000.0
}
$an['raw_log'] = $logPath
if (-not $ParseLog -and $Samples -gt 1) {
    $an.samples['realised_mean_interval_ms'] = [Math]::Round(($hostEnd - $hostStart).TotalMilliseconds / $Samples, 1)
}
Write-PeekReport $an $cfg
Write-Host ''
Write-Host "raw J-Link output: $logPath  (re-analyse with -ParseLog and the same arguments)"
if ($an.samples -and $an.samples.Contains('realised_mean_interval_ms')) {
    Write-Host ("realised mean interval {0} ms per sample (session wall time incl. connect / requested {1} ms)" -f $an.samples.realised_mean_interval_ms, $IntervalMs)
}

if ($Json) {
    $an | ConvertTo-Json -Depth 8 | Set-Content -Path $Json -Encoding ascii
    Write-Host "JSON: $Json"
}

if ($secHits.Count -gt 0) {
    Write-Host ''
    Write-Host '!!! J-LINK MENTIONED SECURITY / ERASE / UNLOCK - STOP AND READ THIS !!!' -ForegroundColor Red
    foreach ($h in $secHits) { Write-Host "    $h" -ForegroundColor Red }
    Write-Host '    The target may be read-protected (APPROTECT) or J-Link may have offered to unsecure it.' -ForegroundColor Red
    Write-Host '    Unsecuring is a MASS ERASE. Check the board still runs its image before anything else.' -ForegroundColor Red
    Write-Host "    Raw output: $logPath" -ForegroundColor Red
    exit 3
}

$bad = (-not $an.parse_ok) -or ($an.liveness.verdict -ne 'running')
if ($an.diag) { foreach ($sn in $an.diag.snapshots) { if (-not $sn.ok) { $bad = $true } } }
if ($bad) { exit 2 }
exit 0
