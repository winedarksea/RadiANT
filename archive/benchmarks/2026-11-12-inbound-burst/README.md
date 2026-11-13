# Inbound multi-packet bursts on air (2026-11-12)

Branch `feat/inbound-bursts`. Pro Micro clone (`EE6986854FD72803`, RadiANT nRF
backend, `dist/radiant_dongle_promicro.uf2`) against the Garmin USB-m (`006`),
both at their default power, same desk, same window. Room loss was not
measured separately; the control leg is the comparison.

    python tools/ant_ack_pair.py --master-serial <originator> \
        --slave-serial <receiver> --master-bursts 100 --burst-packets 8 \
        --json-bursts <leg>.json

| leg | originator -> receiver | completed (originator's TX_COMPLETED) | receiver whole bursts | receiver RX_FAILED |
|---|---|---|---|---|
| A1 | Garmin -> RadiANT | 96/100 | 96 | 2 |
| C  | RadiANT -> Garmin (control) | 97/100 | 97 | n/a (Garmin) |
| A2 | Garmin -> RadiANT | 98/100 | 98 | 0 |

Pass: inbound legs are inside the control's range, the originator's completion
count equals the receiver's whole-burst count in both, no packet was passed up
twice (0 sequence gaps), and the 2 RX_FAILED in A1 match its 2 broken bursts.

## The first image failed every burst

The first build answered every inbound packet at +1560 us. 20/20 Garmin bursts
failed; the receiver got packet 0 of 17 and raised RX_FAILED 17 times. A
master's packet 0 (0x8A, slot opener) is answered on the slave's slot-reply
turnaround, as a Garmin slave does (+2209/+2218 us,
`archive/captures/radio/2026-11-11-master-burst-garmin-ack.log`). Answering
0x8A at `RADIANT_TRANSFER_OPENER_REPLY_US` (2190) gave 19/20 at once, then the
table above. Not sniffed: the fix is inferred from the outcome change and the
2026-11-11 capture, not from a capture of the Garmin master's window.

0xAA (acknowledged data from a master) still gets +1560, unchanged by design.
Whether a Garmin master hears that is untested on air.

Logs: `A1.log`, `C.log`, `A2.log` and the matching JSON.
