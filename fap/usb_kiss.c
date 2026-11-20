#include "usb_kiss.h"

#include <cli/cli_vcp.h>
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb_cdc.h>

#include "modem/kiss.h"

#define TAG "KissTncUsb"

#define USB_PKT        CDC_DATA_SZ
#define OUT_BUF_SIZE   4096u
#define ENCODE_SCRATCH 1400u /**< room for two escaped max-size frames */

typedef enum {
    EvtStop = (1 << 0),
    EvtRx = (1 << 1),
    EvtTxDone = (1 << 2),
    EvtTxData = (1 << 3),
    EvtState = (1 << 4),
} UsbEvt;
#define EVT_ALL (EvtStop | EvtRx | EvtTxDone | EvtTxData | EvtState)

struct UsbKiss {
    FuriThread* thread;
    FuriHalUsbInterface* prev_config;
    CliVcp* cli_vcp;

    FuriStreamBuffer* out; /**< bytes waiting to go to the host */
    FuriMutex* out_mutex; /**< serialises producers (stream buffer is single-writer) */
    uint8_t scratch[ENCODE_SCRATCH];

    KissDecoder decoder;
    UsbKissFrameCallback cb;
    void* cb_ctx;

    volatile bool configured;
    volatile bool dtr;
    volatile uint32_t dropped;

    bool transmitting; /**< a CDC IN transfer is in flight (worker thread only) */
    size_t last_tx_len;
};

/* --- CDC callbacks (USB interrupt context: only signal the worker) --- */

static void cdc_tx_done(void* ctx) {
    UsbKiss* usb = ctx;
    furi_thread_flags_set(furi_thread_get_id(usb->thread), EvtTxDone);
}

static void cdc_rx(void* ctx) {
    UsbKiss* usb = ctx;
    furi_thread_flags_set(furi_thread_get_id(usb->thread), EvtRx);
}

static void cdc_state(void* ctx, CdcState state) {
    UsbKiss* usb = ctx;
    usb->configured = (state == CdcStateConnected);
    if(!usb->configured) usb->dtr = false;
    furi_thread_flags_set(furi_thread_get_id(usb->thread), EvtState);
}

static void cdc_ctrl_line(void* ctx, CdcCtrlLine lines) {
    UsbKiss* usb = ctx;
    usb->dtr = (lines & CdcCtrlLineDTR) != 0;
    furi_thread_flags_set(furi_thread_get_id(usb->thread), EvtState);
}

static void cdc_config(void* ctx, struct usb_cdc_line_coding* config) {
    // Baud rate is meaningless on USB CDC; ignore.
    UNUSED(ctx);
    UNUSED(config);
}

static const CdcCallbacks cdc_callbacks = {
    .tx_ep_callback = cdc_tx_done,
    .rx_ep_callback = cdc_rx,
    .state_callback = cdc_state,
    .ctrl_line_callback = cdc_ctrl_line,
    .config_callback = cdc_config,
};

/* --- worker --- */

static void try_send(UsbKiss* usb) {
    if(usb->transmitting) return;
    uint8_t pkt[USB_PKT];
    size_t n = furi_stream_buffer_receive(usb->out, pkt, sizeof(pkt), 0);
    // A transfer that ended on a full packet needs a zero-length packet to
    // terminate it, or the host may sit on the data.
    if(n > 0 || usb->last_tx_len == USB_PKT) {
        usb->transmitting = true;
        furi_hal_cdc_send(USB_KISS_CHANNEL, pkt, (uint16_t)n);
    }
    usb->last_tx_len = n;
}

static int32_t usb_kiss_worker(void* ctx) {
    UsbKiss* usb = ctx;
    uint8_t buf[USB_PKT];

    while(1) {
        uint32_t evt = furi_thread_flags_wait(EVT_ALL, FuriFlagWaitAny, 200);
        if(evt & FuriFlagError) {
            if(evt != (uint32_t)FuriFlagErrorTimeout) continue;
            evt = 0; // periodic wake: retry any stalled output
        }
        if(evt & EvtStop) break;

        if(evt & EvtRx) {
            int32_t n = furi_hal_cdc_receive(USB_KISS_CHANNEL, buf, sizeof(buf));
            for(int32_t i = 0; i < n; i++) {
                uint32_t dropped = usb->decoder.dropped;
                if(kiss_decoder_byte(&usb->decoder, buf[i])) {
                    usb->cb(usb->cb_ctx, usb->decoder.buf, usb->decoder.len);
                } else if(usb->decoder.dropped != dropped) {
                    usb->cb(usb->cb_ctx, NULL, 0); // oversize frame discarded
                }
            }
        }

        if(evt & EvtTxDone) usb->transmitting = false;

        if(evt & EvtState) {
            if(!usb->configured) {
                // Cable pulled: nothing will ever acknowledge the in-flight packet.
                usb->transmitting = false;
                usb->last_tx_len = 0;
                furi_stream_buffer_reset(usb->out);
            }
        }

        try_send(usb);
    }
    return 0;
}

/* --- public --- */

UsbKiss* usb_kiss_start(UsbKissFrameCallback cb, void* ctx) {
    UsbKiss* usb = malloc(sizeof(UsbKiss));
    memset(usb, 0, sizeof(UsbKiss));
    usb->cb = cb;
    usb->cb_ctx = ctx;
    kiss_decoder_init(&usb->decoder);
    usb->out = furi_stream_buffer_alloc(OUT_BUF_SIZE, 1);
    usb->out_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    usb->thread = furi_thread_alloc_ex("KissTncUsb", 3 * 1024, usb_kiss_worker, usb);

    usb->prev_config = furi_hal_usb_get_config();
    usb->cli_vcp = furi_record_open(RECORD_CLI_VCP);

    furi_hal_usb_unlock();
    if(!furi_hal_usb_set_config(&usb_cdc_dual, NULL)) {
        FURI_LOG_E(TAG, "Could not switch USB to dual CDC");
        furi_record_close(RECORD_CLI_VCP);
        furi_thread_free(usb->thread);
        furi_mutex_free(usb->out_mutex);
        furi_stream_buffer_free(usb->out);
        free(usb);
        return NULL;
    }
    cli_vcp_enable(usb->cli_vcp); // CLI keeps running on channel 0

    furi_thread_start(usb->thread);
    furi_hal_cdc_set_callbacks(USB_KISS_CHANNEL, (CdcCallbacks*)&cdc_callbacks, usb);
    usb->configured = true;
    usb->dtr = (furi_hal_cdc_get_ctrl_line_state(USB_KISS_CHANNEL) & CdcCtrlLineDTR) != 0;
    FURI_LOG_I(TAG, "USB dual CDC up, TNC on channel %d", USB_KISS_CHANNEL);
    return usb;
}

void usb_kiss_stop(UsbKiss* usb) {
    if(!usb) return;
    furi_hal_cdc_set_callbacks(USB_KISS_CHANNEL, NULL, NULL);
    furi_thread_flags_set(furi_thread_get_id(usb->thread), EvtStop);
    furi_thread_join(usb->thread);
    furi_thread_free(usb->thread);

    // Restore whatever USB mode was active before we started (normally the
    // single-CDC CLI). Same sequence the stock USB-UART bridge uses.
    cli_vcp_disable(usb->cli_vcp);
    furi_hal_usb_unlock();
    FuriHalUsbInterface* restore =
        (usb->prev_config && usb->prev_config != &usb_cdc_dual) ? usb->prev_config :
                                                                   &usb_cdc_single;
    if(!furi_hal_usb_set_config(restore, NULL)) {
        FURI_LOG_E(TAG, "USB restore failed, falling back to single CDC");
        furi_hal_usb_set_config(&usb_cdc_single, NULL);
    }
    cli_vcp_enable(usb->cli_vcp);
    furi_record_close(RECORD_CLI_VCP);

    furi_mutex_free(usb->out_mutex);
    furi_stream_buffer_free(usb->out);
    free(usb);
}

static bool queue_encoded(UsbKiss* usb, size_t n) {
    if(!usb->configured || furi_stream_buffer_spaces_available(usb->out) < n) {
        usb->dropped++;
        return false;
    }
    furi_stream_buffer_send(usb->out, usb->scratch, n, 0);
    return true;
}

bool usb_kiss_send(UsbKiss* usb, uint8_t type, const uint8_t* data, size_t len) {
    if(!usb) return false;
    furi_check(furi_mutex_acquire(usb->out_mutex, FuriWaitForever) == FuriStatusOk);
    size_t n = kiss_encode(type, data, len, usb->scratch, sizeof(usb->scratch));
    bool ok = n > 0 && queue_encoded(usb, n);
    furi_mutex_release(usb->out_mutex);
    if(ok) furi_thread_flags_set(furi_thread_get_id(usb->thread), EvtTxData);
    return ok;
}

bool usb_kiss_send_pair(
    UsbKiss* usb,
    uint8_t type_a,
    const uint8_t* a,
    size_t a_len,
    uint8_t type_b,
    const uint8_t* b,
    size_t b_len) {
    if(!usb) return false;
    furi_check(furi_mutex_acquire(usb->out_mutex, FuriWaitForever) == FuriStatusOk);
    size_t n1 = kiss_encode(type_a, a, a_len, usb->scratch, sizeof(usb->scratch));
    size_t n2 =
        n1 ? kiss_encode(type_b, b, b_len, usb->scratch + n1, sizeof(usb->scratch) - n1) : 0;
    bool ok = n1 && n2 && queue_encoded(usb, n1 + n2);
    furi_mutex_release(usb->out_mutex);
    if(ok) furi_thread_flags_set(furi_thread_get_id(usb->thread), EvtTxData);
    return ok;
}

bool usb_kiss_host_connected(UsbKiss* usb) {
    return usb && usb->configured && usb->dtr;
}

uint32_t usb_kiss_dropped(UsbKiss* usb) {
    return usb ? usb->dropped : 0;
}

uint32_t usb_kiss_rx_overflows(UsbKiss* usb) {
    return usb ? usb->decoder.dropped : 0;
}
