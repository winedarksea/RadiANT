# Gate G1 H3: what an HFXO restart changes at the TI receiver, 2026-11-18

Follow-up to `2026-11-18-power-g1-diag/` (B = `RADIANT_NRF_LOW_POWER`, HFXO off between slots, gives the TI
receiver ~1.7x more single-bit CRC failures than A, HFXO always on). Branch `diag/g1-h3` (worktree `C:\wt_g1_h3`)
from e58b047. Diagnostic code only, nothing merged.

## Verdict

> **2026-11-19, owner:** G1 is PASS with accepted TI-only excess (the residual ~0.3-0.4 pp TI RX_FAIL; see
> `2026-11-19-power-g1-rejudge/`). This diagnosis is left as written.

1. **Every repaired TI error, in A and in B, is the last bit or the last-but-one bit of the frame on air, and it is
   always a transmitted 0 received as 1.**
   - Rounds 2 and 3 used the CRC-bit-index receiver: 269 repaired-and-delivered frames. 255 had their flipped bit in
     the CRC field, and 252 of those were in syndrome bits 0-1 (the last two bits on air). Of the other 17, 14 were
     body bits and 3 were other CRC bits; 13 of the body bits fell in the two interference-hit arms, Q3_B and T2_B.
     In the eight clean arms, 191 of 192 were in the last two bits.
   - Direction: received 1 where 0 was sent in 257 of 269. Of the 12 exceptions, 11 are in Q3_B and T2_B, and one
     is in T1_Bt50.
   - Round 1 (position receiver): 122 of 129 in the CRC field, and 126 of 129 received as 1.
   - So B does not add a new kind of error. It raises the rate of the one error A already has, which is the frame's
     final 0 bit read as 1.
2. **Nothing visible in the registers changes across a restart.**
   - Every FICR TRIMCNF target in the RADIO (0x5008A738..0x5008A8BC, 21 words) reads the same in A (HFXO held) and
     in B (asleep and awake), and matches FICR DATA.
   - HFXO INTCAP (OSCILLATORS 0x5012071C = 0x2D) is retained.
   - The hidden XO status word 0x50120700 reads 0x639 after every one of 24 sampled B starts, the same as in A.
   - So (b), a lost trim or load-cap setting, is rejected as far as registers can show. The read-only peeks used a
     worktree-only extension of `scripts/jlink_peek.ps1` (`-RawReads`, whitelisted to FICR, RADIO, CLOCK/POWER and
     OSCILLATORS).
3. **TX truncation is rejected.**
   - `g1h3_tx` (`CONFIG_RADIANT_DIAG_TX_PHYEND`): in A and in B alike, PHYEND (last bit on air) was already set
     every time the END interrupt disabled the radio (B 85/85, A 86/86).
   - The END ISR ran 116-133 µs after ADDRESS in both images. The body plus CRC ends 96 µs after ADDRESS, so there
     is 20-37 µs of carrier after the last bit.
   - END on nRF54L is "packet data memory access completed", not the end on air, so this was worth checking. It is
     not the cause.
4. **The TX-tail variant does not fix it.** `CONFIG_RADIANT_FIX_TX_PHYEND` + `RADIANT_FIX_TX_TAIL_US=50` waits for
   PHYEND, then holds the TXIDLE carrier 50 µs more before DISABLE.
   - At matched −47 dBm: T1_Bt50 43 CRC failures, against B 45/50/55 and A 28/27/30. Its errors are still all in
     the last CRC bits, received as 1.
   - At −33 dBm (T3-T6, after the room signal rose by ~14 dB mid-round): A 12, B 19, Bt50 18 and 10. Too few
     counts to separate.
5. **RX_FAIL shows no B excess** at matched signal. Rounds 1-2 at −47 dBm: B 56 / 32 / 38, A 60 / 63 / 44. Round 3
   at −33 dBm: B 10, A 4, Bt50 7 / 5. In these rounds the excess is in CRC failures only, and every one was
   repairable.
6. **Consequence:** with the repaired-frame drop fixed (the e58b047 finding, on another branch), the TI delivers
   every one of these frames, and B's TI loss falls to its RX_FAIL, which is at A's level here. The B-side cause
   was not isolated. It is a last-bit, one-direction effect, so the most likely reading is the transmitter's carrier
   frequency at the end of the frame (into TXIDLE) seen through the TI demodulator's delay. Not proven.

## Arms

300 s per arm, Pro Micro and TI in the same window (`scripts/h3_arm.py`, a copy of `g1d_arm.py`). Tables in
`h3_summary.md`. TI CRC failures per 300 s, `crc_fail` (RX_FAIL in brackets):

| Round, signal | A | B | B + tail 50 µs |
|---|---|---|---|
| R (−47 dBm, TI h3_ti_bits) | 28 (60), 27 (63) | 45 (56), 50 (32) | - |
| Q (−47 dBm, TI h3_ti_bits2) | 30 (44), 17 (4, −47) | 55 (38); Q3_B 92 (389, interference, void) | - |
| T (T1/T2 −47/−42, then −33 dBm) | T3 12 (4) | T2 48 (85, −42 dBm, room event: PM 1.06 %), T5 19 (10) | T1 43 (15), T4 18 (7), T6 10 (5) |

`verify ... rc 1` on every arm, for both receivers: `ant_verify` always fails "common pages 80/81" for this sample,
which sends page 0 only. The TI also fails its loss limit and loss accounting, the latter because of the
repaired-frame drop. This is the same exit as every G1 arm, not an arm failure.

## Images (md5 in `images_md5.txt`)

| Image | What |
|---|---|
| `g1_A`, `g1_B` | G1's A and B, unchanged (1fbebd37, 26e088bc) |
| `h3_B_meas`, `h3_A_meas` | + `h3_meas.conf` (`RADIANT_DIAG_TX_PHYEND`, `g1h3_tx` symbol) |
| `h3_B_tail50` | B + `h3_tail50.conf` (wait PHYEND, hold 50 µs) |
| `h3_A_tail50` | A + the same (built, not run) |
| `h3_ti_bits` | `g1_ti_rx` + `RADIANT_DIAG_CRC_BITS`: page 12 f5 rx1/rx0, f6 isolated/run, f7 position quartiles |
| `h3_ti_bits2` | same Kconfig, f6/f7 changed to CRC syndrome-bit pairs (bit 0 = last bit on air) |

## Files

`h3_*.conf`, `scripts/h3_build.ps1` (L15 rows), `h3_arm.py`, `h3_seq{1,2,3}.sh`, `h3_summary.py`, `h3_summary.md`,
`runs/*.txt` (verify outputs, 0xF7 dumps, lp peeks, plans; JSON and `.rec` stay in `build/g1h3_runs/`).

## Board state at the end

L15 = `g1_A` (1fbebd37). TI = `g1_ti_rx` (fa76ef0f, `ant_probe` PASS). Pro Micro unchanged.
