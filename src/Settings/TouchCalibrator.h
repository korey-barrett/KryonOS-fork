#ifndef TOUCH_CALIBRATOR_H
#define TOUCH_CALIBRATOR_H

#include <TFT_eSPI.h>

#include "Hal/Display/KryonDisplay.h"
class TouchCalibrator {
public:
    static void init(KryonDisplay *tft);
    static void runCalibration();

private:
    static KryonDisplay *tftInstance;
};

#endif // TOUCH_CALIBRATOR_H
