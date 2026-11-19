"""Parsing of the device's SetHardware (0x06) ASCII status lines.

Examples:
    RX RSSI=-87 FREQ=433920000 MODE=0 FCS=OK T=12345
    TXDONE SEQ=7 DUR=412
    HB STATE=RX RSSI=-110
    OK FREQ=433920000 TXOK=1
    ERR TX_BLOCKED_REGION SEQ=3
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Status:
    kind: str  # RX, TXDONE, HB, OK, ERR, CFG, ...
    fields: dict[str, str] = field(default_factory=dict)
    words: list[str] = field(default_factory=list)  # bare tokens, e.g. the error code
    raw: str = ""

    @property
    def error(self) -> str | None:
        return self.words[0] if self.kind == "ERR" and self.words else None

    def int(self, key: str, default: int | None = None) -> int | None:
        v = self.fields.get(key)
        if v is None or v == "-":
            return default
        try:
            return int(v)
        except ValueError:
            return default


def parse(text: str | bytes) -> Status:
    if isinstance(text, bytes):
        text = text.decode("ascii", errors="replace")
    text = text.strip()
    tokens = text.split()
    if not tokens:
        return Status(kind="", raw=text)
    st = Status(kind=tokens[0].upper(), raw=text)
    for tok in tokens[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            st.fields[k.upper()] = v
        else:
            st.words.append(tok)
    return st
