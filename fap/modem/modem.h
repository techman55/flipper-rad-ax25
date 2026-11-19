/**
 * @file modem.h
 * Mode-level glue: build a complete TX burst, and run the full RX chain.
 *
 *   TX: frames -> HDLC (flags, stuffing, FCS) -> NRZI -> [G3RUH] -> line bits
 *       line bits -> fsk_tx / afsk_tx -> (level, duration) stream
 *   RX: (level, duration) -> fsk_rx / afsk_rx -> line bits -> [G3RUH^-1]
 *       -> NRZI decode -> HDLC deframer -> FCS check -> callback
 *
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "afsk_rx.h"
#include "afsk_tx.h"
#include "bitbuf.h"
#include "fsk_rx.h"
#include "fsk_tx.h"
#include "g3ruh.h"
#include "hdlc.h"

typedef enum {
    ModemModeFsk9600 = 0, /**< 2-FSK 9600, NRZI + G3RUH */
    ModemModeAfsk1200 = 1, /**< Bell 202 square-wave FM, NRZI */
    ModemModeFsk1200 = 2, /**< 2-FSK 1200, NRZI, no scrambler */
    ModemModeCount,
} ModemMode;

const char* modem_mode_name(ModemMode mode);
uint32_t modem_mode_baud(ModemMode mode);
bool modem_mode_scrambled(ModemMode mode);

/* ---------- TX ---------- */

/** A frame to put in a burst (AX.25 bytes without FCS). */
typedef struct {
    const uint8_t* data;
    size_t len;
} ModemFrame;

/** Number of flags needed to cover `ms` milliseconds at the mode's bit rate (at least 1). */
unsigned modem_flags_for_ms(ModemMode mode, uint32_t ms);

/**
 * Build the line-bit stream for a whole burst:
 *   txdelay flags, frame, flag, frame, ..., txtail flags.
 * @return number of line bits, or 0 if `out` was too small.
 */
size_t modem_build_burst(
    ModemMode mode,
    const ModemFrame* frames,
    size_t nframes,
    uint32_t txdelay_ms,
    uint32_t txtail_ms,
    uint8_t* out,
    size_t out_cap_bits);

/** Worst-case line bits for one frame of `len` bytes (stuffing + FCS + separator flag). */
size_t modem_frame_bits_max(size_t len);

/** Streaming (level, duration) generator over a built burst. */
typedef struct {
    ModemMode mode;
    union {
        FskTx fsk;
        AfskTx afsk;
    } u;
} ModemTx;

/**
 * @param start_high  make the very first emitted level high (the Flipper's async
 *                    TX skips leading lows). Polarity is irrelevant after NRZI.
 */
void modem_tx_init(ModemTx* tx, ModemMode mode, const uint8_t* bits, size_t nbits, bool start_high);
bool modem_tx_next(ModemTx* tx, bool* level, uint32_t* duration_us);

/** Total on-air duration of a burst in µs. */
uint32_t modem_burst_duration_us(ModemMode mode, size_t nbits);

/* ---------- RX ---------- */

/** Called for every completed HDLC frame. `frame`/`len` exclude the FCS. */
typedef void (*ModemFrameCallback)(void* ctx, const uint8_t* frame, size_t len, bool fcs_ok);

typedef struct {
    ModemMode mode;
    FskRx fsk;
    AfskRx afsk;
    G3ruh descrambler;
    NrziDec nrzi;
    HdlcRx hdlc;
    ModemFrameCallback cb;
    void* cb_ctx;
    uint32_t frames_ok;
    uint32_t frames_bad;
} ModemRx;

void modem_rx_init(ModemRx* rx, ModemMode mode, ModemFrameCallback cb, void* ctx);

/** Drop bit-sync and any partial frame (e.g. when squelch closes or after TX). */
void modem_rx_reset(ModemRx* rx);

/** Feed one captured (level, duration) edge. */
void modem_rx_edge(ModemRx* rx, bool level, uint32_t duration_us);
