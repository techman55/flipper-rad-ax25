# Testing

Two kinds of testing: automated tests that run anywhere (done, results below), and bench
tests that need the Flipper and radios (procedures below, **not yet run**).

## Where this was built and tested

The firmware app, host tools and tests were written and run in a Linux cloud container
(Ubuntu 24.04, x86_64, gcc 13.3, Python 3.13, Swift 6.0.3, ufbt 0.2.6).
**No Flipper Zero, SDR or second radio was attached to that machine**, so:

* `info device` was **not** run. The firmware fork/version on the target Flipper is
  unknown, which is why FAPs were built for all three common firmwares (below).
* `ufbt launch` and every bench test in §3 were **not** run.
* The SwiftUI layer of the Mac app was not compiled (no macOS). Its core library was.

Nothing below claims a hardware result.

---

## 1. Automated tests (run, passing)

### 1.1 Native modem tests: `cd modem_tests && make test`

Plain `cc`, no Flipper headers, `-Wall -Wextra -Wshadow -Wconversion -Werror`.

```
test_afsk_tx: 9 checks, 0 failures
test_cc1101_preset: 95 checks, 0 failures
test_g3ruh: 103 checks, 0 failures
test_hdlc: 2749 checks, 0 failures
test_kiss_ax25: 1831 checks, 0 failures
  FSK9600   clean                              20/20 bursts decoded
  FSK9600   +2% skew, ±5us jitter, noise      40/40 bursts decoded
  FSK9600   -2% skew, ±5us jitter, noise      40/40 bursts decoded
  FSK9600   inverted, +1% skew, jitter, noise  20/20 bursts decoded
  FSK9600   3 frames/burst, +2%, jitter        15/15 bursts decoded
  FSK9600   heavy jitter, +0.5% skew, noise    30/30 bursts decoded
  AFSK1200  clean                              20/20 bursts decoded
  AFSK1200  +2% skew, ±5us jitter, noise      40/40 bursts decoded
  AFSK1200  -2% skew, ±5us jitter, noise      40/40 bursts decoded
  AFSK1200  inverted, +1% skew, jitter, noise  20/20 bursts decoded
  AFSK1200  3 frames/burst, +2%, jitter        15/15 bursts decoded
  AFSK1200  heavy jitter, +0.5% skew, noise    30/30 bursts decoded
  FSK1200   (same 6 cases)                     all decoded
test_loopback: 560 checks, 0 failures
ALL MODEM TESTS PASSED
```

What they cover:

| Test | Checks |
|---|---|
| FCS | `"123456789"` → `0x906E`; empty → `0x0000`; residue `0x0F47`; a real UI frame → `0x58CF` (same vector in the Python and Swift suites) |
| Bit stuffing | 300 random frames + all-`FF`, all-`7E`, all-`00`, five-ones across byte boundaries, exactly-five-ones, trailing `FF`; no run of 6 ones inside a stuffed body; abort, too-short, back-to-back with shared flag, corrupted bit → FCS bad, oversize dropped |
| NRZI | round trip, 0 toggles / 1 holds, decoder polarity-blind |
| G3RUH | round trip (50 seeds), self-sync from a random state within 17 bits, whitening of all-ones, inverted line inverts output |
| AFSK TX | mean tone 1200.0 / 2200.0 Hz within 0.5 Hz, every half-period within 1 µs of ideal, exact burst length, levels alternate and start high, edges within 0.0025 cycle of a continuously integrated phase (phase-continuous across bit boundaries) |
| Loopback | per mode: bytes → burst → `(level, duration)` → channel (±2 % clock skew, ±5 µs uniform edge jitter, 20 ms of random edges before/after, polarity inversion) → RX → identical bytes, FCS OK; max-length frame; 3 frames per burst; 5 s of pure noise yields no good frame; huge durations don't hang |
| CC1101 preset | every mode × {GFSK, 2-FSK}: MDMCFG2 = `14`/`04`, async serial, GDO0 data, deviation 2975.5 Hz, channel filter, programmed rate within 1 % of the line rate (FSK modes) / ≥ 4400 Bd (AFSK), list terminator + PATABLE |
| KISS / AX.25 | escape vector, 200 random streams, overflow recovery, callsign parsing, UI frame bytes, formatting |

**Margin sweep** (ad-hoc, 60 bursts of a 100-byte frame per point; percentage decoded):

| Mode | +2 % | +3 % | +4 % | +5 % skew (±5 µs jitter) | Jitter tolerance (no skew) |
|---|---|---|---|---|---|
| FSK9600 | 100 | 100 | 100 | 100 | 100 % at ±20 µs, falls off by ±30 µs (29 % of a bit) |
| AFSK1200 | 100 | 100 | 68 | 22 | 100 % at ±40 µs, fails by ±80 µs (half-periods cross the 322 µs threshold) |
| FSK1200 | 100 | 100 | 95 | 52 | 100 % at ±120 µs |

Real radios differ by tens of ppm, so ±2 % skew is very pessimistic. These are
idealised-channel numbers; the CC1101's own demodulator/slicer is not modelled.

The sweep caught a real AFSK RX weakness during development (tone changes placed up to
¼ bit off; ±40 µs jitter decoded only 20 %). Fixed by locating each tone change inside the
straddling half-period; a heavy-jitter case now guards it in `make test`.

### 1.2 Host tests: `pytest host/tests`

```
26 passed
```

KISS escaping/round-trip (random chunking), ACKMODE/SetHardware encoding, AX.25 address
encode/decode, SSID/H-bit, FCS vectors, frame round trip with path, status-line parsing,
the device protocol against a fake TNC (GET/configure/errors, RX status ↔ frame pairing,
ACKMODE, async errors not mistaken for replies), port selection, and the KISS-TCP bridge.

### 1.3 Mac app core: `cd mac/FlipperTNCCore && swift test`

```
Executed 11 tests, with 0 failures
```

Run on Linux with Swift 6.0.3. KISS, AX.25 (same byte vectors as C and Python), status
parsing, port classification, and an end-to-end test that drives `TNCConnection` +
`SerialPort` (termios) through a real pseudo-terminal.

The app itself (`mac/FlipperTNC.xcodeproj`, SwiftUI) **has not been compiled yet**: there is
no Xcode on Linux. The project file was generated with the `xcodeproj` gem and re-parsed to
check targets, sources, resources, the local package link, build settings and the shared
scheme. First `mac/build-app.sh` on a Mac may surface small SwiftUI type-check fixes.

### 1.4 Firmware build: `cd fap && ufbt`

Zero warnings with `-Wall -Wextra -Werror`, and `APPCHK` reports no unresolved symbols for
each SDK:

| Firmware | SDK | API | Output |
|---|---|---|---|
| Official | release 1.5.1 | 88.2 | `dist/kiss_tnc.fap` |
| Momentum | mntm-012 | 87.1 | `dist/momentum/kiss_tnc.fap` |
| Unleashed | unlshd-095 | 88.26 | `dist/unleashed/kiss_tnc.fap` |

All USB CDC (`furi_hal_cdc_*`, `usb_cdc_dual`, `cli_vcp_*`) and `subghz_devices_*` async
functions used are exported to FAPs in these SDKs (checked in `api_symbols.csv` first; the
only non-exported function found, `strtok_r`, was replaced).

---

## 2. Install on the Flipper (not yet run)

1. On the Flipper: **Settings → System → Device info** (or `info device` in the CLI) to read
   the firmware fork/version. Pick the matching FAP above.
2. Either copy it to `SD/apps/Sub-GHz/kiss_tnc.fap` with qFlipper, or rebuild and launch:

```bash
cd fap && ufbt update --channel=release && ufbt launch
```

For Momentum/Unleashed use `ufbt update --index-url=<fork index>` (see README) first. If the
Flipper says "API mismatch", the SDK doesn't match the firmware; update one or the other.

Note: `ufbt launch` starts the app over the CLI port; when the app switches USB to dual CDC
the device re-enumerates, so `ufbt` may report a dropped connection even though the app is
running. That is expected.

---

## 3. Bench tests (procedures; results pending)

Fill in the Result column when run. Record firmware version and app version for each run.

### 3.1 USB

| # | Procedure | Pass criteria | Result |
|---|---|---|---|
| U1 | Start app. `ls /dev/cu.usbmodemflip_*` | Two ports appear; write the exact names into README §Ports | NOT RUN |
| U2 | `fzkiss ports` | The second one is marked `TNC (KISS)` | NOT RUN |
| U3 | `fzkiss get` | `OK FREQ=... APP=1.0.0 FW=...` | NOT RUN |
| U4 | `fzkiss config --freq 433920000 --mode 0 --pwr 10` | `OK FREQ=433920000 TXOK=1 MODE=0 PWR=10`; Flipper screen updates | NOT RUN |
| U5 | `fzkiss config --freq 100000000` | `ERR FREQ_RANGE`; nothing changes | NOT RUN |
| U6 | With the app running: `screen /dev/cu.usbmodemflip_<Name>1 115200`, type `info device` | CLI answers on channel 0 | NOT RUN |
| U7 | Exit app (Back). `ls /dev/cu.usbmodemflip_*`; open CLI port | One port again; CLI works; qFlipper connects | NOT RUN |
| U8 | Restart app; `fzkiss get` | Settings from U4 persisted | NOT RUN |

### 3.2 TX (SDR)

| # | Procedure | Pass criteria | Result |
|---|---|---|---|
| T1 | Set `CALL` to your callsign. `fzkiss send --src <CALL> --dst TEST --text hello` with an SDR (e.g. SDR++/GQRX, 433.920 MHz, NFM) | `sent: ... (acked)`; burst on 433.92 MHz; ≈ 340 ms (mode 0); deviation ≈ ±3 kHz on the waterfall | NOT RUN |
| T2 | Mode 1: `fzkiss config --mode 1`, then `rtl_fm -f 433.92M -s 48k - \| direwolf -r 48000 -B 1200 -` and send again | Direwolf prints `<CALL>>TEST:hello` | NOT RUN |
| T3 | Mode 0: `rtl_fm -f 433.92M -M fm -s 48k -o 4 - \| direwolf -r 48000 -B 9600 -` (9600 needs a wide, flat discriminator; adjust `-s`/`-o` if needed) | Direwolf decodes | NOT RUN |
| T4 | `fzkiss config --freq 434900000` (outside 433.05–434.79 on official US region) then send | `ERR TX_BLOCKED_REGION`; nothing on the SDR | NOT RUN |
| T5 | OK button with `CALL=N0CALL` | Banner "Set CALL first", nothing transmitted | NOT RUN |
| T6 | GFSK check: mode 0, send a long frame (`--text` of ~250 chars) with `--mod GFSK`, then `--mod FSK`; compare on the SDR spectrum (max hold) | GFSK sidelobes visibly lower / narrower occupied bandwidth than 2-FSK; both decode in Direwolf (T3) | NOT RUN |

### 3.3 RX

| # | Procedure | Pass criteria | Result |
|---|---|---|---|
| R1 | Two Flippers, mode 0: A `fzkiss listen`, B `fzkiss send ...` | A prints the frame with RSSI | NOT RUN |
| R2 | Mode 1 from an HT/Direwolf at 433.92 MHz, 1200 AFSK | Frames decode (experimental) | NOT RUN |
| R3 | `fzkiss config --debug 1` near noise | Occasional `[FCS BAD]` lines only | NOT RUN |

### 3.4 Turnaround and stress

| # | Procedure | Pass criteria | Result |
|---|---|---|---|
| S1 | After any TX: `fzkiss get` → `TURN_US` | < 5000 µs | NOT RUN |
| S2 | Note `HEAP`. `fzkiss ping --src <CALL> --count 100 --interval 0` | 100/100 ACK, no hang | NOT RUN |
| S3 | `fzkiss get` again | `HEAP` back within a few hundred bytes of S2's value; `EDGE_OVF`, `USB_DROP`, `KISS_OVF` reasonable | NOT RUN |
| S4 | Exit and relaunch app 5× | USB restored each time, no crash | NOT RUN |
