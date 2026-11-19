/**
 * @file kiss.h
 * KISS framing (FEND/FESC escaping) and a streaming decoder.
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KISS_FEND  0xC0u
#define KISS_FESC  0xDBu
#define KISS_TFEND 0xDCu
#define KISS_TFESC 0xDDu

/* Command nibble (low 4 bits of the type byte). Port is the high nibble. */
#define KISS_CMD_DATA        0x00u
#define KISS_CMD_TXDELAY     0x01u
#define KISS_CMD_P           0x02u
#define KISS_CMD_SLOTTIME    0x03u
#define KISS_CMD_TXTAIL      0x04u
#define KISS_CMD_FULLDUPLEX  0x05u
#define KISS_CMD_SETHARDWARE 0x06u
#define KISS_CMD_ACKMODE     0x0Cu
#define KISS_CMD_RETURN      0xFFu /**< whole type byte, not a nibble */

/** Largest unescaped KISS payload we accept: type + 2 seq bytes + max frame + slack. */
#define KISS_MAX_FRAME 400u

/**
 * Encode `type` + `data` as one KISS frame (FEND ... FEND) into `out`.
 * @return bytes written, or 0 if `cap` was too small.
 */
size_t kiss_encode(uint8_t type, const uint8_t* data, size_t len, uint8_t* out, size_t cap);

/** Worst-case encoded size for a payload of `len` bytes. */
static inline size_t kiss_encoded_max(size_t len) {
    return 2u + 2u * (len + 1u);
}

typedef struct {
    uint8_t buf[KISS_MAX_FRAME]; /**< buf[0] = type byte, then payload */
    size_t len;
    bool in_frame;
    bool escape;
    bool overflow;
    bool ready; /**< buf holds a finished frame; cleared on the next byte */
    uint32_t dropped; /**< frames dropped for overflow or bad escapes */
} KissDecoder;

void kiss_decoder_init(KissDecoder* d);

/**
 * Feed one byte. Returns true when a complete, non-empty frame is in
 * d->buf[0..d->len). Read it before feeding the next byte.
 */
bool kiss_decoder_byte(KissDecoder* d, uint8_t byte);
