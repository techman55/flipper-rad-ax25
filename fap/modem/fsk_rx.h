/**
 * @file fsk_rx.h
 * Bit clock recovery: (level, duration) runs -> bits.
 *
 * Integrate-and-dump over each bit window, with a DPLL that pulls the window
 * boundaries toward observed edges (phase) and nudges the bit time (frequency)
 * to absorb crystal offset. Used directly for FSK9600/FSK1200, and by afsk_rx
 * on the recovered tone stream.
 *
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** At most this many bits come out of one run (spec: clamp n <= 32). */
#define CLOCK_MAX_BITS_PER_RUN 32u

typedef struct {
    float bit_us_nominal; /**< nominal bit time in µs */
    float bit_us; /**< current bit time estimate in µs */
    float phase; /**< position inside the current bit window, [0, 1) */
    float acc; /**< integrated level over the window: +time high, -time low (bits) */
    float since_edge; /**< bits elapsed since the last edge */
    float phase_gain; /**< fraction of the edge timing error corrected per edge */
    float freq_gain; /**< fraction of the edge timing error applied to bit_us */
    float freq_limit; /**< max relative deviation of bit_us from nominal */
    float min_run; /**< runs shorter than this (bits) are glitches and do not steer the loop */
    float lock_window; /**< only adjust frequency when |edge error| is below this (bits) */
} ClockRecovery;

void clock_recovery_init(ClockRecovery* cr, uint32_t baud);

/** Reset phase/frequency tracking (e.g. after squelch closes). */
void clock_recovery_reset(ClockRecovery* cr);

/**
 * Advance time by `duration_us` with the input at `level`. No edge implied.
 * Writes up to `cap` recovered bits (0/1) to `out` and returns how many.
 */
size_t clock_recovery_advance(
    ClockRecovery* cr,
    bool level,
    uint32_t duration_us,
    uint8_t* out,
    size_t cap);

/** Tell the DPLL that the input changed level right now. */
void clock_recovery_edge(ClockRecovery* cr);

/** FSK receiver: each run is (level held for duration), followed by an edge. */
typedef struct {
    ClockRecovery cr;
} FskRx;

void fsk_rx_init(FskRx* rx, uint32_t baud);

/** Feed one run. Returns the number of line bits written to out (<= CLOCK_MAX_BITS_PER_RUN). */
size_t fsk_rx_run(FskRx* rx, bool level, uint32_t duration_us, uint8_t* out);
