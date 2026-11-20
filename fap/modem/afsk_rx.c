#include "afsk_rx.h"

#include "afsk_tx.h"

/* Nominal half-periods in µs. */
#define HALF_MARK_US  (1000000.0f / (2.0f * (float)AFSK_MARK_HZ)) /* 416.7 */
#define HALF_SPACE_US (1000000.0f / (2.0f * (float)AFSK_SPACE_HZ)) /* 227.3 */

void afsk_rx_init(AfskRx* rx) {
    clock_recovery_init(&rx->cr, AFSK_BAUD);
    afsk_rx_reset(rx);
}

void afsk_rx_reset(AfskRx* rx) {
    clock_recovery_reset(&rx->cr);
    rx->tone = true;
    rx->have_tone = false;
    rx->have_pending = false;
    rx->pending_us = 0.0f;
}

static float half_of(bool tone) {
    return tone ? HALF_MARK_US : HALF_SPACE_US;
}

/* How much of a half-period of duration d looks like `other` instead of `self` (0..1). */
static float straddle_score(float d, bool self, bool other) {
    float hs = half_of(self), ho = half_of(other);
    float s = (d - hs) / (ho - hs);
    if(s < 0.0f) s = 0.0f;
    if(s > 1.0f) s = 1.0f;
    return s;
}

/*
 * A half-period of duration d that starts at tone `first` and ends at tone `second`
 * covers half a cycle in total: a / h_first + (d - a) / h_second = 1.
 * Returns a, the time spent in the first tone, clamped to [0, d].
 */
static float split_point(float d, bool first, bool second) {
    float h1 = half_of(first), h2 = half_of(second);
    float a = h1 * (h2 - d) / (h2 - h1);
    if(a < 0.0f) a = 0.0f;
    if(a > d) a = d;
    return a;
}

static size_t advance(AfskRx* rx, bool tone, float us, uint8_t* out, size_t n) {
    if(us <= 0.0f) return n;
    return n + clock_recovery_advance(
                   &rx->cr, tone, (uint32_t)(us + 0.5f), out + n, CLOCK_MAX_BITS_PER_RUN - n);
}

size_t afsk_rx_halfperiod(AfskRx* rx, bool level, uint32_t duration_us, uint8_t* out) {
    (void)level; // only the spacing of edges carries information
    float d = (float)duration_us;
    size_t n = 0;

    bool tone;
    if(duration_us < AFSK_RX_GLITCH_US) {
        tone = rx->have_tone ? rx->tone : true; // noise spike: keep the current tone
    } else if(duration_us > AFSK_RX_MAX_HALF_US) {
        tone = true; // no tone at all; looks like a long mark
    } else {
        tone = duration_us > AFSK_RX_THRESHOLD_US;
    }

    if(!rx->have_pending) {
        rx->pending_tone = tone;
        rx->pending_us = d;
        rx->pending_whole = true;
        rx->have_pending = true;
        rx->have_tone = true;
        rx->tone = tone;
        return 0;
    }

    if(tone == rx->pending_tone) {
        n = advance(rx, rx->pending_tone, rx->pending_us, out, n);
        rx->pending_us = d;
        rx->pending_whole = true;
    } else {
        // Tone change somewhere around the boundary between `pending` and this
        // half-period. One of the two straddles the bit boundary: pick the one
        // whose length sits furthest toward the other tone's half-period.
        bool old_t = rx->pending_tone;
        float s_pending =
            rx->pending_whole ? straddle_score(rx->pending_us, old_t, tone) : 0.0f;
        float s_new = straddle_score(d, tone, old_t);

        if(s_pending >= s_new) {
            float a = split_point(rx->pending_us, old_t, tone);
            n = advance(rx, old_t, a, out, n);
            clock_recovery_edge(&rx->cr);
            n = advance(rx, tone, rx->pending_us - a, out, n);
            rx->pending_us = d;
            rx->pending_whole = true;
        } else {
            n = advance(rx, old_t, rx->pending_us, out, n);
            float a = split_point(d, old_t, tone);
            n = advance(rx, old_t, a, out, n);
            clock_recovery_edge(&rx->cr);
            rx->pending_us = d - a; // remainder of this half-period, already in the new tone
            rx->pending_whole = false;
        }
        rx->pending_tone = tone;
    }
    rx->tone = tone;
    return n;
}
