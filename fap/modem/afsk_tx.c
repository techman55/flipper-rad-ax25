#include "afsk_tx.h"

#include "bitbuf.h"

#define PHASE_MASK ((1ull << AFSK_PHASE_BITS) - 1ull)
#define HALF_CYCLE (1ull << (AFSK_PHASE_BITS - 1u))

/* Phase increment per µs: f * 2^40 / 1e6. Truncation error < 1e-9 relative. */
#define INC_MARK  (((uint64_t)AFSK_MARK_HZ << AFSK_PHASE_BITS) / 1000000ull)
#define INC_SPACE (((uint64_t)AFSK_SPACE_HZ << AFSK_PHASE_BITS) / 1000000ull)

void afsk_tx_init(AfskTx* g, const uint8_t* bits, size_t nbits) {
    g->bits = bits;
    g->nbits = nbits;
    g->pos = 0;
    g->t_us = 0;
    g->bit_end_us = bit_start_us(1, AFSK_BAUD);
    g->last_edge_us = 0;
    g->phase = 0; // starts at the beginning of a high half-cycle
    g->done = (nbits == 0);
}

static inline bool level_of(uint64_t phase) {
    return ((phase >> (AFSK_PHASE_BITS - 1u)) & 1u) == 0;
}

bool afsk_tx_next(AfskTx* g, bool* level, uint32_t* duration_us) {
    if(g->done) return false;

    for(;;) {
        if(g->pos >= g->nbits) {
            // Flush the last partial half-cycle.
            g->done = true;
            if(g->t_us > g->last_edge_us) {
                *level = level_of(g->phase);
                *duration_us = (uint32_t)(g->t_us - g->last_edge_us);
                return true;
            }
            return false;
        }

        uint64_t inc = bitbuf_get(g->bits, g->pos) ? INC_MARK : INC_SPACE;
        uint64_t to_cross = HALF_CYCLE - (g->phase & (HALF_CYCLE - 1ull));
        uint64_t ticks = (to_cross + inc - 1ull) / inc; // first µs tick at/after the crossing

        if(g->t_us + ticks <= g->bit_end_us) {
            // The half-cycle ends inside this bit: emit an edge.
            *level = level_of(g->phase);
            g->t_us += ticks;
            g->phase = (g->phase + ticks * inc) & PHASE_MASK;
            *duration_us = (uint32_t)(g->t_us - g->last_edge_us);
            g->last_edge_us = g->t_us;
            return true;
        }

        // Run to the end of the bit, carry phase into the next bit.
        g->phase = (g->phase + (g->bit_end_us - g->t_us) * inc) & PHASE_MASK;
        g->t_us = g->bit_end_us;
        g->pos++;
        g->bit_end_us = bit_start_us(g->pos + 1u, AFSK_BAUD);
    }
}
