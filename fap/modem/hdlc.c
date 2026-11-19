#include "hdlc.h"

#include <string.h>

uint16_t hdlc_fcs(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            if(crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0x8408u);
            } else {
                crc >>= 1;
            }
        }
    }
    return (uint16_t)(crc ^ 0xFFFFu);
}

/* ---------- TX ---------- */

void hdlc_put_flags(BitBuf* out, unsigned count) {
    for(unsigned i = 0; i < count; i++) {
        for(int b = 0; b < 8; b++) {
            bitbuf_put(out, (HDLC_FLAG >> b) & 1u);
        }
    }
}

static void put_stuffed_byte(BitBuf* out, uint8_t byte, unsigned* ones) {
    for(int b = 0; b < 8; b++) {
        bool bit = (byte >> b) & 1u;
        bitbuf_put(out, bit);
        if(bit) {
            if(++(*ones) == 5) {
                bitbuf_put(out, 0); // stuffed zero
                *ones = 0;
            }
        } else {
            *ones = 0;
        }
    }
}

void hdlc_put_frame(BitBuf* out, const uint8_t* frame, size_t len) {
    unsigned ones = 0; // flags never count toward stuffing
    for(size_t i = 0; i < len; i++) {
        put_stuffed_byte(out, frame[i], &ones);
    }
    uint16_t fcs = hdlc_fcs(frame, len);
    put_stuffed_byte(out, (uint8_t)(fcs & 0xFFu), &ones);
    put_stuffed_byte(out, (uint8_t)(fcs >> 8), &ones);
}

/* ---------- RX ---------- */

void hdlc_rx_init(HdlcRx* h) {
    memset(h, 0, sizeof(*h));
}

HdlcRxResult hdlc_rx_bit(HdlcRx* h, bool bit) {
    if(h->frame_ready) {
        // The caller has had its chance to read the previous frame.
        h->frame_ready = false;
        h->len = 0;
    }
    h->pat = (uint8_t)((h->pat >> 1) | (bit ? 0x80u : 0u));

    if(h->pat == HDLC_FLAG) {
        // By the time the final 0 of a flag arrives, its first 7 bits (0111111)
        // have been shifted into the partial byte. A byte-aligned frame
        // therefore has exactly 7 bits pending here.
        HdlcRxResult res = HdlcRxNone;
        if(h->collecting && h->nbits == 7 && h->len >= AX25_MIN_FRAME + 2) {
            res = HdlcRxFrame;
        }
        // Every flag (re)starts collection. A finished frame stays in buf until the next bit.
        if(res == HdlcRxFrame) {
            h->frame_ready = true;
        } else {
            h->len = 0;
        }
        h->collecting = true;
        h->nbits = 0;
        h->byte = 0;
        return res;
    }

    if(h->pat == 0xFEu) {
        // Seven ones in a row: abort / idle. Drop whatever we had.
        if(h->collecting && h->len > 0) h->aborts++;
        h->collecting = false;
        h->len = 0;
        return HdlcRxNone;
    }

    if(!h->collecting) return HdlcRxNone;

    // Stuffed zero: this bit is 0 and the five before it were 1 (x111110 pattern).
    if((h->pat & 0xFCu) == 0x7Cu) return HdlcRxNone;

    h->byte = (uint8_t)((h->byte >> 1) | (bit ? 0x80u : 0u));
    if(++h->nbits == 8) {
        if(h->len < sizeof(h->buf)) {
            h->buf[h->len++] = h->byte;
        } else {
            h->overlong++;
            h->collecting = false;
            h->len = 0;
        }
        h->nbits = 0;
        h->byte = 0;
    }
    return HdlcRxNone;
}

bool hdlc_rx_fcs_ok(const HdlcRx* h) {
    if(h->len < 3) return false;
    uint16_t want = (uint16_t)(h->buf[h->len - 2] | (h->buf[h->len - 1] << 8));
    return hdlc_fcs(h->buf, h->len - 2) == want;
}
