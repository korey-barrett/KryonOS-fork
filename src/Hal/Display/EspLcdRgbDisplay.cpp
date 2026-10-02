// Arduino.h first: it pulls in sdkconfig.h, so the target macros the guard below uses are in scope
// whichever of the two detection routes this build has available.
#include <Arduino.h>

#include "Hal/Display/EspLcdRgbDisplay.h"

#include <esp32-hal-ledc.h>
#include <esp_heap_caps.h>

#include "Hal/I2C/I2CEngine.h"
#include "Hal/I2C/Tca9554.h"

// The whole implementation is compiled only where ESP-IDF ships the RGB panel driver, matching the
// guard in the header. For every other environment this translation unit is empty: PlatformIO
// compiles all of src/ for all envs, so without this `pio run -e esp32-default` cannot link.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)

static_assert(!__is_abstract(EspLcdRgbDisplay),
              "EspLcdRgbDisplay must implement every KryonDisplay pure virtual");

// The RGB panel consumes native little-endian RGB565; the sprite holds its words byte-swapped (see
// the BYTE ORDER note in the header). Flip to 0 to blit the sprite buffer verbatim if a future IDF
// revision starts swapping, or to A/B it on hardware.
#ifndef KRYONOS_RGB_BLIT_SWAP
#define KRYONOS_RGB_BLIT_SWAP 1
#endif

namespace {

// --- Bus and device addresses -------------------------------------------------------------------
//
// The ST7701's command bus is 3-wire SPI bit-banged here rather than through esp_lcd_panel_io: this
// backend never builds a panel_io for the panel (it drives the RGB peripheral directly), and the
// init sequence is ~110 bytes sent once at boot. See sendInitTable() for the framing.
constexpr int kCmdSdaPin = 1;
constexpr int kCmdSclPin = 2;

// The I2C bus is shared by the TCA9554 expander and the CST820 touch controller, whose pins and bit
// assignments live in Hal/I2C/Tca9554.h. The touch path opens the same bus from
// KRYONOS_TOUCH_I2C_SDA/SCL, which platformio.ini defines as this same pair -- I2CEngine::begin
// returns true for a bus already up on matching pins and refuses one on different pins, so a
// divergence between the two spellings fails loudly instead of quietly re-initializing.
//
// The rest of the bus carries the QMI8658 IMU (0x51), the PCF85063 RTC (0x6B) and one unidentified
// 0x7E device, so a scan at boot reports five addresses; only 0x20 matters to this backend.
constexpr int kI2cSdaPin = 15;
constexpr int kI2cSclPin = 7;

// --- Backlight ----------------------------------------------------------------------------------
constexpr uint32_t kBacklightFreq = 25000;
constexpr uint8_t kBacklightBits = 10;
constexpr uint32_t kBacklightMaxDuty = (1u << kBacklightBits) - 1u;

// A redraw-every-iteration UI must not saturate loop() with 460 KB blits.
constexpr uint32_t kMinPresentIntervalMs = 33;

// --- Panel wiring (from the vendor's board_peripherals.yaml) ------------------------------------
constexpr int kDePin = 40;
constexpr int kPclkPin = 41;
constexpr int kVsyncPin = 39;
constexpr int kHsyncPin = 38;

constexpr int kDataPins[16] = {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17};

static_assert(SOC_LCDCAM_RGB_DATA_WIDTH >= 16, "this board's panel is on a 16-line RGB bus");

// --- ST7701 initialisation -----------------------------------------------------------------------
//
// Transcribed command-for-command from the board's own vendor sequence
// (xiaozhi-esp32/main/.boards/waveshare_esp32_s3_touch_lcd_2_1b/setup_device.c), which is the exact
// table Waveshare ships for this panel. The longest payload is 16 bytes, so the fixed buffer below
// covers every entry; `len` decides how many bytes are actually sent.
struct St7701Cmd {
    uint8_t cmd;
    uint8_t len;
    uint16_t delayMs;
    uint8_t data[16];
};

const St7701Cmd kInitTable[] = {
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}},
    {0xC1, 2, 0, {0x0B, 0x02}},
    {0xC2, 2, 0, {0x07, 0x02}},
    {0xCC, 1, 0, {0x10}},
    {0xCD, 1, 0, {0x08}},
    {0xB0, 16, 0, {0x00, 0x11, 0x16, 0x0e, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}},
    {0xB1, 16, 0, {0x00, 0x11, 0x16, 0x0e, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2a, 0x31, 0x18}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x6d}},
    {0xB1, 1, 0, {0x37}},
    {0xB2, 1, 0, {0x81}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x43}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x20}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 0, {0x78}},
    {0xD0, 1, 0, {0x88}},
    {0xE0, 3, 0, {0x00, 0x00, 0x02}},
    {0xE1, 11, 0, {0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20}},
    {0xE2, 13, 0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x00}},
    {0xE4, 2, 0, {0x22, 0x00}},
    {0xE5, 16, 0, {0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x00}},
    {0xE7, 2, 0, {0x22, 0x00}},
    {0xE8, 16, 0, {0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xEB, 7, 0, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}},
    {0xED, 16, 0, {0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF, 0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}},
    {0xEF, 6, 0, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, 0, {0x08}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    {0x36, 1, 0, {0x00}},
    {0x3A, 1, 0, {0x66}},
    {0x11, 0, 480, {0}}, // sleep out: the panel needs ~480 ms before the next command
    {0x20, 0, 120, {}},  // display inversion off
    {0x29, 0, 0, {}},    // display on
};

// The vendor driver emits this prefix before its vendor table on every path
// (esp_lcd_st7701_rgb.c: panel_st7701_send_init_cmds): bank select back to 0, MADCTL for
// LCD_RGB_ELEMENT_ORDER_RGB, and COLMOD for 16 bpp. The table above re-selects its own banks and
// re-sends both 0x36 and 0x3A near its end, so these three values are short-lived -- they are kept
// because this board's shipped firmware reaches the panel through exactly that driver, and matching
// it byte for byte leaves the sequence with one fewer difference to explain if the panel misbehaves.
constexpr uint8_t kPrefixBank0[5] = {0x77, 0x01, 0x00, 0x00, 0x00};
constexpr uint8_t kPrefixMadctl[1] = {0x00};
constexpr uint8_t kPrefixColmod[1] = {0x50};

// --- 3-wire command bus -------------------------------------------------------------------------
//
// Framing derived from the vendor's esp_lcd_panel_io_3wire_spi.c with this board's settings
// (spi_mode 0, use_dc_bit true, dc_zero_on_data false, lsb_first false, expect_clk_speed 500000):
// SDA and SCL idle low, SCL samples on the rising edge, 1 us half period, MSB first, and a DC bit
// ahead of every byte's eight data bits -- so nine clock cells per byte.
//
// Chip select is held low for the WHOLE sequence rather than pulsed around each byte. The vendor
// driver frames each byte in its own CS pulse, but the ST7701 latches on the ninth clock and needs no
// CS edge between bytes; holding it low is what this board's own shipped firmware does, and it costs
// two I2C writes instead of roughly 220.

inline uint16_t bswap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

// GPIO1/GPIO2, the command bus's own pins. Not to be confused with the expander's chip select,
// which is a TCA9554 output bit and is asserted through boardExpander().
void cmdBusPinsIdle() {
    pinMode(kCmdSdaPin, OUTPUT);
    pinMode(kCmdSclPin, OUTPUT);
    digitalWrite(kCmdSdaPin, LOW);
    digitalWrite(kCmdSclPin, LOW);
}

// Hand both pins back so the SD slot (which shares GPIO1/GPIO2) can claim them later.
void cmdBusRelease() {
    pinMode(kCmdSdaPin, INPUT);
    pinMode(kCmdSclPin, INPUT);
}

// One byte's nine-bit cell frame, with the chip select assumed already low. dcBit is 0 for a command
// byte and 1 for a parameter byte. Driving GPIO is infallible, so this returns nothing; the only
// step in this path that can fail is the I2C write that owns the chip select.
void cmdByte(uint8_t value, uint8_t dcBit) {
    digitalWrite(kCmdSclPin, LOW);
    uint16_t frame = value;
    for (int i = 0; i < 9; i++) {
        if (i == 0) {
            digitalWrite(kCmdSdaPin, dcBit ? HIGH : LOW);
        } else {
            digitalWrite(kCmdSdaPin, (frame & 0x80) ? HIGH : LOW);
            frame <<= 1;
        }
        digitalWrite(kCmdSclPin, LOW);
        delayMicroseconds(1);
        digitalWrite(kCmdSclPin, HIGH);
        delayMicroseconds(1);
    }

    digitalWrite(kCmdSclPin, LOW);
    digitalWrite(kCmdSdaPin, LOW);
    delayMicroseconds(1);
}

// A command plus its parameters, each byte a cell frame of its own.
void cmdCommand(uint8_t cmd, const uint8_t* data, uint8_t len) {
    cmdByte(cmd, 0);
    for (uint8_t i = 0; i < len; i++) cmdByte(data[i], 1);
}

} // namespace

// =================================================================================================
// Lifecycle
// =================================================================================================

EspLcdRgbDisplay::EspLcdRgbDisplay(int16_t nativeWidth, int16_t nativeHeight, int backlightPin)
    : backlightPin_(backlightPin), nativeW_(nativeWidth), nativeH_(nativeHeight),
      w_(nativeWidth), h_(nativeHeight) {}

EspLcdRgbDisplay::~EspLcdRgbDisplay() {
    if (canvas_.created()) canvas_.deleteSprite();
    if (staging_) {
        free(staging_);
        staging_ = nullptr;
    }
    if (panel_) {
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
    }
}

void EspLcdRgbDisplay::init(uint8_t tc) {
    (void)tc;
    if (panel_) return; // idempotent: Display::begin() may run more than once

    Serial.printf("[Display:rgb] bringing up a %dx%d ST7701 RGB panel (blit swap %s)\n",
                  static_cast<int>(nativeW_), static_cast<int>(nativeH_),
#if KRYONOS_RGB_BLIT_SWAP
                  "on"
#else
                  "off"
#endif
    );

    if (!setUpCanvas()) {
        failed_ = true;
        Serial.printf("[Display:rgb] FAILED at canvas: %s -- drawing disabled.\n", lastError_);
        return;
    }
    if (!setUpExpander()) {
        failed_ = true;
        Serial.printf("[Display:rgb] FAILED at expander: %s -- drawing disabled.\n", lastError_);
        return;
    }
    if (!sendInitTable()) {
        failed_ = true;
        Serial.printf("[Display:rgb] FAILED sending the ST7701 init table: %s -- drawing disabled.\n",
                      lastError_);
        return;
    }
    if (!setUpPanel()) {
        failed_ = true;
        Serial.printf("[Display:rgb] FAILED at the RGB panel: %s -- drawing disabled.\n", lastError_);
        return;
    }

    // The panel is now scanning, but PSRAM holds uninitialised bytes: clear and push one frame
    // BEFORE the backlight comes on, so the user never sees noise.
    fillScreen(0x0000);
    blit();
    dirty_ = false;
    lastPresentMs_ = millis();

    setBacklight(255);

    Serial.printf("[Display:rgb] panel ready (canvas %p, %u bytes; staging %u bytes). "
                  "Backlight on GPIO%d at %u Hz.\n",
                  canvas_.getPointer(),
                  static_cast<unsigned>(static_cast<size_t>(nativeW_) * nativeH_ * 2),
                  static_cast<unsigned>(static_cast<size_t>(nativeW_) * nativeH_ * 2),
                  backlightPin_, static_cast<unsigned>(kBacklightFreq));
}

void EspLcdRgbDisplay::setRotation(uint8_t rotation) {
    rotation_ = rotation & 0x03;
    // The sprite is deliberately NOT rotated: the canvas stays in panel scan order and the rotation
    // is applied to the finished frame in blit(). That keeps every draw call in one coordinate
    // space and avoids depending on TFT_eSprite's own rotation handling.
    bool landscape = (rotation_ & 1) != 0;
    w_ = landscape ? nativeH_ : nativeW_;
    h_ = landscape ? nativeW_ : nativeH_;
}

uint8_t EspLcdRgbDisplay::getRotation() { return rotation_; }
int16_t EspLcdRgbDisplay::width() { return w_; }
int16_t EspLcdRgbDisplay::height() { return h_; }

bool EspLcdRgbDisplay::ready() const { return panel_ != nullptr && canvasReady_ && !failed_; }

// =================================================================================================
// Bring-up stages
// =================================================================================================

bool EspLcdRgbDisplay::setUpCanvas() {
    // 16 bpp so the sprite is exactly the panel's colour format: 480*480*2 = 460,800 bytes, which
    // only fits in PSRAM. TFT_eSprite gates its PSRAM allocator behind CONFIG_SPIRAM_SUPPORT (the
    // Arduino-ESP32 core names the same setting CONFIG_SPIRAM), so the env defines it; without that
    // define this createSprite() falls back to an internal calloc that cannot succeed and every
    // draw becomes a silent no-op -- hence the explicit check and message here.
    canvas_.setColorDepth(16);
    void* buf = canvas_.createSprite(nativeW_, nativeH_);
    if (!buf || !canvas_.created()) {
        lastError_ = "16bpp sprite allocation failed (needs PSRAM + CONFIG_SPIRAM_SUPPORT=1)";
        return false;
    }
    canvasReady_ = true;

    // KryonDisplay colors are native RGB565 (see RamFramebufferDisplay and every other method), but
    // the sprite's buffer is byte-swapped -- so a native image handed to pushImage has to be swapped
    // on the way in, or it would come out of present() swapped twice. The sprite does that when its
    // _swapBytes flag is set, which is this backend's default.
    canvas_.setSwapBytes(true);

    size_t bytes = static_cast<size_t>(nativeW_) * static_cast<size_t>(nativeH_) * sizeof(uint16_t);
    staging_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!staging_) staging_ = static_cast<uint16_t*>(malloc(bytes));
    if (!staging_) {
        lastError_ = "could not allocate the blit staging buffer";
        canvas_.deleteSprite();
        canvasReady_ = false;
        return false;
    }

    return true;
}

bool EspLcdRgbDisplay::setUpExpander() {
    // The expander itself lives in Hal/I2C/Tca9554.h: the touch controller's reset hangs off the same
    // chip, so this backend is one of two callers rather than its owner. begin() is idempotent, so if
    // display init ever ran twice it would not re-pulse a reset under the running panel.
    if (!boardExpander().begin(kI2cSdaPin, kI2cSclPin)) {
        lastError_ = boardExpander().lastError();
        return false;
    }

    // Only the panel's own reset belongs here. TP_RST is the touch controller's, and the touch path
    // pulses it immediately before its probe instead of at this point in boot -- the CST820 falls
    // asleep and stops acknowledging I2C long before touch init runs, so a pulse issued here would
    // have expired by the time it was needed. It stays released (the idle level) until then.
    //
    // The panel's own reset GPIO is -1 in the vendor config, so this expander pin is the only reset
    // the ST7701 gets.
    if (!boardExpander().pulseLcdReset()) {
        lastError_ = "could not reset the ST7701 over the TCA9554";
        return false;
    }

    return true;
}

bool EspLcdRgbDisplay::sendInitTable() {
    cmdBusPinsIdle();

    // The chip select is asserted once, here, and held for the whole sequence. If this write fails
    // there is no point clocking anything out -- the panel would ignore all of it. It goes through
    // the expander's shadow rather than a literal byte so it cannot clobber a bit another caller owns.
    if (!boardExpander().setCommandCsAsserted(true)) {
        cmdBusRelease();
        lastError_ = "could not assert the 3-wire chip select";
        return false;
    }
    delayMicroseconds(1);

    cmdCommand(0xFF, kPrefixBank0, 5);
    cmdCommand(0x36, kPrefixMadctl, 1);
    cmdCommand(0x3A, kPrefixColmod, 1);

    for (const St7701Cmd& c : kInitTable) {
        cmdCommand(c.cmd, c.data, c.len);
        if (c.delayMs) delay(c.delayMs);
    }

    // Release the select, then the pins themselves: the board's SD slot shares GPIO1/GPIO2 and can
    // only claim them once this sequence is finished with them.
    bool released = boardExpander().setCommandCsAsserted(false);
    cmdBusRelease();
    if (!released) {
        lastError_ = "could not release the 3-wire chip select";
        return false;
    }
    return true;
}

bool EspLcdRgbDisplay::setUpPanel() {
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.data_width = 16;
    cfg.bits_per_pixel = 16;
    cfg.num_fbs = 2; // double buffered: draw_bitmap fills the back buffer, vsync swaps it
    cfg.bounce_buffer_size_px = 4800;
    cfg.dma_burst_size = 64;

    cfg.timings.pclk_hz = 16000000;
    cfg.timings.h_res = static_cast<uint32_t>(nativeW_);
    cfg.timings.v_res = static_cast<uint32_t>(nativeH_);
    cfg.timings.hsync_pulse_width = 8;
    cfg.timings.hsync_back_porch = 10;
    cfg.timings.hsync_front_porch = 50;
    cfg.timings.vsync_pulse_width = 3;
    cfg.timings.vsync_back_porch = 8;
    cfg.timings.vsync_front_porch = 8;
    cfg.timings.flags.pclk_active_neg = 0;

    cfg.hsync_gpio_num = kHsyncPin;
    cfg.vsync_gpio_num = kVsyncPin;
    cfg.de_gpio_num = kDePin;
    cfg.pclk_gpio_num = kPclkPin;
    cfg.disp_gpio_num = -1; // the init table already sent 0x29 (display on)
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = kDataPins[i];

    cfg.flags.fb_in_psram = 1;

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel_);
    if (err != ESP_OK) {
        panel_ = nullptr;
        lastError_ = "esp_lcd_new_rgb_panel failed";
        Serial.printf("[Display:rgb] esp_lcd_new_rgb_panel -> %s\n", esp_err_to_name(err));
        return false;
    }

    err = esp_lcd_panel_reset(panel_);
    if (err != ESP_OK) {
        lastError_ = "esp_lcd_panel_reset failed";
        Serial.printf("[Display:rgb] esp_lcd_panel_reset -> %s\n", esp_err_to_name(err));
        return false;
    }

    err = esp_lcd_panel_init(panel_);
    if (err != ESP_OK) {
        lastError_ = "esp_lcd_panel_init failed";
        Serial.printf("[Display:rgb] esp_lcd_panel_init -> %s\n", esp_err_to_name(err));
        return false;
    }

    return true;
}

// =================================================================================================
// Backlight
// =================================================================================================

void EspLcdRgbDisplay::setBacklight(uint8_t brightness) {
    brightness_ = brightness;
    // A panel that never came up stays dark: that is the difference between "this board is broken"
    // and "this board is fine but its backlight is off", which is worth keeping legible.
    if (!panel_ || backlightPin_ < 0) return;

    if (!backlightAttached_) {
        if (!ledcAttach(static_cast<uint8_t>(backlightPin_), kBacklightFreq, kBacklightBits)) {
            Serial.printf("[Display:rgb] ledcAttach failed on GPIO%d\n", backlightPin_);
            return;
        }
        backlightAttached_ = true;
    }
    ledcWrite(static_cast<uint8_t>(backlightPin_),
              static_cast<uint32_t>(brightness_) * kBacklightMaxDuty / 255u);
}

// =================================================================================================
// Frame presentation
// =================================================================================================

void EspLcdRgbDisplay::present() {
    if (!panel_ || !staging_ || !canvasReady_ || !dirty_) return;

    uint32_t now = millis();
    if (now - lastPresentMs_ < kMinPresentIntervalMs) return;

    dirty_ = false;
    lastPresentMs_ = now;
    blit();
}

void EspLcdRgbDisplay::blit() {
    if (!panel_ || !staging_) return;

    const uint16_t* src = static_cast<const uint16_t*>(canvas_.getPointer());
    if (!src) return;

    const int32_t W = nativeW_;
    const int32_t H = nativeH_;
    const size_t pixels = static_cast<size_t>(W) * static_cast<size_t>(H);

#if KRYONOS_RGB_BLIT_SWAP
    if (rotation_ == 0 || W != H) {
        // The square-panel rotation cases below are the only ones that can be done here; a rotated
        // non-square RGB panel needs the controller's own swap_xy/mirror instead.
        for (size_t i = 0; i < pixels; i++) staging_[i] = bswap16(src[i]);
    } else {
        // Square panel: a rotation is a pure index remap over the same buffer size.
        for (int32_t y = 0; y < H; y++) {
            for (int32_t x = 0; x < W; x++) {
                int32_t dx = 0, dy = 0;
                switch (rotation_ & 0x03) {
                    case 1: dx = H - 1 - y; dy = x; break;              // 90 degrees clockwise
                    case 2: dx = W - 1 - x; dy = H - 1 - y; break;      // 180 degrees
                    default: dx = y; dy = W - 1 - x; break;             // 270 degrees clockwise
                }
                staging_[static_cast<size_t>(dy) * W + dx] =
                    bswap16(src[static_cast<size_t>(y) * W + x]);
            }
        }
    }
#else
    memcpy(staging_, src, pixels * sizeof(uint16_t));
#endif

    esp_lcd_panel_draw_bitmap(panel_, 0, 0, W, H, staging_);
}

// =================================================================================================
// Pixel plumbing -- thin wrappers over the sprite's own virtual primitives, which do the clipping
// and the byte swap. Every one of them marks the frame dirty.
// =================================================================================================

void EspLcdRgbDisplay::putPixel(int32_t x, int32_t y, uint16_t color) {
    if (!canvasReady_) return;
    canvas_.drawPixel(x, y, color);
    dirty_ = true;
}

void EspLcdRgbDisplay::hLine(int32_t x, int32_t y, int32_t w, uint16_t color) {
    if (!canvasReady_ || w <= 0) return;
    canvas_.drawFastHLine(x, y, w, color);
    dirty_ = true;
}

void EspLcdRgbDisplay::vLine(int32_t x, int32_t y, int32_t h, uint16_t color) {
    if (!canvasReady_ || h <= 0) return;
    canvas_.drawFastVLine(x, y, h, color);
    dirty_ = true;
}

// =================================================================================================
// Shapes
//
// TFT_eSprite only overrides the low-level virtuals (drawPixel, drawFastHLine, drawFastVLine,
// fillRect, drawLine, drawChar...). The composites below are NOT virtual in TFT_eSPI, so calling
// them on the sprite would reach implementations that talk to a bus this board does not have. They
// are therefore built here on the sprite's own primitives, using the same algorithms as
// RamFramebufferDisplay so the geometry matches the other backends exactly.
// =================================================================================================

void EspLcdRgbDisplay::fillScreen(uint32_t color) {
    if (!canvasReady_) return;
    canvas_.fillSprite(color);
    dirty_ = true;
}

void EspLcdRgbDisplay::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.fillRect(x, y, w, h, color);
    dirty_ = true;
}

void EspLcdRgbDisplay::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    hLine(x, y, w, c);
    hLine(x, y + h - 1, w, c);
    vLine(x, y, h, c);
    vLine(x + w - 1, y, h, c);
}

void EspLcdRgbDisplay::drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color) {
    hLine(x, y, w, static_cast<uint16_t>(color));
}

void EspLcdRgbDisplay::drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color) {
    vLine(x, y, h, static_cast<uint16_t>(color));
}

void EspLcdRgbDisplay::drawPixel(int32_t x, int32_t y, uint32_t color) {
    putPixel(x, y, static_cast<uint16_t>(color));
}

void EspLcdRgbDisplay::drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawLine(xs, ys, xe, ye, color);
    dirty_ = true;
}

// Rounds one or more corners of a rectangle. `corners` uses TFT_eSPI's corner bit order so the
// callers below read the same way as their TFT_eSPI counterparts.
void EspLcdRgbDisplay::circleHelper(int32_t x0, int32_t y0, int32_t r, uint8_t corners,
                                    int32_t delta, uint16_t color, bool filled) {
    int32_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++;
        ddF_x += 2;
        f += ddF_x;
        if (filled) {
            if (corners & 0x4) hLine(x0 - x, y0 + y, 2 * x + 1 + delta, color);
            if (corners & 0x2) hLine(x0 - x, y0 - y, 2 * x + 1 + delta, color);
            if (corners & 0x8) hLine(x0 - y, y0 + x, 2 * y + 1 + delta, color);
            if (corners & 0x1) hLine(x0 - y, y0 - x, 2 * y + 1 + delta, color);
        } else {
            if (corners & 0x4) { putPixel(x0 + x, y0 + y, color); putPixel(x0 + y, y0 + x, color); }
            if (corners & 0x2) { putPixel(x0 + x, y0 - y, color); putPixel(x0 + y, y0 - x, color); }
            if (corners & 0x8) { putPixel(x0 - x, y0 + y, color); putPixel(x0 - y, y0 + x, color); }
            if (corners & 0x1) { putPixel(x0 - x, y0 - y, color); putPixel(x0 - y, y0 - x, color); }
        }
    }
}

void EspLcdRgbDisplay::drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    circleHelper(x, y, r, 0xF, 0, static_cast<uint16_t>(color), false);
}

void EspLcdRgbDisplay::fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    vLine(x, y - r, 2 * r + 1, c);
    circleHelper(x, y, r, 0x1 | 0x2 | 0x4 | 0x8, 0, c, true);
}

void EspLcdRgbDisplay::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                     uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    if (radius <= 0) { drawRect(x, y, w, h, color); return; }
    if (radius > (w < h ? w : h) / 2) radius = (w < h ? w : h) / 2;

    hLine(x + radius, y, w - 2 * radius, c);
    hLine(x + radius, y + h - 1, w - 2 * radius, c);
    vLine(x, y + radius, h - 2 * radius, c);
    vLine(x + w - 1, y + radius, h - 2 * radius, c);

    circleHelper(x + radius, y + radius, radius, 0x1, 0, c, false);
    circleHelper(x + w - radius - 1, y + radius, radius, 0x2, 0, c, false);
    circleHelper(x + w - radius - 1, y + h - radius - 1, radius, 0x4, 0, c, false);
    circleHelper(x + radius, y + h - radius - 1, radius, 0x8, 0, c, false);
}

void EspLcdRgbDisplay::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                     uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    if (radius <= 0) { fillRect(x, y, w, h, color); return; }
    if (radius > (w < h ? w : h) / 2) radius = (w < h ? w : h) / 2;

    fillRect(x + radius, y, w - 2 * radius, h, color);
    fillRect(x, y + radius, radius, h - 2 * radius, color);
    fillRect(x + w - radius, y + radius, radius, h - 2 * radius, color);

    circleHelper(x + radius, y + radius, radius, 0x1, 0, c, true);
    circleHelper(x + w - radius - 1, y + radius, radius, 0x2, 0, c, true);
    circleHelper(x + w - radius - 1, y + h - radius - 1, radius, 0x4, 0, c, true);
    circleHelper(x + radius, y + h - radius - 1, radius, 0x8, 0, c, true);
}

void EspLcdRgbDisplay::drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
                                    int32_t y3, uint32_t color) {
    drawLine(x1, y1, x2, y2, color);
    drawLine(x2, y2, x3, y3, color);
    drawLine(x3, y3, x1, y1, color);
}

void EspLcdRgbDisplay::fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
                                    int32_t y3, uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    // Order the vertices by y, then walk scanlines and span between the two edges.
    if (y1 > y2) { int32_t t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y2 > y3) { int32_t t = y2; y2 = y3; y3 = t; t = x2; x2 = x3; x3 = t; }
    if (y1 > y2) { int32_t t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }

    int32_t total = y3 - y1;
    if (total == 0) return;
    for (int32_t y = y1; y <= y3; y++) {
        int32_t xa = x1 + static_cast<int32_t>(static_cast<int64_t>(x3 - x1) * (y - y1) / total);
        int32_t xb = (y < y2)
                         ? (y2 == y1 ? x2
                                     : x1 + static_cast<int32_t>(
                                                static_cast<int64_t>(x2 - x1) * (y - y1) / (y2 - y1)))
                         : (y3 == y2 ? x2
                                     : x2 + static_cast<int32_t>(
                                                static_cast<int64_t>(x3 - x2) * (y - y2) / (y3 - y2)));
        if (xa > xb) { int32_t t = xa; xa = xb; xb = t; }
        hLine(xa, y, xb - xa + 1, c);
    }
}

void EspLcdRgbDisplay::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) {
    if (!canvasReady_ || !data || w <= 0 || h <= 0) return;
    // canvas_ has _swapBytes set, so the sprite swaps this native RGB565 data into its own domain.
    canvas_.pushImage(x, y, w, h, data);
    dirty_ = true;
}

// =================================================================================================
// Text -- all of it goes through the sprite's own font renderer, so glyphs and metrics are
// pixel-identical to the SPI boards.
// =================================================================================================

void EspLcdRgbDisplay::setTextColor(uint16_t color) { canvas_.setTextColor(color); }

void EspLcdRgbDisplay::setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill) {
    canvas_.setTextColor(fgcolor, bgcolor, bgfill);
}

void EspLcdRgbDisplay::setTextDatum(uint8_t datum) { canvas_.setTextDatum(datum); }
void EspLcdRgbDisplay::setTextSize(uint8_t size) { canvas_.setTextSize(size); }

void EspLcdRgbDisplay::drawString(const char* string, int32_t x, int32_t y) {
    if (!canvasReady_ || !string) return;
    canvas_.drawString(string, x, y);
    dirty_ = true;
}

void EspLcdRgbDisplay::drawString(const char* string, int32_t x, int32_t y, uint8_t font) {
    if (!canvasReady_ || !string) return;
    canvas_.drawString(string, x, y, font);
    dirty_ = true;
}

void EspLcdRgbDisplay::drawString(const String& string, int32_t x, int32_t y) {
    if (!canvasReady_) return;
    canvas_.drawString(string, x, y);
    dirty_ = true;
}

void EspLcdRgbDisplay::drawString(const String& string, int32_t x, int32_t y, uint8_t font) {
    if (!canvasReady_) return;
    canvas_.drawString(string, x, y, font);
    dirty_ = true;
}

// textWidth is pure font-table maths that does not touch the buffer, so it is forwarded
// unconditionally: layout keeps working even if the canvas failed, instead of collapsing to zeros.
int16_t EspLcdRgbDisplay::textWidth(const char* string) {
    return string ? canvas_.textWidth(string) : 0;
}

int16_t EspLcdRgbDisplay::textWidth(const char* string, uint8_t font) {
    return string ? canvas_.textWidth(string, font) : 0;
}

int16_t EspLcdRgbDisplay::textWidth(const String& string) { return canvas_.textWidth(string); }

int16_t EspLcdRgbDisplay::textWidth(const String& string, uint8_t font) {
    return canvas_.textWidth(string, font);
}

// =================================================================================================
// Colour and bus helpers
// =================================================================================================

uint16_t EspLcdRgbDisplay::color565(uint8_t red, uint8_t green, uint8_t blue) {
    return canvas_.color565(red, green, blue);
}

void EspLcdRgbDisplay::setSwapBytes(bool swap) {
    // Governs pushImage's input convention, matching TftEspiDisplay. init() sets it true because
    // KryonDisplay colors are native RGB565 and the sprite's buffer is byte-swapped.
    canvas_.setSwapBytes(swap);
}

// The sprite writes straight into its buffer; there is no bus to open or close. TFT_eSPI's own
// startWrite/endWrite would poke the phantom instance's (nonexistent) chip-select pin, so they are
// deliberately not forwarded.
void EspLcdRgbDisplay::startWrite() {}
void EspLcdRgbDisplay::endWrite() {}

#endif // RGB-capable targets only (see the note above the includes)
