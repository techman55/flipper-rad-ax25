/**
 * KISS TNC for Flipper Zero: app entry, thread setup, GUI.
 *
 * Listens by default. Host frames (KISS over USB CDC channel 1) are sent on
 * the configured frequency/mode, then the radio goes straight back to RX.
 * Never transmits on its own: only on host request, or the OK-button test
 * beacon once a real callsign is configured.
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <string.h>

#include "modem/ax25.h"
#include "tnc.h"

#define TAG "KissTnc"

#define BEACON_DEST "TEST"
#define BEACON_TEXT "flipper kiss tnc test"

/* ---------- GUI ---------- */

static void draw_freq(char* out, size_t cap, uint32_t hz) {
    snprintf(
        out,
        cap,
        "%lu.%03lu MHz",
        (unsigned long)(hz / 1000000u),
        (unsigned long)((hz / 1000u) % 1000u));
}

static void draw_callback(Canvas* canvas, void* ctx) {
    Tnc* tnc = ctx;
    char buf[48];
    char freq[20];

    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    TncSettings s = tnc->settings;
    uint32_t rx = tnc->rx_count, tx = tnc->tx_count, bad = tnc->bad_count;
    char last[TNC_LAST_FRAMES][TNC_LINE_LEN];
    memcpy(last, tnc->last, sizeof(last));
    uint8_t last_count = tnc->last_count;
    char banner[sizeof(tnc->banner)];
    bool show_banner = (int32_t)(tnc->banner_until - furi_get_tick()) > 0;
    strlcpy(banner, tnc->banner, sizeof(banner));
    const char* radio = tnc->radio_name ? tnc->radio_name : "...";
    furi_mutex_release(tnc->mutex);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    // Line 1: title + state
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 9, "KISS TNC");
    canvas_draw_str_aligned(canvas, 127, 0, AlignRight, AlignTop, tnc_state_name(tnc->state));

    canvas_set_font(canvas, FontSecondary);
    // Line 2: mode + frequency
    draw_freq(freq, sizeof(freq), s.freq);
    snprintf(buf, sizeof(buf), "%s  %s", modem_mode_name((ModemMode)s.mode), freq);
    canvas_draw_str(canvas, 0, 19, buf);
    // Line 3: RSSI, USB, radio
    snprintf(
        buf,
        sizeof(buf),
        "RSSI %d  USB:%s  %s",
        (int)tnc->rssi,
        usb_kiss_host_connected(tnc->usb) ? "yes" : "no",
        radio);
    canvas_draw_str(canvas, 0, 28, buf);
    // Line 4: counters
    snprintf(
        buf,
        sizeof(buf),
        "RX %lu  TX %lu  BAD %lu",
        (unsigned long)rx,
        (unsigned long)tx,
        (unsigned long)bad);
    canvas_draw_str(canvas, 0, 37, buf);
    canvas_draw_line(canvas, 0, 39, 127, 39);

    // Lines 5-8: last frames, newest first
    if(last_count == 0) {
        canvas_draw_str(canvas, 0, 49, "Listening...");
    }
    for(uint8_t i = 0; i < last_count; i++) {
        canvas_draw_str(canvas, 0, (int32_t)(47 + i * 8), last[i]);
    }

    if(tnc->freq_pending) {
        draw_freq(freq, sizeof(freq), tnc->pending_freq);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 6, 14, 116, 36);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_frame(canvas, 6, 14, 116, 36);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, freq);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignTop, "Up/Down step 25k");
        canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignTop, "OK tune  Back cancel");
    } else if(show_banner) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_box(canvas, 0, 52, 128, 12);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, banner);
        canvas_set_color(canvas, ColorBlack);
    }
}

static void input_callback(InputEvent* event, void* ctx) {
    Tnc* tnc = ctx;
    furi_message_queue_put(tnc->input_q, event, 0);
}

/* ---------- actions ---------- */

static void apply_from_gui(Tnc* tnc, uint32_t changed, const TncSettings* s) {
    TncRadioCmd cmd = {.type = TncRadioCmdApply, .changed = changed, .reply = false};
    cmd.settings = *s;
    if(furi_message_queue_put(tnc->cmd_q, &cmd, 100) != FuriStatusOk) {
        tnc_banner(tnc, "Busy, try again");
    }
}

static void send_beacon(Tnc* tnc) {
    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    char call[sizeof(tnc->settings.call)];
    strlcpy(call, tnc->settings.call, sizeof(call));
    furi_mutex_release(tnc->mutex);

    if(tnc_call_is_default(call)) {
        // Station ID matters: never key up as N0CALL.
        tnc_banner(tnc, "Set CALL first (no TX)");
        return;
    }
    uint8_t frame[64];
    size_t len = ax25_build_ui(
        call,
        BEACON_DEST,
        NULL,
        (const uint8_t*)BEACON_TEXT,
        strlen(BEACON_TEXT),
        frame,
        sizeof(frame));
    if(len && tnc_enqueue_tx(tnc, frame, len, false, 0)) {
        tnc_banner(tnc, "Test beacon queued");
    } else {
        tnc_banner(tnc, "Beacon failed");
    }
}

static bool handle_input(Tnc* tnc, const InputEvent* ev) {
    bool press = ev->type == InputTypeShort;
    bool step = ev->type == InputTypeShort || ev->type == InputTypeRepeat;

    if(tnc->freq_pending) {
        if(step && (ev->key == InputKeyUp || ev->key == InputKeyDown)) {
            uint32_t f = ev->key == InputKeyUp ? tnc->pending_freq + TNC_FREQ_STEP :
                                                 tnc->pending_freq - TNC_FREQ_STEP;
            if(tnc_freq_in_range(f)) tnc->pending_freq = f;
        } else if(press && ev->key == InputKeyOk) {
            TncSettings s = {0};
            s.freq = tnc->pending_freq;
            tnc->freq_pending = false;
            apply_from_gui(tnc, TNC_CH_FREQ, &s);
        } else if(press && ev->key == InputKeyBack) {
            tnc->freq_pending = false;
        }
        return true;
    }

    if(press && ev->key == InputKeyBack) return false; // exit

    if(step && (ev->key == InputKeyUp || ev->key == InputKeyDown)) {
        furi_mutex_acquire(tnc->mutex, FuriWaitForever);
        uint32_t f = tnc->settings.freq;
        furi_mutex_release(tnc->mutex);
        uint32_t nf = ev->key == InputKeyUp ? f + TNC_FREQ_STEP : f - TNC_FREQ_STEP;
        tnc->pending_freq = tnc_freq_in_range(nf) ? nf : f;
        tnc->freq_pending = true;
    } else if(press && (ev->key == InputKeyLeft || ev->key == InputKeyRight)) {
        furi_mutex_acquire(tnc->mutex, FuriWaitForever);
        uint8_t m = tnc->settings.mode;
        furi_mutex_release(tnc->mutex);
        TncSettings s = {0};
        s.mode = ev->key == InputKeyRight ? (uint8_t)((m + 1u) % ModemModeCount) :
                                            (uint8_t)((m + ModemModeCount - 1u) % ModemModeCount);
        apply_from_gui(tnc, TNC_CH_MODE, &s);
    } else if(press && ev->key == InputKeyOk) {
        send_beacon(tnc);
    }
    return true;
}

/* ---------- entry ---------- */

int32_t kiss_tnc_app(void* p) {
    UNUSED(p);
    Tnc* tnc = malloc(sizeof(Tnc));
    memset(tnc, 0, sizeof(Tnc));
    tnc->heap_at_start = memmgr_get_free_heap();
    tnc->state = TncStateInit;
    tnc->rssi = -130.0f;

    tnc->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    tnc_settings_defaults(&tnc->settings);
    tnc_settings_load(&tnc->settings);

    tnc->cmd_q = furi_message_queue_alloc(4, sizeof(TncRadioCmd));
    tnc->tx_q = furi_message_queue_alloc(TNC_TXQ_DEPTH, sizeof(TncTxItem));
    tnc->edges = furi_stream_buffer_alloc(TNC_EDGE_BUF * sizeof(uint32_t), sizeof(uint32_t));
    tnc->input_q = furi_message_queue_alloc(8, sizeof(InputEvent));
    tnc->notifications = furi_record_open(RECORD_NOTIFICATION);

    tnc->view_port = view_port_alloc();
    view_port_draw_callback_set(tnc->view_port, draw_callback, tnc);
    view_port_input_callback_set(tnc->view_port, input_callback, tnc);
    tnc->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(tnc->gui, tnc->view_port, GuiLayerFullscreen);

    tnc->running = true;
    tnc->usb = usb_kiss_start(tnc_control_on_kiss, tnc);
    if(!tnc->usb) tnc_banner(tnc, "USB busy: no host link");

    // Charging noise hurts RX; same as the stock Sub-GHz CLI.
    furi_hal_power_suppress_charge_enter();

    tnc->demod_thread = furi_thread_alloc_ex("KissTncDemod", 3 * 1024, tnc_demod_worker, tnc);
    furi_thread_set_priority(tnc->demod_thread, FuriThreadPriorityHigh);
    furi_thread_start(tnc->demod_thread);

    tnc->radio_thread = furi_thread_alloc_ex("KissTncRadio", 4 * 1024, tnc_radio_worker, tnc);
    furi_thread_set_priority(tnc->radio_thread, FuriThreadPriorityHigh);
    furi_thread_start(tnc->radio_thread);

    FURI_LOG_I(TAG, "Started, heap %u", (unsigned)tnc->heap_at_start);

    // GUI loop. Times out every 250 ms to refresh RSSI on screen.
    while(1) {
        InputEvent ev;
        if(furi_message_queue_get(tnc->input_q, &ev, 250) == FuriStatusOk) {
            if(!handle_input(tnc, &ev)) break;
        }
        view_port_update(tnc->view_port);
    }

    // Detach the screen first: the draw callback reads state we are about to free.
    gui_remove_view_port(tnc->gui, tnc->view_port);

    // Teardown order: radio (stops async RX/TX, idles + ends the device) -> demod -> USB
    // (restores the previous USB config).
    tnc->running = false;
    furi_thread_join(tnc->radio_thread);
    furi_thread_free(tnc->radio_thread);
    furi_thread_join(tnc->demod_thread);
    furi_thread_free(tnc->demod_thread);
    UsbKiss* usb = tnc->usb;
    tnc->usb = NULL;
    usb_kiss_stop(usb);
    furi_hal_power_suppress_charge_exit();

    ViewPort* vp = tnc->view_port;
    tnc->view_port = NULL;
    view_port_free(vp);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    furi_message_queue_free(tnc->input_q);
    furi_stream_buffer_free(tnc->edges);
    furi_message_queue_free(tnc->tx_q);
    furi_message_queue_free(tnc->cmd_q);
    furi_mutex_free(tnc->mutex);
    free(tnc);
    return 0;
}
