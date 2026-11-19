#include "fsk_tx.h"

#include "bitbuf.h"

void fsk_tx_init(FskTx* g, const uint8_t* bits, size_t nbits, uint32_t baud, bool invert) {
    g->bits = bits;
    g->nbits = nbits;
    g->pos = 0;
    g->baud = baud;
    g->invert = invert;
}

bool fsk_tx_next(FskTx* g, bool* level, uint32_t* duration_us) {
    if(g->pos >= g->nbits) return false;

    size_t start = g->pos;
    bool lv = bitbuf_get(g->bits, start);
    while(g->pos < g->nbits && bitbuf_get(g->bits, g->pos) == lv) {
        g->pos++;
    }
    *level = lv ^ g->invert;
    *duration_us = bit_start_us(g->pos, g->baud) - bit_start_us(start, g->baud);
    return true;
}
