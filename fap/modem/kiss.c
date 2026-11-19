#include "kiss.h"

#include <string.h>

static bool put(uint8_t* out, size_t cap, size_t* n, uint8_t b) {
    if(*n >= cap) return false;
    out[(*n)++] = b;
    return true;
}

static bool put_escaped(uint8_t* out, size_t cap, size_t* n, uint8_t b) {
    if(b == KISS_FEND) return put(out, cap, n, KISS_FESC) && put(out, cap, n, KISS_TFEND);
    if(b == KISS_FESC) return put(out, cap, n, KISS_FESC) && put(out, cap, n, KISS_TFESC);
    return put(out, cap, n, b);
}

size_t kiss_encode(uint8_t type, const uint8_t* data, size_t len, uint8_t* out, size_t cap) {
    size_t n = 0;
    if(!put(out, cap, &n, KISS_FEND)) return 0;
    if(!put_escaped(out, cap, &n, type)) return 0;
    for(size_t i = 0; i < len; i++) {
        if(!put_escaped(out, cap, &n, data[i])) return 0;
    }
    if(!put(out, cap, &n, KISS_FEND)) return 0;
    return n;
}

void kiss_decoder_init(KissDecoder* d) {
    memset(d, 0, sizeof(*d));
}

bool kiss_decoder_byte(KissDecoder* d, uint8_t byte) {
    if(d->ready) {
        // The caller has read the previous frame.
        d->ready = false;
        d->len = 0;
    }

    if(byte == KISS_FEND) {
        bool done = d->in_frame && d->len > 0 && !d->overflow;
        if(d->in_frame && d->overflow) d->dropped++;
        // A FEND both closes the current frame and opens the next one.
        d->in_frame = true;
        d->escape = false;
        d->overflow = false;
        if(done) {
            d->ready = true;
            return true;
        }
        d->len = 0;
        return false;
    }

    if(!d->in_frame) return false;

    if(d->escape) {
        d->escape = false;
        if(byte == KISS_TFEND) {
            byte = KISS_FEND;
        } else if(byte == KISS_TFESC) {
            byte = KISS_FESC;
        }
        // Anything else after FESC is a protocol error; keep the byte (lenient, like most TNCs).
    } else if(byte == KISS_FESC) {
        d->escape = true;
        return false;
    }

    if(d->len < sizeof(d->buf)) {
        d->buf[d->len++] = byte;
    } else {
        d->overflow = true;
    }
    return false;
}
