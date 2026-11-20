# Flipper KISS TNC — Protocol (as built)

This is the host ↔ device protocol implemented by `fap/` (app version 1.0.0), plus the
over-the-air format and the CC1101 register settings. Everything here matches the code;
where the code and this file disagree, the code wins and this file has a bug.

---

## 1. Transport

| Item | Value |
|---|---|
| USB mode while the app runs | `usb_cdc_dual` (VID `0483`, PID `5740`) |
| CDC channel 0 (USB interface 0) | Flipper CLI, unchanged |
| CDC channel 1 (USB interface 2) | **KISS TNC** |
| Baud rate | Ignored (USB CDC). Open at 115200. |
| Flow control | None |
| On app exit | Previous USB config is restored (normally single-CDC CLI) |

Expected device names (derived from the USB descriptors and OS naming rules; **not yet
observed on real hardware**, see `docs/TESTING.md`):

| OS | CLI (ch0) | TNC (ch1) |
|---|---|---|
| macOS | `/dev/cu.usbmodemflip_<Name>1` | `/dev/cu.usbmodemflip_<Name>3` |
| Linux | `/dev/ttyACM0` | `/dev/ttyACM1` (`/sys/class/tty/ttyACM1/device/bInterfaceNumber` = `02`) |

`fzkiss ports` and the Mac app pick the higher-numbered interface automatically.

Opening the port asserts DTR. The Flipper uses DTR only for the "USB: yes/no" indicator;
it queues output (4 KiB) whether or not DTR is set and drops output when that buffer is
full (counted as `USB_DROP` in `GET`).

---

## 2. KISS framing

Standard KISS. One frame = `FEND`, type byte, payload, `FEND`, with escaping:

| Name | Byte | Meaning |
|---|---|---|
| FEND | `C0` | frame delimiter |
| FESC | `DB` | escape |
| TFEND | `DC` | after FESC: literal `C0` |
| TFESC | `DD` | after FESC: literal `DB` |

Type byte: high nibble = port (**only port 0 is used; frames for other ports are ignored**),
low nibble = command. `FF` (RETURN) is the whole byte.

| Cmd | Name | Host → device | Notes |
|---|---|---|---|
| `0` | DATA | AX.25 frame, **no FCS** | Device adds FCS on TX. 15–330 bytes. |
| `1` | TXDELAY | 1 byte, 10 ms units | Default 30 (300 ms of flags) |
| `2` | P | 1 byte | Persistence, p = (P+1)/256. Default 63 |
| `3` | SLOTTIME | 1 byte, 10 ms units | Default 10 (100 ms) |
| `4` | TXTAIL | 1 byte, 10 ms units | Default 2 (20 ms of flags) |
| `5` | FULLDUPLEX | 1 byte | Ignored (half-duplex only) |
| `6` | SETHARDWARE | ASCII line | Vendor control, §4 |
| `C` | ACKMODE | `seq_hi seq_lo` + AX.25 | §3.2 |
| `FF` | RETURN | — | Ignored |

KISS parameter commands (1–4) take effect immediately and are **not** persisted. The same
values set through SetHardware (`TXDELAY=`, `P=`, …) **are** persisted.

Host frames that overflow the device's 400-byte KISS buffer are discarded and reported as
`ERR FRAME_TOO_LONG` (and counted as `KISS_OVF`).

---

## 3. Data

### 3.1 Host → device (transmit)

`C0 00 <ax25 without FCS> C0`. Frames are queued (depth 8) and sent as one burst per
channel access (§6). Errors are reported as SetHardware lines (§5):
`ERR FRAME_TOO_LONG` (> 330 bytes), `ERR FRAME_TOO_SHORT` (< 15 bytes),
`ERR QUEUE_FULL`.

After the burst: `TXDONE SEQ=- DUR=<ms>` for each plain DATA frame.

### 3.2 ACKMODE (Mobilinkd / BPQ extension)

Host: `C0 0C <seq_hi> <seq_lo> <ax25 without FCS> C0`.

After that frame has been transmitted the device sends **both**:

1. `C0 0C <seq_hi> <seq_lo> C0` — the standard ACKMODE ack, and
2. `TXDONE SEQ=<seq as decimal> DUR=<ms>` — SetHardware status.

Errors for an ACKMODE frame carry the sequence number: `ERR BUSY_TIMEOUT SEQ=7`.
A frame that fails gets no `0C` ack.

### 3.3 Device → host (receive)

Every forwarded frame is sent as **two consecutive KISS frames**, written atomically:

1. SetHardware status: `RX RSSI=<dBm> FREQ=<Hz> MODE=<0-2> FCS=<OK|BAD> T=<ms since boot>`
2. DATA: `C0 00 <ax25 without FCS> C0`

Frames with a bad FCS are only forwarded when `DEBUG=1` (then with `FCS=BAD`). Frames
shorter than 15 bytes or not byte-aligned are never forwarded. Standard KISS clients see
plain DATA frames and ignore the command-6 lines.

---

## 4. Vendor control: SetHardware (`0x06`)

Payload: one ASCII line `KEY=VALUE[;KEY=VALUE...]` (≤ 199 bytes, keys case-insensitive,
spaces around items ignored). The line is validated as a whole: if any item is invalid the
device replies `ERR ...` and applies **nothing**.

| Key | Values | Effect |
|---|---|---|
| `FREQ` | Hz, 300000000–348000000, 387000000–464000000, 779000000–928000000 | Retune now, stay in RX. Out of range → `ERR FREQ_RANGE`. A frequency the region forbids for TX is accepted for RX; reply says `TXOK=0`. |
| `MODE` | `0` FSK9600, `1` AFSK1200, `2` FSK1200 | Reload preset, restart RX |
| `MOD` | `GFSK` (default), `FSK` | CC1101 modulation: Gaussian-filtered 2-FSK (BT 0.5) or plain 2-FSK. Reload preset. Applies to all modes. |
| `PWR` | `-10`, `0`, `5`, `10` (dBm) | Reload preset |
| `RADIO` | `INT`, `EXT` | Switch CC1101. EXT not detected → falls back to INT, reply contains `EXT_NOT_FOUND`. 5 V OTG is on only while EXT is in use. |
| `SQL` | dBm, −140…−20 | RSSI squelch for RX **and** carrier-sense threshold for CSMA |
| `DEBUG` | `0`, `1` | Forward bad-FCS frames |
| `CALL` | `CALL[-SSID]` | Callsign used by the on-device test beacon |
| `TXDELAY` `P` `SLOTTIME` `TXTAIL` | 0–255 | As the KISS commands, but persisted |
| `TXONCE` | Hz (same ranges as FREQ) | Next burst only goes out on this frequency, then back to `FREQ` |
| `GET` | (no value) | Full status line, see below |

Settings (all of the above except `TXONCE`) are saved to
`/ext/apps_data/kiss_tnc/settings.txt` after each successful change and loaded at start.

**Replies.** Exactly one reply per line:

* `OK` followed by each changed key's new value, e.g.
  `OK FREQ=433920000 TXOK=1 MODE=0`.
* `ERR <CODE> [detail]`, e.g. `ERR BAD_VALUE PWR`.

`GET` alone is answered immediately. `GET` combined with changes (`MODE=1;GET`) gives the
`OK ...` line first, then the `GET` line.

Changes are applied by the radio worker. If it is busy in CSMA/TX (up to 10 s) the reply
waits until it is free.

**GET reply** (single line, wrapped here):

```
OK FREQ=433920000 MODE=0 MOD=GFSK PWR=10 RADIO=INT SQL=-95 DEBUG=0 CALL=N0CALL
   TXDELAY=30 P=63 SLOTTIME=10 TXTAIL=2
   STATE=RX RSSI=-104 RXF=0 TXF=0 BADF=0
   EDGE_OVF=0 USB_DROP=0 USB_RX_DROP=0 KISS_OVF=0 TXQ_FULL=0 TURN_US=0
   HEAP=81234 HEAP_START=96512 HEAP_MIN=80100 REGION=US TXOK=1
   APP=1.0.0 FW=1.5.1
```

| Field | Meaning |
|---|---|
| `RXF` / `TXF` / `BADF` | good frames received / frames transmitted / bad-FCS frames seen |
| `EDGE_OVF` | captured edges dropped because the ISR→demod buffer was full |
| `USB_DROP` | device→host output dropped (host not reading) |
| `USB_RX_DROP` | host→device bytes dropped (receive buffer full) |
| `KISS_OVF` | host→device KISS frames dropped for overflowing the decoder |
| `TXQ_FULL` | frames refused with `ERR QUEUE_FULL` |
| `TURN_US` | last measured TX-complete → RX-running time |
| `HEAP`, `HEAP_START`, `HEAP_MIN` | free heap now, at app start, all-time minimum |
| `REGION`, `TXOK` | region name and whether `FREQ` is TX-allowed there |

---

## 5. Device → host status lines (SetHardware `0x06`)

| Line | When |
|---|---|
| `RX RSSI=<dBm> FREQ=<Hz> MODE=<n> FCS=<OK\|BAD> T=<ms>` | Right before every forwarded data frame |
| `TXDONE SEQ=<n\|-> DUR=<ms>` | After every transmitted frame (one per frame in a burst) |
| `HB STATE=<RX\|CSMA\|TX> RSSI=<dBm>` | Every 5 s |
| `OK ...` / `ERR ...` | Reply to a SetHardware line |
| `CFG MODE=1` / `CFG FREQ=... TXOK=1` | Setting changed with the Flipper's own buttons |

Error codes:

| Code | Cause |
|---|---|
| `TX_BLOCKED_REGION` | Frequency not TX-allowed in the Flipper's region. Nothing is sent. |
| `FREQ_RANGE` | Frequency outside the CC1101 bands |
| `FRAME_TOO_LONG` | AX.25 frame > 330 bytes, or KISS frame > 400 bytes |
| `FRAME_TOO_SHORT` | AX.25 frame < 15 bytes |
| `QUEUE_FULL` | 8 frames already waiting |
| `BUSY_TIMEOUT` | No clear channel within 10 s. All queued frames are dropped. |
| `RADIO_INIT` | `EXT_NOT_FOUND` at startup: external module configured but not detected; running on INT |
| `BAD_VALUE <KEY>`, `UNKNOWN_KEY <KEY>`, `MISSING_VALUE <KEY>`, `LINE_TOO_LONG`, `EMPTY`, `BUSY` | SetHardware parse errors (`BUSY`: radio command queue full) |

Frame-related errors carry `SEQ=<n>` when the frame came in via ACKMODE. A host should
treat `OK` and `ERR` **without** `SEQ` as the reply to its last SetHardware line.

---

## 6. Channel access and timing

```
INIT -> RX (continuous)
RX   --frame queued-->  CSMA
CSMA --clear & rand<=P--> TX: [TXDELAY flags][frame][flag][frame]...[TXTAIL flags]
TX   --complete-->       RX immediately; then ACK/TXDONE to the host
```

* **CSMA:** every SLOTTIME: sample RSSI; if `RSSI < SQL` and a random byte ≤ P → transmit,
  else wait one slot. After 10 s without a clear channel: `ERR BUSY_TIMEOUT` for every
  queued frame.
* **Burst:** all frames waiting at that moment (up to 8) go out in one keyed burst,
  separated by a single shared flag.
* **Turnaround:** the radio worker sleeps until ~2 ms before the computed end of the burst,
  then polls every 50 µs. On completion it stops async TX and restarts async RX. The
  measured time is in `GET` as `TURN_US`. (Design target < 5 ms; CC1101 calibration on
  IDLE→RX is ~0.8 ms.)
* **Squelch:** RSSI is sampled every 5 ms while in RX. The capture ISR forwards edges only
  while RSSI ≥ SQL (3 dB hysteresis when closing). Closing the squelch resets the demodulator.
* **Never transmits on its own.** Only host frames, or the OK-button beacon once `CALL` is
  not `N0CALL`.

---

## 7. Over the air

All modes: AX.25 → HDLC (flags `7E` unstuffed, data LSB first, a 0 inserted after five 1s,
FCS = CRC-16/X.25 sent low byte first) → NRZI (0 = transition, 1 = none).

| Mode | Line coding after NRZI | On the CC1101 (async serial, GDO0 = data) |
|---|---|---|
| 0 FSK9600 | G3RUH scrambler x¹⁷+x¹²+1 (Direwolf convention) | GDO0 = line bit, 9600 bit/s, ±2.98 kHz deviation. |
| 1 AFSK1200 | none | GDO0 toggles at the tone rate: mark 1200 Hz for a 1, space 2200 Hz for a 0, 1200 baud, phase-continuous. An FM receiver's discriminator recovers the Bell 202 audio. |
| 2 FSK1200 | none | As mode 0 at 1200 bit/s, unscrambled |

**Modulation (`MOD`).** Default is **GFSK**: the CC1101's Gaussian filter (BT = 0.5) shapes
the frequency transitions, which narrows the occupied bandwidth compared with plain 2-FSK.
In async serial mode the CC1101 oversamples GDO0 at 8× the programmed data rate and the
Gaussian filter is referenced to that rate, which is why the programmed rate tracks the line
bit rate for each mode (§9). Receive is identical for GFSK and 2-FSK (same demodulator), so
a GFSK transmitter is received fine by a 2-FSK receiver and vice versa.

* Modes 0 and 2: true 2-GFSK at 9600 / 1200 bit/s.
* Mode 1: the "data" is the 1200/2200 Hz square wave; with GFSK the rate is programmed to
  9.6 kBd so the filter (−3 dB ≈ 4.8 kHz) passes the 2200 Hz fundamental and rounds off the
  square wave's harmonics, i.e. closer to clean sine-tone FM.

GFSK in **async** mode is per the CC1101 datasheet but is **not yet verified on hardware**:
check the spectrum on an SDR (TESTING T6). If it misbehaves, `MOD=FSK` restores plain 2-FSK.

FCS check vector: `"123456789"` → `0x906E`.

Approximate burst lengths for the 21-byte frame in §8 with TXDELAY 30 / TXTAIL 2:
FSK9600 3256 bits = 339 ms; AFSK1200 and FSK1200 568 bits = 473 ms.

### TX generation

The whole burst is built **before keying up** into a packed line-bit buffer (12 KiB = 96 kbit;
worst case TXDELAY=TXTAIL=255 with eight 330-byte frames at 9600 ≈ 75 kbit). The async-TX
DMA refill ISR then turns those bits into `LevelDuration`s with integer-only arithmetic:

* FSK: bit *k* starts at `round(k·10⁶/baud)` µs; equal bits merge into one run.
* AFSK: a DDS on the 1 µs timer grid. 40-bit phase accumulator carried across bit
  boundaries; each edge is at the first µs tick after a half-cycle crossing; edge times are
  absolute, so rounding never accumulates.

The firmware's async-TX middleware skips leading *low* levels, so FSK bursts are inverted if
needed to start high (NRZI and G3RUH are polarity-blind).

### RX

ISR packs `(level, duration)` into a `uint32` (bit 31 = level) and pushes it into a stream
buffer (2048 edges). The demod thread:

* FSK: integrate-and-dump per bit window with a DPLL (phase gain 0.3, frequency gain 0.01,
  ±5% limit). Runs < ½ bit don't steer the loop (noise). ≤ 32 bits per run.
* AFSK (experimental): classify each half-period against 322 µs (mark ≈ 417, space ≈ 227),
  place each tone change inside the half-period that straddles it (phase continuity),
  then the same integrate-and-dump/DPLL at 1200 baud.
* Then G3RUH descramble (mode 0), NRZI decode, HDLC deframe, FCS check.

---

## 8. Byte-level examples

AX.25 UI frame `KN4XXX-1>TEST: hello` (21 bytes, no FCS):

```
A8 8A A6 A8 40 40 E0   dest  "TEST  " SSID 0, C bit set
96 9C 68 B0 B0 B0 63   src   "KN4XXX" SSID 1, last address
03 F0                  control UI, PID no layer 3
68 65 6C 6C 6F         "hello"
```

FCS on the air (added by the device): `4C 39`.

| What | Bytes |
|---|---|
| Send (DATA) | `C0 00 A8 8A A6 A8 40 40 E0 96 9C 68 B0 B0 B0 63 03 F0 68 65 6C 6C 6F C0` |
| Send (ACKMODE, seq 0x0102) | `C0 0C 01 02 A8 8A … 6F C0` |
| Device ack | `C0 0C 01 02 C0` |
| Device status after TX | `C0 06` `TXDONE SEQ=258 DUR=341` `C0` |
| Set TXDELAY = 30 | `C0 01 1E C0` |
| `GET` | `C0 06 47 45 54 C0` |
| `FREQ=433920000;MODE=0` | `C0 06 46 52 45 51 3D 34 33 33 39 32 30 30 30 30 3B 4D 4F 44 45 3D 30 C0` |
| Reply | `C0 06` `OK FREQ=433920000 TXOK=1 MODE=0` `C0` |
| RX status line | `C0 06 52 58 20 52 53 53 49 3D 2D 38 37 20 …` (`RX RSSI=-87 FREQ=433920000 MODE=0 FCS=OK T=123456`) `C0` |
| Escaping: data `41 C0 DB 42` | `C0 00 41 DB DC DB DD 42 C0` |

---

## 9. CC1101 configuration

Loaded with `subghz_devices_load_preset(dev, FuriHalSubGhzPresetCustom, regs)`, starting
from the firmware's `2FSK Dev2.38kHz Async` preset. Crystal f<sub>xosc</sub> = 26 MHz.
Source: `cc1101_build_preset()` in `fap/modem/cc1101_preset.c` (pure C, unit-tested in
`modem_tests/test_cc1101_preset.c`).

| Reg | Addr | Value | Meaning / calculation |
|---|---|---|---|
| IOCFG0 | `02` | `0D` | GDO0 = asynchronous serial data (in on TX, out on RX) |
| FSCTRL1 | `0B` | `06` | IF = 26 MHz / 2¹⁰ × 6 = 152.3 kHz |
| PKTCTRL0 | `08` | `32` | Asynchronous serial mode, infinite length, no CRC, no whitening |
| PKTCTRL1 | `07` | `04` | (unused in async) |
| MDMCFG0 | `14` | `00` | (channel spacing, unused) |
| MDMCFG1 | `13` | `02` | CHANSPC_E = 2, no FEC (unused in async) |
| MDMCFG2 | `12` | `14` / `04` | MOD_FORMAT `001` **GFSK** (default) / `000` 2-FSK; DC blocking filter on, no Manchester, no sync word |
| MDMCFG3 | `11` | `83` | DRATE_M = 131 |
| MDMCFG4 | `10` | `D8` / `D8`·`D7` / `D5` | CHANBW `D` (E=3, M=1) + DRATE_E per mode/modulation, see below |
| DEVIATN | `15` | `07` | E=0, M=7: f<sub>dev</sub> = 26e6 / 2¹⁷ × (8+7) × 2⁰ = **2975.5 Hz** |
| MCSM0 | `18` | `18` | Autocalibrate on IDLE→RX/TX, PO_TIMEOUT 149–155 µs |
| FOCCFG | `19` | `16` | Freq. offset compensation on: pre-K 3K, post-K K/2, limit ±BW/4 |
| AGCCTRL0 | `1D` | `91` | Medium hysteresis, 16-sample channel filter, normal AGC |
| AGCCTRL1 | `1C` | `00` | Relative carrier sense off |
| AGCCTRL2 | `1B` | `07` | All gain settings, 42 dB target |
| WORCTRL | `20` | `FB` | (as stock preset) |
| FREND0 | `22` | `10` | PA_POWER index 0 → PATABLE[0] |
| FREND1 | `21` | `56` | (as stock preset) |

**Data rate** R = (256 + DRATE_M) × 2^DRATE_E × 26e6 / 2²⁸, DRATE_M = 131:

| Mode | DRATE_E | R | MDMCFG4 |
|---|---|---|---|
| 0 FSK9600 | 8 | 9595.9 Bd | `D8` |
| 1 AFSK1200, GFSK | 8 | 9595.9 Bd (Gaussian −3 dB ≈ 4.8 kHz: keeps the 2200 Hz tone) | `D8` |
| 1 AFSK1200, 2-FSK | 7 | 4797.9 Bd (above the 2200 Hz tone's 4400 transitions/s) | `D7` |
| 2 FSK1200 | 5 | 1199.5 Bd | `D5` |

In async mode the chip does no clock recovery. The rate sets the 8× input oversampling (TX
edge timing resolution: 13 µs at 9.6 kBd, 104 µs at 1.2 kBd, always ≤ 1/8 bit), the
Gaussian filter (GFSK), and the demodulator's data filter on RX.

**Channel filter** BW = 26e6 / (8 × (4 + M) × 2^E), set by `KISS_TNC_CHANBW` in
`fap/modem/cc1101_preset.h`:

| `KISS_TNC_CHANBW` | E, M | BW |
|---|---|---|
| `0xC0` | 3, 0 | 101.6 kHz |
| **`0xD0`** (default) | 3, 1 | **81.25 kHz** |
| `0xE0` | 3, 2 | 67.7 kHz |
| `0xF0` | 3, 3 | 58.0 kHz |

81 kHz covers a 9600-baud signal (≈ 2·3 + 9.6 = 15.6 kHz) plus ±20 ppm crystal error on two
radios at 433 MHz (±17 kHz) with room to spare.

**PATABLE[0]** (values from the firmware's own 2-FSK preset table, 433 MHz column):

| PWR | PATABLE |
|---|---|
| −10 dBm | `34` |
| 0 dBm | `60` |
| +5 dBm | `84` |
| +10 dBm (default) | `C0` |

Frequency registers are written by `subghz_devices_set_frequency()`:
FREQ = f × 2¹⁶ / 26e6 (433.920 MHz → `0x10B071` → 433.919830 MHz actual).
