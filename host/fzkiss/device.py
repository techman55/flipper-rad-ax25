"""Talk to the Flipper KISS TNC over a serial port (or any byte stream)."""

from __future__ import annotations

import queue
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Protocol

from . import kiss
from .ax25 import Frame
from .status import Status, parse


class ByteStream(Protocol):
    def read(self, n: int) -> bytes: ...
    def write(self, data: bytes) -> int | None: ...
    def close(self) -> None: ...


@dataclass
class RxFrame:
    raw: bytes  # AX.25 without FCS
    status: Status | None  # the "RX ..." line that preceded it, if any
    received_at: float = field(default_factory=time.time)

    @property
    def rssi(self) -> int | None:
        return self.status.int("RSSI") if self.status else None

    @property
    def fcs_ok(self) -> bool:
        return not self.status or self.status.fields.get("FCS", "OK") == "OK"

    def decoded(self) -> Frame | None:
        try:
            return Frame.decode(self.raw)
        except ValueError:
            return None


@dataclass
class Event:
    kind: str  # "frame" | "status" | "ack" | "kiss"
    frame: RxFrame | None = None
    status: Status | None = None
    seq: int | None = None
    kiss_frame: kiss.KissFrame | None = None
    at: float = field(default_factory=time.time)


class Tnc:
    """Background reader + helpers. Use as a context manager."""

    def __init__(self, stream: ByteStream | str, baud: int = 115200):
        if isinstance(stream, str):
            import serial  # pyserial

            stream = serial.Serial(stream, baud, timeout=0.1)
        self._s = stream
        self._decoder = kiss.Decoder()
        self._write_lock = threading.Lock()
        self.events: "queue.Queue[Event]" = queue.Queue()
        self._replies: "queue.Queue[Status]" = queue.Queue()
        self._listeners: list[Callable[[Event], None]] = []
        self._pending_rx: Status | None = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._reader, name="fzkiss-reader", daemon=True)
        self._thread.start()

    # --- lifecycle ---

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1.0)
        self._s.close()

    def __enter__(self) -> "Tnc":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def add_listener(self, fn: Callable[[Event], None]) -> None:
        self._listeners.append(fn)

    # --- reading ---

    def _emit(self, ev: Event) -> None:
        self.events.put(ev)
        for fn in list(self._listeners):
            fn(ev)

    def _reader(self) -> None:
        while not self._stop.is_set():
            try:
                data = self._s.read(512)
            except Exception:  # port vanished (replug) - let the caller notice via closed()
                self._stop.set()
                break
            if not data:
                continue
            for kf in self._decoder.feed(data):
                self._handle(kf)

    def _handle(self, kf: kiss.KissFrame) -> None:
        cmd = kf.command
        if kf.port != 0:
            return
        if cmd == kiss.CMD_SETHARDWARE:
            st = parse(kf.payload)
            if st.kind == "RX":
                self._pending_rx = st
            # CFG lines report changes made on the Flipper itself; they are not replies.
            if st.kind == "OK" or (st.kind == "ERR" and "SEQ" not in st.fields):
                self._replies.put(st)
            self._emit(Event("status", status=st, kiss_frame=kf))
        elif cmd == kiss.CMD_DATA:
            rx = RxFrame(kf.payload, self._pending_rx)
            self._pending_rx = None
            self._emit(Event("frame", frame=rx, kiss_frame=kf))
        elif cmd == kiss.CMD_ACKMODE and len(kf.payload) >= 2:
            self._emit(Event("ack", seq=(kf.payload[0] << 8) | kf.payload[1], kiss_frame=kf))
        else:
            self._emit(Event("kiss", kiss_frame=kf))

    @property
    def closed(self) -> bool:
        return self._stop.is_set()

    # --- writing ---

    def write_raw(self, data: bytes) -> None:
        with self._write_lock:
            self._s.write(data)

    def send(self, ax25: bytes) -> None:
        self.write_raw(kiss.encode_data(ax25))

    def send_ack(self, seq: int, ax25: bytes) -> None:
        self.write_raw(kiss.encode_ackmode(seq & 0xFFFF, ax25))

    def set_param(self, cmd: int, value: int) -> None:
        self.write_raw(kiss.encode(cmd, bytes((value & 0xFF,))))

    def sethw(self, line: str, timeout: float = 3.0) -> Status:
        """Send a SetHardware line and wait for its OK/ERR reply."""
        while not self._replies.empty():
            self._replies.get_nowait()
        self.write_raw(kiss.encode_sethw(line))
        try:
            return self._replies.get(timeout=timeout)
        except queue.Empty:
            raise TimeoutError(f"no reply to {line!r} within {timeout}s") from None

    def get(self, timeout: float = 3.0) -> Status:
        return self.sethw("GET", timeout)

    def configure(self, **settings: object) -> Status:
        """configure(freq=433920000, mode=0, pwr=10) -> 'FREQ=...;MODE=...;PWR=...'."""
        parts = []
        for k, v in settings.items():
            if v is None:
                continue
            if isinstance(v, bool):
                v = int(v)
            parts.append(f"{k.upper()}={v}")
        if not parts:
            return self.get()
        return self.sethw(";".join(parts))
