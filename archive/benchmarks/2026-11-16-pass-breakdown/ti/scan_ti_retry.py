"""tools/ant_scan_rate.py for the TI's UART, with ONE retry of a command that
got no answer.

Why: in the first C2-TI attempt (aborted_leg1_scan_csum/) the TI child died on
"open scan -> no answer" after a host->TI frame failed its checksum on the TI
(0xF6 host_csum 0 -> 1, host_desync 0 -> 5). The TI drops a bad frame, so the
command never arrived; resending it is what any host does. Every retry is
printed with its time so the README can count them. A retry that is answered
with a non-zero code still fails the run, as the stock tool would.

Run exactly like ant_scan_rate.py in single-device mode (one --port), with the
hidden --start-at so it shares a start time with the Pro Micro's own process.
"""
import sys
import time

sys.path.insert(0, r"C:\wt_c2\tools")
import ant_scan_rate as asr  # noqa: E402

_orig = asr._command
T0 = time.time()


RETRIES = 4


def _command_retry(dev, reader, msg_id, payload, timeout=2.0):
    # Second attempt (aborted_leg1b_scan_csum/): ONE resend was not enough,
    # the resend got no answer either (host_csum 1, host_desync 6) - a
    # corrupted length byte can make the TI's parser swallow the next frame
    # while it hunts for SYNC. So: up to RETRIES resends, 0.5 s apart.
    code = _orig(dev, reader, msg_id, payload, timeout)
    n = 0
    while code is None and n < RETRIES:
        n += 1
        print(f"RETRY {n} t={time.time() - T0:.1f}s msg 0x{msg_id:02X} "
              f"payload {payload.hex()} (no answer)", flush=True)
        time.sleep(0.5)
        code = _orig(dev, reader, msg_id, payload, timeout)
        print(f"RETRY {n} result: {code}", flush=True)
    return code


asr._command = _command_retry

if __name__ == "__main__":
    sys.exit(asr.main())
