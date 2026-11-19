import queue
import threading
import time

import pytest

from fzkiss import kiss
from fzkiss.status import parse


class FakeDevice:
    """A byte stream that behaves like the Flipper TNC for protocol tests."""

    def __init__(self, auto_reply=True):
        self._to_host: "queue.Queue[bytes]" = queue.Queue()
        self.written = bytearray()
        self.frames: list[kiss.KissFrame] = []
        self._dec = kiss.Decoder()
        self.auto_reply = auto_reply
        self.closed = False
        self._lock = threading.Lock()

    # host side (what Tnc calls)
    def read(self, n):
        try:
            return self._to_host.get(timeout=0.05)
        except queue.Empty:
            return b""

    def write(self, data):
        with self._lock:
            self.written += data
            for f in self._dec.feed(data):
                self.frames.append(f)
                if self.auto_reply:
                    self._respond(f)
        return len(data)

    def close(self):
        self.closed = True

    # device side
    def push(self, data: bytes):
        self._to_host.put(data)

    def push_status(self, text: str):
        self.push(kiss.encode_sethw(text))

    def _respond(self, f: kiss.KissFrame):
        if f.command == kiss.CMD_SETHARDWARE:
            line = f.payload.decode()
            if line == "GET":
                self.push_status("OK FREQ=433920000 MODE=0 PWR=10 RADIO=INT HEAP=1234")
            elif line.startswith("FREQ=1"):
                self.push_status("ERR FREQ_RANGE")
            else:
                self.push_status("OK " + " ".join(line.split(";")))
        elif f.command == kiss.CMD_ACKMODE:
            seq = (f.payload[0] << 8) | f.payload[1]
            self.push(kiss.encode(kiss.CMD_ACKMODE, f.payload[:2]))
            self.push_status(f"TXDONE SEQ={seq} DUR=120")
        elif f.command == kiss.CMD_DATA:
            self.push_status("TXDONE SEQ=- DUR=120")


@pytest.fixture
def fake():
    return FakeDevice()


def wait_for(pred, timeout=2.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return True
        time.sleep(0.01)
    return False
