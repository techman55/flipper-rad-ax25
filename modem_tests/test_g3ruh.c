/* G3RUH scrambler: round-trip, self-synchronisation, polarity behaviour. */
#include "g3ruh.h"
#include "testlib.h"

static void test_roundtrip(void) {
    for(int trial = 0; trial < 50; trial++) {
        uint32_t seed = rnd();
        G3ruh s, d;
        g3ruh_init(&s, seed);
        g3ruh_init(&d, seed);
        int errors = 0;
        for(int i = 0; i < 5000; i++) {
            bool in = rnd() & 1;
            if(g3ruh_descramble(&d, g3ruh_scramble(&s, in)) != in) errors++;
        }
        CHECK_MSG(errors == 0, "seed %08x: %d errors", seed, errors);
    }
}

static void test_self_sync(void) {
    // Descrambler starts in a random state unrelated to the scrambler.
    // It must be correct from bit 17 on.
    for(int trial = 0; trial < 50; trial++) {
        G3ruh s, d;
        g3ruh_init(&s, rnd());
        g3ruh_init(&d, rnd());
        int late_errors = 0;
        for(int i = 0; i < 3000; i++) {
            bool in = rnd() & 1;
            bool out = g3ruh_descramble(&d, g3ruh_scramble(&s, in));
            if(i >= 17 && out != in) late_errors++;
        }
        CHECK_MSG(late_errors == 0, "trial %d: %d errors after sync", trial, late_errors);
    }
}

static void test_whitening(void) {
    // An all-ones input (worst case for clock recovery) must come out roughly balanced.
    G3ruh s;
    g3ruh_init(&s, 1);
    int ones = 0;
    int longest = 0, run = 0;
    bool prev = false;
    for(int i = 0; i < 20000; i++) {
        bool b = g3ruh_scramble(&s, 1);
        ones += b;
        run = (i > 0 && b == prev) ? run + 1 : 1;
        if(run > longest) longest = run;
        prev = b;
    }
    CHECK_MSG(ones > 9000 && ones < 11000, "ones=%d", ones);
    CHECK_MSG(longest <= 20, "longest run %d", longest);
}

static void test_inversion(void) {
    // The descrambler is linear with 3 taps: inverting the line inverts its output.
    // NRZI after it makes the whole RX chain polarity-blind.
    G3ruh s, d1, d2;
    g3ruh_init(&s, 0x1ABCD);
    g3ruh_init(&d1, 0);
    g3ruh_init(&d2, 0x1FFFF);
    int mismatches = 0;
    for(int i = 0; i < 2000; i++) {
        bool line = g3ruh_scramble(&s, rnd() & 1);
        bool a = g3ruh_descramble(&d1, line);
        bool b = g3ruh_descramble(&d2, !line);
        if(i >= 17 && a == b) mismatches++;
    }
    CHECK(mismatches == 0);
}

int main(void) {
    test_roundtrip();
    test_self_sync();
    test_whitening();
    test_inversion();
    return test_summary("test_g3ruh");
}
