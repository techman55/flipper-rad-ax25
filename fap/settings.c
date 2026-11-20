#include "settings.h"

#include <furi.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>

#include "modem/ax25.h"
#include "modem/modem.h"

#define TAG "KissTncSettings"

void tnc_settings_defaults(TncSettings* s) {
    memset(s, 0, sizeof(*s));
    s->freq = 433920000;
    s->mode = ModemModeFsk9600;
    s->mod = TncModulationGfsk;
    s->pwr_dbm = 10;
    s->radio = TncRadioInternal;
    s->sql_dbm = -95;
    s->debug = false;
    strlcpy(s->call, TNC_DEFAULT_CALL, sizeof(s->call));
    s->txdelay = 30;
    s->persist = 63;
    s->slottime = 10;
    s->txtail = 2;
}

bool tnc_freq_in_range(uint32_t hz) {
    // CC1101 bands as the firmware accepts them (furi_hal_subghz_is_frequency_valid).
    // Anything else would furi_crash() inside set_frequency.
    return (hz >= 300000000u && hz <= 348000000u) || (hz >= 387000000u && hz <= 464000000u) ||
           (hz >= 779000000u && hz <= 928000000u);
}

const char* tnc_mod_name(TncModulation mod) {
    return mod == TncModulation2Fsk ? "FSK" : "GFSK";
}

bool tnc_pwr_valid(int32_t dbm) {
    return dbm == -10 || dbm == 0 || dbm == 5 || dbm == 10;
}

bool tnc_sql_valid(int32_t dbm) {
    return dbm >= -140 && dbm <= -20;
}

bool tnc_call_valid(const char* call) {
    char c[7];
    uint8_t ssid;
    return strlen(call) < sizeof(((TncSettings*)0)->call) && ax25_parse_call(call, c, &ssid);
}

bool tnc_call_is_default(const char* call) {
    char c[7];
    uint8_t ssid;
    if(!ax25_parse_call(call, c, &ssid)) return true;
    return strcmp(c, TNC_DEFAULT_CALL) == 0;
}

static void apply_kv(TncSettings* s, const char* key, const char* val) {
    char* end = NULL;
    long v = strtol(val, &end, 10);
    bool num = end && end != val && *end == 0;

    if(strcmp(key, "FREQ") == 0) {
        unsigned long f = strtoul(val, &end, 10);
        if(end && *end == 0 && tnc_freq_in_range((uint32_t)f)) s->freq = (uint32_t)f;
    } else if(strcmp(key, "MODE") == 0) {
        if(num && v >= 0 && v < ModemModeCount) s->mode = (uint8_t)v;
    } else if(strcmp(key, "MOD") == 0) {
        if(strcmp(val, "GFSK") == 0) s->mod = TncModulationGfsk;
        if(strcmp(val, "FSK") == 0) s->mod = TncModulation2Fsk;
    } else if(strcmp(key, "PWR") == 0) {
        if(num && tnc_pwr_valid(v)) s->pwr_dbm = (int8_t)v;
    } else if(strcmp(key, "RADIO") == 0) {
        if(strcmp(val, "EXT") == 0) s->radio = TncRadioExternal;
        if(strcmp(val, "INT") == 0) s->radio = TncRadioInternal;
    } else if(strcmp(key, "SQL") == 0) {
        if(num && tnc_sql_valid(v)) s->sql_dbm = (int16_t)v;
    } else if(strcmp(key, "DEBUG") == 0) {
        if(num) s->debug = v != 0;
    } else if(strcmp(key, "CALL") == 0) {
        if(tnc_call_valid(val)) strlcpy(s->call, val, sizeof(s->call));
    } else if(strcmp(key, "TXDELAY") == 0) {
        if(num && v >= 0 && v <= 255) s->txdelay = (uint8_t)v;
    } else if(strcmp(key, "P") == 0) {
        if(num && v >= 0 && v <= 255) s->persist = (uint8_t)v;
    } else if(strcmp(key, "SLOTTIME") == 0) {
        if(num && v >= 0 && v <= 255) s->slottime = (uint8_t)v;
    } else if(strcmp(key, "TXTAIL") == 0) {
        if(num && v >= 0 && v <= 255) s->txtail = (uint8_t)v;
    }
}

void tnc_settings_load(TncSettings* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, TNC_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[512];
        size_t n = storage_file_read(file, buf, sizeof(buf) - 1);
        buf[n] = 0;
        char* line = buf;
        while(line && *line) {
            char* next = strchr(line, '\n');
            if(next) *next++ = 0;
            size_t l = strlen(line);
            if(l && line[l - 1] == '\r') line[l - 1] = 0;
            char* eq = strchr(line, '=');
            if(eq && line[0] != '#') {
                *eq = 0;
                apply_kv(s, line, eq + 1);
            }
            line = next;
        }
        FURI_LOG_I(TAG, "Loaded settings");
    } else {
        FURI_LOG_I(TAG, "No settings file, using defaults");
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

bool tnc_settings_save(const TncSettings* s) {
    char buf[384];
    int n = snprintf(
        buf,
        sizeof(buf),
        "# KISS TNC settings\n"
        "FREQ=%lu\nMODE=%u\nMOD=%s\nPWR=%d\nRADIO=%s\nSQL=%d\nDEBUG=%u\nCALL=%s\n"
        "TXDELAY=%u\nP=%u\nSLOTTIME=%u\nTXTAIL=%u\n",
        (unsigned long)s->freq,
        s->mode,
        tnc_mod_name(s->mod),
        s->pwr_dbm,
        s->radio == TncRadioExternal ? "EXT" : "INT",
        s->sql_dbm,
        s->debug ? 1u : 0u,
        s->call,
        s->txdelay,
        s->persist,
        s->slottime,
        s->txtail);
    if(n <= 0 || (size_t)n >= sizeof(buf)) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, TNC_SETTINGS_DIR);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, TNC_SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
              storage_file_write(file, buf, (size_t)n) == (size_t)n;
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(!ok) FURI_LOG_E(TAG, "Failed to save settings");
    return ok;
}
