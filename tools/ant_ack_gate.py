# SPDX-License-Identifier: Apache-2.0
"""Two-ended ACK acceptance runner, invoked by ant_ack_pair.py.

Counts peer reception independently of originator outcomes. Synthetic profiles
stimulate the transport and scheduler; they do not emulate trainer mechanics.
"""
import json
import random
import statistics
import threading
import time
import tomllib
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path

import ant_wire as w
from ant_probe import EP_OUT, FrameReader, close_device, frame, open_device, reset_stack
from ant_sim import HeartRateSensor, Signal, StandardPowerSensor
from ant_verify import extended_fields

KEY = bytes.fromhex('b9a521fbbd72c345')


def command(dev, reader, msg, body):
    dev.write(EP_OUT, frame(msg, body))
    end = time.monotonic() + 3
    while time.monotonic() < end:
        answer = reader.next_frame(end)
        if (answer and answer[0] == w.MESG_RESPONSE_EVENT_ID
                and len(answer[1]) >= 3 and answer[1][1] == msg):
            if answer[1][2]:
                raise RuntimeError(f'command 0x{msg:02x} refused: {answer[1].hex()}')
            return
    raise RuntimeError(f'command 0x{msg:02x} had no response')


def channel(dev, reader, ch, master, number, dtype, period, trans_type=5, scan=False):
    command(dev, reader, w.MESG_ASSIGN_CHANNEL_ID,
            bytes([ch, 0x10 if master else 0, 0]) + (b'\x01' if scan else b''))
    command(dev, reader, w.MESG_CHANNEL_ID_ID,
            bytes([ch, number & 255, number >> 8, dtype, trans_type]))
    command(dev, reader, w.MESG_CHANNEL_MESG_PERIOD_ID,
            bytes([ch, period & 255, period >> 8]))
    command(dev, reader, w.MESG_CHANNEL_RADIO_FREQ_ID, bytes([ch, 57]))
    command(dev, reader, w.MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([ch, 255]))
    command(dev, reader, w.MESG_OPEN_CHANNEL_ID, bytes([ch]))


def assess(report, ceiling):
    reasons = []
    if report['seconds'] < 300 or report['sent'] < 300:
        reasons.append('acceptance requires at least 300 seconds and 300 commands')
    if report['accepted'] != report['sent']:
        reasons.append('not every command was accepted')
    if report['completed'] / max(1, report['sent']) < 0.99:
        reasons.append('acknowledged transfer success below 99%')
    if report['terminal_events'] != report['accepted']:
        reasons.append('missing or duplicate terminal events')
    if report['duplicate_commands'] or report['unexpected_commands']:
        reasons.append('duplicate or unexpected command delivery at peer')
    if report['peer_received'] < report['completed']:
        reasons.append('originator reported success for an undelivered command')
    if report['protocol_errors'] or report['errors']:
        reasons.append('USB, serial framing or runner error')
    if not report['receive']:
        reasons.append('no receive measurement')
    for ch, loss in report['receive'].items():
        if not loss['expected'] or loss['loss_pct'] > ceiling:
            reasons.append(f'channel {ch} receive loss exceeds {ceiling}% or no stimulus')
    return reasons


def staged_payload_loss(transmitted, received, channel_id, start, finish):
    """Match individual payloads across host latency without clipping replies.

    Select TX slots inside the guarded interval, then allow each matching RX
    its own latency margin. Cutting both host streams at identical instants
    falsely loses a frame whose RX notification crosses either boundary.
    Each RX can satisfy at most one TX, including repeated common pages.
    """
    tx = [(ts, data) for ts, ch, data in transmitted
          if ch == channel_id and start + 0.5 <= ts <= finish - 0.5]
    rx = [(ts, data) for ts, ch, data in received if ch == channel_id]
    used = set()
    missing = Counter()
    for ts, data in tx:
        for index, (rx_ts, rx_data) in enumerate(rx):
            if index not in used and rx_data == data and abs(rx_ts - ts) <= 0.5:
                used.add(index)
                break
        else:
            missing[data.hex()] += 1
    count = sum(missing.values())
    return dict(expected=len(tx), received=len(used), missing=count,
                loss_pct=100 * count / max(1, len(tx)), missing_payloads=dict(missing))


def receive_loss(transmitted, received, channel_id, start, finish):
    """Count actual master TX events versus individual channel broadcasts.

    A host-staged payload can be overwritten before it goes on air. Its absence
    is a producer diagnostic, not proof that the receiver missed a transmission.
    Match notifications within 100 ms, less than half the shortest bench period,
    so a neighbouring slot cannot satisfy a missing broadcast. Guard sample
    boundaries and consume each received frame only once.
    """
    tx = [(ts, data) for ts, ch, data in transmitted
          if ch == channel_id and start + 0.5 <= ts <= finish - 0.5]
    rx = [ts for ts, ch, _ in received if ch == channel_id]
    used = set()
    missing = []
    for ts, data in tx:
        candidates = [(abs(at - ts), index) for index, at in enumerate(rx)
                      if index not in used and abs(at - ts) <= 0.1 + 1e-9]
        if candidates:
            used.add(min(candidates)[1])
        else:
            missing.append(dict(time_s=ts - start, staged_payload=data.hex()))
    staged = staged_payload_loss(transmitted, received, channel_id, start, finish)
    return dict(expected=len(tx), received=len(used), missing=len(missing),
                loss_pct=100 * len(missing) / max(1, len(tx)),
                unmatched_tx_events=missing, staged_payload_missing=staged['missing'],
                staged_payload_loss_pct=staged['loss_pct'])


def named_events(counts):
    names = {getattr(w, name): name for name in dir(w)
             if name.startswith('EVENT_') and isinstance(getattr(w, name), int)}
    return {f'ch{ch}.{names.get(code, hex(code))}': count
            for (ch, code), count in sorted(counts.items())}


def run(args, *, phase_hook=None):
    """Run the pair; a bench hook can snapshot statistics over a separate probe."""
    report = dict(recorded=datetime.now(UTC).isoformat(),
                  master=args.master_port or args.master_serial,
                  slave=args.slave_port or args.slave_serial,
                  slave_trans_type=args.slave_trans_type, zwift_load=args.zwift_load,
                  tx_power_level=args.tx_power,
                  receive_measurement='master EVENT_TX matched once to channel RX within 100 ms; staged payload gaps separate',
                  seconds=0, sent=0, accepted=0, completed=0, failed=0,
                  terminal_events=0, peer_received=0, duplicate_commands=0,
                  unexpected_commands=0, protocol_errors=0, errors=[], receive={}, events={})
    devices, readers, threads = [], [], []
    stop = threading.Event()
    received, transmitted, peer_commands = [], [], []
    sent_payloads = []
    start = finish = None
    fec = 1 if args.zwift_load else 0
    configs = {fec: (args.device, 0x11, 8192)}
    if args.zwift_load:
        configs.update({2: (args.device + 1, 0x0B, 8182),
                        3: (args.device + 2, 0x78, 8070)})
    rng = random.Random(1)
    power = StandardPowerSensor(2, args.device + 1, 5, Signal(150, 0, rng), Signal(80, 0, rng))
    hr = HeartRateSensor(3, args.device + 2, 5, Signal(150, 0, rng), Signal(80, 0, rng))
    sequence = Counter()
    event_counts = Counter()
    signal = {ch: [] for ch in configs}

    def payload(ch):
        if ch == fec:
            sequence[ch] += 1
            n = sequence[ch]
            return bytes([16, 25, n & 255, (n >> 8) & 255, 0, 0, 255, 0x24])
        return (power if ch == 2 else hr).next_payload()

    def observe_slave(answer):
        if answer is None:
            return
        msg, body = answer
        if not body:
            return
        ch = body[0] & 31
        if msg == w.MESG_BROADCAST_DATA_ID and ch in configs and len(body) >= 9:
            received.append((time.monotonic(), ch, bytes(body[1:9])))
            radio = extended_fields(body)
            if 'rssi_dbm' in radio:
                signal[ch].append(radio['rssi_dbm'])
        if (msg == w.MESG_RESPONSE_EVENT_ID and len(body) >= 3
                and body[1] == w.MESG_EVENT_ID):
            event_counts[(ch, body[2])] += 1
        if msg == w.MESG_RESPONSE_EVENT_ID and len(body) >= 3 and ch == fec:
            if body[1] == w.MESG_ACKNOWLEDGED_DATA_ID:
                if body[2] == 0:
                    report['accepted'] += 1
                else:
                    report['errors'].append(f'ACK command refused: {body.hex()}')
            elif (body[1] == w.MESG_EVENT_ID
                  and body[2] in (w.EVENT_TRANSFER_TX_COMPLETED, w.EVENT_TRANSFER_TX_FAILED)):
                report['terminal_events'] += 1
                report['completed' if body[2] == w.EVENT_TRANSFER_TX_COMPLETED else 'failed'] += 1

    try:
        for port, serial in ((args.master_port, args.master_serial),
                             (args.slave_port, args.slave_serial)):
            dev = open_device(True, port=port, serial=serial, baud=args.master_baud)
            devices.append(dev)
            reader = FrameReader(dev, timeout_ms=50)
            readers.append(reader)
            if not reset_stack(dev, reader):
                raise RuntimeError(f'endpoint {port or serial} did not reset')
            command(dev, reader, w.MESG_NETWORK_KEY_ID, bytes([0]) + KEY)
            command(dev, reader, w.MESG_RADIO_TX_POWER_ID, bytes([0, args.tx_power]))
        master, slave = devices
        mr, sr = readers
        command(slave, sr, w.MESG_ANTLIB_CONFIG_ID, bytes([0, w.LIB_CONFIG_ALL_EXT_FIELDS]))
        for ch, (number, dtype, period) in configs.items():
            channel(master, mr, ch, True, number, dtype, period)
            channel(slave, sr, ch, False, number, dtype, period,
                    args.slave_trans_type if ch == fec else 0)
        if args.zwift_load:
            channel(slave, sr, 0, False, 0, 0, 8192, 0, scan=True)
        current = {ch: payload(ch) for ch in configs}
        for ch, data in current.items():
            master.write(EP_OUT, frame(w.MESG_BROADCAST_DATA_ID, bytes([ch]) + data))

        def peer():
            try:
                while not stop.is_set():
                    answer = mr.next_frame(time.monotonic() + 0.1)
                    if answer is None:
                        continue
                    msg, body = answer
                    if not body:
                        continue
                    ch = body[0] & 31
                    if (msg == w.MESG_RESPONSE_EVENT_ID and len(body) >= 3
                            and body[1:] == bytes([w.MESG_EVENT_ID, w.EVENT_TX]) and ch in configs):
                        transmitted.append((time.monotonic(), ch, current[ch]))
                        current[ch] = payload(ch)
                        master.write(EP_OUT, frame(w.MESG_BROADCAST_DATA_ID,
                                                  bytes([ch]) + current[ch]))
                    elif msg == w.MESG_ACKNOWLEDGED_DATA_ID and ch == fec:
                        peer_commands.append(bytes(body[1:9]))
                    elif (msg == w.MESG_RESPONSE_EVENT_ID and len(body) >= 3
                          and body[1] != w.MESG_EVENT_ID and body[2] != 0):
                        report['errors'].append(f'peer command refused: {body.hex()}')
            except Exception as exc:
                report['errors'].append(f'peer: {type(exc).__name__}: {exc}')
                stop.set()

        thread = threading.Thread(target=peer, daemon=True)
        thread.start()
        threads.append(thread)
        deadline = time.monotonic() + args.acquire_seconds
        while time.monotonic() < deadline:
            observe_slave(sr.next_frame(time.monotonic() + 0.1))
            if all(sum(ch == item[1] for item in received) >= 4 for ch in configs):
                break
        else:
            raise RuntimeError('not all slave channels acquired four broadcasts')

        if phase_hook is not None:
            phase_hook('before_commands')
        start = time.monotonic()
        next_send, pending_since = start, None
        duration = max(args.seconds, args.sends * args.interval)
        while time.monotonic() < start + duration or pending_since is not None:
            now = time.monotonic()
            if stop.is_set():
                raise RuntimeError('peer reader stopped')
            if pending_since is not None and now - pending_since > 2:
                raise RuntimeError('accepted command has no terminal event after two seconds')
            if now >= next_send and pending_since is None and report['sent'] < args.sends:
                data = bytearray.fromhex('33ffffffff204eff')
                data[5:7] = (20000 + report['sent']).to_bytes(2, 'little')
                sent_payloads.append(bytes(data))
                slave.write(EP_OUT, frame(w.MESG_ACKNOWLEDGED_DATA_ID, bytes([fec]) + data))
                report['sent'] += 1
                pending_since = now
                next_send = start + report['sent'] * args.interval
            before = report['terminal_events']
            observe_slave(sr.next_frame(time.monotonic() + 0.05))
            if report['terminal_events'] != before:
                pending_since = None
                if report['sent'] % 25 == 0:
                    print(f"{report['sent']}/{args.sends}: completed={report['completed']} "
                          f"failed={report['failed']}", flush=True)
        finish = time.monotonic()
        report['seconds'] = finish - start
        if phase_hook is not None:
            phase_hook('after_commands')
        # Allow the peer's independently read terminal data to reach the host.
        until = time.monotonic() + args.settle_seconds
        while time.monotonic() < until:
            observe_slave(sr.next_frame(time.monotonic() + 0.05))
    except Exception as exc:
        report['errors'].append(f'{type(exc).__name__}: {exc}')
    finally:
        if start is not None and finish is None:
            finish = time.monotonic()
        stop.set()
        for thread in threads:
            thread.join(timeout=2)
        expected = Counter(sent_payloads)
        actual = Counter(peer_commands)
        report['peer_received'] = sum(actual.values())
        report['duplicate_commands'] = sum(max(0, n - 1) for n in actual.values())
        report['unexpected_commands'] = sum((actual - expected).values())
        report['pending'] = max(0, report['accepted'] - report['terminal_events'])
        if start is not None and finish is not None:
            report['seconds'] = finish - start
            for ch in configs:
                report['receive'][str(ch)] = receive_loss(transmitted, received, ch, start, finish)
                report['receive'][str(ch)]['broadcasts_received'] = sum(
                    c == ch and start <= ts <= finish for ts, c, _ in received)
                if signal[ch]:
                    report['receive'][str(ch)]['rssi_dbm'] = dict(
                        mean=statistics.fmean(signal[ch]), min=min(signal[ch]), max=max(signal[ch]))
        for dev in devices:
            try:
                for ch in configs:
                    dev.write(EP_OUT, frame(w.MESG_CLOSE_CHANNEL_ID, bytes([ch])))
                if args.zwift_load:
                    dev.write(EP_OUT, frame(w.MESG_CLOSE_CHANNEL_ID, b'\x00'))
            except Exception as exc:
                report['errors'].append(f'cleanup: {type(exc).__name__}: {exc}')
            finally:
                close_device(dev)
        report['protocol_errors'] = sum(getattr(r, 'checksum_errors', 0) +
                                        getattr(r, 'discarded_bytes', 0) for r in readers)
        report['events'] = named_events(event_counts)
        if phase_hook is not None:
            phase_hook('after_cleanup')
    gates = tomllib.loads((Path(__file__).parent / 'ab_gates.toml').read_text())
    report['gate_failures'] = assess(report, gates['gates']['loss_exact']['absolute_ceiling_pct'])
    report['pass'] = not report['gate_failures']
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return int(bool(report['errors']) or (args.gate and not report['pass']))
