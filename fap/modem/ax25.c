#include "ax25.h"

#include <string.h>

static char upcase(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool is_alnum(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool ax25_parse_call(const char* text, char call[7], uint8_t* ssid) {
    size_t n = 0;
    *ssid = 0;
    memset(call, 0, 7);
    while(*text && *text != '-') {
        char c = upcase(*text++);
        if(!is_alnum(c) || n >= 6) return false;
        call[n++] = c;
    }
    if(n == 0) return false;
    if(*text == '-') {
        text++;
        if(!*text) return false;
        unsigned v = 0;
        size_t digits = 0;
        while(*text) {
            if(*text < '0' || *text > '9' || digits >= 2) return false;
            v = v * 10u + (unsigned)(*text++ - '0');
            digits++;
        }
        if(v > 15u) return false;
        *ssid = (uint8_t)v;
    }
    return true;
}

void ax25_encode_addr(uint8_t out[7], const char* call, uint8_t ssid, bool h_bit, bool last) {
    size_t i = 0;
    for(; i < 6 && call[i]; i++) out[i] = (uint8_t)(call[i] << 1);
    for(; i < 6; i++) out[i] = (uint8_t)(' ' << 1);
    // SSID byte: H/C | R R | SSID(4) | extension bit
    out[6] = (uint8_t)((h_bit ? 0x80u : 0u) | 0x60u | ((ssid & 0x0Fu) << 1) | (last ? 1u : 0u));
}

size_t ax25_build_ui(
    const char* src,
    const char* dst,
    const char* path,
    const uint8_t* info,
    size_t info_len,
    uint8_t* out,
    size_t cap) {
    char calls[2 + AX25_MAX_DIGIS][7];
    uint8_t ssids[2 + AX25_MAX_DIGIS];
    size_t naddr = 2;

    if(!ax25_parse_call(dst, calls[0], &ssids[0])) return 0;
    if(!ax25_parse_call(src, calls[1], &ssids[1])) return 0;

    if(path && *path) {
        const char* p = path;
        while(*p) {
            char item[16];
            size_t k = 0;
            while(*p && *p != ',') {
                if(k + 1 >= sizeof(item)) return 0;
                item[k++] = *p++;
            }
            item[k] = 0;
            if(*p == ',') p++;
            if(k == 0) continue;
            if(naddr >= 2 + AX25_MAX_DIGIS) return 0;
            if(!ax25_parse_call(item, calls[naddr], &ssids[naddr])) return 0;
            naddr++;
        }
    }

    size_t need = naddr * 7u + 2u + info_len;
    if(need > cap) return 0;

    size_t n = 0;
    for(size_t i = 0; i < naddr; i++) {
        // Command frame: C bit set on destination, clear on source (AX.25 v2).
        bool c_bit = (i == 0);
        ax25_encode_addr(&out[n], calls[i], ssids[i], c_bit, i == naddr - 1);
        n += 7;
    }
    out[n++] = AX25_CONTROL_UI;
    out[n++] = AX25_PID_NONE;
    if(info_len) memcpy(&out[n], info, info_len);
    n += info_len;
    return n;
}

/* --- formatting --- */

typedef struct {
    char* out;
    size_t cap;
    size_t n;
} Sb;

static void sb_putc(Sb* sb, char c) {
    if(sb->n + 1 < sb->cap) {
        sb->out[sb->n++] = c;
        sb->out[sb->n] = 0;
    }
}

static void sb_puts(Sb* sb, const char* s) {
    while(*s) sb_putc(sb, *s++);
}

static void sb_addr(Sb* sb, const uint8_t* a) {
    for(int i = 0; i < 6; i++) {
        char c = (char)(a[i] >> 1);
        if(c != ' ') sb_putc(sb, c);
    }
    uint8_t ssid = (a[6] >> 1) & 0x0Fu;
    if(ssid) {
        sb_putc(sb, '-');
        if(ssid >= 10) sb_putc(sb, '1');
        sb_putc(sb, (char)('0' + ssid % 10));
    }
}

bool ax25_format(const uint8_t* frame, size_t len, char* out, size_t cap) {
    Sb sb = {.out = out, .cap = cap, .n = 0};
    if(cap) out[0] = 0;

    // Find the end of the address field.
    size_t naddr = 0;
    bool ok = false;
    for(size_t i = 0; i + 7 <= len && naddr < 2 + AX25_MAX_DIGIS; i += 7) {
        naddr++;
        if(frame[i + 6] & 1u) {
            ok = naddr >= 2;
            break;
        }
    }
    if(!ok) {
        static const char hex[] = "0123456789ABCDEF";
        sb_puts(&sb, "?? ");
        for(size_t i = 0; i < len && i < 16; i++) {
            sb_putc(&sb, hex[frame[i] >> 4]);
            sb_putc(&sb, hex[frame[i] & 15]);
        }
        return false;
    }

    sb_addr(&sb, &frame[7]); // source
    sb_putc(&sb, '>');
    sb_addr(&sb, &frame[0]); // destination
    for(size_t i = 2; i < naddr; i++) {
        sb_putc(&sb, ',');
        sb_addr(&sb, &frame[i * 7]);
        if(frame[i * 7 + 6] & 0x80u) sb_putc(&sb, '*');
    }
    sb_putc(&sb, ':');

    size_t p = naddr * 7;
    if(p < len) {
        uint8_t control = frame[p++];
        bool has_pid = (control & 0xEFu) == AX25_CONTROL_UI || (control & 1u) == 0;
        if(has_pid && p < len) p++; // skip PID
        sb_putc(&sb, ' ');
        for(; p < len; p++) {
            char c = (char)frame[p];
            sb_putc(&sb, (c >= 0x20 && c < 0x7F) ? c : '.');
        }
    }
    return true;
}
