# 2026-11-24 — tracked channel vs a same-number sensor of another transmission type

Receiver: TI LaunchXL (COM11 @57600), `ant_verify.py --profile heart-rate
--device-number 12345 --trans-type 0` (wildcard type).
Genuine sensor: Pro Micro `hr_transmitter`, HR #12345, transmission type 5, 120 bpm.
Impostor: nRF54L15 DK (apps/dongle diag, COM4 @115200) as `ant_sim.py` master,
HR #12345, transmission type 0x15 (21), 77 bpm, `--period-ppm -600`, started
15 s after the receiver so the receiver acquires the Pro Micro first.
Script: `run.sh <label> <ppm> <seconds>`.

At -600 ppm the impostor slides 148 us per period against the genuine sensor,
one full phase sweep in ~410 s, so a 480 s run is guaranteed to cross; the
receive guard is ~100-200 us, so the impostor lingers inside it long enough to
be heard clean. (`before_430562b`, +3000 ppm / 200 s, crossed at 740 us per
period and heard nothing: that rate jumps the guard. It is kept as the
negative example.)

| run | image | genuine (0x78 = 120 bpm) | impostor (0x4d = 77 bpm) |
|---|---|---|---|
| before600_430562b | ship 430562b (sha256 8a5c13fe…) | 2020 | **8**, consecutive at 482.3-484.0 s (slot followed the impostor ~2 s) |
| after600_d94d07e | ship d94d07e (sha256 2cf25860…) | 2049, loss 0.09 % | **0** |

Count with: `grep -v '^#' X.antcap | awk '{print substr($4,15,2)}' | sort | uniq -c`.
Room loss floor is 5-8 % on some links; the verdict is presence/absence of
foreign payloads, not loss.
