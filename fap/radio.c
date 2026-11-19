#include "radio.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/drivers/cc1101_regs.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/devices/devices.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <power/power_service/power.h>

#include "modem/modem.h"

#define TAG "KissTncRadio"

struct Radio {
    const SubGhzDevice* device;
    TncRadioKind kind;
    bool otg_on;
    bool rx_running;
    bool tx_running;
    bool registry_up;
    uint8_t preset[64];
};

/*
 * CC1101 register values. Crystal f_xosc = 26 MHz. See docs/PROTOCOL.md for the derivations.
 *
 * DEVIATN 0x07: E=0, M=7  -> f_dev = 26e6 / 2^17 * (8 + 7) * 2^0 = 2975.5 Hz
 * MDMCFG3 0x83 (DRATE_M = 131), DRATE_E per mode (MDMCFG4[3:0]):
 *   R = (256 + 131) * 2^E * 26e6 / 2^28
 *   E=8 ->  9595.9 Bd (FSK9600)
 *   E=7 ->  4797.9 Bd (AFSK1200: passes the 2200 Hz tone's 4400 transitions/s)
 *   E=5 ->  1199.5 Bd (FSK1200)
 */
static uint8_t drate_e_for_mode(uint8_t mode) {
    switch(mode) {
    case ModemModeFsk9600:
        return 8;
    case ModemModeAfsk1200:
        return 7;
    default:
        return 5;
    }
}

static uint8_t patable_for_dbm(int8_t dbm) {
    // Values from the firmware's own 2-FSK async preset table (433 MHz column).
    switch(dbm) {
    case -10:
        return 0x34;
    case 0:
        return 0x60;
    case 5:
        return 0x84;
    default:
        return 0xC0; // +10 dBm
    }
}

size_t radio_build_preset(uint8_t* out, size_t cap, uint8_t mode, int8_t pwr_dbm) {
    const uint8_t regs[] = {
        CC1101_IOCFG0,   0x0D, // GDO0 = async serial data in/out
        CC1101_FSCTRL1,  0x06, // IF = 26e6 / 2^10 * 6 = 152.3 kHz
        CC1101_PKTCTRL0, 0x32, // async serial mode, infinite length, no CRC, no whitening
        CC1101_PKTCTRL1, 0x04, // no address check (unused in async)
        CC1101_MDMCFG0,  0x00, // channel spacing mantissa (unused)
        CC1101_MDMCFG1,  0x02, // CHANSPC_E = 2, no FEC (unused in async)
        CC1101_MDMCFG2,  0x04, // 2-FSK, DC filter on, no Manchester, no sync word
        CC1101_MDMCFG3,  0x83, // DRATE_M = 131
        CC1101_MDMCFG4,  (uint8_t)(KISS_TNC_CHANBW | drate_e_for_mode(mode)),
        CC1101_DEVIATN,  0x07, // ~2.98 kHz deviation
        CC1101_MCSM0,    0x18, // autocal IDLE->RX/TX, PO_TIMEOUT 149-155 us
        CC1101_FOCCFG,   0x16, // FOC on: pre-K 3K, post-K K/2, limit +/- BW/4
        CC1101_AGCCTRL0, 0x91, // medium hysteresis, 16 samples, normal AGC
        CC1101_AGCCTRL1, 0x00, // relative carrier sense off
        CC1101_AGCCTRL2, 0x07, // all gain settings, 42 dB target
        CC1101_WORCTRL,  0xFB,
        CC1101_FREND0,   0x10, // PA_POWER index 0 -> PATABLE[0]
        CC1101_FREND1,   0x56,
    };
    size_t need = sizeof(regs) + 2 + 8;
    if(cap < need) return 0;
    memcpy(out, regs, sizeof(regs));
    size_t n = sizeof(regs);
    out[n++] = 0; // end of registers
    out[n++] = 0;
    memset(&out[n], 0, 8);
    out[n] = patable_for_dbm(pwr_dbm); // 2-FSK uses PATABLE[0] only
    return n + 8;
}

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

void radio_configure(Radio* radio, uint8_t mode, int8_t pwr_dbm) {
    furi_check(!radio->rx_running && !radio->tx_running);
    size_t n = radio_build_preset(radio->preset, sizeof(radio->preset), mode, pwr_dbm);
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
