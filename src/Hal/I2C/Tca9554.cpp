#include "Hal/I2C/Tca9554.h"

#include "Hal/I2C/I2CEngine.h"

#include <Arduino.h>

// TCA9554 register map. Only the first four registers exist on the part.
namespace {
constexpr uint8_t kRegInput  = 0x00; // only meaningful when a bit is configured as an input
constexpr uint8_t kRegOutput = 0x01;
constexpr uint8_t kRegPolar  = 0x02;
constexpr uint8_t kRegConfig = 0x03; // 1 = input
} // namespace

Tca9554& boardExpander() {
    // Function-local static, not a namespace-scope object: see the note in the header. Two callers
    // get the same latch shadow only because they reach it through here.
    static Tca9554 instance;
    return instance;
}

bool Tca9554::writeReg(uint8_t reg, uint8_t value) {
    return I2CEngine::writeReg(kAddress, reg, value);
}

bool Tca9554::begin(int sda, int scl, uint32_t freq) {
    // Idempotent by design. The display backend configures the expander during bring-up and the
    // touch path calls this again before it probes. The second call must not re-write the latch --
    // the command bus could be mid-sequence -- and must not re-pulse a reset under a panel that is
    // already scanning. Reporting the state the first call established is the whole job.
    if (ready_) return true;

    lastError_ = "";

    if (!I2CEngine::begin(sda, scl, freq)) {
        // I2CEngine::begin refuses a bus on a pin any TFT_* / TOUCH_* macro claims, so this is also
        // the message that appears if a pin macro is ever added to this environment by mistake.
        lastError_ = "I2C bus init failed";
        return false;
    }

    // Probe before configuring: a write to an absent chip fails the same way as a write to a
    // misconfigured one, and the two need different fixes. The expander can also need a moment after
    // the rail settles, which is why this retries rather than trusting a single ping.
    bool found = false;
    for (int attempt = 0; attempt < 8 && !found; attempt++) {
        found = I2CEngine::ping(kAddress);
        if (!found) delay(25);
    }
    if (!found) {
        lastError_ = "no TCA9554 expander at 0x20";
        return false;
    }

    // Polarity straight through: a set bit inverts its pin, and we want none of that. Written
    // explicitly because the expander is not cleared by an ESP32 reset, so a stale value written by
    // an earlier boot (or by a JS app through the I2C bindings) would otherwise survive.
    if (!writeReg(kRegPolar, 0x00)) {
        lastError_ = "could not clear the TCA9554 output polarity";
        return false;
    }

    // Levels before directions, so no pin drives before its level is defined. Safe in this order
    // because CONFIG powers up as all-inputs, so nothing is driving when the latch is written.
    out_ = kIdleLevel;
    if (!writeReg(kRegOutput, out_)) {
        lastError_ = "could not set the TCA9554 output latch";
        return false;
    }
    if (!writeReg(kRegConfig, kDirectionMask)) {
        lastError_ = "could not set the TCA9554 pin directions";
        return false;
    }

    ready_ = true;
    Serial.printf("[TCA9554] Ready at 0x%02X (config=0x%02X out=0x%02X)\n",
                  kAddress, kDirectionMask, out_);
    return true;
}

bool Tca9554::setPin(uint8_t pin, bool high) {
    if (pin > 7) return false;
    if (!ready_) {
        lastError_ = "TCA9554 not initialized";
        return false;
    }

    const uint8_t next = high ? (uint8_t)(out_ | (1u << pin)) : (uint8_t)(out_ & ~(1u << pin));
    if (next == out_) return true; // no bus traffic for a no-op

    if (!writeReg(kRegOutput, next)) {
        // The shadow is deliberately NOT advanced. The chip rejected the write, so it is still
        // driving the old level and the shadow still describes it; committing here would poison
        // every later read-modify-write with a bit that is not set on the hardware.
        lastError_ = "could not write the TCA9554 output latch";
        return false;
    }
    out_ = next;
    return true;
}

bool Tca9554::read(uint8_t* levels) {
    if (!levels) return false;
    if (!ready_) {
        lastError_ = "TCA9554 not initialized";
        return false;
    }
    const int value = I2CEngine::readReg(kAddress, kRegInput);
    if (value < 0) {
        lastError_ = "could not read the TCA9554 input register";
        return false;
    }
    *levels = (uint8_t)value;
    return true;
}

bool Tca9554::setCommandCsAsserted(bool asserted) {
    return setPin(kPinLcdCs, !asserted); // active low
}

bool Tca9554::pulseLcdReset() {
    if (!ready_) {
        lastError_ = "TCA9554 not initialized";
        return false;
    }
    // Both levels are derived from the shadow by setPin(), never from a whole-byte literal, so this
    // cannot disagree with a chip select another caller has asserted or a TP_RST it has released.
    //
    // 10 ms low, then a settle before the first command. The vendor's board code settles 50 ms; this
    // keeps the longer settle the panel was first brought up with, which is slack rather than a
    // requirement -- a longer wait before the first command cannot disturb the ST7701.
    bool ok = setPin(kPinLcdReset, false);
    delay(10);
    ok = setPin(kPinLcdReset, true) && ok;
    delay(120);
    return ok;
}

bool Tca9554::pulseTouchReset() {
    if (!ready_) {
        lastError_ = "TCA9554 not initialized";
        return false;
    }
    // The 50 ms low is load-bearing, not a round number: the reference port for this board measured
    // the controller answering on I2C only after a pulse this long, and a 10 ms pulse is the
    // likeliest single cause of a probe failing on a healthy controller.
    bool ok = setPin(kPinTpReset, false);
    delay(50);
    ok = setPin(kPinTpReset, true) && ok;
    delay(50);
    return ok;
}
