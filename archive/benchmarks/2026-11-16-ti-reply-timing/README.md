# TI slave acked replies: early by ~105 us (2026-11-16)

Follow-on to `2026-11-16-ti-acked-rebuild/`: the TI as a slave lost every acked
message for stretches of seconds to whole sessions. Fixed on branch
`fix/ti-reply-timing` (94ec6a8), `CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US=100`.

## Rig

- Sniffer: nRF54L15 DK `001057737173` running `radiant/spike/promisc`
  (`build/promisc_l15/merged.hex`, logs device 14871 on COM3 @115200 with DTR).
  Restored afterwards to `build/zwift_3b486c3/radiant_dongle_l15_mpsl_zwift.hex`
  (59b84375), probe PASS.
- DUT: TI LaunchXL COM11 @57600. Master: Garmin USB-m `006`.
  Control: Pro Micro `EE6986854FD72803` (B4 O2 diag ba0a7084).
- `sniff.py` captures, `ana.py` gives offsets after the master frame (`0A`).
  `air=` in the captures is the sniffer's own END-ADDRESS interrupt latency, so
  sniffed offsets are good to about +/-10 us.

## Before (TI diag 6c5dc985, 497030a, cal 0)

| session | completed | TI reply A2, after master frame | Garmin ACK F2 |
|---|---|---|---|
| s1 | 100 | +2074..+2105, median 2089 | +3726..+3749 |
| s2 | 100 | median 2089 | |
| s3 | 99 | median 2089 | |
| s4 | 41 (first 41 OK, then all failed) | median 2089 throughout | stopped after the TI frame at +2085, as F2 crept 3731 -> 3742 |
| s5 | **0** | 92 frames on air, median 2089, CRC OK, RSSI -53 | **none** |
| s6 | 99 | median 2089 | |

Every A2 frame was byte-identical in good and dead sessions: payload
`33FFFFFFFF844EFF`, CRC `1B4A`, same address. The TI heard the master in
every slot. The `term_empty` windows in the earlier diag diff were the ACK
windows.

Reference offsets: nRF backend (Pro Micro, this session) +2160..+2175, median
2175 (2026-11-11: +2160..+2191). Garmin slave +2176..+2215 (2026-11-11).

## Cause

The TI's RX->TX turnaround was ~100 us short, so its slot reply went on air at
+2085 instead of +2190. The Garmin master's ACK (hence its slot timing) wanders
+3720..+3800 across sessions and drifts within one. Its reply window moves with
it, so the window's early edge drifts across a reply at +2085: whole sessions
pass, whole sessions fail. Real slaves at +2175..+2203 stay inside.

## After (TI diag a63489c5, 94ec6a8, cal 100)

| arm | TI | Pro Micro control, same window |
|---|---|---|
| slave originates acked (s11-s16) | 98, 99, 100, 100, 93, 94; reply median **+2189** | 91, 90, 96 |
| master originates acked | 93, 96 (Garmin ACKs AA at +2160..+2230) | 92 |
| master answers acked | 98 | |

No session fails as a block. Scattered single losses match the control (the
room was lossier late in the evening).

## Not resolved

The 100 us is the sum of the RX timestamp error (`T_SYNC_CAL_US`, still 0) and
the TX start error. One clock cannot split them. Only the RX half moves tracked
windows, so splitting it may explain the Pro Micro -> TI 8-13 % loss (TODO).
MPSL nRF builds' reply offset was not sniffed.
