import socket
import time

from conftest import wait_for

from fzkiss import kiss
from fzkiss.ax25 import build_ui
from fzkiss.device import Tnc
from fzkiss.ports import Candidate, mark_tnc
from fzkiss.tcp import KissTcpServer


def test_mark_tnc_macos_names():
    found = mark_tnc(
        [
            Candidate("/dev/cu.usbmodemflip_Anana1", "flip_Anana", 0, ""),
            Candidate("/dev/cu.usbmodemflip_Anana3", "flip_Anana", 2, ""),
        ]
    )
    assert found[0].device.endswith("3") and found[0].is_tnc
    assert not found[1].is_tnc


def test_mark_tnc_single_cdc_is_not_tnc():
    found = mark_tnc([Candidate("/dev/ttyACM0", "flip_X", 0, "")])
    assert not found[0].is_tnc


def test_tcp_bridge(fake):
    with Tnc(fake) as tnc:
        srv = KissTcpServer(tnc, port=0)
        srv.port = _free_port()
        srv.start()
        try:
            c = socket.create_connection(("127.0.0.1", srv.port), timeout=2)
            assert wait_for(lambda: srv.client_count == 1)
            # client -> radio, split across two writes
            frame = build_ui("N0CALL", "TEST", None, "via tcp")
            data = kiss.encode_data(frame)
            c.sendall(data[:5])
            time.sleep(0.05)
            c.sendall(data[5:])
            assert wait_for(lambda: any(f.payload == frame for f in fake.frames))
            # radio -> client (data frames only by default)
            fake.push(kiss.encode_sethw("RX RSSI=-80 FREQ=1 MODE=0 FCS=OK T=1"))
            fake.push(kiss.encode_data(b"\x01" * 16))
            c.settimeout(2)
            got = kiss.Decoder()
            frames = []
            t0 = time.time()
            while not frames and time.time() - t0 < 2:
                frames += got.feed(c.recv(1024))
            assert frames[0].command == kiss.CMD_DATA and frames[0].payload == b"\x01" * 16
            c.close()
        finally:
            srv.stop()


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p
