# FE-C implementation and bench validation - 2026-10-19

**Hardware acceptance: FAIL.** Both final 200 us USB runs failed the 99%
acknowledged-transfer completion gate. Solo reception and loaded power
reception also failed the 1.5% loss ceiling. The firmware repairs substantially
improved control exchanges, but the original quantitative acceptance criteria
remain unmet. The user's tolerance of RF variance permits a practical handoff;
it does not turn failed gates into passes or establish RF as their cause.

## Final image and results

RadiANT was built with NCS v3.4.0, toolchain `dcbdc366a1`, for
`nrf52840dongle/nrf52840`, native USB and direct nRF radio backend (no MPSL).
Application offset is `0x1000`; repository version 0.1.0 / DFU version 100.
The independent nRF54L15 DK used SDK ANT `BOV02.01.00` with NCS v3.2.4.
The nRF5340 DK ran fake-radio tests and network-core UART radio diagnostics.

The tested source was base `6979eb9ae9417dee5d9de43df5f816765a901048` plus
the implementation later committed as `6bc59fe`. Detailed bench logs, result
JSON, package and source snapshot remain local under the gitignored
`archive/benchmarks/2026-10-19-fec/` directory; this report records the measured
results and image hashes.

| Final run (300 seconds, 300 accepted commands each) | Completed | Failed | Peer receipt | Terminal events | Strict result |
| --- | ---: | ---: | ---: | ---: | --- |
| FE-C alone (`fec-200us-alone.json`) | 295 (98.33%) | 5 | 296 | 300 | FAIL: completion; RX loss 20/1,197 = 1.6708% |
| FE-C + power + HR + scan (`fec-200us-loaded.json`) | 295 (98.33%) | 5 | 297 | 300 | FAIL: completion; power RX loss 21/1,150 = 1.8261% |

Loaded FE-C loss was 1.2998% and HR loss 1.4493%, both below the ceiling.
Both runs had zero missing terminal outcomes, duplicate/unexpected deliveries,
runner errors or observed framing errors. Broadcasts continued on all three
loaded channels. These counts describe this runner's commands and retry policy.

**Delivery despite failure remains a compatibility risk.** One solo command and
two loaded commands reached the peer while the sender reported
`EVENT_TRANSFER_TX_FAILED`. A failed transfer therefore does not establish that
the trainer did nothing. Whether Zwift retries those commands, and whether that
would duplicate a trainer action, has not been tested. Zero duplicate delivery
in the runner does not answer that question. The original capture had 30 failed
of 31 accepted commands and no completed transfers (one pending at capture end);
the bench runs improved to 5 failed of 300 each, against a different peer.

A separate simultaneous receive comparison (`fec-comparative-gate.json`)
ran for 302 seconds: exact loss 0% RadiANT versus 1.1647% SDK ANT. Its unchanged
1.5% absolute and SDK + 0.2 percentage-point comparative gates passed. This
separate result does not override either failed paired-run gate.

All 29 selected assignment/request/serial-number conformance cases had no
unexpected archive differences. The reset startup reason and intentionally
unadvertised serial capability remain the documented archive differences.
Runtime USB serial `3D55F77818BE772A` enumerated with version `RADIANT0.01B00`,
32 channels and 3 networks. The dongle was left running the tested 200 us image.

Local DFU ZIP: `dist/radiant_dongle_nrf52840dongle_fec.zip`.
SHA-256: `679fa432758ee29534f507877166d26a0774bec9432a55cd6b1cfadf4fe40767`.
Firmware BIN: `dc088834acfb2b018fb27298976ee3f562a992799d0b2456dfb4d0907f9d7607`.
Firmware HEX: `095b71886beacce9131e3f1abfbde7b71badf83a8ac111f5022b67456b375899`.

## Firmware changes and timing limits

The transfer engine refreshes its acquired identity for ACK and burst transfers,
while retaining configured wildcards for later acquisition. Slave commands wait
for an actually received master slot; one copied pending command per channel
prevents overlap. The wait expires after the greater of 500 ms and two channel
periods. Tracking resumes after completion or failure. Existing measured
turnaround constants and sender retry policy remain in effect. Counters separate
scheduling/radio failure, missing ACK and invalid ACK.

The follow-on grant reserve is **4,150 us**, derived as
2,190 (slave slot reply) + 1,560 (ACK reply) + 250 (guard) + 150 (frame).
This is separate from the **2,000 us merge-span cap**: one limits a possible
exchange after the window, the other limits the receive union. A new build
assertion caps the exchange reserve at 4,200 us before any uint16 conversion.
A maximum-width 2,000 us merge plus 4,150 us follow-on asks MPSL for 7,400 us
including its 250 us head, 250 us tail and 750 us fixed end margins. Merged
members contribute their maximum reserve, never their sum. A leader's own
window can exceed the merge-span cap; this is not a global grant-duration cap.

The direct backend used by the dongle ignores follow-on metadata; the reserve
does not extend its receive window or hold the radio for 4.15 ms after every
receive. On MPSL, the grant does create scheduling pressure even if unused,
and the backend releases unused air before delivering a terminal callback.
The master's turnaround currently uses the same conservative 4,150 us reserve,
although its one-reply requirement is smaller. The new gate regression checks
the actual 7,400 us MPSL request and early release after an empty receive.
Actual MPSL coexistence throughput with this reserve has not been measured.

The **200 us dispatch margin** applies to scan chunks, ED chunks, and tracked
receives truncated before a transmit. Untruncated tracked windows retain their
bounds; exact on-air transmit deadlines retain their timestamps. Each truncated
receive can sacrifice another 200 us of listening, and a scan chunk near the
1,000 us useful-gap minimum can disappear altogether. This cost must not be
described as exclusive to gap fillers. The original delayed-terminal regression
fails with 100 us and passes with 200 us. A prior real-radio diagnostic recorded
an active scheduling miss at 100 us, which drove this change; that failure is
not attributed to RF noise.

An eight-sensor deterministic test now sweeps 41 relative phases across the
pairwise merge boundary with background scanning, for both 1,960 and 4,150 us
reserves. It verifies continued receptions and no missed slots, and records
granted scan listening time. Another boundary check demonstrates that a frame
in the sacrificed tracked-RX tail is lost while the exact transmit stays on time.
A DK A/B build with only the dispatch margin changed from 200 to 0 us
measured scan listening of 478,240..480,240 us versus 481,040..483,040 us in
499,392 us: a 2,800 us reduction, or 0.5607 percentage points of scan duty.
Both reserve variants produced identical scan time at each tested phase, with
16/16 sensor frames and zero missed slots. The 0 us image is a deterministic
measurement baseline, not a deployable or RF-validated fix. These tests describe
scheduler geometry, not RF/discovery measurements. A real
multi-sensor discovery/sweep-throughput A/B and MPSL coexistence measurement
remain outstanding; the three-sensor USB run cannot establish them. The existing
three-sensors-within-one-arm-lead capacity limit is also retained in its own
regression rather than claimed to be eliminated.

## Pending command concurrency and reset

`api_sched_done` can run in thread or interrupt context. Thread-context
`pass()` and cancel/replacement notifications hold the event critical section.
For radio completions, the owning operation ends before the terminal callback,
and its next operation is not armed until completion processing returns. A
fresh frame cannot preempt that callback and replace its pending-slot timestamp.
Host pending-command mutations also hold the event critical section. This
ordering is now documented beside the read; a backend changing callback
serialization must preserve it. Fake-radio tests exercise logic and ordering,
not actual interrupt preemption.

The single-terminal-outcome guarantee applies to accepted commands **within an
active stack session**. Close/unassign cancel a pending command with the existing
failure semantics. `antr_stack_reset` deliberately discards pending commands and
flushes queued events while tearing down host/channel state. It can therefore
end a session without a transfer terminal event; callers must retire their
pre-reset ledger. The existing reset regression explicitly pins this exception.

## Decoder and capture diagnostics

The decoder reports bad checksum candidates, discarded bytes, incomplete ANT
tails, capture truncation and nonzero USB status separately. Streams are isolated
by bus/device/endpoint/direction. At EOF it now searches beyond an incomplete
false-sync candidate for complete checksum-valid frames and reports
`incomplete_candidates_resynchronized`. During normal reassembly it waits for
later URBs, preserving split frames whose payload contains a valid-looking frame.
Recovered frames contribute to broadcast and transfer summaries; unresolved
accepted commands remain pending. EOF recovery is heuristic when corruption
contains a coincidental valid checksum, as is ordinary byte resynchronization.

Both original captures remain in the local captures directory, including the
second capture. Their hashes and valid counts remain unchanged:
1,858 messages in `zwift-20261018-224924.pcap`, 2,674 in
`zwift-20261018-225653.pcap`. Neither contains bad checksum candidates,
incomplete tails, truncated records or nonzero USB status. Zero padding is
reported as discarded bytes. The capture script supports starting mid-session;
missing startup/configuration limits interpretation and file size proves nothing
about ANT validity.

`CHANNEL_IN_WRONG_STATE` remains a recoverable refusal when Zwift reassigns
its still-assigned scan channel before unassigning it. Zwift then unassigns and
continues. Serial-number requests remain intentionally unsupported with the
same response and capability bits. Neither response is changed to hide a trace.

## Verification and remaining acceptance work

Original deterministic checks: 759 host tests, 804 lower-level tests / 44 suites,
41 API tests and 28 gate tests passed. The final 30-command nRF53 diagnostic had
zero active/cleanup scheduler misses, radio failures, missing/invalid ACKs or
watchdogs; one pending command expired without a master opener. Earlier failures,
interrupted runs, counting-method corrections and 100 us results remain in the
local evidence bundle; they do not substitute for the final failed USB gates.

Review follow-up checks passed: 763 host tests, 805 lower-level tests / 44
suites (at 200 us, plus the diagnostic 0 us A/B), 41 API tests and 29 gate tests.
Both original captures retain every valid message line. The dongle rebuild's BIN
hash exactly matches the tested image, so these comments/assertions require no
reflash. A fresh USB probe passed and the nRF53 app-core UART companion was
restored. Machine-readable review results and raw console logs remain in the
local gitignored evidence directory. No new paired-run RF acceptance pass is
claimed. The unresolved items
are the failed numerical gates, delivered-but-failed commands and potential
application retries, hardware sweep-throughput cost, and MPSL coexistence cost.
Commercial trainer compatibility, the brief Zwift popup and the missing trainer
channel in the second capture remain for the deferred Zwift/trainer session.
