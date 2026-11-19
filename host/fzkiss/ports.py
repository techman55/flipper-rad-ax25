"""Find the Flipper's TNC serial port (CDC channel 1).

With the app running the Flipper enumerates as a dual CDC device
(VID 0483, PID 5740) with two ACM functions:
    interface 0 -> Flipper CLI      (macOS: /dev/cu.usbmodemflip_<Name>1, Linux: ttyACM0)
    interface 2 -> KISS TNC         (macOS: /dev/cu.usbmodemflip_<Name>3, Linux: ttyACM1)
"""

from __future__ import annotations

import re
from dataclasses import dataclass

FLIPPER_VID = 0x0483
FLIPPER_PID = 0x5740


@dataclass
class Candidate:
    device: str
    serial: str | None
    interface: int | None  # USB interface number if known
    description: str
    is_tnc: bool = False


def _interface_number(port) -> int | None:
    # Linux/Windows: location like "1-1.2:1.2" -> interface 2
    loc = getattr(port, "location", None) or ""
    m = re.search(r":\d+\.(\d+)$", loc)
    if m:
        return int(m.group(1))
    # macOS: /dev/cu.usbmodemflip_Name1 / ...3 -> trailing digit = interface + 1
    m = re.search(r"usbmodem.*?(\d)$", port.device)
    if m:
        return int(m.group(1)) - 1
    return None


def candidates() -> list[Candidate]:
    from serial.tools import list_ports  # imported lazily so tests need no hardware

    found: list[Candidate] = []
    for p in list_ports.comports():
        is_flipper = (p.vid == FLIPPER_VID and p.pid == FLIPPER_PID) or "usbmodemflip" in p.device
        if not is_flipper:
            continue
        found.append(
            Candidate(
                device=p.device,
                serial=p.serial_number,
                interface=_interface_number(p),
                description=p.description or "",
            )
        )
    return mark_tnc(found)


def mark_tnc(found: list[Candidate]) -> list[Candidate]:
    """Within each physical Flipper, the higher-numbered ACM interface is the TNC.

    Callout (cu.*) and dial-in (tty.*) nodes on macOS are grouped the same way;
    the cu.* node is preferred.
    """
    groups: dict[str, list[Candidate]] = {}
    for c in found:
        key = c.serial or re.sub(r"\d$", "", c.device.replace("/dev/tty.", "/dev/cu."))
        groups.setdefault(key, []).append(c)
    for group in groups.values():
        ifaces = sorted({c.interface for c in group if c.interface is not None})
        if len(ifaces) >= 2:
            for c in group:
                c.is_tnc = c.interface == ifaces[-1]
    return sorted(found, key=lambda c: (not c.is_tnc, "/tty." in c.device, c.device))


def find_tnc_port() -> str | None:
    for c in candidates():
        if c.is_tnc and "/tty." not in c.device:
            return c.device
    return None
