# Host 0x71 proximity search, on air (2026-11-25)

Plan B4, bullet "Host 0x71". Code under test: wip/s24-bridge-prox `d3f39f7`
(master 1f5ddab merged into 20b79e9). Nobody could move sensors, so the
master's TX power stands in for distance. A real HR strap in the room gave a
real-sensor check.

## Rig

| role | board | notes |
|---|---|---|
| DUT | TI LaunchXL, COM11 @57600 | `build_all -Only radiant_dongle_cc26x2r1_launchxl.hex` from d3f39f7, sha256 f809473d.... Its `.config` matches the ship image's except for source paths. `images/` is local only. ant_probe PASS. Slave 0x00, lib config 0xE0, low-priority search timeout 0, 0x44 = 2 (5 s). |
| master | nRF54L15 DK, COM4 @115200, wd_dir_0fce666 (unchanged) | Broadcast master, #24601 type 0x19 tt 1, RF 66, period 4096. TX power set with 0x60: levels 5/3/1/0 = +8/0/-12/-20 dBm; custom 0x80 + 0x01 = -28 dBm (nRF54L15 raw register). |
| real sensors | ANT+ HR #12345 (the Pro Micro hrtx, off USB) and #8265 (a real strap) | RF 57, period 8070, never moved. |

The TI backend reports `caps.has_rssi = true` (radiant_radio_cc26xx.c), so it
is a valid DUT. Every open came after a random U(0, 0.5 s) delay. Tool:
`tools/ant_prox_onair.py`. Exact commands are in `run.sh`.

## RSSI at the DUT per master TX power (prox off)

| TX | acquisitions | RSSI min..max (all messages) | median of first message |
|---|---|---|---|
| +8 dBm | 20 | -38..-36 | -37 |
| 0 dBm | 20 | -46..-43 | -45 |
| -12 dBm | 20 (+10) | -58..-56 | -57 |
| -20 dBm | 20 | -68..-66 | -66.5 |
| -28 dBm | 20 (+10) | -76..-72 | -73 |

RSSI moves about 1 dB per dB of TX power, with a 2-4 dB spread on this fixed path.

## Trials (0x71 sent before every open, because it is one-shot)

| TX | RSSI | threshold | expected | acquired / n | byte 2, first msg | byte 2, later msgs |
|---|---|---|---|---|---|---|
| +8 | -37 | custom -30 (0x80, 30) | reject | **0/20** (20 timeouts) | - | - |
| +8 | -37 | bin 1 (-44) | pass | 20/20 | -44 all | 0 all |
| 0 | -45 | custom -37 | reject | **0/20** | - | - |
| 0 | -45 | bin 3 (-52) | pass | 20/20 | -52 all | 0 all |
| -12 | -57 | bin 2 (-48) | reject | **0/20** | - | - |
| -12 | -57 | bin 3 (-52) | reject | **0/20** | - | - |
| -12 | -57 | bin 6 (-64) | pass | 40/40 (two runs) | -64 all | 0 all |
| -20 | -67 | bin 5 (-60) | reject | **0/20** | - | - |
| -20 | -67 | bin 9 (-76) | pass | 20/20 | -76 all | 0 all |
| -28 | -73 | bin 6 (-64) | reject | **0/20** | - | - |
| -28 | -73 | bin 7 (-68) | reject | **0/20** | - | - |
| -28 | -73 | custom -82 | pass | 20/20 | -82 all | 0 all |
| -28 | -73 | bin 10 (-80) | pass | 20/20 | -80 all | 0 all |

- **Should reject:** 0 of 140 trials acquired.
- **Should pass:** 140 of 140 acquired, median 55-113 ms, the same as with
  prox off (64-79 ms).
- **Byte 2 on the acquiring message:** matched the bin or custom dBm every
  time (0 mismatches).
- **Byte 2 on later messages:** 0 every time, so the acquisition had cleared
  the threshold.
- **Byte 2 with prox off:** 0x00 on every message from this image. The owner
  has since ruled that it should be -128 (0x80), per AN16 section 4.3.3. That
  change is on wip/s25-rssi-off (2ca4e25) and is not on air in this run.

## One-shot and timeout-keeps

Six rounds with bin 3 (-52 dBm). The weak level is -20 dBm (-67 at the DUT);
the strong level is +8 dBm (-37).

| step | 0x71 sent? | result in 6 of 6 rounds |
|---|---|---|
| 1 weak | bin 3 | search timeout (5.02 s) |
| 2 weak | no | search timeout: **the timeout kept the threshold** |
| 3 strong | no | acquired, byte 2 = **-52**: still the bin from step 1 |
| 4 weak | no | acquired, byte 2 = 0: **the acquisition cleared it** |

The six `bh*_3_strong_noset` summary rows in `raw/behaviour.jsonl` each show
`thr0_mismatch: 1`. That is the tool, not the firmware: step 3 sends no 0x71,
so the tool expected the "off" value, but -52 (the kept bin 3) is the right
answer. Fixed in the tool after the 2026-11-25 review (`run(..., expect=)`).
The tool also now expects -128 for "off"; this image sent 0, so re-scoring
these logs needs `--off-byte 0`.

## Wire

| check | result |
|---|---|
| 0x54 CAPABILITIES, prox image | `200300ba36...`: advanced options 2 = 0x36, bit 0x10 **set** |
| same, ship image d94d07e (after the restore) | `200300ba26...`: 0x26, bit clear |
| 0x71 [0, 0x80, 60] | NO_ERROR (0) |
| 0x71 [0, 11] | 51 = INVALID_PARAMETER_PROVIDED |
| 0x71 [0, 0x80] (two bytes) | 51 |
| 0x71 [0, 0x80, 0] and [0, 0x80, 128] | 51 and 51 |
| 0x71 [0, 10], [0, 1], [0, 3, 99], [0, 0] | 0 each |

## Real sensors (HR, RF 57, wildcard device number unless noted)

- **Two straps on the air.** With prox off, #12345 (the Pro Micro hrtx) read
  a steady -36/-37 dBm. #8265 (a real strap at a fixed distance) read
  **-92..-55 dBm**, median about -73 to -81 over three sittings. Its RSSI swung
  by 30 dB from one acquisition to the next, while the bench path above held
  within 4 dB.
- **Near/far, bin 3 (-52):** with prox off, #8265 was acquired 12 times and
  #12345 8 times in 20. With bin 3, **#12345 was acquired in 30 of 30** and
  #8265 never.
- **#8265 searched for by its own number:**
  - custom -39: 0/10. Custom -33 (wildcard search): 0/10.
  - custom -96 and custom -101: 10/10 each.
  - Bin 4 (-56), about 25 dB above its median in that sitting (-81.5 dBm over
    20 prox-off acquisitions, `raw/hr_8265.jsonl`), acquired 2 of 10. Both came on
    a frame at exactly -56 dBm after about 2.9 s of searching. That is the
    inclusive "at or above" rule acting on a fade peak, not a leak.
  - So a far real sensor can pass a bin it sits well below most of the time,
    if the search runs long enough.

## Verdict

**PASS.** No bug found.

- Too-weak signals were never acquired: 0/140 on the bench, and 0/30 for the
  far real strap under bin 3.
- Strong-enough signals were always acquired, at the normal rate.
- Byte 2 carried the threshold in force on the acquiring frame.
- The one-shot and timeout-keeps behaviour held in 6 of 6 rounds.
- The capability bit is set on the prox image.
- The 3-byte custom form and bin 11 answer as documented.

Not exercised: fail-closed when a frame has no RSSI (the TI always reports
RSSI), and the tool's `pair` mode (the real straps covered near/far instead).

## Default bin (TX power stands in for distance, not a measured distance)

- At +8 dBm the DUT read -36/-37; the hrtx on the desk also reads -36.
- The real strap across the room sat at -73 to -92, with fade peaks to -55.
- Bin 3 (-52) separated the two straps 30/30.
- **Assumed, not measured:** nothing here measured a strap's transmit power.
  If a strap transmits at about 0 dBm, then held about as far from the DUT as
  the bench master was, it would read about -45, as the master did at 0 dBm.
  Bin 3 would then leave about 7 dB of margin for "held next to the dongle".
  The choice of bin 3 rests on that assumption.
- Under the same assumption, bin 2 (-48) is the tightest bin that still
  passes a 0 dBm sensor with some margin.
- Bin 4 (-56) would occasionally admit a far strap's fade peak.

**Default kept at bin 3** (`CONFIG_ANT_DONGLE_PAIRING_PROX_BIN`). The owner
chose not to calibrate distance further.

## Files

- `raw/*.jsonl`: one record per trial (committed).
- Local only: the `.txt` consoles, the flash transcripts and `images/` (the
  prox hex and its .config).
- `raw/pilot.jsonl`: an 11-trial rig check (5 + 3 + 3).
