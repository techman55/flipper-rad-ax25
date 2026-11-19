/**
 * @file fsk_tx.h
 * Line bits -> (level, duration) runs for direct 2-FSK.
 *
 * Bit k starts at round(k * 1e6 / baud) µs, so durations are integer µs with
 * no accumulated rounding error. Consecutive equal bits merge into one run.
 * Cheap enough to call from the Flipper's async TX DMA refill ISR.
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t* bits; /**< packed line bits (bitbuf layout) */
    size_t nbits;
    size_t pos;
    uint32_t baud;
    bool invert; /**< flip every output level */
} FskTx;

void fsk_tx_init(FskTx* g, const uint8_t* bits, size_t nbits, uint32_t baud, bool invert);

/** Next run. Returns false when the stream is exhausted. */
bool fsk_tx_next(FskTx* g, bool* level, uint32_t* duration_us);
