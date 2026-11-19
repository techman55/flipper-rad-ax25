"""KISS-over-TCP server so other packet software can share the Flipper.

Each client's byte stream is decoded into whole KISS frames and re-encoded
onto the serial port under a lock, so two clients can never interleave
partial frames. Frames from the radio are broadcast to every client.
"""

from __future__ import annotations

import socket
import threading

from . import kiss
from .device import Event, Tnc


class KissTcpServer:
    def __init__(self, tnc: Tnc, host: str = "127.0.0.1", port: int = 8001, forward_status: bool = False):
        self.tnc = tnc
        self.host = host
        self.port = port
        self.forward_status = forward_status
        self._clients: list[socket.socket] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._sock: socket.socket | None = None
        tnc.add_listener(self._on_event)

    # radio -> clients
    def _on_event(self, ev: Event) -> None:
        if ev.kiss_frame is None:
            return
        if ev.kind == "status" and not self.forward_status:
            return  # most KISS clients ignore command 6 anyway
        if ev.kind not in ("frame", "status", "ack"):
            return
        data = kiss.encode(ev.kiss_frame.type_byte, ev.kiss_frame.payload)
        with self._lock:
            dead = []
            for c in self._clients:
                try:
                    c.sendall(data)
                except OSError:
                    dead.append(c)
            for c in dead:
                self._clients.remove(c)
                c.close()

    # client -> radio
    def _serve_client(self, conn: socket.socket, addr) -> None:
        dec = kiss.Decoder()
        try:
            while not self._stop.is_set():
                data = conn.recv(4096)
                if not data:
                    break
                for f in dec.feed(data):
                    self.tnc.write_raw(kiss.encode(f.type_byte, f.payload))
        except OSError:
            pass
        finally:
            with self._lock:
                if conn in self._clients:
                    self._clients.remove(conn)
            conn.close()

    @property
    def client_count(self) -> int:
        with self._lock:
            return len(self._clients)

    def start(self) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.host, self.port))
        s.listen(8)
        s.settimeout(0.5)
        self._sock = s
        threading.Thread(target=self._accept_loop, name="fzkiss-tcp", daemon=True).start()

    def _accept_loop(self) -> None:
        assert self._sock
        while not self._stop.is_set():
            try:
                conn, addr = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            with self._lock:
                self._clients.append(conn)
            threading.Thread(target=self._serve_client, args=(conn, addr), daemon=True).start()

    def stop(self) -> None:
        self._stop.set()
        if self._sock:
            self._sock.close()
        with self._lock:
            for c in self._clients:
                c.close()
            self._clients.clear()
