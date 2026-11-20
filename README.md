# Flipper KISS TNC

Turns a Flipper Zero into a half-duplex **AX.25 packet radio TNC**, driven over USB.

* The Flipper **listens by default** and reports every received frame to the host.
* When the host sends a frame, the Flipper transmits it on the configured frequency and
  modulation, then goes **straight back to listening**.
* The host side speaks standard **KISS**, so Xastir, YAAC, Direwolf-style clients and Python
  `kiss`/`aprs` libraries work unchanged. The Flipper CLI keeps working on the other USB
  channel.

Included:

| Path | What |
|---|---|
| `fap/` | Flipper app (`kiss_tnc.fap`), ufbt project |
| `fap/modem/` | Pure-C modem: HDLC, NRZI, G3RUH, FSK and AFSK TX/RX, KISS, AX.25. No Flipper headers. |
| `modem_tests/` | Native unit + loopback tests for the modem (`make test`) |
| `host/` | `fzkiss` Python CLI/library (pyserial) |
| `mac/FlipperTNC.xcodeproj` | Mac app Xcode project (`mac/FlipperTNC/` app sources + icon, `mac/FlipperTNCCore/` protocol/serial Swift package) |
| `dist/` | Prebuilt FAPs |
| `docs/PROTOCOL.md` | Wire protocol, over-the-air format, CC1101 registers |
| `docs/TESTING.md` | What was tested, results, and the bench procedure |

> **Status:** the modem, host tools and builds are tested (see `docs/TESTING.md`), but
> **nothing has been run on a real Flipper or radio yet**. Treat the first bench session as
> bring-up.

---

## Modes

| Mode | Name | Over the air | Works with | Status |
|---|---|---|---|---|
| 0 | `FSK9600` (default) | 2-GFSK 9600 bit/s, ±3 kHz, NRZI + G3RUH scrambler, HDLC | Another Flipper/CC1101, RFM9x in FSK mode, 9600-baud G3RUH packet (Direwolf `-B 9600`) | Primary |
| 1 | `AFSK1200` | Bell 202 (1200/2200 Hz, 1200 baud) as Gaussian-smoothed square-wave FM, NRZI, HDLC | Standard 1200-baud packet/APRS receivers (HTs, Direwolf) | TX ready; **RX experimental** |
| 2 | `FSK1200` | As mode 0 at 1200 bit/s, no scrambler | Another Flipper/CC1101 | Extra |

Default: mode 0, **GFSK**, **433.920 MHz**, +10 dBm. Max frame 330 bytes (AX.25 without FCS).

**Modulation:** all modes transmit **2-GFSK** by default (CC1101 Gaussian filter, BT 0.5),
which narrows the signal compared with plain 2-FSK. `fzkiss config --mod FSK` switches to
plain 2-FSK. Receiving is the same either way, so GFSK and 2-FSK stations hear each other.
GFSK in the CC1101's async mode follows the datasheet but hasn't been checked on an SDR yet
(`docs/TESTING.md` T6).

Mode 1 RX listens to the CC1101's sliced FM output and times tone half-periods. It decodes
cleanly in simulation, but the real slicer on a real AFSK signal is untested. Use it, but
don't rely on it until it has been bench-tested.

---

## Firmware requirements

The app only loads if built against the same API version as the firmware. Prebuilt FAPs:

| Your firmware | FAP | Built against |
|---|---|---|
| Official | `dist/kiss_tnc.fap` | release 1.5.1 (API 88.2) |
| Momentum | `dist/momentum/kiss_tnc.fap` | mntm-012 (API 87.1) |
| Unleashed | `dist/unleashed/kiss_tnc.fap` | unlshd-095 (API 88.26) |

Check yours on the Flipper under **Settings → System → Device info**, or run `info device`
in the CLI. If the Flipper says *API mismatch*, rebuild against your firmware (below).

Everything the app needs (USB dual CDC, `subghz_devices_*` async TX/RX, region check, 5 V
OTG) is exported to FAPs, so no custom firmware is required.

---

## Frequencies and regions

* CC1101 tuning ranges: **300–348, 387–464, 779–928 MHz**. Anything else gets
  `ERR FREQ_RANGE` and is never sent to the radio (the firmware would crash on it).
* Every transmission is gated by the Flipper's region table. If the frequency isn't TX-allowed,
  the app **does not transmit** and reports `ERR TX_BLOCKED_REGION` (and shows it on screen).
* **US region, official firmware:** 433.05–434.79 MHz is TX-allowed. That's inside the
  70 cm amateur band, so the default 433.920 MHz works without unlocking anything. Other
  70 cm frequencies need Momentum or Unleashed with the extended range enabled.
* This is meant for licensed amateur use. The AX.25 source callsign is your station ID.
  The app never transmits on its own: only frames from the host, or the OK-button test
  beacon, which refuses to send while the callsign is still `N0CALL`.

---

## Quick start

### 1. Install the app

Copy the right FAP from `dist/` to `SD Card/apps/Sub-GHz/` with qFlipper, or build and
install in one go (Flipper connected over USB, qFlipper closed):

```bash
pip install --upgrade ufbt
```

```bash
cd fap && ufbt update --channel=release && ufbt launch
```

For Momentum or Unleashed, point ufbt at the fork's SDK first:

```bash
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json
```

```bash
ufbt update --index-url=https://up.unleashedflip.com/directory.json
```

`ufbt launch` may report a lost connection once the app starts: the app switches USB to
dual CDC and the Flipper re-enumerates. That's expected.

### 2. Connect

Start **Apps → Sub-GHz → KISS TNC** on the Flipper. It shows *Listening…*.

#### Ports

While the app runs, the Flipper exposes two serial ports. Expected names (from the USB
descriptors; confirm on first bench run, `docs/TESTING.md` U1):

| | macOS | Linux |
|---|---|---|
| Flipper CLI | `/dev/cu.usbmodemflip_<Name>1` | `/dev/ttyACM0` |
| **TNC** | `/dev/cu.usbmodemflip_<Name>3` | `/dev/ttyACM1` |

`<Name>` is your Flipper's name. The TNC is always the second one. Open it at 115200
(the rate is ignored).

### 3. Host tools

```bash
pip install -e host/
```

```bash
fzkiss ports
```

```bash
fzkiss config --call KN4XXX-1 --freq 433920000 --mode 0 --mod GFSK --pwr 10
```

```bash
fzkiss listen
```

In another terminal:

```bash
fzkiss send --src KN4XXX-1 --dst TEST --path WIDE1-1 --text "hello"
```

More: `fzkiss get` (all settings + counters), `fzkiss listen --json`,
`fzkiss ping --src KN4XXX-1 --count 10 --interval 2`, `fzkiss send --hex <ax25 hex>`,
and `fzkiss tcp --port 8001` (KISS-over-TCP for Xastir/YAAC/etc.). If auto-detection picks
the wrong serial port, name it before the command: `fzkiss -p /dev/cu.usbmodemflip_<Name>3 listen`.

### 4. Mac app

Needs macOS 14+ and Xcode 15+ (the full Xcode, not just the Command Line Tools). No Apple
developer account: the app is signed "to run locally".

Build an installable `FlipperTNC.app` and put it in `/Applications`:

```bash
mac/build-app.sh --install
```

Without `--install` it only builds `mac/dist/FlipperTNC.app`. Rerun the same command after
pulling changes to update the installed copy. If `xcodebuild` says it needs Xcode, point the
tools at it once:

```bash
sudo xcode-select -s /Applications/Xcode.app
```

To work on it in Xcode instead: `open mac/FlipperTNC.xcodeproj`, pick the **FlipperTNC**
scheme, Run (⌘R). Product → Archive also works if you'd rather export the `.app` from
Xcode's Organizer.

It finds the TNC port automatically and reconnects after a replug. It has settings
(frequency, mode, GFSK/2-FSK, power, internal/external radio, squelch, callsign) with the device's
`OK`/`ERR` reply shown, a compose panel (text or raw hex, sent with ACKMODE so each row goes
*queued → sent ✓*), a filterable live log with hex view and `.jsonl`/`.txt` export, a status
bar (connection, device state, heartbeat age, RX/TX counts), and an optional KISS-TCP server
on port 8001.

The app is not sandboxed (it needs to list and open `/dev/cu.*` serial devices). Its icon
is generated by `mac/tools/make_icon.py`. The project file is generated by
`mac/tools/generate_xcodeproj.rb` (`gem install xcodeproj`), so changes to targets or build
settings belong there, though editing in Xcode works too.

> The SwiftUI layer hasn't been compiled on a Mac yet (written in a Linux environment; only
> the core library was compiled and tested). Expect possible minor fixes on first build.

---

## On the Flipper

```
((ψ)) KISS TNC               RX
FSK9600 GFSK 433.920 MHz
RSSI -104  USB:yes  INT
RX 3  TX 1  BAD 0
──────────────────────────────
KN4XXX-1>TEST: hello
...
```

| Button | Action |
|---|---|
| Back | Exit (stops the radio, restores USB) |
| OK | Send test beacon `<CALL>>TEST: flipper kiss tnc test` (refused while CALL is N0CALL) |
| Left / Right | Change mode |
| Up / Down | Step frequency ±25 kHz, then OK to tune or Back to cancel |

The app's icon is a 10×10 pixel antenna (`fap/icons/kiss_10px.png`): it's the menu icon
under Apps → Sub-GHz, and it sits in the top-left of the app's screen showing what the
radio is doing:

| State | Antenna |
|---|---|
| RX (listening) | mast + inner waves |
| CSMA (waiting for a clear channel) | inner waves blink |
| TX | waves pulse outward (inner, then inner + outer) |

LED: green blink per received frame, red per transmission.

Settings live in `/ext/apps_data/kiss_tnc/settings.txt`.

### External CC1101

`fzkiss config --radio EXT` (or the Mac app) switches to an external CC1101 module on the
GPIO header. It uses the firmware's own `cc1101_ext` radio-device plugin (loaded from
`SD/apps_data/subghz/plugins/`, the same one the stock Sub-GHz app uses). 5 V OTG is turned
on only while it's in use. If no module is detected the app falls back to
the internal radio and says `EXT_NOT_FOUND`.

---

## Using it with other software

* Any KISS client on the TNC serial port: standard data frames in and out. The extra
  status frames (KISS command 6) are ignored by standard clients.
* Network clients: `fzkiss tcp --port 8001` (or the Mac app toggle), then point the client
  at `localhost:8001` as a KISS-over-TCP TNC.
* `SQL` (squelch, default −95 dBm) is also the carrier-sense threshold. If it's below your
  noise floor the channel always looks busy and sends fail with `ERR BUSY_TIMEOUT`. Check the
  idle `RSSI` in `fzkiss get` and set SQL a few dB above it.

---

## Building from source

```bash
cd modem_tests && make test
```

```bash
pip install -e "host/[test]" && pytest host/tests
```

```bash
cd mac/FlipperTNCCore && swift test
```

```bash
cd fap && ufbt
```

The FAP lands in `fap/dist/kiss_tnc.fap`. The channel filter width is a compile-time option
(`KISS_TNC_CHANBW` in `fap/modem/cc1101_preset.h`, default 81 kHz).

## Limits

Half-duplex. UI/raw frames only: no connected-mode AX.25, no digipeating, no encryption.
