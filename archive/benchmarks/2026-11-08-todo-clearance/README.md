# Bench record for the 2026-11-08 TODO clearance

Written for whoever later has to judge these fixes from their on-air evidence.
Every result here is from one sitting on 2026-11-08 and 2026-11-09. Raw `.antcap`
recordings and build logs stayed on the bench PC.

Firmware arms: "old" is master before the clearance (0eb9f3f), "new" is the
integrated tree. Receivers: nRF52840 Dongle `3D55F77818BE772A`, Pro Micro
`EE6986854FD72803`, Garmin USB-m `006` (reference receiver, one channel only).
Transmitters: nRF54L15 DK and nRF5340 DK net core, both driven by `ant_sim.py`.

The room is the main limitation of this record. Loss in one window varied from
0.3 % to 48 % with the same firmware, device number and level, and the Garmin
itself moved from 1.1 % to 15.1 % between 10-minute windows. Differences smaller
than that say nothing unless the receivers shared a window.

## Timing crossover (A14-1, A14-2, A9-3, A8-4): pass

Six channels from two boards (54L15 #53000..2, 5340 #41000..2, mixed periods
8182 / 8070 / 8086), 600 s per arm, receivers tracking all six concurrently.
Loss is `(expected - packets) / expected`, from `ant_verify.py`'s per-channel
expected count. `arm*/` holds each receiver's summary.

| Arm | Pro Micro | Dongle | Garmin reference |
|---|---|---|---|
| 1: PM new, Dongle old | 3.83 % (new) | 3.93 % (old) | 15.10 % on #53000 |
| 2: PM old, Dongle new | 1.14 % (old) | 2.29 % (new) | 1.12 % on #53000 |
| 3: control, both new | 1.02 % | 1.74 % | 4.83 % on #41000 |

- Same-window channels #53000 (Garmin's channel): new read 0.41 % (arm 1) and
  0.58 % (arm 2), both inside the Garmin's own interval, the plan's pass rule.
- The weak 5340 channels (#41xxx) read 2-5 pp higher on new than old in arms 1
  and 2, which is why arm 3 was run. In arm 3 the same receivers on new
  firmware read 1.4-1.8 % (PM) and 2.9-3.3 % (Dongle), the PM matching its old-
  firmware 1.7-2.0 %, and the Garmin lost 4.8 % on that channel. Those channels
  are lossy on air and vary with receiver and window.
- On direct images these fixes change little by design, so this shows no
  regression. It does not show an improvement.

## Hardware exercise of A14-1 and A9-3: pass

Instrumented Pro Micro images (`late_cc/`), forcing a late `CC_START` every 256
receive windows, one tracked channel, 300 s:

| Image | `key_rejects` (late starts) | `host_desync` (armed_stale) | `sched_missed` | Packets |
|---|---|---|---|---|
| A14-1 path active | 4 | 0 | 4 | 1178 |
| Refusal removed, frozen window closed by CC_CLOSE | 4 | 0 | 0 | 1177 |
| Refusal removed and CC_CLOSE disconnected | 4 | 4 | 0 | 1178 |

Line 2 is why the third image exists: with the close compare connected, a frozen
window ends normally, so A9-3's reclaim never runs. Disconnecting it produced 4
real hangs, all reclaimed, and the channel kept delivering.

## ant_sens fine ladder (Garmin 4): pass

`sens_fine.json`, 54L15 TX, 12 s rungs. No rung shows the old artifact (17.5 %
with `event_counter` basis, `rx_fail` 0, packets above master_sent), and the
spurious knee at +7.7 dBm is gone. Two rungs read 50 packets against 48 sent on
two receivers at once, so that is a rung-boundary effect in the master count.

The Garmin column of that run is invalid: `ant_features.py` had run on it just
before and left it configured, and it read about 80 % loss on every rung.
Reset, it read 0.00-1.67 % (`sens_garmin.json`). Reset the Garmin after
`ant_features.py`.

## Garmin capability check (Garmin 6)

`garmin_features.txt`. Sections 1-4 and 6 pass; the capabilities fix holds. The
stick's reply to reading the event filter after setting 0x0006 was `0000f1`, so
the tool reports a mismatch. Not investigated further.

## SLIP_GUARD_RUN 8 vs 9: inconclusive, default stays 9

Two arms with the boards swapped (`slip/`), and an old-against-new baseline
(`slip_base/`). Loss was bimodal across windows for every firmware and number,
at both strong and weak signal, so these runs cannot resolve a difference
between run 8 and run 9. For the at-risk numbers at weak signal, run 8 did not
show an improvement. The plan's rule requires an improvement to change the
default, so it stays at 9. Measuring this needs a quieter band, shielding or an
attenuator.

The same data shows no firmware regression on the guarded addresses: old and
new images were equally bimodal (`slip_base/summary.txt`).

## Conformance transcript

The nRF transcript re-recorded byte-identical on the pre-Release-2 image. Release
2 changes `advanced_options_3` from 0xD5 to 0x05, so it was re-recorded on the
final image (54L15 DK, 2026-11-09). One of 1273 records changed, the capabilities
reply, in that byte and its checksum. The cc26xx transcript is owed, and needs a
CC2652.

## Final image on air: session pass, loss gate failed on the Dongle

Dongle and Pro Micro on the final image, the rig from the crossover, 300 s per
window, 2026-11-09. Loss is from each receiver's `ant_verify` summary (`final/`,
`final2/`). Exact loss is withheld on this rig, so these are wall-clock figures.

| Window | Pro Micro | Dongle | Garmin reference (#53000) |
|---|---|---|---|
| 1 | 0.78 %, pass | 3.69 %, FAIL | 1.49 %, pass |
| 2 | 0.71 %, pass | 1.07 %, FAIL | 2.66 %, FAIL |

The Dongle failed `ant_verify`'s 1.5 % per-channel gate in both windows:

- Window 1: all six channels, 4.4-6.4 % on the 54L15's #53xxx and 1.9-2.1 % on
  the 5340's #41xxx. Almost all of it was reported as `RX_FAIL` (265 of 268
  packets), with two drops to search. **This is not explained.** The Pro Micro
  lost 0.3-0.5 % on #53xxx in the same window, so the room as a whole does not
  account for it. It did not recur.
- Window 2: only #41xxx, 1.6-1.9 %. On #53xxx the Dongle read 0.3-0.6 %, level
  with the Pro Micro.

The Dongle's range on earlier images in the crossover was 1.74 % (new) to 3.93 %
(old), and the Pro Micro read lower in every window. Window 1 falls inside that
range and window 2 is below it. No old-firmware arm shared these windows, so this
does not show the absence of a regression on its own; the crossover above is the
evidence for that.

`ant_session` (`session/`), 45 s with both masters up: pass on both boards. Eight
channels held open, six sensors identified, acknowledged and burst paths reached
the stack. 801 broadcasts on the Pro Micro, 758 on the Dongle.
