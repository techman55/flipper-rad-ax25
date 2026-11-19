/*
 * Full loopback per mode:
 *   AX.25 bytes -> modem_build_burst -> modem_tx (level, duration)
 *   -> channel (clock skew, edge jitter, noise before/after)
 *   -> modem_rx -> identical bytes, FCS OK.
 */
#include <math.h>
#include <string.h>

#include "modem.h"
#include "testlib.h"

#define MAX_RX 16

typedef struct {
    uint8_t data[MAX_RX][AX25_MAX_FRAME];
    size_t len[MAX_RX];
    bool ok[MAX_RX];
    int n;
} RxLog;

static void on_frame(void* ctx, const uint8_t* frame, size_t len, bool ok) {
    RxLog* log = ctx;
    if(log->n < MAX_RX) {
        memcpy(log->data[log->n], frame, len);
        log->len[log->n] = len;
        log->ok[log->n] = ok;
        log->n++;
    }
}

static uint8_t line_bits[16384];

typedef struct {
    double skew; /**< relative clock error, e.g. +0.02 */
    double jitter_us; /**< uniform ± jitter on each edge time */
    bool noise; /**< random edges before and after the burst */
    bool invert; /**< invert received polarity */
} Channel;

/* Random short edges, like the CC1101 slicer on an empty channel. */
static void feed_noise(ModemRx* rx, uint32_t total_us, bool* level) {
    uint32_t t = 0;
    while(t < total_us) {
        uint32_t d = 5 + rnd() % 300;
        modem_rx_edge(rx, *level, d);
        *level = !*level;
        t += d;
    }
}

/* Push the TX stream through the channel into the receiver. */
static void transmit(ModemMode mode, size_t nbits, const Channel* ch, ModemRx* rx) {
    ModemTx tx;
    modem_tx_init(&tx, mode, line_bits, nbits, true);

    bool noise_level = false;
    if(ch->noise) feed_noise(rx, 20000, &noise_level);

    double t_ideal = 0.0; // TX time
    double prev_rx_edge = 0.0;
    bool level;
    uint32_t d;
    bool first = true;
    while(modem_tx_next(&tx, &level, &d)) {
        t_ideal += d;
        double edge = t_ideal * (1.0 + ch->skew) + ch->jitter_us * rnd_sym();
        double dur = edge - prev_rx_edge;
        if(first && ch->noise) dur += 50; // gap between noise and carrier
        first = false;
        if(dur < 1.0) dur = 1.0;
        uint32_t du = (uint32_t)lround(dur);
        modem_rx_edge(rx, level ^ ch->invert, du);
        prev_rx_edge += du;
    }
    if(ch->noise) feed_noise(rx, 20000, &noise_level);
}

static int run_case(ModemMode mode, const Channel* ch, int nframes, int trials, const char* label) {
    int failures = 0;
    static uint8_t frames[4][AX25_MAX_FRAME];
    for(int t = 0; t < trials; t++) {
        ModemFrame mf[4];
        for(int f = 0; f < nframes; f++) {
            size_t len = AX25_MIN_FRAME + rnd() % (AX25_MAX_FRAME - AX25_MIN_FRAME + 1);
            if(t == 0 && f == 0) len = AX25_MAX_FRAME; // always cover the max size once
            for(size_t i = 0; i < len; i++) frames[f][i] = (uint8_t)rnd();
            mf[f].data = frames[f];
            mf[f].len = len;
        }
        size_t nbits =
            modem_build_burst(mode, mf, (size_t)nframes, 100, 20, line_bits, sizeof(line_bits) * 8);
        CHECK(nbits > 0);

        RxLog log = {.n = 0};
        ModemRx rx;
        modem_rx_init(&rx, mode, on_frame, &log);
        transmit(mode, nbits, ch, &rx);

        int good = 0;
        for(int i = 0; i < log.n; i++) {
            if(!log.ok[i]) continue;
            for(int f = 0; f < nframes; f++) {
                if(log.len[i] == mf[f].len && memcmp(log.data[i], mf[f].data, mf[f].len) == 0) {
                    good++;
                    break;
                }
            }
        }
        if(good != nframes) failures++;
    }
    printf(
        "  %-9s %-34s %d/%d bursts decoded\n",
        modem_mode_name(mode),
        label,
        trials - failures,
        trials);
    return failures;
}

static void test_mode(ModemMode mode) {
    const Channel clean = {0};
    const Channel fast = {.skew = +0.02, .jitter_us = 5.0, .noise = true};
    const Channel slow = {.skew = -0.02, .jitter_us = 5.0, .noise = true};
    const Channel inv = {.skew = +0.01, .jitter_us = 5.0, .noise = true, .invert = true};

    CHECK(run_case(mode, &clean, 1, 20, "clean") == 0);
    CHECK(run_case(mode, &fast, 1, 40, "+2% skew, ±5us jitter, noise") == 0);
    CHECK(run_case(mode, &slow, 1, 40, "-2% skew, ±5us jitter, noise") == 0);
    CHECK(run_case(mode, &inv, 1, 20, "inverted, +1% skew, jitter, noise") == 0);
    CHECK(run_case(mode, &fast, 3, 15, "3 frames/burst, +2%, jitter") == 0);
}

static void test_flag_count(void) {
    // TXDELAY 30 (300 ms) at 9600 bps = 2880 bits = 360 flags; at 1200 bps = 45 flags.
    CHECK(modem_flags_for_ms(ModemModeFsk9600, 300) == 360);
    CHECK(modem_flags_for_ms(ModemModeAfsk1200, 300) == 45);
    CHECK(modem_flags_for_ms(ModemModeFsk1200, 0) == 1);
}

static void test_burst_too_big(void) {
    static uint8_t f[AX25_MAX_FRAME];
    ModemFrame mf = {.data = f, .len = sizeof(f)};
    uint8_t small[64];
    CHECK(modem_build_burst(ModemModeFsk9600, &mf, 1, 300, 20, small, sizeof(small) * 8) == 0);
}

static void test_fsk_start_high(void) {
    // Whatever the first line bit is, the generator must begin with a high level.
    for(int t = 0; t < 20; t++) {
        static uint8_t f[40];
        for(size_t i = 0; i < sizeof(f); i++) f[i] = (uint8_t)rnd();
        ModemFrame mf = {.data = f, .len = sizeof(f)};
        size_t nbits = modem_build_burst(
            ModemModeFsk9600, &mf, 1, (uint32_t)(rnd() % 50), 10, line_bits, sizeof(line_bits) * 8);
        ModemTx tx;
        modem_tx_init(&tx, ModemModeFsk9600, line_bits, nbits, true);
        bool level;
        uint32_t d;
        CHECK(modem_tx_next(&tx, &level, &d) && level);
        uint64_t total = d;
        while(modem_tx_next(&tx, &level, &d)) total += d;
        CHECK(total == modem_burst_duration_us(ModemModeFsk9600, nbits));
    }
}

static void test_noise_only(void) {
    // Pure noise must not crash and must essentially never yield a good-FCS frame.
    for(int m = 0; m < ModemModeCount; m++) {
        RxLog log = {.n = 0};
        ModemRx rx;
        modem_rx_init(&rx, (ModemMode)m, on_frame, &log);
        bool lv = false;
        feed_noise(&rx, 5000000, &lv); // 5 s
        int good = 0;
        for(int i = 0; i < log.n; i++) good += log.ok[i];
        CHECK_MSG(good == 0, "mode %d: %d good frames from noise", m, good);
        // Long silence (stuck level) must not misbehave either.
        modem_rx_edge(&rx, true, 10000000);
        modem_rx_edge(&rx, false, 4000000000u);
    }
}

int main(void) {
    test_flag_count();
    test_burst_too_big();
    test_fsk_start_high();
    for(int m = 0; m < ModemModeCount; m++) test_mode((ModemMode)m);
    test_noise_only();
    return test_summary("test_loopback");
}
