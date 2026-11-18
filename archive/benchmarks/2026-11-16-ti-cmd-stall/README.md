# TI LaunchXL command stall under the C2 load (2026-11-16)

Follow-up to `../2026-11-16-pass-breakdown/ti/` (the C2-TI legs that died on "no answer"). Branch
`fix/ti-cmd-stall` in `C:\wt_tistall`, fix commit **684a4a8** on top of master 3292198. Not merged or pushed.

## Result

**Root cause: the TI drops host→TI bytes because its UART receiver overruns.** Zephyr's `uart_cc13xx_cc26xx`
driver turns the UART FIFOs off (`UARTFIFODisable()` in `uart_cc13xx_cc26xx_configure()`, "character mode"),
so RX is a 1-byte holding register. At 57600 baud a byte lands every 174 µs. Any span longer than about one
byte time in which the ANT UART ISR cannot run loses a byte. The ISR is priority 0 and the RF callbacks run in
the TI SwiP at the lowest priority, so it is something that masks interrupts (irq_lock / `HwiP_disable`), or
an equal-priority ISR, that holds it off. That source was **not identified** (see "Open").

- **Proof.** An ad hoc build (`images/ti_diagerr.patch`, not committed) counts UART `RSR.OE` into the unused
  0xF6 `rx_backpressure` field. Every overrun came with a frame lost: `host_csum` and `host_desync` moved in
  step with `OE`. TI→host was clean: 0 host-side checksum errors in every run. 0xF6 `tx_drops` and
  `tx_timeout` stayed 0.
- **Why one lost byte stalls the host for seconds or minutes.** The bridge parser accepted any LEN up to 255.
  - Lose a LEN byte: the message id is taken as the length (0x42 → 66 bytes). The parser then swallows the
    host's next several commands *and every resend of them*, until enough bytes have arrived. A tool that
    sends a few bytes and waits seconds for an answer stays "ignored" for minutes, which is the
    `aborted_leg1c_cmd_stall` symptom (reset ignored for >2 min while events streamed).
  - Lose the last byte of a short frame: the NEXT frame's SYNC becomes its checksum. `leg4_preB1` fits this
    exactly. The scan's UNASSIGN (`A4 01 41 00 E4`, sent without waiting for its reply) lost a byte → 1
    `host_csum`. The ASSIGN that followed was consumed as 7 desync bytes → `host_desync` 7. The resent
    ASSIGN was refused with code 21, because the UNASSIGN never ran. The first ASSIGN was **not** processed
    with a lost reply, as the earlier README inferred. It was never parsed.
  - The command thread never blocked, and no reply was lost.
- **Fix (684a4a8).**
  - `apps/common/ant_uart_transport.c`: on a `ti,cc13xx-cc26xx-uart` node, the FIFO is turned on after the
    driver's init. RX triggers at 8 bytes, plus the receive-timeout interrupt for shorter frames, which gives
    ~24 bytes ≈ 4 ms of ISR-latency tolerance. The ISR reads hardware state directly because the driver's
    `irq_is_pending()` ignores the receive-timeout interrupt, which would give an endless ISR re-entry.
    - **Correction (review, follow-up commit 343aef3):** 684a4a8's comment also said `irq_tx_ready()` means
      "line idle". That is wrong. In NCS v3.4.0 `irq_tx_ready()` is the masked TX interrupt status
      (`UARTSpaceAvail()` in v3.2.4); "line idle" (`!UARTBusy()`) is `irq_tx_complete()`. TX needed no
      workaround. The ISR's direct check (TX unmasked and FIFO space) is correct either way.
    - Every other UART (the nRF54L15) keeps the old ISR, refactored into the same two helpers.
  - `apps/common/ant_serial_bridge.c`: a LEN above `ANTW_MAX_SIZE_VALUE` (38) is a lost byte. The parser now
    counts it as desync and hunts for the next SYNC, so it no longer eats the host's next commands. This
    limits the damage if a byte is ever lost again.

## UART TX budget (the brief's hypothesis, ruled out)

- **Link capacity.** 57600 8N1 carries 5760 B/s.
- **Frame size.** The tools enable LIB_CONFIG 0xE0 (channel id + RSSI + timestamp). A broadcast data frame is
  SYNC + LEN + ID + ch + 8 data + flag + 4 (id) + 3 (RSSI) + 2 (timestamp) + CSUM = **23 bytes**. A channel
  event is 7 bytes.
- **Computed load.** 9 tracked channels × ~4.05 Hz × 23 B ≈ 840 B/s, plus the scan channel's sightings
  (~4/s × 23 ≈ 100 B/s) ≈ **940 B/s ≈ 16 %** of the link. The brief's 4.7 kB/s was an arithmetic slip:
  9 × 4 × 13 = 468 B/s.
- **Measured** (framed bytes the host received, `rx_stats` in each run): **938 B/s** with 9 tracked + reopen
  (fixed image) and 937 B/s with the scan alone. The TX queue never saturates (0xF6 `tx_high_water` 0,
  `tx_drops` 0, `tx_timeout` 0). Native USB vs UART is not about TX bandwidth: the nRF parts receive through
  DMA/USB and cannot overrun like a 1-byte holding register.

## Bisection (unfixed vs fixed, 120 s each, `stall_run.py`)

`stall_run.py` drives the TI alone and never gives up: each command gets up to 6 resends, with latency and
outcome logged, and 0xF6 is read every 10 s. "Probe" is a 5 Hz stream of harmless SET_CHANNEL_PERIOD commands
on tracked channels. They are answered (code 21 on an open channel, which is fine), so host→TI loss can be
measured without any scan reopen.

| Run | Image | Load | Cmds | Resent | Never answered | UART OE | host_csum | host_desync |
|---|---|---|---|---|---|---|---|---|
| A | unfixed+OE counter | 9 tracked + 4.5 s reopen | 175 | 6 | 2 | **5** | 4 | 21 |
| B | unfixed+OE counter | 9 tracked, **no reopen**, probe 5 Hz | 278 | 10 | 3 | **9** | 8 | 41 |
| C | unfixed+OE counter | **0 tracked**, reopen only (937 B/s TX) | 191 | 0 | 0 | 0 | 0 | 0 |
| D | unfixed+OE counter | 4 tracked + reopen | 208 | 1 | 0 | 1 | 1 | 7 |
| E | fixed+OE counter | 9 tracked + reopen | 245 | 0 | 0 | **0** | 0 | 0 |
| F | fixed+OE counter | 9 tracked, probe 5 Hz | 551 | 0 | 0 | **0** | 0 | 0 |

- **The scan reopen is not needed** (B). It only supplies the host traffic.
- **The trigger is tracked-channel load,** not TX volume. C sends as many bytes to the host as E and never
  overruns. 4 tracked channels overrun about 1/5 as often as 9 (D vs A).
- Unfixed: about 3–5 % of host commands lose a byte at 9 tracked channels. Fixed: 0 of 796.

## Acceptance: full C2 workload, 300 s (`leg_ti.py --trainer`, Pro Micro same-window control)

| Leg | TI image | TI scan child | Resends | TI 0xF6 after |
|---|---|---|---|---|
| `raw/accept_A_fix` | fix 684a4a8 (`images/ti_fix.hex` 28d1a257…) | **rc 0, 300 s, 66 opens** | **0** | host_csum 0, host_desync 0, tx_drops 0 |
| `raw/accept_B_unfixed` | B1 -Os fa85be8b… (archive image) | **rc 1: stalls at 25.9 s (ASSIGN) and 40.0 s (CLOSE), each answered on the 2nd resend; then SEARCH_TIMEOUT unanswered for 4 resends from 49.6 s, child dies** | 16 lines | **unreadable**: ant_diag, ant_health and the end-of-leg reset (`ti_reset_end` rc 1) all unanswered, as in `aborted_leg1c` |

- Masters were 8/8 before and after in each leg. Same masters, trainer, Pro Micro control and runner; legs 8
  min apart (A 13:04, B 13:12). The Pro Micro ran all 300 s in both.
- After B the TI was reflashed with `ti_fix.hex` (`flash_fix_final.log`, probe PASS) and is left on it.
- **A tracked loss (TI):** masters 1.0–5.2 %, trainer FE-C 55 %. This is not a loss A/B; the Pro Micro numbers
  for the same window are in `raw/accept_A_fix/scan_pm.txt`.

## Open

- **What holds the UART ISR off for over ~170 µs is not identified.** It scales with the number of tracked
  channels and is absent with the scan alone. Candidates, all unmeasured:
  - the TI RF driver's `HwiP_disable()` sections around `RF_scheduleCmd` (submit-policy queue walk, then
    dispatch);
  - `RF_cancelCmd`/`RF_flushCmd`;
  - equal-priority (0) ISRs such as the RTC/sys-clock ISR running ClockP expiries.
  
  The FIFO gives 4 ms of headroom, so this is no longer a host-link defect. But if such a span exists, it
  could matter for radio timing. Measuring it needs a DWT-timed instrument on the irq-lock paths.
- **No automated test.** No native_sim or ztest target compiles `ant_serial_bridge.c` or
  `ant_uart_transport.c`, and the FIFO path is CC26xx-only hardware behaviour. Compile-checked:
  - TI LaunchXL diag build (`build/ti_fix`);
  - nRF54L15 UART dongle (`build_all -Only radiant_dongle_nrf54l15dk.hex`), which exercises the generic ISR
    branch.
  
  The cc2652p_dongle board uses the same compatible and was not built.
- **A permanent OE counter in 0xF6** would have made this a one-read diagnosis. It needs a new appended field
  plus a `tools/ant_health.py` change, so it is left as an option.

## Files

- `stall_run.py`: the bisection runner.
- `flash_ti.ps1`: the archive's flash wrapper, pointed at `C:\wt_tistall`.
- `flash_*.log`: md5, dslite verify and probe for each flash.
- `images/`:
  - `ti_diagerr.hex` 493be761… (unfixed + OE counter, patch `ti_diagerr.patch`);
  - `ti_fixdiag.hex` 95ae63a3… (fix + OE counter, `ti_fixdiag.patch`);
  - `ti_fix.hex` 28d1a257… (the commit as built, `-Os` diag config `ti_health.conf;ti_diag.conf`).
- `raw/<run>/`: `console.txt`, `run.json` (every command and health read) and `summary.json` per bisection
  run, plus the acceptance legs' full `leg_ti.py` output. `raw/aborted_accept_A_relpath/` is a first attempt
  that aborted on a relative-path bug in my invocation (master count could not write its JSON). No bench
  meaning.
