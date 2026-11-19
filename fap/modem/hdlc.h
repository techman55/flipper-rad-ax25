/**
 * @file hdlc.h
 * HDLC framing for AX.25: flags, bit stuffing, NRZI, FCS (CRC-16/X.25).
 * Pure C, no Flipper dependencies.
 *
 * Bit order: every byte is sent LSB first. FCS is sent low byte first.
 * NRZI: a 0 bit is a level transition, a 1 bit keeps the level.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bitbuf.h"

#define HDLC_FLAG 0x7Eu

/** Largest AX.25 frame (without FCS) we accept from the host or from the air. */
#define AX25_MAX_FRAME 330u
/** Smallest valid AX.25 frame without FCS: 2 addresses (14) + control (1). */
#define AX25_MIN_FRAME 15u

/** CRC-16/X.25: reflected poly 0x8408, init 0xFFFF, final XOR 0xFFFF. "123456789" -> 0x906E. */
uint16_t hdlc_fcs(const uint8_t* data, size_t len);

/* ---------- TX ---------- */

/** Append `count` unstuffed flags. */
void hdlc_put_flags(BitBuf* out, unsigned count);

/** Append one frame body + FCS (low byte first), bit-stuffed. Does not add flags. */
void hdlc_put_frame(BitBuf* out, const uint8_t* frame, size_t len);

/** NRZI encoder state. */
typedef struct {
    bool level;
} NrziEnc;

/** Encode one data bit to a line level. 0 -> toggle, 1 -> hold. */
static inline bool nrzi_encode(NrziEnc* s, bool bit) {
    if(!bit) s->level = !s->level;
    return s->level;
}

/** NRZI decoder state. */
typedef struct {
    bool prev;
} NrziDec;

/** Decode one line level to a data bit. Same level -> 1, change -> 0. */
static inline bool nrzi_decode(NrziDec* s, bool level) {
    bool bit = (level == s->prev);
    s->prev = level;
    return bit;
}

/* ---------- RX ---------- */

typedef enum {
    HdlcRxNone = 0, /**< nothing to report */
    HdlcRxFrame, /**< a complete frame is in buf[0..len) (FCS included in the last 2 bytes) */
} HdlcRxResult;

typedef struct {
    uint8_t buf[AX25_MAX_FRAME + 2];
    size_t len; /**< bytes collected (including FCS while collecting) */
    uint8_t byte; /**< partial byte being assembled */
    uint8_t nbits; /**< bits in partial byte */
    uint8_t pat; /**< last 8 decoded bits, newest in bit 7 */
    bool collecting; /**< inside a frame (after an opening flag) */
    bool frame_ready; /**< buf holds a finished frame; cleared on the next bit */
    uint32_t aborts; /**< frames aborted by 7+ ones */
    uint32_t overlong; /**< frames dropped for exceeding the buffer */
} HdlcRx;

void hdlc_rx_init(HdlcRx* h);

/**
 * Feed one NRZI-decoded data bit.
 * @return HdlcRxFrame when a closing flag ends a byte-aligned frame of at least
 *         AX25_MIN_FRAME + 2 bytes. Read h->buf / h->len before the next call.
 */
HdlcRxResult hdlc_rx_bit(HdlcRx* h, bool bit);

/** True if the last completed frame's FCS matches. */
bool hdlc_rx_fcs_ok(const HdlcRx* h);
