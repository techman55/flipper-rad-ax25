#include "fsk_rx.h"


void clock_recovery_init(ClockRecovery* cr, uint32_t baud) {
    cr->bit_us_nominal = 1000000.0f / (float)baud;
    cr->phase_gain = 0.30f;
    cr->freq_gain = 0.01f;
    cr->freq_limit = 0.05f;
    cr->min_run = 0.5f;
    cr->lock_window = 0.25f;
    clock_recovery_reset(cr);
}

void clock_recovery_reset(ClockRecovery* cr) {
    cr->bit_us = cr->bit_us_nominal;
    cr->phase = 0.0f;
    cr->acc = 0.0f;
    cr->since_edge = 0.0f;
}

size_t clock_recovery_advance(
    ClockRecovery* cr,
    bool level,
    uint32_t duration_us,
    uint8_t* out,
    size_t cap) {
    size_t n = 0;
    float sign = level ? 1.0f : -1.0f;
    float adv = (float)duration_us / cr->bit_us; // in bits
    cr->since_edge += adv;

    // Close every bit window this run reaches the end of.
    while(cr->phase + adv >= 1.0f) {
        float portion = 1.0f - cr->phase;
        cr->acc += sign * portion;
        adv -= portion;
        cr->phase = 0.0f;
        if(n < cap) {
            out[n++] = cr->acc > 0.0f ? 1u : 0u;
        }
        cr->acc = 0.0f;
        if(n >= cap && adv >= 1.0f) {
            // Long run (no signal / stuck level): drop the excess whole bits cheaply.
            adv -= (float)(uint32_t)adv;
        }
    }
    cr->phase += adv;
    cr->acc += sign * adv;
    return n;
}

void clock_recovery_edge(ClockRecovery* cr) {
    // Ideally edges land on a window boundary (phase 0).
    // e > 0: the edge came late in our window, so our boundaries are early.
    // e < 0: the edge came just before our boundary, so our boundaries are late.
    float e = cr->phase >= 0.5f ? cr->phase - 1.0f : cr->phase;

    // Noise produces swarms of short runs; letting them steer the loop walks
    // the bit-time estimate off to its limit. Real data runs are >= 1 bit.
    bool glitch = cr->since_edge < cr->min_run;
    cr->since_edge = 0.0f;
    if(glitch) return;

    cr->phase -= cr->phase_gain * e;
    if(cr->phase < 0.0f) cr->phase = 0.0f;
    if(cr->phase > 0.999f) cr->phase = 0.999f; // next advance closes this window

    if(e > cr->lock_window || e < -cr->lock_window) return; // not locked: phase only
    cr->bit_us += cr->bit_us * cr->freq_gain * e;
    float lo = cr->bit_us_nominal * (1.0f - cr->freq_limit);
    float hi = cr->bit_us_nominal * (1.0f + cr->freq_limit);
    if(cr->bit_us < lo) cr->bit_us = lo;
    if(cr->bit_us > hi) cr->bit_us = hi;
}

void fsk_rx_init(FskRx* rx, uint32_t baud) {
    clock_recovery_init(&rx->cr, baud);
}

size_t fsk_rx_run(FskRx* rx, bool level, uint32_t duration_us, uint8_t* out) {
    size_t n = clock_recovery_advance(&rx->cr, level, duration_us, out, CLOCK_MAX_BITS_PER_RUN);
    clock_recovery_edge(&rx->cr);
    return n;
}
