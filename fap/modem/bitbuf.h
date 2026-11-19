/**
 * @file bitbuf.h
 * Tiny packed bit buffer helpers (bit i lives in byte i/8, bit position i%8).
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t* data; /**< storage, cap_bits/8 bytes (rounded up) */
    size_t cap_bits; /**< capacity in bits */
    size_t nbits; /**< bits written so far */
    bool overflow; /**< set when a write was dropped for lack of room */
} BitBuf;

static inline void bitbuf_init(BitBuf* b, uint8_t* storage, size_t cap_bits) {
    b->data = storage;
    b->cap_bits = cap_bits;
    b->nbits = 0;
    b->overflow = false;
}

static inline bool bitbuf_get(const uint8_t* data, size_t i) {
    return (data[i >> 3] >> (i & 7u)) & 1u;
}

static inline void bitbuf_put(BitBuf* b, bool bit) {
    if(b->nbits >= b->cap_bits) {
        b->overflow = true;
        return;
    }
    size_t i = b->nbits++;
    uint8_t mask = (uint8_t)(1u << (i & 7u));
    if(bit) {
        b->data[i >> 3] |= mask;
    } else {
        b->data[i >> 3] &= (uint8_t)~mask;
    }
}

static inline void bitbuf_set(uint8_t* data, size_t i, bool bit) {
    uint8_t mask = (uint8_t)(1u << (i & 7u));
    if(bit) {
        data[i >> 3] |= mask;
    } else {
        data[i >> 3] &= (uint8_t)~mask;
    }
}

/** Time (µs, rounded) at which bit k starts, for a bit stream at `baud`. Exact, no drift. */
static inline uint32_t bit_start_us(size_t k, uint32_t baud) {
    return (uint32_t)(((uint64_t)k * 1000000u + baud / 2u) / baud);
}
