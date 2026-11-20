#include "radio.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/devices/devices.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <power/power_service/power.h>

#include "modem/cc1101_preset.h"
#include "modem/modem.h"

#define TAG "KissTncRadio"

struct Radio {
    const SubGhzDevice* device;
    TncRadioKind kind;
    bool otg_on;
    bool rx_running;
    bool tx_running;
    bool registry_up;
    uint8_t preset[48]; /* CC1101_PRESET_MAX */
};

Radio* radio_alloc(void) {
    Radio* radio = malloc(sizeof(Radio));
    memset(radio, 0, sizeof(Radio));
    return radio;
}

void radio_free(Radio* radio) {
    radio_end(radio);
    if(radio->registry_up) {
        subghz_devices_deinit();
        radio->registry_up = false;
    }
    free(radio);
}

static void otg(Radio* radio, bool on) {
    if(radio->otg_on == on) return;
    Power* power = furi_record_open(RECORD_POWER);
    power_enable_otg(power, on);
    furi_record_close(RECORD_POWER);
    radio->otg_on = on;
}

TncRadioKind radio_begin(Radio* radio, TncRadioKind want) {
    if(!radio->registry_up) {
        subghz_devices_init();
        radio->registry_up = true;
    }

    radio->device = NULL;
    radio->kind = TncRadioInternal;

    if(want == TncRadioExternal) {
        otg(radio, true);
        furi_delay_ms(10); // let the module power up
        const SubGhzDevice* ext = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
        if(ext && subghz_devices_is_connect(ext) && subghz_devices_begin(ext)) {
            radio->device = ext;
            radio->kind = TncRadioExternal;
        } else {
            FURI_LOG_W(TAG, "External CC1101 not found, using internal");
            otg(radio, false);
        }
    }

    if(!radio->device) {
        radio->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
        furi_check(radio->device);
        subghz_devices_begin(radio->device);
    }

    subghz_devices_reset(radio->device);
    subghz_devices_idle(radio->device);
    FURI_LOG_I(TAG, "Using %s", subghz_devices_get_name(radio->device));
    return radio->kind;
}

void radio_end(Radio* radio) {
    if(radio->device) {
        if(radio->rx_running) radio_stop_rx(radio);
        if(radio->tx_running) radio_stop_tx(radio);
        subghz_devices_idle(radio->device);
        subghz_devices_sleep(radio->device);
        subghz_devices_end(radio->device);
        radio->device = NULL;
    }
    otg(radio, false);
}

const char* radio_name(const Radio* radio) {
    return radio->kind == TncRadioExternal ? "EXT" : "INT";
}

void radio_configure(Radio* radio, uint8_t mode, TncModulation mod, int8_t pwr_dbm) {
    furi_check(!radio->rx_running && !radio->tx_running);
    // Register values and their derivations: modem/cc1101_preset.c, docs/PROTOCOL.md §9.
    size_t n = cc1101_build_preset(
        radio->preset,
        sizeof(radio->preset),
        mode,
        mod == TncModulation2Fsk ? Cc1101Mod2Fsk : Cc1101ModGfsk,
        pwr_dbm);
    furi_check(n);
    subghz_devices_reset(radio->device);
    subghz_devices_load_preset(radio->device, FuriHalSubGhzPresetCustom, radio->preset);
}

uint32_t radio_set_frequency(Radio* radio, uint32_t hz) {
    furi_check(!radio->rx_running && !radio->tx_running);
    // The internal device furi_crash()es on an invalid frequency; never let one through.
    furi_check(tnc_freq_in_range(hz) && subghz_devices_is_frequency_valid(radio->device, hz));
    return subghz_devices_set_frequency(radio->device, hz);
}

bool radio_tx_allowed(uint32_t hz) {
    return furi_hal_region_is_frequency_allowed(hz);
}

void radio_start_rx(Radio* radio, RadioRxCallback cb, void* ctx) {
    if(radio->rx_running) return;
    subghz_devices_flush_rx(radio->device);
    subghz_devices_start_async_rx(radio->device, cb, ctx); // switches the chip to RX
    radio->rx_running = true;
}

void radio_stop_rx(Radio* radio) {
    if(!radio->rx_running) return;
    subghz_devices_stop_async_rx(radio->device); // leaves the chip idle
    radio->rx_running = false;
}

bool radio_is_rx(const Radio* radio) {
    return radio->rx_running;
}

bool radio_start_tx(Radio* radio, RadioTxCallback cb, void* ctx) {
    furi_check(!radio->rx_running && !radio->tx_running);
    radio->tx_running = subghz_devices_start_async_tx(radio->device, cb, ctx);
    return radio->tx_running;
}

bool radio_tx_complete(Radio* radio) {
    return !radio->tx_running || subghz_devices_is_async_complete_tx(radio->device);
}

void radio_stop_tx(Radio* radio) {
    if(!radio->tx_running) return;
    subghz_devices_stop_async_tx(radio->device); // leaves the chip idle
    radio->tx_running = false;
}

float radio_rssi(Radio* radio) {
    return subghz_devices_get_rssi(radio->device);
}
