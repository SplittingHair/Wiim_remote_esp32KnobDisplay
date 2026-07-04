# WiiM Knob Controller

A physical volume knob and now-playing display for the **WiiM Mini** (and other
LinkPlay-based streamers), built on the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8**
— a rotary knob with a 360x360 round AMOLED touch screen.

Turn the knob on your desk or in your car and the WiiM's volume follows.
Tap the screen to play/pause, swipe to change tracks, and see what's playing
at a glance.

![Board: Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)

## What to expect

**Now-playing screen**
- Song title (large, scrolling) and artist
- Volume arc around the screen edge + numeric volume
- Play/pause state, WiFi status dot, battery percentage with charging icon
- Mute button (bottom center)

**Controls**
- **Rotate knob** — volume, with velocity acceleration (slow = ±1 fine steps,
  fast spin = big jumps)
- **Tap screen** — play / pause
- **Swipe left / right** — next / previous track
- **Swipe down** — settings menu
- **Swipe up** — back

**Settings menu**
- **WiFi Details** — network, IP, signal strength, WiiM status, battery
- **Sleep Timer** — screen off after 1/5/10 min idle (true AMOLED power-down;
  wake with any touch or knob turn)
- **WiFi Setup** — starts a captive-portal hotspot for configuring WiFi from
  a phone

**Connectivity & configuration**
- **Web config page** — while connected, open `http://wiim-knob.local` (or the
  device IP) from a phone to change WiFi credentials or the WiiM's IP address.
  WiiM IP changes apply instantly, no reflash needed.
- **Captive portal** — with no saved network the device broadcasts a
  `WiiM-Knob-Setup` hotspot (device at `192.168.4.1`); join it with a phone and
  a setup page appears.
- **Offline resilience** — if WiFi drops it auto-reconnects; if the WiiM is
  off/unreachable a status screen shows what's wrong and the UI returns to
  now-playing automatically when the WiiM answers. The interface stays fully
  responsive throughout (no blocking on dead connections).

**Power**
- WiFi modem sleep, slow polling while the display sleeps, and true panel
  sleep-in keep battery drain low when idle.

## Hardware

| Part | Notes |
|---|---|
| Waveshare ESP32-S3-Knob-Touch-LCD-1.8 | ESP32-S3, 16MB flash, 8MB PSRAM, SH8601 360x360 AMOLED (QSPI), CST816 touch, rotary encoder |
| WiiM Mini (or other LinkPlay device) | Controlled over the local network via the LinkPlay HTTP API (HTTPS with self-signed cert supported) |

Both devices must be on the same WiFi network.

## 1. Install the Arduino IDE

1. Download **Arduino IDE 2.x** from https://www.arduino.cc/en/software and install it.
2. Open **Settings / Preferences** and add this URL to
   **Additional boards manager URLs**:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Open **Boards Manager** (left sidebar), search for **esp32**, and install
   **"esp32 by Espressif Systems"** (~200 MB; this project was built against
   the 3.x core).
4. Open **Library Manager**, search for **ArduinoJson** (by Benoit Blanchon)
   and install **version 6.x**.

## 2. Get the project

```
git clone https://github.com/YOURUSERNAME/wiim-knob-controller.git
```

Open `wiim_full.ino` in the Arduino IDE (File → Open, select the file inside
the project folder — the folder name must match the sketch name).

## 3. Configure the board (Tools menu)

These settings are **required** — wrong flash size or partition scheme causes
a boot loop, and the sketch will not fit in the default partition:

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| Flash Size | **16MB (128Mb)** |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
| PSRAM | **Disabled** |
| USB CDC On Boot | **Enabled** |
| CPU Frequency | 240MHz (WiFi) |
| Upload Speed | 921600 |

> PSRAM is intentionally disabled: the project doesn't use it and early-revision
> ESP32-S3 silicon has PSRAM cache errata that caused random reboots.

Also make sure `lv_conf.h` (included in the repo) has these fonts enabled —
they already are in the committed file:

```c
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
```

## 4. Project configuration (optional)

Open `wiim_full.ino` and optionally set a fallback WiFi network:

```c
static const char* WIFI_SSID_FALLBACK = "";   // "" = rely on portal/saved creds
static const char* WIFI_PASS_FALLBACK = "";
```

You don't have to hardcode anything: with both left empty, a factory-fresh
device automatically opens the **WiiM-Knob-Setup** hotspot on first boot and
you configure WiFi from your phone. The WiiM's IP defaults to `192.168.1.93`
(`WIIM_IP_DEFAULT` in `wiim_client.h`) and can be changed at any time from the
web config page — give the WiiM a DHCP reservation on your router so it never
moves.

## 5. Upload to the device

1. Connect the knob over USB-C. The board shows up as a serial port
   (`/dev/cu.usbmodem…` on macOS, `COMx` on Windows) — select it under
   **Tools → Port**.
2. Click **Upload**. First compile takes a few minutes.
3. If the upload fails to connect, force download mode: hold **BOOT**,
   tap **RESET**, release **BOOT**, then upload again.
4. Open **Serial Monitor at 115200** to watch the boot log
   (`[boot] reset reason`, `[sleep]` heartbeat, `[wiim]` request log).

### First boot

- With no credentials anywhere: the device opens the **WiiM-Knob-Setup**
  hotspot → join it from a phone → the setup page appears (or browse to
  `192.168.4.1`) → pick your network, enter the password, Save.
- Once connected, the now-playing screen appears as soon as the WiiM responds.
  If the WiiM isn't found, a status screen shows the WiFi state, the WiiM IP
  being tried, and the config URL — fix the IP from
  `http://wiim-knob.local` if needed.

### Recovery

If a bad flash ever boot-loops the device, a full erase + reflash always
recovers it:

```
pip install esptool
esptool --chip esp32s3 --port <YOUR_PORT> erase-flash
```

then upload from the Arduino IDE again (settings from step 3).

## Project structure

| File | Purpose |
|---|---|
| `wiim_full.ino` | Main sketch: tasks (knob, actions, polling, battery, sleep), WiFi lifecycle |
| `wiim_ui.h` | LVGL v8 UI: now-playing, settings menu + sub-pages, offline screen, gestures |
| `wiim_client.h` | WiiM / LinkPlay HTTP(S) client with connection reuse and offline backoff |
| `wiim_portal.h` | Captive-portal WiFi setup (AP mode) |
| `wiim_webcfg.h` | Web config page + mDNS (`wiim-knob.local`) while connected |
| `wiim_battery.h` | Battery voltage → percentage (ADC + LiPo discharge curve) |
| `lcd_bsp.*`, `cst816.*`, `esp_lcd_sh8601.*`, `bidi_switch_knob.*`, … | Waveshare board support (display, touch, encoder) |
| `lv_conf.h` | LVGL configuration (fonts, features) |

## Architecture notes (for contributors)

Hard-won rules that keep this board stable — follow them when extending:

- **Touch/gesture handlers only set flags.** Never call network functions or
  write LVGL widgets from the raw input callbacks — TLS overflows the LVGL
  task stack and widget writes mid-render corrupt the heap.
- **All network I/O lives in dedicated tasks pinned to core 0** with 12KB
  stacks; the LVGL render task is pinned to core 1 at higher priority so TLS
  crypto can never starve the UI.
- **Panel commands run in the LVGL context** (via `lv_async_call`), and the
  LVGL refresh timer is paused while the panel sleeps — never flush to a
  sleeping panel.
- **GPIO47 is the panel power rail** — keep it constantly high; PWM-chopping
  it destabilizes the QSPI display link.
- The WiiM returns all values as quoted strings (use `atoi`) and hex-encodes
  title/artist in `getPlayerStatus`.

## Credits

- Display/touch/encoder board support based on **Waveshare's** demo code for
  the ESP32-S3-Knob-Touch-LCD-1.8.
- UI built with **LVGL v8**, JSON parsing with **ArduinoJson v6**.
- Talks to the WiiM via the **LinkPlay HTTP API**.

## License

MIT (or your preference — the Waveshare-derived files retain their original terms).
