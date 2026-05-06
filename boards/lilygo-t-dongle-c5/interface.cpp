#include "core/powerSave.h"
#include "core/utils.h"
#include <interface.h>

// Forward decl: Bruce's global SD-mounted flag (defined in src/main.cpp:101).
// We set this true after our IDF sdspi mount succeeds so Bruce's setupSdCard()
// sees the card as already mounted and skips its broken Arduino SD.h path.
extern bool sdcardMounted;

// Headless wardriver runtime
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <FS.h>
#include <TinyGPS++.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <time.h>
// ESP-IDF sdspi driver — Arduino-ESP32's SD.h fails on ESP32-C5 GPIO 2 (we
// proved this by flashing LilyGo's factory firmware; it mounts SD on this
// exact hardware via the IDF sdspi_host driver instead of Arduino SD.h).
// We use the IDF driver directly here. After esp_vfs_fat_sdspi_mount() the
// card appears at /sdcard via VFS, so standard POSIX fopen/fread/etc. work.
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <SPI.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

// Splash logo (RGB565), generated from media/pictures/bruce_hd.png by
// tools/png_to_rgb565.py at build time. Re-run that script if you swap the
// source PNG.
#include "bruce_logo.h"

// =============================================================================
// LilyGo T-Dongle C5 — headless wardriver build
//
// ESP32-C5 hardware SPI cannot drive GPIO 2 (panel MOSI). We bit-bang the
// ST7735 directly to display a one-shot boot splash, and we bit-bang the
// APA102 LED on the same shared bus for runtime status feedback. Bruce's
// normal TFT_eSPI calls are no-ops on this hardware; we accept that for v1.
// See Adafruit-ST7735-swap-plan.md for the eventual fix.
// =============================================================================

// ---- bit-bang SPI (LCD + APA102 share MOSI=GPIO2 / SCK=GPIO6) ---------------
static inline void bb_spi_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(TFT_MOSI, (b >> i) & 1);
        digitalWrite(TFT_SCLK, HIGH);
        digitalWrite(TFT_SCLK, LOW);
    }
}

// ---- ST7735 bit-bang driver -------------------------------------------------
static inline void lcd_cmd(uint8_t c)  { digitalWrite(TFT_DC, LOW);  bb_spi_byte(c); digitalWrite(TFT_DC, HIGH); }
static inline void lcd_data(uint8_t d) { digitalWrite(TFT_DC, HIGH); bb_spi_byte(d); }
static void lcd_init() {
    digitalWrite(TFT_RST, HIGH); delay(50);
    digitalWrite(TFT_RST, LOW);  delay(50);
    digitalWrite(TFT_RST, HIGH); delay(150);
    digitalWrite(TFT_CS, LOW);
    lcd_cmd(0x01); delay(150);            // SWRESET
    lcd_cmd(0x11); delay(255);            // SLPOUT
    lcd_cmd(0x3A); lcd_data(0x05);        // COLMOD = 16bpp 565
    lcd_cmd(0x36); lcd_data(0x68);        // MADCTL = MX|MV|BGR (rotation 3, BGR ON)
    lcd_cmd(0x21);                        // INVON
    lcd_cmd(0x29); delay(100);            // DISPON
    digitalWrite(TFT_CS, HIGH);
}
// Visible window after rotation 3 on 0.96" 160x80 mini panel:
//   X: 1 .. 161   (rowstart=1, plus ST7735's own offset)
//   Y: 26 .. 105  (colstart=26)
// Address ranges below match TFT_eSPI's expectations for the same panel.
static void lcd_set_window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    digitalWrite(TFT_CS, LOW);
    lcd_cmd(0x2A); lcd_data(0); lcd_data(x0 + 1);  lcd_data(0); lcd_data(x1 + 1);   // CASET (rotation 3, +1 row offset)
    lcd_cmd(0x2B); lcd_data(0); lcd_data(y0 + 26); lcd_data(0); lcd_data(y1 + 26);  // RASET (+26 col offset)
    lcd_cmd(0x2C);                                                                  // RAMWR
    digitalWrite(TFT_DC, HIGH);
}
static void lcd_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t color) {
    if (w == 0 || h == 0) return;
    lcd_set_window(x, y, x + w - 1, y + h - 1);
    uint8_t hi = color >> 8, lo = color & 0xff;
    for (int i = 0; i < (int)w * (int)h; i++) { bb_spi_byte(hi); bb_spi_byte(lo); }
    digitalWrite(TFT_CS, HIGH);
}
static inline void lcd_clear(uint16_t c) { lcd_fill_rect(0, 0, 160, 80, c); }

// Blit an RGB565 bitmap (row-major, big-endian on the wire) to (x,y).
static void lcd_blit(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint16_t* pix) {
    if (w == 0 || h == 0) return;
    lcd_set_window(x, y, x + w - 1, y + h - 1);
    int n = (int)w * (int)h;
    for (int i = 0; i < n; i++) {
        uint16_t c = pix[i];
        bb_spi_byte(c >> 8);
        bb_spi_byte(c & 0xff);
    }
    digitalWrite(TFT_CS, HIGH);
}

// ---- minimal 5x7 ASCII font (only the chars we need) ------------------
// Column-major: each byte = one column, LSB = top pixel, bit6 = bottom.
struct Glyph { char ch; uint8_t col[5]; };
static const Glyph FONT[] = {
    { 'B', {0x7F, 0x49, 0x49, 0x49, 0x36} },
    { 'R', {0x7F, 0x09, 0x19, 0x29, 0x46} },
    { 'U', {0x3F, 0x40, 0x40, 0x40, 0x3F} },
    { 'C', {0x3E, 0x41, 0x41, 0x41, 0x22} },
    { 'E', {0x7F, 0x49, 0x49, 0x49, 0x41} },
    { 'P', {0x7F, 0x09, 0x09, 0x09, 0x06} },
    { 'D', {0x7F, 0x41, 0x41, 0x22, 0x1C} },
    { 'A', {0x7E, 0x11, 0x11, 0x11, 0x7E} },
    { 'T', {0x01, 0x01, 0x7F, 0x01, 0x01} },
    { 'O', {0x3E, 0x41, 0x41, 0x41, 0x3E} },
    { 'Y', {0x07, 0x08, 0x70, 0x08, 0x07} },
    { 'F', {0x7F, 0x09, 0x09, 0x09, 0x01} },
    { 'I', {0x00, 0x41, 0x7F, 0x41, 0x00} },
    { 'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F} },
    { 'W', {0x3F, 0x40, 0x38, 0x40, 0x3F} },
    { '>', {0x41, 0x22, 0x14, 0x08, 0x00} },
    { '<', {0x08, 0x14, 0x22, 0x41, 0x00} },
    { ' ', {0x00, 0x00, 0x00, 0x00, 0x00} },
};
static const Glyph* glyph(char ch) {
    for (auto &g : FONT) if (g.ch == ch) return &g;
    // Fall back to the literal space glyph by character lookup so we don't
    // depend on its index inside FONT[].
    for (auto &g : FONT) if (g.ch == ' ') return &g;
    return &FONT[0];
}
// Draw text. If 'bold' is true, each "on" pixel is rendered (scale × scale)
// AND its right-neighbour gets the same — gives the characters extra weight.
static void lcd_text_ex(uint8_t x, uint8_t y, const char* s,
                        uint8_t scale, uint16_t fg, uint16_t bg, bool bold) {
    for (const char* p = s; *p; p++) {
        const Glyph* g = glyph(*p);
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g->col[col];
            for (int row = 0; row < 7; row++) {
                bool on = bits & (1 << row);
                if (on) {
                    lcd_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
                    if (bold) lcd_fill_rect(x + col * scale + 1, y + row * scale, scale, scale, fg);
                } else {
                    lcd_fill_rect(x + col * scale, y + row * scale, scale, scale, bg);
                }
            }
        }
        x += (bold ? 7 : 6) * scale;     // 1 px inter-char gap
        if (x > 160) break;
    }
}
static inline void lcd_text(uint8_t x, uint8_t y, const char* s,
                            uint8_t scale, uint16_t fg, uint16_t bg) {
    lcd_text_ex(x, y, s, scale, fg, bg, false);
}
// Draw text with a 1-pixel down-right shadow in `shade`, then the
// foreground in `fg`. Background pixels are NOT painted by either pass —
// the splash must clear the region first.
static void lcd_text_shadow(uint8_t x, uint8_t y, const char* s,
                            uint16_t fg, uint16_t shade, bool bold) {
    // Shadow pass: only paint "on" pixels, offset by (1, 1).
    for (const char* p = s; *p; p++) {
        const Glyph* g = glyph(*p);
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g->col[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) {
                    lcd_fill_rect(x + col + 1, y + row + 1, 1, 1, shade);
                    if (bold) lcd_fill_rect(x + col + 2, y + row + 1, 1, 1, shade);
                }
            }
        }
        x += (bold ? 7 : 6);
        if (x > 160) break;
    }
}

// ---- Bit-bang ↔ FSPI peripheral handoff ------------------------------------
// The LCD splash and APA102 LED both bit-bang the shared MOSI=GPIO2 / SCK=GPIO6
// bus. Each bit-bang `pinMode(.., OUTPUT)` re-routes the IO MUX away from the
// FSPI peripheral, which then breaks any subsequent SD-card SPI transaction.
// `wd_restore_fspi_routing()` wires the GPIO matrix back to FSPI's signals
// (FSPID/FSPIQ/FSPICLK) so sdmmc reads work again. Forward decl needed because
// led_set_rgb() calls it but `wd_sd_mounted` is defined further down.
static bool wd_sd_mounted_fwd();
static void wd_restore_fspi_routing();

// ---- APA102 single-LED bit-bang status driver ------------------------------
// APA102 frame: start (0x00 0x00 0x00 0x00) + LED frame (0xE0|brightness, B, G, R)
//               + end (0xFF 0xFF 0xFF 0xFF). No CS — filtered by start frame.
//
// Borrows the shared SPI bus from the FSPI peripheral, sends the frame, then
// returns the bus to FSPI control so the SD card stays usable. Called from
// the wardriver scanner task on every iteration, so the borrow/return cycle
// must stay self-contained.
static void led_set_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t bright = 8) {
    pinMode(SDCARD_MOSI, OUTPUT); digitalWrite(SDCARD_MOSI, LOW);
    pinMode(SDCARD_SCK,  OUTPUT); digitalWrite(SDCARD_SCK,  LOW);
    bb_spi_byte(0); bb_spi_byte(0); bb_spi_byte(0); bb_spi_byte(0);     // start
    bb_spi_byte(0xE0 | (bright & 0x1F));
    bb_spi_byte(b); bb_spi_byte(g); bb_spi_byte(r);
    bb_spi_byte(0xFF); bb_spi_byte(0xFF); bb_spi_byte(0xFF); bb_spi_byte(0xFF);  // end
    if (wd_sd_mounted_fwd()) wd_restore_fspi_routing();
}

// ---- Boot splash -----------------------------------------------------------
static void show_boot_splash() {
    // Black background, full-width logo, bracketed bold-purple subtitle with
    // a darker drop shadow for a more tactical / red-team feel.
    // (Note: WHITE/YELLOW/ORANGE/RED/etc. are macros from ST7735_Defines.h
    //  — never declare locals with those names.)
    const uint16_t clrBg     = 0x0000;   // black
    const uint16_t clrFg     = 0xA33B;   // bright pinkish purple
    const uint16_t clrShade  = 0x4011;   // dark purple shadow

    // Belt-and-braces clear: do it twice in case earlier HW-SPI traffic from
    // tft.init() / "Booting" calls left half-latched address counters.
    lcd_clear(clrBg);
    lcd_clear(clrBg);

    // Logo fills the top 72 of 80 rows. Bottom 8 px is reserved for the
    // subtitle.
    lcd_blit(0, 0, BRUCE_LOGO_W, BRUCE_LOGO_H, BRUCE_LOGO);

    // "> PREDATORY FIRMWARE <" — bold 5x7 with shadow. Bold widths each
    // glyph to 6+1=7 px; total 22 chars * 7 = 154 px. Centred x = 3.
    // y=72 leaves the (1,1) shadow ending at y=79 — fits the 80-row panel.
    const char* line = "> PREDATORY FIRMWARE <";
    lcd_text_shadow(3, 72, line, clrFg, clrShade, /*bold=*/true);
    lcd_text_ex   (3, 72, line, /*scale=*/1, clrFg, clrBg, /*bold=*/true);
}

// ---- Public API used by Bruce ---------------------------------------------

// Re-claim the LCD/LED-shared pins from the SPI peripheral (TFT_eSPI's
// spi.begin() routes them to FSPI/HSPI). Calling pinMode + digitalWrite
// pulls them back to GPIO mode for our bit-bang traffic.
//
// MOSI idles HIGH and SCK idles LOW so the SD card on the same shared bus
// sees the SPI mode-0 idle state during its own power-up / bring-up. Some
// SDXC cards lock into a degraded state if they see clock pulses with
// MOSI low at power-up — see audit addendum P0-8.
static void reclaim_bb_pins() {
    pinMode(TFT_MOSI, OUTPUT); digitalWrite(TFT_MOSI, HIGH);
    pinMode(TFT_SCLK, OUTPUT); digitalWrite(TFT_SCLK, LOW);
    pinMode(TFT_CS,   OUTPUT); digitalWrite(TFT_CS,   HIGH);
    pinMode(TFT_DC,   OUTPUT); digitalWrite(TFT_DC,   HIGH);
    pinMode(TFT_RST,  OUTPUT); digitalWrite(TFT_RST,  HIGH);
    pinMode(TFT_BL,   OUTPUT); digitalWrite(TFT_BL,   LOW);   // active LOW = ON
}

// =============================================================================
// SD card mount via ESP-IDF sdspi driver
// =============================================================================
// Arduino-ESP32's SD.h (sd_diskio.cpp) does NOT successfully drive an SD card
// via the GPSPI peripheral on ESP32-C5 — every SPI mode init returns
// f_mount(3) FR_NOT_READY. We confirmed this is software-side by flashing
// LilyGo's factory firmware to this exact dongle and seeing it mount the
// card cleanly. LilyGo uses ESP-IDF's sdspi_host driver. So do we, here.
//
// After esp_vfs_fat_sdspi_mount() succeeds, the card is registered in VFS
// at /sdcard. Standard POSIX fopen("/sdcard/...") + fwrite/fclose work, and
// so do opendir/readdir/stat/unlink. The wardriver code below uses POSIX
// directly on /sdcard rather than going through Arduino's SD.h API.
// =============================================================================
static sdmmc_card_t *wd_sd_card = nullptr;
static bool          wd_sd_mounted = false;

static bool wd_sd_mounted_fwd() { return wd_sd_mounted; }

// Re-attach the FSPI peripheral signals to GPIO 2/6/7 via the GPIO matrix
// after a bit-bang (LCD splash or APA102 LED) has temporarily taken those
// pins to plain GPIO output. Indices match what spi_bus_initialize routes —
// confirmed via gpio_dump_io_configuration in the instrumented run that
// established the fix.
static void wd_restore_fspi_routing() {
    // MOSI: FSPID OUT (signal 58)
    gpio_set_direction((gpio_num_t)SDCARD_MOSI, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal((gpio_num_t)SDCARD_MOSI, FSPID_OUT_IDX, false, false);
    esp_rom_gpio_connect_in_signal ((gpio_num_t)SDCARD_MOSI, FSPID_IN_IDX, false);
    // SCK: FSPICLK (signal 56)
    gpio_set_direction((gpio_num_t)SDCARD_SCK, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal((gpio_num_t)SDCARD_SCK, FSPICLK_OUT_IDX, false, false);
    esp_rom_gpio_connect_in_signal ((gpio_num_t)SDCARD_SCK, FSPICLK_IN_IDX, false);
    // MISO: FSPIQ IN (signal 57)
    gpio_set_direction((gpio_num_t)SDCARD_MISO, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal ((gpio_num_t)SDCARD_MISO, FSPIQ_IN_IDX, false);
}

// Bus mask for instrumented GPIO dumps. Covers our shared SPI bus + SD CS +
// boot button + LCD DC/RST/BL — everything we touch on this board. Use the
// SDCARD_* macros for SPI lines (TFT_MISO is -1 because the panel is
// write-only; (1ULL << -1) is undefined behaviour).
#define WD_GPIO_DUMP_MASK \
    ((1ULL << SDCARD_MOSI) | (1ULL << SDCARD_SCK) | (1ULL << SDCARD_MISO) | \
     (1ULL << TFT_CS)      | (1ULL << TFT_DC)     | (1ULL << TFT_RST)     | \
     (1ULL << TFT_BL)      | (1ULL << SDCARD_CS)  | (1ULL << BTN_PIN))

static bool wd_mount_sd_idf() {
    Serial.println("[wd][sd] === wd_mount_sd_idf() entry ===");

    // Step 1: release any Arduino SPIClass that may have grabbed the FSPI bus.
    // On ESP32-C5 the only GPSPI peripheral is FSPI (= SPI2_HOST in IDF), so
    // any SPIClass instance bound to FSPI sits on top of the same bus the SD
    // card needs. SPI.end() is a no-op if .begin() was never called, so it's
    // safe to call unconditionally.
    Serial.println("[wd][sd] step 1: SPI.end()");
    SPI.end();

    // Step 2: tear down any pre-existing IDF SPI bus state on SPI2_HOST.
    // ESP_ERR_INVALID_STATE means the bus wasn't initialized — which is the
    // expected case on a fresh boot, but on warm reset / re-mount it might
    // be live. Either outcome is fine.
    Serial.println("[wd][sd] step 2: spi_bus_free(SPI2_HOST)");
    esp_err_t ret = spi_bus_free(SPI2_HOST);
    Serial.printf("[wd][sd]   spi_bus_free => 0x%x (%s)\n", ret, esp_err_to_name(ret));

    // Step 3: settling delay. Per espressif/arduino-esp32 issue #6237 (C3/C6
    // class: same ESP_ERR_TIMEOUT 0x107 we see on C5), a 10–50 ms gap between
    // bus tear-down and re-init prevents intermittent mount failures.
    Serial.println("[wd][sd] step 3: settling delay 30ms");
    delay(30);

    Serial.println("[wd][sd] GPIO config BEFORE spi_bus_initialize:");
    gpio_dump_io_configuration(stdout, WD_GPIO_DUMP_MASK);

    // Step 4: configure the SPI bus. CRITICAL — use SDCARD_* (not TFT_*) macros
    // because the LCD is write-only and TFT_MISO is build-flagged to -1
    // ("no MISO pin"). Passing TFT_MISO=-1 to spi_bus_initialize made the IDF
    // sdspi driver run with no MISO connected, so CMD0/CMD8 responses never
    // reached the chip and esp_vfs_fat_sdspi_mount returned ESP_ERR_TIMEOUT.
    // SDCARD_MISO=7, which is the actual physical pin we need for SD reads.
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num     = SDCARD_MOSI;  // GPIO 2 — shared bus
    buscfg.miso_io_num     = SDCARD_MISO;  // GPIO 7 — required for SD CMD response
    buscfg.sclk_io_num     = SDCARD_SCK;   // GPIO 6
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = 4000;

    Serial.println("[wd][sd] step 4: spi_bus_initialize(SPI2_HOST, &buscfg, SDSPI_DEFAULT_DMA)");
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SDSPI_DEFAULT_DMA);
    Serial.printf("[wd][sd]   spi_bus_initialize => 0x%x (%s)\n", ret, esp_err_to_name(ret));
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        Serial.println("[wd][sd] FATAL: spi_bus_initialize failed; aborting mount");
        return false;
    }

    // Step 5: post-init settling delay, again per #6237 community workaround.
    // Some reporters needed 10 ms, some 50 ms — we use 50 ms for safety.
    Serial.println("[wd][sd] step 5: post-init settling delay 50ms");
    delay(50);

    Serial.println("[wd][sd] GPIO config AFTER spi_bus_initialize, BEFORE mount:");
    gpio_dump_io_configuration(stdout, WD_GPIO_DUMP_MASK);

    // Step 6: configure slot + host + mount options.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)SDCARD_CS;   // GPIO 23
    slot_config.host_id = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SPI2_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;        // 20 MHz steady-state — IDF
                                                   // auto-clamps init to 400 kHz

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files              = 8;
    mount_config.allocation_unit_size   = 16 * 1024;

    Serial.println("[wd][sd] step 6: esp_vfs_fat_sdspi_mount(\"/sdcard\", ...)");
    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &wd_sd_card);
    Serial.printf("[wd][sd]   esp_vfs_fat_sdspi_mount => 0x%x (%s)\n", ret, esp_err_to_name(ret));

    Serial.println("[wd][sd] GPIO config AFTER mount attempt:");
    gpio_dump_io_configuration(stdout, WD_GPIO_DUMP_MASK);

    if (ret != ESP_OK) {
        Serial.println("[wd][sd] FAILED: SD did not mount.");
        return false;
    }

    Serial.println("[wd][sd] SD card mounted at /sdcard via ESP-IDF sdspi");
    if (wd_sd_card) {
        Serial.printf("[wd][sd]   Name: %s\n", wd_sd_card->cid.name);
        Serial.printf("[wd][sd]   Capacity: %llu MB\n",
            ((uint64_t)wd_sd_card->csd.capacity * wd_sd_card->csd.sector_size) / (1024 * 1024));
    }
    wd_sd_mounted = true;
    sdcardMounted = true;   // tell Bruce setupSdCard() to skip its broken path
    return true;
}

void _setup_gpio() {
    // Audit addendum: do NOT bit-bang SPI here (disrupts SD bring-up).
    // Audit P0-5: bruceConfig assignments here get clobbered by config-load.
    reclaim_bb_pins();
    pinMode(SDCARD_CS, OUTPUT); digitalWrite(SDCARD_CS, HIGH);
    pinMode(BTN_PIN,   INPUT_PULLUP);

    // SD mount intentionally NOT done here. Bruce calls tft.init() at
    // src/main.cpp:444 — TFT_eSPI's init invokes spi.begin() which runs
    // spiStartBus()/spiInitBus(), writing directly to the SPI2 peripheral
    // registers (spi->dev->user.usr_mosi etc.) and clobbering whatever the
    // IDF sdspi_host driver had set up. If we mount here, the mount succeeds
    // but every subsequent sdmmc_read_sectors_dma returns ESP_ERR_TIMEOUT
    // (0x107) because the peripheral state no longer matches the driver's
    // expectations. We instead mount in _post_setup_gpio (which runs AFTER
    // tft.init()), where wd_mount_sd_idf's spi_bus_free + spi_bus_initialize
    // cleanly re-configures the peripheral after TFT has finished with it.
}

extern "C" void wd_boot_headless_runtime();

void _post_setup_gpio() {
    // Apply Bruce config defaults that were deferred from _setup_gpio
    // (audit P0-5: setting them in _setup_gpio gets clobbered by config
    // load).
    bruceConfig.colorInverted = 1;
    bruceConfigPins.rotation  = ROTATION;

    // Mount the SD card via ESP-IDF sdspi NOW — after Bruce's tft.init()
    // (main.cpp:444) has run and finished poking the SPI2 peripheral via
    // its private SPIClass instance. wd_mount_sd_idf's preamble (SPI.end +
    // spi_bus_free + delay + spi_bus_initialize) cleans up after TFT_eSPI's
    // clobbering and gives sdspi_host a fresh peripheral to drive. Bruce's
    // earlier setupSdCard() (in begin_storage() at main.cpp:453) failed
    // because Arduino SD.h doesn't work here, but bruceConfig falls back to
    // LittleFS so that's harmless noise.
    wd_mount_sd_idf();

    // Reclaim the SPI bus from TFT_eSPI's peripheral binding, then bit-bang
    // our own ST7735 init + splash. From here on, Bruce's later TFT_eSPI
    // writes won't visibly do anything (HW SPI on GPIO 2 doesn't reach the
    // panel), so the splash is the last thing painted to the panel and
    // stays up for the lifetime of the wardriver session.
    reclaim_bb_pins();
    lcd_init();
    show_boot_splash();

    led_set_rgb(0x00, 0x80, 0xFF);   // cyan = ready
    // led_set_rgb already restores FSPI routing internally if the SD card
    // mounted, but be explicit here to guard against any lcd_*/show_boot_splash
    // calls that left GPIO 2/6 in plain-output mode without a follow-up LED
    // call.
    if (wd_sd_mounted) wd_restore_fspi_routing();

    // Spin up softAP, REST handoff, GPS UDP pump, and WiFi-scan wardriver
    // tasks. From here on the dongle behaves as the headless wardriver.
    wd_boot_headless_runtime();
}

// Battery: T-Dongle has no battery sensor.
int  getBattery() { return 0; }
bool isCharging() { return false; }

// Brightness: keep BL hardpinned LOW (ON). LEDC PWM on GPIO 0 (boot strap)
// proved unreliable in early tests; revisit during the Adafruit_ST7735 swap.
void _setBrightness(uint8_t /*brightval*/) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW);
}

// Headless v1: button is disabled so accidental presses can't navigate
// Bruce's (invisible) main menu and tear down our AP/REST tasks.
void InputHandler(void) {
    AnyKeyPress = false;
    PrevPress = NextPress = SelPress = EscPress = false;
}

void powerOff()    {}
void checkReboot() {}

// Hooks the wardriver/AP/REST tasks will call later for status updates.
extern "C" void board_status_led(uint8_t r, uint8_t g, uint8_t b) { led_set_rgb(r, g, b); }

// =============================================================================
// HEADLESS WARDRIVER RUNTIME
// =============================================================================
// Compiled-in defaults. Override at build time via -D flags or rebuild after
// editing this file. (Keeping it simple for v1; bruce.conf integration later.)
#ifndef WD_AP_SSID
#define WD_AP_SSID "PredatoryWarDriverDongle"
#endif
#ifndef WD_AP_PASS
#define WD_AP_PASS "Gateway1."
#endif
#ifndef WD_BEARER
#define WD_BEARER  "wd-shared-secret-change-me"
#endif
#ifndef WD_GPS_UDP_PORT
#define WD_GPS_UDP_PORT 11123
#endif
#ifndef WD_SCAN_PERIOD_MS
#define WD_SCAN_PERIOD_MS 4000
#endif
#ifndef WD_CSV_ROTATE_ROWS
#define WD_CSV_ROTATE_ROWS 1500
#endif
#ifndef WD_REQUIRE_GPS_FIX
#define WD_REQUIRE_GPS_FIX 1   // Set 0 to log scans even without GPS (lat/lng=0)
#endif

// Shared state across tasks
static TinyGPSPlus       wd_gps;
static SemaphoreHandle_t wd_gps_mtx = nullptr;
// Recursive SD mutex — protects every SD.* and File-derived call. Without
// this, concurrent ops from the scanner task + REST handlers can corrupt
// the FAT, manifesting as a card that fails to mount on subsequent boots.
static SemaphoreHandle_t wd_sd_mtx = nullptr;
#define WD_SD_LOCK()   if (wd_sd_mtx) xSemaphoreTakeRecursive(wd_sd_mtx, portMAX_DELAY)
#define WD_SD_UNLOCK() if (wd_sd_mtx) xSemaphoreGiveRecursive(wd_sd_mtx)
static volatile uint32_t wd_total_aps_seen   = 0;
static volatile uint32_t wd_total_csv_rows   = 0;
static volatile uint32_t wd_pending_csv_count = 0;
static char              wd_current_csv_path[64] = "";
static uint32_t          wd_session_started_ms   = 0;
static AsyncWebServer    wd_http_server(80);
static WiFiUDP           wd_udp;

// ---- WiGLE-1.6 CSV writer (POSIX, mounted at /sdcard via IDF) -------------
static void wd_write_wigle_header(FILE *f) {
    fputs("WigleWifi-1.6,appRelease=v1,model=ESP32-C5,release=1.0,device=T-Dongle-C5,", f);
    fputs("display=80x160,board=ESP32C5,brand=LilyGo,star=Sol,body=3,subBody=0\n", f);
    fputs("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,", f);
    fputs("CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n", f);
}

// Build a /sdcard/wd-...csv path. CSVs live at the FS root via VFS so the
// iPhone Shortcut sees /api/csv/wd-foo.csv as just <fname>.
static void wd_make_csv_path(char *out, size_t n) {
    int yr = 1970, mo = 1, dy = 1, hh = 0, mm = 0, ss = 0;
    if (xSemaphoreTake(wd_gps_mtx, pdMS_TO_TICKS(50))) {
        if (wd_gps.date.isValid() && wd_gps.time.isValid()) {
            yr = wd_gps.date.year();  mo = wd_gps.date.month(); dy = wd_gps.date.day();
            hh = wd_gps.time.hour();  mm = wd_gps.time.minute(); ss = wd_gps.time.second();
        }
        xSemaphoreGive(wd_gps_mtx);
    }
    if (yr < 2000) {
        snprintf(out, n, "/sdcard/wd-boot-%lu.csv", (unsigned long)millis());
    } else {
        snprintf(out, n, "/sdcard/wd-%04d%02d%02dT%02d%02d%02d.csv",
                 yr, mo, dy, hh, mm, ss);
    }
}

static String wd_authmode_str(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:           return "[ESS]";
        case WIFI_AUTH_WEP:            return "[WEP][ESS]";
        case WIFI_AUTH_WPA_PSK:        return "[WPA-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA2_PSK:       return "[WPA2-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA_WPA2_PSK:   return "[WPA-PSK-CCMP][WPA2-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA2_ENTERPRISE:return "[WPA2-EAP-CCMP][ESS]";
        case WIFI_AUTH_WPA3_PSK:       return "[WPA3-SAE-CCMP][ESS]";
        case WIFI_AUTH_WPA2_WPA3_PSK:  return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
        default:                       return "[ESS]";
    }
}

// Count *.csv files at /sdcard/ that don't have a matching .uploaded sibling.
// POSIX dirent under VFS — works with the IDF-mounted FATFS at /sdcard.
static uint32_t wd_count_pending() {
    if (!wd_sd_mounted) return 0;
    uint32_t n = 0;
    WD_SD_LOCK();
    DIR *d = opendir("/sdcard");
    if (!d) { WD_SD_UNLOCK(); return 0; }
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        size_t L = strlen(nm);
        bool isCsv = (L >= 4 && strcmp(nm + L - 4, ".csv") == 0);
        bool isMarker = (L >= 13 && strcmp(nm + L - 13, ".csv.uploaded") == 0);
        if (isCsv && !isMarker) {
            char marker[80];
            snprintf(marker, sizeof(marker), "/sdcard/%s.uploaded", nm);
            struct stat st;
            if (stat(marker, &st) != 0) n++;
        }
    }
    closedir(d);
    WD_SD_UNLOCK();
    return n;
}

// ---- WiFi scan / wardriver task --------------------------------------------
static void wd_task_scanner(void * /*arg*/) {
    Serial.println("[wd] scanner task starting");
    wd_make_csv_path(wd_current_csv_path, sizeof(wd_current_csv_path));
    {
        WD_SD_LOCK();
        FILE *f = fopen(wd_current_csv_path, "wb");
        if (f) { wd_write_wigle_header(f); fclose(f); }
        else   { Serial.printf("[wd] fopen %s failed: %d\n", wd_current_csv_path, errno); }
        WD_SD_UNLOCK();
    }
    wd_session_started_ms = millis();
    uint32_t row_count_in_session = 0;

    while (true) {
        // Capture GPS position under mutex.
        bool have_fix = false;
        double lat = 0.0, lng = 0.0, alt = 0.0;
        int yr = 1970, mo = 1, dy = 1, hh = 0, mm = 0, ss = 0;
        uint32_t hdop_cm = 0;
        if (xSemaphoreTake(wd_gps_mtx, pdMS_TO_TICKS(100))) {
            if (wd_gps.location.isValid() && wd_gps.location.age() < 5000) {
                have_fix = true;
                lat = wd_gps.location.lat();
                lng = wd_gps.location.lng();
                alt = wd_gps.altitude.meters();
                hdop_cm = wd_gps.hdop.value();   // hundredths of metre
            }
            if (wd_gps.date.isValid() && wd_gps.time.isValid()) {
                yr = wd_gps.date.year();  mo = wd_gps.date.month(); dy = wd_gps.date.day();
                hh = wd_gps.time.hour();  mm = wd_gps.time.minute(); ss = wd_gps.time.second();
            }
            xSemaphoreGive(wd_gps_mtx);
        }

        if (have_fix) {
            // Green flash to indicate "scanning + GPS valid"
            led_set_rgb(0x00, 0xFF, 0x00);
        } else {
            led_set_rgb(0x00, 0x80, 0xFF);   // cyan = idle, no fix
            #if WD_REQUIRE_GPS_FIX
            vTaskDelay(pdMS_TO_TICKS(WD_SCAN_PERIOD_MS));
            continue;
            #endif
        }

        int n = WiFi.scanNetworks(false /*async*/, true /*hidden*/);
        if (n < 0) n = 0;
        wd_total_aps_seen += n;

        WD_SD_LOCK();
        FILE *f = fopen(wd_current_csv_path, "ab");
        if (f) {
            for (int i = 0; i < n; i++) {
                String mac    = WiFi.BSSIDstr(i);
                String ssid   = WiFi.SSID(i);
                int chan      = WiFi.channel(i);
                int rssi      = WiFi.RSSI(i);
                int freq      = (chan <= 14) ? (2407 + chan * 5) : (5000 + chan * 5);
                String auth   = wd_authmode_str((wifi_auth_mode_t)WiFi.encryptionType(i));
                ssid.replace(",", " ");
                fprintf(f, "%s,%s,%s,%04d-%02d-%02d %02d:%02d:%02d,%d,%d,%d,%.6f,%.6f,%.1f,%.1f,,,WIFI\n",
                    mac.c_str(), ssid.c_str(), auth.c_str(),
                    yr, mo, dy, hh, mm, ss,
                    chan, freq, rssi,
                    lat, lng, alt, (double)hdop_cm / 100.0);
                row_count_in_session++;
                wd_total_csv_rows++;
            }
            fclose(f);
        }
        WD_SD_UNLOCK();
        WiFi.scanDelete();

        if (row_count_in_session >= WD_CSV_ROTATE_ROWS) {
            wd_make_csv_path(wd_current_csv_path, sizeof(wd_current_csv_path));
            WD_SD_LOCK();
            FILE *f2 = fopen(wd_current_csv_path, "wb");
            if (f2) { wd_write_wigle_header(f2); fclose(f2); }
            WD_SD_UNLOCK();
            row_count_in_session = 0;
        }

        wd_pending_csv_count = wd_count_pending();
        vTaskDelay(pdMS_TO_TICKS(WD_SCAN_PERIOD_MS));
    }
}

// ---- GPS UDP pump task ------------------------------------------------------
static void wd_task_gps_udp(void * /*arg*/) {
    Serial.printf("[wd] gps udp listening on :%d\n", WD_GPS_UDP_PORT);
    wd_udp.begin(WD_GPS_UDP_PORT);
    uint8_t buf[256];
    while (true) {
        int sz = wd_udp.parsePacket();
        if (sz > 0) {
            int n = wd_udp.read(buf, sizeof(buf));
            if (n > 0 && xSemaphoreTake(wd_gps_mtx, portMAX_DELAY)) {
                for (int i = 0; i < n; i++) wd_gps.encode((char)buf[i]);
                xSemaphoreGive(wd_gps_mtx);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// ---- REST handoff helpers ---------------------------------------------------
static bool wd_check_bearer(AsyncWebServerRequest *req) {
    if (!req->hasHeader("Authorization")) return false;
    const String h = req->header("Authorization");
    String want = String("Bearer ") + WD_BEARER;
    return h.equals(want);
}
static void wd_unauth(AsyncWebServerRequest *req) {
    req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
}

static void wd_setup_rest_endpoints() {
    using R = AsyncWebServerRequest;

    wd_http_server.on("/api/health", HTTP_GET, [](R *r) {
        if (!wd_check_bearer(r)) return wd_unauth(r);
        bool gpsValid = false;
        int sats = 0;
        double lat = 0, lng = 0;
        if (xSemaphoreTake(wd_gps_mtx, pdMS_TO_TICKS(50))) {
            gpsValid = wd_gps.location.isValid();
            sats = wd_gps.satellites.value();
            if (gpsValid) { lat = wd_gps.location.lat(); lng = wd_gps.location.lng(); }
            xSemaphoreGive(wd_gps_mtx);
        }
        char body[320];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"gpsValid\":%s,\"sats\":%d,\"lat\":%.6f,\"lng\":%.6f,"
            "\"pendingCount\":%lu,\"totalAps\":%lu,\"totalRows\":%lu,\"uptimeMs\":%lu}",
            gpsValid ? "true" : "false", sats, lat, lng,
            (unsigned long)wd_pending_csv_count,
            (unsigned long)wd_total_aps_seen,
            (unsigned long)wd_total_csv_rows,
            (unsigned long)millis());
        r->send(200, "application/json", body);
    });

    wd_http_server.on("/api/pending", HTTP_GET, [](R *r) {
        if (!wd_check_bearer(r)) return wd_unauth(r);
        String json = "[";
        bool first = true;
        WD_SD_LOCK();
        DIR *d = wd_sd_mounted ? opendir("/sdcard") : nullptr;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                const char *nm = de->d_name;
                size_t L = strlen(nm);
                bool isCsv = (L >= 4 && strcmp(nm + L - 4, ".csv") == 0);
                bool isMarker = (L >= 13 && strcmp(nm + L - 13, ".csv.uploaded") == 0);
                if (!isCsv || isMarker) continue;
                char marker[80];
                snprintf(marker, sizeof(marker), "/sdcard/%s.uploaded", nm);
                struct stat st;
                if (stat(marker, &st) == 0) continue;   // already uploaded
                char full[80];
                snprintf(full, sizeof(full), "/sdcard/%s", nm);
                struct stat st2;
                size_t sz = (stat(full, &st2) == 0) ? (size_t)st2.st_size : 0;
                if (!first) json += ",";
                first = false;
                json += "{\"name\":\"" + String(nm) + "\",\"size\":" + String((unsigned long)sz) + "}";
            }
            closedir(d);
        }
        WD_SD_UNLOCK();
        json += "]";
        r->send(200, "application/json", json);
    });

    wd_http_server.on("/api/csv/", HTTP_GET, [](R *r) {
        if (!wd_check_bearer(r)) return wd_unauth(r);
        String url = r->url();
        int slash = url.lastIndexOf('/');
        if (slash < 0) { r->send(400, "text/plain", "bad path"); return; }
        String fname = url.substring(slash + 1);
        if (fname.length() == 0) { r->send(400, "text/plain", "no filename"); return; }
        // Sanity: no directory traversal
        if (fname.indexOf('/') >= 0 || fname.startsWith("..")) {
            r->send(400, "text/plain", "bad filename"); return;
        }
        String full = String("/sdcard/") + fname;
        struct stat st;
        WD_SD_LOCK();
        bool exists = wd_sd_mounted && (stat(full.c_str(), &st) == 0);
        WD_SD_UNLOCK();
        if (!exists) { r->send(404, "text/plain", "no such file"); return; }
        // AsyncWebServer chunked response — reads chunks via fopen/fread.
        // Each chunk acquires the SD mutex to coexist with scanner writes.
        FILE *fp = fopen(full.c_str(), "rb");
        if (!fp) { r->send(500, "text/plain", "open failed"); return; }
        AsyncWebServerResponse *resp = r->beginChunkedResponse(
            "text/csv",
            [fp](uint8_t *buffer, size_t maxLen, size_t /*index*/) -> size_t {
                WD_SD_LOCK();
                size_t got = fread(buffer, 1, maxLen, fp);
                if (got == 0) { fclose(fp); }
                WD_SD_UNLOCK();
                return got;
            });
        r->send(resp);
    });

    wd_http_server.on("/api/ack/", HTTP_POST, [](R *r) {
        if (!wd_check_bearer(r)) return wd_unauth(r);
        String url = r->url();
        int slash = url.lastIndexOf('/');
        if (slash < 0) { r->send(400, "text/plain", "bad path"); return; }
        String fname = url.substring(slash + 1);
        if (fname.indexOf('/') >= 0 || fname.startsWith("..")) {
            r->send(400, "text/plain", "bad filename"); return;
        }
        String full   = String("/sdcard/") + fname;
        String marker = full + ".uploaded";
        struct stat st;
        WD_SD_LOCK();
        bool exists = wd_sd_mounted && (stat(full.c_str(), &st) == 0);
        if (exists) {
            FILE *mf = fopen(marker.c_str(), "wb");
            if (mf) { fputs("uploaded\n", mf); fclose(mf); }
        }
        WD_SD_UNLOCK();
        if (!exists) { r->send(404, "text/plain", "no such file"); return; }
        wd_pending_csv_count = wd_count_pending();
        led_set_rgb(0xFF, 0x00, 0xFF);   // brief magenta — restored by next scan cycle
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // Unauthenticated dashboard at "/" so the user can hit 192.168.4.1
    // from Safari and see status without needing the bearer token.
    wd_http_server.on("/", HTTP_GET, [](R *r) {
        bool gpsValid = false;
        int sats = 0;
        double lat = 0, lng = 0;
        if (xSemaphoreTake(wd_gps_mtx, pdMS_TO_TICKS(50))) {
            gpsValid = wd_gps.location.isValid();
            sats = wd_gps.satellites.value();
            if (gpsValid) { lat = wd_gps.location.lat(); lng = wd_gps.location.lng(); }
            xSemaphoreGive(wd_gps_mtx);
        }
        // SD status comes from our IDF mount flag + the card's CSD info.
        bool sdOk = wd_sd_mounted;
        uint64_t sdSize = 0;
        if (sdOk && wd_sd_card) {
            sdSize = ((uint64_t)wd_sd_card->csd.capacity * wd_sd_card->csd.sector_size);
        }
        char body[1280];
        snprintf(body, sizeof(body),
            "<!doctype html><html><head><meta charset=utf-8>"
            "<meta http-equiv=refresh content=2>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>WD T-Dongle</title>"
            "<style>body{font:16px/1.4 -apple-system,system-ui,sans-serif;"
            "background:#101418;color:#cce;padding:20px;max-width:420px;margin:auto}"
            "h1{margin:0 0 8px;color:#fa3}"
            "table{width:100%%;border-collapse:collapse;margin-top:12px}"
            "td{padding:6px 4px;border-bottom:1px solid #234}"
            "td:first-child{color:#7ad;font-weight:600}"
            ".ok{color:#5d6}.warn{color:#fa3}.err{color:#f45}</style>"
            "</head><body><h1>WD T-Dongle</h1>"
            "<p>Wardriver → wdgwars.pl bridge. Auto-refresh 2 s.</p>"
            "<table>"
            "<tr><td>SD card</td><td class=%s>%s</td></tr>"
            "<tr><td>GPS fix</td><td class=%s>%s (%d sats)</td></tr>"
            "<tr><td>Lat / Lng</td><td>%.6f, %.6f</td></tr>"
            "<tr><td>APs seen</td><td>%lu</td></tr>"
            "<tr><td>Rows written</td><td>%lu</td></tr>"
            "<tr><td>Pending CSVs</td><td class=%s>%lu</td></tr>"
            "<tr><td>Uptime</td><td>%lu s</td></tr>"
            "</table>"
            "<p style='font-size:12px;color:#789;margin-top:18px'>"
            "Open GPS2IP on this iPhone, set output to UDP broadcast on port 11123. "
            "The Wardrive Sync Shortcut polls /api/* with the bearer token.</p>"
            "</body></html>",
            sdOk ? "ok" : "err",
            sdOk ? (sdSize > 0 ? "mounted" : "mounted (size unknown)") : "NOT MOUNTED - reboot with card inserted",
            gpsValid ? "ok" : "warn",
            gpsValid ? "valid" : "no fix",
            sats, lat, lng,
            (unsigned long)wd_total_aps_seen,
            (unsigned long)wd_total_csv_rows,
            wd_pending_csv_count > 0 ? "warn" : "ok",
            (unsigned long)wd_pending_csv_count,
            (unsigned long)(millis() / 1000));
        r->send(200, "text/html", body);
    });

    wd_http_server.onNotFound([](R *r) {
        r->send(404, "text/plain", "wd t-dongle: unknown endpoint. Try /");
    });
}

// ---- One-time setup of the headless wardriver ------------------------------
static void wd_start_headless_runtime() {
    wd_gps_mtx = xSemaphoreCreateMutex();
    wd_sd_mtx  = xSemaphoreCreateRecursiveMutex();

    // Stop any STA Bruce may have started; force AP+STA so we can softAP +
    // also scan WiFi networks for the wardriver. eraseAP=false preserves any
    // STA creds Bruce may have stored for OTA / fallback.
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WD_AP_SSID, WD_AP_PASS);
    delay(200);
    Serial.printf("[wd] softAP up at %s\n", WiFi.softAPIP().toString().c_str());

    wd_setup_rest_endpoints();
    // ESP32Async/ESPAsyncWebServer 3.x keeps all request headers by default
    // (no collectHeaders() call exists or is needed in this fork). If a
    // future upstream rev re-introduces selective collection, audit P0-1
    // will need a different fix.
    wd_http_server.begin();

    xTaskCreate(wd_task_gps_udp,  "wd_gps",  4096,  nullptr, 5, nullptr);
    xTaskCreate(wd_task_scanner,  "wd_scan", 8192,  nullptr, 4, nullptr);
}

// Public entry-point hook for _post_setup_gpio
extern "C" void wd_boot_headless_runtime() { wd_start_headless_runtime(); }
