# ESP32 Spotify Controller

A touchscreen Spotify controller built on the **Elecrow 3.2" ESP32 display**, written in C++ with the Arduino framework. Displays the currently playing track, album art, and playback controls.

> **Note:** This project was built as a hands on dive into embedded systems fundamentals, SPI peripherals, touch digitizer calibration, OAuth flows on constrained hardware, and real time UI rendering without an OS

<!-- -->

### Features

- Now playing display: track title, artist, album, and album art
- Scrolling marquee for long titles and artist names
- Playback controls: previous, play/pause, next
- Save / unsave track to your Spotify library
- Smooth progress bar with local tick interpolation
- OAuth 2.0 authentication served directly from the ESP32 over LAN
- Auto token refresh: stays authenticated indefinitely
- WiFi watchdog with automatic reconnect


---
<br/>

## Hardware

| Component | Details |
|---|---|
| Board | Elecrow 3.2" ESP32 Display (ESP-WROVER-KIT compatible) |
| Display | ILI9488 480×320 TFT, SPI |
| Touch | XPT2046 resistive digitizer, SPI |
| MCU | ESP32 dual-core 240 MHz, 320 KB RAM, 4 MB Flash |

### Pin Connections

The display and touch controller share the ESP32's SPI bus. Connections are handled internally on the Elecrow board, but for reference:

| Signal | ESP32 GPIO |
|---|---|
| SPI SCLK | 14 |
| SPI MISO | 33 |
| SPI MOSI | 13 |
| Touch CS | 5 |

TFT CS, DC, and reset pins are configured in `User_Setup.h` for the TFT_eSPI library

---

### Project Structure

```
src/
├── main.cpp          # setup(), loop(), global state, WiFi
├── display.cpp/.h    # all TFT drawing: layout, carousel, controls
├── touch_handler.cpp/.h  # XPT2046 read, calibration, button hit detection
├── spotify_api.cpp/.h    # Spotify Web API calls (poll, play, pause, skip, save)
├── auth.cpp/.h       # OAuth 2.0: LAN auth flow, token refresh, NVS storage
├── http_helpers.cpp/.h   # HTTPS GET/PUT helpers
├── state.h           # Track struct, AppState enum, shared globals
├── config.h          # Display layout: button positions, touch calibration, colors
└── secrets.h         # WiFi credentials and Spotify app keys (not committed)
```

---

### What I Learned

This project was intentionally built from scratch to understand the fundamentals rather than rely on high level abstractions.

**SPI and peripheral communication**: The display and touch controller both run over SPI. Understanding how chip select lines work, why byte order matters (`setSwapBytes`), and how the XPT2046 ADC reports coordinates helped debug weeks of incorrect touch readings.

**Touch calibration**: The XPT2046 reports raw 12-bit ADC values that need mapping to screen pixels. Both axes are physically inverted on this board, and the raw range doesn't start at zero. Getting this right required logging raw values at each corner, reverse-engineering the mapping from serial output, and understanding `Arduino::map()` deeply enough to invert it correctly.

**Coordinate system gotchas**: `TFT_eSPI::setViewport()` resets the drawing origin to the viewport's top-left. Passing screen-absolute coordinates inside a viewport silently draws off-screen. This caused the scrolling marquee to render nothing for a long time.

**OAuth on embedded hardware**: The ESP32 runs a tiny HTTP server to receive the Spotify authorization callback, exchanges the code for tokens, and persists the refresh token to NVS (non-volatile storage). Token refresh happens automatically before expiry with no user interaction.

**State machine UI**: Rather than redrawing everything every loop, the display tracks `curr` and `prev` track state and only redraws changed fields. Progress is interpolated locally at 500 ms ticks between 3-second API polls so the bar moves smoothly without hammering the network.

**HTTP on constrained hardware**: Spotify's API has specific requirements: `Content-Length: 0` on empty POSTs, JSON bodies for track save/delete, and 204 responses for transport controls. Each of these required reading the API spec carefully rather than guessing.

---

<br/>

## Setup

### 1. Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- A [Spotify Developer App](https://developer.spotify.com/dashboard)

### 2. Clone and configure

```bash
git clone git@github.com:Edward-Serna/ELECROW_3.2_ESP32_SPOTIFY_CONTROL.git
cd ELECROW_3.2_ESP32_APP
```

Create `src/secrets.h`:

```cpp
#pragma once
#define WIFI_SSID        "your_wifi_name"
#define WIFI_PASS        "your_wifi_password"
#define SPOTIFY_CLIENT_ID      "your_client_id"
#define SPOTIFY_CLIENT_SECRET  "your_client_secret"
```

### 3. Configure TFT_eSPI

Copy the correct `User_Setup.h` for the Elecrow 3.2" board into the TFT_eSPI library folder, or set `USER_SETUP_LOADED` in `platformio.ini`. The board uses an ILI9488 driver.

### 4. Add your redirect URI

In your Spotify Developer Dashboard, add the following redirect URI:

```
http://<ESP32_IP_ADDRESS>/callback
```

You'll see the ESP32's IP on the boot screen after it connects to WiFi.

### 5. Build and flash

```bash
pio run --target upload
```

Or use the PlatformIO sidebar in VS Code → Upload.

---
<br/>

## First Boot

1. The device connects to WiFi and displays its IP address
2. Open `http://<ESP32_IP>` in a browser on the same network
3. Click **Authorize with Spotify** and log in
4. The device stores the refresh token and loads the player — you won't need to authorize again

---

### Touch Calibration

If buttons feel off, enable calibration mode in `config.h`:

```cpp
#define TOUCH_CAL_MODE 1
```

Flash, then tap each corner (TL → TR → BL → BR) and read the raw values from the serial monitor at 115200 baud. Update these constants in `config.h`:

```cpp
#define TOUCH_X_MIN   150   // rawX at screen right edge
#define TOUCH_X_MAX  3880   // rawX at screen left edge  (inverted)
#define TOUCH_Y_MIN   320   // rawY at screen bottom
#define TOUCH_Y_MAX  3880   // rawY at screen top        (inverted)
```

Set `TOUCH_CAL_MODE` back to `0` and reflash.

---

### Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Purpose |
|---|---|
| TFT_eSPI | Display driver and graphics |
| XPT2046_Touchscreen | Touch digitizer |
| TJpg_Decoder | JPEG decoding for album art |
| ArduinoJson | Spotify API response parsing |
| WiFiClientSecure | HTTPS connections |

