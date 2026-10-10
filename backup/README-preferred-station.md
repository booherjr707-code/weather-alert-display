# Preferred Weather Station — firmware archive (2026-10-09)

Original "Weather Alert Display" firmware (compiled 2026-09-16, source lost in the
2026-09-25 Mac wipe), read off the Elegoo ESP32 DevKit V1 (ESP32-D0WD-V3, 4MB).
Features: NWS weather + alerts, Open-Meteo 15-minute rain countdown, barometer,
3-day forecast, MQTT / Home Assistant.

CONTAINS WI-FI AND MQTT PASSWORDS — never upload these .bin files publicly.

## Files
- `preferred-weather-station-2026-10-09.bin` — full 4MB flash dump, exactly as
  the board was before any change (Lonely Binary display, color invert ON).
- `app0-colorfix-noinvert-2026-10-09.bin` — the program currently on the board:
  identical except one byte (invert colors OFF) for the new blue 8-pin 1.8" TFT.

## Restore
    ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py
    PY=~/.platformio/penv/bin/python
    # Whole original board, exactly as it was:
    $PY $ESPTOOL --port /dev/cu.usbserial-0001 --baud 115200 write_flash 0 preferred-weather-station-2026-10-09.bin
    # Current version (new display) — program area only:
    $PY $ESPTOOL --port /dev/cu.usbserial-0001 --baud 115200 write_flash 0x10000 app0-colorfix-noinvert-2026-10-09.bin

Use 115200 baud; faster speeds failed on this board.

## Wiring (new blue 8-pin 1.8" TFT, J1 open)
| Display | Wire   | ESP32 |
|---------|--------|-------|
| RST     | red    | D4    |
| CS      | orange | D15   |
| D/C     | blue   | D2    |
| DIN     | green  | D23   |
| CLK     | gray   | D18   |
| VCC     | yellow | VIN   |
| BL      | white  | 3V3   |
| GND     | black  | GND   |

The blue LED on the ESP32 is on D2 (D/C) — it lighting up is normal.

## Color fix detail
Display driver call `Arduino_ST7735(..., rst=4, rotation=1, ips=true, ...)`; the
`ips` (invert) argument is `movi.n a14,1` at 0x400e606c, changed to `movi.n a14,0`
(byte 0x1e -> 0x0e), image checksum and SHA-256 recomputed. Lonely Binary panel
needs invert ON; the new panel needs it OFF.
