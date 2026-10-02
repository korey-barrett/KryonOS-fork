#include "Hal/Touch/TouchDriverFactory.h"

#include "Hal/Touch/CapacitiveTouchDriver.h"
#include "Hal/Touch/NullTouchDriver.h"
#include "Hal/Touch/TouchConfig.h"
#include "Hal/Touch/Xpt2046BitbangDriver.h"
#include "Hal/Touch/Xpt2046TftDriver.h"

#include <Arduino.h>
#include <string.h>

namespace {

enum class Kind { Null, Xpt2046Bitbang, Xpt2046Tft, Ft6236, Gt911, Cst816 };

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Xpt2046Bitbang: return "xpt2046-bitbang";
        case Kind::Xpt2046Tft:     return "xpt2046-tft";
        case Kind::Ft6236:         return "ft6236";
        case Kind::Gt911:          return "gt911";
        case Kind::Cst816:         return "cst816";
        case Kind::Null:
        default:                   return "none";
    }
}

// "auto" keeps every pre-existing board on the driver it already used.
Kind autoDetect() {
#if KRYONOS_TOUCH_HAS_BITBANG_PINS
    return Kind::Xpt2046Bitbang;
#elif KRYONOS_TOUCH_HAS_TFT_TOUCH
    return Kind::Xpt2046Tft;
#else
    return Kind::Null;
#endif
}

Kind resolve() {
    const char* requested = KRYONOS_TOUCH_DRIVER;

    if (!requested || !*requested || strcmp(requested, "auto") == 0) {
        return autoDetect();
    }
    if (strcmp(requested, "null") == 0 || strcmp(requested, "none") == 0) {
        return Kind::Null;
    }
    if (strcmp(requested, "xpt2046_bitbang") == 0) {
#if KRYONOS_TOUCH_HAS_BITBANG_PINS
        return Kind::Xpt2046Bitbang;
#else
        Serial.println("[TOUCH] xpt2046_bitbang requested but the four XPT2046 pins are not defined");
        return Kind::Null;
#endif
    }
    if (strcmp(requested, "xpt2046_tft") == 0) {
#if KRYONOS_TOUCH_HAS_TFT_TOUCH
        return Kind::Xpt2046Tft;
#else
        Serial.println("[TOUCH] xpt2046_tft requested but TOUCH_CS is not defined");
        return Kind::Null;
#endif
    }
    if (strcmp(requested, "ft6236") == 0) return Kind::Ft6236;
    if (strcmp(requested, "gt911") == 0) return Kind::Gt911;
    if (strcmp(requested, "cst816") == 0) return Kind::Cst816;

    Serial.printf("[TOUCH] unknown KRYONOS_TOUCH_DRIVER \"%s\"; falling back to auto\n", requested);
    return autoDetect();
}

ITouchDriver* build(Kind kind) {
    switch (kind) {
        case Kind::Xpt2046Bitbang: return new Xpt2046BitbangDriver();
        case Kind::Xpt2046Tft:     return new Xpt2046TftDriver();
        case Kind::Ft6236:         return new Ft6236Driver();
        case Kind::Gt911:          return new Gt911Driver();
        case Kind::Cst816:         return new Cst816Driver();
        case Kind::Null:
        default:                   return new NullTouchDriver();
    }
}

} // namespace

const char* resolvedTouchDriverName() {
    return kindName(resolve());
}

ITouchDriver& touchDriver() {
    // Constructed once, on first use, and never destroyed — it is a process-lifetime singleton and
    // its destructor would run after the hardware it owns has gone away.
    static ITouchDriver* instance = build(resolve());
    return *instance;
}
