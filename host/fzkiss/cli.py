"""fzkiss command line."""

from __future__ import annotations

import argparse
import json
import queue
import sys
import time
from datetime import datetime

from . import __version__
from .ax25 import Frame, build_ui
from .device import Event, Tnc
from .ports import candidates, find_tnc_port


def _open(args) -> Tnc:
    port = args.port or find_tnc_port()
    if not port:
        sys.exit(
            "No Flipper TNC port found. Is the KISS TNC app running? "
            "Use `fzkiss ports` to list ports and pass --port."
        )
    return Tnc(port)


def _ts() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


# ---------- commands ----------


def cmd_ports(args) -> int:
    found = candidates()
    if not found:
        print("No Flipper serial ports found.")
        return 1
    for c in found:
        role = "TNC (KISS)" if c.is_tnc else "CLI" if c.interface == 0 else "?"
        iface = "?" if c.interface is None else c.interface
        print(f"{c.device:45s} iface={iface:<3} {role:11s} serial={c.serial or '-'}")
    return 0


def cmd_config(args) -> int:
    with _open(args) as tnc:
        settings = dict(
            freq=args.freq,
            mode=args.mode,
            mod=args.mod,
            pwr=args.pwr,
            radio=args.radio,
            sql=args.sql,
            debug=args.debug,
            call=args.call,
            txdelay=args.txdelay,
            p=args.persist,
            slottime=args.slottime,
            txtail=args.txtail,
            txonce=args.txonce,
        )
        reply = tnc.configure(**settings)
        print(reply.raw)
        return 0 if reply.kind == "OK" else 1


def cmd_get(args) -> int:
    with _open(args) as tnc:
        reply = tnc.get()
        if args.json:
            print(json.dumps({"kind": reply.kind, **reply.fields}))
        else:
            for k, v in reply.fields.items():
                print(f"{k:12s} {v}")
        return 0 if reply.kind == "OK" else 1


def _frame_from_args(args) -> bytes:
    if args.hex:
        return bytes.fromhex(args.hex.replace(" ", ""))
    if not (args.src and args.dst):
        sys.exit("send: give --src and --dst (and --text), or --hex")
    return build_ui(args.src, args.dst, args.path, args.text or "")


def _wait_tx_result(tnc: Tnc, seq: int | None, timeout: float) -> tuple[bool, str]:
    """Wait for ACK/TXDONE (or ERR) for one frame."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ev = tnc.events.get(timeout=0.2)
        except queue.Empty:
            continue
        if ev.kind == "ack" and ev.seq == seq:
            return True, "acked"
        if ev.kind == "status" and ev.status:
            st = ev.status
            want = str(seq) if seq is not None else "-"
            if st.kind == "TXDONE" and st.fields.get("SEQ") == want:
                return True, st.raw
            if st.kind == "ERR" and (seq is None or st.fields.get("SEQ") == str(seq)):
                return False, st.raw
    return False, "timeout"


def cmd_send(args) -> int:
    frame = _frame_from_args(args)
    with _open(args) as tnc:
        seq = int(time.time() * 1000) & 0xFFFF
        tnc.send_ack(seq, frame)
        ok, detail = _wait_tx_result(tnc, seq, args.timeout)
        try:
            shown = str(Frame.decode(frame))
        except ValueError:
            shown = frame.hex()
        print(f"{'sent' if ok else 'FAILED'}: {shown} ({detail})")
        return 0 if ok else 1


def _event_json(ev: Event) -> dict | None:
    if ev.kind == "frame" and ev.frame:
        rx = ev.frame
        st = rx.status.fields if rx.status else {}
        out = {
            "time": datetime.fromtimestamp(rx.received_at).isoformat(timespec="milliseconds"),
            "dir": "rx",
            "rssi": rx.rssi,
            "freq": int(st["FREQ"]) if "FREQ" in st else None,
            "mode": int(st["MODE"]) if "MODE" in st else None,
            "fcs": st.get("FCS", "OK"),
            "hex": rx.raw.hex(),
        }
        f = rx.decoded()
        if f:
            out.update(
                src=str(f.src),
                dst=str(f.dst),
                path=[str(a) + ("*" if a.h_bit else "") for a in f.path],
                info=f.info.decode("utf-8", errors="replace"),
            )
        return out
    if ev.kind == "status" and ev.status and ev.status.kind != "RX":
        return {"time": datetime.now().isoformat(timespec="milliseconds"), "status": ev.status.raw}
    if ev.kind == "ack":
        return {"time": datetime.now().isoformat(timespec="milliseconds"), "ack": ev.seq}
    return None


def cmd_listen(args) -> int:
    with _open(args) as tnc:
        if not args.json:
            print("Listening (Ctrl-C to stop)...", file=sys.stderr)
        try:
            while not tnc.closed:
                try:
                    ev = tnc.events.get(timeout=0.5)
                except queue.Empty:
                    continue
                if args.json:
                    j = _event_json(ev)
                    if j and (args.heartbeats or "HB " not in j.get("status", "")):
                        print(json.dumps(j), flush=True)
                    continue
                if ev.kind == "frame" and ev.frame:
                    rx = ev.frame
                    f = rx.decoded()
                    text = str(f) if f else f"?? {rx.raw.hex()}"
                    fcs = "" if rx.fcs_ok else " [FCS BAD]"
                    print(f"{_ts()} RX {rx.rssi if rx.rssi is not None else '?':>4} dBm  {text}{fcs}", flush=True)
                elif ev.kind == "status" and ev.status:
                    if ev.status.kind == "RX":
                        continue  # folded into the frame line
                    if ev.status.kind == "HB" and not args.heartbeats:
                        continue
                    print(f"{_ts()} -- {ev.status.raw}", flush=True)
                elif ev.kind == "ack":
                    print(f"{_ts()} -- ACK {ev.seq}", flush=True)
        except KeyboardInterrupt:
            pass
        if tnc.closed:
            print("Port closed.", file=sys.stderr)
            return 1
    return 0


def cmd_ping(args) -> int:
    with _open(args) as tnc:
        ok_count = 0
        for i in range(args.count):
            info = f"ping {i + 1}/{args.count} {datetime.now().strftime('%H:%M:%S')}"
            frame = build_ui(args.src, args.dst, args.path, info)
            seq = i + 1
            t0 = time.time()
            tnc.send_ack(seq, frame)
            ok, detail = _wait_tx_result(tnc, seq, args.timeout)
            dt = (time.time() - t0) * 1000
            ok_count += ok
            print(f"seq={seq:<4} {'ACK' if ok else 'FAIL':4s} {dt:7.0f} ms  {detail if not ok else ''}")
            if i + 1 < args.count:
                time.sleep(args.interval)
        print(f"{ok_count}/{args.count} acknowledged")
        return 0 if ok_count == args.count else 1


def cmd_tcp(args) -> int:
    from .tcp import KissTcpServer

    with _open(args) as tnc:
        srv = KissTcpServer(tnc, host=args.bind, port=args.tcp_port, forward_status=args.status)
        srv.start()
        print(f"KISS-over-TCP on {args.bind}:{args.tcp_port} (Ctrl-C to stop)", file=sys.stderr)
        try:
            while not tnc.closed:
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass
        srv.stop()
    return 0


# ---------- parser ----------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="fzkiss", description="Flipper Zero KISS TNC host tool")
    p.add_argument("--version", action="version", version=f"fzkiss {__version__}")
    p.add_argument("-p", "--port", help="serial port (default: auto-detect the TNC channel)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports", help="list Flipper serial ports").set_defaults(func=cmd_ports)

    c = sub.add_parser("config", help="change settings (no options = GET)")
    c.add_argument("--freq", type=int, help="Hz, e.g. 433920000")
    c.add_argument("--mode", type=int, choices=[0, 1, 2], help="0=FSK9600 1=AFSK1200 2=FSK1200")
    c.add_argument("--mod", choices=["GFSK", "FSK"], type=str.upper,
                   help="CC1101 modulation: GFSK (Gaussian, BT 0.5; default) or plain 2-FSK")
    c.add_argument("--pwr", type=int, choices=[-10, 0, 5, 10], help="dBm")
    c.add_argument("--radio", choices=["INT", "EXT"])
    c.add_argument("--sql", type=int, help="squelch / carrier sense, dBm")
    c.add_argument("--debug", type=int, choices=[0, 1], help="forward bad-FCS frames")
    c.add_argument("--call", help="callsign for the on-device test beacon")
    c.add_argument("--txdelay", type=int, help="10 ms units")
    c.add_argument("--persist", type=int, help="KISS P, 0-255")
    c.add_argument("--slottime", type=int, help="10 ms units")
    c.add_argument("--txtail", type=int, help="10 ms units")
    c.add_argument("--txonce", type=int, help="Hz, for the next TX only")
    c.set_defaults(func=cmd_config)

    g = sub.add_parser("get", help="show all settings and counters")
    g.add_argument("--json", action="store_true")
    g.set_defaults(func=cmd_get)

    s = sub.add_parser("send", help="transmit one frame")
    s.add_argument("--src")
    s.add_argument("--dst")
    s.add_argument("--path", help="comma-separated digipeaters, e.g. WIDE1-1")
    s.add_argument("--text", help="info field")
    s.add_argument("--hex", help="raw AX.25 frame (no FCS) as hex")
    s.add_argument("--timeout", type=float, default=15.0)
    s.set_defaults(func=cmd_send)

    li = sub.add_parser("listen", help="print received frames and status lines")
    li.add_argument("--json", action="store_true", help="one JSON object per line")
    li.add_argument("--heartbeats", action="store_true", help="also show HB lines")
    li.set_defaults(func=cmd_listen)

    pg = sub.add_parser("ping", help="send numbered frames, report ACKs")
    pg.add_argument("--count", type=int, default=10)
    pg.add_argument("--interval", type=float, default=2.0)
    pg.add_argument("--src", required=True, help="your callsign (station ID)")
    pg.add_argument("--dst", default="PING")
    pg.add_argument("--path")
    pg.add_argument("--timeout", type=float, default=15.0)
    pg.set_defaults(func=cmd_ping)

    t = sub.add_parser("tcp", help="KISS-over-TCP server for other apps")
    t.add_argument("--port", dest="tcp_port", type=int, default=8001)
    t.add_argument("--bind", default="127.0.0.1")
    t.add_argument("--status", action="store_true", help="also forward SetHardware status frames")
    t.set_defaults(func=cmd_tcp)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except TimeoutError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
