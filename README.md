# Weather Alert Display

An ESP32 weather station for Jonesboro, AR, built around a 1.8" color TFT. It pulls
current conditions, a 3-day forecast, and active severe weather alerts from the
National Weather Service (`api.weather.gov`, free, no API key) and publishes
everything to Home Assistant over MQTT.

This replaces an earlier board whose firmware was lost, and recreates the same
Home Assistant entities so an existing dashboard keeps working unchanged.

## What it shows

- **Current conditions**: temperature, feels-like, humidity, dew point, wind, and
  barometric pressure.
- **Barometer gauge**: a vertical bar on the right edge of the screen. A white line
  across the middle marks standard pressure (29.92" inHg); a blue fill and arrow
  move above it for high pressure, below it for low pressure.
- **Header** rotates every few seconds between the city name and the next 3 days'
  forecast (e.g. "Tue 93/74 Sunny").
- **Severe weather alerts**: watches/warnings take over the header as a flashing,
  scrolling banner (e.g. "!! SEVERE THUNDERSTORM WARNING") until they clear.
- **Rain heads-up**: when the forecast calls for rain, a line appears below the
  conditions (e.g. "Rain This Afternoon"), and the screen periodically switches to a
  dedicated rain screen with the forecast period, text, and chance of
  precipitation.
- A 4-bar Wi-Fi signal indicator and a 12-hour clock (syncs over the internet).

Conditions and the forecast refresh every 10 minutes. Alerts are checked every 2
minutes.

## Parts

- ESP32-WROOM-32 dev board
- 1.8" SPI TFT, 128x160, ST7735-family

## Wiring

| Display | ESP32 |
|---|---|
| VCC | VIN (5V) |
| GND | GND |
| SCL | D18 |
| SDA | D23 |
| RES | D4 |
| DC | RX2 (D16) |
| CS | D5 |
| BLK | 3V3 |

(RX2/TX2 are this board's labels for the second hardware UART, GPIO16/17 — unused
by this sketch, so safe to reuse as plain GPIOs. RES is on D4 rather than TX2/GPIO17
because GPIO17 is bad on this specific ESP32 chip.)

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Copy `src/secrets.example.h` to `src/secrets.h` and fill in your Wi-Fi name and
   password, plus your Home Assistant MQTT broker details if you want the Home
   Assistant integration (leave `MQTT_HOST` empty to turn it off). `secrets.h` is
   git-ignored so your credentials are never uploaded. The ESP32 only works on
   2.4 GHz Wi-Fi.
3. Change the city, latitude, and longitude near the top of `src/main.cpp` if you
   are not in Jonesboro. If you do, also look up your new
   `NWS_GRID_ID`/`NWS_GRID_X`/`NWS_GRID_Y`/`NWS_STATION` from
   `https://api.weather.gov/points/{lat},{lon}` and its `observationStations` link,
   and update those constants too.
4. Plug in the ESP32 with a USB cable that carries data, then run
   `pio run -t upload -t monitor`.

If the screen stays solid white or shows garbled colors, see the `PANEL_BGR`,
`PANEL_INVERT`, `ROTATION`, and `COL_OFFSET`/`ROW_OFFSET` settings near the top of
`src/main.cpp` — the comments there walk through each symptom.

## Credits

Built with [Claude Code](https://claude.com/claude-code), Anthropic's AI coding assistant.
