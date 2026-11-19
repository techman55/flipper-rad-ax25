/* KISS escaping round-trip and AX.25 address helpers. */
#include <string.h>

#include "ax25.h"
#include "hdlc.h"
#include "kiss.h"
#include "testlib.h"

static void test_kiss_escape(void) {
    const uint8_t payload[] = {0x01, KISS_FEND, 0x02, KISS_FESC, KISS_TFEND, KISS_TFESC, 0xC0, 0xDB};
    uint8_t enc[64];
    size_t n = kiss_encode(0x00, payload, sizeof(payload), enc, sizeof(enc));
    const uint8_t want[] = {0xC0, 0x00, 0x01, 0xDB, 0xDC, 0x02, 0xDB, 0xDD, 0xDC,
                            0xDD, 0xDB, 0xDC, 0xDB, 0xDD, 0xC0};
    CHECK(n == sizeof(want) && memcmp(enc, want, n) == 0);

    KissDecoder d;
    kiss_decoder_init(&d);
    int frames = 0;
    for(size_t i = 0; i < n; i++) {
        if(kiss_decoder_byte(&d, enc[i])) {
            frames++;
            CHECK(d.len == sizeof(payload) + 1);
            CHECK(d.buf[0] == 0x00);
            CHECK(memcmp(d.buf + 1, payload, sizeof(payload)) == 0);
        }
    }
    CHECK(frames == 1);
}

static void test_kiss_stream(void) {
    // Random payloads, back to back, with extra FENDs and garbage before the first FEND.
    KissDecoder d;
    kiss_decoder_init(&d);
    const uint8_t junk[] = {'h', 'i', 0x00};
    for(size_t i = 0; i < sizeof(junk); i++) CHECK(!kiss_decoder_byte(&d, junk[i]));

    for(int t = 0; t < 200; t++) {
        uint8_t payload[300];
        size_t len = 1 + rnd() % sizeof(payload);
        for(size_t i = 0; i < len; i++) payload[i] = (uint8_t)rnd();
        if(t % 3 == 0) payload[0] = KISS_FEND;
        uint8_t enc[700];
        size_t n = kiss_encode((uint8_t)(t & 0x0F), payload, len, enc, sizeof(enc));
        CHECK(n > 0);
        int got = 0;
        if(t % 5 == 0) kiss_decoder_byte(&d, KISS_FEND); // doubled FEND
        for(size_t i = 0; i < n; i++) {
            if(kiss_decoder_byte(&d, enc[i])) {
                got++;
                CHECK(d.len == len + 1 && d.buf[0] == (t & 0x0F));
                CHECK(memcmp(d.buf + 1, payload, len) == 0);
            }
        }
        CHECK(got == 1);
    }
}

static void test_kiss_overflow(void) {
    KissDecoder d;
    kiss_decoder_init(&d);
    kiss_decoder_byte(&d, KISS_FEND);
    for(int i = 0; i < 1000; i++) CHECK(!kiss_decoder_byte(&d, 0x55));
    CHECK(!kiss_decoder_byte(&d, KISS_FEND));
    CHECK(d.dropped == 1);
    // Recovers for the next frame.
    uint8_t enc[16];
    const uint8_t p[] = {1, 2, 3};
    size_t n = kiss_encode(0, p, 3, enc, sizeof(enc));
    int got = 0;
    for(size_t i = 0; i < n; i++) got += kiss_decoder_byte(&d, enc[i]);
    CHECK(got == 1);
    CHECK(kiss_encode(0, p, 3, enc, 4) == 0); // too small
}

static void test_ax25_calls(void) {
    char c[7];
    uint8_t s;
    CHECK(ax25_parse_call("kn4xxx-1", c, &s) && strcmp(c, "KN4XXX") == 0 && s == 1);
    CHECK(ax25_parse_call("WIDE2-15", c, &s) && s == 15);
    CHECK(ax25_parse_call("TEST", c, &s) && s == 0);
    CHECK(!ax25_parse_call("TOOLONG1", c, &s));
    CHECK(!ax25_parse_call("BAD-16", c, &s));
    CHECK(!ax25_parse_call("BAD-", c, &s));
    CHECK(!ax25_parse_call("", c, &s));
    CHECK(!ax25_parse_call("A*B", c, &s));
}

static void test_ax25_build_format(void) {
    uint8_t f[AX25_MAX_FRAME];
    const char* info = "hello";
    size_t n = ax25_build_ui("N0CALL", "TEST", NULL, (const uint8_t*)info, 5, f, sizeof(f));
    const uint8_t want[] = {0xA8, 0x8A, 0xA6, 0xA8, 0x40, 0x40, 0xE0, 0x9C, 0x60, 0x86, 0x82,
                            0x98, 0x98, 0x61, 0x03, 0xF0, 'h',  'e',  'l',  'l',  'o'};
    CHECK(n == sizeof(want) && memcmp(f, want, n) == 0);

    char out[128];
    CHECK(ax25_format(f, n, out, sizeof(out)));
    CHECK(strcmp(out, "N0CALL>TEST: hello") == 0);

    n = ax25_build_ui("KN4XXX-1", "APRS", "WIDE1-1,WIDE2-2", (const uint8_t*)"x\ty", 3, f, sizeof(f));
    CHECK(n == 4 * 7 + 2 + 3);
    CHECK((f[13] & 1) == 0 && (f[27] & 1) == 1); // extension bit only on the last address
    f[14 + 6] |= 0x80; // mark WIDE1-1 (3rd address, SSID byte at 20) as repeated
    CHECK(ax25_format(f, n, out, sizeof(out)));
    CHECK(strcmp(out, "KN4XXX-1>APRS,WIDE1-1*,WIDE2-2: x.y") == 0);

    // Truncated output stays NUL-terminated.
    char tiny[6];
    ax25_format(f, n, tiny, sizeof(tiny));
    CHECK(strlen(tiny) == 5);

    // Garbage does not crash and is reported as malformed.
    uint8_t junk[10] = {0};
    CHECK(!ax25_format(junk, sizeof(junk), out, sizeof(out)));

    CHECK(ax25_build_ui("N0CALL", "TEST", NULL, f, 400, f, sizeof(f)) == 0);
    CHECK(ax25_build_ui("N0CALL", "TEST", "BAD-99", NULL, 0, f, sizeof(f)) == 0);
}

int main(void) {
    test_kiss_escape();
    test_kiss_stream();
    test_kiss_overflow();
    test_ax25_calls();
    test_ax25_build_format();
    return test_summary("test_kiss_ax25");
}
