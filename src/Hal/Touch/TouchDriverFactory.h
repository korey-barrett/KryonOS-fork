#ifndef KRYONOS_TOUCH_DRIVER_FACTORY_H
#define KRYONOS_TOUCH_DRIVER_FACTORY_H

#include "Hal/Touch/ITouchDriver.h"

// The one place a touch driver is chosen. TouchDriver (the facade) calls this once, from init().
//
// Selection follows KRYONOS_TOUCH_DRIVER (TouchConfig.h). "auto" reproduces the historical
// compile-time ladder exactly:
//     all four XPT2046 pins defined  -> xpt2046_bitbang
//     else TOUCH_CS defined          -> xpt2046_tft
//     else                           -> null
// Naming a driver the board has no pins for falls back to null with a warning rather than reading
// unconnected pins, so a typo cannot turn into phantom touches.
//
// The returned reference is a function-local static and lives for the whole program.
ITouchDriver& touchDriver();

// The name the factory would resolve to, without constructing anything. For logs and for the
// boot banner, which prints it before the driver is initialised.
const char* resolvedTouchDriverName();

#endif // KRYONOS_TOUCH_DRIVER_FACTORY_H
