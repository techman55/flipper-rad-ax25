/**
 * KISS command dispatch and the SetHardware (0x06) vendor protocol.
 * Runs on the USB worker thread. Anything that touches the radio is handed to
 * the radio worker as a TncRadioCmd; the worker sends the OK/ERR reply.
 */
#include <furi_hal_region.h>
#include <furi_hal_version.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "modem/kiss.h"
#include "tnc.h"

#define TAG "KissTncCtl"

const char* tnc_state_name(TncState state) {
    switch(state) {
    case TncStateInit:
        return "INIT";
    case TncStateRx:
        return "RX";
    case TncStateCsma:
        return "CSMA";
    case TncStateTx:
        return "TX";
    default:
        return "ERROR";
    }
}

void tnc_report(Tnc* tnc, const char* fmt, ...) {
    char buf[384];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if(n <= 0) return;
    if((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    usb_kiss_send(tnc->usb, KISS_CMD_SETHARDWARE, (const uint8_t*)buf, (size_t)n);
}

void tnc_banner(Tnc* tnc, const char* text) {
    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    strlcpy(tnc->banner, text, sizeof(tnc->banner));
    tnc->banner_until = furi_get_tick() + 3000;
    furi_mutex_release(tnc->mutex);
    if(tnc->view_port) view_port_update(tnc->view_port);
}

void tnc_control_report_get(Tnc* tnc) {
    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    TncSettings s = tnc->settings;
    uint32_t rx = tnc->rx_count, tx = tnc->tx_count, bad = tnc->bad_count;
    uint32_t turn = tnc->turnaround_us, txq_full = tnc->txq_full;
    size_t heap_start = tnc->heap_at_start;
    const char* radio = tnc->radio_name;
    furi_mutex_release(tnc->mutex);

    const Version* ver = furi_hal_version_get_firmware_version();
    tnc_report(
        tnc,
        "OK FREQ=%lu MODE=%u PWR=%d RADIO=%s SQL=%d DEBUG=%u CALL=%s "
        "TXDELAY=%u P=%u SLOTTIME=%u TXTAIL=%u "
        "STATE=%s RSSI=%d RXF=%lu TXF=%lu BADF=%lu "
        "EDGE_OVF=%lu USB_DROP=%lu KISS_OVF=%lu TXQ_FULL=%lu TURN_US=%lu "
        "HEAP=%lu HEAP_START=%lu HEAP_MIN=%lu REGION=%s TXOK=%u "
        "APP=%s FW=%s",
        (unsigned long)s.freq,
        s.mode,
        s.pwr_dbm,
        radio ? radio : "?",
        s.sql_dbm,
        s.debug ? 1u : 0u,
        s.call,
        s.txdelay,
        s.persist,
        s.slottime,
        s.txtail,
        tnc_state_name(tnc->state),
        (int)tnc->rssi,
        (unsigned long)rx,
        (unsigned long)tx,
        (unsigned long)bad,
        (unsigned long)tnc->edge_overflow,
        (unsigned long)usb_kiss_dropped(tnc->usb),
        (unsigned long)usb_kiss_rx_overflows(tnc->usb),
        (unsigned long)txq_full,
        (unsigned long)turn,
        (unsigned long)memmgr_get_free_heap(),
        (unsigned long)heap_start,
        (unsigned long)memmgr_get_minimum_free_heap(),
        furi_hal_region_get_name(),
        radio_tx_allowed(s.freq) ? 1u : 0u,
        TNC_APP_VERSION,
        ver ? version_get_version(ver) : "?");
}

static char* trim(char* s) {
    while(*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while(n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    return s;
}

static bool parse_long(const char* v, long* out) {
    char* end = NULL;
    *out = strtol(v, &end, 10);
    return end && end != v && *end == 0;
}

static void handle_sethw(Tnc* tnc, const uint8_t* data, size_t len) {
    char line[200];
    if(len >= sizeof(line)) {
        tnc_report(tnc, "ERR LINE_TOO_LONG");
        return;
    }
    memcpy(line, data, len);
    line[len] = 0;

    TncRadioCmd cmd = {.type = TncRadioCmdApply, .reply = true};
    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    cmd.settings = tnc->settings;
    furi_mutex_release(tnc->mutex);

    bool get = false;
    char* next = line;
    while(next) {
        // Split on ';' by hand (strtok_r is not exported to FAPs).
        char* tok = next;
        next = strchr(tok, ';');
        if(next) *next++ = 0;
        char* item = trim(tok);
        if(!*item) continue;
        char* val = strchr(item, '=');
        if(val) *val++ = 0;
        char* key = trim(item);
        for(char* p = key; *p; p++)
            if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        if(val) val = trim(val);
        long v = 0;

        if(strcmp(key, "GET") == 0) {
            get = true;
        } else if(!val) {
            tnc_report(tnc, "ERR MISSING_VALUE %s", key);
            return;
        } else if(strcmp(key, "FREQ") == 0 || strcmp(key, "TXONCE") == 0) {
            char* end = NULL;
            unsigned long f = strtoul(val, &end, 10);
            if(!end || end == val || *end || !tnc_freq_in_range((uint32_t)f)) {
                tnc_report(tnc, "ERR FREQ_RANGE");
                return;
            }
            if(key[0] == 'F') {
                cmd.settings.freq = (uint32_t)f;
                cmd.changed |= TNC_CH_FREQ;
            } else {
                cmd.txonce_hz = (uint32_t)f;
                cmd.changed |= TNC_CH_TXONCE;
            }
        } else if(strcmp(key, "MODE") == 0) {
            if(!parse_long(val, &v) || v < 0 || v >= ModemModeCount) {
                tnc_report(tnc, "ERR BAD_VALUE MODE");
                return;
            }
            cmd.settings.mode = (uint8_t)v;
            cmd.changed |= TNC_CH_MODE;
        } else if(strcmp(key, "PWR") == 0) {
            if(!parse_long(val, &v) || !tnc_pwr_valid(v)) {
                tnc_report(tnc, "ERR BAD_VALUE PWR");
                return;
            }
            cmd.settings.pwr_dbm = (int8_t)v;
            cmd.changed |= TNC_CH_PWR;
        } else if(strcmp(key, "RADIO") == 0) {
            if(strcmp(val, "INT") == 0 || strcmp(val, "int") == 0) {
                cmd.settings.radio = TncRadioInternal;
            } else if(strcmp(val, "EXT") == 0 || strcmp(val, "ext") == 0) {
                cmd.settings.radio = TncRadioExternal;
            } else {
                tnc_report(tnc, "ERR BAD_VALUE RADIO");
                return;
            }
            cmd.changed |= TNC_CH_RADIO;
        } else if(strcmp(key, "SQL") == 0) {
            if(!parse_long(val, &v) || !tnc_sql_valid(v)) {
                tnc_report(tnc, "ERR BAD_VALUE SQL");
                return;
            }
            cmd.settings.sql_dbm = (int16_t)v;
            cmd.changed |= TNC_CH_SQL;
        } else if(strcmp(key, "DEBUG") == 0) {
            if(!parse_long(val, &v) || v < 0 || v > 1) {
                tnc_report(tnc, "ERR BAD_VALUE DEBUG");
                return;
            }
            cmd.settings.debug = v != 0;
            cmd.changed |= TNC_CH_DEBUG;
        } else if(strcmp(key, "CALL") == 0) {
            for(char* p = val; *p; p++)
                if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
            if(!tnc_call_valid(val)) {
                tnc_report(tnc, "ERR BAD_VALUE CALL");
                return;
            }
            strlcpy(cmd.settings.call, val, sizeof(cmd.settings.call));
            cmd.changed |= TNC_CH_CALL;
        } else if(
            strcmp(key, "TXDELAY") == 0 || strcmp(key, "P") == 0 ||
            strcmp(key, "SLOTTIME") == 0 || strcmp(key, "TXTAIL") == 0) {
            if(!parse_long(val, &v) || v < 0 || v > 255) {
                tnc_report(tnc, "ERR BAD_VALUE %s", key);
                return;
            }
            if(strcmp(key, "TXDELAY") == 0) cmd.settings.txdelay = (uint8_t)v;
            if(strcmp(key, "P") == 0) cmd.settings.persist = (uint8_t)v;
            if(strcmp(key, "SLOTTIME") == 0) cmd.settings.slottime = (uint8_t)v;
            if(strcmp(key, "TXTAIL") == 0) cmd.settings.txtail = (uint8_t)v;
            cmd.changed |= TNC_CH_KISS;
        } else {
            tnc_report(tnc, "ERR UNKNOWN_KEY %s", key);
            return;
        }
    }

    if(cmd.changed) {
        if(furi_message_queue_put(tnc->cmd_q, &cmd, 100) != FuriStatusOk) {
            tnc_report(tnc, "ERR BUSY");
        }
        // The radio worker replies "OK ..." once applied. A GET in the same line
        // is queued behind it so the worker answers it afterwards, in order.
        if(get) {
            TncRadioCmd g = {.type = TncRadioCmdGet};
            furi_message_queue_put(tnc->cmd_q, &g, 100);
        }
    } else if(get) {
        tnc_control_report_get(tnc);
    } else {
        tnc_report(tnc, "ERR EMPTY");
    }
}

static void set_kiss_param(Tnc* tnc, uint8_t cmd, uint8_t value) {
    furi_mutex_acquire(tnc->mutex, FuriWaitForever);
    switch(cmd) {
    case KISS_CMD_TXDELAY:
        tnc->settings.txdelay = value;
        break;
    case KISS_CMD_P:
        tnc->settings.persist = value;
        break;
    case KISS_CMD_SLOTTIME:
        tnc->settings.slottime = value;
        break;
    case KISS_CMD_TXTAIL:
        tnc->settings.txtail = value;
        break;
    default:
        break;
    }
    furi_mutex_release(tnc->mutex);
}

void tnc_control_on_kiss(void* ctx, const uint8_t* frame, size_t len) {
    Tnc* tnc = ctx;
    if(!frame || len < 1) {
        tnc_report(tnc, "ERR FRAME_TOO_LONG"); // overflowed the KISS decoder
        return;
    }
    uint8_t type = frame[0];
    if(type == KISS_CMD_RETURN) return; // nothing to return from
    if((type >> 4) != 0) return; // single-port TNC: ignore other ports
    uint8_t cmd = type & 0x0F;

    switch(cmd) {
    case KISS_CMD_DATA:
        tnc_enqueue_tx(tnc, frame + 1, len - 1, false, 0);
        break;
    case KISS_CMD_TXDELAY:
    case KISS_CMD_P:
    case KISS_CMD_SLOTTIME:
    case KISS_CMD_TXTAIL:
        if(len >= 2) set_kiss_param(tnc, cmd, frame[1]);
        break;
    case KISS_CMD_FULLDUPLEX:
        break; // half-duplex only
    case KISS_CMD_SETHARDWARE:
        handle_sethw(tnc, frame + 1, len - 1);
        break;
    case KISS_CMD_ACKMODE:
        if(len >= 3) {
            uint16_t seq = (uint16_t)((frame[1] << 8) | frame[2]);
            tnc_enqueue_tx(tnc, frame + 3, len - 3, true, seq);
        }
        break;
    default:
        FURI_LOG_D(TAG, "Ignoring KISS command 0x%02X", type);
        break;
    }
}
