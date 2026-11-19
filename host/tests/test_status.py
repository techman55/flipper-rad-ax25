from fzkiss.status import parse


def test_rx_line():
    st = parse(b"RX RSSI=-87 FREQ=433920000 MODE=0 FCS=OK T=12345")
    assert st.kind == "RX"
    assert st.int("RSSI") == -87 and st.int("FREQ") == 433920000
    assert st.fields["FCS"] == "OK"


def test_err_and_txdone():
    st = parse("ERR TX_BLOCKED_REGION SEQ=3")
    assert st.kind == "ERR" and st.error == "TX_BLOCKED_REGION" and st.int("SEQ") == 3
    st = parse("TXDONE SEQ=- DUR=412")
    assert st.int("SEQ") is None and st.int("DUR") == 412
    assert parse("").kind == ""
