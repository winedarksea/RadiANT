# FE-C implementation and bench validation — 2026-10-19

The patch resolves the acquired channel identity before originating transfers,
queues slave acknowledged commands until a received master slot, reserves the
data/ACK turnaround, and restores tracking after completion. Configured wildcard
identities remain available for reacquisition. Pending commands have a bounded
watchdog and preserve busy/cancellation semantics. Transfer failure counters are
available through the existing statistics accessor and periodic firmware log.

A hardware load run exposed an additional scheduling defect: a background scan
could deliver its terminal callback after its airtime tail and consume the exact
transmit operation's arming lead. Gap fillers now leave 200 microseconds for
software dispatch. An initial 100 microsecond reserve substantially improved
the loaded link but still produced a rare active-session scheduling failure;
the strengthened regression fails with 100 and passes with 200. On-air reply
constants and sender retry policy are unchanged.
The deterministic API regression fails without this margin and passes with it.

The decoder reports checksum candidates, discarded bytes, partial ANT tails,
capture truncation and USB errors independently. Reassembly includes USB bus,
device, endpoint and direction. Summaries retain unresolved accepted commands as
pending and show channel broadcasts and named events/responses. The capture
script documents captures started during an existing Zwift session. Unsupported
serial-number requests and recoverable scan-channel assignment refusals retain
their existing protocol responses and capability bits.

## Build and hardware

- Base revision: `6979eb9ae9417dee5d9de43df5f816765a901048`, plus the working
  tree patch recorded in `dist/fec-validation/manifest.json` and source snapshot.
- RadiANT: NCS v3.4.0, toolchain `dcbdc366a1`; nRF52840 dongle with native USB,
  nRF radio backend, no MPSL gate; application offset `0x1000`.
- Repository version: 0.1.0; DFU application version: 100.
- Independent SDK ANT peer: nRF54L15 DK, probe `1057737173`, UART COM4,
  NCS v3.2.4 and SDK ANT core (`BOV02.01.00`).
- nRF5340 DK, probe `1050006310`: fake-radio ztests on application core/COM6;
  real RadiANT diagnostic endpoint on network core/COM5 with UART companion.
- Dongle rediscovered in DFU as `CE548CAB73A6`, COM7; runtime USB serial
  `3D55F77818BE772A`, version `RADIANT0.01B00`, 32 channels and 3 networks.

Package: [`radiant_dongle_nrf52840dongle_fec.zip`](../dist/radiant_dongle_nrf52840dongle_fec.zip).
SHA-256: `679fa432758ee29534f507877166d26a0774bec9432a55cd6b1cfadf4fe40767`.
Firmware BIN SHA-256: `dc088834acfb2b018fb27298976ee3f562a992799d0b2456dfb4d0907f9d7607`.
Firmware HEX SHA-256: `095b71886beacce9131e3f1abfbde7b71badf83a8ac111f5022b67456b375899`.
The artifact directory contains the resolved configuration, source hashes,
firmware, test logs and machine-readable results.

## Deterministic checks

| Check | Result |
| --- | --- |
| Host unittest discovery | 759 passed |
| Lower-level ztests, nRF53 DK | 804 passed, 44 suites |
| API ztests, nRF53 DK | 41 passed |
| Scheduler gate ztests, nRF53 DK | 28 passed |
| Ruff on changed Python tools/tests | Passed |
| `git diff --check` | Passed |
| Flashed dongle USB probe | Passed |

API coverage includes resolved and pinned identities, reacquisition, command
arrival around window opening, overlap, missed master openers, missing/wrong
ACKs, cancellation/reset, recovery, continued broadcasts, and single terminal
outcomes. A synthetic delayed scan completion verifies the scheduler regression.
Host fixtures cover split/coalesced frames, embedded sync, checksum corruption,
isolated USB streams, error/truncation handling, and mid-session transfers.

Both original captures remain unchanged. Valid message lines exactly match the
original decoder: 1,858 for `zwift-20261018-224924.pcap` and 2,674 for
`zwift-20261018-225653.pcap`. Neither has bad-checksum candidates, incomplete ANT
tails, truncated records or nonzero USB statuses. Discarded zero padding is
reported separately. Hashes and complete summaries are in `fec-capture-final.json`.

## Hardware acceptance results

The final 200 microsecond image completed both 300-second, 300-command USB
scenarios with 295/300 transfers completed in each, single terminal outcomes,
and no duplicate deliveries or observed USB/protocol corruption. The strict
99% completion target was not met. Detailed final results follow the earlier
100 microsecond revision results below. The user confirmed that the
hardware was already spaced appropriately and accepted that ambient RF noise
can sometimes exceed the strict 1.5% loss ceiling. Thresholds in the tools have
not been relaxed.

| Scenario | Accepted | Completed | Peer receipt | Terminal outcomes | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| FE-C alone, `fec-alone-validated.json`, 300.031 s | 300 | 299 (99.67%) | 299 | 300 | Control target met; staged-payload receive check 2.673%, strict receive gate failed |
| FE-C + power + HR + scan, `fec-loaded-acceptance.json`, 300.047 s | 300 | 298 (99.33%) | 298 | 300 | All paired-run gates passed |
| Independent simultaneous power receive comparison, 302 s | — | — | — | — | Exact loss 0.1653% RadiANT vs 0.9959% SDK; comparative and absolute gates passed |

Both full command runs had zero duplicate or unexpected peer deliveries, zero
missing terminal outcomes, and zero observed framing errors. The final loaded
run's TX-event receive-loss figures were 0.9524% (FE-C), 0.8681% (power), and
0.4255% (HR), with continued broadcasts on all channels. The solo receive result
used the earlier staged-payload method described below; it is preserved as a
failed gate rather than reclassified as a pass. The complete evidence supports
the firmware repair and handoff, with RF-loss exceptions explicitly retained.

A later solo repeat stopped after 223 commands when the independent SDK peer's
COM4 UART driver reported that its device was not functioning. This interrupted
run does not establish a dongle USB disconnect or a valid 300-second result.
Its earlier runner version also left `peer_received` at its default zero on
abort; that field is unavailable for this run, not evidence of undelivered
commands. Finalization now preserves collected delivery counts on abort. One
additional setup attempt missed an endpoint reset before sending any commands;
subsequent conformance resets and runtime probes succeeded. These attempts and
all earlier failures are included in the artifact directory.

The first patched-image solo run (`fec-alone-fixed.json`) completed 297/300
commands with no duplicate deliveries or missing terminal outcomes, but failed
the receive gate at 1.5038%. That run used an earlier host counting method that
clipped TX and RX notifications at identical time boundaries. A regression
demonstrates that host latency across either boundary can falsely count a miss.
The corrected payload comparison matches across those boundaries and consumes
each RX at most once. A second source issue was then demonstrated: the host can
stage a payload version that is overwritten before transmission, producing
repeated event counters at otherwise successful receivers. Missing staged
versions cannot establish a receiver packet loss. The final runner therefore
matches actual master TX events to channel broadcasts, individually, within
100 ms (less than half the shortest configured period). It retains staged
payload gaps separately. Tests verify that neighbouring slots cannot conceal a
missing broadcast. Thresholds are unchanged; earlier results remain preserved,
and acceptance requires fresh runs with the corrected measurement.

The earlier 100 microsecond simultaneous comparison using a DK-generated power counter ran for 302
seconds. Exact loss was 0.1653% for RadiANT and 0.9959% for SDK ANT. Both the
1.5% absolute ceiling and the SDK + 0.2 percentage-point comparative gate passed.
The initial host-generated source comparison is retained separately; its
repeated counter made exact loss unavailable, so its wall-clock result is not
used for that gate. The source application's code and configuration accompany
the report.

Selected conformance checks exercised 29 assignment, request and serial-number
cases. There were no unexpected differences from the archived transcript.
Known archive differences are the command-reset startup reason (0x20 versus
0x00) and the intentionally unadvertised serial-number capability bit. The
unmodified archive comparison and the reviewed differences are both retained;
this is a selected conformance check, not a new full conformance baseline.

Two initial 30-command nRF53 radio diagnostics recorded zero transfer radio failures,
missing ACKs, invalid ACKs or stuck-transfer watchdogs. Each had one pending
command expire without an opener. The cumulative scheduler missed count was
two, including configuration/teardown; it does not establish that active-session
scheduler misses were zero. This distinction is retained in the hardware
acceptance assessment. A subsequent check with separate phase snapshots recorded
one active-session scheduler miss and transfer radio failure. That finding drove
the final 200 microsecond reserve revision; it is not attributed to RF noise.

The final 200 microsecond revision passed all 41 API, 804 lower-level and 28
scheduler-gate tests on the nRF53 DK. Its focused loaded radio check recorded
zero scheduler misses, transfer radio failures, missing/invalid ACKs, or
stuck-transfer watchdogs during the command interval and cleanup. All observed
master broadcast events were received. One of 30 commands expired while waiting
for a master opener, with exactly one terminal outcome per accepted command.
The revised package was flashed after rediscovering DFU serial `CE548CAB73A6`
on COM7. Runtime enumeration and the startup/capabilities/version probe passed.
The final 200 microsecond solo USB run (`fec-200us-alone.json`) completed a full
300.047 seconds and 300 accepted commands: 295 completed (98.33%), 5 failed,
296 reached the independent peer, and all 300 had a terminal outcome. There
were no duplicate/unexpected peer deliveries, framing errors, or runner errors.
Actual TX-event receive loss was 20/1,197 (1.6708%). This run failed the strict
99% completion and 1.5% receive gates; it is reported as such, with the user's
explicit allowance for ambient RF variability. One command reached the peer
but its acknowledgement did not complete at the originator.

The final 200 microsecond loaded run (`fec-200us-loaded.json`) completed
300.046 seconds and 300 accepted commands: 295 completed (98.33%), 5 failed,
297 reached the independent peer, and all 300 had a terminal outcome. No
duplicate/unexpected peer deliveries, framing errors, or runner errors occurred.
Receive loss was 1.2998% (FE-C), 1.8261% (power), and 1.4493% (HR). Two failed
transfers reached the peer but did not complete their acknowledgement. The
strict completion gate and power-channel loss ceiling failed; both failures
remain recorded rather than hidden by changing thresholds.

These results establish completed control exchanges and continued broadcasts,
with single terminal outcomes and no observed protocol corruption. They do not
constitute a pass of every numerical target in the original plan. The user's
subsequent guidance accepts occasional RF-related excursions above the exact
limits. The receiver comparison applies to the attached bench hardware and
its physical positions; RSSI differs between the two receivers.

The final 200 microsecond simultaneous receive comparison ran for 302 seconds
with fresh result files from both receivers. Exact loss was 0% for RadiANT and
1.1647% for SDK ANT. The unchanged 1.5% absolute and SDK + 0.2 percentage-point
comparative gates both passed (`fec-comparative-gate.json`).

All 29 selected conformance cases were repeated on the final flashed image,
with no unexpected archive differences. The final USB reset, capabilities and
version probe passed. The dongle is left running this tested image; the DKs
were restored to idle UART diagnostic endpoints. `fec-200us-phases.json`
retains the nonzero solo/loaded gate exits alongside the successful comparison,
conformance and probe phases. The DFU ZIP and `dist/fec-validation/manifest.json`
provide the image, hashes, resolved configurations, source snapshot and test
evidence for handoff.

Earlier diagnostic results are retained rather than treated as acceptance:
before the scheduler margin, the loaded dongle run completed only 5/300 commands.
The nRF53 real-radio diagnostic showed scheduling misses and radio failures,
without missing-ACK or invalid-ACK failures, supporting the scheduling diagnosis.

Synthetic sensor profiles exercise transport and scheduling. Commercial trainer
compatibility, the Zwift popup, and why the trainer channel was absent in the
second capture require the deferred trainer/Zwift session.
