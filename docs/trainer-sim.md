# apps/trainer: FE-C trainer simulator with programmable ACK faults

`apps/trainer` is an ANT+ FE-C bike trainer (equipment type 25) on a DK. It has
two jobs.

- **A usable example.** A spec-conformant controllable trainer. It sends pages
  16, 25 and 17 in SS10.1's interleave, page 54 declares basic resistance,
  target power and simulation (maximum resistance 2000 N), and every control
  page gets a page 71 answer. A simulated rider supplies power and cadence,
  set from the shell. Zwift or a head unit pairs with it as a trainer.
- **A test instrument.** Its acknowledgements can be delayed, jittered, dropped
  after the command is applied (MUTE), or never received at all (DEAF), in
  bursts or in busy windows. It also records the ground truth for every
  acknowledged packet it receives. Any other radiant master acknowledges at
  exactly data `t_sync` + 1560 us, the centre of the dongle's 1560 ± 250 us
  window. Pairing a dongle with one of those only reproduces the case that
  already passes.

It has no BLE, and that is deliberate. MPSL would wrap the ACK's transmit
instant in arbitration and a follow-on reserve, so the image would end up
measuring the arbiter. `CMakeLists.txt` refuses `CONFIG_BT` and the MPSL gate.

## Build and flash

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -s <repo>\apps\trainer -d <repo>\build\trainer_l15 -- -DRADIANT_BACKEND=nrf
Pop-Location
.\scripts\flash_sim_jlink.ps1 -HexPath build\trainer_l15\trainer\zephyr\zephyr.hex -Device nRF54L15_M33 -Serial <probe SN>
```

Before trusting the image, read back `build\trainer_l15\trainer\zephyr\.config`.
It must contain `CONFIG_RADIANT_BACKEND_NRF=y`, `CONFIG_RADIANT_ACK_FAULTS=y`,
`# CONFIG_MPSL is not set` and `# CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL is not set`.
CI's `build-trainer` job asserts the same lines. Default device number: 4244.

The shell is on the DK's console VCOM at 115200 and needs DTR asserted. The
port moves between sessions (it was COM3 on 2026-11-04), so find it by sending
`ackfault show` to each candidate.

## Shell

| Command | What it does |
|---|---|
| `rider [power=W] [cadence=RPM]` | Sets the simulated rider (default 200 W, 90 rpm). With no arguments it prints the model: mode, road, speed, power. |
| `ackfault show` | Prints the fault parameters. |
| `ackfault set k=v ...` | Sets parameters: `offset_us` (signed), `jitter_us`, `mute_pm`, `deaf_pm`, `p_bad`, `p_good`, `bad_action=mute\|deaf`, `busy_ms`, `seed`. All or nothing, and decision state is not reset. |
| `ackfault preset NAME` | `ideal`, `late300`, `late-edge` (+250 us), `jitter` (±200 us), `lossy25` (25 % MUTE), `wahoo`. Installs the parameters and resets decision state. |
| `ackfault stats` | Decision counters, plus radiant's own `acks_sent` and `too_early`. |
| `ackfault dump` | The ground-truth ring, one `AF ...` line per packet, between `AF-BEGIN` and `AF-END`. |
| `ackfault reset` | Clears the ring and re-seeds the generator. Parameters stay. |

**`wahoo` is a placeholder equal to `ideal`.** It stays that way until the
sniffer calibration measures the real trainer: its data→ACK `dt` distribution
and the share of commands that get an ACK on the air.

## The fault model

The trainer makes one decision per received acknowledged packet, in radio
interrupt context. Rules apply in this order (`src/trainer_ackfault.h`):

1. **Busy.** Within `busy_ms` of the last command it applied, MUTE. A busy
   MUTE does not restart the window.
2. **Burst.** A Gilbert chain steps once per packet: GOOD goes to BAD with
   probability `p_bad`, BAD goes to GOOD with `p_good` (both per-mille). In BAD
   the packet gets `bad_action`. The mean burst lasts 1000/`p_good` packets.
3. **DEAF** with probability `deaf_pm`.
4. **MUTE** with probability `mute_pm`.
5. Otherwise **ACK** at the normal instant, plus `offset_us`, plus a uniform
   draw in ±`jitter_us`.

MUTE means "applied, ACK lost": the command reaches the trainer, page 71
advances, and nothing is sent back. DEAF means "never heard": nothing reaches
the trainer at all. From the controller's side the two differ only in whether
page 71's sequence advances, which is how a controller could tell them apart.

**Too early.** If an offset puts the ACK before the radio can arm it, no ACK
is sent. radiant counts it in `too_early`, and the record shows `act=ack tx=0`.
The trainer never sends an early ACK in place of an unreachable one.

**Reproducibility.** The generator is xorshift32 from `seed`. `reset` and
`preset` restart it, so a run is reproducible from its parameters alone.

## The link-layer hook

`CONFIG_RADIANT_ACK_FAULTS` (radiant/Kconfig, default n) adds an optional
`reply_plan` op to `struct radiant_transfer_ops`. `radiant_transfer_on_data()`
asks the op what to do once a packet is known to be acknowledgeable. A packet
radiant refuses for its own reasons is never offered to the op. With the op
NULL, or the symbol off, the path is unchanged. The app reaches the op through
`antr_reply_plan_hook_set()` and `antr_reply_stats_get()` in
`apps/common/ant/ant_radio.h`. Both exist only under the symbol.

**This symbol never goes into a dongle image.** Every dongle row in
`.github/workflows/release.yml` asserts `# CONFIG_RADIANT_ACK_FAULTS is not set`.
`nm` on a dongle ELF finds no `reply_plan` and no `antr_reply_*` symbol.

## Measuring a dongle: tools/fec_ack_sweep.py

The dongle under test opens a pinned bidirectional slave to the trainer and
sends page 51 at 1 Hz. Every command carries a unique grade, which serves as
its tag. The tool joins each command's outcome at the dongle to the trainer's
record of the same tag:

| Trainer | Dongle | Verdict |
|---|---|---|
| ACK on the air | completed | `ok` |
| ACK on the air | failed | `window_missed`, the number being measured |
| ACK planned, too early to send | failed | `sim_no_tx` |
| MUTE or DEAF | failed | `expected` |
| no record | failed | `not_heard` (lost on the way in) |
| no ACK on the air | completed | **`IMPOSSIBLE`**: instrument fault, so the run stops |

```powershell
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
# 1. lone-sim control: no originator anywhere, must record 0 frames
& $py tools\fec_ack_sweep.py --sim-port COM3 --lone-seconds 60
# 2. ideal baseline
& $py tools\fec_ack_sweep.py --sim-port COM3 --serial 3D55F77818BE772A --preset ideal --count 300
# 3. window edges
& $py tools\fec_ack_sweep.py --sim-port COM3 --serial 3D55F77818BE772A --offset=-400:600:50 --count 30 --json sweep.json
# 4. mode signatures
& $py tools\fec_ack_sweep.py --sim-port COM3 --serial 3D55F77818BE772A --preset lossy25 --count 100
& $py tools\fec_ack_sweep.py --sim-port COM3 --serial 3D55F77818BE772A --set deaf_pm=250 --count 100
& $py tools\fec_ack_sweep.py --sim-port COM3 --serial 3D55F77818BE772A --set busy_ms=1500 --count 100
```

Run the steps in order. Each one gates the next. If the ideal baseline shows
anything other than about 0 % `window_missed`, the instrument or the hook is
at fault, and nothing measured after it means anything.

## Measured, 2026-11-04

Rig: the trainer on an nRF54L15 DK, and the DUT on a PCA10059 (`3D55F77818BE772A`)
running this tree's dongle image. The two were side by side on the bench.

| Step | Result |
|---|---|
| Lone-sim control, 60 s | 0 acked frames. PASS |
| Ideal baseline, 300 commands | 299 `ok`, 1 `window_missed` (0.3 %, at the room's loss floor), 0 `not_heard` |
| Window edges, 10 us steps, 20 commands each | Caught at **−260 … +240 us** around the normal 1560 us instant, 20/20 at every point inside. Missed 20/20 at −270 and at +250. |
| `lossy25`, 100 commands | 21 MUTE → 21 `expected`, 0 `window_missed`. Page 71 advanced 99 (it tracks all 100 applied commands) |
| `deaf_pm=250`, 100 commands | 21 DEAF → 21 `expected`. Page 71 advanced 78 (79 applied): **it skips exactly the unheard commands** |
| `busy_ms=1500`, 100 commands at 1 Hz | ACK and MUTE alternate, 50/50 |

What follows from these measurements:

- **The window is about 510 us wide**, offset about 10 us early relative to
  the paper's ±250. Both edges are a step function at 10 us resolution: there
  is no gradual zone where loss creeps in. A trainer whose ACK lands more than
  ~245 us late is missed every time, not some of the time.
- **Page 71 discriminates the two failure modes from the controller's side.**
  When a command fails and page 71's sequence still advances past it, the
  trainer applied the command and the ACK was lost (MUTE, or a late ACK).
  When the sequence does not advance, the trainer never heard it (DEAF).
  This is the check to run on the 11-03/11-04 Zwift pcaps (plan Phase 6, the
  no-cost first check).

Not run yet: Zwift on the sim (Phase 5 step 5) and the sniffer calibration
against the real trainer (Phase 6). Both need a person at the bench.

## Tests

- `radiant/tests` with the symbol off (`radiant.unit`) and on
  (`radiant.unit.ack_faults`, which adds the seven `reply_plan` cases in
  `test_transfer.c`).
- `radiant/tests/src/test_profile_fec_tx.c`: page 25, pages 50/51 on request,
  page 55 in full, and the trainer interleave.
- `apps/trainer/tests/model`: the physics against hand-computed forces, and
  the fault model's statistics over 10,000 draws.
- `tools/test_fec_ack_sweep.py`: the join and the classifier.

On Windows, the C suites run on a DK: `scripts\run_ztest_hw.ps1 -App <dir>`,
with `-ExtraConfig CONFIG_RADIANT_ACK_FAULTS=y` for the second build.
