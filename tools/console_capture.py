# SPDX-License-Identifier: Apache-2.0
"""Capture a board's log console for the length of a measurement window.

The question this answers is "when the ANT port died, what was the firmware
doing?" - and it can only answer it if every line carries a host timestamp
that can be laid beside the ANT side's own times and Windows' PnP log. So:

  - every line is prefixed with the host's wall clock, to the millisecond;
  - every line is flushed as it is written, so a capture cut short by a dying
    port or a killed process still holds everything up to the cut;
  - the moment the console port itself dies is recorded, with the error. A
    console that dies AT THE SAME MOMENT as the ANT port points at the probe
    or USB (the J-Link OB carries both VCOMs); a console that carries on, or
    prints a fault dump or a boot banner, points at the firmware.

DTR and RTS are asserted on open: J-Link's CDC hands over one 128-byte chunk
and then goes silent without DTR, which looks exactly like a board that hung
after boot (tools/ant_ack_pair.py's log_thread, the model for this).
"""

from __future__ import annotations

import datetime as _dt
import threading
import time
from collections.abc import Callable


def _stamp(now: float) -> str:
    return _dt.datetime.fromtimestamp(now).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3]


def _default_opener(port: str, baud: int):
    import serial

    return serial.Serial(port, baud, timeout=0.2)


class ConsoleCapture:
    """One background thread reading one console port into one file.

    `opener(port, baud)` returns a pyserial-like object (read, close, dtr,
    rts); `clock()` returns wall-clock seconds. Both exist for the tests.
    """

    def __init__(self, port: str, path, baud: int = 115200,
                 opener: Callable | None = None,
                 clock: Callable[[], float] = time.time):
        self.port = port
        self.path = str(path)
        self.baud = baud
        self._opener = opener or _default_opener
        self._clock = clock
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.lines = 0
        self.bytes = 0
        self.opened = False
        self.started_at: float | None = None
        self.died_at: float | None = None
        self.error: str | None = None

    # The thread body, callable directly so tests need no thread.
    def run(self) -> None:
        self.started_at = self._clock()
        with open(self.path, "w", encoding="utf-8", newline="\n") as fh:
            def emit(text: str) -> None:
                fh.write(f"[{_stamp(self._clock())}] {text}\n")
                fh.flush()

            try:
                port = self._opener(self.port, self.baud)
            except Exception as exc:  # noqa: BLE001 - reported, not raised
                self.error = f"open failed: {type(exc).__name__}: {exc}"
                emit(f"[console_capture] {self.port}: {self.error}")
                return
            self.opened = True
            try:
                port.dtr = True
                port.rts = True
            except Exception:  # noqa: BLE001 - a fake or odd driver
                pass
            emit(f"[console_capture] {self.port}: capturing at {self.baud}")

            pending = b""
            try:
                while not self._stop.is_set():
                    data = port.read(4096)
                    if not data:
                        continue
                    self.bytes += len(data)
                    pending += data
                    *complete, pending = pending.split(b"\n")
                    for raw in complete:
                        emit(raw.rstrip(b"\r").decode("utf-8", "replace"))
                        self.lines += 1
            except Exception as exc:  # noqa: BLE001 - the death is the datum
                self.died_at = self._clock()
                self.error = f"{type(exc).__name__}: {exc}"
            finally:
                if pending:
                    emit(pending.rstrip(b"\r").decode("utf-8", "replace")
                         + "  [partial line]")
                    self.lines += 1
                if self.died_at is not None:
                    emit(f"[console_capture] {self.port}: PORT DIED: "
                         f"{self.error}")
                else:
                    emit(f"[console_capture] {self.port}: stopped")
                try:
                    port.close()
                except Exception:  # noqa: BLE001
                    pass

    def start(self) -> ConsoleCapture:
        self._thread = threading.Thread(target=self.run, daemon=True,
                                        name=f"console {self.port}")
        self._thread.start()
        return self

    def stop(self, timeout: float = 2.0) -> dict:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=timeout)
        return self.summary()

    def summary(self) -> dict:
        return {
            "port": self.port, "path": self.path, "opened": self.opened,
            "lines": self.lines, "bytes": self.bytes,
            "started_at_unix": self.started_at,
            "died_at_unix": self.died_at,
            "died_after_s": (round(self.died_at - self.started_at, 3)
                             if self.died_at is not None
                             and self.started_at is not None else None),
            "error": self.error,
        }
