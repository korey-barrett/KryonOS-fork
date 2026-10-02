#ifndef KRYONOS_I_TOUCH_DRIVER_H
#define KRYONOS_I_TOUCH_DRIVER_H

// ---------------------------------------------------------------------------------------------
// ITouchDriver — the seam every touch controller implements.
//
// The on-device code never talks to a driver directly: it calls the TouchDriver facade
// (src/Settings/TouchDriver.h), whose public API is unchanged from before this seam existed. That
// keeps the ~30 call sites across the kernel, keyboard, JS bindings and settings screens untouched
// while the driver behind them becomes replaceable.
//
// COORDINATES
//   getTouch() returns *pixels in the live logical canvas*, i.e. already rotated and already
//   mapped through the calibration. It asks Display::width()/height() for that canvas rather than
//   assuming 240x320, so any resolution in KRYONOS_DISPLAY_* maps correctly.
//
//   getTouchRaw() returns *controller counts*, pre-calibration. It exists for the calibration
//   routine itself, which is the only thing that should ever need raw values.
//
// CALIBRATION
//   needsCalibration() is false for drivers that report absolute pixels (every capacitive
//   controller): the OS then skips the touch-calibration screen entirely and TouchCalibrator
//   becomes a no-op. Resistive drivers return true and implement calibrate().
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

class KryonDisplay;

class ITouchDriver {
public:
    virtual ~ITouchDriver() {}

    // Short stable name for boot logs ("xpt2046-bitbang", "ft6236", ...).
    virtual const char* name() const = 0;

    // Bring up pins / bus. `display` is needed only by drivers that draw (calibration) or that
    // delegate to TFT_eSPI's own touch path -- the latter reaches that through
    // KryonDisplay::nativeTft() and must cope with a nullptr; capacitive drivers may ignore it
    // entirely. It is a pointer to the one global display instance and outlives the driver.
    virtual void begin(KryonDisplay* display) = 0;

    // Raw controller counts. False when there is no touch to report.
    virtual bool getTouchRaw(uint16_t* x, uint16_t* y) = 0;

    // Calibrated pixel coordinates in the logical canvas. False when there is no touch.
    // `threshold` is the XPT2046 pressure/z threshold and is ignored by absolute-position drivers.
    virtual bool getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) = 0;

    // Install calibration parameters. The 5-word layout is the historical
    // [x0, dx, y0, dy, flags] tuple persisted to /touch_cal_p.bin; drivers that do not calibrate
    // may ignore it.
    virtual void setCalibration(const uint16_t* parameters) = 0;

    // Run the interactive corner-touch calibration, writing 5 parameters into `parameters`.
    // Drivers that do not calibrate must leave `parameters` untouched and return immediately.
    virtual void calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                           uint8_t size) = 0;

    // True when the OS must run the calibration screen before this driver can map pixels.
    virtual bool needsCalibration() const = 0;

    // True for pressure/raw-count drivers, false for absolute-position (capacitive) ones. Used
    // only for logging and for the /touch_cal_p.bin decision.
    virtual bool isResistive() const { return true; }
};

#endif // KRYONOS_I_TOUCH_DRIVER_H
