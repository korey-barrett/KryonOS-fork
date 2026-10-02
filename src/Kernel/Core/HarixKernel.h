#ifndef HARIX_KERNEL_H
#define HARIX_KERNEL_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "Hal/Display/KryonDisplay.h"
#include "../../Runtime/duktape.h"

class HarixKernel {
public:
    static void init(KryonDisplay *tft);
    static void runFile(const char* filePath);
    static void loop();
    static void executeJS(const char* jsCode);
    static String checkSyntax(const char* jsCode);
    static void checkJSError(duk_context *ctx, duk_int_t result);
    static duk_context *ctx;
    static KryonDisplay *tftInstance;
};

#endif // HARIX_KERNEL_H
