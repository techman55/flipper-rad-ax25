/**
 * @file afsk_tx.h
 * Bell 202 AFSK (mark 1200 Hz / space 2200 Hz, 1200 baud) as a square wave,
 * for "square-wave FM": the CC1101 runs 2-FSK and GDO0 toggles at the tone
 * rate, so an FM receiver's discriminator outputs the tone.
 *
 * Implementation is a DDS on the 1 µs grid the Flipper TX timer uses:
 *   - phase accumulator, one full tone cycle = 2^40, carried across bit
 *     boundaries (phase-continuous);
 *   - bit k starts at round(k * 1e6 / 1200) µs (no drift);
 *   - an edge is emitted at the first µs tick where the phase has crossed a
 *     half-cycle boundary, computed directly with one division (no per-µs loop).
 * Edge times are absolute, so rounding never accumulates.
 *
 * Line bit 1 = mark (1200 Hz), 0 = space (2200 Hz). Feed NRZI-encoded bits.
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AFSK_BAUD       1200u
#define AFSK_MARK_HZ    1200u
#define AFSK_SPACE_HZ   2200u
#define AFSK_PHASE_BITS 40u

typedef struct {
    const uint8_t* bits;
    size_t nbits;
    size_t pos; /**< index of the bit being transmitted */
    uint64_t t_us; /**< current position in time */
    uint64_t bit_end_us; /**< time the current bit ends */
    uint64_t last_edge_us; /**< time of the last emitted edge */
    uint64_t phase; /**< tone phase, mod 2^AFSK_PHASE_BITS */
    bool done;
} AfskTx;

void afsk_tx_init(AfskTx* g, const uint8_t* bits, size_t nbits);

/** Next half-period (level, duration). Returns false when finished. */
bool afsk_tx_next(AfskTx* g, bool* level, uint32_t* duration_us);
