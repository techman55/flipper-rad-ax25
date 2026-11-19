/* AFSK TX: tone frequencies, phase continuity, no drift. */
#include <math.h>
#include <string.h>

#include "afsk_tx.h"
#include "bitbuf.h"
#include "testlib.h"

typedef struct {
    uint32_t dur[200000];
    bool lvl[200000];
    size_t n;
    uint64_t total;
} Edges;

static Edges E;

static void run(const uint8_t* bits, size_t nbits) {
    AfskTx g;
    afsk_tx_init(&g, bits, nbits);
    E.n = 0;
    E.total = 0;
    bool level;
    uint32_t d;
    while(afsk_tx_next(&g, &level, &d) && E.n < 200000) {
        E.dur[E.n] = d;
        E.lvl[E.n] = level;
        E.total += d;
        E.n++;
    }
}

static uint8_t bits[4096];

static void test_pure_tone(bool mark, double expect_hz) {
    size_t nbits = 2400; // 2 seconds
    memset(bits, mark ? 0xFF : 0x00, sizeof(bits));
    run(bits, nbits);

    // Skip the trailing partial half-period.
    uint64_t sum = 0;
    for(size_t i = 0; i + 1 < E.n; i++) sum += E.dur[i];
    double mean_half = (double)sum / (double)(E.n - 1);
    double f = 1e6 / (2.0 * mean_half);
    CHECK_MSG(fabs(f - expect_hz) < 0.5, "tone %.3f Hz, want %.1f", f, expect_hz);

    // Every half-period within 1 µs of ideal (µs-grid quantisation only).
    double ideal = 1e6 / (2.0 * expect_hz);
    int bad = 0;
    for(size_t i = 0; i + 1 < E.n; i++) {
        if(fabs((double)E.dur[i] - ideal) > 1.0) bad++;
    }
    CHECK_MSG(bad == 0, "%d half-periods off by > 1 µs", bad);

    // Burst length is exactly nbits/1200 s.
    CHECK_MSG(E.total == bit_start_us(nbits, 1200), "total %llu", (unsigned long long)E.total);
}

static void test_levels_alternate_and_start_high(void) {
    for(size_t i = 0; i < 64; i++) bits[i] = (uint8_t)rnd();
    run(bits, 512);
    CHECK(E.n > 0 && E.lvl[0] == true);
    int bad = 0;
    for(size_t i = 1; i < E.n; i++) {
        if(E.lvl[i] == E.lvl[i - 1]) bad++;
    }
    CHECK(bad == 0);
}

static void test_phase_continuity(void) {
    // Random bits. Integrate the instantaneous tone frequency over time to get the
    // ideal phase; each emitted edge must sit within one µs of a half-cycle boundary
    // of that continuous phase. A generator that reset phase at bit boundaries
    // would drift off by a large fraction of a cycle within a few bits.
    size_t nbits = 3000;
    for(size_t i = 0; i < nbits / 8 + 1; i++) bits[i] = (uint8_t)rnd();
    run(bits, nbits);

    uint64_t t = 0;
    double worst = 0.0;
    for(size_t i = 0; i + 1 < E.n; i++) {
        t += E.dur[i];
        // Phase (in cycles) at time t, integrating piecewise-constant frequency.
        double phase = 0.0;
        size_t k = 0;
        double tb_prev = 0.0;
        for(;; k++) {
            double tb = (double)bit_start_us(k + 1, 1200);
            double f = bitbuf_get(bits, k) ? 1200.0 : 2200.0;
            if((double)t <= tb) {
                phase += f * ((double)t - tb_prev) * 1e-6;
                break;
            }
            phase += f * (tb - tb_prev) * 1e-6;
            tb_prev = tb;
        }
        // Edge i should be at half-cycle number i+1.
        double err_cycles = phase - 0.5 * (double)(i + 1);
        // Allow up to 1 µs at 2200 Hz (0.0022 cycles) of grid quantisation.
        if(fabs(err_cycles) > worst) worst = fabs(err_cycles);
    }
    CHECK_MSG(worst < 0.0025, "worst phase error %.5f cycles", worst);
}

int main(void) {
    test_pure_tone(true, 1200.0);
    test_pure_tone(false, 2200.0);
    test_levels_alternate_and_start_high();
    test_phase_continuity();
    return test_summary("test_afsk_tx");
}
