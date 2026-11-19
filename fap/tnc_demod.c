/**
 * Async RX ISR + demod thread.
 *
 * The ISR only packs (level, duration) into a uint32 and pushes it into a
 * stream buffer, and only while the squelch gate is open (the radio worker
 * opens it when RSSI >= SQL). Everything else happens here, in thread context.
 */
#include <string.h>

#include "modem/ax25.h"
#include "modem/kiss.h"
#include "tnc.h"

#define TAG "KissTncDemod"

#define EDGE_LEVEL_BIT 0x80000000u
#define EDGE_DUR_MASK  0x7FFFFFFFu

void tnc_rx_capture_isr(bool level, uint32_t duration, void* ctx) {
    Tnc* tnc = ctx;
    if(!tnc->rx_gate) return; // squelch closed: drop noise edges at the source
    uint32_t packed = (duration & EDGE_DUR_MASK) | (level ? EDGE_LEVEL_BIT : 0u);
    // Never write a partial word: that would misalign every edge after it.
    if(furi_stream_buffer_spaces_available(tnc->edges) < sizeof(packed)) {
        tnc->edge_overflow++;
        return;
    }
    furi_stream_buffer_send(tnc->edges, &packed, sizeof(packed), 0);
}

static void on_frame(void* ctx, const uint8_t* frame, size_t len, bool fcs_ok) {
    Tnc* tnc = ctx;

    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    if(fcs_ok) {
        tnc->rx_count++;
    } else {
        tnc->bad_count++;
    }
    bool debug = tnc->settings.debug;
    uint32_t freq = tnc->settings.freq;
    furi_mutex_release(tnc->mutex);

    if(!fcs_ok && !debug) return;

    char status[96];
    int n = snprintf(
        status,
        sizeof(status),
        "RX RSSI=%d FREQ=%lu MODE=%u FCS=%s T=%lu",
        (int)tnc->rssi,
        (unsigned long)freq,
        tnc->rx_mode,
        fcs_ok ? "OK" : "BAD",
        (unsigned long)furi_get_tick());
    if(n > 0) {
        usb_kiss_send_pair(
            tnc->usb,
            KISS_CMD_SETHARDWARE,
            (const uint8_t*)status,
            (size_t)n,
            KISS_CMD_DATA,
            frame,
            len);
    }

    if(fcs_ok) {
        char line[TNC_LINE_LEN];
        ax25_format(frame, len, line, sizeof(line));
        furi_mutex_acquire(tnc->mutex, FuriWaitForever);
        // Newest first.
        memmove(tnc->last[1], tnc->last[0], (TNC_LAST_FRAMES - 1) * TNC_LINE_LEN);
        strlcpy(tnc->last[0], line, TNC_LINE_LEN);
        if(tnc->last_count < TNC_LAST_FRAMES) tnc->last_count++;
        furi_mutex_release(tnc->mutex);
        notification_message(tnc->notifications, &sequence_blink_green_10);
        if(tnc->view_port) view_port_update(tnc->view_port);
    }
}

int32_t tnc_demod_worker(void* ctx) {
    Tnc* tnc = ctx;
    ModemRx* rx = malloc(sizeof(ModemRx));
    uint32_t gen = tnc->rx_gen - 1u; // force an init on the first pass
    uint32_t edges[64];

    while(tnc->running) {
        size_t got = furi_stream_buffer_receive(tnc->edges, edges, sizeof(edges), 50);

        uint32_t g = tnc->rx_gen;
        if(g != gen) {
            // Squelch closed, retune, mode change, or we just transmitted.
            gen = g;
            modem_rx_init(rx, (ModemMode)tnc->rx_mode, on_frame, tnc);
        }

        for(size_t i = 0; i < got / sizeof(uint32_t); i++) {
            modem_rx_edge(rx, (edges[i] & EDGE_LEVEL_BIT) != 0, edges[i] & EDGE_DUR_MASK);
        }
    }

    free(rx);
    return 0;
}
