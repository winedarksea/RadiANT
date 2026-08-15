# `archive/captures/coex/` — the P4 ANT+/OpenThread coexistence arms

Checked by: nothing automated. These are raw bench recordings, and
`docs/radiant-bridge.md` §7.4.2 is what reads them — every packet count in that
section's table appears verbatim in the `-verify.txt` files here. If the section
and these files ever disagree, **the files are right**: they were written by the
instrument, the table by a person.

---

## What these are

The measurement that settled whether ANT+ and OpenThread can share one nRF54L15
radio. Three arms, 240 s each, same master, same instrument, same board, same
sitting on 2026-08-13:

| File prefix | Arm | Result, as reported by `ant_verify.py` |
|---|---|---|
| `2026-08-13-p4ctrl-control` | `gate.conf` — the arbiter, no second stack | 959 of 962, 3 misses, all `RX_FAIL` |
| `2026-08-13-p4med-thread-med` | `thread.conf` — arbiter + OpenThread MED, load transmitting | 960 of 961, 1 miss, `RX_FAIL` |
| `2026-08-13-p4sed-thread-sed` | `thread.conf;thread_sed.conf` — the SED role | 957 of 960, 3 misses, all `RX_FAIL` |

Every miss in all three arms carries a matching `EVENT_RX_FAIL`, which is the
one thing that distinguishes a packet lost on the air from a packet lost in
software after the radio already had it. That is why `ant_verify.py`'s
accounting line matters more than its loss percentage, and why these arms are
worth keeping rather than just the number.

The arms are indistinguishable from each other at this run length — one packet
is about 0.1 % here — against a +0.5 pp bar. **§7.3's two-box fallback is
therefore not needed**, which is the decision these three files support.

Each arm has three files: `-console.log` is the DUT's VCOM (COM7, DTR asserted)
for the whole run, `-verify.txt` is the instrument's report, and `-sim.txt` is
the driven master's side.

## Provenance

Recorded by `scripts/p4_arm.ps1`, which flashes, resets, opens both VCOMs and
checks the Thread leader's `ot state` **before and after** every arm — a dead
peer and a broken DUT look identical from the DUT's console, and that check is
the only thing that separates them.

- **DUT:** nRF54L15 DK, J-Link `001057737173`. Log COM7, ANT UART COM8.
- **Thread leader:** nRF5340 DK, J-Link `001050006310`, running
  `nrf/samples/openthread/cli` with `scripts/p4_peer.conf`. CLI on COM9.
- **ANT+ master:** driven over the USB dongle by `tools/ant_sim.py`.

Their original filenames, before the rename into this directory's date-first
convention, were `20260813-170054-p4ctrl-final-ctrl-*`,
`20260813-165053-p4med-final-med-*` and `20260813-165555-p4sed-final-sed-*`.
The timestamps are the link back to the full sitting.

## What was discarded, and why that is stated here

**Nine files were kept out of 149.** The other 140 are the rest of that
sitting — `p4med-bug15-fix`, `p4med-bug19-fix-iter`, `bug20`, `bug21`, `bug22`,
`b23-fix-hold0`, several abandoned controls, and a harness smoke run. They are
the *debugging* of five defects in series, not the *result*, and §7.4.2's table
does not cite any of them.

They were not deleted for space. They were deleted because a preservation
directory whose contents nobody can account for stops being evidence and
becomes a pile — `docs/preservation.md`'s opening rule. Keeping the three arms
a document actually cites, and saying plainly that 140 intermediate runs were
dropped, is a claim a reader can check. Keeping all 149 would have cost 9.3 MB
against a 10 MB budget with 2.75 MB already spent, and would have put the
directory over.

If the intermediate runs are ever wanted, they are reproducible in the sense
that matters least — the *defects* they recorded are fixed, so re-running the
harness cannot produce them again. That is an argument for having archived the
three that state the outcome, not for having kept the other 140.

## SHA-256

```
d5bf98bb26c0c0b67e750fa8ac88417e62694cf4d6bd8c31f15d95e97cd85112  2026-08-13-p4ctrl-control-console.log
620f9195c8fcea6b5b397e86c604daa70feea93bd0298594c12bcaf80743e546  2026-08-13-p4ctrl-control-sim.txt
159ee84ebc60e17991dcf66676c74b79db24f463774c9e9f565103dad7012db3  2026-08-13-p4ctrl-control-verify.txt
540983f395d7decf735209b5e8eab1eae8ad7e083f61f4382064e9f17c094c62  2026-08-13-p4med-thread-med-console.log
6f0a1564d6060d12fe764197034306942522cddcc43773a6c642dc3871d62e76  2026-08-13-p4med-thread-med-sim.txt
7eb666d774b23e69ed4b23e354fcc324747cfb065bcb47d0a9c79825743fcc17  2026-08-13-p4med-thread-med-verify.txt
fb021ec621f9086cef9a7066878650cdeea286226f2397d78f7727dbe9ef2145  2026-08-13-p4sed-thread-sed-console.log
0b5db3030215b008de36d0f448907bbbbf9df8449ea88fe795456e2faa596b87  2026-08-13-p4sed-thread-sed-sim.txt
b8a13f4fd40f1f2cf47a06336aee4b1c789662a22c32f9f38314e49dbd277391  2026-08-13-p4sed-thread-sed-verify.txt
```
