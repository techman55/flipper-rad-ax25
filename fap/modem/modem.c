#include "modem.h"

#include <string.h>

const char* modem_mode_name(ModemMode mode) {
    switch(mode) {
    case ModemModeFsk9600:
        return "FSK9600";
    case ModemModeAfsk1200:
        return "AFSK1200";
    case ModemModeFsk1200:
        return "FSK1200";
    default:
        return "?";
    }
}

uint32_t modem_mode_baud(ModemMode mode) {
    return mode == ModemModeFsk9600 ? 9600u : 1200u;
}

bool modem_mode_scrambled(ModemMode mode) {
    return mode == ModemModeFsk9600;
}

/* ---------- TX ---------- */

unsigned modem_flags_for_ms(ModemMode mode, uint32_t ms) {
    uint32_t bits = (uint32_t)(((uint64_t)ms * modem_mode_baud(mode) + 999u) / 1000u);
    unsigned flags = (bits + 7u) / 8u;
    return flags ? flags : 1u;
}

size_t modem_frame_bits_max(size_t len) {
    size_t data_bits = (len + 2u) * 8u; // + FCS
    return data_bits + data_bits / 5u + 1u + 8u; // stuffing worst case + separator flag
}

size_t modem_build_burst(
    ModemMode mode,
    const ModemFrame* frames,
    size_t nframes,
    uint32_t txdelay_ms,
    uint32_t txtail_ms,
    uint8_t* out,
    size_t out_cap_bits) {
    // Pass 1: raw HDLC bits, written straight into `out`.
    BitBuf b;
    bitbuf_init(&b, out, out_cap_bits);
    hdlc_put_flags(&b, modem_flags_for_ms(mode, txdelay_ms));
    for(size_t i = 0; i < nframes; i++) {
        if(i > 0) hdlc_put_flags(&b, 1);
        hdlc_put_frame(&b, frames[i].data, frames[i].len);
    }
    hdlc_put_flags(&b, modem_flags_for_ms(mode, txtail_ms));
    if(b.overflow) return 0;

    // Pass 2, in place: NRZI, then scramble for G3RUH.
    NrziEnc nrzi = {.level = false};
    G3ruh scr;
    g3ruh_init(&scr, 0);
    bool scramble = modem_mode_scrambled(mode);
    for(size_t i = 0; i < b.nbits; i++) {
        bool line = nrzi_encode(&nrzi, bitbuf_get(out, i));
        if(scramble) line = g3ruh_scramble(&scr, line);
        bitbuf_set(out, i, line);
    }
    return b.nbits;
}

void modem_tx_init(ModemTx* tx, ModemMode mode, const uint8_t* bits, size_t nbits, bool start_high) {
    tx->mode = mode;
    if(mode == ModemModeAfsk1200) {
        afsk_tx_init(&tx->u.afsk, bits, nbits); // always starts high
    } else {
        bool invert = start_high && nbits > 0 && !bitbuf_get(bits, 0);
        fsk_tx_init(&tx->u.fsk, bits, nbits, modem_mode_baud(mode), invert);
    }
}

bool modem_tx_next(ModemTx* tx, bool* level, uint32_t* duration_us) {
    if(tx->mode == ModemModeAfsk1200) {
        return afsk_tx_next(&tx->u.afsk, level, duration_us);
    }
    return fsk_tx_next(&tx->u.fsk, level, duration_us);
}

uint32_t modem_burst_duration_us(ModemMode mode, size_t nbits) {
    return bit_start_us(nbits, modem_mode_baud(mode));
}

/* ---------- RX ---------- */

void modem_rx_init(ModemRx* rx, ModemMode mode, ModemFrameCallback cb, void* ctx) {
    memset(rx, 0, sizeof(*rx));
    rx->mode = mode;
    rx->cb = cb;
    rx->cb_ctx = ctx;
    fsk_rx_init(&rx->fsk, modem_mode_baud(mode));
    afsk_rx_init(&rx->afsk);
    modem_rx_reset(rx);
}

void modem_rx_reset(ModemRx* rx) {
    clock_recovery_reset(&rx->fsk.cr);
    afsk_rx_reset(&rx->afsk);
    g3ruh_init(&rx->descrambler, 0);
    rx->nrzi.prev = false;
    hdlc_rx_init(&rx->hdlc);
}

static void modem_rx_line_bit(ModemRx* rx, bool line) {
    if(modem_mode_scrambled(rx->mode)) line = g3ruh_descramble(&rx->descrambler, line);
    bool bit = nrzi_decode(&rx->nrzi, line);
    if(hdlc_rx_bit(&rx->hdlc, bit) == HdlcRxFrame) {
        bool ok = hdlc_rx_fcs_ok(&rx->hdlc);
        if(ok) {
            rx->frames_ok++;
        } else {
            rx->frames_bad++;
        }
        if(rx->cb) rx->cb(rx->cb_ctx, rx->hdlc.buf, rx->hdlc.len - 2u, ok);
    }
}

void modem_rx_edge(ModemRx* rx, bool level, uint32_t duration_us) {
    uint8_t bits[CLOCK_MAX_BITS_PER_RUN];
    size_t n;
    if(rx->mode == ModemModeAfsk1200) {
        n = afsk_rx_halfperiod(&rx->afsk, level, duration_us, bits);
    } else {
        n = fsk_rx_run(&rx->fsk, level, duration_us, bits);
    }
    for(size_t i = 0; i < n; i++) {
        modem_rx_line_bit(rx, bits[i]);
    }
}
