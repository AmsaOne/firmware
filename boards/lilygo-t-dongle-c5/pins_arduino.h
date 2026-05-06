#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include "soc/soc_caps.h"
#include <stdint.h>

// USB-Serial-JTAG (native CDC)
static const uint8_t USB_DM = 13;
static const uint8_t USB_DP = 14;

// UART0 — broken out on the 4-pin header (P3)
static const uint8_t TX = 11;
static const uint8_t RX = 12;
#define SERIAL_TX 11
#define SERIAL_RX 12

// LP I2C / LP UART are fixed on ESP32-C5
static const uint8_t LP_SDA = 4;
static const uint8_t LP_SCL = 5;
static const uint8_t LP_RX  = 12;
static const uint8_t LP_TX  = 11;
#define WIRE1_PIN_DEFINED
#define SDA1 LP_SDA
#define SCL1 LP_SCL

// I2C — not wired on the T-Dongle bare; reserve free pins for future header use
static const uint8_t SDA = 4;
static const uint8_t SCL = 5;

// =====================================================================
// SHARED SPI bus — LCD + SD + APA102 LED all live on this one bus.
// CS pins multiplex; APA102 has no CS (uses start/end frames).
// =====================================================================
static const uint8_t MOSI = 2;
static const uint8_t MISO = 7;
static const uint8_t SCK  = 6;
static const uint8_t SS   = 10; // default SS = LCD CS

#define SPI_MOSI_PIN 2
#define SPI_MISO_PIN 7
#define SPI_SCK_PIN  6
#define SPI_SS_PIN   10

// =====================================================================
// LCD — ST7735 80x160 IPS, mini-greentab. Most pins also defined via
// build_flags for TFT_eSPI; mirrored here for clarity / non-TFT_eSPI code.
// =====================================================================
#define HAS_SCREEN 1
#define ROTATION 3               // landscape 160x80 (matches LilyGo Factory example)
#define MINBRIGHT (uint8_t)1
#define LCD_BL_ACTIVE_LOW 1      // hint for HAL — backlight P-MOSFET inverted
#define COLOR_INVERTED 1         // 0.96" ST7735 mini-panel needs INVERT_ON

// =====================================================================
// APA102 single RGB LED on shared SPI (no CS, uses SPI start/end frames)
// =====================================================================
// DIAGNOSTIC: temporarily disable the RGB LED so FastLED doesn't hog the
// shared SPI bus (LED MOSI=GPIO2 / SCK=GPIO6 are also the LCD's pins).
// Re-enable after confirming LCD comes on, then either bit-bang APA102 or
// share the SPI bus with explicit transactions.
// #define HAS_RGB_LED 1
#define LED_TYPE APA102          // explicit — Bruce default for *_C5 was WS2812
#define LED_TYPE_IS_RGBW 0
#define LED_COUNT 1
#define LED_ORDER GRB
#define LED_COLOR_STEP 15

// Bruce led_control.cpp uses RGB_LED (data pin) and, when RGB_LED_CLK is
// defined, switches to the 4-arg APA102 FastLED template.
#define RGB_LED      2     // APA102 DI on shared SPI MOSI
#define RGB_LED_CLK  6     // APA102 CI on shared SPI SCK
#define APA102_DI_PIN 2
#define APA102_CI_PIN 6
#define PIN_RGB_LED RGB_LED   // Arduino-core compatibility alias

// =====================================================================
// SD card — SPI mode on the shared bus
// =====================================================================
#define SDCARD_CS   23
#define SDCARD_SCK  SPI_SCK_PIN
#define SDCARD_MISO SPI_MISO_PIN
#define SDCARD_MOSI SPI_MOSI_PIN

// =====================================================================
// Single user button (BOOT button on the side of the dongle)
// External 10K pull-up to 3.3V (R16), button shorts to GND.
// =====================================================================
#define HAS_BTN 1
#define BTN_PIN  28
#define BTN_ACT  LOW
#define SEL_BTN  28
// Bruce navigation falls back gracefully when UP/DW are not defined.

#define DEEPSLEEP_WAKEUP_PIN BTN_PIN
#define DEEPSLEEP_PIN_ACT    LOW

// =====================================================================
// Peripherals NOT on this board — pin = -1 disables init paths in Bruce
// (CS pins kept on shared SPI bus pins for any user-wired add-ons later)
// =====================================================================
#define CC1101_GDO0_PIN -1
#define CC1101_SS_PIN   -1
#define CC1101_MOSI_PIN SPI_MOSI_PIN
#define CC1101_SCK_PIN  SPI_SCK_PIN
#define CC1101_MISO_PIN SPI_MISO_PIN

#define NRF24_CE_PIN -1
#define NRF24_SS_PIN -1
#define NRF24_MOSI_PIN SPI_MOSI_PIN
#define NRF24_SCK_PIN  SPI_SCK_PIN
#define NRF24_MISO_PIN SPI_MISO_PIN

#define W5500_INT_PIN -1
#define W5500_SS_PIN  -1
#define W5500_MOSI_PIN SPI_MOSI_PIN
#define W5500_SCK_PIN  SPI_SCK_PIN
#define W5500_MISO_PIN SPI_MISO_PIN

// IR LEDs / Bad-USB / GPS-UART — none physically present, but Bruce wants
// these defined. Park them on free GPIOs (4, 5) so the user can wire later
// without changing firmware. Wardriver firmware uses UDP-NMEA GPS, not UART.
#define RXLED -1
#define TXLED -1
#define LED_ON  HIGH
#define LED_OFF LOW

#define BAD_RX 4
#define BAD_TX 5

#define GPS_SERIAL_TX 4
#define GPS_SERIAL_RX 5

#endif /* Pins_Arduino_h */
