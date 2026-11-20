from conftest import FakeDevice, wait_for

from fzkiss import kiss
from fzkiss.ax25 import build_ui
from fzkiss.device import Tnc


def test_get_and_configure(fake):
    with Tnc(fake) as tnc:
        st = tnc.get()
        assert st.kind == "OK" and st.int("HEAP") == 1234
        st = tnc.configure(freq=433920000, mode=1, pwr=None, debug=True)
        assert st.kind == "OK"
        assert fake.frames[-1].payload == b"FREQ=433920000;MODE=1;DEBUG=1"
        st = tnc.configure(mod="GFSK")
        assert fake.frames[-1].payload == b"MOD=GFSK" and st.kind == "OK"
        st = tnc.configure(freq=100000000)
        assert st.kind == "ERR" and st.error == "FREQ_RANGE"


def test_rx_status_pairs_with_frame(fake):
    with Tnc(fake) as tnc:
        frame = build_ui("N0CALL", "TEST", None, "hi")
        fake.push(kiss.encode_sethw("RX RSSI=-91 FREQ=433920000 MODE=0 FCS=OK T=5"))
        fake.push(kiss.encode_data(frame))
        frames = []
        assert wait_for(lambda: _drain(tnc, frames) and frames)
        rx = frames[0]
        assert rx.raw == frame and rx.rssi == -91 and rx.fcs_ok
        assert str(rx.decoded()) == "N0CALL>TEST: hi"


def test_ackmode(fake):
    with Tnc(fake) as tnc:
        tnc.send_ack(7, build_ui("N0CALL", "TEST", None, "x"))
        acks = []
        assert wait_for(lambda: _drain(tnc, [], acks) and 7 in acks)
        sent = fake.frames[-1]
        assert sent.command == kiss.CMD_ACKMODE and sent.payload[:2] == b"\x00\x07"


def test_async_errors_are_not_replies():
    dev = FakeDevice(auto_reply=False)
    with Tnc(dev) as tnc:
        statuses = []
        tnc.add_listener(lambda ev: ev.kind == "status" and statuses.append(ev.status.kind))
        dev.push_status("ERR BUSY_TIMEOUT SEQ=4")
        dev.push_status("CFG MODE=1")
        dev.push_status("HB STATE=RX RSSI=-110")
        assert wait_for(lambda: len(statuses) == 3)
        # A SEQ-tagged error, a CFG line and a heartbeat are not replies to sethw().
        assert tnc._replies.empty()
        dev.auto_reply = True
        assert tnc.sethw("MODE=2").raw == "OK MODE=2"


def _drain(tnc, frames, acks=None):
    while not tnc.events.empty():
        ev = tnc.events.get_nowait()
        if ev.kind == "frame":
            frames.append(ev.frame)
        if ev.kind == "ack" and acks is not None:
            acks.append(ev.seq)
    return True
