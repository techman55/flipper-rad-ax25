/**
 * @file cc1101_preset.h
 * CC1101 register preset for the TNC, in the format the Flipper's
 * subghz_devices_load_preset(..., FuriHalSubGhzPresetCustom, data) expects:
 * {addr, value}... 0, 0, PATABLE[8].
 *
 * Pure C (register addresses from the CC1101 datasheet), so the values can be
 * unit-tested on the host. Derivations are in docs/PROTOCOL.md §9.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CC1101 configuration register addresses (datasheet table 43). */
#define CC1101_REG_IOCFG0   0x02u
#define CC1101_REG_PKTCTRL1 0x07u
#define CC1101_REG_PKTCTRL0 0x08u
#define CC1101_REG_FSCTRL1  0x0Bu
#define CC1101_REG_MDMCFG4  0x10u
#define CC1101_REG_MDMCFG3  0x11u
#define CC1101_REG_MDMCFG2  0x12u
#define CC1101_REG_MDMCFG1  0x13u
#define CC1101_REG_MDMCFG0  0x14u
#define CC1101_REG_DEVIATN  0x15u
#define CC1101_REG_MCSM0    0x18u
#define CC1101_REG_FOCCFG   0x19u
#define CC1101_REG_AGCCTRL2 0x1Bu
#define CC1101_REG_AGCCTRL1 0x1Cu
#define CC1101_REG_AGCCTRL0 0x1Du
#define CC1101_REG_WORCTRL  0x20u
#define CC1101_REG_FREND1   0x21u
#define CC1101_REG_FREND0   0x22u

/* MDMCFG2[6:4] MOD_FORMAT */
#define CC1101_MOD_2FSK 0x00u
#define CC1101_MOD_GFSK 0x10u

/*
 * Channel filter bandwidth (MDMCFG4[7:4]): BW = 26 MHz / (8 * (4 + M) * 2^E).
 *   0xC0 = 101.6 kHz, 0xD0 = 81.25 kHz, 0xE0 = 67.7 kHz, 0xF0 = 58.0 kHz.
 * Override at build time with -DKISS_TNC_CHANBW=0x..
 */
#ifndef KISS_TNC_CHANBW
#define KISS_TNC_CHANBW 0xD0
#endif

/** Space needed for a preset buffer. */
#define CC1101_PRESET_MAX 48u

typedef enum {
    Cc1101ModGfsk = 0, /**< Gaussian-shaped 2-FSK, BT = 0.5 (default) */
    Cc1101Mod2Fsk = 1, /**< plain 2-FSK */
} Cc1101Mod;

/** DRATE_E for a modem mode and modulation (DRATE_M is always 131). */
uint8_t cc1101_drate_e(uint8_t mode, Cc1101Mod mod);

/** PATABLE[0] value for -10/0/5/10 dBm (anything else -> +10 dBm). */
uint8_t cc1101_patable(int8_t dbm);

/** Build the preset. Returns bytes written, or 0 if cap < CC1101_PRESET_MAX. */
size_t cc1101_build_preset(uint8_t* out, size_t cap, uint8_t mode, Cc1101Mod mod, int8_t pwr_dbm);

/** Look up a register value in a built preset. Returns false if absent. */
bool cc1101_preset_get(const uint8_t* preset, uint8_t reg, uint8_t* value);
