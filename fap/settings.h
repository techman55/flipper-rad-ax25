/**
 * @file settings.h
 * TNC settings, persisted as KEY=VALUE lines in /ext/apps_data/kiss_tnc/settings.txt.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TNC_SETTINGS_DIR  "/ext/apps_data/kiss_tnc"
#define TNC_SETTINGS_PATH TNC_SETTINGS_DIR "/settings.txt"
#define TNC_DEFAULT_CALL  "N0CALL"

typedef enum {
    TncRadioInternal = 0,
    TncRadioExternal = 1,
} TncRadioKind;

typedef enum {
    TncModulationGfsk = 0, /**< Gaussian 2-FSK, BT 0.5 (default) */
    TncModulation2Fsk = 1, /**< plain 2-FSK */
} TncModulation;

typedef struct {
    uint32_t freq; /**< Hz */
    uint8_t mode; /**< ModemMode */
    TncModulation mod; /**< CC1101 MOD_FORMAT: GFSK or 2-FSK */
    int8_t pwr_dbm; /**< -10, 0, 5, 10 */
    TncRadioKind radio;
    int16_t sql_dbm; /**< squelch / carrier-sense threshold */
    bool debug; /**< forward bad-FCS frames */
    char call[10]; /**< my callsign, "CALL-SSID" */
    /* KISS parameters */
    uint8_t txdelay; /**< 10 ms units */
    uint8_t persist; /**< p = (persist + 1) / 256 */
    uint8_t slottime; /**< 10 ms units */
    uint8_t txtail; /**< 10 ms units */
} TncSettings;

void tnc_settings_defaults(TncSettings* s);

/** Load from SD. Missing file or keys keep their defaults. Invalid values are ignored. */
void tnc_settings_load(TncSettings* s);

/** Save to SD. Returns false on storage error. */
bool tnc_settings_save(const TncSettings* s);

/* Validation helpers shared with the SetHardware parser. */
bool tnc_freq_in_range(uint32_t hz);
const char* tnc_mod_name(TncModulation mod); /**< "GFSK" / "FSK" */
bool tnc_pwr_valid(int32_t dbm);
bool tnc_sql_valid(int32_t dbm);
bool tnc_call_valid(const char* call);
/** True if the callsign is still the placeholder (N0CALL with any SSID). */
bool tnc_call_is_default(const char* call);
