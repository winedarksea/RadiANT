# 0019 — A tracking address the nRF demodulator slips on gets a 2-byte hardware match and a CRC-checked repair; every other address keeps the full match

- **Status:** accepted. Built, ztest-verified, and measured on air (see
  [Result](#result)).
- **Date:** 2026-11-07
- **Evidence:** `archive/benchmarks/2026-11-07-devnum-rx-loss/README.md`
  (rig, every window's raw logs, the analysis scripts)
- **Code:** `radiant/src/radiant_addr_split.[ch]` (pure, ztest
  `radiant_addr_split`), `radiant/src/radiant_radio_nrf.c`
  (`rx_sw_addr_len()`, the END branch of the ISR),
  `CONFIG_RADIANT_NRF_SLIP_GUARD`, `CONFIG_RADIANT_NRF_SLIP_GUARD_RUN`

> **Provenance.** Clean-room. Written from this project's source and its own
> bench measurements. Nothing here derives from sdk-ant, `libant.a` or any
> ANT+ profile document. See [0002](0002-clean-room-policy.md).

---

## Context

The 2026-11-07 comparison with a Garmin ANT USB-m found that RadiANT receivers
lost 1.5-3 % of a power sensor's frames at device number #54100 and none at
#54900, while the Garmin lost 0-0.2 % at both. It followed the number, not the
transmitter or the board.

## What was measured

All on one rig: a transmitter, four or five receivers in the same window, 120-
300 s per window, loss = `EVENT_RX_FAIL / (packets + EVENT_RX_FAIL)`.

1. **Receive side, not transmit.** With the Garmin transmitting, every RadiANT
   receiver still lost 1.7-2.1 % at #54100 and 0-0.08 % at #54900.
2. **The correlator never fires.** A diagnostic build
   (`CONFIG_RADIANT_NRF_RX_DIAG`) classified every missed window on the
   nRF54L15: 21 of 21 were "silent" (no ADDRESS, no END), 0 CRC failures.
3. **The predictor is an alternating run.** A sweep of ten numbers put the
   threshold at an alternating run (0101...) of about 9 bits in the on-air
   address `[A6 C5 dnl dnh dtype]` (MSb first per byte). `C5` ends in `0101`,
   so any `dnl` starting `010` extends it.
4. **It is one deleted bit.** Matching fewer bytes in hardware turned the
   silent windows into delivered frames that failed the software compare. Their
   raw bytes were the sent stream with **exactly one bit deleted** inside the
   run, intact before and after it: 21 of 21 captures. Re-inserting the bit made
   every one CRC-valid, uniquely.
5. **Where it slips.** The slip happens inside the hardware-matched address or
   within a few bits after its end. A run that starts 5 bits past the end of
   the match is clean (#43634 at a 2-byte match: 0.00 % on both DKs).

So on the nRF RADIO in `Nrf_1Mbit` mode (nRF52840, nRF5340 network core,
nRF54L15 alike), a long alternating run near the address costs bit sync by one
bit. The Garmin's demodulator does not do this.

## Options considered

- **A fixed shorter hardware match for every address** (the first plan). A
  2-byte match, `[A6 C5]`, keeps every device-number bit out of the matcher.
  It fixes numbers whose run starts far enough past `C5` (#43634) but not
  #54100, whose run joins `C5`'s own `0101` and so always straddles the end of
  the match. It also has a cost of its own: a 16-bit match fires on noise
  about once per 65536 bit times, and a false match just before the real frame
  occupies the receiver through it. Measured on a clean number (#54132): 2
  windows in 481, both noise matches whose payload contained the real frame's
  preamble and address. A 3-byte match is worse (it moves the slip zone onto
  `dnh`).
- **RX mode or preamble experiments.** The plan's fallback. The slip is a
  property of the demodulator's acquisition, and every register that changes
  acquisition (mode, preamble length) also changes sensitivity. Not pursued,
  because the deletion turned out to be recoverable.
- **Repair alone, at the full 5-byte match.** Impossible: a slip inside the
  hardware address is a frame never delivered.

## Decision

Per receive window, from its filters:

1. **No filter at risk** (no alternating run of `SLIP_GUARD_RUN` = 9 or more
   bits in its 5-byte address): the full 5-byte hardware match, exactly as
   before. No register, timing or code path differs. That is about 95 % of
   device numbers.
2. **Any filter at risk:** match only `[A6 C5]` in hardware. The other three
   address bytes arrive as payload and are compared in software (as the first
   plan built it, t_sync corrected by their airtime). Now every slip lands in a
   delivered frame. When the compare fails, `radiant_addr_split_repair()` tries
   one thing: are the received address bits exactly the filter's with one bit
   deleted? If so, the original frame is the filter's address followed by the
   received bits from one place earlier. It is delivered only if the CRC over
   that reconstruction verifies, and then as an ordinary OK frame.

The repair is a single CRC trial per candidate filter, and only on a frame
that already failed the compare. An accepted frame pays a 3-byte `memcmp`.
Nothing about the RF configuration changes, so sensitivity cannot.

A false repair needs a frame whose 23 received address bits happen to be ours
with one deletion **and** whose CRC then verifies (2^-16). Noise and foreign
sensors almost never pass the first test. The repair does not touch a deletion
in the body, an insertion, or a CRC failure on a frame whose address arrived
intact; none of those was ever observed from this mechanism.

`CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN` (fixed length for every address,
default 5) stays as a measurement knob.

## Result

Full tables: `archive/benchmarks/2026-11-07-devnum-rx-loss/README.md`,
Phase 5. A stock Feather sat in every window as the control.

- **A/B/A on #54100, Garmin receiving, 300 s per arm.** The 54L15 DK lost
  2.08 % on the stock image, **0.17 %** with the guard, and 2.33 % back on
  stock. The Garmin lost 0.17 / 0.17 / 0.33 % in the same windows, and the
  stock Feather 2.33 / 2.41 / 2.58 %. The nRF5340 with the guard lost 0.08 % in
  all three arms. That is inside the Garmin's Wilson interval, the Done gate.
- **Sweep at normal signal (-37..-40 dBm), Garmin transmitting.** On the
  at-risk numbers #54100, #43634 and #21845, the Dongle, 54L15 and 5340 with
  the guard lost 0.00-0.21 %, and the stock Feather 1.87-2.28 %. Clean numbers
  were 0 on every board.
- **Full sweep** (11 numbers). The guard chose the right match length in every
  window. The 54L15 repaired 52 frames, and saw one noise match in about 6700
  windows.
- **Residual.** At -50..-60 dBm the guarded boards still lost 0.2-1.25 % on
  runs of 8-12 bits (against 1-3 % stock). The 54L15's captures there show
  slips the repair rightly refuses (body errors; a deletion followed by an
  insertion). The unguarded run-8 #54058 also lost 0.83 % on the 5340 at
  -50 dBm. The mechanism grows with falling signal. That is open, below.

## Consequences

- At-risk numbers still pay the 2-byte match's noise cost (a few tenths of a
  percent at most), in exchange for removing a 1-5 % loss. Clean numbers pay
  nothing.
- A merged window that contains one at-risk filter matches 2 bytes for all of
  its filters. That is correct (the software compare demuxes them) and only
  costs the noise exposure on that window.
- The threshold rests on one sweep at strong signal. There, 8-bit runs
  measured at most 0.2 %, about what the short match costs, so 9 is the
  break-even. At -50 dBm the 5340 lost 0.83 % on a run of 8, so a weaker link
  may want `SLIP_GUARD_RUN=8`, which doubles the share of numbers that get the
  guard (5 % → 10 %). Not decided; it needs a controlled-level run.
- If a future part's demodulator slips differently (an insertion, or two
  bits), those frames stay lost exactly as before; nothing is made worse.
