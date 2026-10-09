#include "Hal/Touch/TouchDriverFactory.h"

#include "Hal/Touch/CapacitiveTouchDriver.h"
#include "Hal/Touch/NullTouchDriver.h"
#include "Hal/Touch/TouchConfig.h"
#include "Hal/Touch/Xpt2046BitbangDriver.h"
#include "Hal/Touch/Xpt2046TftDriver.h"

// The esp_lcd adapter is declared only by the component that builds it, so the include and every use of
// it below sit behind the same macro. On the PlatformIO environments the macro is never defined, this
// translation unit never mentions the class, and the driver stays exactly the seven it has always been.
#if defined(KRYONOS_TOUCH_USE_ESP_LCD)
#include "Hal/Touch/EspLcdTouchDriver.h"
#endif

#include <Arduino.h>
#include <string.h>

namespace {

enum class Kind { Null, Xpt2046Bitbang, Xpt2046Tft, Ft6236, Gt911, Gt1151, Cst816, EspLcd };

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Xpt2046Bitbang: return "xpt2046-bitbang";
        case Kind::Xpt2046Tft:     return "xpt2046-tft";
        case Kind::Ft6236:         return "ft6236";
        case Kind::Gt911:          return "gt911";
        case Kind::Gt1151:         return "gt1151";
        case Kind::Cst816:         return "cst816";
        case Kind::EspLcd:         return "esp-lcd-touch";
        case Kind::Null:
        default:                   return "none";
    }
}

// "auto" keeps every pre-existing board on the driver it already used.
Kind autoDetect() {
#if defined(KRYONOS_TOUCH_USE_ESP_LCD)
    // Checked before the pin-based cases because this is the one board whose controller is not reached
    // over pins at all: the BSP brings it up and hands back an esp_lcd_touch handle. A build that
    // defines the macro is a build whose board has no XPT2046 to detect, so this cannot shadow one.
    return Kind::EspLcd;
#elif KRYONOS_TOUCH_HAS_BITBANG_PINS
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
    // The Korvo-1's part. Named explicitly rather than reached through autoDetect(), because this
    // controller is found over I2C at a known address rather than inferred from a pin being defined --
    // there is no pin to infer it from, and a wrong guess would probe an address nothing answers on.
    if (strcmp(requested, "gt1151") == 0) return Kind::Gt1151;
    if (strcmp(requested, "cst816") == 0) return Kind::Cst816;
    if (strcmp(requested, "esp_lcd") == 0 || strcmp(requested, "esp-lcd-touch") == 0) {
#if defined(KRYONOS_TOUCH_USE_ESP_LCD)
        return Kind::EspLcd;
#else
        Serial.println("[TOUCH] esp_lcd requested but this build has no BSP touch -- only the IDF/S31 "
                       "build defines KRYONOS_TOUCH_USE_ESP_LCD");
        return Kind::Null;
#endif
    }

    Serial.printf("[TOUCH] unknown KRYONOS_TOUCH_DRIVER \"%s\"; falling back to auto\n", requested);
    return autoDetect();
}

ITouchDriver* build(Kind kind) {
    switch (kind) {
        case Kind::Xpt2046Bitbang: return new Xpt2046BitbangDriver();
        case Kind::Xpt2046Tft:     return new Xpt2046TftDriver();
        case Kind::Ft6236:         return new Ft6236Driver();
        case Kind::Gt911:          return new Gt911Driver();
        case Kind::Gt1151:         return new Gt1151Driver();
        case Kind::Cst816:         return new Cst816Driver();
        case Kind::EspLcd:
#if defined(KRYONOS_TOUCH_USE_ESP_LCD)
            return new EspLcdTouchDriver();
#else
            // Unreachable: resolve() only returns EspLcd when the macro is defined. Kept so the switch
            // stays exhaustive without a #if around the case label, which would read far worse.
            return new NullTouchDriver();
#endif
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
