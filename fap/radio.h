/**
 * @file radio.h
 * CC1101 control through the subghz_devices API (internal or external module).
 * Not thread-safe: call everything from the radio worker thread.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <toolbox/level_duration.h>

#include "settings.h"

/* Channel filter width: KISS_TNC_CHANBW in modem/cc1101_preset.h (default 81.25 kHz). */

typedef struct Radio Radio;

typedef void (*RadioRxCallback)(bool level, uint32_t duration, void* ctx);
typedef LevelDuration (*RadioTxCallback)(void* ctx);

Radio* radio_alloc(void);
void radio_free(Radio* radio);

/**
 * Initialise the device registry and pick a module. Falls back to the internal
 * CC1101 if the external one is requested but not detected (5 V OTG is enabled
 * only while the external module is in use).
 * @return the kind actually selected
 */
TncRadioKind radio_begin(Radio* radio, TncRadioKind want);

/** Idle the radio, release the device and turn OTG back off. radio_begin() may follow. */
void radio_end(Radio* radio);

const char* radio_name(const Radio* radio);

/** Load the custom async preset (GFSK or 2-FSK) for a mode + power. Radio must be idle. */
void radio_configure(Radio* radio, uint8_t mode, TncModulation mod, int8_t pwr_dbm);

/** Tune. `hz` must satisfy tnc_freq_in_range(). Radio must be idle. Returns the real frequency. */
uint32_t radio_set_frequency(Radio* radio, uint32_t hz);

/** Region check (furi_hal_region_is_frequency_allowed). */
bool radio_tx_allowed(uint32_t hz);

void radio_start_rx(Radio* radio, RadioRxCallback cb, void* ctx);
void radio_stop_rx(Radio* radio);
bool radio_is_rx(const Radio* radio);

/** Start async TX. Returns false if the device refused (e.g. region). */
bool radio_start_tx(Radio* radio, RadioTxCallback cb, void* ctx);
bool radio_tx_complete(Radio* radio);
void radio_stop_tx(Radio* radio);

float radio_rssi(Radio* radio);
