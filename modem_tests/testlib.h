/* Minimal test harness: CHECK records failures, test_summary() returns the exit code. */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if(!(cond)) {                                                          \
            g_failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while(0)

#define CHECK_MSG(cond, ...)                                    \
    do {                                                        \
        g_checks++;                                             \
        if(!(cond)) {                                           \
            g_failures++;                                       \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
        }                                                       \
    } while(0)

static inline int test_summary(const char* name) {
    printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures ? 1 : 0;
}

/* Deterministic PRNG (xorshift32) so failures are reproducible. */
static uint32_t g_rng = 0x12345678u;
static inline uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
/* Uniform double in [-1, 1). */
static inline double rnd_sym(void) {
    return ((double)(rnd() & 0xFFFFFFu) / (double)0x800000u) - 1.0;
}
