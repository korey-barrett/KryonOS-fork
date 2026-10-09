// Arduino.h first: it pulls in sdkconfig.h, so the target macros the guard below uses are in scope
// whichever of the two detection routes this build has available.
#include <Arduino.h>

#include "Hal/Display/EspLcdRgbDisplay.h"

// The whole implementation is compiled only where ESP-IDF ships the RGB panel driver, matching the
// guard in the header. For every other environment this translation unit is empty: PlatformIO
// compiles all of src/ for all envs, so without this `pio run -e esp32-default` cannot link.
//
// The IDF headers have to sit INSIDE this guard, not above it: esp_lcd_panel_rgb.h and esp_cache.h
// do not exist in a classic-ESP32 build at all, so an unconditional include here is what breaks
// `esp32-default` even though nothing below it is ever compiled there.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)

#include <esp32-hal-ledc.h>
#include <esp_cache.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#include "Hal/I2C/I2CEngine.h"
#include "Hal/I2C/Tca9554.h"

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

// A redraw-every-iteration UI must not saturate loop() with full-aperture blits. 20 Hz matches the
// previous port's CANVAS_FLUSH_HZ, and it caps how much PSRAM bandwidth a busy UI can take from the
// panel's bounce-buffer refill, which is itself reading PSRAM.
constexpr uint32_t kMinPresentIntervalMs = 50;

// A presented framebuffer is adopted by the scanout at the next frame boundary, ~18 ms later at this
// panel's 55.8 Hz. Half a second of no boundary means the frame-complete interrupt is not arriving
// at all, and the handshake below would otherwise block every present forever -- a frozen screen
// being a far worse failure than the tearing this backend exists to remove.
constexpr uint32_t kSwapStallMs = 500;

// --- Double buffering ---------------------------------------------------------------------------
//
// Handing esp_lcd_panel_draw_bitmap a pointer that IS one of the driver's framebuffers is what makes
// the driver adopt it: rgb_panel_draw_bitmap recognises the pointer, sets `cur_fb_index` and copies
// nothing. With a bounce buffer configured, the scanout then follows through `bb_fb_index` latching
// to `cur_fb_index` at the bounce-position wrap -- that is the only channel by which framebuffer 1 can
// reach the glass on ESP32-S3, for the restart-link reason spelled out in setUpPanel.
//
// Two framebuffers, rather than the single one the reference port uses, so that a blit targets the
// buffer the scanout is not reading. The residual tear is that the latch is only sometimes on the
// right side of the VSYNC; see setUpPanel for the arithmetic and present() for the handshake.
//
// The deferred flip is the one hazard, and the handshake below is exactly it: between presenting
// framebuffer B and the boundary that adopts it, framebuffer A is still on screen and must not be
// written. These are file-scope rather than members because the frame-complete callback runs in the
// GDMA ISR and there is one RGB panel per board.
static volatile uint8_t s_scanIdx = 0;    // the framebuffer the scanout is reading
static volatile uint8_t s_pendingIdx = 0; // the framebuffer presented, not yet adopted
static volatile bool s_swapPending = false;
// How many flips the scanout has actually adopted. present() logs the first one, because "the panel
// came up double buffered" and "the flip mechanism works" are different claims and only the second
// one means anything for tearing. Nothing else reads it.
static volatile uint32_t s_flipCount = 0;

// IRAM_ATTR: the driver requires it when CONFIG_LCD_RGB_ISR_IRAM_SAFE is on, and this runs from the
// end-of-frame interrupt regardless. It must not log, allocate or block.
static IRAM_ATTR bool rgbFrameBufComplete(esp_lcd_panel_handle_t,
                                          const esp_lcd_rgb_panel_event_data_t*, void*) {
    if (s_swapPending) {
        s_scanIdx = s_pendingIdx;
        s_swapPending = false;
        s_flipCount++;
    }
    return false;
}

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

EspLcdRgbDisplay::EspLcdRgbDisplay(int16_t nativeWidth, int16_t nativeHeight, int16_t logicalWidth,
                                   int16_t logicalHeight, int backlightPin)
    : backlightPin_(backlightPin), nativeW_(nativeWidth), nativeH_(nativeHeight),
      logicalW_(logicalWidth), logicalH_(logicalHeight), w_(logicalWidth), h_(logicalHeight) {
    // The aperture rect, derived rather than hardcoded so a different canvas cannot leave the blit
    // and the touch transform describing different rectangles. Both edges come from the same ceiling
    // the touch path inverts, so 240x320 lands on exactly 288x384 at offset (96,48).
    blitW_ = static_cast<int16_t>(canvasToPanelEdge(logicalW_));
    blitH_ = static_cast<int16_t>(canvasToPanelEdge(logicalH_));
    offsetX_ = static_cast<int16_t>((nativeW_ - blitW_) / 2);
    offsetY_ = static_cast<int16_t>((nativeH_ - blitH_) / 2);
}

EspLcdRgbDisplay::~EspLcdRgbDisplay() {
    if (canvas_.created()) canvas_.deleteSprite();
    if (panel_) {
        // Stop the handshake before the interrupts behind it go away, so nothing is left waiting on
        // a boundary that will never arrive. The callback itself touches only file-scope flags, so it
        // is safe even if it fires once more while the panel is being torn down.
        s_swapPending = false;
        doubleBuffered_ = false;
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
    }
}

void EspLcdRgbDisplay::init(uint8_t tc) {
    (void)tc;
    if (panel_) return; // idempotent: Display::begin() may run more than once

    Serial.printf("[Display:rgb] bringing up a %dx%d ST7701 RGB panel; canvas %dx%d -> %dx%d "
                  "aperture at +%d+%d (blit swap %s)\n",
                  static_cast<int>(nativeW_), static_cast<int>(nativeH_),
                  static_cast<int>(logicalW_), static_cast<int>(logicalH_),
                  static_cast<int>(blitW_), static_cast<int>(blitH_),
                  static_cast<int>(offsetX_), static_cast<int>(offsetY_),
#if KRYONOS_RGB_BLIT_SWAP
                  "on"
#else
                  "off"
#endif
    );

    // The aperture is only the inscribed rect of the circle at the scale this build assumes. A
    // canvas whose scaled rect corners fall outside the bezel loses content to the round edge, and
    // that is invisible on a desk -- so say so once, at bring-up, with the numbers that prove it.
    if (blitW_ > nativeW_ || blitH_ > nativeH_) {
        Serial.printf("[Display:rgb] WARNING: the %dx%d aperture does not fit the %dx%d panel\n",
                      static_cast<int>(blitW_), static_cast<int>(blitH_),
                      static_cast<int>(nativeW_), static_cast<int>(nativeH_));
    }
    {
        const int32_t hx = blitW_ / 2, hy = blitH_ / 2;
        const int32_t r = nativeW_ < nativeH_ ? nativeW_ / 2 : nativeH_ / 2;
        if (hx * hx + hy * hy > r * r) {
            Serial.printf("[Display:rgb] WARNING: the aperture's corners (+-%d,+-%d) fall outside "
                          "the %d px bezel; the round edge will cut content\n", hx, hy, r);
        }
    }

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

    // The panel is now scanning, but PSRAM holds uninitialised bytes: blacken BOTH framebuffers and
    // push one frame BEFORE the backlight comes on, so the user never sees noise. The clear is also
    // what makes the surround permanent -- it paints everything outside the aperture rect once, in
    // every buffer the scanout can ever reach, so a seam landing there has nothing to show.
    fillScreen(0x0000);
    clearFrameBuffer();

    // The first frame goes straight into framebuffer 0, which is the one the scanout starts on
    // (`cur_fb_index` and `bb_fb_index` both begin at 0). Drawing into the buffer it is already
    // reading cannot tear anything the user has seen -- the backlight is still off -- and it leaves
    // the handshake at its initial state, so the first present() flips to framebuffer 1.
    if (fb_[0]) {
        blitInto(static_cast<uint16_t*>(fb_[0]));
        esp_lcd_panel_draw_bitmap(panel_, 0, 0, nativeW_, nativeH_, fb_[0]);
    }
    dirty_ = false;
    lastPresentMs_ = millis();

    setBacklight(255);

    Serial.printf("[Display:rgb] panel ready (canvas %p, %u bytes; %dx%d at +%d+%d; %s). "
                  "Backlight on GPIO%d at %u Hz.\n",
                  canvas_.getPointer(),
                  static_cast<unsigned>(static_cast<size_t>(logicalW_) * logicalH_ * 2),
                  static_cast<int>(blitW_), static_cast<int>(blitH_),
                  static_cast<int>(offsetX_), static_cast<int>(offsetY_),
                  doubleBuffered_ ? "double buffered" : "single buffered",
                  backlightPin_, static_cast<unsigned>(kBacklightFreq));
}

void EspLcdRgbDisplay::setRotation(uint8_t rotation) {
    // The aperture rect's offset and the touch transform are both derived from rotation 0, so a
    // rotation here would move the canvas inside the panel while the touch path kept mapping to
    // where it used to be. Ignoring it is the safe failure; rotating would mean deriving the
    // aperture from the rotation too, which nothing on this board asks for.
    if ((rotation & 0x03) != 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Serial.printf("[Display:rgb] rotation %u ignored: the %dx%d aperture and the touch "
                          "transform are both defined at rotation 0\n",
                          static_cast<unsigned>(rotation & 0x03), static_cast<int>(blitW_),
                          static_cast<int>(blitH_));
        }
        return;
    }
    // The sprite is deliberately NOT rotated: the canvas stays in one coordinate space, which is
    // what lets width()/height() be the layout's source of truth without any draw call knowing.
    rotation_ = 0;
    w_ = logicalW_;
    h_ = logicalH_;
}

uint8_t EspLcdRgbDisplay::getRotation() { return rotation_; }
int16_t EspLcdRgbDisplay::width() { return w_; }
int16_t EspLcdRgbDisplay::height() { return h_; }

bool EspLcdRgbDisplay::ready() const { return panel_ != nullptr && canvasReady_ && !failed_; }

// =================================================================================================
// Bring-up stages
// =================================================================================================

bool EspLcdRgbDisplay::setUpCanvas() {
    // 16 bpp so the sprite is exactly the panel's colour format: 240*320*2 = 153,600 bytes, which
    // only fits in PSRAM. The sprite is the LOGICAL canvas, not the panel: every draw call writes
    // 1:1 into it at 240x320, and blitInto() is the only thing that knows the panel is bigger. Sizing
    // it to the panel instead would give the UI a 480x480 coordinate space again, which is the
    // layout the round bezel was cutting.
    //
    // TFT_eSprite gates its PSRAM allocator behind CONFIG_SPIRAM_SUPPORT (the Arduino-ESP32 core
    // names the same setting CONFIG_SPIRAM), so the env defines it; without that define this
    // createSprite() falls back to an internal calloc that cannot succeed and every draw becomes a
    // silent no-op -- hence the explicit check and message here.
    canvas_.setColorDepth(16);
    void* buf = canvas_.createSprite(logicalW_, logicalH_);
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
    // TWO framebuffers AND a bounce buffer. Both are load-bearing on this chip, and the reason is an
    // ESP32-S3-only hardware workaround in the driver:
    //
    //   #if CONFIG_IDF_TARGET_ESP32S3
    //   #define RGB_LCD_NEEDS_SEPARATE_RESTART_LINK 1
    //
    // With that set, lcd_rgb_panel_init_trans_link mounts the restart link on `fbs[0]` -- or, when a
    // bounce buffer exists, on `bounce_buffer[0]` -- and lcd_rgb_panel_try_restart_transmission
    // restarts the GDMA from it at EVERY VSYNC in stream mode. So the scanout is anchored: every field
    // begins at fbs[0].
    //
    // That is why the bounce buffer cannot be dropped. Without it there is no way to show framebuffer
    // 1 at all: the VSYNC restart re-enters at fbs[0] on every field, so a page flip to fb_[1] is
    // overwritten 56 times a second. Measured on hardware -- with bounce_buffer_size_px = 0 the panel
    // came up "double buffered", the flip callback fired, and the screen stayed BLACK, because
    // fb_[0] still held the black bring-up frame while the UI had been blitted into fb_[1].
    //
    // With the bounce buffer present the restart link points at the bounce buffer instead, and
    // `bb_fb_index` -- not cur_fb_index -- decides which framebuffer is copied into it. That is the
    // only channel through which fb_[1] can reach the glass on this chip, so the bounce buffer stays.
    //
    // The catch, and it is the residual tear: bb_fb_index re-latches to cur_fb_index only when
    // `bounce_pos_px` wraps, while the VSYNC ISR resets `bounce_pos_px` to 0 whenever it exceeds two
    // bounce buffers' worth (lcd_rgb_panel_try_restart_transmission's desync branch) and pre-fills
    // both buffers. A frame is 230400 px and a bounce buffer is 4800 px, so a field is 48 fills and the
    // wrap needs exactly 48 -- the driver derives expect_bb_eof_count from that same division -- so the
    // wrap lands on the field boundary itself, and which side of the VSYNC it falls on is a race every
    // frame. That is why the tear came and went. See the note in present().
    cfg.num_fbs = 2;
    cfg.bounce_buffer_size_px = 4800;
    cfg.dma_burst_size = 64;

    cfg.timings.pclk_hz = 16000000;
    cfg.timings.h_res = static_cast<uint32_t>(nativeW_);
    cfg.timings.v_res = static_cast<uint32_t>(nativeH_);
    cfg.timings.hsync_pulse_width = 8;
    cfg.timings.hsync_back_porch = 10;
    cfg.timings.hsync_front_porch = 50;
    cfg.timings.vsync_pulse_width = 3;
    cfg.timings.vsync_front_porch = 8;
    cfg.timings.flags.pclk_active_neg = 0;

    // The vertical back porch is the ONE timing value this board does not take from the vendor
    // sequence (which is 3/8/8); everything else here matches it. It is the whole budget the driver
    // has to re-anchor the scanout each field, and the vendor value does not leave enough of it.
    //
    // Why it is load-bearing: the prebuilt Arduino libs are built with CONFIG_LCD_RGB_RESTART_IN_VSYNC
    // set (framework-arduinoespressif32-libs/esp32s3/sdkconfig), which compiles
    // lcd_rgb_panel_try_restart_transmission down to `do_restart = true` on every VBlank
    // (esp_lcd_panel_rgb.c:1153-1154) and drops the `bb_eof_count < expect_eof_count` desync guard
    // further down. So the GDMA channel is torn down and restarted once per field, every field, and
    // Espressif's own note above that code says what a late interrupt costs:
    //
    //   "if this interrupt is late enough, the display will shift as the LCD controller already read
    //    out the first data bytes, and resetting DMA will re-send those."
    //
    // The restart also mounts its buffer with a fixed restart_skip_bytes of (LCD_LL_FIFO_DEPTH + 1) * 2
    // -- a 17-pixel FIFO-preserve skip that is only correct if the restart lands where the driver
    // assumes it does. Land late and that skip is wrong, which is what a thin band of bad pixels
    // sweeping the screen is.
    //
    // The window is vsync_pulse_width + vsync_back_porch. At the vendor 3+8 that is 11 of 499 lines,
    // and a line is 548 pclk at 16 MHz, so the ISR has ~377us. The bounce-buffer refill that keeps the
    // panel fed runs from the GDMA EOF interrupt and memcpy's 9600 bytes out of PSRAM every ~300us
    // (esp_lcd_panel_rgb.c:913, :920-922) -- the same PSRAM that WiFi, mbedTLS and every LittleFS
    // write are also using, and a flash write stalls it outright. Missing the window is not rare.
    //
    // 32 makes the window 35 lines, ~1.2ms, for a refresh of 55.8Hz instead of 58.5. This is a DE-mode
    // RGB panel: it takes the longer blanking, and no other timing value changes.
    // 64, not the 32 this was originally widened to. 32 removed the coherent band but left a
    // per-bounce-refill speckle that accumulated until the panel was re-initialised. Measured on
    // hardware, both artifacts are the same event: the restart landing before its window opens. The
    // window is vsync_pulse_width + vsync_back_porch, so 3 + 64 = 67 lines (~2.3 ms at 16 MHz) is
    // what it takes here, at about 52 Hz instead of 55.8. Narrower values bring the speckle back;
    // doubling the bounce buffer only halved its rate, because that halves how often a refill can
    // miss rather than how often the restart does.
    cfg.timings.vsync_back_porch = 64;

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

    // The frame-complete callback is the only thing that can advance the handshake, so a failure to
    // register it is a bring-up failure rather than something to limp along without: the panel would
    // show the first frame and then never change again.
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_frame_buf_complete = rgbFrameBufComplete;
    err = esp_lcd_rgb_panel_register_event_callbacks(panel_, &cbs, nullptr);
    if (err != ESP_OK) {
        lastError_ = "esp_lcd_rgb_panel_register_event_callbacks failed";
        Serial.printf("[Display:rgb] esp_lcd_rgb_panel_register_event_callbacks -> %s\n",
                      esp_err_to_name(err));
        return false;
    }

    err = esp_lcd_rgb_panel_get_frame_buffer(panel_, 2, &fb_[0], &fb_[1]);
    if (err != ESP_OK || !fb_[0] || !fb_[1]) {
        lastError_ = "esp_lcd_rgb_panel_get_frame_buffer(2) failed";
        Serial.printf("[Display:rgb] could not take hold of both framebuffers (%s)\n",
                      esp_err_to_name(err));
        return false;
    }

    s_scanIdx = 0;
    s_pendingIdx = 0;
    s_swapPending = false;
    doubleBuffered_ = true;

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
    const uint32_t now = millis();

    if (!panel_ || !canvasReady_ || !dirty_ || !fb_[0]) return;

    if (doubleBuffered_ && s_swapPending) {
        // The flip asked for by the last present has not reached a frame boundary yet, so the
        // framebuffer we would draw into next is still on screen. Wait for the boundary rather than
        // write through the picture, and give up on the handshake if it never arrives -- see
        // kSwapStallMs.
        if (now - swapAskedMs_ <= kSwapStallMs) return;

        doubleBuffered_ = false;
        s_swapPending = false;
        s_scanIdx = 0; // a dead callback means the scanout never left framebuffer 0
        Serial.printf("[Display:rgb] no frame boundary in %u ms; falling back to single buffering "
                      "(expect tearing, but the screen keeps updating)\n",
                      static_cast<unsigned>(kSwapStallMs));
    }

    if (now - lastPresentMs_ < kMinPresentIntervalMs) return;

    // One line, once, the first time the scanout actually adopts a presented buffer. Without it a
    // boot log that says "double buffered" would still leave the question this whole change is about
    // open: whether the boundary callback ever fires. If this line is missing from a log whose screen
    // has been redrawn, the flip is not happening -- and the stall watchdog above will have said so.
    if (doubleBuffered_ && !flipLogged_ && s_flipCount > 0) {
        flipLogged_ = true;
        Serial.printf("[Display:rgb] frame-boundary flip confirmed after %u presents; the blit no "
                      "longer races the scanout.\n", static_cast<unsigned>(s_flipCount));
    }

    // Double buffered: the buffer the scanout is NOT reading. Single: framebuffer 0, which is the one
    // the scanout stays on when there is no second buffer to flip to.
    const uint8_t back = doubleBuffered_ ? static_cast<uint8_t>(1u - s_scanIdx) : 0u;
    uint16_t* dst = static_cast<uint16_t*>(fb_[back]);
    if (!dst) return;

    blitInto(dst);

    // Handing the driver a pointer that IS one of its own framebuffers is what makes it adopt this
    // buffer: rgb_panel_draw_bitmap recognises it, sets cur_fb_index and copies nothing. The bounce
    // refill then picks the new buffer up when bb_fb_index re-latches at the wrap -- see setUpPanel.
    //
    // Called on EVERY present, the single-buffer fallback included. In that mode it is what keeps
    // cur_fb_index, and therefore bb_fb_index, on the framebuffer being written, so the screen goes on
    // updating -- tearing, as the fallback's own log line says -- rather than going stale while the CPU
    // draws into a buffer the bounce refill is not reading. It must stay AFTER blitInto and BEFORE the
    // flag: s_swapPending set after this call is what makes the boundary race resolve safely.
    esp_lcd_panel_draw_bitmap(panel_, 0, 0, nativeW_, nativeH_, fb_[back]);

    if (doubleBuffered_) {
        s_pendingIdx = back;
        s_swapPending = true;
        swapAskedMs_ = now;
    }

    dirty_ = false;
    lastPresentMs_ = now;
}

void EspLcdRgbDisplay::clearFrameBuffer() {
    if (!panel_ || !fb_[0]) return;

    // EVERY framebuffer, not just the first: once the double-buffer handshake starts flipping, the
    // scanout can reach either one, and the surround outside the aperture rect is only permanent if
    // both were painted black while the backlight was still off.
    const size_t bytes = static_cast<size_t>(nativeW_) * nativeH_ * sizeof(uint16_t);
    bool synced = true;
    for (size_t i = 0; i < 2 && fb_[i]; i++) {
        // Zero is black in either byte order, so this needs no swap.
        memset(fb_[i], 0, bytes);
        synced &= esp_cache_msync(static_cast<uint8_t*>(fb_[i]), bytes,
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) ==
                  ESP_OK;
    }
    if (!synced) {
        Serial.println("[Display:rgb] the bring-up framebuffer clear did not reach PSRAM; the "
                       "surround may flash until the next full frame.");
    }
}

void EspLcdRgbDisplay::blitInto(uint16_t* dst) {
    if (!dst || !canvasReady_) return;

    const uint16_t* src = static_cast<const uint16_t*>(canvas_.getPointer());
    if (!src) return;

    // This writes the framebuffer's pixels directly instead of handing esp_lcd_panel_draw_bitmap a
    // staging buffer to copy. Presenting through a copy would need a full 480x480 = 230,400 pixel
    // pass per frame; driving the aperture rect directly is 288x384 = 110,592. The cost of owning the
    // framebuffer is owning its cache maintenance, which is what the esp_cache_msync at the end is
    // for.
    const int32_t dstStride = nativeW_; // the PANEL's width: the rect is a band inside it

    const int32_t dxFirst = offsetX_;
    const int32_t dyFirst = offsetY_;
    const int32_t dxLast = offsetX_ + blitW_;
    const int32_t dyLast = offsetY_ + blitH_;

    // Nearest-neighbour 6/5 upscale, driven from the DESTINATION. Every destination pixel in the
    // rect is written exactly once, so there are no seams by construction -- and, the reason it is
    // done this way round, the source pixel chosen for a given destination pixel IS
    // apertureOffsetToCanvas, the same function panelToCanvas inverts for the touch path. Forward
    // and inverse are one expression and cannot drift.
    //
    // The source-driven alternative -- each source pixel painting the block [sx*6/5, (sx+1)*6/5) --
    // tiles just as cleanly but does NOT agree with that inverse: measured across the 288
    // destination columns, 192 of them would display a pixel one off from the one the touch
    // transform reports.
    for (int32_t dy = dyFirst; dy < dyLast; dy++) {
        const int32_t sy = apertureOffsetToCanvas(dy - offsetY_);
        const uint16_t* srow = src + static_cast<size_t>(sy) * logicalW_;
        uint16_t* out = dst + static_cast<size_t>(dy) * dstStride + dxFirst;
        for (int32_t dx = dxFirst; dx < dxLast; dx++) {
            const uint16_t p = srow[apertureOffsetToCanvas(dx - offsetX_)];
#if KRYONOS_RGB_BLIT_SWAP
            *out++ = bswap16(p);
#else
            *out++ = p;
#endif
        }
    }

    // Coherency. The driver's own cache sync for this case is gated on there being NO bounce buffer
    // (rgb_panel_draw_bitmap: `if (!rgb_panel->bb_size && rgb_panel->flags.fb_behind_cache)`), and
    // this backend keeps the bounce buffer -- so the maintenance the usual route relies on does not
    // happen. Since the framebuffer is written here directly, the cache must be pushed to PSRAM or
    // the bounce-buffer refill reads stale data.
    //
    // The written region is a band of rows, each only blitW_ wide inside a nativeW_ wide
    // framebuffer, so the rows are not contiguous and no single span of w*h pixels describes them.
    // This syncs the bounding box in one call rather than one call per row. It over-covers the gaps
    // between rows, which is harmless: those pixels are the black surround and nothing writes them.
    const size_t offset = static_cast<size_t>(dyFirst) * dstStride * sizeof(uint16_t);
    const size_t bytes = static_cast<size_t>(dyLast - dyFirst) * dstStride * sizeof(uint16_t);
    if (esp_cache_msync(reinterpret_cast<uint8_t*>(dst) + offset, bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK) {
        // Non-fatal: the panel may show one stale frame. One line only -- this runs per frame and
        // would otherwise flood the console.
        static bool warned = false;
        if (!warned) {
            warned = true;
            Serial.println("[Display:rgb] esp_cache_msync failed -- expect a stale frame.");
        }
    }
}

// =================================================================================================
// Panel-to-canvas mapping
// =================================================================================================

bool EspLcdRgbDisplay::panelToCanvas(int32_t px, int32_t py, int32_t* cx, int32_t* cy) const {
    if (!cx || !cy) return false;

    // The offsets are subtracted in PANEL space and the bounds are checked there too, BEFORE the
    // divide. That order matters: integer division truncates toward zero, so a touch one pixel left
    // of the rect would divide -1 by 6/5 to 0 and be accepted as a legitimate hit on the canvas's
    // left edge. Checking first makes an out-of-rect tap a rejection instead of a phantom edge tap.
    const int32_t bx = px - offsetX_;
    const int32_t by = py - offsetY_;
    if (bx < 0 || bx >= blitW_ || by < 0 || by >= blitH_) {
        // Once, not per touch: this runs on every sample, and a controller reporting something other
        // than panel pixels should say so on the first flash rather than present as dead touch.
        if (!loggedReject_) {
            loggedReject_ = true;
            Serial.printf("[Display:rgb] touch at panel (%d,%d) is outside the %dx%d aperture at "
                          "+%d+%d -- rejected (this logs once)\n",
                          static_cast<int>(px), static_cast<int>(py), static_cast<int>(blitW_),
                          static_cast<int>(blitH_), static_cast<int>(offsetX_),
                          static_cast<int>(offsetY_));
        }
        return false;
    }

    *cx = apertureOffsetToCanvas(bx);
    *cy = apertureOffsetToCanvas(by);
    return true;
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

    // Fill the four r-by-r corner squares the bands above leave uncovered, one row per chord,
    // with the span clipped to the square. (sx, sy) points from the circle's centre towards the
    // corner being filled; the centre sits on the square's inner corner, which is where
    // drawRoundRect() puts it too.
    //
    // circleHelper() is the wrong tool for this and used to be used here -- the same bug
    // RamFramebufferDisplay fixed in cc0a62a, still present on this backend. Its filled branch
    // anchors each chord on the centre column and runs symmetric about it, which is correct for
    // a whole circle (fillCircle still uses it) but not for a quadrant: the outer half of the
    // corner square went unpainted and the inner half was spent on the band beside it. Every
    // fillRoundRect came out with its right-hand corners bitten away -- which on a keypad whose
    // keys are filled round rects, with drawRoundRect drawing the outline correctly on top,
    // reads as a hollow link at the top and bottom right of each key. The exact chord is one
    // integer square-root walk per row and radius is single digits.
    auto corner = [&](int32_t sx, int32_t sy) {
        const int32_t cx = (sx < 0) ? x + radius : x + w - radius - 1;
        const int32_t cy = (sy < 0) ? y + radius : y + h - radius - 1;
        for (int32_t d = 1; d <= radius; d++) {
            int32_t e = 0;
            while ((e + 1) * (e + 1) + d * d <= radius * radius) e++;
            if (e == 0) continue;
            hLine((sx < 0) ? cx - e : cx, cy + sy * d, e + 1, c);
        }
    };
    corner(-1, -1); // top-left
    corner(1, -1);  // top-right
    corner(1, 1);   // bottom-right
    corner(-1, 1);  // bottom-left
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
