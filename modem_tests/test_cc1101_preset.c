/* CC1101 preset: modulation format, data rates, deviation, PA, list format. */
#include <math.h>

#include "cc1101_preset.h"
#include "modem.h"
#include "testlib.h"

static uint8_t reg(const uint8_t* p, uint8_t r) {
    uint8_t v = 0xEE;
    CHECK_MSG(cc1101_preset_get(p, r, &v), "register 0x%02X missing", r);
    return v;
}

static double data_rate(uint8_t mdmcfg4, uint8_t mdmcfg3) {
    return (256.0 + mdmcfg3) * pow(2.0, mdmcfg4 & 0x0F) * 26e6 / pow(2.0, 28);
}

int main(void) {
    uint8_t p[CC1101_PRESET_MAX];
    const double baud[ModemModeCount] = {9600, 1200, 1200};

    for(int m = 0; m < ModemModeCount; m++) {
        for(int mod = 0; mod < 2; mod++) {
            size_t n = cc1101_build_preset(p, sizeof(p), (uint8_t)m, (Cc1101Mod)mod, 10);
            CHECK(n > 0 && n <= CC1101_PRESET_MAX);

            // MDMCFG2: MOD_FORMAT 001 = GFSK, 000 = 2-FSK; rest unchanged (0x04).
            uint8_t m2 = reg(p, CC1101_REG_MDMCFG2);
            CHECK_MSG(m2 == (mod == Cc1101ModGfsk ? 0x14 : 0x04), "mode %d mod %d MDMCFG2=%02X", m, mod, m2);

            CHECK(reg(p, CC1101_REG_PKTCTRL0) == 0x32); // async serial, no CRC/whitening
            CHECK(reg(p, CC1101_REG_IOCFG0) == 0x0D); // GDO0 = async data
            CHECK(reg(p, CC1101_REG_DEVIATN) == 0x07); // 2975.5 Hz
            CHECK((reg(p, CC1101_REG_MDMCFG4) & 0xF0) == KISS_TNC_CHANBW);

            // In async mode the GFSK filter is referenced to the programmed rate, so it
            // must match the line bit rate within 1/8 (datasheet) for the FSK modes.
            double r = data_rate(reg(p, CC1101_REG_MDMCFG4), reg(p, CC1101_REG_MDMCFG3));
            if(m != ModemModeAfsk1200) {
                CHECK_MSG(fabs(r - baud[m]) / baud[m] < 0.01, "mode %d rate %.1f", m, r);
            } else {
                // AFSK: GDO0 toggles up to 4400/s; programmed rate must exceed that.
                CHECK_MSG(r > 4400.0, "AFSK rate %.1f", r);
                if(mod == Cc1101ModGfsk) CHECK(fabs(r - 9595.9) < 1.0);
            }
            // List ends with 0,0 then PATABLE[0].
            CHECK(p[n - 10] == 0 && p[n - 9] == 0 && p[n - 8] == 0xC0);
        }
    }
    CHECK(fabs(26e6 / pow(2, 17) * (8 + 7) - 2975.46) < 0.01);
    CHECK(cc1101_patable(-10) == 0x34 && cc1101_patable(0) == 0x60);
    CHECK(cc1101_patable(5) == 0x84 && cc1101_patable(10) == 0xC0);
    CHECK(cc1101_build_preset(p, 10, 0, Cc1101ModGfsk, 10) == 0);
    return test_summary("test_cc1101_preset");
}
