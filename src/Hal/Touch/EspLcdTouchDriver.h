#ifndef KRYONOS_ESP_LCD_TOUCH_DRIVER_H
#define KRYONOS_ESP_LCD_TOUCH_DRIVER_H

// ---------------------------------------------------------------------------------------------
// Touch via ESP-IDF's esp_lcd_touch, brought up by Espressif's board BSP.
//
// WHY THIS EXISTS INSTEAD OF A REGISTER DRIVER
//   The other capacitive drivers in this directory (FT6236, GT911, CST816) each own a register map,
//   a probe and an I2C bus. None of that is needed on a board whose BSP already knows which
//   controller is fitted: bsp_touch_new() selects the part, pulses its reset, creates the I2C master
//   bus and hands back an esp_lcd_touch_handle_t. Re-deriving the GT1158's map here would be a second
//   copy of something the vendor already maintains, and the second copy is the one that goes stale.
//
//   So this driver is an adapter, not an implementation: it is the ~60 lines that turn
//   esp_lcd_touch_read_data() into the ITouchDriver contract.
//
// POLLED, NOT INTERRUPT-DRIVEN
//   BSP_LCD_TOUCH_INT is GPIO_NUM_NC on the Korvo-1, so the BSP configures the controller with no
//   interrupt line. There is nothing to wait on and nothing to miss: getTouchRaw() reads on demand,
//   which is how every other driver here already works.
//
// COORDINATES
//   The BSP sets the controller's x_max/y_max to the panel's own size, so esp_lcd_touch reports PANEL
//   pixels. getTouchRaw() returns those; getTouch() maps them onto the live canvas through the
//   backend's own panelToCanvas(), exactly as the I2C drivers do -- so if a backend ever scales its
//   canvas into a larger panel, touch follows the blit rather than a copy of its scale.
//
// COMPILE GATING
//   KRYONOS_TOUCH_USE_ESP_LCD is defined by the IDF component that builds this file (see
//   idf/components/kryonos_s31_touch). It is never defined by platformio.ini, so under PlatformIO
//   this header declares nothing and the .cpp is an empty translation unit -- which is what keeps the
//   six existing environments compiling src/ from seeing esp_lcd_touch.h at all.
// ---------------------------------------------------------------------------------------------

#if defined(KRYONOS_TOUCH_USE_ESP_LCD)

#include <esp_lcd_touch.h>

#include "Hal/Touch/ITouchDriver.h"

class EspLcdTouchDriver : public ITouchDriver {
public:
    const char* name() const override { return "esp-lcd-touch"; }

    void begin(KryonDisplay* display) override;
    bool getTouchRaw(uint16_t* x, uint16_t* y) override;
    bool getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) override;

    // The controller reports absolute pixels, so there is nothing to calibrate. Both are no-ops, and
    // calibrate() deliberately leaves `parameters` untouched so a caller cannot persist a bogus
    // calibration tuple for a panel that never needed one.
    void setCalibration(const uint16_t* parameters) override { (void)parameters; }
    void calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                   uint8_t size) override;
    bool needsCalibration() const override { return false; }
    bool isResistive() const override { return false; }

    /** The handle the BSP returned, or nullptr if bring-up has not run or failed. */
    esp_lcd_touch_handle_t handle() const { return touch_; }

    // The panel pixels behind the most recent getTouchRaw(). Off the ITouchDriver interface on
    // purpose: it exists so a caller comparing panel against canvas coordinates can print both from a
    // single sample, and adding it to the seam would push that need onto every other driver.
    uint16_t lastRawX() const { return lastRawX_; }
    uint16_t lastRawY() const { return lastRawY_; }

private:
    KryonDisplay* display_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    uint16_t lastRawX_ = 0;
    uint16_t lastRawY_ = 0;

    // The adapter holds the contact state itself, because the GT1151 part driver cannot: see the long
    // note in getTouchRaw(). Without these, a poll that finds no fresh report reads as a release, and a
    // finger held still becomes a click per frame.
    uint32_t lastReportMs_ = 0;
    bool down_ = false;
};

#endif // KRYONOS_TOUCH_USE_ESP_LCD

#endif // KRYONOS_ESP_LCD_TOUCH_DRIVER_H
