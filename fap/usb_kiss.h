/**
 * @file usb_kiss.h
 * KISS over USB CDC channel 1 (the Flipper CLI stays on channel 0).
 *
 * usb_kiss_start() saves the current USB config, switches to usb_cdc_dual and
 * re-enables the CLI on channel 0. usb_kiss_stop() restores the saved config.
 * One worker thread reads channel 1 and feeds the KISS decoder; the same
 * thread drains an outgoing byte stream in 64-byte CDC packets.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_KISS_CHANNEL 1

typedef struct UsbKiss UsbKiss;

/**
 * Called on the USB worker thread with each decoded KISS frame (frame[0] is the
 * type byte), or with (NULL, 0) when a frame too big for the decoder was discarded.
 */
typedef void (*UsbKissFrameCallback)(void* ctx, const uint8_t* frame, size_t len);

/** Returns NULL if the USB config could not be changed (e.g. USB locked by another app). */
UsbKiss* usb_kiss_start(UsbKissFrameCallback cb, void* ctx);
void usb_kiss_stop(UsbKiss* usb);

/** Queue one KISS frame (type byte + payload). Thread-safe. False if dropped (buffer full). */
bool usb_kiss_send(UsbKiss* usb, uint8_t type, const uint8_t* data, size_t len);

/** Queue two frames back to back, both or neither (used for "RX ..." status + data). */
bool usb_kiss_send_pair(
    UsbKiss* usb,
    uint8_t type_a,
    const uint8_t* a,
    size_t a_len,
    uint8_t type_b,
    const uint8_t* b,
    size_t b_len);

/** Host has the port open (DTR asserted) and the device is configured. */
bool usb_kiss_host_connected(UsbKiss* usb);

uint32_t usb_kiss_dropped(UsbKiss* usb);
/** Host bytes lost because the receive buffer was full. */
uint32_t usb_kiss_rx_dropped(UsbKiss* usb);
/** KISS frames from the host that were discarded for overflowing the decoder. */
uint32_t usb_kiss_rx_overflows(UsbKiss* usb);
