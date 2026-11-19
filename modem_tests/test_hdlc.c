/* HDLC: FCS vectors, bit stuffing, NRZI, deframer edge cases. */
#include <string.h>

#include "hdlc.h"
#include "testlib.h"

static void test_fcs_vectors(void) {
    // CRC-16/X.25 check value from the CRC catalogue.
    const char* s = "123456789";
    CHECK(hdlc_fcs((const uint8_t*)s, 9) == 0x906E);
    // Empty input: init 0xFFFF xor-out 0xFFFF.
    CHECK(hdlc_fcs(NULL, 0) == 0x0000);
    // Residue property: CRC over data + FCS (low byte first) gives the constant 0xF0B8
    // before the final XOR, i.e. 0x0F47 after it.
    uint8_t buf[11];
    memcpy(buf, s, 9);
    uint16_t f = hdlc_fcs(buf, 9);
    buf[9] = (uint8_t)(f & 0xFF);
    buf[10] = (uint8_t)(f >> 8);
    CHECK(hdlc_fcs(buf, 11) == (0xF0B8 ^ 0xFFFF));
    // A real AX.25 UI frame (N0CALL>TEST: hi); value from an independent table-free Python CRC-16/X.25.
    const uint8_t ui[] = {0xA8, 0x8A, 0xA6, 0xA8, 0x40, 0x40, 0xE0, 0x9C, 0x60, 0x86,
                          0x82, 0x98, 0x98, 0x61, 0x03, 0xF0, 0x68, 0x69};
    CHECK(hdlc_fcs(ui, sizeof(ui)) == 0x58CF);
}

/* Feed raw (pre-NRZI) HDLC bits straight into the deframer. */
typedef struct {
    uint8_t frames[8][AX25_MAX_FRAME + 2];
    size_t lens[8];
    bool ok[8];
    int count;
} Sink;

static void decode_bits(const BitBuf* b, Sink* sink) {
    HdlcRx rx;
    hdlc_rx_init(&rx);
    sink->count = 0;
    for(size_t i = 0; i < b->nbits; i++) {
        if(hdlc_rx_bit(&rx, bitbuf_get(b->data, i)) == HdlcRxFrame && sink->count < 8) {
            memcpy(sink->frames[sink->count], rx.buf, rx.len - 2);
            sink->lens[sink->count] = rx.len - 2;
            sink->ok[sink->count] = hdlc_rx_fcs_ok(&rx);
            sink->count++;
        }
    }
}

static void max_ones_run(const BitBuf* b, size_t from, size_t to, int* max_run) {
    int run = 0;
    *max_run = 0;
    for(size_t i = from; i < to; i++) {
        run = bitbuf_get(b->data, i) ? run + 1 : 0;
        if(run > *max_run) *max_run = run;
    }
}

static void roundtrip(const uint8_t* frame, size_t len, const char* what) {
    static uint8_t store[4096];
    BitBuf b;
    bitbuf_init(&b, store, sizeof(store) * 8);
    hdlc_put_flags(&b, 3);
    size_t body_start = b.nbits;
    hdlc_put_frame(&b, frame, len);
    size_t body_end = b.nbits;
    hdlc_put_flags(&b, 2);
    CHECK(!b.overflow);

    int max_run;
    max_ones_run(&b, body_start, body_end, &max_run);
    CHECK_MSG(max_run <= 5, "%s: %d ones in a row inside stuffed body", what, max_run);

    Sink sink;
    decode_bits(&b, &sink);
    CHECK_MSG(sink.count == 1, "%s: got %d frames", what, sink.count);
    if(sink.count == 1) {
        CHECK_MSG(sink.ok[0], "%s: FCS bad", what);
        CHECK_MSG(sink.lens[0] == len && memcmp(sink.frames[0], frame, len) == 0, "%s: bytes differ", what);
    }
}

static void test_stuffing(void) {
    uint8_t f[AX25_MAX_FRAME];

    memset(f, 0xFF, sizeof(f));
    roundtrip(f, 20, "all 0xFF");
    roundtrip(f, AX25_MAX_FRAME, "all 0xFF max length");

    memset(f, 0x7E, sizeof(f));
    roundtrip(f, 20, "all flag-like 0x7E");

    memset(f, 0x00, sizeof(f));
    roundtrip(f, 20, "all zero");

    // Five ones straddling byte boundaries (LSB first): 0xF8 then 0x03 -> 11111 + 11.
    for(size_t i = 0; i < 40; i++) f[i] = (i & 1) ? 0x03 : 0xF8;
    roundtrip(f, 40, "five-ones across byte boundary");

    // Exactly five ones then a real zero (must still get a stuffed zero).
    for(size_t i = 0; i < 20; i++) f[i] = 0x1F;
    roundtrip(f, 20, "0x1F (exactly five ones)");

    // Ones right at the end, next to the FCS and closing flag.
    for(size_t i = 0; i < 19; i++) f[i] = (uint8_t)i;
    f[19] = 0xFF;
    roundtrip(f, 20, "trailing 0xFF");

    for(int trial = 0; trial < 300; trial++) {
        size_t len = AX25_MIN_FRAME + rnd() % (AX25_MAX_FRAME - AX25_MIN_FRAME + 1);
        for(size_t i = 0; i < len; i++) f[i] = (uint8_t)rnd();
        roundtrip(f, len, "random");
    }
}

static void test_stuffed_bit_count(void) {
    // 0xFF x2 + FCS: 16 ones -> 3 stuffed zeros in the data part alone.
    static uint8_t store[64];
    BitBuf b;
    bitbuf_init(&b, store, sizeof(store) * 8);
    const uint8_t two[2] = {0xFF, 0xFF};
    hdlc_put_frame(&b, two, 2);
    CHECK(b.nbits >= 16 + 3 + 16);
}

static void test_deframer_edges(void) {
    static uint8_t store[4096];
    uint8_t f[40];
    for(size_t i = 0; i < sizeof(f); i++) f[i] = (uint8_t)(i * 7 + 1);
    Sink sink;
    BitBuf b;

    // Abort (7 ones) in the middle of a frame kills it.
    bitbuf_init(&b, store, sizeof(store) * 8);
    hdlc_put_flags(&b, 2);
    hdlc_put_frame(&b, f, 20);
    size_t cut = b.nbits - 30;
    b.nbits = cut;
    for(int i = 0; i < 8; i++) bitbuf_put(&b, 1);
    hdlc_put_flags(&b, 2);
    decode_bits(&b, &sink);
    CHECK(sink.count == 0);

    // Too short frames are ignored.
    bitbuf_init(&b, store, sizeof(store) * 8);
    hdlc_put_flags(&b, 2);
    hdlc_put_frame(&b, f, AX25_MIN_FRAME - 1);
    hdlc_put_flags(&b, 2);
    decode_bits(&b, &sink);
    CHECK(sink.count == 0);

    // Back-to-back frames separated by a single shared flag.
    bitbuf_init(&b, store, sizeof(store) * 8);
    hdlc_put_flags(&b, 2);
    hdlc_put_frame(&b, f, 20);
    hdlc_put_flags(&b, 1);
    hdlc_put_frame(&b, f + 5, 30);
    hdlc_put_flags(&b, 1);
    decode_bits(&b, &sink);
    CHECK(sink.count == 2);
    CHECK(sink.count == 2 && sink.lens[0] == 20 && sink.lens[1] == 30);
    CHECK(sink.count == 2 && sink.ok[0] && sink.ok[1]);

    // Corrupted bit -> frame still delimited but FCS bad.
    bitbuf_init(&b, store, sizeof(store) * 8);
    hdlc_put_flags(&b, 2);
    size_t s = b.nbits;
    hdlc_put_frame(&b, f, 20);
    hdlc_put_flags(&b, 2);
    // Flip a 0 that is followed by a 0 so we do not create a flag/abort pattern.
    for(size_t i = s + 40; i < s + 120; i++) {
        if(!bitbuf_get(store, i) && !bitbuf_get(store, i + 1) && !bitbuf_get(store, i - 1)) {
            bitbuf_set(store, i, 1);
            break;
        }
    }
    decode_bits(&b, &sink);
    CHECK(sink.count == 1 && !sink.ok[0]);

    // Oversize frame is dropped, not overflowed.
    static uint8_t big[AX25_MAX_FRAME + 50];
    memset(big, 0x55, sizeof(big));
    bitbuf_init(&b, store, sizeof(store) * 8);
    hdlc_put_flags(&b, 2);
    hdlc_put_frame(&b, big, sizeof(big));
    hdlc_put_flags(&b, 2);
    decode_bits(&b, &sink);
    CHECK(sink.count == 0);
}

static void test_nrzi(void) {
    NrziEnc e = {.level = false};
    NrziDec d = {.prev = false};
    for(int i = 0; i < 1000; i++) {
        bool bit = rnd() & 1;
        bool lvl = nrzi_encode(&e, bit);
        CHECK(nrzi_decode(&d, lvl) == bit);
    }
    // 0 toggles, 1 holds.
    NrziEnc e2 = {.level = false};
    CHECK(nrzi_encode(&e2, 0) == true);
    CHECK(nrzi_encode(&e2, 1) == true);
    CHECK(nrzi_encode(&e2, 0) == false);
    // Decoder is polarity-blind.
    NrziEnc e3 = {.level = false};
    NrziDec d3 = {.prev = true};
    bool first = true;
    for(int i = 0; i < 200; i++) {
        bool bit = rnd() & 1;
        bool got = nrzi_decode(&d3, !nrzi_encode(&e3, bit));
        if(!first) CHECK(got == bit);
        first = false;
    }
}

int main(void) {
    test_fcs_vectors();
    test_stuffing();
    test_stuffed_bit_count();
    test_deframer_edges();
    test_nrzi();
    return test_summary("test_hdlc");
}
