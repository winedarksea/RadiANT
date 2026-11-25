# Garmin ANTUSB-m: RSSI field byte 2 with proximity search off (2026-11-25)

The question (TODO, the RSSI byte-2 item): does a real Garmin stick send -128
(0x80) as the threshold configuration value with proximity search off, as
AN16 section 4.3.3 says and radiant now does?

**Answer: this stick cannot say.** Its RSSI extended field is not the AN16
format, so it carries no threshold configuration byte to compare.

## Rig

- DUT: Garmin ANTUSB-m, USB `0FCF:1009` serial `006` (libusb0), via
  `tools/ant_probe.py`'s USB transport.
- Signal: the ANT+ HR straps already on the air: #12345 (Pro Micro hrtx,
  about -37 dBm at the TI) and #8265 (real strap, -73..-92 dBm). RF 57, period
  8070, ANT+ key, slave with a wildcard device number, no master.
- Commands: `garmin_raw.py` here (raw dump) and
  `tools/ant_prox_onair.py --dut-serial 006 --dev-type 120 --trans-type 0
  --rf 57 --period 8070 --levels=fixed --device 0 --search-timeout 4 --mode
  wire --mode ladder --cal 20 --trials 10 --pass-bin 3 --seed 2521`
  (`raw/garmin_hr.jsonl`).

## What the stick sends

Broadcast data (0x4E) bodies, verbatim:

| lib config | body |
|---|---|
| 0x40 (RSSI) | `00 00 ff ff ff ad 3d 9b 5d` `40` `10 01 6d 00` |
| 0xE0 (id + RSSI + timestamp) | `00 00 ff ff ff d7 73 68 78` `e0` `39 30 78 05` `10 00 69 00` `72 c5` |

- **Measurement type 0x10, not 0x20 (dBm).** AN16 section 4.3.1 / MP
  7.1.1.2.1 say to stop decoding when the type is not 0x20, so there is no dBm
  value and no threshold configuration value to read.
- **The RSSI field is 4 bytes, not 3.** With 0xE0 the body is 20 bytes: 1 +
  8 + flag + 4 (channel ID: 12345, type 0x78, trans 5) + **4** + 2
  (timestamp). The MP and AN16 give 3. A parser written to the documents
  misplaces the timestamp after a Garmin RSSI field.
- The third byte sits at 0x68-0x6d whether proximity search is off, bin 10
  or custom; meaning unknown (not decoded, per the type rule). On the acquiring
  frame under custom 0x80/60 it was 0x00 once, then 0x68 again.

## Its 0x71 answers (for comparison with radiant)

- CAPABILITIES `080800ba3600df`: advanced options 2 = 0x36, prox bit set.
- **Every** 0x71 form answered NO_ERROR: `[0,0x80,60]`, `[0,11]`,
  `[0,0x80]` (2 bytes), `[0,10]`, `[0,1]`, `[0,0x80,0]`, `[0,0x80,128]`,
  `[0,3,99]`, `[0,0]`. radiant refuses 11, a two-byte 0x80, custom 0 and
  custom 128 with INVALID_PARAMETER_PROVIDED. The MP gives the range 0-10 and
  no error behaviour, so both readings are allowed. radiant's refusal is the
  stricter reading and is kept: a host that sends 11 gets told.
- Bin 3 enforced: with prox off it acquired the near strap 20/20; with bin 3
  sent before each open it acquired 2/10, and 0 in an 8 s search. Its bins are
  on its own scale. MP section 6.4 says bins are design-dependent, so this is
  not a disagreement with radiant's table.

## Verdict

The 0x80 "off" value stays as the documents give it (owner ruling,
2026-11-25). This stick is not a reference for it, because it never emits a
type-0x20 RSSI field. A stick or SoC that reports dBm (type 0x20) would be
needed to confirm it on the wire.

`tools/ant_prox_onair.py`'s `ext_fields()` read the Garmin's 0x10 field as
dBm (rssi 0, threshold 104). It now decodes only a type-0x20 field and
otherwise reports None, as the documents say.
