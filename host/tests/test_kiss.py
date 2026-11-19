import random

from fzkiss import kiss


def test_escape_vector():
    payload = bytes([0x01, 0xC0, 0x02, 0xDB, 0xDC, 0xDD, 0xC0, 0xDB])
    enc = kiss.encode(0x00, payload)
    assert enc == bytes(
        [0xC0, 0x00, 0x01, 0xDB, 0xDC, 0x02, 0xDB, 0xDD, 0xDC, 0xDD, 0xDB, 0xDC, 0xDB, 0xDD, 0xC0]
    )


def test_roundtrip_random():
    rng = random.Random(1)
    dec = kiss.Decoder()
    for _ in range(500):
        payload = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 400)))
        t = rng.randrange(16)
        stream = kiss.encode(t, payload)
        frames = []
        # feed in random chunk sizes to exercise the streaming decoder
        i = 0
        while i < len(stream):
            n = rng.randrange(1, 64)
            frames += dec.feed(stream[i : i + n])
            i += n
        assert len(frames) == 1
        assert frames[0].type_byte == t
        assert frames[0].payload == payload


def test_unescape_inverse():
    rng = random.Random(2)
    for _ in range(200):
        data = bytes(rng.choice([0xC0, 0xDB, 0xDC, 0xDD, 0x00, 0x41]) for _ in range(50))
        esc = kiss.escape(data)
        assert 0xC0 not in esc
        assert kiss.unescape(esc) == data


def test_back_to_back_and_garbage():
    dec = kiss.Decoder()
    stream = b"junk" + kiss.encode(0, b"a") + bytes([0xC0, 0xC0]) + kiss.encode(6, b"OK") + kiss.encode(0x0C, b"\x00\x05")
    frames = dec.feed(stream)
    assert [(f.command, f.payload) for f in frames] == [(0, b"a"), (6, b"OK"), (0x0C, b"\x00\x05")]


def test_ackmode_and_sethw_encoding():
    assert kiss.encode_ackmode(0x1234, b"X") == bytes([0xC0, 0x0C, 0x12, 0x34, ord("X"), 0xC0])
    assert kiss.encode_sethw("GET") == bytes([0xC0, 0x06]) + b"GET" + bytes([0xC0])
    # a sequence number containing FEND must be escaped
    assert kiss.encode_ackmode(0xC0DB, b"") == bytes([0xC0, 0x0C, 0xDB, 0xDC, 0xDB, 0xDD, 0xC0])


def test_return_command():
    f = kiss.Decoder().feed(bytes([0xC0, 0xFF, 0xC0]))[0]
    assert f.command == kiss.CMD_RETURN
