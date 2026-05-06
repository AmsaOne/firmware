# LilyGo T-Dongle C5 — pin map

Authoritative source: LilyGo `idf/Examples/FACTORY/main/t-dongle-c5-board.h` and
`arduino/examples/Factory/pin_config.h`.

| Function | GPIO | Notes |
|---|---|---|
| LCD MOSI / SD CMD / APA102 DI | 2 | shared SPI MOSI |
| LCD MISO / SD DAT0 | 7 | shared SPI MISO |
| LCD CLK / SD CLK / APA102 CI | 6 | shared SPI SCK |
| LCD CS | 10 | |
| LCD DC / RS | 3 | |
| LCD RST | 1 | |
| LCD BL | 0 | **active LOW** (P-MOSFET SI2301) |
| SD CS | 23 | |
| BOOT button | 28 | active LOW (10K pull-up to 3.3V) |
| UART0 TX | 11 | |
| UART0 RX | 12 | |
| USB DM | 13 | |
| USB DP | 14 | |

## Free GPIOs (no on-board function)
4, 5, 8, 9, 24, 25, 26, 27 — broken out via the optional 4-pin header (P3) and via
test pads on the PCB. Use these for any expansion (external GPS module, additional
buttons, etc.). Note GPIO 8/9 are strap pins.

## Display
- **ST7735** 80×160 IPS (mini-greentab variant, BGR colour order).
- Pixel clock: 20 MHz max stable.
- Native portrait 80×160; firmware uses `ROTATION=1` (landscape 160×80).
- Backlight is **active LOW** — `digitalWrite(TFT_BL, LOW)` = on.
  The board variant inverts PWM in `_setBrightness()` so Bruce settings
  ("brightness 0..100") work intuitively.

## RGB LED
- Single **APA102** on the shared SPI bus, **no chip-select**.
- Filtered by APA102's start-frame protocol: writes that aren't valid frames
  are ignored, so coexistence with TFT/SD on the same bus is safe.

## Power
- USB-C only. No battery, no charger. `getBattery()` always returns 0.

## Notes for the wardriver build
- Wi-Fi: defaults to softAP-only (`BRUCE_AP_MODE_DEFAULT=1`).
- GPS: UDP-NMEA from iPhone GPS2IP app (`BRUCE_GPS_UDP=1`); no wired module.
- Upload: iOS Shortcut polls the dongle's REST API (`BRUCE_REST_HANDOFF=1`) and
  POSTs each CSV to `wdgwars.pl/api/upload-csv` over cellular.
- Wardrive autostart on boot when GPS is valid (`BRUCE_AUTOSTART_WARDRIVE=1`).

## Flashing
```powershell
pio run -e lilygo-t-dongle-c5 -t upload --upload-port COM18
```
