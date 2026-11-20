#include "cc1101_preset.h"

#include <string.h>

#include "modem.h"

uint8_t cc1101_drate_e(uint8_t mode, Cc1101Mod mod) {
    /* R = (256 + 131) * 2^E * 26e6 / 2^28:
     *   E=8 -> 9595.9 Bd, E=7 -> 4797.9 Bd, E=5 -> 1199.5 Bd.
     * In async mode the CC1101 oversamples GDO0 at 8x R and, for GFSK, applies a
     * BT=0.5 Gaussian filter relative to R. So R must match the line bit rate. */
    switch(mode) {
    case ModemModeFsk9600:
        return 8;
    case ModemModeAfsk1200:
        /* GDO0 toggles at the tone rate (up to 4400 transitions/s for 2200 Hz).
         * GFSK: 9.6 kBd puts the Gaussian -3 dB point near 4.8 kHz, so the 2200 Hz
         * fundamental passes and the square wave's harmonics are smoothed off.
         * 2-FSK: 4.8 kBd (as the stock async preset). */
        return mod == Cc1101ModGfsk ? 8 : 7;
    default:
        return 5;
    }
}

uint8_t cc1101_patable(int8_t dbm) {
    /* From the firmware's 2-FSK async preset table (433 MHz column). */
    switch(dbm) {
    case -10:
        return 0x34;
    case 0:
        return 0x60;
    case 5:
        return 0x84;
    default:
        return 0xC0; /* +10 dBm */
    }
}

size_t cc1101_build_preset(uint8_t* out, size_t cap, uint8_t mode, Cc1101Mod mod, int8_t pwr_dbm) {
    uint8_t mod_format = (mod == Cc1101ModGfsk) ? CC1101_MOD_GFSK : CC1101_MOD_2FSK;
    const uint8_t regs[] = {
        CC1101_REG_IOCFG0,   0x0D, /* GDO0 = async serial data in/out */
        CC1101_REG_FSCTRL1,  0x06, /* IF = 26e6 / 2^10 * 6 = 152.3 kHz */
        CC1101_REG_PKTCTRL0, 0x32, /* async serial, infinite length, no CRC, no whitening */
        CC1101_REG_PKTCTRL1, 0x04, /* unused in async */
        CC1101_REG_MDMCFG0,  0x00, /* channel spacing, unused */
        CC1101_REG_MDMCFG1,  0x02, /* CHANSPC_E = 2, no FEC (unused in async) */
        CC1101_REG_MDMCFG2,  (uint8_t)(mod_format | 0x04), /* DC filter on, no Manchester, no sync */
        CC1101_REG_MDMCFG3,  0x83, /* DRATE_M = 131 */
        CC1101_REG_MDMCFG4,  (uint8_t)(KISS_TNC_CHANBW | cc1101_drate_e(mode, mod)),
        CC1101_REG_DEVIATN,  0x07, /* E=0, M=7 -> 2975.5 Hz */
        CC1101_REG_MCSM0,    0x18, /* autocal IDLE->RX/TX */
        CC1101_REG_FOCCFG,   0x16, /* FOC on, limit +/- BW/4 */
        CC1101_REG_AGCCTRL0, 0x91,
        CC1101_REG_AGCCTRL1, 0x00,
        CC1101_REG_AGCCTRL2, 0x07,
        CC1101_REG_WORCTRL,  0xFB,
        CC1101_REG_FREND0,   0x10, /* PA_POWER index 0 -> PATABLE[0] */
        CC1101_REG_FREND1,   0x56,
    };
    if(cap < CC1101_PRESET_MAX || sizeof(regs) + 2u + 8u > CC1101_PRESET_MAX) return 0;
    memcpy(out, regs, sizeof(regs));
    size_t n = sizeof(regs);
    out[n++] = 0; /* end of register list */
    out[n++] = 0;
    memset(&out[n], 0, 8);
    out[n] = cc1101_patable(pwr_dbm); /* 2-FSK/GFSK use PATABLE[0] only */
    return n + 8u;
}

bool cc1101_preset_get(const uint8_t* preset, uint8_t reg, uint8_t* value) {
    for(size_t i = 0; preset[i] != 0; i += 2) {
        if(preset[i] == reg) {
            *value = preset[i + 1];
            return true;
        }
    }
    return false;
}
