/**
 * Radio worker: the only thread that talks to the CC1101.
 *
 *   INIT -> RX (continuous)
 *   RX --frame queued--> CSMA (p-persistent, RSSI vs SQL, SLOTTIME)
 *   CSMA --> TX (async: TXDELAY flags + frames + TXTAIL flags)
 *   TX --complete--> RX immediately, then TXDONE / ACKMODE acks to the host
 */
#include <furi_hal.h>
#include <string.h>

#include "modem/kiss.h"
#include "tnc.h"

#define TAG "KissTncWorker"

#define SQL_HYSTERESIS_DB 3.0f

typedef struct {
    Tnc* tnc;
    Radio* radio;
    uint8_t* bits; /**< TNC_TX_BITS_CAP line bits */
    TncTxItem* batch; /**< TNC_TXQ_DEPTH items */
    ModemTx gen; /**< read from the TX DMA ISR */
    /* what the radio is actually configured for */
    uint32_t freq;
    uint8_t mode;
    TncModulation mod;
    int8_t pwr;
    TncRadioKind kind;
    uint32_t txonce_hz;
} Worker;

/* ---------- helpers ---------- */

static LevelDuration tx_isr_callback(void* ctx) {
    Worker* w = ctx;
    bool level;
    uint32_t duration;
    if(modem_tx_next(&w->gen, &level, &duration)) {
        return level_duration_make(level, duration);
    }
    return level_duration_reset();
}

static void start_rx(Worker* w) {
    Tnc* tnc = w->tnc;
    tnc->rx_mode = w->mode;
    tnc->rx_gen++;
    tnc->rx_gate = false; // reopened by the next RSSI sample above squelch
    radio_start_rx(w->radio, tnc_rx_capture_isr, tnc);
    tnc->state = TncStateRx;
}

static void stop_rx(Worker* w) {
    w->tnc->rx_gate = false;
    w->tnc->rx_gen++;
    radio_stop_rx(w->radio);
}

static uint32_t cycles_now(void) {
    return furi_hal_cortex_timer_get(0).start; // DWT cycle counter
}

static void sample_rssi(Worker* w) {
    Tnc* tnc = w->tnc;
    if(!radio_is_rx(w->radio)) return;
    float rssi = radio_rssi(w->radio);
    tnc->rssi = rssi;
    float sql = (float)tnc->settings.sql_dbm;
    bool open = tnc->rx_gate ? (rssi >= sql - SQL_HYSTERESIS_DB) : (rssi >= sql);
    if(open != tnc->rx_gate) {
        if(!open) tnc->rx_gen++; // squelch closed: demod drops partial state
        tnc->rx_gate = open;
    }
}

static void report_seq_error(Tnc* tnc, const TncTxItem* item, const char* err) {
    if(item->has_seq) {
        tnc_report(tnc, "ERR %s SEQ=%u", err, item->seq);
    } else {
        tnc_report(tnc, "ERR %s", err);
    }
}

/* ---------- configuration ---------- */

static void radio_bringup(Worker* w, TncRadioKind want) {
    w->kind = radio_begin(w->radio, want);
    radio_configure(w->radio, w->mode, w->mod, w->pwr);
    radio_set_frequency(w->radio, w->freq);
    w->tnc->radio_name = radio_name(w->radio);
    w->tnc->radio_ok = true;
}

static void handle_apply(Worker* w, const TncRadioCmd* cmd) {
    Tnc* tnc = w->tnc;
    const TncSettings* s = &cmd->settings;
    uint32_t ch = cmd->changed;
    char msg[160];
    size_t n = 0;
    msg[0] = 0;
#define ADD(...)                                                             \
    do {                                                                     \
        int _r = snprintf(msg + n, sizeof(msg) - n, __VA_ARGS__);            \
        if(_r > 0) n = ((size_t)_r < sizeof(msg) - n) ? n + (size_t)_r : sizeof(msg) - 1; \
    } while(0)

    bool radio_change = (ch & TNC_CH_RADIO) && s->radio != w->kind;
    bool reconf = radio_change || ((ch & TNC_CH_MODE) && s->mode != w->mode) ||
                  ((ch & TNC_CH_MOD) && s->mod != w->mod) ||
                  ((ch & TNC_CH_PWR) && s->pwr_dbm != w->pwr);
    bool retune = reconf || ((ch & TNC_CH_FREQ) && s->freq != w->freq);

    if(ch & TNC_CH_FREQ) w->freq = s->freq;
    if(ch & TNC_CH_MODE) w->mode = s->mode;
    if(ch & TNC_CH_MOD) w->mod = s->mod;
    if(ch & TNC_CH_PWR) w->pwr = s->pwr_dbm;

    if(reconf || retune) {
        stop_rx(w);
        if(radio_change) {
            radio_end(w->radio);
            radio_bringup(w, s->radio); // also configures + tunes
        } else {
            if(reconf) radio_configure(w->radio, w->mode, w->mod, w->pwr);
            radio_set_frequency(w->radio, w->freq);
        }
        start_rx(w);
    }
    if(ch & TNC_CH_TXONCE) w->txonce_hz = cmd->txonce_hz;

    // Commit only the fields that were changed, so concurrent KISS parameter
    // updates from the USB thread are not overwritten by a stale copy.
    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    TncSettings* t = &tnc->settings;
    if(ch & TNC_CH_FREQ) t->freq = w->freq;
    if(ch & TNC_CH_MODE) t->mode = w->mode;
    if(ch & TNC_CH_MOD) t->mod = w->mod;
    if(ch & TNC_CH_PWR) t->pwr_dbm = w->pwr;
    if(ch & TNC_CH_RADIO) t->radio = w->kind;
    if(ch & TNC_CH_SQL) t->sql_dbm = s->sql_dbm;
    if(ch & TNC_CH_DEBUG) t->debug = s->debug;
    if(ch & TNC_CH_CALL) strlcpy(t->call, s->call, sizeof(t->call));
    if(ch & TNC_CH_KISS) {
        t->txdelay = s->txdelay;
        t->persist = s->persist;
        t->slottime = s->slottime;
        t->txtail = s->txtail;
    }
    TncSettings snapshot = *t;
    furi_mutex_release(tnc->mutex);

    if(ch & ~TNC_CH_TXONCE) tnc_settings_save(&snapshot);

    ADD("%s", cmd->reply ? "OK" : "CFG");
    if(ch & TNC_CH_FREQ) {
        ADD(" FREQ=%lu TXOK=%u", (unsigned long)w->freq, radio_tx_allowed(w->freq) ? 1u : 0u);
    }
    if(ch & TNC_CH_MODE) ADD(" MODE=%u", w->mode);
    if(ch & TNC_CH_MOD) ADD(" MOD=%s", tnc_mod_name(w->mod));
    if(ch & TNC_CH_PWR) ADD(" PWR=%d", w->pwr);
    if(ch & TNC_CH_RADIO) {
        ADD(" RADIO=%s", radio_name(w->radio));
        if(s->radio == TncRadioExternal && w->kind != TncRadioExternal) ADD(" EXT_NOT_FOUND");
    }
    if(ch & TNC_CH_SQL) ADD(" SQL=%d", snapshot.sql_dbm);
    if(ch & TNC_CH_DEBUG) ADD(" DEBUG=%u", snapshot.debug ? 1u : 0u);
    if(ch & TNC_CH_CALL) ADD(" CALL=%s", snapshot.call);
    if(ch & TNC_CH_KISS) {
        ADD(" TXDELAY=%u P=%u SLOTTIME=%u TXTAIL=%u",
            snapshot.txdelay,
            snapshot.persist,
            snapshot.slottime,
            snapshot.txtail);
    }
    if(ch & TNC_CH_TXONCE) {
        ADD(" TXONCE=%lu TXOK=%u",
            (unsigned long)w->txonce_hz,
            radio_tx_allowed(w->txonce_hz) ? 1u : 0u);
    }
#undef ADD
    tnc_report(tnc, "%s", msg);
    if(tnc->view_port) view_port_update(tnc->view_port);
}

/* ---------- TX ---------- */

static void fail_queue(Worker* w, const char* err) {
    TncTxItem item;
    while(furi_message_queue_get(w->tnc->tx_q, &item, 0) == FuriStatusOk) {
        report_seq_error(w->tnc, &item, err);
    }
}

static void tx_burst(Worker* w) {
    Tnc* tnc = w->tnc;

    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    TncSettings s = tnc->settings;
    furi_mutex_release(tnc->mutex);

    size_t n = 0;
    while(n < TNC_TXQ_DEPTH &&
          furi_message_queue_get(tnc->tx_q, &w->batch[n], 0) == FuriStatusOk) {
        n++;
    }
    if(n == 0) return;

    uint32_t freq = w->txonce_hz ? w->txonce_hz : w->freq;
    if(!radio_tx_allowed(freq)) {
        for(size_t i = 0; i < n; i++) report_seq_error(tnc, &w->batch[i], "TX_BLOCKED_REGION");
        tnc_banner(tnc, "TX blocked (region)");
        w->txonce_hz = 0;
        tnc->state = TncStateRx;
        return;
    }

    ModemFrame frames[TNC_TXQ_DEPTH];
    for(size_t i = 0; i < n; i++) {
        frames[i].data = w->batch[i].data;
        frames[i].len = w->batch[i].len;
    }
    // Worst case (TXDELAY=TXTAIL=255 at 9600 + 8 max frames) is ~75 kbit; the buffer is 96 kbit.
    size_t nbits = modem_build_burst(
        w->mode, frames, n, s.txdelay * 10u, s.txtail * 10u, w->bits, TNC_TX_BITS_CAP);
    if(nbits == 0) {
        for(size_t i = 0; i < n; i++) report_seq_error(tnc, &w->batch[i], "FRAME_TOO_LONG");
        tnc->state = TncStateRx;
        return;
    }
    uint32_t burst_ms = (modem_burst_duration_us(w->mode, nbits) + 999u) / 1000u;

    // RX -> TX
    stop_rx(w);
    if(freq != w->freq) radio_set_frequency(w->radio, freq);
    modem_tx_init(&w->gen, w->mode, w->bits, nbits, true);
    tnc->state = TncStateTx;
    notification_message(tnc->notifications, &sequence_blink_red_10);
    if(tnc->view_port) view_port_update(tnc->view_port);

    uint32_t t0 = furi_get_tick();
    bool started = radio_start_tx(w->radio, tx_isr_callback, w);
    uint32_t done_cycles = 0;
    if(started) {
        if(burst_ms > 3) furi_delay_ms(burst_ms - 2);
        uint32_t deadline = t0 + burst_ms + 500;
        // Final stretch: poll tightly so we turn around fast.
        while(!radio_tx_complete(w->radio) && (int32_t)(furi_get_tick() - deadline) < 0) {
            furi_delay_us(50);
        }
        done_cycles = cycles_now();
        radio_stop_tx(w->radio);
    } else {
        done_cycles = cycles_now();
    }

    // TX -> RX
    if(freq != w->freq) radio_set_frequency(w->radio, w->freq);
    w->txonce_hz = 0;
    start_rx(w);
    uint32_t turn_us =
        (cycles_now() - done_cycles) / furi_hal_cortex_instructions_per_microsecond();
    uint32_t dur_ms = furi_get_tick() - t0;

    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    tnc->turnaround_us = turn_us;
    tnc->last_tx_ms = dur_ms;
    if(started) tnc->tx_count += (uint32_t)n;
    furi_mutex_release(tnc->mutex);

    for(size_t i = 0; i < n; i++) {
        TncTxItem* it = &w->batch[i];
        if(!started) {
            report_seq_error(tnc, it, "TX_BLOCKED_REGION");
            continue;
        }
        if(it->has_seq) {
            uint8_t ack[2] = {(uint8_t)(it->seq >> 8), (uint8_t)(it->seq & 0xFF)};
            usb_kiss_send(tnc->usb, KISS_CMD_ACKMODE, ack, sizeof(ack));
            tnc_report(tnc, "TXDONE SEQ=%u DUR=%lu", it->seq, (unsigned long)dur_ms);
        } else {
            tnc_report(tnc, "TXDONE SEQ=- DUR=%lu", (unsigned long)dur_ms);
        }
    }
    FURI_LOG_I(
        TAG, "TX %u frame(s), %lu ms, turnaround %lu us", (unsigned)n, dur_ms, turn_us);
    if(tnc->view_port) view_port_update(tnc->view_port);
}

static void csma_and_tx(Worker* w) {
    Tnc* tnc = w->tnc;
    tnc->state = TncStateCsma;
    uint32_t t0 = furi_get_tick();

    while(tnc->running) {
        sample_rssi(w);
        bool busy = tnc->rssi >= (float)tnc->settings.sql_dbm;
        if(!busy && (furi_hal_random_get() & 0xFFu) <= tnc->settings.persist) {
            tx_burst(w);
            return;
        }
        if(furi_get_tick() - t0 >= TNC_BUSY_TIMEOUT) {
            fail_queue(w, "BUSY_TIMEOUT");
            tnc_banner(tnc, "Channel busy, dropped");
            tnc->state = TncStateRx;
            return;
        }
        uint32_t slot = tnc->settings.slottime * 10u;
        furi_delay_ms(slot ? slot : 1);
    }
}

bool tnc_enqueue_tx(Tnc* tnc, const uint8_t* frame, size_t len, bool has_seq, uint16_t seq) {
    TncTxItem item = {.len = (uint16_t)len, .has_seq = has_seq, .seq = seq};
    if(len > AX25_MAX_FRAME) {
        report_seq_error(tnc, &item, "FRAME_TOO_LONG");
        return false;
    }
    if(len < AX25_MIN_FRAME) {
        report_seq_error(tnc, &item, "FRAME_TOO_SHORT");
        return false;
    }
    memcpy(item.data, frame, len);
    if(furi_message_queue_put(tnc->tx_q, &item, 0) != FuriStatusOk) {
        furi_mutex_acquire(tnc->mutex, FuriWaitForever);
        tnc->txq_full++;
        furi_mutex_release(tnc->mutex);
        report_seq_error(tnc, &item, "QUEUE_FULL");
        return false;
    }
    return true;
}

/* ---------- thread ---------- */

int32_t tnc_radio_worker(void* ctx) {
    Tnc* tnc = ctx;
    Worker* w = malloc(sizeof(Worker));
    memset(w, 0, sizeof(Worker));
    w->tnc = tnc;
    w->radio = radio_alloc();
    w->bits = malloc(TNC_TX_BITS_CAP / 8u);
    w->batch = malloc(sizeof(TncTxItem) * TNC_TXQ_DEPTH);

    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    w->freq = tnc->settings.freq;
    w->mode = tnc->settings.mode;
    w->mod = tnc->settings.mod;
    w->pwr = tnc->settings.pwr_dbm;
    TncRadioKind want = tnc->settings.radio;
    furi_mutex_release(tnc->mutex);

    radio_bringup(w, want);
    if(w->kind != want) {
        furi_mutex_acquire(tnc->mutex, FuriWaitForever);
        tnc->settings.radio = w->kind;
        furi_mutex_release(tnc->mutex);
        tnc_banner(tnc, "EXT radio not found");
        tnc_report(tnc, "ERR RADIO_INIT EXT_NOT_FOUND RADIO=%s", radio_name(w->radio));
    }
    start_rx(w);
    FURI_LOG_I(
        TAG,
        "Listening on %lu Hz, mode %s, %s",
        w->freq,
        modem_mode_name(w->mode),
        tnc_mod_name(w->mod));

    uint32_t next_hb = furi_get_tick() + TNC_HEARTBEAT_MS;
    while(tnc->running) {
        TncRadioCmd cmd;
        if(furi_message_queue_get(tnc->cmd_q, &cmd, TNC_RSSI_PERIOD) == FuriStatusOk) {
            if(cmd.type == TncRadioCmdApply) {
                handle_apply(w, &cmd);
            } else if(cmd.type == TncRadioCmdGet) {
                tnc_control_report_get(tnc);
            }
        }
        sample_rssi(w);

        if(furi_message_queue_get_count(tnc->tx_q) > 0) {
            csma_and_tx(w);
        }

        if((int32_t)(furi_get_tick() - next_hb) >= 0) {
            next_hb += TNC_HEARTBEAT_MS;
            tnc_report(
                tnc, "HB STATE=%s RSSI=%d", tnc_state_name(tnc->state), (int)tnc->rssi);
        }
    }

    // Exit: stop async, idle radio, end device (radio_free does all of it).
    stop_rx(w);
    radio_free(w->radio);
    free(w->batch);
    free(w->bits);
    free(w);
    return 0;
}
