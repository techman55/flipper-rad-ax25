#include "afsk_rx.h"

#include "afsk_tx.h"

void afsk_rx_init(AfskRx* rx) {
    clock_recovery_init(&rx->cr, AFSK_BAUD);
    // Tone boundaries are only known to within ~half a half-period, so pull
    // the clock a little more gently than for direct FSK.
    rx->cr.phase_gain = 0.25f;
    afsk_rx_reset(rx);
}

void afsk_rx_reset(AfskRx* rx) {
    clock_recovery_reset(&rx->cr);
    rx->tone = true;
    rx->have_tone = false;
}

size_t afsk_rx_halfperiod(AfskRx* rx, bool level, uint32_t duration_us, uint8_t* out) {
    (void)level; // only the spacing of edges carries information

    bool tone;
    if(duration_us < AFSK_RX_GLITCH_US) {
        tone = rx->tone; // noise spike: keep whatever tone we were in
    } else if(duration_us > AFSK_RX_MAX_HALF_US) {
        tone = true;
    } else {
        tone = duration_us > AFSK_RX_THRESHOLD_US;
    }

    if(rx->have_tone && tone != rx->tone) {
        clock_recovery_edge(&rx->cr);
    }
    rx->tone = tone;
    rx->have_tone = true;

    return clock_recovery_advance(&rx->cr, tone, duration_us, out, CLOCK_MAX_BITS_PER_RUN);
}
