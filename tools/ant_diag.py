#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Read the 0xF7 diagnostic pages (MESG_RADIANT_DIAG, ADR 0023).

    python tools/ant_diag.py --port COM11 --baud 57600 --json before.json
    ... run a workload ...
    python tools/ant_diag.py --port COM11 --baud 57600 --json after.json
    python tools/ant_diag.py --diff before.json after.json

Each page is requested with MESG_REQUEST [page, 0xF7] and answers 36 bytes:
[0] page, [1] version, [2] flags, [3] backend id, then eight u32 LE counters.
The counters wrap rather than saturate, so the useful reading is a
before/after difference (--diff). A field reading 0xFFFFFFFF is "not
measured" on this build or backend, never zero.

No reset is sent: like ant_health.py, this is meant to run against a live
session, and the counters count from boot.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_wire import (  # noqa: E402
    MESG_RADIANT_DIAG_ID,
    MESG_REQUEST_ID,
    MESG_RESPONSE_EVENT_ID,
    RESPONSE_NO_ERROR,
)

NOT_MEASURED = 0xFFFFFFFF
BODY_LEN = 36
VERSION = 1
BACKENDS = {0: "null", 1: "nrf", 2: "cc26xx", 0xFF: "fake"}

# Page -> eight field names, in order. radiant/include/radiant/radiant_diag.h
# and protocol/ant_wire.yaml's 0xF7 entry are the authority; test_ant_diag.py
# checks the count and that the page numbers match the header.
PAGES: dict[int, tuple[str, list[str]]] = {
    0: ("api", ["sched_dones", "sched_missed", "sched_failed",
                "sched_denied", "slots_missed", "slots_denied",
                "rx_sched_events", "xfer_no_ack"]),
    1: ("master_rx", ["mrx_posted", "mrx_refused", "mrx_skipped",
                      "turn_dropped", "mrx_done_ok", "mrx_done_missed",
                      "mrx_done_other", "mrx_frames"]),
    2: ("transfer", ["cont_submits", "cont_late", "turn_min_us",
                     "turn_max_us", "tx_expired", "tx_margin_min_us",
                     "sched_arm_rejected", "sched_missed"]),
    3: ("backend_a", ["rx_posted", "rx_refused", "term_ok", "term_empty",
                      "term_error", "status_err", "status_last",
                      "sync_entries"]),
    4: ("backend_b", ["sw_addr_mismatch", "window_drops", "crc_fail",
                      "alt_sync_hits", "rx_nok", "rx_buf_full",
                      "drain_max_us", "min_lead_us"]),
    5: ("search_a", ["windows", "sets_advanced", "sweeps", "frames_ok",
                     "crc_fail", "acquired", "scan_reports", "timeouts"]),
    6: ("search_b", ["cache_steers", "cache_acquires", "permit_rejected",
                     "rssi_rejected", "decode_failed", "bad_filter_index",
                     "late_events", "sets_advanced_uncovered"]),
    # ADR 0022 gate 3. The first three are the backend table's tail
    # (radiant_radio_diag_idx after min_lead_us); 0xFFFFFFFF on TI and null.
    7: ("handoff", ["srch_cut_close", "srch_cut_abort", "srch_end_near_close",
                    "trk_open_late", "trk_open_late_max_us",
                    "trk_missed_behind_scan", "tx_start_missed",
                    "rx_start_missed"]),
    # 2026-11-13 timing page. Fields 0-3 and 7 are the backend table's tail
    # after srch_end_near_close; 0xFFFFFFFF on nRF. 4-6 are the scheduler's.
    # crc_max_us on cc26xx is preemption-free from 2026-11-16 (ADR 0023);
    # an older image's value includes preempting interrupts.
    8: ("timing", ["drain_max_frames", "frame_max_us", "crc_max_us",
                   "rx_cb_max_us", "pass_max_us", "pass_steps_max",
                   "pass_over_100us", "drain_over_250us"]),
    # 2026-11-15 pass breakdown. 0-4 are the scheduler's per-pass sums on the
    # cycle clock (0xFFFFFFFF where it does not run, e.g. native_sim); 5-7 are
    # the backend table's tail after rx_start_missed.
    9: ("pass", ["sweep_max_us", "decide_max_us", "arm_call_max_us",
                 "done_cb_max_us", "book_max_us", "pass_to_arm_max_us",
                 "arm_slack_min_us", "early_etime"]),
    # 2026-11-15 receive integrity. 0-3 are radiant_api_stats' CRC repair
    # counters (0xFFFFFFFF without CONFIG_RADIANT_CRC_REPAIR); 4-7 the backend
    # table's tail after early_etime (4-5 nRF, 6-7 cc26xx).
    10: ("rx_integrity", ["crc_repaired", "crc_unrepairable",
                          "crc_repair_refuted", "crc_repair_implausible",
                          "late_isr_drops", "end_late_max_us",
                          "t_sync_max_us", "match_max_us"]),
    # 2026-11-15 scheduler contention (TODO.md section 1). All counts.
    11: ("sched", ["windows_merged", "preempted", "replans", "arm_rejected",
                   "tx_armed", "stale_events", "slots_collided",
                   "turn_yields"]),
    # 2026-11-16 page 11's stale_events by site; 0-4 sum to it. All counts.
    # stale_ed is 0xFFFFFFFF without CONFIG_RADIANT_ED_SCAN; 5-7 reserved
    # (always 0xFFFFFFFF until given a meaning).
    12: ("stale", ["stale_rx_frame", "stale_rx_end", "stale_rx_filter_index",
                   "stale_tx", "stale_ed", "reserved_5", "reserved_6",
                   "reserved_7"]),
    # 2026-11-17 transfer outcomes, radiant_api_stats (TODO item 3). All
    # counts; 6-7 reserved (always 0xFFFFFFFF until given a meaning).
    13: ("xfer", ["xfer_watchdogs", "xfer_no_opener", "xfer_lost_track",
                  "xfer_reacquired", "xfer_bad_ack", "xfer_radio_failed",
                  "reserved_6", "reserved_7"]),
}
# Fields that are a level (min/max/last), not a running count: a difference
# means nothing, so --diff reports the later value.
LEVELS = {"turn_min_us", "turn_max_us", "tx_margin_min_us", "status_last",
          "drain_max_us", "min_lead_us", "trk_open_late_max_us",
          "drain_max_frames", "frame_max_us", "crc_max_us", "rx_cb_max_us",
          "pass_max_us", "pass_steps_max",
          "sweep_max_us", "decide_max_us", "arm_call_max_us",
          "done_cb_max_us", "book_max_us", "pass_to_arm_max_us",
          "arm_slack_min_us", "end_late_max_us", "t_sync_max_us",
          "match_max_us"}
SIGNED = {"tx_margin_min_us", "arm_slack_min_us"}
# A measured level with no sample yet reads 0x7FFFFFFF (RADIANT_DIAG_NO_SAMPLE),
# which is not 0xFFFFFFFF ("not measured"): decoded as the string "no_sample".
NO_SAMPLE = 0x7FFFFFFF
NO_SAMPLE_FIELDS = {"arm_slack_min_us"}


def decode(body: bytes) -> dict:
    """One page's reply body -> dict. Raises ValueError on a bad body."""
    if len(body) < BODY_LEN:
        raise ValueError(f"{len(body)} bytes, need {BODY_LEN}")
    page = body[0]
    if page not in PAGES:
        raise ValueError(f"unknown page {page}")
    if body[1] != VERSION:
        raise ValueError(f"structure version {body[1]}, expected {VERSION}")
    name, fields = PAGES[page]
    values = struct.unpack_from("<8I", body, 4)
    out = {"page": page, "name": name,
           "backend": BACKENDS.get(body[3], f"0x{body[3]:02X}"),
           "unmeasured_flag": bool(body[2] & 0x01), "fields": {}}
    for field, v in zip(fields, values):
        if v == NOT_MEASURED:
            out["fields"][field] = None
        elif field in NO_SAMPLE_FIELDS and v == NO_SAMPLE:
            out["fields"][field] = "no_sample"
        elif field in SIGNED:
            out["fields"][field] = struct.unpack("<i", struct.pack("<I", v))[0]
        else:
            out["fields"][field] = v
    return out


def diff(before: dict, after: dict) -> dict:
    """after - before per counter (mod 2^32); levels report the later value."""
    out = {}
    for key, page in after.items():
        prev = before.get(key, {}).get("fields", {})
        rows = {}
        for field, v in page["fields"].items():
            p = prev.get(field)
            if v is None:
                rows[field] = None
            elif field in LEVELS or p is None:
                rows[field] = v
            else:
                rows[field] = (v - p) % (1 << 32)
        out[key] = {"name": page["name"], "fields": rows}
    return out


class Refused(Exception):
    """The dongle answered the request with a response code instead of a page:
    a build without this page (or without 0xF7 at all)."""

    def __init__(self, code: int):
        super().__init__(f"response code 0x{code:02X}")
        self.code = code


def request(dev, reader, page: int, timeout_s: float = 2.0) -> bytes | None:
    """One page. Returns the body, None on a timeout, or raises Refused.

    A refusal and a timeout are different facts: a refusal is the build
    saying it lacks the page, a timeout is a reply lost on the way (libusb0
    loses ~1 % of short reads near reply latency), so only the first may be
    skipped."""
    from ant_probe import EP_OUT, frame
    dev.write(EP_OUT, frame(MESG_REQUEST_ID, bytes([page, MESG_RADIANT_DIAG_ID])))
    deadline = time.monotonic() + timeout_s
    while True:
        got = reader.next_frame(deadline)
        if got is None:
            return None
        msg_id, payload = got
        if msg_id == MESG_RADIANT_DIAG_ID and len(payload) >= 1                 and payload[0] == page:
            return bytes(payload)
        if (msg_id == MESG_RESPONSE_EVENT_ID and len(payload) >= 3
                and payload[0] == page and payload[1] == MESG_REQUEST_ID
                and payload[2] != RESPONSE_NO_ERROR):
            raise Refused(payload[2])


def read_pages(dev, reader, wanted: list[int], retries: int = 1,
               err=sys.stderr) -> dict[str, dict]:
    """Read `wanted`. A page the build refuses is skipped once an earlier page
    has answered (an older build without the newest page); a page that never
    answers is retried, then fails the read, so a before/after pair never
    silently misses a page. Raises RuntimeError on failure."""
    pages: dict[str, dict] = {}
    for page in wanted:
        body = None
        try:
            for _ in range(retries + 1):
                body = request(dev, reader, page)
                if body is not None:
                    break
        except Refused as exc:
            if pages:
                print(f"page {page}: refused ({exc}); this build lacks the "
                      "page, skipped.", file=err)
                continue
            raise RuntimeError(
                f"page {page}: refused ({exc}). Built without "
                "CONFIG_ANT_DONGLE_DIAG (off by default), or a page this "
                "build lacks.") from exc
        if body is None:
            raise RuntimeError(
                f"page {page}: no 0xF7 reply after {retries + 1} tries. A "
                "lost reply, or a build without CONFIG_ANT_DONGLE_DIAG.")
        pages[str(page)] = decode(body)
    return pages


def render(pages: dict) -> str:
    lines = []
    for key in sorted(pages, key=int):
        page = pages[key]
        lines.append(f"page {key} {page['name']}")
        for field, v in page["fields"].items():
            lines.append(f"  {field:<24} {'-' if v is None else v:>12}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", help="a USB dongle whose serial ends with this")
    ap.add_argument("--port", help="a UART build on this port (e.g. COM11)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--pages", default=f"0-{max(PAGES)}",
                    help="pages to read, e.g. 0-10 or 1,2,4 (default all). "
                         "A page an older build refuses is skipped once an "
                         "earlier page has answered; a page that never "
                         "answers fails the read")
    ap.add_argument("--json", help="also write the decoded pages here")
    ap.add_argument("--diff", nargs=2, metavar=("BEFORE", "AFTER"),
                    help="print AFTER - BEFORE from two --json files; no device")
    args = ap.parse_args()

    if args.diff:
        with open(args.diff[0], encoding="utf-8") as fh:
            before = json.load(fh)["pages"]
        with open(args.diff[1], encoding="utf-8") as fh:
            after = json.load(fh)["pages"]
        print(render(diff(before, after)))
        return 0

    wanted: list[int] = []
    for part in args.pages.split(","):
        lo, _, hi = part.partition("-")
        wanted += list(range(int(lo), int(hi or lo) + 1))

    from ant_probe import FrameReader, close_device, open_device
    dev = open_device(True, serial=args.serial, port=args.port, baud=args.baud)
    reader = FrameReader(dev)
    try:
        pages = read_pages(dev, reader, wanted)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 2
    finally:
        close_device(dev)
    backend = next(iter(pages.values()))["backend"] if pages else None

    print(f"MESG_RADIANT_DIAG (0xF7), backend {backend}")
    print(render(pages))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"tool": "ant_diag.py", "backend": backend,
                       "pages": pages}, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
