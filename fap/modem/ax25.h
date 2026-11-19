/**
 * @file ax25.h
 * Minimal AX.25 helpers for the on-device GUI and the test beacon:
 * callsign parsing, UI-frame building, and "SRC>DST,PATH: info" formatting.
 * Pure C, no Flipper dependencies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AX25_CONTROL_UI 0x03u
#define AX25_PID_NONE   0xF0u
#define AX25_MAX_DIGIS  8u

/**
 * Parse "CALL" or "CALL-SSID" (1-6 alphanumerics, SSID 0-15, case-insensitive).
 * @param call  out: upper-case callsign, NUL-terminated (7 bytes)
 * @return false if malformed
 */
bool ax25_parse_call(const char* text, char call[7], uint8_t* ssid);

/** Encode one 7-byte address field. `h_bit` is the has-been-repeated / C bit (bit 7 of SSID byte). */
void ax25_encode_addr(uint8_t out[7], const char* call, uint8_t ssid, bool h_bit, bool last);

/**
 * Build a UI frame (control 0x03, PID 0xF0) without FCS.
 * @param path  comma-separated digipeaters ("WIDE1-1,WIDE2-1"), or NULL/"" for none
 * @return frame length, or 0 on a bad callsign / not enough room
 */
size_t ax25_build_ui(
    const char* src,
    const char* dst,
    const char* path,
    const uint8_t* info,
    size_t info_len,
    uint8_t* out,
    size_t cap);

/**
 * Format a frame as "SRC>DST,DIGI*,DIGI: info" (non-printables shown as '.').
 * Always NUL-terminates when cap > 0. Returns false if the address field is malformed
 * (the output then holds a hex dump prefix instead).
 */
bool ax25_format(const uint8_t* frame, size_t len, char* out, size_t cap);
