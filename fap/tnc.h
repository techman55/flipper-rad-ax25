/**
 * @file tnc.h
 * Shared state for the KISS TNC app.
 *
 * Threads:
 *   - GUI (app main thread): draws, handles buttons.
 *   - USB worker (usb_kiss.c): CDC ch1 reader/writer, KISS decode -> tnc_control_on_kiss().
 *   - Radio worker (tnc_worker.c): sole owner of the CC1101. RX/CSMA/TX state machine,
 *     RSSI sampling + squelch gate, config changes, heartbeat.
 *   - Demod (tnc_demod.c): drains captured edges -> modem -> frames -> USB.
 *   - Async RX ISR: packs edges into `edges` stream buffer (nothing else).
 */
#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <notification/notification_messages.h>

#include "modem/modem.h"
#include "radio.h"
#include "settings.h"
#include "usb_kiss.h"

#define TNC_APP_VERSION   "1.0.0"
#define TNC_TXQ_DEPTH     8u
#define TNC_LAST_FRAMES   4u
#define TNC_LINE_LEN      48u
#define TNC_EDGE_BUF      2048u /**< edges (uint32 each) buffered between ISR and demod */
#define TNC_BUSY_TIMEOUT  10000u /**< ms of CSMA before giving up */
#define TNC_HEARTBEAT_MS  5000u
#define TNC_RSSI_PERIOD   5u /**< ms between RSSI samples in RX */
#define TNC_TX_BITS_CAP   (12u * 1024u * 8u) /**< line-bit buffer for one burst (12 KiB) */
#define TNC_FREQ_STEP     25000u

typedef enum {
    TncStateInit,
    TncStateRx,
    TncStateCsma,
    TncStateTx,
    TncStateError,
} TncState;

const char* tnc_state_name(TncState state);

/** A frame waiting to be sent. */
typedef struct {
    uint16_t len;
    bool has_seq; /**< came in via ACKMODE */
    uint16_t seq;
    uint8_t data[AX25_MAX_FRAME];
} TncTxItem;

/** Bits in TncRadioCmd.changed */
#define TNC_CH_FREQ   (1u << 0)
#define TNC_CH_MODE   (1u << 1)
#define TNC_CH_PWR    (1u << 2)
#define TNC_CH_RADIO  (1u << 3)
#define TNC_CH_SQL    (1u << 4)
#define TNC_CH_DEBUG  (1u << 5)
#define TNC_CH_CALL   (1u << 6)
#define TNC_CH_TXONCE (1u << 7)
#define TNC_CH_KISS   (1u << 8) /**< TXDELAY/P/SLOTTIME/TXTAIL: store only */

typedef enum {
    TncRadioCmdApply, /**< apply `settings` (fields flagged in `changed`) */
    TncRadioCmdGet, /**< answer GET (queued behind an Apply so replies stay in order) */
} TncRadioCmdType;

typedef struct {
    TncRadioCmdType type;
    uint32_t changed;
    bool reply; /**< send OK/ERR over USB when done (false for GUI changes) */
    uint32_t txonce_hz;
    TncSettings settings;
} TncRadioCmd;

typedef struct Tnc {
    FuriMutex* mutex; /**< guards settings, counters, last[], status text */
    TncSettings settings;

    /* live status (written by one thread, read by others) */
    volatile TncState state;
    volatile float rssi;
    volatile bool running;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t bad_count;
    volatile uint32_t edge_overflow;
    uint32_t txq_full;
    uint32_t turnaround_us;
    uint32_t last_tx_ms;
    size_t heap_at_start;
    const char* radio_name;
    bool radio_ok;

    char last[TNC_LAST_FRAMES][TNC_LINE_LEN];
    uint8_t last_count;
    char banner[32]; /**< transient message on the GUI */
    uint32_t banner_until;

    /* radio worker */
    FuriThread* radio_thread;
    FuriMessageQueue* cmd_q;
    FuriMessageQueue* tx_q;

    /* demod */
    FuriThread* demod_thread;
    FuriStreamBuffer* edges;
    volatile bool rx_gate; /**< ISR forwards edges only while true (squelch open) */
    volatile uint32_t rx_gen; /**< bumped to make the demod reset (squelch close, retune, mode) */
    volatile uint8_t rx_mode;

    /* USB */
    UsbKiss* usb;

    /* GUI */
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_q;
    NotificationApp* notifications;
    bool freq_pending;
    uint32_t pending_freq;
} Tnc;

/* tnc_worker.c */
int32_t tnc_radio_worker(void* ctx);
/** Queue a frame for TX. Returns false (and reports ERR) if invalid or the queue is full. */
bool tnc_enqueue_tx(Tnc* tnc, const uint8_t* frame, size_t len, bool has_seq, uint16_t seq);

/* tnc_demod.c */
int32_t tnc_demod_worker(void* ctx);
void tnc_rx_capture_isr(bool level, uint32_t duration, void* ctx);

/* tnc_control.c */
void tnc_control_on_kiss(void* ctx, const uint8_t* frame, size_t len);
/** Send the full "OK FREQ=... HEAP=..." status line. */
void tnc_control_report_get(Tnc* tnc);
/** Send a SetHardware status line to the host (printf-style). */
void tnc_report(Tnc* tnc, const char* fmt, ...) _ATTRIBUTE((__format__(__printf__, 2, 3)));
/** Set the GUI banner for a few seconds. */
void tnc_banner(Tnc* tnc, const char* text);
