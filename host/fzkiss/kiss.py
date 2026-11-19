"""KISS framing: escaping, encoding and a streaming decoder."""

from __future__ import annotations

from dataclasses import dataclass

FEND = 0xC0
FESC = 0xDB
TFEND = 0xDC
TFESC = 0xDD

CMD_DATA = 0x00
CMD_TXDELAY = 0x01
CMD_P = 0x02
CMD_SLOTTIME = 0x03
CMD_TXTAIL = 0x04
CMD_FULLDUPLEX = 0x05
CMD_SETHARDWARE = 0x06
CMD_ACKMODE = 0x0C
CMD_RETURN = 0xFF


def escape(data: bytes) -> bytes:
    out = bytearray()
    for b in data:
        if b == FEND:
            out += bytes((FESC, TFEND))
        elif b == FESC:
            out += bytes((FESC, TFESC))
        else:
            out.append(b)
    return bytes(out)


def unescape(data: bytes) -> bytes:
    out = bytearray()
    it = iter(data)
    for b in it:
        if b == FESC:
            nxt = next(it, None)
            if nxt == TFEND:
                out.append(FEND)
            elif nxt == TFESC:
                out.append(FESC)
            elif nxt is not None:
                out.append(nxt)  # protocol error: keep the byte, like most TNCs
        else:
            out.append(b)
    return bytes(out)


def encode(type_byte: int, payload: bytes = b"") -> bytes:
    """One complete KISS frame: FEND, escaped(type + payload), FEND."""
    return bytes((FEND,)) + escape(bytes((type_byte & 0xFF,)) + bytes(payload)) + bytes((FEND,))


def encode_data(ax25: bytes, port: int = 0) -> bytes:
    return encode((port << 4) | CMD_DATA, ax25)


def encode_ackmode(seq: int, ax25: bytes, port: int = 0) -> bytes:
    return encode((port << 4) | CMD_ACKMODE, bytes(((seq >> 8) & 0xFF, seq & 0xFF)) + ax25)


def encode_sethw(text: str, port: int = 0) -> bytes:
    return encode((port << 4) | CMD_SETHARDWARE, text.encode("ascii"))


@dataclass
class KissFrame:
    type_byte: int
    payload: bytes

    @property
    def port(self) -> int:
        return self.type_byte >> 4

    @property
    def command(self) -> int:
        return self.type_byte & 0x0F if self.type_byte != CMD_RETURN else CMD_RETURN


class Decoder:
    """Feed arbitrary byte chunks; get complete frames back."""

    def __init__(self, max_len: int = 4096):
        self._buf = bytearray()
        self._in_frame = False
        self._max_len = max_len

    def feed(self, data: bytes) -> list[KissFrame]:
        frames: list[KissFrame] = []
        for b in data:
            if b == FEND:
                if self._in_frame and self._buf:
                    raw = unescape(bytes(self._buf))
                    if raw:
                        frames.append(KissFrame(raw[0], raw[1:]))
                self._buf.clear()
                self._in_frame = True
            elif self._in_frame:
                if len(self._buf) < self._max_len:
                    self._buf.append(b)
                else:  # runaway frame: drop it and resync on the next FEND
                    self._buf.clear()
                    self._in_frame = False
        return frames
