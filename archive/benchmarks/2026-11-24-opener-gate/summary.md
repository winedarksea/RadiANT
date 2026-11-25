# Slot-opener gate, on-air A/B/A (2026-11-24)

Code under test: `22c3aa0` (wip/s24-opener-gate). Old image: TI ship image
d94d07e (sha256 2cf25860..., byte-identical to the 6aba33e build;
`git diff 6aba33e 90137c2 -- radiant/` is empty, so old = 90137c2 for radiant).
New image: `build_all -Only radiant_dongle_cc26x2r1_launchxl.hex` from the
22c3aa0 worktree, sha256 5e31866f..., `.config` identical to the ship
image's `ti.config`. Both hex files are in `images/` locally (gitignored).

## Rig

| role | board | how |
|---|---|---|
| master | nRF54L15 DK, COM4 @115200, wd_dir_0fce666 (unchanged) | bidirectional master 0x10 |
| slave (ACKer) | Garmin USB-m `006`, 0x60 level 3 (0 dBm) | bidirectional slave 0x00, never writes broadcasts |
| DUT | TI LaunchXL, COM11 @57600 | receive-only slave 0x40, lib config 0xE0 |

Channel #24601, type 0x19, trans type 1, RF 66 (2466 MHz, off the ANT+
frequency), period 1024 (32 Hz). The master sends acknowledged data in three
periods of every four (pattern AAAB). So in 3/4 of periods the only frame
2.19 ms after the opener is the Garmin's ACK.

Tool: `tools/ant_opener_ab.py` (committed with this). Per trial: random delay
U(0, 0.5 s), open, first data message = acquisition, watch 2 s, close, wait for
CHANNEL_CLOSED. Counts come from `analyse.py raw/*.jsonl`.

**Signature.** An on-grid DUT posts master data (0x4F plus the B-period
broadcast) about 48 times in 2 s. A DUT that claims on an ACK sits 2.19 ms
late. The old image re-anchors on every ACK, so it stays on the ACK grid,
posts nothing and gets an RX_FAIL in each broadcast period (17 in 2 s).
OFF-GRID means RX_FAIL + GO_TO_SEARCH > master-data posts.

**ACK payload.** The Garmin's ACK does NOT echo the acknowledged data. It
carries the slave's own never-written buffer, `00 00 51 5b 7d bf fb 16`,
which is unlike anything the master sends (0xAC.. acked, 0xBB.. broadcast).
A first BROADCAST_DATA with that payload is the ACK, posted as a broadcast.

## Counts (same sitting, 22:42-23:07)

| arm | image | trials | acquired | off-grid within 2 s | ACK payload as first broadcast | GO_TO_SEARCH | trials with any RX_FAIL | on-grid mean RX_FAIL / master posts |
|---|---|---|---|---|---|---|---|---|
| A1 | old d94d07e | 200 | 200 | **13** | **13** | 0 | 72 | 0.37 / 47.6 |
| B  | new 22c3aa0 | 200 | 200 | **0**  | **0**  | 0 | 51 | 0.29 / 47.7 |
| A2 | old d94d07e | 200 | 200 | **13** | **13** | 0 | 74 | 0.42 / 47.6 |

- All 26 off-grid old-image trials are the same event: the first post is the
  ACK payload, then 0 master posts and 17 RX_FAILs in 2 s, with no
  GO_TO_SEARCH. The old image stays locked to the ACK grid for the whole
  window. Expected rate is 0.75 x 2.19/31.25, about 5.3 %; measured 6.5 %.
- 25 of the 26 acquired 7-9 ms after the open (on-grid acquisitions spread
  over 0-40 ms). The search's first listen lands at a fixed offset from the
  open, so the delay had to be randomised.
- Old 26/400 against new 0/200: Fisher one-sided p is about 2e-5.
- The "trials with any RX_FAIL" column is the room's loss floor (on-grid,
  about 0.3-0.4 per 2 s). It is not the fault.
- Master/slave health was the same in every arm: about 11 000 acked sent per
  arm, 1.2-1.4 % TX_FAILED, slave RX_FAIL about 50.

**Verdict: PASS.** The new image has zero off-grid acquisitions and zero ACK
payloads posted as broadcasts. The old image shows 13 of each in both
same-window control arms.

Residual, not exercised: a repeated 0x8A/0xAA packet-0 retry (see TODO).
This rig sends no slave 0x02 replies (`--slave-replies` exists but was not run).

## Files

- `raw/{A1_old,B_new,A2_old}.jsonl` hold one record per trial, with every
  post (time, kind, rx timestamp, payload bytes 1-3) and every RX_FAIL /
  GO_TO_SEARCH event. The `.txt` consoles sit beside them locally only
  (gitignored, as are `images/`).
- The per-trial `ack_as_bcast` field inside the jsonl was written by a rule
  that assumed an echoed ACK (first post 0xAC), so it reads false
  everywhere. Use `analyse.py`, which matches the ACK payload directly. The
  tool now uses the corrected rule.
- `raw/pilot_old.jsonl` is a 20-trial rig check on the old image, run with an
  earlier script revision that did not log per-event payloads.
- `raw/flash_*.txt` (local only) are the flash_ti.ps1 transcripts. Both
  report "Program verification successful", and ant_probe PASSed after each.
- `run.sh` holds the exact commands.
