#ifndef KRYONOS_TCA9554_H
#define KRYONOS_TCA9554_H

#include <stdint.h>

// TCA9554 8-bit I2C GPIO expander, as wired on the Waveshare ESP32-S3-Touch-LCD-2.1B.
//
// Three signals this board needs are not on the SoC and all three live here: the ST7701's reset
// (LCD_RST), the touch controller's reset (TP_RST), and the chip select of the 3-wire bus the ST7701
// takes its init sequence over. The board also wires the micro-SD socket's chip select to this chip;
// that pin is left at the vendor's idle level and nothing here drives it.
//
// WHY THIS IS A CLASS AND NOT A BYTE OF CONSTANTS
//   The chip has ONE output latch (register 0x01) and now has more than one caller. The display
//   backend asserts the command-bus chip select while it clocks out the init table; the touch path
//   pulses TP_RST before every probe. A caller that recomputes the whole byte from its own idea of
//   the other bits is exactly how one of them clobbers the other's pin -- a literal 0x0B written for
//   the chip select would drop a TP_RST that touch had just released. Every write therefore goes
//   through this object's shadow of the latch.
//
// THE SHADOW IS ONLY SAFE WHILE THESE HOLD
//   - The shadow advances only after the I2C write is acknowledged. A failed write leaves the chip
//     and the shadow both where they were, so the next read-modify-write still starts from truth.
//   - begin() returns early once ready. A second caller cannot re-write the latch behind the first
//     caller's back, and cannot re-pulse a reset under a panel that is already scanning.
//   - Nothing else may write register 0x01 directly. In particular a JS app can reach the same
//     register through the I2C bindings and a bus teardown (I2CEngine::end()/reset()) leaves the
//     shadow describing a chip the module can no longer talk to. Neither is preventable from here;
//     it is why this class is the only sanctioned writer, and why every write reports its result.
//
// The module is deliberately free of namespace-scope objects: a global with a constructor emits an
// .init_array entry, which the SDK's linker script keeps, so the object would be linked into every
// environment whether or not it uses the chip. boardExpander() is a function-local static instead.

class Tca9554 {
public:
    // 7-bit address. The vendor quotes the 8-bit form, 0x40.
    static constexpr uint8_t kAddress = 0x20;

    // This board's pin assignments, as register bits. The Waveshare wiki and the vendor's code
    // number the same lines EXIO1..EXIO8, so EXIO<n> is bit (n-1) here. Stating the mapping once
    // matters: an off-by-one resets a different peripheral than the one named.
    static constexpr uint8_t kPinLcdReset = 0; // EXIO1
    static constexpr uint8_t kPinTpReset  = 1; // EXIO2
    static constexpr uint8_t kPinLcdCs    = 2; // EXIO3 -- the ST7701's 3-wire command bus
    static constexpr uint8_t kPinSdCs     = 3; // EXIO4 -- micro-SD chip select, not driven here
    static constexpr uint8_t kPinBuzzer   = 7; // EXIO8

    // Vendor masks (board_devices.yaml): outputs on 0,1,2,3,7; inputs on 4,5,6 (the IMU's two
    // interrupt lines and the RTC's); output levels 1,1,1,1,0.
    static constexpr uint8_t kDirectionMask = 0x70; // 1 = input
    static constexpr uint8_t kIdleLevel    = 0x0F; // resets released, CS high, buzzer low

    // Bring the bus up and configure the chip. Idempotent: once ready, a second call is a no-op that
    // reports the state the first one established. That is what lets the display backend and the
    // touch path each call it without either re-pulsing a reset or re-writing the latch.
    bool begin(int sda, int scl, uint32_t freq = 400000);

    bool isReady() const { return ready_; }

    // Read-modify-write one output pin. False if the chip did not acknowledge (the shadow is then
    // left untouched and still describes the hardware).
    bool setPin(uint8_t pin, bool high);

    // Read the input register (0x00), where the IMU and RTC interrupt lines are.
    bool read(uint8_t* levels);

    // Drive the ST7701's 3-wire chip select. Named for the state rather than the level, so a call
    // site cannot read as its own opposite: the line is active low.
    bool setCommandCsAsserted(bool asserted);

    // LCD_RST: low 10 ms, then released, then a settle before the first command.
    bool pulseLcdReset();
    // TP_RST: low 50 ms, then released, then a settle. The controller only answers on I2C after
    // this pulse, and it falls back asleep between touches, so the touch path issues it immediately
    // before every probe rather than once during display bring-up.
    bool pulseTouchReset();

    // The latch as this object last wrote it, for logging and for callers that need to reason about
    // a bit without reading the chip.
    uint8_t output() const { return out_; }

    // One-line reason the last failed call gave up, or "" when nothing failed.
    const char* lastError() const { return lastError_; }

private:
    bool writeReg(uint8_t reg, uint8_t value);

    bool ready_ = false;
    uint8_t out_ = kIdleLevel;
    const char* lastError_ = "";
};

// The one expander instance, shared by everything that drives the chip.
Tca9554& boardExpander();

#endif // KRYONOS_TCA9554_H
