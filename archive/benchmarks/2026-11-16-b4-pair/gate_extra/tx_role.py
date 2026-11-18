"""B4 gate_extra: one end of the transmit-path workload. Bench script only.

    python tx_role.py pm <json_out> <start_at_epoch> <seconds>
    python tx_role.py ti <json_out> <start_at_epoch> <seconds>

pm: the Pro Micro (USB EE6986854FD72803). ch0 is an ANT+ master (CSC #25001,
    type 0x79, period 8086, trans 5, payloads from ant_sim's CSC sensor, paced
    by EVENT_TX); every ACK_EVERY-th EVENT_TX it loads an acknowledged message
    (0x4F, byte 0 = sequence) instead of a broadcast and counts the
    TRANSFER_TX_COMPLETED / FAILED outcome. ch1/ch2 track the 5340 HR master
    #24001 (0x78, 8070) and the Dongle power master #23002 (0x0B, 8182) as
    pinned slaves with infinite search timeout. So every master slot (a TX arm,
    plus an ack RX window on acked slots) sits between RX arms on the same radio.
pm-noack: as pm but never sends acknowledged data.
ti: the TI LaunchXL (COM11 @ 57600). ch0..2 track #25001 (the Pro Micro's
    master), #24001 and #23002, counting broadcasts, acknowledged messages
    (distinct / duplicates by sequence byte), RX_FAIL and RX_FAIL_GO_TO_SEARCH.

Both roles open their channels, then count only inside the shared window
[start_at, start_at + seconds), then close and unassign everything.
"""
import json
import random
import sys
import time

TOOLS = r"C:\Users\Colin\ant_dongle\tools"
sys.path.insert(0, TOOLS)

import ant_sim  # noqa: E402
from ant_probe import (EP_OUT, FrameReader, close_device, frame,  # noqa: E402
                       open_device, reset_stack)
from ant_scan import ANT_PLUS_FREQ, ANT_PLUS_KEY  # noqa: E402
from ant_wire import (EVENT_CHANNEL_CLOSED, EVENT_RX_FAIL,  # noqa: E402
                      EVENT_RX_FAIL_GO_TO_SEARCH, EVENT_TRANSFER_TX_COMPLETED,
                      EVENT_TRANSFER_TX_FAILED, EVENT_TX,
                      MESG_ACKNOWLEDGED_DATA_ID, MESG_ASSIGN_CHANNEL_ID,
                      MESG_BROADCAST_DATA_ID, MESG_BURST_DATA_ID,
                      MESG_CHANNEL_ID_ID, MESG_CHANNEL_MESG_PERIOD_ID,
                      MESG_CHANNEL_RADIO_FREQ_ID,
                      MESG_CHANNEL_SEARCH_TIMEOUT_ID, MESG_CLOSE_CHANNEL_ID,
                      MESG_EVENT_ID, MESG_NETWORK_KEY_ID, MESG_OPEN_CHANNEL_ID,
                      MESG_RESPONSE_EVENT_ID, MESG_UNASSIGN_CHANNEL_ID)

PM_SERIAL = "EE6986854FD72803"
MASTER = (25001, 0x79, 8086)
TRACK_N53 = (24001, 0x78, 8070)
TRACK_DONGLE = (23002, 0x0B, 8182)
ACK_EVERY = 8          # one acked message per 8 master slots (~0.5 Hz)
ACK_TIMEOUT_S = 3.0


def command(dev, reader, msg_id, payload, what, timeout=2.0):
    dev.write(EP_OUT, frame(msg_id, payload))
    deadline = time.monotonic() + timeout
    while True:
        r = reader.next_frame(deadline)
        if r is None:
            raise RuntimeError(f"{what}: no answer")
        mid, body = r
        if mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
            if body[2] != 0:
                raise RuntimeError(f"{what}: code {body[2]}")
            return


def open_track(dev, reader, ch, dn, dt, period):
    command(dev, reader, MESG_ASSIGN_CHANNEL_ID, bytes([ch, 0x00, 0x00]), f"assign ch{ch}")
    command(dev, reader, MESG_CHANNEL_ID_ID,
            bytes([ch]) + dn.to_bytes(2, "little") + bytes([dt, 0]), f"id ch{ch}")
    command(dev, reader, MESG_CHANNEL_RADIO_FREQ_ID, bytes([ch, ANT_PLUS_FREQ]), f"freq ch{ch}")
    command(dev, reader, MESG_CHANNEL_MESG_PERIOD_ID,
            bytes([ch]) + period.to_bytes(2, "little"), f"period ch{ch}")
    command(dev, reader, MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([ch, 0xFF]), f"timeout ch{ch}")
    command(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([ch]), f"open ch{ch}")


def close_all(dev, reader, chans):
    for ch in chans:
        try:
            dev.write(EP_OUT, frame(MESG_CLOSE_CHANNEL_ID, bytes([ch])))
        except Exception:  # noqa: BLE001
            pass
    pending = set(chans)
    deadline = time.monotonic() + 3.0
    while pending and time.monotonic() < deadline:
        r = reader.next_frame(deadline)
        if r is None:
            break
        mid, body = r
        if (mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == MESG_EVENT_ID
                and body[2] == EVENT_CHANNEL_CLOSED):
            pending.discard(body[0])
    for ch in chans:
        dev.write(EP_OUT, frame(MESG_UNASSIGN_CHANNEL_ID, bytes([ch])))
        time.sleep(0.05)
    return sorted(pending)


def new_track_stats(key):
    return {"key": key, "broadcasts": 0, "acked": 0, "acked_distinct": 0,
            "acked_dups": 0, "burst": 0, "rx_fail": 0, "go_to_search": 0,
            "other_events": {}, "_last_seq": None}


def main():
    role, out, start_at, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
    is_pm = role.startswith("pm")
    send_acked = role == "pm"
    if is_pm:
        dev = open_device(False, serial=PM_SERIAL)
    else:
        dev = open_device(False, port="COM11", baud=57600)
    reader = FrameReader(dev)
    res = {"role": role, "start_at": start_at, "seconds": seconds}
    try:
        if not reset_stack(dev, reader):
            raise RuntimeError("no startup after reset")
        command(dev, reader, MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "network key")
        tracks = {}
        sensor = None
        if is_pm:
            rng = random.Random(25001)
            sensor = ant_sim.SENSORS["csc"](0, MASTER[0], 5,
                                            ant_sim.Signal(100, 3, rng),
                                            ant_sim.Signal(80, 3, rng))
            assert sensor.device_type == MASTER[1] and sensor.period == MASTER[2]
            if not sensor.bring_up(dev, reader):
                raise RuntimeError("master bring-up failed")
            for ch, (dn, dt, per) in ((1, TRACK_N53), (2, TRACK_DONGLE)):
                open_track(dev, reader, ch, dn, dt, per)
                tracks[ch] = new_track_stats(f"{dn}:0x{dt:02X}")
            chans = [0, 1, 2]
        else:
            for ch, (dn, dt, per) in ((0, MASTER), (1, TRACK_N53), (2, TRACK_DONGLE)):
                open_track(dev, reader, ch, dn, dt, per)
                tracks[ch] = new_track_stats(f"{dn}:0x{dt:02X}")
            chans = [0, 1, 2]
        res["opened_at"] = time.time()

        m = {"event_tx": 0, "broadcasts_loaded": 0, "acked_loaded": 0,
             "acked_completed": 0, "acked_failed": 0, "acked_no_terminal": 0,
             "fallback_sends": 0, "other_events": {}}
        ack_pending_since = None
        seq = 0
        slot_n = 0
        last_master_evt = time.monotonic()
        counting = False
        end_wall = start_at + seconds

        def load_next():
            nonlocal ack_pending_since, seq, slot_n
            slot_n += 1
            if send_acked and counting and slot_n % ACK_EVERY == 0 and ack_pending_since is None:
                seq = (seq + 1) & 0xFF
                dev.write(EP_OUT, frame(MESG_ACKNOWLEDGED_DATA_ID,
                                        bytes([0, seq, 0xAC, 0xA5, 0x5A, 1, 2, 3, 4])))
                ack_pending_since = time.monotonic()
                if counting:
                    m["acked_loaded"] += 1
                return
            if ack_pending_since is not None:
                return  # leave the acked message in the buffer until its outcome
            dev.write(EP_OUT, frame(MESG_BROADCAST_DATA_ID, bytes([0]) + sensor.next_payload()))
            if counting:
                m["broadcasts_loaded"] += 1

        while True:
            now_wall = time.time()
            if now_wall >= end_wall:
                break
            counting = now_wall >= start_at
            r = reader.next_frame(time.monotonic() + 0.1)
            now = time.monotonic()
            if is_pm:
                if ack_pending_since is not None and now - ack_pending_since > ACK_TIMEOUT_S:
                    if counting:
                        m["acked_no_terminal"] += 1
                    ack_pending_since = None
                if now - last_master_evt > 2 * MASTER[2] / 32768.0:
                    if counting:
                        m["fallback_sends"] += 1
                    last_master_evt = now
                    load_next()
            if r is None:
                continue
            mid, body = r
            if mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == MESG_EVENT_ID:
                ch, code = body[0], body[2]
                if is_pm and ch == 0:
                    last_master_evt = now
                    if code == EVENT_TX:
                        if counting:
                            m["event_tx"] += 1
                        load_next()
                    elif code in (EVENT_TRANSFER_TX_COMPLETED, EVENT_TRANSFER_TX_FAILED):
                        if ack_pending_since is not None and counting:
                            m["acked_completed" if code == EVENT_TRANSFER_TX_COMPLETED
                              else "acked_failed"] += 1
                        ack_pending_since = None
                        load_next()
                    elif counting:
                        k = f"0x{code:02X}"
                        m["other_events"][k] = m["other_events"].get(k, 0) + 1
                    continue
                if ch in tracks and counting:
                    t = tracks[ch]
                    if code == EVENT_RX_FAIL:
                        t["rx_fail"] += 1
                    elif code == EVENT_RX_FAIL_GO_TO_SEARCH:
                        t["go_to_search"] += 1
                    else:
                        k = f"0x{code:02X}"
                        t["other_events"][k] = t["other_events"].get(k, 0) + 1
                continue
            if mid in (MESG_BROADCAST_DATA_ID, MESG_ACKNOWLEDGED_DATA_ID, MESG_BURST_DATA_ID):
                ch = body[0] & 0x1F
                if ch not in tracks or not counting:
                    continue
                t = tracks[ch]
                if mid == MESG_BROADCAST_DATA_ID:
                    t["broadcasts"] += 1
                elif mid == MESG_BURST_DATA_ID:
                    t["burst"] += 1
                else:
                    t["acked"] += 1
                    if len(body) >= 2 and body[1] == t["_last_seq"]:
                        t["acked_dups"] += 1
                    else:
                        t["acked_distinct"] += 1
                    t["_last_seq"] = body[1] if len(body) >= 2 else None
        res["window_end"] = time.time()
        if is_pm:
            res["master"] = m
        for t in tracks.values():
            t.pop("_last_seq", None)
            rx = t["broadcasts"] + t["acked"] + t["burst"]
            t["received"] = rx
            t["loss_pct"] = round(100.0 * t["rx_fail"] / (rx + t["rx_fail"]), 3) if rx + t["rx_fail"] else None
        res["tracks"] = {t["key"]: t for t in tracks.values()}
        res["not_closed"] = close_all(dev, reader, chans)
    finally:
        close_device(dev)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    print(json.dumps(res, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
