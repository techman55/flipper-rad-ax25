import pytest

from fzkiss.ax25 import Address, Frame, build_ui, fcs, fcs_bytes


def test_fcs_vectors():
    # CRC-16/X.25 check value from the CRC catalogue
    assert fcs(b"123456789") == 0x906E
    assert fcs(b"") == 0x0000
    # Same vector as modem_tests/test_hdlc.c (N0CALL>TEST: hi)
    ui = bytes([0xA8, 0x8A, 0xA6, 0xA8, 0x40, 0x40, 0xE0, 0x9C, 0x60, 0x86, 0x82, 0x98, 0x98, 0x61, 0x03, 0xF0, 0x68, 0x69])
    assert fcs(ui) == 0x58CF
    assert fcs_bytes(ui) == bytes([0xCF, 0x58])
    # residue: CRC over data + FCS is constant
    assert fcs(b"123456789" + fcs_bytes(b"123456789")) == 0xF0B8 ^ 0xFFFF


def test_address_encode_decode():
    a = Address.parse("kn4xxx-1")
    raw = a.encode(last=False)
    assert raw == bytes([ord(c) << 1 for c in "KN4XXX"]) + bytes([0x62])
    b, last = Address.decode(raw)
    assert (b.call, b.ssid, last) == ("KN4XXX", 1, False)

    raw = Address("WIDE2", 15, h_bit=True).encode(last=True)
    assert raw[5] == 0x40  # padded with spaces
    assert raw[6] == 0x80 | 0x60 | (15 << 1) | 1
    b, last = Address.decode(raw)
    assert (str(b), b.h_bit, last) == ("WIDE2-15", True, True)


@pytest.mark.parametrize("bad", ["", "TOOLONG1", "BAD-16", "A*B", "AB-"])
def test_bad_calls(bad):
    with pytest.raises(ValueError):
        Address.parse(bad)


def test_ui_frame_matches_device_encoding():
    # Byte-identical to ax25_build_ui() in fap/modem (see test_kiss_ax25.c)
    f = build_ui("N0CALL", "TEST", None, "hello")
    assert f == bytes([0xA8, 0x8A, 0xA6, 0xA8, 0x40, 0x40, 0xE0, 0x9C, 0x60, 0x86, 0x82, 0x98, 0x98, 0x61, 0x03, 0xF0]) + b"hello"


def test_frame_roundtrip_with_path():
    raw = build_ui("KN4XXX-1", "APRS", "WIDE1-1,WIDE2-2", b"!4903.50N/07201.75W-")
    f = Frame.decode(raw)
    assert str(f.src) == "KN4XXX-1" and str(f.dst) == "APRS"
    assert [str(a) for a in f.path] == ["WIDE1-1", "WIDE2-2"]
    assert f.control == 0x03 and f.pid == 0xF0
    assert f.info == b"!4903.50N/07201.75W-"
    assert str(f) == "KN4XXX-1>APRS,WIDE1-1,WIDE2-2: !4903.50N/07201.75W-"
    assert f.encode() == raw


def test_repeated_flag_shown():
    raw = bytearray(build_ui("A1A", "B2B", "WIDE1-1", "x"))
    raw[20] |= 0x80
    assert str(Frame.decode(bytes(raw))) == "A1A>B2B,WIDE1-1*: x"


def test_decode_errors():
    with pytest.raises(ValueError):
        Frame.decode(b"\x00" * 10)
    with pytest.raises(ValueError):
        build_ui("A", "B", None, b"x" * 400)
