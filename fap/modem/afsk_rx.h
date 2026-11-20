/**
 * @file afsk_rx.h
 * EXPERIMENTAL Bell 202 AFSK receiver from CC1101 sliced edges.
 *
 * With the CC1101 2-FSK demodulator listening to an FM signal carrying AFSK,
 * the sliced data output toggles once per tone half-period: ~417 µs for mark
 * (1200 Hz) and ~227 µs for space (2200 Hz). Each half-period is classified
 * against a threshold (~322 µs), giving a tone-vs-time signal. That signal is
 * integrated over 833.33 µs bit windows (majority vote by time) with a DPLL
 * aligned on tone changes (clock_recovery in fsk_rx.h). Output bits are tone
 * levels (1 = mark); NRZI decode them next.
 *
 * Tone changes happen mid half-period. With one half-period of look-behind we
 * pick the half-period that straddles the change (its length lies between the
 * two nominal values) and place the boundary inside it by phase continuity:
 *   a / h_old + (d - a) / h_new = 1 half-cycle.
 * Without this, edges land up to ~0.25 bit off and jitter tolerance collapses.
 *
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsk_rx.h"

#define AFSK_RX_THRESHOLD_US 322u /**< between 227 µs (space) and 417 µs (mark) */
#define AFSK_RX_GLITCH_US    100u /**< shorter half-periods are noise, folded into the current tone */
#define AFSK_RX_MAX_HALF_US  1000u /**< longer is no tone at all; treated as mark (idle) */

typedef struct {
    ClockRecovery cr;
    bool tone; /**< current tone: true = mark */
    bool have_tone;
    /* One half-period of look-behind, so a tone change can be placed inside
     * whichever half-period actually straddles the bit boundary. */
    bool have_pending;
    bool pending_tone;
    bool pending_whole; /**< pending_us is a full half-period (not a split remainder) */
    float pending_us;
} AfskRx;

void afsk_rx_init(AfskRx* rx);
void afsk_rx_reset(AfskRx* rx);

/** Feed one sliced half-period. Writes up to CLOCK_MAX_BITS_PER_RUN tone bits to out. */
size_t afsk_rx_halfperiod(AfskRx* rx, bool level, uint32_t duration_us, uint8_t* out);
