"""AX.25 UI-frame encode/decode (addresses, SSIDs, H-bit, control, PID) and FCS."""

from __future__ import annotations

import re
from dataclasses import dataclass, field

CONTROL_UI = 0x03
PID_NO_L3 = 0xF0
MAX_DIGIS = 8
MAX_FRAME = 330  # without FCS, same limit as the device

_CALL_RE = re.compile(r"^([A-Z0-9]{1,6})(?:-(\d{1,2}))?(\*)?$")


def fcs(data: bytes) -> int:
    """CRC-16/X.25 (HDLC FCS): reflected poly 0x8408, init 0xFFFF, xorout 0xFFFF."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def fcs_bytes(data: bytes) -> bytes:
    """FCS as transmitted: low byte first."""
    v = fcs(data)
    return bytes((v & 0xFF, v >> 8))


@dataclass
class Address:
    call: str
    ssid: int = 0
    h_bit: bool = False  # has-been-repeated (digis) / command-response bit (src/dst)

    @classmethod
    def parse(cls, text: str) -> "Address":
        m = _CALL_RE.match(text.strip().upper())
        if not m:
            raise ValueError(f"bad callsign: {text!r}")
        ssid = int(m.group(2) or 0)
        if ssid > 15:
            raise ValueError(f"SSID out of range: {text!r}")
        return cls(m.group(1), ssid, bool(m.group(3)))

    def encode(self, last: bool) -> bytes:
        call = self.call.ljust(6)
        out = bytes((ord(c) << 1) & 0xFE for c in call)
        ssid_byte = (0x80 if self.h_bit else 0) | 0x60 | ((self.ssid & 0x0F) << 1) | (1 if last else 0)
        return out + bytes((ssid_byte,))

    @classmethod
    def decode(cls, raw: bytes) -> tuple["Address", bool]:
        """Returns (address, is_last)."""
        if len(raw) != 7:
            raise ValueError("address field must be 7 bytes")
        call = "".join(chr(b >> 1) for b in raw[:6]).rstrip()
        ssid = (raw[6] >> 1) & 0x0F
        return cls(call, ssid, bool(raw[6] & 0x80)), bool(raw[6] & 1)

    def __str__(self) -> str:
        return self.call + (f"-{self.ssid}" if self.ssid else "")


@dataclass
class Frame:
    src: Address
    dst: Address
    path: list[Address] = field(default_factory=list)
    info: bytes = b""
    control: int = CONTROL_UI
    pid: int | None = PID_NO_L3

    def encode(self) -> bytes:
        if len(self.path) > MAX_DIGIS:
            raise ValueError("too many digipeaters")
        # AX.25 v2 command frame: C bit set on destination, clear on source.
        dst = Address(self.dst.call, self.dst.ssid, True)
        src = Address(self.src.call, self.src.ssid, False)
        addrs = [dst, src, *self.path]
        out = bytearray()
        for i, a in enumerate(addrs):
            out += a.encode(last=i == len(addrs) - 1)
        out.append(self.control)
        if self.pid is not None:
            out.append(self.pid)
        out += self.info
        if len(out) > MAX_FRAME:
            raise ValueError(f"frame is {len(out)} bytes, max {MAX_FRAME}")
        return bytes(out)

    @classmethod
    def decode(cls, raw: bytes) -> "Frame":
        addrs: list[Address] = []
        i = 0
        while True:
            if i + 7 > len(raw):
                raise ValueError("truncated address field")
            a, last = Address.decode(raw[i : i + 7])
            addrs.append(a)
            i += 7
            if last:
                break
            if len(addrs) >= 2 + MAX_DIGIS:
                raise ValueError("address field too long")
        if len(addrs) < 2:
            raise ValueError("need at least destination and source")
        if i >= len(raw):
            raise ValueError("missing control byte")
        control = raw[i]
        i += 1
        pid = None
        # I-frames (bit0 == 0) and UI frames carry a PID.
        if (control & 0x01) == 0 or (control & 0xEF) == CONTROL_UI:
            if i < len(raw):
                pid = raw[i]
                i += 1
        return cls(src=addrs[1], dst=addrs[0], path=addrs[2:], info=raw[i:], control=control, pid=pid)

    def __str__(self) -> str:
        path = "".join(f",{a}{'*' if a.h_bit else ''}" for a in self.path)
        text = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in self.info)
        return f"{self.src}>{self.dst}{path}: {text}"


def build_ui(src: str, dst: str, path: str | list[str] | None = None, info: bytes | str = b"") -> bytes:
    """Convenience: build a UI frame (no FCS) from strings."""
    if isinstance(path, str):
        path = [p for p in path.split(",") if p.strip()]
    if isinstance(info, str):
        info = info.encode("utf-8")
    return Frame(
        src=Address.parse(src),
        dst=Address.parse(dst),
        path=[Address.parse(p) for p in (path or [])],
        info=info,
    ).encode()
