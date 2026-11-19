/**
 * @file g3ruh.h
 * G3RUH self-synchronizing scrambler, polynomial x^17 + x^12 + 1.
 *
 * Same convention as Direwolf: the shift register holds previous *line* bits
 * (scrambler outputs / descrambler inputs), newest in bit 0.
 *   scramble:   out = in ^ s[12] ^ s[17];  s <<= out
 *   descramble: out = in ^ s[12] ^ s[17];  s <<= in
 * where s[k] is the line bit k positions back.
 *
 * TX order: data -> HDLC -> NRZI -> scramble -> radio.
 * RX order: radio -> descramble -> NRZI decode -> HDLC.
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sr;
} G3ruh;

static inline void g3ruh_init(G3ruh* g, uint32_t seed) {
    g->sr = seed & 0x1FFFFu;
}

static inline bool g3ruh_scramble(G3ruh* g, bool in) {
    bool out = in ^ ((g->sr >> 11) & 1u) ^ ((g->sr >> 16) & 1u);
    g->sr = ((g->sr << 1) | out) & 0x1FFFFu;
    return out;
}

static inline bool g3ruh_descramble(G3ruh* g, bool in) {
    bool out = in ^ ((g->sr >> 11) & 1u) ^ ((g->sr >> 16) & 1u);
    g->sr = ((g->sr << 1) | in) & 0x1FFFFu;
    return out;
}
